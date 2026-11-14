/*
 * Recordings waiting to be uploaded (upload.c): each chunk's FLAC stream is
 * written here as it is encoded, and goes to the server once it is complete.
 * Kept in PSRAM where there is some (port_big_alloc), so uploads are short
 * bursts after the chunk rather than a trickle for as long as it lasts, and
 * survive the network being away for a while.
 *
 * Items are kept in order, oldest first: one is added at the back and written
 * until closed (or cancelled); they leave from the front. Bytes are kept in a
 * circular buffer, so an item's bytes may wrap around its end: read them with
 * bps_store_read(). Offsets are from the start of an item's stream.
 */
#ifndef BPS_STORE_H
#define BPS_STORE_H

#include "bps_common.h"

/* Items held at most (a 3 MB store holds about 12 chunks of 9 s; one sized
 * to the free PSRAM, up to about 6 MB). */
#define BPS_STORE_ITEMS 48

typedef struct {
    uint32_t pos;  /* where its byte at offset `base` is in the buffer */
    uint32_t base; /* bytes released from its front (bps_store_release) */
    uint32_t len;  /* bytes written so far */
} bps_store_item;

typedef struct {
    uint8_t *buf;
    uint32_t cap;  /* bytes */
    uint32_t used; /* bytes held by items */
    bps_store_item items[BPS_STORE_ITEMS];
    uint32_t first, count; /* items[first] is the front */
    bool open;             /* the item at the back is still being written */
} bps_store;

/* Uses `bytes` at `buf` (NULL or 0: holds nothing). */
void bps_store_init(bps_store *s, uint8_t *buf, uint32_t bytes);

/* Starts an item at the back; returns its slot (stable while it is held), or
 * -1 when every slot is taken or one is still being written. */
int bps_store_open(bps_store *s);
/* Adds to the item being written. False (nothing added) if it does not fit. */
bool bps_store_append(bps_store *s, const void *p, uint32_t n);
/* The item being written is complete. */
void bps_store_close(bps_store *s);
/* The item being written is given up, and its bytes freed. */
void bps_store_cancel(bps_store *s);

/* Items held, and the slot of the k-th from the front (-1 if none). */
uint32_t bps_store_count(const bps_store *s);
int bps_store_at(const bps_store *s, uint32_t k);
/* Whether `slot` is the item still being written. */
bool bps_store_is_open(const bps_store *s, int slot);
/* Bytes of the item written so far (whether or not released). */
uint32_t bps_store_len(const bps_store *s, int slot);
/* Copies up to `n` bytes of item `slot` from offset `off` (not below what was
 * released); returns how many. */
uint32_t bps_store_read(const bps_store *s, int slot, uint32_t off, void *out, uint32_t n);
/* The front item's bytes before offset `off` are not needed any more (an
 * upload streaming it as it is written); their space is freed. */
void bps_store_release(bps_store *s, uint32_t off);
/* Removes the front item. */
void bps_store_pop(bps_store *s);

uint32_t bps_store_free(const bps_store *s);
/* Moves what is held to the buffer's start, then has `re` (realloc-like:
 * NULL on failure, the old buffer kept) make the buffer `cap` bytes. False
 * (still the old size, its contents moved) if more than `cap` is held or `re`
 * fails. Items keep their slots and offsets. */
bool bps_store_resize(bps_store *s, uint32_t cap, void *(*re)(void *p, size_t n));

#endif
