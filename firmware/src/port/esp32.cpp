// Board layer for the ESP32 and ESP32-S3 (Arduino core 3.x on ESP-IDF 5).
// Wi-Fi comes from the Arduino core; I2S, timer and sockets are IDF/lwIP C
// APIs. Capture runs in its own high-priority task.
#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <WiFi.h>

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
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

#ifdef BPS_BEACON_LOG
#include <esp_private/wifi.h>

// Beacon sync experiment: logs every beacon of this network's access points
// (on any channel the radio visits) with the TSF it carries and two arrival
// times: the radio's (rx_ctrl.timestamp, 32-bit us) and esp_timer's when the
// callback ran. scripts/beacon_jitter.py compares two boards' logs.
struct beacon_obs {
    uint8_t bssid[6];
    uint16_t seq;
    int8_t rssi;
    uint8_t channel;
    uint64_t tsf;
    uint32_t rx_us;
    int64_t cb_us;
};

static QueueHandle_t beacon_q;
static char beacon_ssid[33];
static size_t beacon_ssid_len;
static uint32_t beacon_dropped; // written by the Wi-Fi task, read by the log task

static void beacon_rx(void *buf, wifi_promiscuous_pkt_type_t type)
{
    int64_t now = esp_timer_get_time();
    if (type != WIFI_PKT_MGMT)
        return;
    const wifi_promiscuous_pkt_t *p = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *f = p->payload;
    // Beacon: 24-byte header, TSF (8), interval (2), capabilities (2), then
    // the SSID element first.
    size_t n = p->rx_ctrl.sig_len;
    if (n < 38 || f[0] != 0x80 || f[36] != 0 || f[37] != beacon_ssid_len ||
        n < 38 + beacon_ssid_len || memcmp(f + 38, beacon_ssid, beacon_ssid_len) != 0)
        return;
    beacon_obs o;
    memcpy(o.bssid, f + 16, 6);
    o.seq = (uint16_t)((f[22] | f[23] << 8) >> 4);
    o.tsf = 0;
    for (int i = 7; i >= 0; i--)
        o.tsf = o.tsf << 8 | f[24 + i];
    o.rssi = (int8_t)p->rx_ctrl.rssi;
    o.channel = (uint8_t)p->rx_ctrl.channel;
    o.rx_us = p->rx_ctrl.timestamp;
    o.cb_us = now;
    if (xQueueSend(beacon_q, &o, 0) != pdTRUE)
        __atomic_fetch_add(&beacon_dropped, 1, __ATOMIC_RELAXED);
}

// Links the radio's clock (that of rx_ctrl.timestamp) to esp_timer: the
// tightest of 16 reads between two esp_timer reads.
static void log_mac_clock(void)
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
    port_log("macclk esp %lld mac %lu width %lld", (long long)best_mid, (unsigned long)best_mac,
             (long long)best_width);
}

static void beacon_log_task(void *)
{
    beacon_obs o;
    uint32_t dropped = 0;
    int64_t next_clock = 0;
    for (;;) {
        if (esp_timer_get_time() >= next_clock) {
            log_mac_clock();
            next_clock = esp_timer_get_time() + 1000000;
        }
        if (xQueueReceive(beacon_q, &o, pdMS_TO_TICKS(100)) != pdTRUE)
            continue;
        port_log("beacon %02x:%02x:%02x:%02x:%02x:%02x ch %u seq %u tsf %llu rx %lu cb %lld rssi %d",
                 o.bssid[0], o.bssid[1], o.bssid[2], o.bssid[3], o.bssid[4], o.bssid[5],
                 o.channel, o.seq, (unsigned long long)o.tsf, (unsigned long)o.rx_us,
                 (long long)o.cb_us, o.rssi);
        if (__atomic_load_n(&beacon_dropped, __ATOMIC_RELAXED) != dropped) {
            dropped = __atomic_load_n(&beacon_dropped, __ATOMIC_RELAXED);
            port_log("beacon: %lu dropped (log too slow)", (unsigned long)dropped);
        }
    }
}

static void beacon_log_start(const char *ssid)
{
    beacon_ssid_len = strnlen(ssid, 32);
    memcpy(beacon_ssid, ssid, beacon_ssid_len);
    beacon_q = xQueueCreate(64, sizeof(beacon_obs));
    xTaskCreatePinnedToCore(beacon_log_task, "beacons", 4096, nullptr, 1, nullptr, 0);
    wifi_promiscuous_filter_t filter = {};
    filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(beacon_rx);
    if (esp_wifi_set_promiscuous(true) == ESP_OK)
        port_log("beacon: logging the beacons of \"%s\"", ssid);
    else
        port_log("beacon: cannot turn on promiscuous mode");
}
#endif

// Logs why joining failed, once per distinct reason (auto-reconnect repeats it).
static void on_wifi_event(arduino_event_id_t event, arduino_event_info_t info)
{
    static uint8_t last_reason;
    if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
        last_reason = 0;
    } else if (event == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
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
#ifdef BPS_BEACON_LOG
    if (!beacon_q)
        beacon_log_start(ssid);
#endif
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

static i2s_chan_handle_t rx;

static void capture_task(void *)
{
    static int32_t lr[2 * DMA_FRAMES];
    for (;;) {
        size_t got = 0;
        if (i2s_channel_read(rx, lr, sizeof lr, &got, portMAX_DELAY) != ESP_OK || got < 8)
            continue;
        // Blocks until a DMA buffer completes and this task has the highest
        // priority, so the read returns right after its last frame.
        bps_capture_frames(lr, got / 8, port_mono_us());
    }
}

extern "C" void port_audio_start(uint32_t sample_rate)
{
    i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    cc.dma_desc_num = 8;
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
    if (i2s_channel_init_std_mode(rx, &sc) != ESP_OK || i2s_channel_enable(rx) != ESP_OK) {
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
