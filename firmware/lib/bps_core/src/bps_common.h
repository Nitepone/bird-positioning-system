/* Shared constants for the bps microcontroller client core. */
#ifndef BPS_COMMON_H
#define BPS_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BPS_NANOS_PER_SEC 1000000000LL
#define BPS_NANOS_PER_MS 1000000LL

/* Nanoseconds covered by `samples` at `rate` Hz, without 128-bit arithmetic. */
static inline int64_t bps_samples_to_ns(uint64_t samples, uint32_t rate)
{
    return (int64_t)(samples / rate) * BPS_NANOS_PER_SEC +
           (int64_t)((samples % rate) * (uint64_t)BPS_NANOS_PER_SEC / rate);
}

static inline int64_t bps_abs64(int64_t v) { return v < 0 ? -v : v; }

#endif
