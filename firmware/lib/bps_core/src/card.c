#include "card.h"

static bps_card_user holder;

bool bps_card_take(bps_card_user who)
{
    if (holder != BPS_CARD_FREE && holder != who)
        return false;
    holder = who;
    return true;
}

void bps_card_give(bps_card_user who)
{
    if (holder == who)
        holder = BPS_CARD_FREE;
}

bps_card_user bps_card_holder(void) { return holder; }
