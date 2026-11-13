/*
 * The build's identity, embedded once in the image as a marker and a JSON
 * object (the server reads it from an uploaded firmware.bin), and handed to
 * the client for its registration. The defines come from
 * scripts/bps_config.py, for this file only: the build time changes on every
 * build, so only this file is compiled again (and the image linked again).
 * BPS_HW_ID comes from the environment's build_flags in platformio.ini.
 */
#include <stdbool.h>

#include "ota.h"

#ifndef BPS_HW_ID
#define BPS_HW_ID "unknown"
#endif
#ifndef BPS_GIT_DESCRIBE
#define BPS_GIT_DESCRIBE "unknown"
#endif
#ifndef BPS_BUILD_TAG
#define BPS_BUILD_TAG ""
#define BPS_BUILD_TAG_JSON "null"
#endif
#ifndef BPS_BUILD_COMMIT
#define BPS_BUILD_COMMIT "unknown"
#endif
#ifndef BPS_BUILD_DIRTY
#define BPS_BUILD_DIRTY 0
#endif
#ifndef BPS_BUILD_TIME
#define BPS_BUILD_TIME "1970-01-01T00:00:00Z"
#endif
#ifndef BPS_APP_VERSION
#define BPS_APP_VERSION "unknown"
#endif

#if BPS_BUILD_DIRTY
#define BPS_BUILD_DIRTY_JSON "true"
#else
#define BPS_BUILD_DIRTY_JSON "false"
#endif

/* The only place the marker is written: the image holds it exactly once.
 * Referenced by bps_build below (and so kept by the linker's garbage
 * collection), and `used` against the compiler dropping it. */
__attribute__((used)) const char bps_build_info[] =
    "BPS_BUILD_INFO:{\"hw\":\"" BPS_HW_ID "\",\"tag\":" BPS_BUILD_TAG_JSON
    ",\"commit\":\"" BPS_BUILD_COMMIT "\",\"dirty\":" BPS_BUILD_DIRTY_JSON
    ",\"describe\":\"" BPS_GIT_DESCRIBE "\",\"built\":\"" BPS_BUILD_TIME "\"}";

/* The marker's length: what follows it is the JSON object. */
#define MARKER_LEN 15

const bps_build bps_build_this = {
    .hardware_id = BPS_HW_ID,
    .tag = BPS_BUILD_TAG,
    .commit = BPS_BUILD_COMMIT,
    .dirty = BPS_BUILD_DIRTY,
    .describe = BPS_GIT_DESCRIBE,
    .built_at = BPS_BUILD_TIME,
    .info = bps_build_info + MARKER_LEN,
};

#if defined(ARDUINO_ARCH_ESP32)
/* The image's app description (the bootloader's and esptool's "App
 * version"), with the describe string as its version. The prebuilt ESP-IDF
 * libraries define it weakly, with the library build's version; the other
 * fields are as they set them. */
#include <esp_app_desc.h>
#include <sdkconfig.h>

#ifndef IDF_VER
#define IDF_VER ""
#endif

const __attribute__((section(".rodata_desc"))) esp_app_desc_t esp_app_desc = {
    .magic_word = ESP_APP_DESC_MAGIC_WORD,
#ifdef CONFIG_BOOTLOADER_APP_SECURE_VERSION
    .secure_version = CONFIG_BOOTLOADER_APP_SECURE_VERSION,
#else
    .secure_version = 0,
#endif
    .version = BPS_APP_VERSION,
    .project_name = "bps-mcu",
#ifdef CONFIG_APP_COMPILE_TIME_DATE
    .time = __TIME__,
    .date = __DATE__,
#endif
    .idf_ver = IDF_VER,
#ifdef CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL
    .min_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MIN_FULL,
#endif
#ifdef CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL
    .max_efuse_blk_rev_full = CONFIG_ESP_EFUSE_BLOCK_REV_MAX_FULL,
#endif
#ifdef CONFIG_MMU_PAGE_SIZE
    .mmu_page_size = 31 - __builtin_clz(CONFIG_MMU_PAGE_SIZE),
#endif
};
#endif
