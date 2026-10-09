// Board layer for the classic ESP32 (Arduino core 3.x on ESP-IDF 5).
// Wi-Fi comes from the Arduino core; I2S, timer and sockets are IDF/lwIP C
// APIs. Capture runs in its own high-priority task.
#if defined(ARDUINO_ARCH_ESP32)

#include <Arduino.h>
#include <WiFi.h>

#include <driver/i2s_std.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_timer.h>
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

extern "C" void port_wifi_begin(const char *ssid, const char *password, const char *hostname)
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    // Power saving delays packets by tens of ms, which ruins clock measurement.
    WiFi.setSleep(false);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid, password);
}

extern "C" bool port_wifi_up(void) { return WiFi.status() == WL_CONNECTED; }

extern "C" int port_wifi_rssi(void) { return (int)WiFi.RSSI(); }

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
    clk.clk_src = I2S_CLK_SRC_APLL; // exact 48 kHz from the audio PLL
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

extern "C" void port_led(bool on)
{
#if BPS_LED_PIN >= 0
    digitalWrite(BPS_LED_PIN, on ? HIGH : LOW);
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
#if BPS_LED_PIN >= 0
    pinMode(BPS_LED_PIN, OUTPUT);
#endif
#if BPS_BUTTON_PIN >= 0
    pinMode(BPS_BUTTON_PIN, INPUT_PULLUP);
#endif
    delay(500);
}

#endif
