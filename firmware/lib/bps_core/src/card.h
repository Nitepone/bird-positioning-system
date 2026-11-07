/*
 * The storage card's reads and listings (port.h) go one at a time and share
 * one result: whoever asks holds the card until done with the answer, and
 * others wait. The spool holds it for a pass (spool.h); validation while it
 * loads a model (validate.h). Both run on the client's loop.
 */
#ifndef BPS_CARD_H
#define BPS_CARD_H

#include "bps_common.h"

typedef enum { BPS_CARD_FREE, BPS_CARD_SPOOL, BPS_CARD_MODEL } bps_card_user;

/* True if `who` holds the card: it already did, or it was free. */
bool bps_card_take(bps_card_user who);
/* Frees the card, if `who` holds it. */
void bps_card_give(bps_card_user who);
bps_card_user bps_card_holder(void);

#endif
