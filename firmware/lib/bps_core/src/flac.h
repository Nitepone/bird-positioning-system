/*
 * A small FLAC encoder for uploads: 16-bit mono, fixed-size blocks, fixed
 * predictors (orders 0-4) with Rice-coded residuals, falling back to
 * verbatim or constant subframes. Integer only, no allocation: cheap enough
 * for an RP2040. Lossless, so timing between microphones is untouched.
 *
 * A stream is the header, then one frame per BPS_FLAC_BLOCK samples (the
 * last may be shorter).
 */
#ifndef BPS_FLAC_H
#define BPS_FLAC_H

#include "bps_common.h"

#define BPS_FLAC_BLOCK 1024
/* "fLaC" and the STREAMINFO metadata block. */
#define BPS_FLAC_HEADER_LEN 42
/* The largest frame of up to BPS_FLAC_BLOCK samples (verbatim, plus header
 * and footer). */
#define BPS_FLAC_MAX_FRAME (16 + 1 + 2 * BPS_FLAC_BLOCK + 2)

/* Stream header for `total_samples` samples (0 if unknown). */
void bps_flac_header(uint8_t out[BPS_FLAC_HEADER_LEN], uint32_t sample_rate,
                     uint64_t total_samples);

/* Encodes frame `frame_number` of `n` samples (1..BPS_FLAC_BLOCK; fewer only
 * for the last frame) into `out`, which holds BPS_FLAC_MAX_FRAME bytes.
 * Returns its length. */
size_t bps_flac_frame(uint8_t *out, const int16_t *pcm, uint32_t n, uint32_t frame_number,
                      uint32_t sample_rate);

#endif
