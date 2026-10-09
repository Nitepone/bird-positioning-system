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

int port_tcp_connect(const char *host, uint16_t port);
/* Bytes accepted (0 when the send buffer is full), or -1. */
int port_tcp_write(int h, const void *buf, size_t len);
/* Bytes read (0 when none yet), or -1 once closed or failed. */
int port_tcp_read(int h, void *buf, size_t len);
void port_tcp_close(int h);

int port_udp_open(const char *host, uint16_t port);
int port_udp_send(int h, const void *buf, size_t len);
/* Size of the datagram read (0 when none), or -1. */
int port_udp_recv(int h, void *buf, size_t len);
void port_udp_close(int h);

/* Starts I2S capture on the capture core; it then calls bps_capture_frames(). */
void port_audio_start(uint32_t sample_rate);

void port_led(bool on);
/* True while the positioning button is held (false if there is none). */
bool port_button(void);

#ifdef __cplusplus
}
#endif

#endif
