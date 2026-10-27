#include "devices.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "port.h"

extern const bps_driver bps_max17048_driver;
extern const bps_driver bps_sd_driver;

static const bps_driver *const DRIVERS[] = {
    &bps_max17048_driver,
    &bps_sd_driver,
};

#define I2C_HZ 100000

static bps_bus_settings buses = {-1, -1, -1, -1, -1, ""};
static bps_device devs[BPS_MAX_DEVICES];
static size_t n_devs;

const bps_bus_settings *bps_devices_buses(void) { return &buses; }

/* "I2C address 0x36" or "SPI chip select 2". */
static void where(const bps_device *d, char *out, size_t cap)
{
    if (d->drv->bus == BPS_BUS_I2C)
        snprintf(out, cap, "I2C address 0x%02x", d->address);
    else if (d->address >= 0)
        snprintf(out, cap, "SPI chip select %d", d->address);
    else
        snprintf(out, cap, "SPI");
}

const bps_driver *bps_driver_find(const char *kind, size_t len)
{
    for (size_t i = 0; i < sizeof DRIVERS / sizeof DRIVERS[0]; i++)
        if (strlen(DRIVERS[i]->kind) == len && memcmp(DRIVERS[i]->kind, kind, len) == 0)
            return DRIVERS[i];
    return NULL;
}

int bps_devices_parse(const char *list, bps_device *out, size_t cap, char *err, size_t err_cap)
{
    size_t n = 0;
    const char *p = list ? list : "";
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\t')
            p++;
        if (!*p)
            return (int)n;
        const char *kind = p;
        while (*p && *p != '@' && *p != ' ' && *p != ',' && *p != '\t')
            p++;
        size_t kind_len = (size_t)(p - kind);
        const bps_driver *drv = bps_driver_find(kind, kind_len);
        if (!drv) {
            snprintf(err, err_cap, "unknown device \"%.*s\"", (int)kind_len, kind);
            return -1;
        }
        int address = drv->default_address;
        if (*p == '@') {
            char *end;
            long v = strtol(p + 1, &end, 0);
            if (end == p + 1 || (*end && *end != ' ' && *end != ',' && *end != '\t')) {
                snprintf(err, err_cap, "%s: the address after @ is not a number", drv->kind);
                return -1;
            }
            address = (int)v;
            p = end;
        }
        /* An SPI device may leave its chip select to the board (the host
         * build's storage is a directory). */
        if (address < 0 && drv->bus == BPS_BUS_I2C) {
            snprintf(err, err_cap, "%s needs an address (%s@...)", drv->kind, drv->kind);
            return -1;
        }
        if (n == cap) {
            snprintf(err, err_cap, "more than %u devices", (unsigned)cap);
            return -1;
        }
        memset(&out[n], 0, sizeof out[n]);
        out[n].drv = drv;
        out[n].address = address;
        n++;
    }
}

void bps_devices_setup(const bps_bus_settings *s)
{
    buses = *s;
    char err[64];
    int n = bps_devices_parse(s->devices, devs, BPS_MAX_DEVICES, err, sizeof err);
    if (n < 0) {
        port_log("devices: %s; using none", err);
        n = 0;
    }
    n_devs = (size_t)n;
    bool i2c = false;
    for (size_t i = 0; i < n_devs; i++)
        i2c |= devs[i].drv->bus == BPS_BUS_I2C;
    bool i2c_up = i2c && s->i2c_sda >= 0 && s->i2c_scl >= 0 &&
                  port_i2c_begin(s->i2c_sda, s->i2c_scl, I2C_HZ);
    if (i2c && !i2c_up)
        port_log("devices: no I2C bus (SDA %d, SCL %d); its devices are left out", s->i2c_sda,
                 s->i2c_scl);
    for (size_t i = 0; i < n_devs; i++) {
        bps_device *d = &devs[i];
        if (d->drv->bus == BPS_BUS_I2C && !i2c_up)
            continue;
        d->started = true;
        d->present = d->drv->start(d);
        char at[32];
        where(d, at, sizeof at);
        port_log("devices: %s on %s: %s", d->drv->kind, at, d->present ? "found" : "not found (yet)");
    }
}

void bps_devices_poll(uint64_t now_us)
{
    for (size_t i = 0; i < n_devs; i++) {
        bps_device *d = &devs[i];
        if (!d->started || now_us < d->next_poll_us)
            continue;
        d->next_poll_us = now_us + (uint64_t)d->drv->poll_ms * 1000;
        d->drv->poll(d);
    }
}

static const char *bus_name(const bps_device *d) { return d->drv->bus == BPS_BUS_I2C ? "i2c" : "spi"; }

void bps_devices_heartbeat(bps_json *j)
{
    for (size_t i = 0; i < n_devs; i++) {
        const char *role = devs[i].drv->role;
        /* The first device of each role that is present, else the first. */
        const bps_device *pick = NULL;
        bool seen_before = false;
        for (size_t k = 0; k < n_devs; k++) {
            if (strcmp(devs[k].drv->role, role) != 0)
                continue;
            if (k < i) {
                seen_before = true;
                break;
            }
            if (!pick || (!pick->present && devs[k].present))
                pick = &devs[k];
        }
        if (!seen_before && pick)
            pick->drv->heartbeat(pick, j);
    }
    if (n_devs == 0)
        return;
    bps_json_arr(j, "devices");
    for (size_t i = 0; i < n_devs; i++) {
        if (i)
            bps_json_raw(j, ",");
        bps_json_raw(j, "{");
        bps_json_str(j, "kind", devs[i].drv->kind);
        bps_json_str(j, "bus", bus_name(&devs[i]));
        bps_json_i64(j, "address", devs[i].address);
        bps_json_bool(j, "present", devs[i].present);
        bps_json_raw(j, "}");
    }
    bps_json_raw(j, "]");
}

void bps_devices_describe(char *out, size_t cap)
{
    size_t len = 0;
    out[0] = 0;
    for (size_t i = 0; i < n_devs && len < cap; i++) {
        char what[96];
        if (devs[i].present)
            devs[i].drv->describe(&devs[i], what, sizeof what);
        else
            snprintf(what, sizeof what, "not found");
        int w = snprintf(out + len, cap - len, "%s%s: %s", i ? "; " : "", devs[i].drv->kind, what);
        if (w < 0)
            break;
        len += (size_t)w;
    }
}
