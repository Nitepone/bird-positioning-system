#!/usr/bin/env bash
# Settings are read and written indirectly (${!k}, printf -v):
# shellcheck disable=SC2034
# Edits a bps microcontroller client's settings (bps.conf by default). A menu
# lists every setting with its current value and says whether the whole is
# ready to use; choose a setting to change it, then save (and build with
# ./build.sh).
#
#   ./configure.sh              edit bps.conf
#   ./configure.sh north.conf   edit another board's settings
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
conf=$(realpath -m "${1:-$here/bps.conf}")
cfgpy=(python3 "$here/scripts/bps_config.py")

if command -v whiptail >/dev/null; then
    ui=whiptail ok_flag=--ok-button cancel_flag=--cancel-button yes_flag=--yes-button no_flag=--no-button
elif command -v dialog >/dev/null; then
    ui=dialog ok_flag=--ok-label cancel_flag=--cancel-label yes_flag=--yes-label no_flag=--no-label
else
    echo "configure.sh needs whiptail or dialog (Debian/Ubuntu: sudo apt install whiptail)" >&2
    exit 1
fi
backtitle="bps (bird positioning system) - microcontroller client"

keys=(BOARD WIFI_SSID WIFI_PASSWORD SERVER_HOST SERVER_PORT CLIENT_NAME I2S_BCLK I2S_WS I2S_DATA
    MIC_CHANNEL BUTTON_PIN LED_PIN LOOKBACK_MS RING_KB DEVICE_NAME CLIENT_ID)
declare -A label=(
    [BOARD]="Board" [WIFI_SSID]="Wi-Fi network" [WIFI_PASSWORD]="Wi-Fi password"
    [SERVER_HOST]="Server address" [SERVER_PORT]="Server port" [CLIENT_NAME]="Client name"
    [I2S_BCLK]="Mic BCLK pin" [I2S_WS]="Mic LRCL pin" [I2S_DATA]="Mic DOUT pin"
    [MIC_CHANNEL]="Mic channel" [BUTTON_PIN]="Button pin" [LED_PIN]="Status LED"
    [LOOKBACK_MS]="Look-back" [RING_KB]="Ring buffer" [DEVICE_NAME]="Mic name"
    [CLIENT_ID]="Client ID"
)
declare -A board_title=()
board_keys=()
while IFS=$'\t' read -r key title; do
    board_keys+=("$key")
    board_title[$key]=$title
done < <("${cfgpy[@]}" boards)

# Runs a widget and prints the answer; fails on Cancel or Esc.
ask() { "$ui" --backtitle "$backtitle" "$@" 3>&1 1>&2 2>&3; }
msg() { "$ui" --backtitle "$backtitle" --title "$1" --msgbox "$2" 16 74 || true; }

pico() { [[ $BOARD == pico* ]]; }

# Current settings: the file's, else a new Pico W config.
for k in "${keys[@]}"; do declare "$k="; done
if [[ -f $conf ]]; then
    eval "$("${cfgpy[@]}" shell "$conf")"
fi
[[ -n $BOARD ]] || BOARD=pico_w

# Empty settings that have a default (for this board) take it.
fill_defaults() {
    eval "$("${cfgpy[@]}" defaults "$BOARD")"
    local k def
    for k in "${keys[@]}"; do
        def=DEF_$k
        if [[ -z ${!k} && -n ${!def:-} ]]; then
            printf -v "$k" '%s' "${!def}"
        fi
    done
}

# Pins and memory follow the board.
use_board_defaults() {
    eval "$("${cfgpy[@]}" defaults "$BOARD")"
    I2S_BCLK=$DEF_I2S_BCLK I2S_WS=$DEF_I2S_WS I2S_DATA=$DEF_I2S_DATA
    BUTTON_PIN=$DEF_BUTTON_PIN LED_PIN=$DEF_LED_PIN
    LOOKBACK_MS=$DEF_LOOKBACK_MS RING_KB=$DEF_RING_KB
}

fill_defaults

state() {
    local k
    for k in "${keys[@]}"; do printf '%s=%s\n' "$k" "${!k}"; done
}
saved_state=""
[[ -f $conf ]] && saved_state=$(state)

# Runs bps_config.py with the current settings in its environment.
with_settings() {
    (
        for k in "${keys[@]}"; do export "BPS_SET_$k=${!k}"; done
        "${cfgpy[@]}" "$@"
    )
}

pin_text() {
    case $1 in
        none) echo "none" ;;
        builtin) echo "onboard LED" ;;
        "") echo "(not set)" ;;
        *) if pico; then echo "GP$1"; else echo "GPIO $1"; fi ;;
    esac
}

# How a setting is shown in the menu.
show() {
    local v=${!1}
    case $1 in
        BOARD) echo "${board_title[$v]:-$v}" ;;
        WIFI_PASSWORD) if [[ -z $v ]]; then echo "(none: open network)"; else echo "set (${#v} characters)"; fi ;;
        CLIENT_NAME) echo "${v:-(none)}" ;;
        CLIENT_ID) echo "${v:-(derived from the chip)}" ;;
        I2S_BCLK | I2S_DATA | BUTTON_PIN | LED_PIN) pin_text "$v" ;;
        I2S_WS) if pico; then echo "$(pin_text "$v") (always BCLK + 1)"; else pin_text "$v"; fi ;;
        MIC_CHANNEL) if [[ $v == right ]]; then echo "right (SEL to 3V)"; else echo "left (SEL to GND or unconnected)"; fi ;;
        LOOKBACK_MS) echo "${v:-?} ms (at most $("${cfgpy[@]}" max-lookback "${RING_KB:-0}" 2>/dev/null || echo ?) ms)" ;;
        RING_KB) echo "${v:-?} KB" ;;
        *) echo "${v:-(not set)}" ;;
    esac
}

input() { # KEY TITLE TEXT
    local v
    if v=$(ask --title "$2" --inputbox "$3" 14 74 "${!1}"); then
        printf -v "$1" '%s' "$v"
    fi
}

edit() {
    local v
    case $1 in
        BOARD)
            local items=() b old=$BOARD
            for b in "${board_keys[@]}"; do items+=("$b" "${board_title[$b]}"); done
            v=$(ask --title "Board" --default-item "$BOARD" \
                --menu "Which board is this client?" 14 74 5 "${items[@]}") || return 0
            BOARD=$v
            if [[ $BOARD != "$old" ]]; then
                eval "$("${cfgpy[@]}" defaults "$BOARD")"
                if ask --title "Board" $yes_flag "Use defaults" $no_flag "Keep mine" --yesno \
                    "Switch the pins and memory settings to the ${board_title[$BOARD]}'s defaults?\n\nBCLK $DEF_I2S_BCLK, LRCL $DEF_I2S_WS, DOUT $DEF_I2S_DATA, button $DEF_BUTTON_PIN, LED $DEF_LED_PIN,\nring buffer $DEF_RING_KB KB, look-back $DEF_LOOKBACK_MS ms" \
                    14 74; then
                    use_board_defaults
                elif pico && [[ $I2S_BCLK =~ ^[0-9]+$ ]]; then
                    I2S_WS=$((I2S_BCLK + 1))
                fi
            fi
            ;;
        WIFI_SSID) input WIFI_SSID "Wi-Fi" "Network name (SSID). The Pico W and ESP32 only use 2.4 GHz networks:" ;;
        WIFI_PASSWORD)
            v=$(ask --title "Wi-Fi" --passwordbox \
                "Password for \"$WIFI_SSID\" (empty for an open network). It is compiled into the firmware:" \
                12 74 "$WIFI_PASSWORD") && WIFI_PASSWORD=$v
            ;;
        SERVER_HOST) input SERVER_HOST "Server" "IP address or host name of the bps server:" ;;
        SERVER_PORT) input SERVER_PORT "Server" "The server's client port: 2473 unless changed in bsp-server.toml (not the web UI port):" ;;
        CLIENT_NAME) input CLIENT_NAME "Client name" "Name to suggest for this client, e.g. \"North fence\" (optional; a name set in the web UI takes precedence):" ;;
        I2S_BCLK)
            if pico; then
                input I2S_BCLK "Microphone" "GPIO for BCLK. LRCL (word select) is wired to the next GPIO up:"
                [[ $I2S_BCLK =~ ^[0-9]+$ ]] && I2S_WS=$((I2S_BCLK + 1))
            else
                input I2S_BCLK "Microphone" "GPIO for BCLK:"
            fi
            ;;
        I2S_WS)
            if pico; then
                msg "Microphone" "On a Pico, LRCL (word select) must be on the GPIO after BCLK: the PIO program drives them together.\n\nChange the BCLK pin to move both."
            else
                input I2S_WS "Microphone" "GPIO for LRCL (word select):"
            fi
            ;;
        I2S_DATA) input I2S_DATA "Microphone" "GPIO for DOUT:" ;;
        MIC_CHANNEL)
            v=$(ask --title "Microphone" --default-item "${MIC_CHANNEL:-left}" --menu \
                "Which channel does the mic use? Its SEL pin decides. (The board's log shows both channels' levels at startup.)" \
                13 74 2 left "SEL to GND or unconnected" right "SEL to 3V") && MIC_CHANNEL=$v
            ;;
        BUTTON_PIN) input BUTTON_PIN "Positioning button" "GPIO of a push button to GND that requests positioning (as Enter does on bsp-client), or \"none\":" ;;
        LED_PIN)
            if pico; then
                input LED_PIN "Status LED" "GPIO of a status LED, \"builtin\" for the onboard LED, or \"none\":"
            else
                input LED_PIN "Status LED" "GPIO of a status LED (2 on most ESP32 dev boards), or \"none\":"
            fi
            ;;
        LOOKBACK_MS) input LOOKBACK_MS "Look-back" "How much audio from before the sound that opened the noise gate each upload starts with, in ms. With a ${RING_KB} KB ring buffer, at most $("${cfgpy[@]}" max-lookback "${RING_KB:-0}" 2>/dev/null || echo "?") ms:" ;;
        RING_KB) input RING_KB "Ring buffer" "RAM asked for recent audio, in KB (used in 16 KB blocks; 94 KB holds a second). The board takes what fits while keeping 32 KB free, and logs what it got:" ;;
        DEVICE_NAME) input DEVICE_NAME "Mic name" "Reported to the server as the microphone's name:" ;;
        CLIENT_ID) input CLIENT_ID "Client ID" "UUID to use as this client's ID, or empty to derive a stable one from the chip:" ;;
    esac
    return 0 # a cancelled dialog just leaves the setting as it was
}

save() {
    local errors
    if ! errors=$(with_settings check-env); then
        msg "Not saved: please fix" "$errors"
        return 1
    fi
    with_settings write "$conf" >/dev/null
    saved_state=$(state)
}

build_menu() {
    local choice
    choice=$(ask --title "Build" --menu "Saved to $conf.\n\nConnect the board by USB first to flash it. A Pico flashed for the first time must be held in BOOTSEL mode (hold the button while plugging it in)." \
        16 74 4 build "Build only" flash "Build and flash" monitor "Build, flash and show the board's log" ) || return 0
    clear
    case $choice in
        build) exec "$here/build.sh" "$conf" ;;
        flash) exec "$here/build.sh" --flash "$conf" ;;
        monitor) exec "$here/build.sh" --flash --monitor "$conf" ;;
    esac
}

rows=$( (stty size </dev/tty) 2>/dev/null | cut -d' ' -f1)
rows=${rows:-24}
height=$((rows - 2 > 34 ? 34 : rows - 2))
list=$((height - 10))
current=${label[BOARD]}

while true; do
    status=""
    [[ $(state) != "$saved_state" ]] && status=" (unsaved changes)"
    if problems=$(with_settings check-env); then
        check="Ready to save."
    else
        n=$(printf '%s\n' "$problems" | wc -l)
        plural=""
        if ((n > 1)); then plural=s; fi
        first=$(printf '%s\n' "$problems" | head -1)
        check="$n problem$plural to fix before saving, e.g.: ${first%% (*}"
    fi
    items=()
    for k in "${keys[@]}"; do items+=("${label[$k]}" "$(show "$k")"); done
    items+=("──────────────" "" "Save" "Write the config file" "Save & build" "Save, then build or flash it" "Quit" "Leave")
    choice=$(ask --title "$(basename "$conf")$status" $ok_flag "Select" $cancel_flag "Quit" \
        --default-item "$current" --menu "$conf\n$check" "$height" 78 "$list" "${items[@]}") || choice=Quit
    current=$choice
    case $choice in
        Save) save || true ;;
        "Save & build") save && build_menu ;;
        Quit)
            if [[ $(state) == "$saved_state" ]] ||
                ask --title "Quit" --defaultno --yesno "Discard the unsaved changes?" 8 50; then
                clear
                exit 0
            fi
            ;;
        *)
            for k in "${keys[@]}"; do
                if [[ ${label[$k]} == "$choice" ]]; then edit "$k"; fi
            done
            ;;
    esac
done
