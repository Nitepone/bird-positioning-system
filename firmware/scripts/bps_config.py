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
"""

import os
import re
import shlex
import sys

SAMPLE_RATE = 48000

PICO_PINS = set(range(0, 23)) | {26, 27, 28}  # GP23-25 and GP29 belong to the radio
ESP32_OUT_PINS = {0, 2, 4, 5, 12, 13, 14, 15, 16, 17, 18, 19, 21, 22, 23, 25, 26, 27, 32, 33}
ESP32_IN_PINS = ESP32_OUT_PINS | {34, 35, 36, 39}

BOARDS = {
    "pico_w": {
        "title": "Raspberry Pi Pico W (RP2040)",
        "short": "picow",
        "family": "rp2",
        "defaults": {"I2S_BCLK": "18", "I2S_DATA": "20", "LED_PIN": "builtin",
                     "BUTTON_PIN": "none", "RING_KB": "160", "LOOKBACK_MS": "750"},
    },
    "pico2_w": {
        "title": "Raspberry Pi Pico 2 W (RP2350)",
        "short": "pico2w",
        "family": "rp2",
        "defaults": {"I2S_BCLK": "18", "I2S_DATA": "20", "LED_PIN": "builtin",
                     "BUTTON_PIN": "none", "RING_KB": "400", "LOOKBACK_MS": "2500"},
    },
    "esp32": {
        "title": "ESP32 (classic, e.g. ESP32-DevKitC)",
        "short": "esp32",
        "family": "esp32",
        "defaults": {"I2S_BCLK": "26", "I2S_WS": "25", "I2S_DATA": "33", "LED_PIN": "2",
                     "BUTTON_PIN": "0", "RING_KB": "192", "LOOKBACK_MS": "1000"},
    },
}

COMMON_DEFAULTS = {
    "SERVER_PORT": "2473",
    "CLIENT_NAME": "",
    "CLIENT_ID": "",
    "DEVICE_NAME": "ICS-43434",
    "MIC_CHANNEL": "left",
}

KEYS = ["BOARD", "WIFI_SSID", "WIFI_PASSWORD", "SERVER_HOST", "SERVER_PORT", "CLIENT_NAME",
        "I2S_BCLK", "I2S_WS", "I2S_DATA", "MIC_CHANNEL", "BUTTON_PIN", "LED_PIN",
        "LOOKBACK_MS", "RING_KB", "DEVICE_NAME", "CLIENT_ID"]

UUID_RE = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")


def defaults(board):
    d = dict(COMMON_DEFAULTS)
    d.update(BOARDS[board]["defaults"])
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
    return full


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
    family = BOARDS[board]["family"]

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

    out_pins = PICO_PINS if family == "rp2" else ESP32_OUT_PINS
    in_pins = PICO_PINS if family == "rp2" else ESP32_IN_PINS
    used = [pin("I2S_BCLK", out_pins), pin("I2S_WS", out_pins), pin("I2S_DATA", in_pins)]
    button = pin("BUTTON_PIN", in_pins, ("none",))
    led = pin("LED_PIN", out_pins, ("none", "builtin") if family == "rp2" else ("none",))
    used += [p for p in (button, led) if isinstance(p, int)]
    nums = [p for p in used if isinstance(p, int)]
    if len(nums) != len(set(nums)):
        errors.append("the I2S, button and LED pins must all be different")

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
        ("BPS_I2S_WS", int(cfg["I2S_WS"]), False),
        ("BPS_I2S_DATA", int(cfg["I2S_DATA"]), False),
        ("BPS_MIC_CHANNEL", 1 if cfg["MIC_CHANNEL"] == "right" else 0, False),
        ("BPS_BUTTON_PIN", pin_value(cfg["BUTTON_PIN"]), False),
        ("BPS_LED_PIN", pin_value(cfg["LED_PIN"]), False),
        ("BPS_LOOKBACK_MS", int(cfg["LOOKBACK_MS"]), False),
        ("BPS_RING_BYTES", int(cfg["RING_KB"]) * 1024, False),
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
