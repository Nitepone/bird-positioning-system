/*
 * MAX17048 battery fuel gauge (I2C, address 0x36): state of charge, cell
 * voltage and charge rate of a single lithium cell. It is powered by the
 * battery, so with none connected it does not answer.
 */
#include <stdio.h>
#include <stdlib.h>

#include "devices.h"
#include "port.h"

#define REG_VCELL 0x02   /* 78.125 uV per bit */
#define REG_SOC 0x04     /* 1/256 % per bit */
#define REG_VERSION 0x08 /* 0x001_ */
#define REG_CRATE 0x16   /* 0.208 %/h per bit, signed */

static bool read_reg(const bps_device *d, uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    if (!port_i2c_xfer((uint8_t)d->address, &reg, 1, b, 2))
        return false;
    *out = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}

static bool identify(const bps_device *d)
{
    uint16_t v;
    return read_reg(d, REG_VERSION, &v) && (v & 0xfff0) == 0x0010;
}

static void poll(bps_device *d)
{
    bps_max17048 *g = &d->u.gauge;
    uint16_t vcell, soc, crate;
    /* After a battery is connected the gauge powers up afresh: look again. */
    if (!d->present && !identify(d))
        return;
    if (!read_reg(d, REG_VCELL, &vcell) || !read_reg(d, REG_SOC, &soc) ||
        !read_reg(d, REG_CRATE, &crate)) {
        d->present = false;
        g->valid = false;
        return;
    }
    d->present = true;
    g->valid = true;
    g->millivolts = (uint16_t)((uint64_t)vcell * 78125 / 1000000);
    g->centi_percent = (int32_t)((uint32_t)soc * 100 / 256);
    g->centi_pct_per_h = (int32_t)(int16_t)crate * 208 / 10;
}

static bool start(bps_device *d)
{
    d->present = identify(d);
    if (d->present)
        poll(d);
    return d->present;
}

static void heartbeat(const bps_device *d, bps_json *j)
{
    const bps_max17048 *g = &d->u.gauge;
    if (!d->present || !g->valid)
        return;
    bps_json_key(j, "battery");
    bps_json_raw(j, "{");
    bps_json_key(j, "percent");
    bps_json_fixed(j, g->centi_percent / 100.0, 2);
    bps_json_key(j, "voltage_v");
    bps_json_fixed(j, g->millivolts / 1000.0, 3);
    bps_json_key(j, "rate_pct_per_h");
    bps_json_fixed(j, g->centi_pct_per_h / 100.0, 2);
    bps_json_raw(j, "}");
}

static void describe(const bps_device *d, char *out, size_t cap)
{
    const bps_max17048 *g = &d->u.gauge;
    long rate = labs((long)g->centi_pct_per_h);
    snprintf(out, cap, "battery %ld.%02ld%%, %u mV, %s%ld.%02ld %%/h",
             (long)(g->centi_percent / 100), (long)(g->centi_percent % 100),
             (unsigned)g->millivolts, g->centi_pct_per_h < 0 ? "-" : "", rate / 100, rate % 100);
}

const bps_driver bps_max17048_driver = {
    .kind = "max17048",
    .bus = BPS_BUS_I2C,
    .default_address = 0x36,
    .role = "battery",
    .poll_ms = 10000,
    .start = start,
    .poll = poll,
    .heartbeat = heartbeat,
    .describe = describe,
};
