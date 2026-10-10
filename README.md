# bsp

Identifies birds by their calls and estimates their direction using several microphones.

## User guide

### 1. Start the server

```sh
cargo build --release
scripts/fetch-birdnet.sh                          # download the BirdNET model (once, ~270 MB)
cp bsp-server.example.toml bsp-server.toml        # edit if needed
sudo setcap cap_net_bind_service=+ep target/release/bsp-server   # allow ports 80/443 (after each build)
./target/release/bsp-server -c bsp-server.toml
```

Open the web UI at `https://SERVER/` (plain `http://` redirects there). Its pages are
**Dashboard** and **Detections** (what was heard), **Config** (clients and settings), **Clocks**
(each client's clock synchronisation) and **Logs** (raw events).

On first start the server creates a self-signed certificate in `tls/`, so your browser warns
once; accept it to continue. To use a real certificate, put it at `tls/cert.pem` and its key at
`tls/key.pem` (or point `[web] cert_path` / `key_path` elsewhere). If the server's LAN address or
DNS name is missing from the generated certificate, list it under `[web] self_signed_names`,
delete `tls/` and restart.

| Port | Used by |
|---|---|
| TCP 443 | web UI (HTTPS) |
| TCP 80 | redirect to HTTPS |
| TCP + UDP 2473 | clients (2473 spells "BIRD" on a phone keypad) |

If you'd rather not grant the port capability, set other ports under `[web]` instead.

The server identifies species with BirdNET (see [BirdNET](#birdnet)). It won't start until the
model has been downloaded.

### 2. Set the site location and confidence levels

On the **Config** page under **Server settings**:

- **Site location:** enter latitude and longitude, or click **Detect location** (your browser
  will ask for permission).
  BirdNET's geo model then works out which species occur here at any time of year. Species
  outside that list are still reported but flagged **unexpected**.
- **Minimum confidence:** separate levels for expected and unexpected species: very high (95%),
  high (90%), medium (80%) or low (70%). The defaults are medium for expected species and high
  for unexpected ones, so unusual species need stronger evidence. Detections below the lowest
  level (70%) are never kept; any already in the database are removed when the server starts.

Click **Save settings**. Settings are stored in the server's database.

### 3. Start a client on each microphone device

```sh
./target/release/bsp-client --list-devices                        # find your microphone
./target/release/bsp-client -s http://SERVER:2473 -d "USB Mic"     # -d is optional
./target/release/bsp-client -s http://SERVER:2473 -n "North fence" # suggest a name (optional)
```

The client shows up in the **Clients** table on the **Config** page. Its ID is stored in `./state/client_id`,
so it stays the same across restarts.

**Or use a browser as a client:** open `https://SERVER/client` on the device (phone, laptop) and
click **Start**. The page shows the microphone in use and whether the browser really turned its
audio processing off, a live spectrogram (the last 6 s, 0–12 kHz, so you can see calls arrive), connection and clock status, and recent events. **Flag for
positioning** does what Enter does on `bsp-client`. **Low power mode** blacks out the screen while
capture continues. Keep the page open and in front: most browsers pause the microphone in background
tabs, and iOS pauses it when the screen locks (the page holds a screen wake lock where supported).
Its ID and **Suggested name** are kept in the browser's local storage, so each browser profile is
one client.

Browser clients are less precise than `bsp-client` for positioning:

- Browsers apply echo cancellation, noise suppression and automatic gain by default. The page asks
  for all of them off (`echoCancellation`, `noiseSuppression`, `autoGainControl` and `voiceIsolation`
  set to `false`). It then shows what the browser actually applied. Chrome, Edge and Firefox honour
  this. Safari on macOS turns its voice processing off with `echoCancellation: false`. On macOS, also
  set the menu-bar **Mic Mode** to *Standard*, because the system's *Voice Isolation* mode is outside
  the page's control. **Safari on iOS/iPadOS keeps its voice processing on** whatever the page
  requests (it band-limits and alters the signal), so use iOS devices for identification only, not
  positioning.
- Browsers cannot discipline the system clock or send UDP, so the page measures its offset to the
  server over a WebSocket and corrects its own timestamps, like `bsp-client` (see
  [Keep client clocks accurate](#keep-client-clocks-accurate)). A slow or busy network therefore
  shows up as **NOT synced**, not as wrong directions.
- Some input latency is invisible to a web page. Chrome reports it and the page compensates. Other
  browsers may leave a constant offset of a few milliseconds per device.

**Or use a microcontroller as a client:** a Raspberry Pi Pico W, Pico 2 W or classic ESP32 with an
I2S microphone such as the Adafruit ICS-43434 makes a cheap, always-on client. See
[Microcontroller clients](#microcontroller-clients-pico-w-esp32).

### 4. Set each client's name and position

1. Press **Enter** in the client's terminal. A banner appears on every page; click
   **Configure** to open that client in the **Config** page's form.
2. Enter a name and its position in metres from a reference point you choose:
   **East (x), North (y), Up (z)**. A client started with `-n` (or given a name on the browser
   client) is already shown by that suggested name. A name entered here takes precedence, and
   clearing it goes back to the suggestion.
3. Click **Save**.

You can also click **Edit** on any row in the Clients table.

The **Microphone positions** map beside the form shows every positioned microphone as an "×"
around the site origin, with distance rings. The client being edited is highlighted, and a
position you type appears as "unsaved" before you save it, which makes typos easy to spot.

### 5. Watch the birds

The **Dashboard** shows one day at a time (today by default; use the arrows or date picker):

- totals for the day: species heard, detections, the most active species and the last one heard;
- a **timeline** with one row per species and a mark for each detection, from midnight to
  midnight. Hover a mark for details; click a mark or a species name to open that species'
  detections for the day. Scroll over the chart or drag the bar under it to zoom in;
- tables of the day's species and, beside it, the day's **unexpected** species, with first and
  last times heard.

By default the dashboard shows only **expected** species: those the geo model expects at the
site. **Show** can add unexpected species (orange), or everything, including sounds the range
data doesn't cover (non-bird classes, "Unknown bird" from the mock identifier). A note under
the timeline says what is hidden. Without a site location nothing can be classified, so
everything is shown.

The **Detections** page lists individual detections, newest first, with a picture of the
recording, the species, confidence and which monitors heard it. The picture is a spectrogram
(0–12 kHz, low to high, with steady background noise flattened so calls stand out; an orange bar
marks the identified call) or, if you choose **Show recordings as: waveform**, the waveform with
the call shaded. Click it to play the clearest recording, or a monitor's button to hear and show
that monitor's; a red line follows playback. **Normalize volume** (on by default) plays every
recording at the same peak level, up to +30 dB, so quiet calls are audible; a loud non-bird sound
in the same clip limits how much a quiet call is raised.
Click a scientific name (dashboard tables, Detections, Config's expected-species list) to open the
species on Wikipedia in a new tab. Filter by species name, monitor, date range, expected or unexpected species, minimum confidence,
or detections with a direction. The filters are part of the page address, so a filtered view can
be bookmarked or shared.

A detection shows a direction (e.g. `NE (47°)`) only when **at least 3** clients heard the call
and each of them:

- has a position set,
- is **active** (sent a heartbeat in the last 15 s), and
- is **synced** (its audio timestamps are within 2 ms of the server's clock, worst case).

The **Logs** page shows the raw activity log, which is useful when something isn't working.

### Keep client clocks accurate

Direction finding needs every client's audio timestamps on the server's clock, to well under 1 ms
(sound travels 34 cm per millisecond). Clients measure their clock against the server several
times a second (`bsp-client` over UDP, the browser client over its WebSocket) and add the measured offset
to their audio timestamps, so they end up on the server's clock even if their own is off. The
offset comes from a line fitted through the fastest round trips of the last minute or so, which
also tracks a clock that runs fast or slow (drift).

With every heartbeat a client reports the **timestamp error**: the worst case left in its
timestamps. That is how far the estimate has moved since the last chunk was corrected, plus half
the fastest round trip (a measurement cannot tell how delay splits between the two directions),
plus drift within half a chunk. A client whose timestamp error is over `max_clock_offset_us`
(2 ms by default) is **NOT synced** and left out of positioning. On a wired network the error is
typically well under 0.5 ms; on Wi-Fi it depends on how fast its best round trips are.

Still keep each client's OS clock steady with chrony or similar, so the clock doesn't drift fast
or jump. Pointing the clients' chrony at the server (run chronyd on the server with an `allow`
line for your network) works well. If clients and server are all disciplined by PTP or GPS, which
beats measuring over the network, run `bsp-client --trust-os-clock`: it then sends its OS clock's
timestamps unchanged and only reports how far off they are.

The **Clocks** page shows every client's timestamp error, clock offset, drift, round trips and
answered measurements, and for one client at a time, charts of the last 15 minutes to an hour
(kept in the server's memory, so they start afresh when it restarts). Use it to compare networks
or placements, or to screenshot a client's sync for a bug report.

### Microcontroller clients (Pico W / ESP32)

The firmware in `firmware/` turns a Raspberry Pi Pico W, Pico 2 W or classic ESP32 (e.g.
ESP32-DevKitC) with an I2S microphone into a client. It uses the same client API as `bsp-client`,
so to the server it is just another client: it appears in the **Clients** table, is measured on the
**Clocks** page and takes part in positioning. It is written for the Adafruit ICS-43434 breakout
(any I2S microphone with 24-bit samples in 32-bit slots should work).

**Wiring** (defaults; `./configure.sh` lets you choose other pins):

| ICS-43434 | Pico W / Pico 2 W | ESP32 |
|---|---|---|
| 3V | 3V3 (pin 36) | 3V3 |
| GND | GND | GND |
| BCLK | GP18 | GPIO 26 |
| LRCL (word select) | GP19 (must be BCLK + 1) | GPIO 25 |
| DOUT | GP20 | GPIO 33 |
| SEL | GND or unconnected (left channel) | GND or unconnected |

**Configure, build and flash.** Two scripts in `firmware/` do it:

```sh
cd firmware
./configure.sh       # edit the settings: board, Wi-Fi, server, name, pins
./build.sh -f -m     # build, flash the board connected by USB, show its log
```

`./configure.sh` is a text UI (it needs `whiptail` or `dialog`). It lists every setting with its
current value and says whether they are ready to use. Choose a setting to change it, then
**Save**, or **Save & build** to go straight on to building and flashing. It writes `bps.conf`
(see `bps.conf.example` to write one by hand).

`./build.sh` builds for the board named in the config. `-f` flashes, `-m` shows the board's log
(Ctrl-C leaves it), `-c` rebuilds from scratch, `-t` runs the unit tests, and `-h` lists the
rest. It uses [PlatformIO](https://platformio.org/) Core if installed (free and open source:
`pipx install platformio`). If not, it uses Docker, building the `bps-firmware` image from
`firmware/Dockerfile` the first time. That image holds every board's toolchain (about 8.5 GB), so
later builds work offline. Pass `--docker` to use it even when PlatformIO is installed.

To flash a Pico for the first time, hold its BOOTSEL button while plugging it in. Later flashes
work without it. When building in Docker, `./build.sh -f` flashes a Pico from outside the
container: with `picotool` if installed, otherwise by copying `firmware.uf2` to the Pico's
BOOTSEL drive. An ESP32 flashes over its USB serial port (`-p /dev/ttyUSB0` if detection picks
the wrong one). The firmware is written to `firmware/.pio/build/<board>/`.

For several boards, keep one config per board: `./configure.sh north.conf`, then
`./build.sh -f north.conf`. Each board's client ID is derived from its chip's unique ID, so it
stays the same across reflashes without any storage. The settings, including the Wi-Fi password,
are compiled into the firmware. Config files are created readable only by you, and git and the
Docker build ignore them.

**Status LED:**

| LED | Meaning |
|---|---|
| fast blink | joining Wi-Fi or registering |
| short flash every second | registered, clock not yet synced |
| on | synced and listening |
| on, flickering | uploading a chunk |

To request positioning (what Enter does on `bsp-client`), wire a push button from a GPIO to GND
and set it as **Button pin** in `./configure.sh`. ESP32 boards use their BOOT button (GPIO 0) by
default.

**How it differs from `bsp-client`.**

- **Noise gate and look-back.** A 9 s chunk is 864 KB, far more than these boards' RAM, so they
  cannot record a chunk and then decide whether to send it. Instead, they run the noise gate on
  the live audio and keep the last second or so in a ring buffer. When a sound passes the gate,
  they stream a 9 s chunk that starts that **look-back** before the sound. If it is still going
  at the end of a chunk, the next chunk follows without a gap. Silence is never sent.
- **What gets dropped.** A sound that starts quietly and passes the gate only later than the
  look-back loses its quiet start. There is no upload queue: while the server is unreachable,
  audio is dropped. If Wi-Fi stalls for longer than the ring buffer holds, the lost stretch is
  sent as silence, so the rest of the chunk keeps its exact timing.
- **Wi-Fi and sync.** A worst-case clock error includes half the fastest round trip, and a Pico
  W's Wi-Fi round trips are rarely under 4 ms. So on many networks it stays above the server's
  default 2 ms limit (**NOT synced**: identified, but left out of positioning). Its status line
  shows the best round trip and the signal strength.
- **Clock.** The boards have no clock of their own beyond a timer from boot. They set their
  clock from the server once, then measure and correct it over UDP exactly like `bsp-client`. The
  I2S sample clock runs off the same crystal as that timer. Wi-Fi power saving is turned off,
  because it delays packets.

| Board | RAM | Ring buffer | Default look-back |
|---|---|---|---|
| Pico W (RP2040) | 264 KB | about 1.4 s | 0.75 s |
| Pico 2 W (RP2350) | 520 KB | about 4 s | 2.5 s |
| ESP32 (classic) | 520 KB, about half free | about 1.5–2 s | 1 s |

The board takes what `RING_KB` asks for, as long as 32 KB of heap stays free for the network
stack. At startup it logs what it got and the look-back it can keep. It also logs the
microphone's level on both I2S channels: a channel at about −180 dBFS is silent, which means
`MIC_CHANNEL` or the wiring is wrong.

### Troubleshooting

| Symptom | Check |
|---|---|
| Client never appears | Client's `-s` URL uses port 2473 (not the web port); TCP 2473 reachable |
| Client never becomes synced | UDP 2473 reachable; **Clocks** page: low *Replies* means lost packets, a large *Best RTT* a slow or busy network |
| Server exits with "Permission denied" on port 443 or 80 | Run the `setcap` command above, or change the ports under `[web]` |
| No detections | Events log: are `audio` events arriving? If not, the noise filter is dropping everything; try `--no-gate` to test |
| Detections but no direction | Fewer than 3 positioned, active, synced clients heard the call |
| Microcontroller never appears | Its log (`./build.sh -m`): is Wi-Fi joining (the Pico W and ESP32 only use 2.4 GHz)? Does `SERVER_HOST`/`SERVER_PORT` point at the client port? |
| Microcontroller never uploads | The mic level line in its log: about −180 dBFS on the channel in use means wrong `MIC_CHANNEL` or wiring |
| "upload: the network stalled; … sent as silence" | Wi-Fi stalled for longer than the ring buffer holds: check the `Wi-Fi … dBm` in the status line, move the board or access point, or lower `LOOKBACK_MS` for more slack |
| Microcontroller stays **NOT synced** | Its status line's *best round trip*: the error bound includes half of it, so above about 4 ms it cannot get under the default 2 ms. Improve the Wi-Fi path (signal, a less busy channel, no router between board and server), or raise `max_clock_offset_us` if looser positioning is acceptable |

## BirdNET

`scripts/fetch-birdnet.sh` downloads a model from Zenodo, checks it against Zenodo's checksum,
and writes `models/birdnet-<version>/model.onnx` and `labels.txt`.

| | `v3.0` (default) | `v2.4` |
|---|---|---|
| Release | BirdNET+ V3.0 developer preview 3.1 | stable |
| Classes | 11,560 (includes some insects, frogs, mammals, humans) | 6,522 |
| Download | official ONNX, ~270 MB (`--fp32` for ~540 MB) | TensorFlow model converted to ONNX; temporarily installs TensorFlow (~1 GB) |
| License | CC BY-SA 4.0 + terms of use | CC BY-NC 4.0 (non-commercial) |

To use v2.4, run `scripts/fetch-birdnet.sh v2.4` and point `[identifier.birdnet]` at it. Set
`version = "v2.4"` and the two `models/birdnet-v2.4/...` paths.

The script also downloads the BirdNET+ geo model (V3.0.4, ~15 MB) into `models/birdnet-geo/`. It
is used with either acoustic model. A species counts as expected when its highest weekly
likelihood over the year is at least 3% (`[identifier.geo] threshold`). Species the geo model
doesn't cover (about 7% of V3.0's labels, mostly insects and recently renamed species) are always
treated as expected.

To check a recording without the server:
`cargo run --release -p bsp-core --example identify -- [v3.0|v2.4] file.wav`. To list the species
expected at a location: `cargo run --release -p bsp-core --example local_species -- LAT LON`.

For a run without any model, set `kind = "mock"`. Every loud burst is then reported as
"Unknown bird", and its confidence only reflects loudness.

## License and attribution

bsp uses BirdNET models, but they are not part of this repository; `scripts/fetch-birdnet.sh`
downloads them. Their licenses apply to anyone who downloads and uses them, separately from
bsp's own code:

| Model | License | Conditions |
|---|---|---|
| BirdNET+ V3.0 developer preview (acoustic, default) | [CC BY-SA 4.0](https://creativecommons.org/licenses/by-sa/4.0/) plus [terms of use](https://zenodo.org/records/20703646) | Commercial use allowed. Credit BirdNET, link the license and note changes; derivatives use the same license. **Never** use it for poaching or any military purpose. The terms describe it as provided solely for research and evaluation. |
| BirdNET v2.4 (acoustic) | [CC BY-NC 4.0](https://creativecommons.org/licenses/by-nc/4.0/) | Non-commercial use only. Credit BirdNET. |
| BirdNET+ Geomodel V3.0.4 (expected species) | [Apache 2.0](https://www.apache.org/licenses/LICENSE-2.0) | Keep the license and notices when redistributing. |

The license files are saved next to each model (`TERMS_OF_USE.txt`, `LICENSE-MODELS.md`).

**Attribution.** BirdNET requires that publications, presentations and derived tools credit the
models, either by citation or by an acknowledgment such as "Powered by BirdNET". If you publish
or share results or a deployment of bsp, include that credit and cite:

> Kahl, S., Wood, C. M., Eibl, M., & Klinck, H. (2021). BirdNET: A deep learning solution for
> avian diversity monitoring. *Ecological Informatics*, 61, 101236.
> https://doi.org/10.1016/j.ecoinf.2021.101236

BirdNET is developed by the K. Lisa Yang Center for Conservation Bioacoustics at the Cornell Lab
of Ornithology and Chemnitz University of Technology, with Museum für Naturkunde Berlin for
V3.0. Model releases: [V3.0 preview](https://zenodo.org/records/20703646),
[v2.4](https://zenodo.org/records/15050749),
[geo model](https://github.com/birdnet-team/geomodel/releases/tag/v3.0.4).

The web UI embeds [Apache ECharts](https://echarts.apache.org/) 6.1.0 (Apache 2.0), vendored with
its license and notice in `crates/bsp-server/web/vendor/` so the UI works without internet access.

## Testing without hardware

```sh
./target/release/bsp-client -s http://SERVER:2473 --wav-file birds.wav   # replay a recording
./target/release/bsp-sim --set-positions --bearing 225 --distance 60     # 3 virtual mics
```

`bsp-sim` registers 3 virtual clients, sets their positions and plays one simulated call from a
known direction. The detection appears after about 15 s. Its synthetic chirp is not a real bird,
so run the server with `kind = "mock"` for this test.

The microcontroller client's code also builds for Linux, with a WAV file standing in for the
microphone and Enter for the positioning button:

```sh
make -C firmware/host
firmware/host/bps-host-client -s SERVER -w birds.wav -n "Host test"   # 16-bit PCM WAV, loops
```

## Developer notes

**Crates:** `bsp-proto` (wire types), `bsp-core` (audio helpers, `Identifier`, `Locator`,
`Detection`), `bsp-server`, `bsp-client`, `bsp-sim`, `bps-mcu-check` (tests only: checks the
firmware's C clock estimator and band-pass against `bsp-proto` and `bsp-core`).

**Firmware** (`firmware/`, PlatformIO):
- `lib/bps_core/` is portable C11 with no vendor headers: C ports of the clock measurement,
  `Timestamper` and noise gate, plus the ring buffer and the client's state machines.
  `src/port/` adapts it to each board: `rp2.cpp` (arduino-pico; capture on core 1) and
  `esp32.cpp` (Arduino Wi-Fi with ESP-IDF I2S and lwIP sockets; capture in its own task).
  `host/` is a Linux board layer for testing.
- `scripts/bps_config.py` holds the board defaults and the config validation, used by both
  `./configure.sh` and the build. `./build.sh` drives PlatformIO, locally or in Docker.
- Tests: `./build.sh -t` (or `pio test -e native`) runs the core's unit tests on the computer, and `cargo test` runs
  `bps-mcu-check`. Both run without hardware.

**How a call becomes a detection:**

1. The client records 9 s chunks and skips any chunk with no clear 1–10 kHz sound above the
   background noise.
2. The server identifies calls in each chunk and narrows their start times to about 10 ms.
3. Calls of the same species from different clients that start within 200 ms of each other
   are treated as one detection.
4. If enough clients qualify, the server lines up their recordings and estimates direction
   from the arrival-time differences, plus a rough position when it can.

**APIs:**

- Client endpoints, under `/api/v1/client`:
  - `POST /register`
  - `POST /{id}/heartbeat`
  - `POST /{id}/audio`: WAV body plus an `x-bsp-start-ns` header
  - `POST /{id}/position-request`
  - `GET /timesync`: WebSocket carrying the UDP clock-measurement packets (for browser clients)
- Clock measurement: UDP on port 2473.
- The client endpoints are served on port 2473 (plain HTTP) and also on the web ports (HTTPS), where
  the browser client at `/client` uses them. The control endpoints are served only on the web ports.
- Control endpoints, under `/api/v1/control`:
  - `GET /status`
  - `GET|PUT /settings`: site location and confidence levels
  - `GET /local-species`: species expected at the site
  - `GET /clients`
  - `GET|PUT|DELETE /clients/{id}`
  - `GET /clients/{id}/clock?minutes=`: the client's clock reports (one per heartbeat) over the
    last `minutes` (default 30, at most 60)
  - `GET /detections`: filters `q` (name contains), `species` (exact scientific name),
    `client`, `from` / `to` (ns), `unexpected`, `located`, `min_confidence`, `before`, `limit`
  - `GET /timeline?from=&to=`: compact detections for the dashboard
  - `GET /species`: every species detected so far
  - `GET /detections/{id}/audio/{client_id}`: that client's clip as 16-bit FLAC (clips stored
    before FLAC are converted when the server starts), with an `x-call-range: start,end` header
    giving the identified call in seconds from the clip's start
  - `GET /detections/{id}/waveform[?client=]`: SVG waveform of the best (or given) clip
  - `GET /events`
  - `GET|DELETE /positioning-request`

**Known limitations:**

- No authentication on any endpoint.
- Audio clips (about 1–2 s per client per detection, roughly 50 KB each as FLAC) are kept in the
  database indefinitely.
- A call that spans two chunks may be missed.
- The direction is measured from the centre of the microphone array, so it is less accurate
  when the bird is close to the array.
