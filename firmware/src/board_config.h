// Settings from bps.conf, passed in as defines by scripts/bps_config.py.
#ifndef BPS_BOARD_CONFIG_H
#define BPS_BOARD_CONFIG_H

#ifndef BPS_WIFI_SSID
#error "No configuration: run ./configure.sh in firmware/ (or set BPS_CONF) before building"
#endif

#define BPS_LED_BUILTIN (-2)

#ifndef BPS_LED_PIN
#define BPS_LED_PIN (-1)
#endif
#ifndef BPS_BUTTON_PIN
#define BPS_BUTTON_PIN (-1)
#endif

#define BPS_FIRMWARE_VERSION "bps-mcu 0.1.0"
#define BPS_SAMPLE_RATE 48000

#endif
