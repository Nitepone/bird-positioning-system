#include "power.h"

int bps_power_update(bps_power *p, const bps_power_cfg *c, bool have, uint32_t mv)
{
    bool low;
    if (!have || c->cut_mv == 0)
        low = false;
    else if (p->low)
        low = mv < c->resume_mv;
    else
        low = mv < c->cut_mv;
    if (low == p->low)
        return BPS_POWER_SAME;
    p->low = low;
    return low ? BPS_POWER_WENT_LOW : BPS_POWER_RECOVERED;
}
