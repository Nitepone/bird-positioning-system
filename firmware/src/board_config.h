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
/* 1: a PDM mic, whose clock is BPS_I2S_BCLK (no word select). */
#ifndef BPS_MIC_PDM
#define BPS_MIC_PDM 0
#endif

/* Peripheral buses (-1: none) and the devices on them, "kind@address ...". */
#ifndef BPS_I2C_SDA
#define BPS_I2C_SDA (-1)
#endif
#ifndef BPS_I2C_SCL
#define BPS_I2C_SCL (-1)
#endif
#ifndef BPS_SPI_SCK
#define BPS_SPI_SCK (-1)
#endif
#ifndef BPS_SPI_MISO
#define BPS_SPI_MISO (-1)
#endif
#ifndef BPS_SPI_MOSI
#define BPS_SPI_MOSI (-1)
#endif
#ifndef BPS_DEVICES
#define BPS_DEVICES ""
#endif

#define BPS_FIRMWARE_VERSION "bps-mcu 0.1.0"
#define BPS_SAMPLE_RATE 48000

#endif
