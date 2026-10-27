/*
 * Peripherals on the board's I2C and SPI buses: a battery gauge, a storage
 * card, and whatever comes later.
 *
 * The board's settings give the bus pins and a device list, e.g.
 * "max17048@0x36 sd@2": each entry is a driver's kind and, for I2C, its
 * address (the driver's usual one if left out) or, for SPI, its chip select
 * pin. Each device is started once, polled at its driver's interval, and adds
 * its own part to the heartbeat.
 *
 * Adding a kind of device: a driver (a bps_driver, as in dev_max17048.c), its
 * state in the union below, a line in the table in devices.c, and its entry
 * in DRIVERS in scripts/bps_config.py.
 */
#ifndef BPS_DEVICES_H
#define BPS_DEVICES_H

#include "bps_common.h"
#include "proto.h"

#define BPS_MAX_DEVICES 8

typedef enum { BPS_BUS_I2C, BPS_BUS_SPI } bps_bus;

typedef struct bps_device bps_device;

typedef struct {
    const char *kind;
    bps_bus bus;
    int default_address; /* -1: it must be given */
    /* The heartbeat key it reports under ("battery", "storage"): one device
     * per role reports, the first that is present. */
    const char *role;
    uint32_t poll_ms;
    /* Sets the device up; true if it answered. */
    bool (*start)(bps_device *d);
    /* Updates its state and `present` (a device may come and go). */
    void (*poll)(bps_device *d);
    /* Writes `"role": value` into the heartbeat object, or nothing. */
    void (*heartbeat)(const bps_device *d, bps_json *j);
    /* A few words for the status log. */
    void (*describe)(const bps_device *d, char *out, size_t cap);
} bps_driver;

typedef struct {
    bool valid;
    uint16_t millivolts;
    int32_t centi_percent;   /* state of charge */
    int32_t centi_pct_per_h; /* rate of change */
} bps_max17048;

struct bps_device {
    const bps_driver *drv;
    int address; /* I2C address or SPI chip select */
    bool started; /* false if its bus could not be started */
    bool present;
    uint64_t next_poll_us;
    union {
        bps_max17048 gauge;
    } u;
};

typedef struct {
    int i2c_sda, i2c_scl; /* -1: no I2C bus */
    int spi_sck, spi_miso, spi_mosi; /* -1: no SPI bus */
    const char *devices; /* "kind@address ...", or "" */
} bps_bus_settings;

/* Parses the device list into `out` (at most `cap`); returns how many, or -1
 * (with a message in `err`) if an entry is not understood. */
int bps_devices_parse(const char *list, bps_device *out, size_t cap, char *err, size_t err_cap);

/* Starts the buses and every device in the settings. */
void bps_devices_setup(const bps_bus_settings *s);
/* The bus pins, for drivers (an SPI device starts its own use of the bus). */
const bps_bus_settings *bps_devices_buses(void);
/* Polls the devices that are due; call from the client's loop. */
void bps_devices_poll(uint64_t now_us);
/* Adds each role's report, then `"devices": [...]`, to an open heartbeat object. */
void bps_devices_heartbeat(bps_json *j);
/* One line about every device, for the status log ("" if there are none). */
void bps_devices_describe(char *out, size_t cap);

/* The drivers (devices.c), for tests. */
const bps_driver *bps_driver_find(const char *kind, size_t len);

#endif
