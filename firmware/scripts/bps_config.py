"""bps.conf: board defaults, validation, and turning it into build defines.

Used two ways:
- as a PlatformIO pre-build script (`extra_scripts`): reads $BPS_CONF
  (default firmware/bps.conf) and adds its settings as BPS_* defines;
- from the command line, by ./configure.sh:
    bps_config.py boards               board keys and names
    bps_config.py defaults BOARD       default settings for a board (shell)
    bps_config.py shell FILE           a config file's settings (shell)
    bps_config.py write FILE           write BPS_SET_* environment variables to FILE
    bps_config.py check FILE           validate FILE
    bps_config.py check-env            validate BPS_SET_* environment variables
    bps_config.py max-lookback RING_KB the longest look-back a ring buffer allows
    bps_config.py drivers              device kinds for DEVICES: kind, bus, what it is
"""

import os
import re
import shlex
import sys

SAMPLE_RATE = 48000

PICO_PINS = set(range(0, 23)) | {26, 27, 28}  # GP23-25 and GP29 belong to the radio
ESP32_OUT_PINS = {0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}
ESP32_IN_PINS = ESP32_OUT_PINS | {34, 35, 36, 39}
# XIAO ESP32S3: the header's D0-D10, plus the BOOT button (GPIO 0) and the user LED (GPIO 21).
XIAO_S3_PINS = {1, 2, 3, 4, 5, 6, 7, 8, 9, 43, 44}
XIAO_S3_PLUS_PINS = XIAO_S3_PINS | {10, 11, 12, 13, 38, 39, 40, 41, 42}  # D11-D19 underneath
# The bps XIAO board (bps-xaio.kicad_sch): a XIAO ESP32S3 in its socket, with an
# IM73D122 PDM microphone wired to D2 (clock, GPIO 3) and D3 (data, GPIO 4).
# Every other header pin is taken (GPS, I2C, UART, microSD), so the button and
# LED can only be the XIAO's own.
BPS_XIAO_PDM_CLK = 3
BPS_XIAO_PDM_DATA = 4

# Drivers for devices on the I2C and SPI buses (firmware/lib/bps_core/src/
# devices.c): their bus and usual I2C address. DEVICES lists a board's as
# "kind@address" (I2C) or "kind@chip-select-pin" (SPI).
DRIVERS = {
    "max17048": {"bus": "i2c", "address": 0x36, "about": "battery fuel gauge"},
    "sd": {"bus": "spi", "address": None, "about": "microSD card (keeps a copy of recordings)"},
}
BUS_KEYS = {"i2c": ["I2C_SDA", "I2C_SCL"], "spi": ["SPI_SCK", "SPI_MISO", "SPI_MOSI"]}

BOARDS = {
    "pico_w": {
        "title": "Raspberry Pi Pico W (RP2040)",
        "short": "picow",
        "family": "rp2",
        "out_pins": PICO_PINS, "in_pins": PICO_PINS, "builtin_led": True,
        "defaults": {"I2S_BCLK": "18", "I2S_DATA": "20", "LED_PIN": "builtin",
                     "BUTTON_PIN": "none", "RING_KB": "160", "LOOKBACK_MS": "750"},
    },
    "pico2_w": {
        "title": "Raspberry Pi Pico 2 W (RP2350)",
        "short": "pico2w",
        "family": "rp2",
        "out_pins": PICO_PINS, "in_pins": PICO_PINS, "builtin_led": True,
        "defaults": {"I2S_BCLK": "18", "I2S_DATA": "20", "LED_PIN": "builtin",
                     "BUTTON_PIN": "none", "RING_KB": "400", "LOOKBACK_MS": "2500"},
    },
    "esp32": {
        "title": "ESP32 (classic, e.g. ESP32-DevKitC)",
        "short": "esp32",
        "family": "esp32",
        "out_pins": ESP32_OUT_PINS, "in_pins": ESP32_IN_PINS, "builtin_led": False,
        "defaults": {"I2S_BCLK": "26", "I2S_WS": "25", "I2S_DATA": "33", "LED_PIN": "2",
                     "BUTTON_PIN": "0", "RING_KB": "192", "LOOKBACK_MS": "1000"},
    },
    "xiao_esp32s3": {
        "title": "Seeed XIAO ESP32S3 (8 MB PSRAM)",
        "short": "xiao-esp32s3",
        "family": "esp32",
        "out_pins": XIAO_S3_PINS, "in_pins": XIAO_S3_PINS | {0}, "builtin_led": True,
        "defaults": {"I2S_BCLK": "7", "I2S_WS": "8", "I2S_DATA": "9", "LED_PIN": "builtin",
                     "BUTTON_PIN": "0", "RING_KB": "1024", "LOOKBACK_MS": "2500"},
    },
    "xiao_esp32s3_plus": {
        "title": "Seeed XIAO ESP32S3 Plus (8 MB PSRAM)",
        "short": "xiao-esp32s3-plus",
        "family": "esp32",
        "out_pins": XIAO_S3_PLUS_PINS, "in_pins": XIAO_S3_PLUS_PINS | {0}, "builtin_led": True,
        "defaults": {"I2S_BCLK": "7", "I2S_WS": "8", "I2S_DATA": "9", "LED_PIN": "builtin",
                     "BUTTON_PIN": "0", "RING_KB": "1024", "LOOKBACK_MS": "2500"},
    },
    "bps_xiao": {
        "title": "bps XIAO board (XIAO ESP32S3, PDM mic)",
        "short": "bps-xiao",
        "family": "esp32",
        "mic": "pdm",
        # The mic's pins are fixed by the board (see `complete`).
        # The mic's and buses' pins are wired on the board (see `complete`): I2C
        # on D4/D5 (the battery gauge and J2), SPI on D8-D10 (the microSD
        # card, chip select D1). D0 goes to the GPS connector.
        "fixed": {"I2S_BCLK": str(BPS_XIAO_PDM_CLK), "I2S_DATA": str(BPS_XIAO_PDM_DATA),
                  "I2S_WS": "none", "I2C_SDA": "5", "I2C_SCL": "6",
                  "SPI_SCK": "7", "SPI_MISO": "8", "SPI_MOSI": "9"},
        "out_pins": XIAO_S3_PINS, "in_pins": XIAO_S3_PINS | {0}, "builtin_led": True,
        "defaults": {"LED_PIN": "builtin", "BUTTON_PIN": "0", "RING_KB": "1024",
                     "LOOKBACK_MS": "2500", "DEVICE_NAME": "IM73D122",
                     "DEVICES": "max17048@0x36 sd@2"},
    },
}

COMMON_DEFAULTS = {
    "SERVER_PORT": "2473",
    "CLIENT_NAME": "",
    "CLIENT_ID": "",
    "DEVICE_NAME": "ICS-43434",
    "MIC_CHANNEL": "left",
    "I2C_SDA": "none",
    "I2C_SCL": "none",
    "SPI_SCK": "none",
    "SPI_MISO": "none",
    "SPI_MOSI": "none",
    "DEVICES": "",
}

KEYS = ["BOARD", "WIFI_SSID", "WIFI_PASSWORD", "SERVER_HOST", "SERVER_PORT", "CLIENT_NAME",
        "I2S_BCLK", "I2S_WS", "I2S_DATA", "MIC_CHANNEL", "BUTTON_PIN", "LED_PIN",
        "I2C_SDA", "I2C_SCL", "SPI_SCK", "SPI_MISO", "SPI_MOSI", "DEVICES",
        "LOOKBACK_MS", "RING_KB", "DEVICE_NAME", "CLIENT_ID"]

UUID_RE = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")


def defaults(board):
    d = dict(COMMON_DEFAULTS)
    d.update(BOARDS[board]["defaults"])
    d.update(BOARDS[board].get("fixed", {}))
    if BOARDS[board]["family"] == "rp2":
        d["I2S_WS"] = str(int(d["I2S_BCLK"]) + 1)
    return d


def parse(text):
    cfg = {}
    for n, line in enumerate(text.splitlines(), 1):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        words = shlex.split(line, comments=True)
        if len(words) != 1 or "=" not in words[0]:
            raise ValueError(f"line {n}: expected KEY=value")
        key, value = words[0].split("=", 1)
        cfg[key] = value
    return cfg


def load(path):
    with open(path, encoding="utf-8") as f:
        return parse(f.read())


def complete(cfg):
    """`cfg` with board and common defaults filled in."""
    board = cfg.get("BOARD", "")
    full = defaults(board) if board in BOARDS else dict(COMMON_DEFAULTS)
    full.update({k: v for k, v in cfg.items() if v != "" or k not in full})
    if BOARDS.get(board, {}).get("family") == "rp2" and full.get("I2S_BCLK", "").isdigit():
        full["I2S_WS"] = str(int(full["I2S_BCLK"]) + 1)  # fixed by the PIO program
    full.update(BOARDS.get(board, {}).get("fixed", {}))  # wired on the board
    return full


def is_pdm(board):
    return BOARDS.get(board, {}).get("mic") == "pdm"


def parse_devices(text):
    """[(kind, address)] from "kind@address ..." (or "none"), or raises ValueError."""
    out = []
    if text.strip() == "none":
        return out
    for item in re.split(r"[\s,]+", text.strip()):
        if not item:
            continue
        kind, _, addr = item.partition("@")
        if kind not in DRIVERS:
            raise ValueError(f"unknown device {kind!r} (known: {', '.join(DRIVERS)})")
        if addr:
            try:
                address = int(addr, 0)
            except ValueError:
                raise ValueError(f"{kind}: {addr!r} is not a number") from None
        elif DRIVERS[kind]["address"] is not None:
            address = DRIVERS[kind]["address"]
        else:
            raise ValueError(f"{kind} needs its chip select pin ({kind}@PIN)")
        out.append((kind, address))
    return out


def format_devices(devices):
    return " ".join(f"{k}@0x{a:02x}" if DRIVERS[k]["bus"] == "i2c" else f"{k}@{a}"
                    for k, a in devices)


# The firmware allocates the ring in 16 KB blocks, and keeps this much of it
# clear of the look-back: 400 ms of slack, one gate frame and a 4096-sample guard.
# It also leaves 32 KB of heap free, so a board may get less ring than RING_KB
# asks for (and then logs the look-back it could keep).
RING_BLOCK_KB = 16
RING_RESERVED_MS = 600


def ring_ms(ring_kb):
    return ring_kb // RING_BLOCK_KB * RING_BLOCK_KB * 1024 // 2 * 1000 // SAMPLE_RATE


def max_lookback_ms(ring_kb):
    return max(0, ring_ms(ring_kb) - RING_RESERVED_MS)


def validate(cfg):
    """Errors in a completed config (empty when it is fine)."""
    errors = []
    unknown = sorted(set(cfg) - set(KEYS))
    if unknown:
        errors.append("unknown settings: " + ", ".join(unknown))
    board = cfg.get("BOARD", "")
    if board not in BOARDS:
        errors.append(f"BOARD must be one of {', '.join(BOARDS)}")
        return errors
    b = BOARDS[board]

    ssid = cfg.get("WIFI_SSID", "")
    if not ssid or len(ssid.encode()) > 32:
        errors.append("WIFI_SSID must be 1-32 bytes")
    pw = cfg.get("WIFI_PASSWORD", "")
    if pw and not 8 <= len(pw) <= 63:
        errors.append("WIFI_PASSWORD must be empty (open network) or 8-63 characters")
    if not cfg.get("SERVER_HOST"):
        errors.append("SERVER_HOST is required (the server's IP address or host name)")
    if not cfg.get("SERVER_PORT", "").isdigit() or not 1 <= int(cfg["SERVER_PORT"]) <= 65535:
        errors.append("SERVER_PORT must be 1-65535 (the server's client port, 2473 by default)")
    if len(cfg.get("CLIENT_NAME", "")) > 64:
        errors.append("CLIENT_NAME must be at most 64 characters")
    if cfg.get("CLIENT_ID") and not UUID_RE.match(cfg["CLIENT_ID"]):
        errors.append("CLIENT_ID must be a UUID, or empty to derive one from the chip")
    if cfg.get("MIC_CHANNEL") not in ("left", "right"):
        errors.append("MIC_CHANNEL must be left or right")

    def pin(key, allowed, extra=()):
        v = cfg.get(key, "")
        if v in extra:
            return v
        if not v.isdigit() or int(v) not in allowed:
            errors.append(f"{key} = {v!r} is not a usable pin on {board}"
                          + (f" (or {'/'.join(extra)})" if extra else ""))
            return None
        return int(v)

    out_pins, in_pins = b["out_pins"], b["in_pins"]
    # A PDM mic has only a clock and data; I2S_BCLK is its clock.
    used = [pin("I2S_BCLK", out_pins), pin("I2S_DATA", in_pins)]
    if not is_pdm(board):
        used.append(pin("I2S_WS", out_pins))
    button = pin("BUTTON_PIN", in_pins, ("none",))
    led = pin("LED_PIN", out_pins, ("none", "builtin") if b["builtin_led"] else ("none",))
    used += [p for p in (button, led) if isinstance(p, int)]

    # Peripheral buses: all of a bus's pins, or none.
    bus_up = {}
    for bus, keys in BUS_KEYS.items():
        if all(cfg.get(k, "none") == "none" for k in keys):
            bus_up[bus] = False
            continue
        bus_up[bus] = True
        for k in keys:
            used.append(pin(k, in_pins if k == "SPI_MISO" else out_pins))
    try:
        devices = parse_devices(cfg.get("DEVICES", ""))
    except ValueError as e:
        errors.append(f"DEVICES: {e}")
        devices = []
    if b["family"] == "rp2" and (devices or any(bus_up.values())):
        errors.append("the Pico boards do not support I2C or SPI devices yet: "
                      "leave DEVICES empty and the bus pins at none")
    addresses = set()
    for kind, address in devices:
        bus = DRIVERS[kind]["bus"]
        if not bus_up[bus]:
            errors.append(f"DEVICES: {kind} is on the {bus.upper()} bus, which has no pins set "
                          f"({', '.join(BUS_KEYS[bus])})")
        if bus == "i2c" and not 0x08 <= address <= 0x77:
            errors.append(f"DEVICES: {kind}@0x{address:02x} is not an I2C address (0x08-0x77)")
        elif bus == "spi" and address not in out_pins:
            errors.append(f"DEVICES: {kind}@{address} is not a usable chip select pin on {board}")
        elif bus == "spi":
            used.append(address)
        if (bus, address) in addresses:
            where = f"I2C address 0x{address:02x}" if bus == "i2c" else f"chip select {address}"
            errors.append(f"DEVICES: two devices at {where}")
        addresses.add((bus, address))
    if len(devices) > 8:
        errors.append("DEVICES: at most 8")

    nums = [p for p in used if isinstance(p, int)]
    if len(nums) != len(set(nums)):
        errors.append("the microphone, bus, chip select, button and LED pins must all be different")

    ring = cfg.get("RING_KB", "")
    lookback = cfg.get("LOOKBACK_MS", "")
    if not ring.isdigit() or not 32 <= int(ring) <= 1024:
        errors.append("RING_KB must be 32-1024")
    elif not lookback.isdigit() or int(lookback) > max_lookback_ms(int(ring)):
        errors.append(f"LOOKBACK_MS must be 0-{max_lookback_ms(int(ring))} "
                      f"(the {ring} KB ring buffer holds {ring_ms(int(ring))} ms)")
    return errors


def defines(cfg):
    """(name, value, is_string) for each build define."""
    def pin_value(v):
        return {"none": -1, "builtin": -2}.get(v, None) if not v.isdigit() else int(v)

    return [
        ("BPS_BOARD", BOARDS[cfg["BOARD"]]["short"], True),
        ("BPS_WIFI_SSID", cfg["WIFI_SSID"], True),
        ("BPS_WIFI_PASSWORD", cfg.get("WIFI_PASSWORD", ""), True),
        ("BPS_SERVER_HOST", cfg["SERVER_HOST"], True),
        ("BPS_SERVER_PORT", int(cfg["SERVER_PORT"]), False),
        ("BPS_CLIENT_NAME", cfg.get("CLIENT_NAME", ""), True),
        ("BPS_CLIENT_ID", cfg.get("CLIENT_ID", ""), True),
        ("BPS_DEVICE_NAME", cfg.get("DEVICE_NAME", ""), True),
        ("BPS_I2S_BCLK", int(cfg["I2S_BCLK"]), False),
        ("BPS_I2S_WS", -1 if is_pdm(cfg["BOARD"]) else int(cfg["I2S_WS"]), False),
        ("BPS_MIC_PDM", 1 if is_pdm(cfg["BOARD"]) else 0, False),
        ("BPS_I2S_DATA", int(cfg["I2S_DATA"]), False),
        ("BPS_MIC_CHANNEL", 1 if cfg["MIC_CHANNEL"] == "right" else 0, False),
        ("BPS_BUTTON_PIN", pin_value(cfg["BUTTON_PIN"]), False),
        ("BPS_LED_PIN", pin_value(cfg["LED_PIN"]), False),
        ("BPS_LOOKBACK_MS", int(cfg["LOOKBACK_MS"]), False),
        ("BPS_RING_BYTES", int(cfg["RING_KB"]) * 1024, False),
    ] + [(f"BPS_{k}", pin_value(cfg[k]), False) for keys in BUS_KEYS.values() for k in keys] + [
        ("BPS_DEVICES", format_devices(parse_devices(cfg.get("DEVICES", ""))), True),
    ]


def format_conf(cfg):
    lines = ["# bps microcontroller client settings (written by ./configure.sh).",
             "# Holds the Wi-Fi password: keep it private.", ""]
    for key in KEYS:
        if key in cfg:
            lines.append(f"{key}={shlex.quote(cfg[key])}")
    return "\n".join(lines) + "\n"


def shell(values, prefix=""):
    return "".join(f"{prefix}{k}={shlex.quote(v)}\n" for k, v in values.items())


def cli(argv):
    cmd = argv[1] if len(argv) > 1 else ""
    if cmd == "boards":
        for key, b in BOARDS.items():
            print(f"{key}\t{b['title']}")
    elif cmd == "defaults" and len(argv) == 3 and argv[2] in BOARDS:
        sys.stdout.write(shell(defaults(argv[2]), "DEF_"))
    elif cmd == "shell" and len(argv) == 3:
        sys.stdout.write(shell({k: v for k, v in load(argv[2]).items() if k in KEYS}))
    elif cmd == "drivers":
        for kind, d in DRIVERS.items():
            print(f"{kind}\t{d['bus']}\t{d['about']}")
    elif cmd == "max-lookback" and len(argv) == 3 and argv[2].isdigit():
        print(max_lookback_ms(int(argv[2])))
    elif (cmd in ("write", "check") and len(argv) == 3) or (cmd == "check-env" and len(argv) == 2):
        if cmd == "check":
            cfg = load(argv[2])
        else:
            cfg = {k: os.environ[f"BPS_SET_{k}"] for k in KEYS if f"BPS_SET_{k}" in os.environ}
        cfg = complete(cfg)
        errors = validate(cfg)
        if errors:
            print("\n".join(errors))
            return 1
        if cmd == "write":
            fd = os.open(argv[2], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                f.write(format_conf(cfg))
            os.chmod(argv[2], 0o600)
    else:
        print((__doc__ or "").strip())
        return 2
    return 0


def scons(env):
    name = env["PIOENV"]
    if name not in BOARDS:
        return  # e.g. the native test environment
    path = os.environ.get("BPS_CONF") or os.path.join(env["PROJECT_DIR"], "bps.conf")
    if not os.path.exists(path):
        sys.stderr.write(f"bps: {path} not found; run ./configure.sh first\n")
        env.Exit(1)
        return
    try:
        cfg = complete(load(path))
    except ValueError as e:
        sys.stderr.write(f"bps: {path}: {e}\n")
        env.Exit(1)
        return
    errors = validate(cfg)
    if not errors and cfg["BOARD"] != name:
        errors.append(f"it is for BOARD={cfg['BOARD']}; build with: pio run -e {cfg['BOARD']}")
    if errors:
        sys.stderr.write(f"bps: {path}:\n  " + "\n  ".join(errors) + "\n")
        env.Exit(1)
        return
    print(f"bps: using {path} ({cfg['BOARD']}, server {cfg['SERVER_HOST']}:{cfg['SERVER_PORT']})")
    env.Append(CPPDEFINES=[(n, env.StringifyMacro(v) if s else v) for n, v, s in defines(cfg)])


try:
    Import("env")  # noqa: F821 (provided by SCons)
except NameError:
    if __name__ == "__main__":
        sys.exit(cli(sys.argv))
else:
    scons(env)  # noqa: F821
