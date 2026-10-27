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
#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void port_log(const char *fmt, ...);

/* Starts (or restarts) joining the network; Wi-Fi power saving off. */
void port_wifi_begin(const char *ssid, const char *password, const char *hostname);
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
} port_sd_info;
void port_sd_info_get(port_sd_info *out);

/* Queues `len` bytes for the file at `path` (relative to the card's root;
 * missing directories are made), replacing the file (`append` false) or added
 * at its end. Writes happen in order; the file stays open until another file
 * is written or port_sd_close(). False (and the bytes are dropped) when there
 * is no card or the queue is full. */
bool port_sd_write(const char *path, const void *buf, size_t len, bool append);
/* Queues closing the open file. */
void port_sd_close(void);

/* Starts I2S capture on the capture core; it then calls bps_capture_frames(). */
void port_audio_start(uint32_t sample_rate);

void port_led(bool on);
/* True while the positioning button is held (false if there is none). */
bool port_button(void);

#ifdef __cplusplus
}
#endif

#endif
