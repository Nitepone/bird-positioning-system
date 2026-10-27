/*
 * MAX17048 battery fuel gauge (I2C, address 0x36): state of charge, cell
 * voltage and charge rate of a single lithium cell. It is powered from the
 * cell's side of the charger: with no battery it either does not answer or,
 * on USB power, reads the charger's output, above any lithium cell.
 *
 * Its charge estimate starts from the voltage it sees at power-up. One that
 * started off the charger is wrong once a battery is connected (well over
 * 100%), so it is restarted from the cell's voltage (a quick-start).
 */
#include <stdio.h>
#include <stdlib.h>

#include "devices.h"
#include "port.h"

#define REG_VCELL 0x02   /* 78.125 uV per bit */
#define REG_SOC 0x04     /* 1/256 % per bit */
#define REG_VERSION 0x08 /* 0x001_ */
#define REG_MODE 0x06    /* write QUICK_START to restart the estimate */
#define REG_CRATE 0x16   /* 0.208 %/h per bit, signed */
#define QUICK_START 0x4000

#define NO_BATTERY_MV 4500     /* above this, it reads the charger */
#define IMPLAUSIBLE_CENTI 10500 /* a charge over 105% */

static bool read_reg(const bps_device *d, uint8_t reg, uint16_t *out)
{
    uint8_t b[2];
    if (!port_i2c_xfer((uint8_t)d->address, &reg, 1, b, 2))
        return false;
    *out = (uint16_t)(b[0] << 8 | b[1]);
    return true;
}

static bool write_reg(const bps_device *d, uint8_t reg, uint16_t v)
{
    uint8_t b[3] = {reg, (uint8_t)(v >> 8), (uint8_t)v};
    return port_i2c_xfer((uint8_t)d->address, b, 3, NULL, 0);
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
        g->restarted = false; /* it powers up afresh, with an estimate of its own */
        return;
    }
    d->present = true;
    bool had_none = g->valid && !g->battery;
    g->millivolts = (uint16_t)((uint64_t)vcell * 78125 / 1000000);
    g->centi_percent = (int32_t)((uint32_t)soc * 100 / 256);
    g->centi_pct_per_h = (int32_t)(int16_t)crate * 208 / 10;
    g->valid = true;
    g->battery = g->millivolts <= NO_BATTERY_MV;
    if (!g->battery) {
        g->restarted = false;
        return;
    }
    /* Once per battery; the new estimate is read at the next poll. */
    if (!g->restarted && (had_none || g->centi_percent > IMPLAUSIBLE_CENTI) &&
        write_reg(d, REG_MODE, QUICK_START)) {
        port_log("max17048: %s; restarting its charge estimate",
                 had_none ? "a battery was connected" : "charge estimate over 105%");
        g->restarted = true;
        g->valid = false;
    }
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
    if (!d->present || !g->valid || !g->battery)
        return;
    bps_json_obj(j, "battery");
    bps_json_fixed(j, "percent", g->centi_percent / 100.0, 2);
    bps_json_fixed(j, "voltage_v", g->millivolts / 1000.0, 3);
    bps_json_fixed(j, "rate_pct_per_h", g->centi_pct_per_h / 100.0, 2);
    bps_json_raw(j, "}");
}

static void describe(const bps_device *d, char *out, size_t cap)
{
    const bps_max17048 *g = &d->u.gauge;
    long rate = labs((long)g->centi_pct_per_h);
    if (!g->valid) {
        snprintf(out, cap, "restarting its estimate");
        return;
    }
    if (!g->battery) {
        snprintf(out, cap, "no battery (%u mV from the charger)", (unsigned)g->millivolts);
        return;
    }
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
