#!/usr/bin/env bash
# Builds the bps microcontroller client for the board in a config file
# (written by ./configure.sh), and optionally flashes it and shows its log.
# Uses PlatformIO if it is installed, otherwise the bps-firmware Docker image
# (built from ./Dockerfile on first use).
#
#   ./build.sh                    build for bps.conf
#   ./build.sh -f                 build and flash
#   ./build.sh -f -m north.conf   build and flash north.conf's board, then show its log
#   ./build.sh -m                 just show the log of a connected board
#   ./build.sh -t                 run the unit tests (no board or config needed)
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
cfgpy=(python3 "$here/scripts/bps_config.py")
image=${BPS_IMAGE:-bps-firmware}

usage() {
    cat <<EOF
Usage: ./build.sh [options] [CONFIG]

Builds the firmware for the board named in CONFIG (default: bps.conf).

  -f, --flash       flash it to the board connected by USB
  -m, --monitor     show the board's log (serial monitor; Ctrl-C to leave)
  -p, --port PORT   serial port for flashing / the log (default: detected)
  -c, --clean       rebuild from scratch
  -t, --test        run the core's unit tests on this computer instead
      --docker      build in the Docker image even if PlatformIO is installed
      --local       use the installed PlatformIO
  -h, --help        show this help
EOF
}

flash=0 monitor=0 clean=0 test=0 port="" runner="" conf=""
while (($#)); do
    case $1 in
        -f | --flash) flash=1 ;;
        -m | --monitor) monitor=1 ;;
        -c | --clean) clean=1 ;;
        -t | --test) test=1 ;;
        -p | --port)
            [[ $# -ge 2 ]] || { usage >&2; exit 2; }
            port=$2
            shift
            ;;
        --docker) runner=docker ;;
        --local) runner=local ;;
        -h | --help) usage; exit 0 ;;
        -*) echo "unknown option: $1" >&2; usage >&2; exit 2 ;;
        *) conf=$1 ;;
    esac
    shift
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { printf 'build.sh: %s\n' "$*" >&2; exit 1; }

# ---- How to run PlatformIO ----

if [[ -z $runner ]]; then
    if command -v pio >/dev/null; then
        runner=local
    elif command -v docker >/dev/null; then
        runner=docker
    else
        die "needs PlatformIO (pipx install platformio) or Docker"
    fi
fi
[[ $runner == local ]] && ! command -v pio >/dev/null && die "PlatformIO is not installed (pipx install platformio), or use --docker"

docker_args=()
if [[ $runner == docker ]]; then
    command -v docker >/dev/null || die "Docker is not installed"
    if ! docker image inspect "$image" >/dev/null 2>&1; then
        say "Building the $image Docker image (once; it downloads every board's toolchain)"
        docker build -t "$image" "$here"
    fi
    docker_args=(--rm -v "$here:/firmware")
    [[ -t 0 && -t 1 ]] && docker_args+=(-it)
fi

# Runs a pio command, locally or in the image; extra `docker run` arguments
# (devices, environment) come before "--".
pio_run() {
    local extra=()
    while [[ $1 != -- ]]; do extra+=("$1"); shift; done
    shift
    if [[ $runner == local ]]; then
        local env=()
        local a
        for a in "${extra[@]}"; do [[ $a == BPS_CONF=* ]] && env+=("$a"); done
        (cd "$here" && env "${env[@]}" pio "$@")
    else
        local args=()
        local a
        for a in "${extra[@]}"; do
            case $a in
                BPS_CONF=*) args+=(-e "$a") ;;
                *) args+=("$a") ;;
            esac
        done
        docker run "${docker_args[@]}" "${args[@]}" "$image" pio "$@"
    fi
}

if ((test)); then
    say "Running the unit tests ($runner)"
    pio_run -- test -e native
    exit
fi

# ---- The config ----

conf=$(realpath -m "${conf:-$here/bps.conf}")
[[ -f $conf ]] || die "$conf not found; run ./configure.sh first"
if ! problems=$("${cfgpy[@]}" check "$conf"); then
    die "$conf has problems (fix them with ./configure.sh $(basename "$conf")):"$'\n'"$problems"
fi
BOARD=$("${cfgpy[@]}" shell "$conf" | sed -n 's/^BOARD=//p')
case $BOARD in
    pico*) artifact=firmware.uf2 ;;
    *) artifact=firmware.bin ;;
esac
out="$here/.pio/build/$BOARD/$artifact"

# The config as the build sees it: in the container, the project is /firmware.
conf_env=BPS_CONF=$conf
conf_mount=()
if [[ $runner == docker ]]; then
    if [[ $conf == "$here"/* ]]; then
        conf_env=BPS_CONF=/firmware/${conf#"$here"/}
    else
        conf_mount=(-v "$(dirname "$conf"):/conf:ro")
        conf_env=BPS_CONF=/conf/$(basename "$conf")
    fi
fi

# ---- Build ----

if ((clean)); then
    say "Cleaning $BOARD"
    pio_run "${conf_mount[@]}" "$conf_env" -- run -e "$BOARD" -t clean
fi
if ((flash || !monitor)); then
    say "Building $BOARD from $(basename "$conf") ($runner)"
    pio_run "${conf_mount[@]}" "$conf_env" -- run -e "$BOARD"
    say "Built $out ($(du -h "$out" | cut -f1))"
fi

# ---- Flash and monitor ----

# The first serial port that shows up within 10 s (a Pico re-appears after flashing).
find_port() {
    [[ -n $port ]] && { echo "$port"; return; }
    local i p
    for ((i = 0; i < 20; i++)); do
        for p in /dev/ttyACM* /dev/ttyUSB*; do
            [[ -e $p ]] && { echo "$p"; return; }
        done
        sleep 0.5
    done
    return 1
}

# A Pico in BOOTSEL mode shows up as a USB drive.
find_bootsel_drive() {
    local d
    for d in /media/"$USER"/{RPI-RP2,RP2350} /run/media/"$USER"/{RPI-RP2,RP2350}; do
        [[ -d $d ]] && { echo "$d"; return; }
    done
    return 1
}

flash_pico_from_host() {
    local drive
    if command -v picotool >/dev/null && picotool load -v -x -f "$out"; then
        return 0
    fi
    if drive=$(find_bootsel_drive); then
        say "Copying to $drive"
        cp "$out" "$drive/" && sync
        return 0
    fi
    die "could not flash: hold BOOTSEL while plugging the Pico in, then copy $out to the drive that appears"
}

if ((flash)); then
    say "Flashing $BOARD"
    if [[ $runner == local ]]; then
        pio_run "$conf_env" -- run -e "$BOARD" -t upload ${port:+--upload-port "$port"}
    elif [[ $BOARD == pico* ]]; then
        # USB access from a container is awkward; the UF2 is on this side anyway.
        flash_pico_from_host
    else
        p=$(find_port) || die "no serial port found: connect the ESP32 by USB, or pass -p PORT"
        pio_run --device "$p" "${conf_mount[@]}" "$conf_env" -- run -e "$BOARD" -t upload --upload-port "$p"
    fi
fi

if ((monitor)); then
    [[ -t 0 ]] || die "the log needs a terminal"
    say "Showing the board's log (Ctrl-C to leave)"
    if [[ $runner == local ]]; then
        pio_run -- device monitor -b 115200 ${port:+-p "$port"}
    else
        p=$(find_port) || die "no serial port found: is the board connected by USB?"
        pio_run --device "$p" -- device monitor -b 115200 -p "$p"
    fi
fi
