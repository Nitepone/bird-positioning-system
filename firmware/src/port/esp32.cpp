// Board layer for the ESP32 and ESP32-S3 (Arduino core 3.x on ESP-IDF 5).
// Wi-Fi comes from the Arduino core; I2S, timer and sockets are IDF/lwIP C
// APIs. Capture runs in its own high-priority task.
#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <WiFi.h>

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_private/wifi.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <fcntl.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

extern "C" {
#include "capture.h"
#include "port.h"
}

#include "board_config.h"

extern "C" uint64_t port_mono_us(void) { return (uint64_t)esp_timer_get_time(); }

extern "C" void port_unique_id(uint8_t out[8])
{
    memset(out, 0, 8);
    esp_efuse_mac_get_default(out);
}

extern "C" size_t port_free_heap(void) { return heap_caps_get_free_size(MALLOC_CAP_8BIT); }

extern "C" void port_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial.printf("[%10lu] %s\r\n", (unsigned long)millis(), buf);
}

// ---- Beacons ----
//
// In promiscuous mode alongside the normal connection, every beacon of the
// connected access point is kept with the TSF it carries and the radio's
// arrival timestamp (rx_ctrl.timestamp). That timestamp is on the radio's own
// 32-bit microsecond clock, so once a second the radio's clock is read
// between two esp_timer reads; the median of the last few such links
// converts arrivals to the monotonic clock. Built with -DBPS_BEACON_LOG, every
// beacon and link is also logged, for scripts/beacon_jitter.py.

struct beacon_obs {
    uint8_t bssid[6];
    uint64_t tsf;
    uint32_t rx_us;
    int64_t cb_us;
};

#define LINKS 9

static QueueHandle_t beacon_q;
static uint8_t ap_bssid[6]; // the connected access point, all zero if none
static int32_t link_off[LINKS]; // esp_timer - radio clock, mod 2^32
static uint32_t n_links;
static int64_t next_link_us;

static void beacon_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    int64_t now = esp_timer_get_time();
    if (type != WIFI_PKT_MGMT)
        return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = p->payload;
    // Beacon: 24-byte header (BSSID at 16), then the TSF.
    if (p->rx_ctrl.sig_len < 36 || f[0] != 0x80 || memcmp(f + 16, ap_bssid, 6) != 0)
        return;
    beacon_obs o;
    memcpy(o.bssid, f + 16, 6);
    o.tsf = 0;
    for (int i = 7; i >= 0; i--)
        o.tsf = o.tsf << 8 | f[24 + i];
    o.rx_us = p->rx_ctrl.timestamp;
    o.cb_us = now;
    xQueueSend(beacon_q, &o, 0);
}

// The tightest of 16 reads of the radio's clock between two esp_timer reads.
static void link_clocks(void)
{
    int64_t best_mid = 0, best_width = INT64_MAX;
    uint32_t best_mac = 0;
    for (int i = 0; i < 16; i++) {
        int64_t t0 = esp_timer_get_time();
        uint32_t mac = esp_wifi_internal_get_mac_clock_time();
        int64_t t1 = esp_timer_get_time();
        if (t1 - t0 < best_width) {
            best_width = t1 - t0;
            best_mid = t0 + (t1 - t0) / 2;
            best_mac = mac;
        }
    }
    link_off[n_links++ % LINKS] = (int32_t)((uint32_t)best_mid - best_mac);
#ifdef BPS_BEACON_LOG
    port_log("macclk esp %lld mac %lu width %lld", (long long)best_mid, (unsigned long)best_mac,
             (long long)best_width);
#endif
}

static int32_t link_offset(void)
{
    uint32_t n = n_links < LINKS ? n_links : LINKS;
    int32_t s[LINKS];
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = i;
        for (; j > 0 && s[j - 1] > link_off[i]; j--)
            s[j] = s[j - 1];
        s[j] = link_off[i];
    }
    return s[n / 2];
}

static void beacons_start(void)
{
    beacon_q = xQueueCreate(32, sizeof(beacon_obs));
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(beacon_rx);
    if (esp_wifi_set_promiscuous(true) != ESP_OK)
        port_log("beacons: cannot turn on promiscuous mode");
}

extern "C" bool port_beacon_take(port_beacon *out)
{
    if (!beacon_q)
        return false;
    int64_t now = esp_timer_get_time();
    if (now >= next_link_us) {
        link_clocks();
        next_link_us = now + 1000000;
    }
    beacon_obs o;
    if (n_links == 0 || xQueueReceive(beacon_q, &o, 0) != pdTRUE)
        return false;
    // Arrival on esp_timer: the radio's time plus the link, unwrapped near now.
    uint32_t rx_esp = o.rx_us + (uint32_t)link_offset();
    int64_t esp = now - (int64_t)(uint32_t)((uint32_t)now - rx_esp);
#ifdef BPS_BEACON_LOG
    port_log("beacon %02x:%02x:%02x:%02x:%02x:%02x ch %u seq %u tsf %llu rx %lu cb %lld rssi %d",
             o.bssid[0], o.bssid[1], o.bssid[2], o.bssid[3], o.bssid[4], o.bssid[5], 0u, 0u,
             (unsigned long long)o.tsf, (unsigned long)o.rx_us, (long long)o.cb_us, 0);
#endif
    memcpy(out->bssid, o.bssid, 6);
    out->tsf_us = o.tsf;
    out->mono_ns = esp * 1000;
    return true;
}

// Logs why joining failed, once per distinct reason (auto-reconnect repeats it).
static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info)
{
    static uint8_t last_reason;
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        last_reason = 0;
        memcpy(ap_bssid, info.wifi_sta_connected.bssid, 6);
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
        memset(ap_bssid, 0, 6);
        uint8_t r = info.wifi_sta_disconnected.reason;
        if (r != last_reason)
            port_log("wifi: not connected: %s (reason %u)",
                     WiFi.disconnectReasonName((wifi_err_reason_t)r), r);
        last_reason = r;
    }
}

extern "C" void port_wifi_begin(const char *ssid, const char *password, const char *hostname)
{
    static bool started;
    if (started) {
        // A retry. Arduino's auto-reconnect retries after almost every failure
        // by itself, and begin() would collide with it; only restart the
        // driver in case it gave up. While it is still trying, the driver
        // just logs "sta is connecting", so quiet that.
        esp_log_level_t level = esp_log_level_get("wifi");
        esp_log_level_set("wifi", ESP_LOG_NONE);
        esp_wifi_connect();
        esp_log_level_set("wifi", level);
        return;
    }
    WiFi.onEvent(on_wifi_event);
    started = true;
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    // Power saving delays packets by tens of ms, which ruins clock measurement.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
    beacons_start();
}

extern "C" bool port_wifi_up(void) { return WiFi.status() == WL_CONNECTED; }

extern "C" int port_wifi_rssi(void) { return (int)WiFi.RSSI(); }

extern "C" void port_wifi_diag(char *out, size_t cap)
{
    snprintf(out, cap, "ip %s, power saving %s, access point %s channel %ld",
             WiFi.localIP().toString().c_str(), WiFi.getSleep() ? "ON" : "off",
             WiFi.BSSIDstr().c_str(), (long)WiFi.channel());
}

// ---- Sockets ----

static bool resolve(const char *host, uint16_t port, int type, struct sockaddr_in *out)
{
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = type;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host, nullptr, &hints, &res) != 0 || !res)
        return false;
    memcpy(out, res->ai_addr, sizeof *out);
    out->sin_port = htons(port);
    freeaddrinfo(res);
    return true;
}

static void set_nonblocking(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK); }

extern "C" int port_tcp_connect(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_STREAM, &addr))
        return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    set_nonblocking(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0 && errno != EINPROGRESS) {
        close(fd);
        return -1;
    }
    // Wait at most 2 s for the connection.
    fd_set w;
    FD_ZERO(&w);
    FD_SET(fd, &w);
    struct timeval tv = {2, 0};
    int err = 0;
    socklen_t len = sizeof err;
    if (select(fd + 1, nullptr, &w, nullptr, &tv) != 1 ||
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err) {
        close(fd);
        return -1;
    }
    return fd;
}

extern "C" int port_tcp_write(int h, const void *buf, size_t len)
{
    int n = send(h, buf, len, MSG_DONTWAIT);
    if (n >= 0)
        return n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

extern "C" int port_tcp_read(int h, void *buf, size_t len)
{
    int n = recv(h, buf, len, MSG_DONTWAIT);
    if (n > 0)
        return n;
    if (n == 0)
        return -1; // closed by the server
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

extern "C" void port_tcp_close(int h)
{
    if (h >= 0)
        close(h);
}

#ifdef BPS_NET_DEBUG
extern "C" bool port_tcp_get_stats(int h, port_tcp_stats *out)
{
    (void)h;
    (void)out;
    return false; // lwIP's internals belong to its own task here
}
#endif

extern "C" int port_udp_open(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (!resolve(host, port, SOCK_DGRAM, &addr))
        return -1;
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        close(fd);
        return -1;
    }
    set_nonblocking(fd);
    return fd;
}

extern "C" int port_udp_send(int h, const void *buf, size_t len)
{
    return send(h, buf, len, MSG_DONTWAIT) == (int)len ? (int)len : -1;
}

extern "C" int port_udp_recv(int h, void *buf, size_t len)
{
    int n = recv(h, buf, len, MSG_DONTWAIT);
    if (n >= 0)
        return n;
    return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -1;
}

extern "C" void port_udp_close(int h)
{
    if (h >= 0)
        close(h);
}

// ---- Capture task ----

#define DMA_FRAMES 256
#define DMA_DESCS 8

static i2s_chan_handle_t rx;

// When each DMA buffer completed, stamped in the I2S interrupt: far less
// delayed than the capture task's wake-up. The driver hands buffers to reads
// in order and drops the oldest unread one when its queue is full, so the
// n-th buffer read completed (n + buffers dropped so far)-th, or later if a
// drop raced the read; a later stamp is only a later estimate, which the
// timestamper ignores.
#define STAMPS 16
static int64_t isr_stamp[STAMPS];
static uint32_t isr_done, isr_dropped;

static bool IRAM_ATTR on_recv(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    uint32_t n = isr_done;
    isr_stamp[n % STAMPS] = esp_timer_get_time();
    __atomic_store_n(&isr_done, n + 1, __ATOMIC_RELEASE);
    return false;
}

static bool IRAM_ATTR on_recv_q_ovf(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    __atomic_store_n(&isr_dropped, isr_dropped + 1, __ATOMIC_RELEASE);
    return false;
}

static void capture_task(void *)
{
    static int32_t lr[2 * DMA_FRAMES]; // exactly one DMA buffer
    uint32_t reads = 0;
    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, lr, sizeof lr, &got, portMAX_DELAY) != ESP_OK || got < 8)
            continue;
        uint64_t t = port_mono_us();
        uint32_t k = reads++ + __atomic_load_n(&isr_dropped, __ATOMIC_ACQUIRE);
        uint32_t done = __atomic_load_n(&isr_done, __ATOMIC_ACQUIRE);
        if (got == sizeof lr && (int32_t)(done - k) > 0 && done - k < STAMPS) {
            int64_t s = isr_stamp[k % STAMPS];
            // Unless the stamp was overwritten while being read.
            if (__atomic_load_n(&isr_done, __ATOMIC_ACQUIRE) - k < STAMPS && (uint64_t)s <= t)
                t = (uint64_t)s;
        }
        bps_capture_frames(lr, got / 8, t);
    }
}

extern "C" void port_audio_start(uint32_t sample_rate)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = DMA_DESCS;
    cc.dma_frame_num = DMA_FRAMES;
    if (i2s_new_channel(&cc, nullptr, &rx) != ESP_OK) {
        port_log("audio: cannot create the I2S channel");
        return;
    }
    // ICS-43434: Philips I2S, two 32-bit slots (64 bit clocks per frame),
    // 24-bit data left-aligned. Both slots are read; the core picks one.
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
#if SOC_I2S_SUPPORTS_APLL
    clk.clk_src = I2S_CLK_SRC_APLL; // exact 48 kHz from the audio PLL
#endif
    // Without one (ESP32-S3), the default 160 MHz PLL: its fractional divider
    // gives exactly 48 kHz on average (160 MHz / 12.288 MHz = 13 + 1/48).
    i2s_std_slot_config_t slot =
        I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
    i2s_std_config_t sc = {};
    sc.clk_cfg = clk;
    sc.slot_cfg = slot;
    sc.gpio_cfg.mclk = I2S_GPIO_UNUSED;
    sc.gpio_cfg.bclk = (gpio_num_t)BPS_I2S_BCLK;
    sc.gpio_cfg.ws = (gpio_num_t)BPS_I2S_WS;
    sc.gpio_cfg.dout = I2S_GPIO_UNUSED;
    sc.gpio_cfg.din = (gpio_num_t)BPS_I2S_DATA;
    i2s_event_callbacks_t cbs = {};
    cbs.on_recv = on_recv;
    cbs.on_recv_q_ovf = on_recv_q_ovf;
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK ||
        i2s_channel_register_event_callback(rx, &cbs, nullptr) != ESP_OK ||
        i2s_channel_enable(rx) != ESP_OK) {
        port_log("audio: I2S failed to start");
        return;
    }
    // Arduino's loop() runs on core 1 at priority 1; Wi-Fi lives on core 0.
    xTaskCreatePinnedToCore(capture_task, "capture", 8192, nullptr, 10, nullptr, 1);
}

// ---- LED and button ----

#if BPS_LED_PIN == BPS_LED_BUILTIN
#define LED_GPIO LED_BUILTIN
#define LED_ON LOW // the XIAO ESP32S3's user LED is wired to 3V3
#elif BPS_LED_PIN >= 0
#define LED_GPIO BPS_LED_PIN
#define LED_ON HIGH
#endif

extern "C" void port_led(bool on)
{
#ifdef LED_GPIO
    digitalWrite(LED_GPIO, on ? LED_ON : !LED_ON);
#else
    (void)on;
#endif
}

extern "C" bool port_button(void)
{
#if BPS_BUTTON_PIN >= 0
    return digitalRead(BPS_BUTTON_PIN) == LOW;
#else
    return false;
#endif
}

extern "C" void port_board_init(void)
{
    Serial.begin(115200);
#ifdef LED_GPIO
    pinMode(LED_GPIO, OUTPUT);
    port_led(false);
#endif
#if BPS_BUTTON_PIN >= 0
    pinMode(BPS_BUTTON_PIN, INPUT_PULLUP);
#endif
    delay(500);
}

#endif
