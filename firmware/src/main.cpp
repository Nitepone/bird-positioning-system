// bps microcontroller client: board setup, then the portable client core.
#include <Arduino.h>

extern "C" {
#include "client.h"
}

#include "board_config.h"

extern "C" void port_board_init(void);

void setup()
{
    port_board_init();
    static const bps_settings settings = {
        BPS_WIFI_SSID,
        BPS_WIFI_PASSWORD,
        BPS_SERVER_HOST,
        BPS_SERVER_PORT,
        BPS_CLIENT_NAME,
        BPS_CLIENT_ID,
        BPS_DEVICE_NAME,
        BPS_BOARD,
        BPS_FIRMWARE_VERSION,
        BPS_SAMPLE_RATE,
        BPS_MIC_CHANNEL,
        BPS_RING_BYTES,
        BPS_LOOKBACK_MS,
    };
    bps_client_setup(&settings);
}

void loop() { bps_client_loop(); }
