// Board layer for the Raspberry Pi Pico W and Pico 2 W (arduino-pico core).
// Networking and the client run on core 0; I2S capture runs on core 1.
#if defined(ARDUINO_ARCH_RP2040)

#include <Arduino.h>
#include <I2S.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <hardware/watchdog.h>
#include <pico/mutex.h>
#include <pico/unique_id.h>
#ifdef BPS_NET_DEBUG
#include <lwip/priv/tcp_priv.h>
#include <lwip/tcp.h>
#include <lwip_wrap.h>
#endif

#include <stdarg.h>
#include <stdio.h>

extern "C" {
#include <cyw43.h>

#include "capture.h"
#include "logbuf.h"
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

// The trace log's lock, shared by both cores; only ever tried, never waited
// for. Set up in port_board_init(), before anything logs.
static mutex_t log_mutex;
static volatile bool log_mutex_ready;

extern "C" bool port_log_trylock(void)
{
    uint32_t owner;
    return log_mutex_ready && mutex_try_enter(&log_mutex, &owner);
}

extern "C" void port_log_unlock(void) { mutex_exit(&log_mutex); }

extern "C" void port_log(const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    Serial.printf("[%10lu] %s\r\n", (unsigned long)millis(), buf);
    bps_log_capture(port_mono_us(), buf);
}

// ---- Restarts ----
//
// The watchdog's scratch registers 0-3 (the SDK's watchdog_reboot() uses 4-7)
// keep the last command run, and a mark on a restart the server asked for,
// across the watchdog reset that restarts the board. A power cycle (or the
// RUN pin) clears them, so the command is forgotten then.

#define KEEP_MAGIC 0x62707372u         // "bpsr"
#define KEEP_RESTART (KEEP_MAGIC ^ 1u) // ... and the restart was asked for

static uint32_t keep_check(void)
{
    return watchdog_hw->scratch[0] ^ watchdog_hw->scratch[1] ^ watchdog_hw->scratch[2] ^
           0x5a5a5a5au;
}

static void keep_write(int64_t id, bool restart)
{
    watchdog_hw->scratch[1] = (uint32_t)id;
    watchdog_hw->scratch[2] = (uint32_t)((uint64_t)id >> 32);
    watchdog_hw->scratch[0] = restart ? KEEP_RESTART : KEEP_MAGIC;
    watchdog_hw->scratch[3] = keep_check();
}

extern "C" void port_boot_info(port_boot *out)
{
    uint32_t m = watchdog_hw->scratch[0];
    bool valid = (m == KEEP_MAGIC || m == KEEP_RESTART) && watchdog_hw->scratch[3] == keep_check();
    out->last_command =
        valid ? (int64_t)((uint64_t)watchdog_hw->scratch[2] << 32 | watchdog_hw->scratch[1]) : 0;
    // A watchdog that was enabled timed out; any other watchdog reset is one
    // asked for: ours, or one from a tool (picotool, a USB reset).
    if (watchdog_enable_caused_reboot())
        out->reason = BPS_BOOT_WATCHDOG;
    else if (watchdog_caused_reboot())
        out->reason = valid && m == KEEP_RESTART ? BPS_BOOT_REQUESTED : BPS_BOOT_OTHER;
    else
        out->reason = BPS_BOOT_POWER_ON;
    keep_write(out->last_command, false); // the mark is reported once
}

extern "C" void port_keep_command(int64_t id, bool restarting) { keep_write(id, restarting); }

extern "C" void port_restart(void)
{
    Serial.flush();
    watchdog_reboot(0, 0, 10);
    for (;;)
        tight_loop_contents();
}

// ---- Firmware updates: not on the Pico boards (they neither advertise nor take them) ----

extern "C" bool port_ota_supported(void) { return false; }

extern "C" void port_ota_boot_info(port_ota_state *out) { memset(out, 0, sizeof *out); }

extern "C" void port_ota_confirm(void) {}

extern "C" void port_ota_forget(void) {}

extern "C" bool port_ota_update(const char *host, uint16_t port, const char *path,
                                int64_t build_id, char *err, size_t err_cap)
{
    (void)host, (void)port, (void)path, (void)build_id;
    snprintf(err, err_cap, "this board cannot update itself");
    return false;
}

extern "C" void port_wifi_begin(const char *ssid, const char *password, const char *hostname)
{
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(hostname);
    // Power saving delays packets by tens of ms, which ruins clock measurement.
    WiFi.noLowPowerMode();
    WiFi.beginNoBlock(ssid, password);
}

// Batch mode also needs a storage card, which these boards do not have yet.
extern "C" bool port_wifi_off(void) { return false; }

// The CYW43's power saving would delay the time sync replies, and the low-power
// live mode is not ported here: the client stays live.
extern "C" bool port_wifi_power(int level) { return level == PORT_WIFI_FULL; }
extern "C" void port_cpu_low(bool low) { (void)low; }
extern "C" void port_power_diag(char *out, size_t cap)
{
    if (cap)
        out[0] = 0;
}

extern "C" void port_idle_ms(uint32_t ms) { delay(ms); }

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

extern "C" void port_wifi_diag(char *out, size_t cap)
{
    // Five ioctls to the radio (some ms each): call it rarely.
    uint32_t pm = 0;
    const char *mode = "unknown";
    if (cyw43_wifi_get_pm(&cyw43_state, &pm) == 0)
        mode = (pm & 0xf) == CYW43_NO_POWERSAVE_MODE ? "off"
               : (pm & 0xf) == CYW43_PM1_POWERSAVE_MODE ? "ON (PM1)"
                                                         : "ON (PM2)";
    uint8_t b[6] = {0};
    WiFi.BSSID(b);
    snprintf(out, cap, "ip %s, power saving %s, access point %02x:%02x:%02x:%02x:%02x:%02x channel %ld",
             WiFi.localIP().toString().c_str(), mode, b[0], b[1], b[2], b[3], b[4], b[5],
             (long)WiFi.channel());
}

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

#ifdef BPS_NET_DEBUG
// WiFiClient hides its lwIP connection; find it by local port. This reads
// lwIP's private list of connections, so it may break with a core update.
extern "C" bool port_tcp_get_stats(int h, port_tcp_stats *out)
{
    if (h < 0 || h >= MAX_TCP || !tcp_used[h])
        return false;
    uint16_t local = tcp[h].localPort();
    if (local == 0)
        return false;
    LWIPMutex lock; // lwIP also runs from the radio's interrupt
    for (struct tcp_pcb *p = tcp_active_pcbs; p; p = p->next) {
        if (p->local_port != local)
            continue;
        out->retransmits = p->nrtx;
        out->rto_ms = (uint32_t)p->rto * TCP_SLOW_INTERVAL;
        out->cwnd = p->cwnd;
        out->peer_wnd = p->snd_wnd;
        out->unacked = p->snd_nxt - p->lastack;
        return true;
    }
    return false;
}
#endif

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
// The ring stays where it is (no PSRAM to move it from).
extern "C" bool port_audio_hold(bool) { return false; }

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

// The CYW43 driver has no monitor mode, so no beacon timestamps.
extern "C" bool port_beacon_take(port_beacon *out)
{
    (void)out;
    return false;
}

// ---- Buses and storage: not on the Pico boards yet ----

extern "C" bool port_i2c_begin(int, int, uint32_t) { return false; }
extern "C" bool port_i2c_xfer(uint8_t, const uint8_t *, size_t, uint8_t *, size_t) { return false; }
extern "C" bool port_sd_begin(int, int, int, int) { return false; }
extern "C" bool port_sd_idle(void) { return true; }
extern "C" void port_sd_hold(bool) {}
extern "C" void port_sd_info_get(port_sd_info *out) { *out = port_sd_info{}; }
extern "C" bool port_sd_write(const char *, const void *, size_t, bool) { return false; }
extern "C" void port_sd_close(void) {}
extern "C" bool port_sd_remove(const char *) { return false; }
extern "C" bool port_sd_read(const char *, uint32_t, size_t) { return false; }
extern "C" bool port_sd_list(const char *, const char *) { return false; }
extern "C" int port_sd_result(const void **data, uint32_t *total)
{
    *data = nullptr;
    *total = 0;
    return -1;
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
    mutex_init(&log_mutex);
    log_mutex_ready = true;
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

// ---- On-device models: not on these boards ----

extern "C" void *port_big_alloc(size_t n) { return malloc(n); }
extern "C" void port_big_free(void *p) { free(p); }
extern "C" void *port_store_realloc(void *p, size_t n) { return realloc(p, n); }
extern "C" size_t port_big_free_size(void) { return port_free_heap(); }
extern "C" bool port_has_psram(void) { return false; }
extern "C" void *port_internal_alloc(size_t n) { return malloc(n); }
extern "C" void port_internal_free(void *p) { free(p); }
extern "C" size_t port_internal_free_heap(void) { return port_free_heap(); }
extern "C" bool port_ml_load(const void *, size_t, size_t, port_ml_tensor *, port_ml_tensor *,
                             size_t *, char *err, size_t cap)
{
    snprintf(err, cap, "this board cannot run models");
    return false;
}
extern "C" bool port_ml_invoke(void) { return false; }
extern "C" void port_ml_unload(void) {}
extern "C" bool port_worker_start(void (*)(void)) { return false; }
extern "C" void port_worker_kick(void) {}
