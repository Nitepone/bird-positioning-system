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

Open the web UI at `https://SERVER/` (plain `http://` redirects there). It has three pages:
**Bird Positioning System** (detections), **Config** (clients and settings) and **Logs** (raw
events).

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
  for unexpected ones, so unusual species need stronger evidence.

Click **Save settings**. Settings are stored in the server's database.

### 3. Start a client on each microphone device

```sh
./target/release/bsp-client --list-devices                        # find your microphone
./target/release/bsp-client -s http://SERVER:2473 -d "USB Mic"     # -d is optional
```

The client shows up in the **Clients** table on the **Config** page. Its ID is stored in `./state/client_id`,
so it stays the same across restarts.

### 4. Set each client's name and position

1. Press **Enter** in the client's terminal. A banner appears on every page; click
   **Configure** to open that client in the **Config** page's form.
2. Enter a name and its position in metres from a reference point you choose:
   **East (x), North (y), Up (z)**.
3. Click **Save**.

You can also click **Edit** on any row in the Clients table.

### 5. Watch the birds

The **Dashboard** shows one day at a time (today by default; use the arrows or date picker):

- totals for the day: species heard, detections, the most active species and the last one heard;
- a **timeline** with one row per species and a mark for each detection, from midnight to
  midnight. Unexpected species are orange and labelled. Hover a mark for details; click a mark
  or a species name to open that species' detections for the day. Scroll over the chart or drag
  the bar under it to zoom in;
- a table of the day's species with first and last times heard.

The **Detections** page lists individual detections, newest first, with a waveform image, the
species, confidence and which monitors heard it. Click the waveform to play the clearest
recording, or a monitor's button to hear its recording; the shaded band is the identified call.
Filter by species name, monitor, date range, expected or unexpected species, minimum confidence,
or detections with a direction. The filters are part of the page address, so a filtered view can
be bookmarked or shared.

A detection shows a direction (e.g. `NE (47°)`) only when **at least 3** clients heard the call
and each of them:

- has a position set,
- is **active** (sent a heartbeat in the last 15 s), and
- is **synced** (clock within 2 ms of the server).

The **Logs** page shows the raw activity log, which is useful when something isn't working.

### Keep client clocks accurate

Direction finding needs every client's clock to be accurate to well under 1 ms. Run chrony
(with a good NTP server), PTP or GPS on each client. If the Clients table shows
**NOT synced**, that client's clock is too far off.

### Troubleshooting

| Symptom | Check |
|---|---|
| Client never appears | Client's `-s` URL uses port 2473 (not the web port); TCP 2473 reachable |
| Client never becomes synced | UDP 2473 reachable; client clock disciplined (`chronyc tracking`) |
| Server exits with "Permission denied" on port 443 or 80 | Run the `setcap` command above, or change the ports under `[web]` |
| No detections | Events log: are `audio` events arriving? If not, the noise filter is dropping everything; try `--no-gate` to test |
| Detections but no direction | Fewer than 3 positioned, active, synced clients heard the call |

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

## Developer notes

**Crates:** `bsp-proto` (wire types), `bsp-core` (audio helpers, `Identifier`, `Locator`,
`Detection`), `bsp-server`, `bsp-client`, `bsp-sim`.

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
- Clock measurement: UDP on port 2473.
- The client endpoints are served only on port 2473 (plain HTTP), and the control endpoints only
  on the web ports (HTTPS).
- Control endpoints, under `/api/v1/control`:
  - `GET /status`
  - `GET|PUT /settings`: site location and confidence levels
  - `GET /local-species`: species expected at the site
  - `GET /clients`
  - `GET|PUT|DELETE /clients/{id}`
  - `GET /detections`: filters `q` (name contains), `species` (exact scientific name),
    `client`, `from` / `to` (ns), `unexpected`, `located`, `min_confidence`, `before`, `limit`
  - `GET /timeline?from=&to=`: compact detections for the dashboard
  - `GET /species`: every species detected so far
  - `GET /detections/{id}/audio/{client_id}`: that client's clip as WAV
  - `GET /detections/{id}/waveform[?client=]`: SVG waveform of the best (or given) clip
  - `GET /events`
  - `GET|DELETE /positioning-request`

**Known limitations:**

- No authentication on any endpoint.
- Audio clips (about 1–2 s per client per detection) are kept in the database indefinitely.
- A call that spans two chunks may be missed.
- The direction is measured from the centre of the microphone array, so it is less accurate
  when the bird is close to the array.
