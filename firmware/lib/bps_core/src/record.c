#include "record.h"

#include <stdio.h>
#include <string.h>

#include "port.h"
#include "proto.h"

static struct {
    bool active;
    char dir[24];   /* recordings/YYYY-MM-DD */
    char file[16];  /* HHMMSS.mmm.flac */
    char path[48];
    int64_t start_ns, end_ns;
    char tsf[80];
    uint32_t sample_rate;
    uint64_t bytes;
    uint64_t dropped; /* bytes of this file that did not reach the card */
} rec;

static bps_record_stats stats;

/* Days since 1970-01-01 to a civil date (Howard Hinnant's algorithm). */
static void civil(int64_t days, int *y, int *m, int *d)
{
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*m <= 2));
}

void bps_record_names(int64_t ns, char day[11], char time[11])
{
    const int64_t day_ns = 86400 * BPS_NANOS_PER_SEC;
    int64_t days = ns / day_ns, in_day = ns % day_ns;
    if (in_day < 0) {
        days--;
        in_day += day_ns;
    }
    int y, m, d;
    civil(days, &y, &m, &d);
    int64_t ms = in_day / BPS_NANOS_PER_MS;
    snprintf(day, 11, "%04u-%02u-%02u", (unsigned)y % 10000, (unsigned)m % 100, (unsigned)d % 100);
    snprintf(time, 11, "%02d%02d%02d.%03d", (int)(ms / 3600000), (int)(ms / 60000 % 60),
             (int)(ms / 1000 % 60), (int)(ms % 1000));
}

static void rec_write(const void *buf, size_t len, bool append)
{
    if (port_sd_write(rec.path, buf, len, append))
        rec.bytes += len;
    else
        rec.dropped += len;
}

void bps_record_begin(int64_t start_ns, int64_t end_ns, const char *tsf, uint32_t sample_rate,
                      uint32_t samples, const uint8_t *head, size_t len)
{
    (void)samples;
    rec.active = false;
    port_sd_info info;
    port_sd_info_get(&info);
    if (!info.present || info.free_bytes < BPS_RECORD_MIN_FREE) {
        stats.skipped++;
        return;
    }
    char day[11], time[11];
    bps_record_names(start_ns, day, time);
    snprintf(rec.dir, sizeof rec.dir, "recordings/%s", day);
    snprintf(rec.file, sizeof rec.file, "%s.flac", time);
    snprintf(rec.path, sizeof rec.path, "%s/%s", rec.dir, rec.file);
    snprintf(rec.tsf, sizeof rec.tsf, "%s", tsf ? tsf : "");
    rec.start_ns = start_ns;
    rec.end_ns = end_ns;
    rec.sample_rate = sample_rate;
    rec.bytes = rec.dropped = 0;
    rec.active = true;
    rec_write(head, len, false);
}

void bps_record_frame(const uint8_t *frame, size_t len)
{
    if (rec.active)
        rec_write(frame, len, true);
}

void bps_record_end(uint32_t samples, uint32_t lost, const char *upload)
{
    if (!rec.active)
        return;
    rec.active = false;
    port_sd_close();
    char line[384], index[48];
    bps_json j;
    bps_json_init(&j, line, sizeof line - 1);
    bps_json_raw(&j, "{");
    bps_json_str(&j, "file", rec.file);
    bps_json_i64(&j, "start_ns", rec.start_ns);
    bps_json_i64(&j, "end_ns", rec.end_ns);
    bps_json_i64(&j, "sample_rate", rec.sample_rate);
    bps_json_i64(&j, "samples", samples);
    bps_json_i64(&j, "lost_samples", lost);
    bps_json_i64(&j, "bytes", (int64_t)rec.bytes);
    if (rec.dropped) { /* the file misses some of its frames */
        bps_json_i64(&j, "dropped_bytes", (int64_t)rec.dropped);
    }
    if (rec.tsf[0]) {
        bps_json_str(&j, "tsf", rec.tsf);
    }
    bps_json_str(&j, "upload", upload);
    bps_json_raw(&j, "}\n");
    if (j.overflow)
        return;
    snprintf(index, sizeof index, "%s/index.jsonl", rec.dir);
    port_sd_write(index, line, j.len, true);
    port_sd_close();
    stats.files++;
    stats.bytes += rec.bytes;
}

void bps_record_stats_get(bps_record_stats *out) { *out = stats; }
