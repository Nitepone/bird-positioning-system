# Build environment for the bps microcontroller client firmware: PlatformIO with
# the Pico W, Pico 2 W and ESP32 toolchains installed, so builds work offline.
# ./build.sh builds and uses this image when PlatformIO is not installed (or
# with --docker); it can also be used directly:
#
#   docker build -t bps-firmware firmware
#   docker run --rm -it -v "$PWD/firmware:/firmware" bps-firmware ./configure.sh
#   docker run --rm -v "$PWD/firmware:/firmware" -e BPS_CONF=north.conf bps-firmware pio run -e pico_w
#
# The firmware lands in firmware/.pio/build/<board>/.
#
# Commands run as root (see scripts/docker-entrypoint.sh); what they create in
# firmware/ is handed back to the directory's owner afterwards.

FROM python:3.13-slim-trixie

# git: the Raspberry Pi platform is fetched from git. gcc: native unit tests.
# whiptail: ./configure.sh.
RUN apt-get update \
    && apt-get install -y --no-install-recommends git gcc libc6-dev whiptail \
    && rm -rf /var/lib/apt/lists/*

RUN pip install --no-cache-dir platformio==6.2.0

ENV PLATFORMIO_CORE_DIR=/opt/platformio \
    PLATFORMIO_SETTING_ENABLE_TELEMETRY=no \
    PLATFORMIO_SETTING_CHECK_PLATFORMIO_INTERVAL=36500

# Platforms, toolchains and frameworks for every board (several GB), so builds
# need no downloads. Some of it only happens on a first build (the ESP32
# platform's Python tools, the Unity test library), so build a stub sketch
# for every board once. Only platformio.ini and the config script are copied:
# this layer stays cached while the sources change. Libraries go to a shared
# directory, so the mounted project finds Unity there.
ENV PLATFORMIO_LIBDEPS_DIR=/opt/platformio/libdeps
WORKDIR /opt/bps-firmware
COPY platformio.ini ./
COPY scripts/bps_config.py scripts/
RUN pio pkg install -e pico_w -e pico2_w -e esp32 \
    && mkdir -p src test/test_stub \
    && printf '#include <Arduino.h>\nvoid setup() {}\nvoid loop() {}\n' > src/main.cpp \
    && printf '#include <unity.h>\nvoid setUp(void) {}\nvoid tearDown(void) {}\nint main(void) { UNITY_BEGIN(); return UNITY_END(); }\n' > test/test_stub/test_main.c \
    && for board in pico_w pico2_w esp32; do \
        BPS_SET_BOARD=$board BPS_SET_WIFI_SSID=stub BPS_SET_SERVER_HOST=127.0.0.1 \
            python3 scripts/bps_config.py write /tmp/stub.conf \
        && BPS_CONF=/tmp/stub.conf pio run -e $board || exit 1; \
    done \
    && pio test -e native \
    && rm -rf /opt/bps-firmware /tmp/stub.conf

COPY scripts/docker-entrypoint.sh /usr/local/bin/
WORKDIR /firmware
ENTRYPOINT ["docker-entrypoint.sh"]
CMD ["pio", "run"]
