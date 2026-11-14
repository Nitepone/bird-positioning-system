/*
 * The bps client on a microcontroller: registers with the server, measures
 * its clock over UDP, sends heartbeats, and uploads gated audio chunks
 * (starting a look-back before the sound that opened the gate) to the same
 * client API as bsp-client.
 */
#ifndef BPS_CLIENT_H
#define BPS_CLIENT_H

#include "bps_common.h"
#include "devices.h"
#include "ota.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *wifi_ssid;
    const char *wifi_password;
    const char *server_host;
    uint16_t server_port;
    const char *name;        /* suggested client name; "" for none */
    const char *client_id;   /* UUID; "" to derive one from the chip */
    const char *device_name; /* reported microphone name */
    const char *board;       /* e.g. "picow"; used in the hostname */
    const char *version;
    uint32_t sample_rate;
    int mic_channel; /* 0 = left, 1 = right */
    uint32_t ring_bytes;
    uint32_t lookback_ms;
    bps_bus_settings buses; /* peripherals: battery gauge, storage card */
    const bps_build *build; /* what this firmware is (registration); NULL if unknown */
    int ota_min_battery;    /* % of charge an update needs on a battery that is not charging */
    /* The storage card is closed while the cell is under sd_cut_mv, until it
     * is back at sd_resume_mv (power.h); 0: never. */
    int sd_cut_mv, sd_resume_mv;
    /* Recordings waiting to be uploaded (store.h), in PSRAM where there is
     * some. There, it is sized to what is free (leaving room for the
     * selected model), at most this unless 0. Without PSRAM, this (0: 32 KB).
     * Smaller than a chunk can be (about 900 KB for 9 s), chunks stream. */
    uint32_t store_bytes;
} bps_settings;

void bps_client_setup(const bps_settings *s);
/* Call as often as possible from the main loop. */
void bps_client_loop(void);

#ifdef __cplusplus
}
#endif

#endif
