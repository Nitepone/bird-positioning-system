# Over-the-air firmware updates for the ESP32 clients: research and design

Board task #15. Targets: the ESP32 boards in `firmware/platformio.ini` (`esp32`, `xiao_esp32s3`,
`xiao_esp32s3_plus`, `bps_xiao`), above all the bps XIAO board in batch mode.
Written 2026-11-12. Nothing here is implemented, and no hardware was touched.

## Summary and recommendation

**Feasible, and cheaper than it looks.** The ESP32 builds already use an OTA-capable partition table
(`default_8MB.csv`: `otadata`, `app0`, `app1`, 3264 KB each), and the Arduino core 3.x firmware
already has ESP-IDF app rollback compiled in. The bps XIAO image is 1.22 MiB, so it fits a slot
with 1.97 MiB to spare. Units already in the field can take an OTA update without a serial reflash.

**Recommendation:**

1. **Client pull**, carried by the heartbeat. The server's heartbeat reply names a target build
   (id, size, SHA-256, a signed manifest); the client fetches `firmware.bin` itself over HTTP with
   `Range` requests. Push is impossible for batch clients (radio off) and would need an
   inbound, unauthenticated listener on every unit.
2. **Integrity and authenticity at the application level:** an Ed25519 signature over a small
   manifest (env, version, serial, size, SHA-256), verified on the device against a public key
   built into the firmware, *before* `esp_ota_set_boot_partition`. Secure Boot v2 and
   `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` need a different sdkconfig than the prebuilt Arduino
   libraries have, so they are out of reach without leaving the current build setup. With the
   signature in place the plain-HTTP, unauthenticated client API is acceptable for delivery:
   a LAN attacker can neither forge nor modify an image.
3. **Rollback with the IDF mechanism that is already there.** The firmware only has to override
   the weak `verifyRollbackLater()` to return true (otherwise the Arduino core confirms every new
   image at boot) and call `esp_ota_mark_app_valid_cancel_rollback()` once the client is healthy:
   registered, clock anchored, audio running. No app-level fallback is needed, but the hardware test
   must prove it works with the prebuilt bootloader.
4. **In batch mode, stage on the SD card.** Download across contacts (resuming with `Range`), verify
   from the card, then install in one short step right after a contact, at a chunk boundary, when
   the battery allows it. Flash erase and write stall the cache and can overrun the 42.7 ms I2S
   DMA queue; doing it once, deliberately, costs a few seconds of audio rather than spreading
   dropouts over a contact.
5. **Server:** store signed builds per env, assign a target per client (then per env, with a
   canary-first workflow), record outcomes from heartbeats, show it on the Clients page.
   The API change is additive: a minor bump under #13.

## Current state

### Partition tables

| Env | Board | Partition table | Flash | OTA-capable |
|---|---|---|---|---|
| `esp32` | `esp32dev` | `default.csv` (Arduino core default; the board JSON has no `partitions` key) | 4 MB | yes, two 1280 KB slots |
| `xiao_esp32s3` | `seeed_xiao_esp32s3` | `default_8MB.csv` (board JSON `build.arduino.partitions`) | 8 MB | yes, two 3264 KB slots |
| `bps_xiao` | `seeed_xiao_esp32s3` | `default_8MB.csv` | 8 MB | yes |
| `xiao_esp32s3_plus` | `seeed_xiao_esp32_s3_plus` | `default_8MB.csv` | **16 MB** chip, table only uses 8 MB | yes |

`platformio.ini` sets no `board_build.partitions`, so each env takes its board's default.
The board JSON values come from the pioarduino platform's `boards/*.json` on GitHub (the platform is
not installed on this machine, see below). `default_8MB.csv`:

```
nvs      data nvs      0x9000    0x5000     (20 KB)
otadata  data ota      0xe000    0x2000     (8 KB)
app0     app  ota_0    0x10000   0x330000   (3264 KB)
app1     app  ota_1    0x340000  0x330000   (3264 KB)
spiffs   data spiffs   0x670000  0x180000   (1536 KB)
coredump data coredump 0x7F0000  0x10000    (64 KB)
```

`default.csv` (4 MB `esp32` env): `app0`/`app1` at `0x10000` / `0x150000`, 0x140000 (1280 KB) each,
`spiffs` 0x160000, `coredump` at `0x3F0000`. There is no `factory` partition in either table.
A unit flashed over serial boots `app0`; the first OTA goes to `app1`.

Two notes. The XIAO ESP32S3 Plus wastes half its flash. Moving it to a 16 MB table is possible but
changes the partition table, which OTA cannot do (see Risks). And nothing in `firmware/` appears to
use `spiffs` (1.5 MB); it could grow the app slots in a future table, with the same caveat.

### Measured sizes (`bps_xiao`)

**How measured:** `pio` is not installed on this machine and Docker was off limits, so I used the
newest existing build in `/home/luna/projects/bps/bps-src/firmware/.pio/build/bps_xiao/`, built
**2026-11-06 21:03**. It may lag the current `main` by a few commits (the research doc for the audio
trigger quotes 1.04 MB for an older build). I decoded `partitions.bin` with a short Python script
(32-byte entries: magic `0xAA50`, type, subtype, offset, size, name) and took sizes with `ls -l`.
Re-run `pio run -e bps_xiao` before relying on them to the byte.

| Item | Bytes | |
|---|---|---|
| `firmware.bin` (the app image, what OTA carries) | 1,278,848 | 1.22 MiB |
| `firmware.factory.bin` (bootloader + table + app, for serial flashing) | 1,344,384 | |
| `bootloader.bin` | 19,968 | |
| App slot (`app0` = `app1`) | 3,342,336 | 3264 KB = 3.19 MiB |
| Headroom per slot | 2,063,488 | 1.97 MiB, image uses 38.3% of the slot |
| Two slots | 6,684,672 | of 8,388,608 B flash; nvs + otadata + spiffs + coredump take the rest |

So two app slots fit with room to spare; the image could grow 2.6x before it stops fitting. Header
byte 3 of both bootloader and app is `0x3f`: 8 MB flash, 80 MHz. The image is 1.28 MB, so a full
download is 1.28 MB of radio-on traffic.

### What the repository already has

- The firmware uses raw sockets (`port_tcp_*`) and its own small HTTP client (`txn` in
  `client.c`/`upload.c`), not Arduino's `HTTPClient`. Chunked upload, timeouts and the contact
  state machine already exist.
- The heartbeat reply is `{"mode": {...}}` (`HeartbeatResponse` in `crates/bsp-proto/src/lib.rs`).
  Older servers answer with no body, and clients tolerate that, so a new optional field is
  compatible both ways.
- A batch contact runs 6-10 s and is capped at `CONTACT_MAX_US` = 120 s; the connect phase at 30 s.
- Capture runs in a task pinned to core 0, priority 10; the I2S receive callback is `IRAM_ATTR`
  and the DMA queue holds 42.7 ms (comment in `esp32.cpp`). The ring buffer sits on the heap
  (PSRAM on the XIAO boards, `RING_KB` 1024), and the SD queue is 256 KB of PSRAM.

## Delivery

### Pull versus push

| | Pull (client fetches) | Push (server sends) |
|---|---|---|
| Batch clients | Works: they contact anyway, the radio is already on | Impossible: radio off for 30 s-1 h; the server cannot reach them |
| Live clients | Works | Needs an HTTP listener on every unit |
| Attack surface | None added to the unit; the server is only asked | An open, unauthenticated write-firmware port on the LAN |
| NAT/firewall | Outbound only | Server must reach units |
| Fits "clients are reached only via their own contacts" | Yes | No |

Pull wins on every row. The design: the heartbeat reply carries an optional `firmware` offer; the
client decides when to act on it (battery, mode, time budget), downloads, verifies, installs, and
reports progress in later heartbeats.

### Download mechanics

- **Own HTTP client, not `HTTPUpdate`.** The client already has sockets, timeouts, a contact
  clock and logging. A new `port_ota_*` layer over `esp_ota_ops` (begin, write, end, set boot,
  restart) keeps the portable core free of IDF headers and testable natively. Arduino's `HTTPUpdate`
  and `Update` check only an MD5 on the whole stream (as I understand them), and `esp_https_ota`
  wants to own the connection; neither gives `Range` resume across radio-off periods or the
  contact budget. (I did not verify every feature of `Update`'s optional signature support;
  see open questions.)
- **Range resume.** `GET /api/v1/client/firmware/{build_id}` with `Range: bytes=N-`, answered 206.
  Build ids are content-addressed (the SHA-256 prefix), so the bytes at an id never change, and
  resuming against it is safe. The client keeps `N` and the build id in the `.part` file name.
- **Batch mode: stage on the card.** The client writes `ota/<build_id>.part` on the SD card. A
  contact downloads as much as its OTA budget allows (say 45 s of radio, after the audio upload is
  done and only if the battery gate passes) and stops cleanly; the next contact resumes. When the
  file is complete, the client hashes it from the card (1.3 MB, about a second; avoids saving
  hash state across contacts), checks the signature, then installs from the card. A card
  that goes away mid-download just restarts the download.
- **Live mode and cards-less boards (e.g. `esp32`).** Stream straight into the inactive slot with a
  running SHA-256, verify at the end, resume with `esp_ota_resume`-style offsets within the same
  boot only; if the connection drops, abort and start again (1.3 MB). Using `OTA_WITH_SEQUENTIAL_WRITES`
  makes the erase incremental instead of erasing all 3.2 MB at `esp_ota_begin` with
  `OTA_SIZE_UNKNOWN` (see the ESP-IDF OTA docs). Pass the real image size when it is known.
- **Radio-on budget (estimates, to measure in the PoC).** Wi-Fi TCP on an ESP32-S3 in station mode
  typically delivers 200-600 KB/s, so 1.28 MB is 2-7 s of transfer. With one extra contact of
  10-20 s, the energy cost (receive at about 100 mA) is a fraction of one mAh. This is small next
  to the audio uploads. A per-contact OTA budget bounds the worst case; most contacts after the
  first finish the job.
- **Order inside a contact:** register, clock anchors, upload the spool, *then* the OTA download.
  Updating never delays the audio or the clock; it only lengthens contacts that have time.
- **Delivery of the manifest** is in the heartbeat reply (a few hundred bytes), so the decision to
  start a download needs no extra request.

## Integrity and authentication

### Threat model

The API is plain HTTP on port 2473 with no authentication. Anyone on the LAN can send a client a
forged heartbeat reply (spoofing the server, or just racing it), or can talk to the server's
control endpoints (served on the web ports, also unauthenticated). The units are outdoors, battery
powered, and run unattended; a bad image means a trip to every unit. The threats, in order of
likelihood: (a) a wrong image (wrong board, truncated download, corrupted write); (b) a stale
but genuine image replayed to downgrade; (c) a LAN attacker serving malicious firmware.

### Layers

1. **Transport integrity (a):** SHA-256 of the whole image, computed while writing. The IDF also
   checks the image header, chip id, segment checksums and the app's own SHA-256 in `esp_ota_end`,
   which catches truncation and corruption; a wrong chip family is rejected by the chip id in
   the image header. The manifest's `env` field covers same-chip mixups (`esp32s3` XIAO versus
   a future S3 board).
2. **Authenticity (c): an Ed25519 signature over a manifest.** The text manifest (served and signed
   verbatim, so there is no JSON canonicalisation problem):

   ```
   bps-fw-v1
   env=bps_xiao
   version=v0.4.0-12-g2ebaedd
   serial=412
   size=1278848
   sha256=<64 hex>
   min_api=1.4.0
   ```

   The public key is compiled in (`OTA_PUBKEY=` in `bps.conf`, two slots so a key can be rotated).
   The private key stays on the developer's machine; the server never holds it, so a compromised
   server or a LAN attacker cannot mint firmware. The client verifies: signature over the manifest,
   `env` equals its own, `sha256`/`size` equal what it downloaded, and (b) `serial` is higher than
   its own (the build counter, e.g. `git rev-list --count HEAD`; the same idea as ESP-IDF's
   anti-rollback `secure_version`, without burning eFuses). A signed manifest that sets
   `downgrade=1` is the deliberate exception, for when the operator really wants an older
   build.
   Ed25519 needs a library: mbedTLS in IDF 5 has no Ed25519, so I suggest vendoring Monocypher
   (public domain / BSD-2, one `.c` file) into `bps_core`. That keeps it portable to the Picos,
   and unit-testable natively with RFC 8032 vectors. **Alternative:** ECDSA P-256 with mbedTLS
   (`mbedtls_pk_verify`), which is already linked into the Arduino build (hardware-accelerated
   SHA too), at the cost of a larger key/signature and no native-test path without mbedTLS.
   Verification takes well under a second on the S3 either way (estimate).
3. **Transport confidentiality:** not needed; the firmware is open source. TLS would also need
   certificate handling on the client; skip it.

### Why not the IDF mechanisms

| Option | What it gives | Why not now |
|---|---|---|
| Secure Boot v2 (RSA-3072 on ESP32-S3) | Bootloader verifies every boot; key digest in eFuse | Burns eFuses (irreversible; keys can only be revoked, not recovered), needs a rebuilt, signed bootloader and a sdkconfig that the prebuilt Arduino libs do not have |
| `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT` | The *running* app verifies a signature block when an OTA image is written, using the public key in its own signature block; no eFuses | Same sdkconfig problem. Also the first, serially-flashed app must itself be signed, and the docs state it does not stop an attacker with flash write access |
| App-level Ed25519/ECDSA (recommended) | Same protection against the LAN threat, same limits against physical attackers | Needs a vendored verifier and a manifest format; fully under our control |

**Is it usable from Arduino-as-PlatformIO-framework?** Not with the setup in `platformio.ini`.
`framework = arduino` links the *precompiled* Arduino libraries built by `esp32-arduino-lib-builder`
from a fixed `defconfig`, so the Kconfig options behind signing and secure boot are baked in. To
change them you must either (1) use pioarduino's `custom_sdkconfig` (hybrid compile), which
rebuilds the Arduino libraries from source against your overrides; this needs the full ESP-IDF
toolchain, slows every build, breaks the Docker image's "everything pre-installed, offline" design,
and has had bugs on some platform releases; or (2) switch to `framework = arduino, espidf`
with Arduino as an IDF component and our own `sdkconfig.defaults`. Both are large build-system
changes for no gain over the app-level signature. Revisit only if units become physically
exposed to a capable attacker.

### Minimum to stop a LAN attacker pushing firmware

Because the client pulls and verifies, the answer is two small things:

1. **The on-device signature check above.** With it, an attacker who spoofs the server, tampers
   with the download or uploads their own build to the server still cannot get an image installed.
   Without it, the unauthenticated heartbeat reply is a remote-code-execution channel, so **the
   signature check is a hard prerequisite for shipping OTA at all**, and no server work should go live without it.
2. **An admin token on the firmware control endpoints** (`Authorization: Bearer ...`, from a
   server setting or environment variable; `GET` stays open, writes need it). It stops a casual
   LAN user from uploading builds and reassigning clients (a denial-of-service route, since a
   forged-but-signed-by-nobody build is only rejected on the device after wasting radio time
   and battery). It is not a general fix for the "no authentication on any endpoint" limitation in the
   README; that is a separate ticket.

Residual risks: a replayed *genuine, signed* older image (stopped by `serial`); a LAN attacker
flooding bogus offers to drain batteries (bounded by checking the manifest signature, which is
cheap, *before* downloading anything: a bad manifest costs no radio time beyond the heartbeat
reply); an attacker with physical access, who can reflash anyway.

## Rollback

### Is IDF rollback available in the Arduino core 3.x build? Yes, verified three ways

- `esp32-arduino-lib-builder`'s `configs/defconfig.common` contains
  `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`, the shared config for the precompiled libraries and the
  prebuilt bootloader (read on GitHub; the platform is not installed here to read the generated
  `sdkconfig.h`).
- The Arduino core's `initArduino()` (`esp32-hal-misc.c`) has a `CONFIG_APP_ROLLBACK_ENABLE` block:
  unless the weak `verifyRollbackLater()` returns true, it checks the running slot's state and, if
  it is `ESP_OTA_IMG_PENDING_VERIFY`, calls the weak `verifyOta()` (default: true) and then
  `esp_ota_mark_app_valid_cancel_rollback()`; on false it calls
  `esp_ota_mark_app_invalid_rollback_and_reboot()`.
- The existing `firmware.elf` from the 2026-11-06 build contains the strings `OTA verification
  failed! Start rollback to the previous version ...` and `ESP_OTA_IMG_PENDING_VERIFY`, and the
  symbols `verifyRollbackLater` and `esp_ota_mark_app_valid_cancel_rollback`. Those exist only when
  the app side was compiled with `CONFIG_APP_ROLLBACK_ENABLE`.

**Not verified:** that the *bootloader binary on the deployed units* honours it. It is built from
the same config, but the real proof is the hardware test below (flash a deliberately broken OTA
image; it must come back on the old slot).

### The catch

By default the Arduino core calls `verifyOta()` -> true at boot, so every new image is confirmed
*immediately*, before our code has run a single check. To keep the rollback window open, the
firmware must override:

```c
extern "C" bool verifyRollbackLater(void) { return true; }   // keep the image PENDING_VERIFY
```

and later call `esp_ota_mark_app_valid_cancel_rollback()` itself. Any reset before that (panic,
watchdog, brown-out, power loss) makes the bootloader mark the image `ABORTED` and boot the
previous slot. That gives crash-loop protection for free, with no boot counter. An explicit
`esp_ota_mark_app_invalid_rollback_and_reboot()` handles the image that boots but is not healthy.

### "Healthy" for this project

Confirm the new image only when **all** of these hold, measured from boot:

1. It reached `ST_RUN`: Wi-Fi joined and `POST /register` succeeded.
2. A heartbeat got a 200 reply (the server saw the new version, which doubles as the outcome report).
3. The clock has an anchor (a timesync measurement completed, the existing `SYNCED` condition).
4. Audio is flowing: the I2S capture task has delivered samples for the last N seconds with no
   stall; on the bps XIAO the PDM mic produces non-trivial samples.
5. If a `sd` device is configured and the card was present, the card initialised.
6. It has stayed up for at least 60 s (not counting the contact's radio-off time).

In **batch mode** this is evaluated in the first contact after the update (the contact starts
right after boot, since the client has to register). If not healthy by `CONTACT_MAX_US` + margin
(say 5 min of uptime), call `esp_ota_mark_app_invalid_rollback_and_reboot()`. A real
caveat: with the radio off between contacts, a *power loss* before the first contact completes
also rolls back (safe: the old image runs, and the update is retried; but the server must not loop
forever, see below).

After a rollback, the old image reports `ota_state: rolled_back` with the build id that failed;
the server then stops offering that build to that client (and halts an env-wide rollout) until the
operator re-assigns. Without that, the client would download the same bad image again at every
contact.

An app-level fallback (NVS boot counter, manual `esp_ota_set_boot_partition` back) is only needed if
the hardware test shows the bootloader does not roll back. The design does not depend on it.

## Safety

| Hazard | What happens | Mitigation |
|---|---|---|
| **Low battery** | A flash write at brownout can corrupt the slot being written, and a dying cell can reset mid-install | Gate on the MAX17048 reading in the heartbeat: start a download only at >= 40% and installation at >= 40% (tunable). Cells that are charging (positive rate) or a board with no gauge reading (external power) pass. Re-check right before install. A/B slots mean the running image is never touched, and `otadata` is written last, so a power loss costs only the partial download |
| **Audio in progress** | `esp_restart()` after install drops the ring (up to ~10 s of audio) and the reboot plus Wi-Fi takes 5-15 s | Install right after a chunk boundary (chunk saved/uploaded), in batch mode right after the contact's last heartbeat and with the spool queue empty. Accept a gap of one chunk at most, and record it (`reboot_reason: ota`, #14) so it is not read as a fault |
| **SD writes** | A reset during a write leaves a partial chunk or an unflushed FAT | Wait for the `sd_task` queue to drain, then `port_sd_close()` (flush) before `esp_restart()`. Partial chunks have no `.json`, which the spool already treats as incomplete, and it uploads them on the next boot |
| **Flash cache vs capture** | During flash erase/program the cache is disabled; code and data in flash and PSRAM are inaccessible on *both* cores unless in IRAM/internal RAM. The `capture_task` (reads PSRAM-backed ring) stalls; I2S DMA keeps filling its internal buffers, but the queue holds only 42.7 ms. A sector erase typically takes tens of ms (up to 400 ms worst case in many flash datasheets), a 64 KB block erase longer | Prefer the install-from-card step at a chosen moment: erase and write ~1.3 MB takes, by my estimate, 3-15 s, during which audio is lost anyway, so pause capture explicitly, mark the gap, and install in one go. For streaming installs on live boards, accept `capture_dropouts` (already reported) and keep `esp_ota_begin` erase incremental. The I2S callback is `IRAM_ATTR`, but whether the driver's ISR itself is IRAM-safe (`CONFIG_I2S_ISR_IRAM_SAFE`) is **not verified**; assume it is not, so the interrupt is deferred until the flash op ends |
| **Download resume** | Connection drops, contact budget ends, Wi-Fi flaps | `Range` resume on the card (batch); restart on cardless boards; the signed manifest and SHA-256 make a resumed file safe to trust |
| **Update loop** | A bad build is re-offered at every contact | Server stops offering a build to a client after `rolled_back`/repeated `failed`; client also remembers the last failed build id in NVS |
| **Wrong build** | Image for another board | `env` in the signed manifest; chip id in the image header |

During the update the client keeps reporting heartbeats (live mode) so the operator sees progress.
Do not start while the unit is in the middle of a positioning request.

## Server-side management

The server is Rust/axum with SQLite (`rusqlite`); the client API is in `crates/bsp-server/src/api/client.rs`.

- **Storage:** `<data dir>/firmware/<build_id>/{firmware.bin, manifest.txt, manifest.sig}`, where
  `build_id` is the first 16 hex of the SHA-256. SQLite table `firmware_builds(build_id, env,
  version, serial, size, sha256, created_ns, notes)`. Upload is `POST /api/v1/control/firmware`
  (multipart: bin, manifest, sig). The server checks that the SHA-256 and size in the manifest match
  the file and that the manifest parses; it does not need the private key and, if given the public
  key, may refuse an unsigned build to catch mistakes early.
- **Assignment:** `firmware_target(client_id, build_id NULL)` for a client, and `firmware_default(env,
  build_id NULL)` for every client of an env without its own target. A client's own target wins.
  Clients report their `env` at registration (#13 supplies the version fields; if `env` is not
  part of it, add it). The fleet is small, so **staged rollout is a workflow**: assign one client
  (the canary), watch it confirm, then set the env default. Rolling out to a percentage by
  `hash(client_id)` is a small extension if wanted. If any client reports `rolled_back` for a build,
  the server stops offering it to every client (an automatic halt) and flags it in the UI.
- **Offer logic in the heartbeat:** include the `firmware` object only when the target build differs
  from the reported running build, the build's `env` matches, the client reports a `min_api` it
  meets, and the build has not already failed for this client. Batch clients get it at their next
  contact. For batch clients, hold the offer until the heartbeat says `radio_on` (that is,
  within a contact).
- **Outcome reporting:** the client's heartbeat carries `firmware: {version, build_id, serial,
  state, progress_bytes, error}`, with `state` one of `idle`, `downloading`, `staged`,
  `installing`, `rolled_back`, `failed`. The server turns state changes into events (the existing
  `state.event` mechanism: `firmware_started`, `firmware_installed`, `firmware_rolled_back`,
  `firmware_failed`) and keeps `firmware_history(client_id, build_id, state, at_ns, detail)`.
  "Installed" is when the *new* version appears in a heartbeat after the client confirmed.
- **UI:** the Clients page gains a firmware column (version, with an amber badge if it differs from
  the target) and a per-client "Update firmware" selector next to *Operation mode*, with the
  download progress. The Config sidebar (Server and Clients sections) gets a **Firmware** section: upload
  a build, list builds per env with their serial, set an env default, see rollout status per
  client and the history. A client that has failed shows why and offers "retry".
- **Interplay with #14 (commands):** firmware is **desired state, not a command**: it persists until
  the client matches it, and is idempotent. Do not model it as a one-shot command. But reuse #14's
  reboot-reason reporting: `ota_installed`, `ota_rollback`; if #14's heartbeat reply has a
  command list with ids and acks, `install_now` may be added later as a command, for letting the
  operator choose the moment.

## Protocol changes

All additive and optional, so a **minor** bump of the semver API version (#13); older clients ignore
the new reply field and older servers never send it.

Heartbeat request (client to server), new optional object:

```json
"firmware": {
  "version": "v0.4.0-12-g2ebaedd", "serial": 412, "build_id": "9f3a...", "env": "bps_xiao",
  "slot": "app0", "state": "downloading", "progress_bytes": 524288, "total_bytes": 1278848,
  "error": null
}
```

(`version` and the API version are #13's registration fields; the heartbeat repeats the part that
changes: `state`, `progress_bytes`, `error`.)

Heartbeat reply, new optional field next to `mode`:

```json
{
  "mode": {"mode": "batch", "interval_s": 300},
  "firmware": {
    "build_id": "9f3a1c20e4b7d855",
    "url": "/api/v1/client/firmware/9f3a1c20e4b7d855",
    "manifest": "bps-fw-v1\nenv=bps_xiao\nversion=...\nserial=412\nsize=1278848\nsha256=...\nmin_api=1.4.0\n",
    "sig": "<base64 Ed25519 over the manifest>",
    "min_battery_pct": 40
  }
}
```

New endpoints:

| Endpoint | Purpose |
|---|---|
| `GET /api/v1/client/firmware/{build_id}` (client port 2473 and web ports) | Serves `firmware.bin`, with `Range`, `Content-Length`, `ETag: "<sha256>"` |
| `POST /api/v1/control/firmware` | Upload a build |
| `GET /api/v1/control/firmware` | List builds with per-client status |
| `PUT /api/v1/control/clients/{id}/firmware` | `{"build_id": "..." \| null}`, the client's target |
| `PUT /api/v1/control/firmware/default/{env}` | The env's default target |

Control writes need the admin token (see above). The registration response should also tell the client the
server supports OTA (`"ota": true`) so a new client does not misread an old server's empty reply.

## Code sketches (not wired into the firmware)

### Client-side install routine (ESP32 port, ESP-IDF 5 API as used by Arduino core 3.x)

```cpp
// firmware/src/port/ota_esp32.cpp (sketch)
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>

// Keep the new image PENDING_VERIFY: the Arduino core confirms it at boot otherwise.
extern "C" bool verifyRollbackLater(void) { return true; }

struct ota_ctx {
    const esp_partition_t *part;
    esp_ota_handle_t h;
    mbedtls_sha256_context sha;
    size_t written, total;
};

extern "C" bool port_ota_begin(ota_ctx *c, size_t total) {
    c->part = esp_ota_get_next_update_partition(nullptr);    // the slot we are NOT running
    if (!c->part || total > c->part->size) return false;
    // Known size: erases only what is needed, not all 3.2 MB; sequential writes
    // allow the erase to be incremental.
    if (esp_ota_begin(c->part, total, &c->h) != ESP_OK) return false;
    mbedtls_sha256_init(&c->sha);
    mbedtls_sha256_starts(&c->sha, 0);
    c->written = 0; c->total = total;
    return true;
}

extern "C" bool port_ota_write(ota_ctx *c, const void *p, size_t n) {
    if (esp_ota_write(c->h, p, n) != ESP_OK) { esp_ota_abort(c->h); return false; }
    mbedtls_sha256_update(&c->sha, (const uint8_t *)p, n);
    c->written += n;
    return true;
}

// Called only after the signature over the manifest checked out and sha256 == manifest's.
extern "C" bool port_ota_commit(ota_ctx *c, const uint8_t want_sha[32]) {
    uint8_t got[32];
    mbedtls_sha256_finish(&c->sha, got);
    if (c->written != c->total || memcmp(got, want_sha, 32) != 0) { esp_ota_abort(c->h); return false; }
    if (esp_ota_end(c->h) != ESP_OK) return false;            // IDF image checks
    return esp_ota_set_boot_partition(c->part) == ESP_OK;     // writes otadata: last step
}
// Then: flush the SD queue, port_sd_close(), esp_restart().

// At boot, once the project's "healthy" conditions hold (client.c calls this):
extern "C" void port_ota_confirm(void) {
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY)
        esp_ota_mark_app_valid_cancel_rollback();
}
extern "C" void port_ota_reject(void) { esp_ota_mark_app_invalid_rollback_and_reboot(); }
```

The portable part (in `client.c`) is a small state machine: *offer seen* -> battery/budget gates ->
`GET` with `Range` (stage to card or stream) -> hash -> `ed25519_verify(manifest, sig, PUBKEY)` ->
parse manifest, compare `env`/`serial`/`size`/`sha256` -> `port_ota_commit` -> restart. The verify
step runs on the manifest *before the first byte of the image is requested*, so a forged offer
costs nothing.

### Server endpoint shape (axum 0.8)

```rust
// crates/bsp-server/src/api/client.rs (sketch)
.route("/firmware/{build_id}", get(firmware_bin))

async fn firmware_bin(
    State(state): State<SharedState>,
    Path(build_id): Path<String>,
    headers: HeaderMap,
) -> ApiResult<Response> {
    let b = state.db.firmware_build(&build_id)?.ok_or_else(|| ApiError::not_found("build"))?;
    let path = state.firmware_dir.join(&b.build_id).join("firmware.bin");
    // tower-http's ServeFile answers Range (206) and sets Content-Length itself.
    let mut resp = ServeFile::new(path).oneshot(request_with(headers)).await?.into_response();
    resp.headers_mut().insert(ETAG, format!("\"{}\"", b.sha256).parse()?);
    Ok(resp)
}
// In heartbeat(): after computing `after`, resolve the client's target build, and
// HeartbeatResponse { mode: after.mode, firmware: offer_for(&after, &hb) }
// with #[serde(skip_serializing_if = "Option::is_none")] on `firmware`.
```

(`HeartbeatResponse` is `Copy` today; an owned manifest string means dropping `Copy`.)

## Hardware PoC test plan

Run on one bps XIAO with a battery and a card, serial cable attached for the log. Server is a
local build on a laptop. Do each step on a bench unit, not a deployed one.

**Prep**

1. Build the baseline from `main` with `./build.sh` (`bps_xiao`). Note `firmware.bin` size and the
   serial boot log's partition line (`app0 ... 0x330000`). Flash it over USB (`-f`).
2. Build image B: same code with a visibly different version string (and, once #13 lands, its own
   version). Build image C: broken, with `abort()` right after `initArduino()` (or a build whose
   health check can never pass).
3. Generate an Ed25519 key pair with the signing script (task 2), build the baseline with the
   public key, and sign manifests for B and C. For step 6, sign a third manifest with a *different*
   key, and a fourth with `serial` lower than the baseline.
4. Start a local server and put `firmware.bin` for B and C behind the new endpoint
   (a static file server with Range support is enough: `python3 -m http.server` does *not* do Range,
   use `caddy file-server` or `miniserve`).

**Tests** (record the result of each in the ticket)

| # | Test | Expected |
|---|---|---|
| 1 | Live mode. Offer B. Watch the log | Download, hash ok, signature ok, reboot, new version registers, `mark_app_valid` logged, server shows B. Time the whole thing |
| 2 | Measure: capture dropouts (`capture_dropouts` in the heartbeat, dropouts line in the log) during step 1's flash write; time `esp_ota_begin` and the write loop | Number of dropouts and the longest stall. Compare against the 42.7 ms DMA queue. Repeat with sequential-writes erase on |
| 3 | Offer C (broken) | Boots C, resets or fails health, bootloader returns to B within one reset; log shows `ota_state: rolled_back`; server stops offering C |
| 4 | Offer C2: boots fine but never registers (point it at a wrong server port) | After the health deadline the unit calls `mark_invalid_rollback_and_reboot` and returns to B |
| 5 | Pull the power 10 s into the download, then 10 s into the install (use a switchable supply) | Unit boots B each time; no corruption; resumes or restarts the download |
| 6 | Offer manifests signed by the wrong key; with a modified `firmware.bin`; with lower `serial`; with the wrong `env` | Each rejected, logged, no radio time beyond the heartbeat reply, B keeps running |
| 7 | Batch mode, 60 s interval, card in. Offer B | Staging file appears on the card, grows over several contacts if the budget is cut, resumes with `Range`, install after a contact, spool chunks intact and uploaded after reboot. Kill Wi-Fi (turn off the AP) mid-download: next contact resumes |
| 8 | Battery gate: with a gauge reading under the threshold (a bench supply can fake it, or lower the threshold to just above the current charge) | No download starts; heartbeat reports why |
| 9 | Measure the radio-on time and extra energy of the update contact (a USB power meter or the gauge's percent change over many updates) | Seconds and mAh per update, to confirm the estimates |
| 10 | Verify the rollback-enabled bootloader on a *fielded-style* unit: flash the baseline from the old factory image (or an old release), *then* OTA | The same result as step 3; i.e. the old bootloader supports it. If not, this changes the plan (serial reflash of bootloaders) |

Exit criteria: steps 1, 3, 5, 6 and 7 pass, with step 2's dropout numbers recorded.

## Risks and open questions

- **Bootloader and partition table are not OTA-updatable.** The OTA image is the app only. If the
  bootloader (it is the prebuilt one from the pioarduino libs) or the table must change, those units need
  a serial reflash, so the table must stay stable. Do not let the 16 MB Plus table work
  slip in unnoticed.
- **Deployed bootloaders.** Unknown whether every deployed unit has a rollback-enabled
  bootloader (test 10). Units flashed long ago with older pioarduino releases may differ.
- **Flash-op stalls vs the 42.7 ms DMA queue:** not measured; test 2. If dropouts are bad, pause
  capture explicitly during install (planned for the card-staged path).
- **I2S ISR IRAM safety** (`CONFIG_I2S_ISR_IRAM_SAFE`) and `SPI_FLASH_AUTO_SUSPEND` in the
  prebuilt libs: I did not read the generated `sdkconfig` (the platform packages are not installed
  here). Check in the Docker image's `/opt/platformio/packages/framework-arduinoespressif32-libs/esp32s3/`.
- **Arduino `Update`'s own signature support** (`Updater_Signing.h`, RSA) was not evaluated; it would
  be an alternative to a vendored Ed25519 but ties us to Arduino's `Update` class.
- **Key management:** a lost or leaked signing key means re-flashing every unit by cable to change the
  embedded key. Two key slots (current and next) from day one soften this. Keep the private key off the
  server and off the CI machine.
- **Version/serial source:** depends on #13's git-describe. The build must stamp `serial`, `version`
  and `env` into both the image (`esp_app_desc_t`) and the manifest.
- **Unit tests:** Ed25519 and manifest parsing belong in `bps_core` with native Unity tests (the
  `native` env), so they run without hardware.
- **Wi-Fi and unmeasured numbers:** throughput (200-600 KB/s) and install time (3-15 s) are
  estimates, not measurements.
- **Clients without a card (plain `esp32`):** no staging, restart-on-failure only. Acceptable; they
  are live-mode bench units.
- **Pico W / Pico 2 W** are out of scope, see below.

## Pico W / Pico 2 W (later)

Feasible but a separate project. arduino-pico has an OTA path: the new image is written to a
file on LittleFS and `picoOTA` (`PicoOTA.begin()`, `addFile()`, `commit()`) arranges for the
bootloader to copy it on the next reboot; it needs a filesystem partition big enough for the
whole image (flash minus the app, and the Pico W has 2 MB and the Pico 2 W 4 MB), and it supports
signed images (arduino-pico's `Updater` can verify with a public key). The Ed25519 manifest and the pull
protocol above would carry over unchanged because they live in `bps_core`; only the `port_ota_*`
layer differs. (From my knowledge of arduino-pico; not verified for this document.)

## Proposed follow-up implementation tasks

**1. ESP32 OTA port layer and rollback confirmation**
Add `port_ota_*` (begin/write/commit/confirm/reject) over `esp_ota_ops`, override `verifyRollbackLater()`, and confirm the image once the client is healthy.
- A new image stays PENDING_VERIFY until registered, clock-anchored and capturing audio for 60 s
- An unhealthy or crashing new image returns to the previous slot without help from a person
- A host-side or log test shows `esp_ota_mark_app_valid_cancel_rollback` runs only after the health check

**2. Signed manifest verification in bps_core**
Vendor Monocypher (Ed25519) and SHA-256 helpers in `bps_core`; parse the `bps-fw-v1` manifest; check signature, env, serial and hash; compile-time key slots in `bps.conf`.
- Native Unity tests with RFC 8032 vectors pass; tampered manifest, wrong key, wrong env and lower serial are rejected
- The public key (two slots) is set in `bps.conf`/`bps_config.py` and rejected at build time if missing for OTA builds
- A verify costs under one second on the S3 (measured)

**3. Client OTA download and install state machine**
In `client.c`, act on the heartbeat `firmware` offer: gates (battery, contact budget), Range download to the inactive slot (live) or an SD staging file (batch), hash, verify, install, restart at a safe moment.
- A download interrupted by a contact ending resumes with `Range` at the next contact in batch mode
- Install runs only after a chunk boundary, with the SD queue drained and closed
- A bad offer (bad signature, wrong env, lower serial) costs no image download

**4. Server: firmware store, serving and assignment**
Add `firmware_builds` and targets tables, upload and list endpoints, `GET /api/v1/client/firmware/{build_id}` with Range, and the optional `firmware` field in the heartbeat reply. Minor API bump per #13.
- A build uploaded with a mismatched hash/size is refused; a good one is served with correct 206 responses
- A client with a target differing from its running build gets the offer; one that rolled back does not get it again
- Old clients and old servers still interoperate (the field is optional)

**5. Outcome reporting and events**
Heartbeat `firmware` state object (downloading/staged/installing/rolled_back/failed, progress), server events and history, automatic halt of an env rollout after a rollback. Uses #14's reboot reasons.
- State changes appear in the events feed and in a per-client history
- One `rolled_back` report stops the build being offered to other clients
- Reboot reasons `ota_installed`/`ota_rollback` are reported (with #14)

**6. Web UI: Firmware section and Clients page**
Config sidebar "Firmware" (upload a build, list per env, set the env default) and Clients page columns/selector (running version, target, progress, failure reason, retry).
- An operator can upload a build, assign it to one client and see the progress and the final outcome
- A client whose version differs from its target is visibly flagged
- Works on mobile widths like the rest of the Config pages

**7. Build and signing tooling**
A PlatformIO post-build script emitting `firmware-<env>-<version>.bin` plus manifest, a signing script (`scripts/sign_firmware.py`) and docs for key generation, storage and rotation; stamps version/serial/env (from #13).
- `./build.sh` produces a signed-ready artefact set for any ESP32 env
- Signing is offline, and the private key is never read by the server
- Wrong-env or stale-serial manifests are caught by the script

**8. Admin token for firmware control endpoints**
Require `Authorization: Bearer <token>` (from a server setting) on firmware upload, assignment and default changes; reads stay open; the UI asks for the token.
- Writes without the token get 401; reads and all existing endpoints are unchanged
- The token is not logged and not shown in `/events`
- The README's limitations list says exactly what is and is not covered

**9. Hardware validation (the PoC)**
Run the test plan above on a bps XIAO, record dropouts during flash writes, install timing, power-cut results and the rollback test on a unit with an old bootloader. Decide if capture must pause during install.
- Tests 1, 3, 5, 6, 7 pass and are logged on the ticket
- Dropout count and longest stall measured; the pause-capture decision is made
- Test 10 shows whether old bootloaders roll back

(Nine tasks; if the board wants eight, merge 5 into 4, or 8 into 4.)

## Sources

- ESP-IDF OTA API and app rollback (states, `esp_ota_mark_app_valid_cancel_rollback`, `OTA_SIZE_UNKNOWN`, `esp_ota_resume`, anti-rollback): https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/api-reference/system/ota.html
- ESP-IDF Secure Boot v2 (RSA-PSS, `CONFIG_SECURE_SIGNED_APPS_NO_SECURE_BOOT`, eFuse key digests, revocation): https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32s3/security/secure-boot-v2.html
- arduino-esp32 `esp32-hal-misc.c` (`initArduino`, `verifyRollbackLater`, `verifyOta`, `CONFIG_APP_ROLLBACK_ENABLE`): https://raw.githubusercontent.com/espressif/arduino-esp32/master/cores/esp32/esp32-hal-misc.c
- esp32-arduino-lib-builder `configs/defconfig.common` (`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`): https://raw.githubusercontent.com/espressif/esp32-arduino-lib-builder/master/configs/defconfig.common
- arduino-esp32 partition tables: https://raw.githubusercontent.com/espressif/arduino-esp32/master/tools/partitions/default_8MB.csv and https://raw.githubusercontent.com/espressif/arduino-esp32/master/tools/partitions/default.csv
- pioarduino board definitions: https://raw.githubusercontent.com/pioarduino/platform-espressif32/develop/boards/seeed_xiao_esp32s3.json , https://raw.githubusercontent.com/pioarduino/platform-espressif32/develop/boards/seeed_xiao_esp32_s3_plus.json , https://raw.githubusercontent.com/pioarduino/platform-espressif32/develop/boards/esp32dev.json
- pioarduino platform (hybrid compile, `custom_sdkconfig`): https://github.com/pioarduino/platform-espressif32 ; known issue threads: https://github.com/pioarduino/platform-espressif32/issues/335 and https://github.com/pioarduino/platform-espressif32/pull/353 ; Tasmota's README describing `custom_sdkconfig`: https://github.com/tasmota/platform-espressif32
- Forum discussion that sdkconfig options are baked into the Arduino libs: https://esp32.com/viewtopic.php?p=74216
- Local evidence: `firmware/.pio/build/bps_xiao/{firmware.bin,partitions.bin,bootloader.bin,firmware.elf,firmware.map}` (built 2026-11-06), `firmware/platformio.ini`, `firmware/lib/bps_core/src/client.c`, `firmware/src/port/esp32.cpp`, `crates/bsp-server/src/api/client.rs`, `crates/bsp-proto/src/lib.rs`, `docs/audio-trigger-research.md`.
