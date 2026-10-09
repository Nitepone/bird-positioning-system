/*
 * The bps client on a microcontroller: registers with the server, measures
 * its clock over UDP, sends heartbeats, and uploads gated audio chunks
 * (starting a look-back before the sound that opened the gate) to the same
 * client API as bsp-client.
 */
#ifndef BPS_CLIENT_H
#define BPS_CLIENT_H

#include "bps_common.h"

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
} bps_settings;

void bps_client_setup(const bps_settings *s);
/* Call as often as possible from the main loop. */
void bps_client_loop(void);

#ifdef __cplusplus
}
#endif

#endif
