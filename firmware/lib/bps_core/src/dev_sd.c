/*
 * A microSD card on the SPI bus (its chip select as the address). The board
 * layer mounts it, and again when it is put back after being pulled out;
 * record.c keeps a copy of each recording on it.
 */
#include <stdio.h>

#include "devices.h"
#include "port.h"
#include "record.h"

static void poll(bps_device *d)
{
    port_sd_info info;
    port_sd_info_get(&info);
    d->present = info.present;
}

static bool start(bps_device *d)
{
    const bps_bus_settings *b = bps_devices_buses();
    if (!port_sd_begin(b->spi_sck, b->spi_miso, b->spi_mosi, d->address))
        return false;
    poll(d);
    return true; /* the card may come later */
}

static void heartbeat(const bps_device *d, bps_json *j)
{
    (void)d;
    port_sd_info info;
    port_sd_info_get(&info);
    bps_record_stats rec;
    bps_record_stats_get(&rec);
    bps_json_key(j, "storage");
    bps_json_raw(j, "{");
    bps_json_key(j, "present");
    bps_json_bool(j, info.present);
    if (info.present) {
        bps_json_key(j, "total_bytes");
        bps_json_i64(j, (int64_t)info.total_bytes);
        bps_json_key(j, "free_bytes");
        bps_json_i64(j, (int64_t)info.free_bytes);
    }
    bps_json_key(j, "recordings");
    bps_json_i64(j, rec.files);
    bps_json_key(j, "recorded_bytes");
    bps_json_i64(j, (int64_t)rec.bytes);
    bps_json_key(j, "errors");
    bps_json_i64(j, info.errors);
    bps_json_key(j, "dropped_bytes");
    bps_json_i64(j, (int64_t)info.dropped_bytes);
    bps_json_raw(j, "}");
}

static void describe(const bps_device *d, char *out, size_t cap)
{
    (void)d;
    port_sd_info info;
    port_sd_info_get(&info);
    bps_record_stats rec;
    bps_record_stats_get(&rec);
    snprintf(out, cap, "%lu of %lu MB free, %lu recordings saved, %lu errors",
             (unsigned long)(info.free_bytes >> 20), (unsigned long)(info.total_bytes >> 20),
             (unsigned long)rec.files, (unsigned long)info.errors);
}

const bps_driver bps_sd_driver = {
    .kind = "sd",
    .bus = BPS_BUS_SPI,
    .default_address = -1,
    .role = "storage",
    .poll_ms = 1000,
    .start = start,
    .poll = poll,
    .heartbeat = heartbeat,
    .describe = describe,
};
