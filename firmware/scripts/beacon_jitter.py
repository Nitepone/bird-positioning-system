#!/usr/bin/env python3
"""Beacon sync experiment: how precisely do two boards agree on when a Wi-Fi
beacon arrived?

Boards built with -DBPS_BEACON_LOG (see platformio.ini) log every beacon of
their network's access points: its BSSID and TSF (which identify it), and two
local arrival times, the radio's (rx) and esp_timer's when the callback ran
(cb). This script captures those logs and compares them:

    beacon_jitter.py capture [--seconds N] PORT:FILE [PORT:FILE ...]
    beacon_jitter.py analyze [--window S] FILE_A FILE_B

`analyze` reports, per access point:
- each board against the AP's TSF: how well a straight line (offset and drift
  over a window) maps the TSF to the board's clock. This is what syncing to
  the AP's clock alone would give, AP timestamp jitter included;
- the two boards against each other (reference broadcast: both heard the same
  beacon, so the AP's jitter cancels). This is what syncing the boards to each
  other would give;
- for each, the radio's timestamps (rx), the same in esp_timer time (esp,
  via the "macclk" links the firmware logs every second), and esp_timer when
  the callback ran (cb). Audio is timestamped in esp_timer time, so "esp" is
  what a positioning client could use;
- for both, "next window": the error when the line fitted on one window is
  used for the next, as a running client would.
Errors are in microseconds.
"""

import argparse
import os
import re
import select
import statistics
import sys
import termios
import time
import tty

LINE_RE = re.compile(r"beacon ([0-9a-f:]{17}) ch (\d+) seq (\d+) tsf (\d+) rx (\d+) cb (-?\d+) rssi (-?\d+)")
MACCLK_RE = re.compile(r"macclk esp (-?\d+) mac (\d+) width (\d+)")


# ---- Capture ----

def capture(specs, seconds):
    ports = []
    for spec in specs:
        port, sep, path = spec.rpartition(":")
        if not sep or not port:
            sys.exit(f"expected PORT:FILE, got {spec!r}")
        fd = os.open(port, os.O_RDONLY | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(fd)
        termios.tcflush(fd, termios.TCIFLUSH)
        ports.append({"port": port, "fd": fd, "out": open(path, "ab"), "buf": b"", "beacons": 0})
    start = last = time.monotonic()
    print(f"capturing {len(ports)} port(s)" + (f" for {seconds} s" if seconds else "") + "; Ctrl-C stops")
    try:
        while not seconds or time.monotonic() - start < seconds:
            ready, _, _ = select.select([p["fd"] for p in ports], [], [], 1.0)
            for p in ports:
                if p["fd"] not in ready:
                    continue
                try:
                    data = os.read(p["fd"], 4096)
                except BlockingIOError:
                    continue
                if not data:
                    sys.exit(f"{p['port']}: disconnected")
                p["out"].write(data)
                p["buf"] += data
                *lines, p["buf"] = p["buf"].split(b"\n")
                p["beacons"] += sum(b" beacon " in line and b" tsf " in line for line in lines)
            if time.monotonic() - last >= 10:
                last = time.monotonic()
                print(f"{last - start:6.0f} s: " + ", ".join(f"{p['port']} {p['beacons']} beacons" for p in ports))
                for p in ports:
                    p["out"].flush()
    except KeyboardInterrupt:
        pass
    for p in ports:
        p["out"].close()
        os.close(p["fd"])


# ---- Reading logs ----

def load(path):
    """The beacons of a log's last boot: dicts with bssid, tsf, rx (unwrapped),
    cb, rssi, and esp (rx in esp_timer time, if the log links the clocks)."""
    obs, links = [], []
    prev = None
    mac64 = 0

    def unwrap(v):
        # The radio's clock is 32-bit us; beacons and links share one timeline.
        nonlocal prev, mac64
        if prev is None:
            mac64 = v
        else:
            d = (v - prev) % (1 << 32)
            mac64 += d - (1 << 32) if d >= 1 << 31 else d
        prev = v
        return mac64

    with open(path, errors="replace") as f:
        for line in f:
            m = LINE_RE.search(line)
            if m:
                bssid, ch, _seq, tsf, rx, cb, rssi = m.groups()
                rx, cb = int(rx), int(cb)
                if obs and cb < obs[-1]["cb"] - 1_000_000:
                    obs, links, prev = [], [], None  # the board restarted: keep the last boot
                obs.append({"bssid": bssid, "ch": int(ch), "tsf": int(tsf), "rx": unwrap(rx), "rx32": rx,
                            "cb": cb, "rssi": int(rssi)})
                continue
            m = MACCLK_RE.search(line)
            if m:
                esp, mac, width = map(int, m.groups())
                if links and esp < links[-1]["esp"] - 1_000_000:
                    obs, links, prev = [], [], None
                links.append({"esp": esp, "mac": unwrap(mac), "width": width})
    # esp_timer - radio clock, interpolated between the links around each beacon.
    links.sort(key=lambda k: k["mac"])
    if len(links) >= 2:
        j = 0
        for o in obs:
            while j < len(links) - 2 and links[j + 1]["mac"] < o["rx"]:
                j += 1
            l0, l1 = links[j], links[j + 1]
            off0, off1 = l0["esp"] - l0["mac"], l1["esp"] - l1["mac"]
            f = (o["rx"] - l0["mac"]) / (l1["mac"] - l0["mac"]) if l1["mac"] != l0["mac"] else 0
            o["esp"] = o["rx"] + off0 + (off1 - off0) * min(max(f, 0), 1)
    return obs, links


# ---- Fitting ----

def line_fit(xs, ys):
    n = len(xs)
    mx, my = sum(xs) / n, sum(ys) / n
    sxx = sum((x - mx) ** 2 for x in xs)
    if sxx == 0:
        return my, 0.0, mx
    b = sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / sxx
    return my, b, mx  # y = a + b (x - mx)


def robust_fit(xs, ys):
    """Least squares, then again without points far off the first line."""
    a, b, mx = line_fit(xs, ys)
    res = [y - (a + b * (x - mx)) for x, y in zip(xs, ys)]
    mad = statistics.median(abs(r) for r in res) * 1.4826
    keep = [i for i, r in enumerate(res) if abs(r) <= max(6 * mad, 3.0)]
    if len(keep) >= 10:
        a, b, mx = line_fit([xs[i] for i in keep], [ys[i] for i in keep])
    return lambda x: a + b * (x - mx), b


def windowed(points, window_us):
    """points: (x, y) sorted by x. Residuals within each window's own fit, the
    errors of each window's fit applied to the next window, and the slopes."""
    if not points:
        return [], [], []
    windows, cur, start = [], [], points[0][0]
    for x, y in points:
        if x - start >= window_us:
            windows.append(cur)
            cur, start = [], x
        cur.append((x, y))
    windows.append(cur)
    resid, pred, slopes, prev = [], [], [], None
    for w in windows:
        if len(w) < 10:
            prev = None
            continue
        xs, ys = [p[0] for p in w], [p[1] for p in w]
        f, b = robust_fit(xs, ys)
        resid += [y - f(x) for x, y in w]
        if prev:
            pred += [y - prev(x) for x, y in w]
        slopes.append(b)
        prev = f
    return resid, pred, slopes


def summary(errs):
    if not errs:
        return "no data"
    a = sorted(abs(e) for e in errs)

    def pct(p):
        return a[min(len(a) - 1, int(p / 100 * len(a)))]

    sd = statistics.pstdev(errs)
    big = sum(e > 100 for e in a)
    return (f"sd {sd:7.1f}  |err| p50 {pct(50):6.1f}  p95 {pct(95):7.1f}  p99 {pct(99):7.1f}  "
            f"max {a[-1]:8.1f}  (n {len(a)}" + (f", {big} over 100" if big else "") + ")")


# ---- Analysis ----

def analyze(path_a, path_b, window_s):
    window_us = window_s * 1_000_000
    loaded = {"A": load(path_a), "B": load(path_b)}
    logs = {n: v[0] for n, v in loaded.items()}
    for name, path in (("A", path_a), ("B", path_b)):
        obs, links = loaded[name]
        print(f"{name}: {path}: {len(obs)} beacons, {len(links)} clock links")
        if not obs:
            sys.exit(f"{path}: no beacon lines (was it built with -DBPS_BEACON_LOG?)")
        # If the radio's timestamps are esp_timer's low 32 bits, cb - rx is the callback delay.
        delays = sorted((o["cb"] - o["rx32"]) % (1 << 32) for o in obs)
        med = delays[len(delays) // 2]
        if med < 1_000_000:
            print(f"   callback runs {med} us after the radio's timestamp (median; "
                  f"p99 {delays[int(len(delays) * 0.99)]}, max {delays[-1]})")
        else:
            print("   the radio's timestamps are not esp_timer time (cb - rx is not small)")
        if len(links) >= 2:
            offs = [(k["mac"], k["esp"] - k["mac"]) for k in links]
            f, b = robust_fit([x for x, _ in offs], [y for _, y in offs])
            res = [y - f(x) for x, y in offs]
            widths = sorted(k["width"] for k in links)
            print(f"   esp_timer - radio clock: {offs[0][1]} us at first, {b * 1e6:+.2f} ppm; "
                  f"around the line {summary(res).strip()}; read in {widths[len(widths) // 2]} us")
    print()

    bssids = sorted({o["bssid"] for o in logs["A"]} & {o["bssid"] for o in logs["B"]})
    if not bssids:
        print("the two logs have no access point in common")
    for bssid in bssids:
        per = {n: {o["tsf"]: o for o in logs[n] if o["bssid"] == bssid} for n in logs}
        common = sorted(set(per["A"]) & set(per["B"]))
        a0 = next(iter(per["A"].values()))
        tsfs = sorted(per["A"])
        gaps = [b - a for a, b in zip(tsfs, tsfs[1:]) if b > a]
        interval = statistics.median(gaps) if gaps else 0
        print(f"== {bssid} (channel {a0['ch']}; beacons every {interval / 1000:.1f} ms) ==")
        for n in logs:
            ts = sorted(per[n])
            if len(ts) < 2 or not interval:
                continue
            expected = (ts[-1] - ts[0]) / interval + 1
            rssi = statistics.median(o["rssi"] for o in per[n].values())
            print(f"   {n} heard {len(ts)} of about {expected:.0f} ({100 * len(ts) / expected:.1f}%), "
                  f"rssi {rssi:.0f} dBm")
        print(f"   both heard {len(common)}")
        if len(common) < 20:
            print()
            continue
        span = (common[-1] - common[0]) / 1e6
        print(f"   over {span / 60:.1f} min, fitted in {window_s} s windows")
        clocks = [("rx", "radio"), ("esp", "radio, in esp_timer time"), ("cb", "callback")]
        for clock, title in clocks:
            if any(clock not in o for n in logs for o in per[n].values()):
                continue
            print(f"  {clock} timestamps ({title}):")
            for n in logs:
                pts = sorted((t, per[n][t][clock] - t) for t in per[n])
                resid, pred, slopes = windowed(pts, window_us)
                drift = statistics.median(slopes) * 1e6 if slopes else float("nan")
                print(f"    {n} vs the AP's TSF ({drift:+.1f} ppm)")
                print(f"      within window  {summary(resid)}")
                print(f"      next window    {summary(pred)}")
            pts = [(t, per["A"][t][clock] - per["B"][t][clock]) for t in common]
            resid, pred, slopes = windowed(pts, window_us)
            drift = statistics.median(slopes) * 1e6 if slopes else float("nan")
            print(f"    A vs B, same beacons ({drift:+.1f} ppm)")
            print(f"      within window  {summary(resid)}")
            print(f"      next window    {summary(pred)}")
        print()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    c = sub.add_parser("capture", help="save serial ports' output to files")
    c.add_argument("--seconds", type=float, default=0, help="stop after this long (default: Ctrl-C)")
    c.add_argument("specs", nargs="+", metavar="PORT:FILE")
    a = sub.add_parser("analyze", help="compare two boards' beacon logs")
    a.add_argument("--window", type=float, default=10, help="fit window in seconds (default 10)")
    a.add_argument("file_a")
    a.add_argument("file_b")
    args = ap.parse_args()
    if args.cmd == "capture":
        capture(args.specs, args.seconds)
    else:
        analyze(args.file_a, args.file_b, args.window)


if __name__ == "__main__":
    main()
