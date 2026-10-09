// Board layer for the Raspberry Pi Pico W and Pico 2 W (arduino-pico core).
// Networking and the client run on core 0; I2S capture runs on core 1.
#if defined(ARDUINO_ARCH_RP2040)

#include <Arduino.h>
#include <I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <pico/unique_id.h>

#include <stdarg.h>
#include <stdio.h>

extern "C" {
#include "capture.h"
#include "port.h"
}

#include "board_config.h"

// Must run before anything else configures clocks: 153.6 MHz divides
// exactly to 48 kHz I2S, so the sample clock and the timer stay locked.
static I2S i2s(INPUT);

extern "C" uint64_t port_mono_us(void) { return time_us_64(); }

extern "C" void port_unique_id(uint8_t out[8])
{
    pico_unique_board_id_t id;
    pico_get_unique_board_id(&id);
    memcpy(out, id.id, 8);
}

extern "C" size_t port_free_heap(void) { return rp2040.getFreeHeap(); }

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
    WiFi.noLowPowerMode();
    WiFi.beginNoBlock(ssid, password);
}

extern "C" bool port_wifi_up(void)
{
    static bool was_up;
    bool up = WiFi.status() == WL_CONNECTED;
    if (up && !was_up)
        WiFi.noLowPowerMode(); // again: joining may have re-enabled it
    was_up = up;
    return up;
}

extern "C" int port_wifi_rssi(void) { return (int)WiFi.RSSI(); }

// ---- TCP ----

#define MAX_TCP 4
static WiFiClient tcp[MAX_TCP];
static bool tcp_used[MAX_TCP];

extern "C" int port_tcp_connect(const char *host, uint16_t port)
{
    for (int h = 0; h < MAX_TCP; h++) {
        if (tcp_used[h])
            continue;
        tcp[h].setTimeout(2000);
        if (!tcp[h].connect(host, port))
            return -1;
        tcp[h].setNoDelay(true);
        tcp_used[h] = true;
        return h;
    }
    return -1;
}

extern "C" int port_tcp_write(int h, const void *buf, size_t len)
{
    if (h < 0 || h >= MAX_TCP || !tcp_used[h] || !tcp[h].connected())
        return -1;
    int room = tcp[h].availableForWrite();
    if (room <= 0)
        return 0;
    if ((size_t)room < len)
        len = (size_t)room;
    return (int)tcp[h].write((const uint8_t *)buf, len);
}

extern "C" int port_tcp_read(int h, void *buf, size_t len)
{
    if (h < 0 || h >= MAX_TCP || !tcp_used[h])
        return -1;
    if (tcp[h].available() > 0)
        return tcp[h].read((uint8_t *)buf, len);
    return tcp[h].connected() ? 0 : -1;
}

extern "C" void port_tcp_close(int h)
{
    if (h < 0 || h >= MAX_TCP || !tcp_used[h])
        return;
    tcp[h].stop();
    tcp_used[h] = false;
}

// ---- UDP (one socket is all the client needs) ----

static WiFiUDP udp;
static IPAddress udp_ip;
static uint16_t udp_port;
static bool udp_open;

extern "C" int port_udp_open(const char *host, uint16_t port)
{
    if (!WiFi.hostByName(host, udp_ip))
        return -1;
    udp_port = port;
    if (!udp.begin(0))
        return -1;
    udp_open = true;
    return 0;
}

extern "C" int port_udp_send(int h, const void *buf, size_t len)
{
    if (h != 0 || !udp_open || !udp.beginPacket(udp_ip, udp_port))
        return -1;
    udp.write((const uint8_t *)buf, len);
    return udp.endPacket() ? (int)len : -1;
}

extern "C" int port_udp_recv(int h, void *buf, size_t len)
{
    if (h != 0 || !udp_open)
        return -1;
    int n = udp.parsePacket();
    if (n <= 0)
        return 0;
    return udp.read((uint8_t *)buf, len);
}

extern "C" void port_udp_close(int h)
{
    if (h == 0 && udp_open)
        udp.stop();
    udp_open = false;
}

// ---- Capture on core 1 ----

static volatile uint32_t audio_rate;

extern "C" void port_audio_start(uint32_t sample_rate) { audio_rate = sample_rate; }

void setup1()
{
    while (!audio_rate)
        delay(1);
    // ICS-43434: 64 bit clocks per frame, i.e. two 32-bit slots, 24-bit data
    // left-aligned. DMA buffers of 256 frames (5.3 ms), 8 of them.
    i2s.setBCLK(BPS_I2S_BCLK); // word select is BCLK + 1
    i2s.setDATA(BPS_I2S_DATA);
    i2s.setBitsPerSample(32);
    i2s.setFrequency(audio_rate);
    i2s.setBuffers(8, 512);
    if (!i2s.begin())
        port_log("audio: I2S failed to start");
}

void loop1()
{
    // A left word left over from the previous read starts the next frame.
    static int32_t lr[2 * 256 + 1];
    static size_t carry;
    // Returns bytes (whole 32-bit words), without blocking.
    size_t words = i2s.read((uint8_t *)(lr + carry), sizeof lr - 4 - carry * 4) / 4;
    if (words == 0)
        return;
    // Data appears a whole DMA buffer at a time, and this core does nothing
    // else, so the read returns right after the last frame was captured.
    // (Lost buffers show up as a timeline jump, counted as a dropout.)
    uint64_t t = time_us_64();
    size_t total = carry + words;
    bps_capture_frames(lr, total / 2, t);
    carry = total % 2;
    if (carry)
        lr[0] = lr[total - 1];
}

// ---- LED and button ----

extern "C" void port_led(bool on)
{
#if BPS_LED_PIN == BPS_LED_BUILTIN
    digitalWrite(LED_BUILTIN, on ? HIGH : LOW);
#elif BPS_LED_PIN >= 0
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
    i2s.setSysClk(48000);
    Serial.begin(115200);
#if BPS_LED_PIN == BPS_LED_BUILTIN
    pinMode(LED_BUILTIN, OUTPUT);
#elif BPS_LED_PIN >= 0
    pinMode(BPS_LED_PIN, OUTPUT);
#endif
#if BPS_BUTTON_PIN >= 0
    pinMode(BPS_BUTTON_PIN, INPUT_PULLUP);
#endif
    // Give a USB serial monitor a moment to attach, so the boot log is seen.
    for (uint32_t t = millis(); !Serial && millis() - t < 2000;)
        delay(10);
}

#endif
