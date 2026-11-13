/*
 * Keeping the storage card safe from a failing battery. A card that loses
 * power, or browns out, while it writes can corrupt its controller's own
 * records and never start again. As a cell runs down every Wi-Fi burst and
 * card write dips it lower, so below a cutoff the client closes the card and
 * holds its writes until the cell is back over a higher voltage (the gap
 * keeps it from flapping on the cell's recovery under a lighter load).
 */
#ifndef BPS_POWER_H
#define BPS_POWER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t cut_mv;    /* close the card under this; 0: never */
    uint32_t resume_mv; /* open it again at or over this */
} bps_power_cfg;

typedef struct {
    bool low;
} bps_power;

enum { BPS_POWER_SAME = 0, BPS_POWER_WENT_LOW = 1, BPS_POWER_RECOVERED = -1 };

/* Feeds a reading of the cell: `have` false when there is none (no gauge, or
 * one running off the charger with no cell), which is never low. Returns
 * BPS_POWER_WENT_LOW or BPS_POWER_RECOVERED on a change, else BPS_POWER_SAME. */
int bps_power_update(bps_power *p, const bps_power_cfg *c, bool have, uint32_t mv);

#endif
