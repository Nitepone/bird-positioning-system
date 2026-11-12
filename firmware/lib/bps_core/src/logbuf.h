/*
 * The trace log: while it is on, every line the board logs (port_log) is also
 * kept in a ring in RAM, and the client's heartbeats carry the lines to the
 * server. Every board's port_log hands its formatted line to
 * bps_log_capture(), and keeps printing it to the serial port as before.
 *
 * Lines come from several tasks (and, on the Picos, both cores), so the ring
 * has a lock: port_log_trylock(), which never waits. A line logged while it is
 * held is retried briefly, then dropped and counted, so logging never holds up
 * a task (capture never logs). port_log is never called from an interrupt.
 *
 * Lines are delivered at least once: a heartbeat's lines count as delivered
 * only once the server has answered it with a 2xx (bps_log_delivered()).
 * When the ring is full the oldest undelivered lines go, and are counted.
 */
#ifndef BPS_LOGBUF_H
#define BPS_LOGBUF_H

#include "bps_common.h"
#include "proto.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The ring's size (a power of two). The Pico W has the least RAM to spare. */
#ifndef BPS_LOG_RING_BYTES
#if defined(ARDUINO_ARCH_RP2040)
#define BPS_LOG_RING_BYTES 4096
#else
#define BPS_LOG_RING_BYTES 8192
#endif
#endif
/* Longest line kept; longer ones are cut. */
#define BPS_LOG_LINE_MAX 240

/* A line just logged, at `mono_us` (port_mono_us()). Kept only while capture
 * is on. Any task; never from an interrupt; never waits. Bytes outside
 * printable ASCII are kept as '?' (or ' ' for control characters), so the
 * heartbeat stays valid JSON whatever was logged. */
void bps_log_capture(uint64_t mono_us, const char *text);

/* Turns capture on or off (the client's loop). Lines already captured stay,
 * to be delivered. */
void bps_log_set_capture(bool on);
bool bps_log_capturing(void);

/* What one heartbeat carries of the ring. */
typedef struct {
    bool valid; /* the ring was read: the rest means something */
    bool more;  /* lines were left out for want of room */
    uint32_t lines;
    uint32_t end_seq, end_pos, contended;
} bps_log_batch;

/* Writes `"dropped":N,"lines":[[ms,"text"],...]` into the open object at `j`:
 * the lines not yet delivered, oldest first, as many as fit in `budget`
 * bytes (`now_ms` is port_mono_us() / 1000, to give each line's time in full),
 * and how many were lost since the last delivery. Writes nothing (and `b` is
 * not valid) if the budget is too small to be sure of the fixed part, or the
 * ring is busy: the lines then wait for the next heartbeat. */
void bps_log_json(bps_json *j, size_t budget, uint64_t now_ms, bps_log_batch *b);
/* The heartbeat that carried `b` was delivered (2xx). */
void bps_log_delivered(const bps_log_batch *b);
/* Lines (or losses) not yet delivered. */
bool bps_log_pending(void);
/* Empties the ring and turns capture off (tests). */
void bps_log_reset(void);

#ifdef __cplusplus
}
#endif

#endif
