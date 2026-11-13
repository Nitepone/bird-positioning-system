/*
 * What the core needs from each board (implemented in firmware/src/port/).
 * All network calls are non-blocking except connecting, which waits at most
 * a couple of seconds. Handles are small non-negative ints; -1 is an error.
 */
#ifndef BPS_PORT_H
#define BPS_PORT_H

#include <stdarg.h>

#include "bps_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Monotonic microseconds since boot. On these chips it shares a crystal with
 * the I2S clock, so audio and timer do not drift apart. */
uint64_t port_mono_us(void);
/* The chip's unique id (padded with zeros if shorter). */
void port_unique_id(uint8_t out[8]);
/* Free heap bytes (the ring buffer leaves 32 KB of it). */
size_t port_free_heap(void);
/* Prints a line to the serial port, and hands it to bps_log_capture()
 * (logbuf.h) for the trace log. Any task or core; never from an interrupt. */
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void port_log(const char *fmt, ...);
/* The trace log's lock (logbuf.h): takes it if it is free, and never waits.
 * Never called from an interrupt. */
bool port_log_trylock(void);
void port_log_unlock(void);

/* Starts (or restarts) joining the network; Wi-Fi power saving off. After
 * port_wifi_off() it turns the radio back on first. */
void port_wifi_begin(const char *ssid, const char *password, const char *hostname);
/* Leaves the network and turns the radio off, to save power (batch mode).
 * False where the board cannot. */
bool port_wifi_off(void);
/* How the radio uses power while connected (low-power live mode, client.c):
 * FULL, as port_wifi_begin() leaves it: power saving off, and the access
 * point's beacons captured (port_beacon_take()). AWAKE: power saving off, no
 * beacon capture. SAVE: modem sleep (the radio wakes for the beacons it must
 * hear, and the access point holds packets for it meanwhile), no beacon
 * capture. Kept across reconnects and port_wifi_off(). False (FULL stays)
 * where the board cannot. */
enum { PORT_WIFI_FULL, PORT_WIFI_AWAKE, PORT_WIFI_SAVE };
bool port_wifi_power(int level);
/* Runs the CPU slower (`low`), or at its usual speed; nothing where the board
 * cannot. */
void port_cpu_low(bool low);
/* What the board does to save power, for the trace log (CPU clock, the
 * radio's power saving); empty if the board has nothing to add. */
void port_power_diag(char *out, size_t cap);
/* Lets the CPU rest for up to `ms` (the loop has nothing to do meanwhile). */
void port_idle_ms(uint32_t ms);
bool port_wifi_up(void);
/* Signal strength of the access point, dBm (0 if unknown). */
int port_wifi_rssi(void);
/* A short description of the Wi-Fi link for the log (power saving, access
 * point); empty if the board has nothing to add. */
void port_wifi_diag(char *out, size_t cap);

int port_tcp_connect(const char *host, uint16_t port);
/* Bytes accepted (0 when the send buffer is full), or -1. */
int port_tcp_write(int h, const void *buf, size_t len);
/* Bytes read (0 when none yet), or -1 once closed or failed. */
int port_tcp_read(int h, void *buf, size_t len);
void port_tcp_close(int h);

#ifdef BPS_NET_DEBUG
/* A TCP connection's state, for diagnosing slow uploads. */
typedef struct {
    uint32_t retransmits; /* of the oldest unacknowledged segment */
    uint32_t rto_ms;      /* retransmission timeout */
    uint32_t cwnd;        /* congestion window, bytes */
    uint32_t peer_wnd;    /* receive window the server advertises, bytes */
    uint32_t unacked;     /* bytes sent, not yet acknowledged */
} port_tcp_stats;
/* False where the board cannot tell. */
bool port_tcp_get_stats(int h, port_tcp_stats *out);
#endif

int port_udp_open(const char *host, uint16_t port);
int port_udp_send(int h, const void *buf, size_t len);
/* Size of the datagram read (0 when none), or -1. */
int port_udp_recv(int h, void *buf, size_t len);
void port_udp_close(int h);

/* A beacon of the access point the board is connected to: the TSF (the AP's
 * clock) it carries, and when it arrived on the monotonic clock. */
typedef struct {
    uint8_t bssid[6];
    uint64_t tsf_us;
    int64_t mono_ns;
} port_beacon;
/* Takes the next beacon heard, if any. Boards whose radio cannot timestamp
 * beacons (the Pico W's) never have one. */
bool port_beacon_take(port_beacon *out);

/* ---- Buses (see devices.h) ---- */

/* Starts the I2C bus as controller; false where the board has none. */
bool port_i2c_begin(int sda, int scl, uint32_t hz);
/* Writes `tx_len` bytes to the device at 7-bit address `addr`, then (with a
 * repeated start) reads `rx_len`; either may be 0. False if it does not answer. */
bool port_i2c_xfer(uint8_t addr, const uint8_t *tx, size_t tx_len, uint8_t *rx, size_t rx_len);

/* ---- Storage card (microSD over SPI) ---- */

/* Starts looking after the card with chip select `cs`: it is mounted when
 * found and again whenever it comes back after being pulled out. Card I/O runs
 * on its own task, so a slow card never holds up the caller. False where the
 * board cannot have one. */
bool port_sd_begin(int sck, int miso, int mosi, int cs);

typedef struct {
    bool present; /* a card is mounted */
    uint64_t total_bytes, free_bytes;
    uint32_t errors;        /* failed writes and mounts lost, so far */
    uint64_t dropped_bytes; /* not written: no card, or the queue to it was full */
    bool held;              /* closed by port_sd_hold() (then not `present`) */
} port_sd_info;
void port_sd_info_get(port_sd_info *out);

/* Holding the card (the battery is low, power.h): what is already queued is
 * written, the open file closed and the card unmounted, and no card I/O runs
 * until it is let go (writes, removes, reads and listings return false, and
 * do not count as dropped). Letting go mounts it again. */
void port_sd_hold(bool hold);

/* Queues `len` bytes for the file at `path` (relative to the card's root;
 * missing directories are made), replacing the file (`append` false) or added
 * at its end. Writes happen in order; the file stays open until another file
 * is written or port_sd_close(). False (and the bytes are dropped) when there
 * is no card or the queue is full. */
bool port_sd_write(const char *path, const void *buf, size_t len, bool append);
/* Queues closing the open file. */
void port_sd_close(void);
/* Queues removing the file at `path`. */
bool port_sd_remove(const char *path);

/* Reads and listings go through the same queue as writes (so they see every
 * write queued before them), one at a time; port_sd_result() gives the
 * outcome. Reading: up to `len` (at most PORT_SD_READ_MAX) bytes of the file
 * at `path` from `offset`. Listing: the names in directory `dir` that end in
 * `suffix`, one per line, as many as fit in PORT_SD_READ_MAX bytes. False
 * (and nothing queued) without a card, or while one is still to finish. */
#define PORT_SD_READ_MAX 4096
bool port_sd_read(const char *path, uint32_t offset, size_t len);
bool port_sd_list(const char *dir, const char *suffix);
#define PORT_SD_PENDING (-2)
/* PORT_SD_PENDING while the latest read or listing is still to finish, -1 if
 * it failed (no such file or directory), else the bytes now at `*data` (0 at
 * the end of a file; for a listing, `*total` is how many names matched). The
 * bytes stay valid until the next read or listing. */
int port_sd_result(const void **data, uint32_t *total);

/* ---- On-device models (validate.h) ---- */

/* Large buffers, in PSRAM where there is some; 16-byte aligned. */
void *port_big_alloc(size_t n);
void port_big_free(void *p);

/* A model's input or output tensor. */
typedef struct {
    bool is_int8; /* else float32 */
    void *data;
    size_t bytes;
    float scale; /* int8: real = (q - zero_point) * scale */
    int32_t zero_point;
} port_ml_tensor;

/* Sets up TFLite Micro for the `model_bytes` model at `model` (which must stay
 * put), with a tensor arena of `arena_bytes`. False (with the reason in `err`)
 * where the board cannot run models, or the model does not load, or is not a
 * well-formed model of that size (it is checked before use). */
bool port_ml_load(const void *model, size_t model_bytes, size_t arena_bytes, port_ml_tensor *in,
                  port_ml_tensor *out, size_t *arena_used, char *err, size_t err_cap);
bool port_ml_invoke(void);
void port_ml_unload(void);

/* A low-priority task, off the capture core, that runs `fn` each time it is
 * kicked (validation's front end and model, which take tens of ms to
 * seconds). False where there is none. */
bool port_worker_start(void (*fn)(void));
void port_worker_kick(void);

/* Whether everything queued for the storage card has been written (true
 * without a card): the client waits for it before restarting. */
bool port_sd_idle(void);

/* ---- Maintenance (commands from the server; client.c) ---- */

/* Why the board last started. */
enum {
    BPS_BOOT_UNKNOWN,
    BPS_BOOT_POWER_ON,
    BPS_BOOT_REQUESTED, /* port_restart() after port_keep_command(id, true) */
    BPS_BOOT_WATCHDOG,
    BPS_BOOT_BROWNOUT,
    BPS_BOOT_PANIC,
    BPS_BOOT_OTHER,
};
typedef struct {
    int reason;           /* BPS_BOOT_* */
    int64_t last_command; /* the last command run before the start; 0 if none or forgotten */
} port_boot;
/* Called once, at setup. A requested restart is reported once: its mark is
 * cleared. */
void port_boot_info(port_boot *out);
/* Keeps `id`, the last command run, across restarts (and power cycles where
 * the board can); with `restarting`, marks the coming software restart as one
 * the server asked for. */
void port_keep_command(int64_t id, bool restarting);
/* Restarts the board now. */
void port_restart(void);

/* ---- Firmware updates over the air (ota.h; client.c) ---- */

/* False where the board cannot update itself (the Pico boards, the host
 * build): it then neither advertises updates nor is offered any. */
bool port_ota_supported(void);

typedef struct {
    int64_t build_id; /* the build an update was last started for; 0 if none is pending */
    bool trial;       /* running that build, which has yet to confirm itself */
    bool rolled_back; /* it never did, and the bootloader went back to this firmware */
} port_ota_state;
/* Called once, at setup. */
void port_ota_boot_info(port_ota_state *out);
/* The build on trial is healthy: keeps it (no rollback any more) and forgets
 * the pending build id. */
void port_ota_confirm(void);
/* Forgets the pending build id (a rollback has been reported). */
void port_ota_forget(void);
/* Remembers `build_id` as pending (across restarts), stops capture, downloads
 * http://host:port`path` into the other app slot and restarts into it. Returns
 * true where the restart is left to the caller; on failure false, with capture
 * running again, the pending id forgotten, and the reason in `err`. */
bool port_ota_update(const char *host, uint16_t port, const char *path, int64_t build_id,
                     char *err, size_t err_cap);

/* Starts I2S capture on the capture core; it then calls bps_capture_frames(). */
void port_audio_start(uint32_t sample_rate);

void port_led(bool on);
/* True while the positioning button is held (false if there is none). */
bool port_button(void);

#ifdef __cplusplus
}
#endif

#endif
