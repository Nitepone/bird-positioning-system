# On-device bird-call trigger for the ESP32-S3: research

Board task #12. Target: the bps XIAO board (XIAO ESP32-S3, 8 MB flash, 8 MB PSRAM, IM73D122 PDM mic).
Written 2026-11-05.

## Summary

**Feasible.** A small int8 CNN on a log-mel spectrogram can decide "bird / no bird" for the look-back
window. It needs about 10–60 KB of flash for weights, 20–100 KB of RAM and about 25–150 ms of CPU per
3 s window at 160 MHz. Published ESP32-S3 birdsong detectors fall inside these numbers
(BioDCASE 2025: 2–17 KB models, 1–30 KB peak RAM, 17–200 ms per clip). BirdNET itself does not fit.

**Recommendation:** keep the existing energy gate as stage 1 and lower its threshold. Add a tiny CNN
as stage 2, using TensorFlow Lite Micro with ESP-NN. The CNN runs only when the gate opens, and it
decides whether that upload happens. Train it on clips from our own sites, labeled by the server's
BirdNET results.

## What we have now

`firmware/lib/bps_core/src/gate.c` is a band-energy gate rather than a raw peak detector. It
band-passes the signal to 1–10 kHz and measures RMS in 100 ms frames. A frame passes when it is 10 dB
or more above an adaptive noise floor. When the gate opens, an upload starts 2.5 s back
(`LOOKBACK_MS=2500`). The audio is 48 kHz int16, in a 1 MB PSRAM ring of about 10.9 s.

The gate costs almost nothing, but it measures loudness, not "birdness":

- **False triggers:** any broadband sound in 1–10 kHz opens it, such as wind gusts, rain, insects,
  voices, cars, aircraft or handling noise. Each one costs an upload (Wi-Fi TX) and server-side
  BirdNET time.
- **Misses:** distant or faint calls less than 10 dB above the floor never open it. Lowering the
  threshold today simply means more false triggers.

## Candidate approaches

| # | Approach | Flash | RAM | CPU per 3 s window @160 MHz | Notes |
|---|---|---|---|---|---|
| 0 | Current energy gate | ~0 | <1 KB | <1 ms | Baseline |
| A | Better DSP features: spectral flatness/tonality, band-peak persistence, frequency modulation | ~2 KB code | ~4 KB (one FFT frame) | ~3–5 ms (512-pt FFTs, esp-dsp) | No training. Rejects broadband noise (wind, rain), but not tonal non-birds (insects, squeaks) |
| B | **Tiny CNN on log-mel**, int8, TFLM + ESP-NN (recommended) | 10–60 KB weights + ~150–250 KB TFLM runtime | 20–100 KB tensor arena + ~6 KB features | ~25–150 ms (est.) | Matches BioDCASE 2025 entries; binary bird / not-bird |
| C | Larger DS-CNN / MobileNet-style, ESP-DL (`.espdl` via ESP-PPQ) | 100–500 KB | 100–400 KB (PSRAM-backed) | ~150–600 ms (est.) | More robust across species and noise; heavier toolchain (ESP-IDF ≥ 5.3 components) |
| D | BirdNET v2.4 on device | 62 MB ONNX (≫ 3.2 MB app partition) | MBs | seconds | **Not viable**, too large by more than 10× |

Headroom on the current `bps_xiao` build: `firmware.bin` is 1.04 MB in a 3.2 MB app partition, so
there is about 2.2 MB of flash free. Static internal DRAM (data + bss) is about 95 KB. The ring and SD
queue are already in PSRAM. Before choosing where the tensor arena goes, measure free *internal* RAM at
runtime with `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)` while Wi-Fi is up (`port_free_heap()`
counts PSRAM too). For B, an internal-RAM arena is preferred. A PSRAM arena works but runs slower.

### Where the timing estimates come from

- BioDCASE 2025 Task 3 (yellowhammer detection on ESP32-S3-Korvo-2, TFLite Micro). Measured totals
  per clip, including feature extraction:

  | Entry | AP | Size | Time | RAM |
  |---|---|---|---|---|
  | Oguamanam (flux) | 0.97 | 1.9 KB | 16.7 ms | 0.8 KB |
  | Oguamanam | 0.97 | 4.2 KB | 41.7 ms | 18 KB |
  | Toby_Martin | 0.99 | 7.8 KB | 88.6 ms | 21 KB |
  | Londono | 0.98 | 16.6 KB | 200 ms | 27 KB |

  Walter's report gives a breakdown: 16.45 ms of model time and 1.63 ms of features, using 16 mel
  bands with a 2048-point window and 1024 hop. The board runs at 240 MHz and we run at 160 MHz, so I
  multiplied by about 1.5.
- ESP-NN speedup on ESP32-S3 (TFLM person detection): 2300 ms without ESP-NN and 54 ms with it.
  **ESP-NN is essential.**
- TinyChirp ran comparable models on a 64 MHz Cortex-M4 (nRF52840) in 1–2.4 s per 3 s window. The
  S3 at 160 MHz with SIMD is roughly 10–20× faster.

Caveat: those models detect **one species**. "Any bird" is a harder task, and I expect it to need
the upper end of approach B (around 30–60 KB, about 100–150 ms per 3 s). This is an estimate. It has
to be measured on the board.

## Recommended design (B as a cascade)

1. **Stage 1 (existing gate):** lower `threshold_db` to around 5–6 dB so that faint calls open it.
   The server already sends this value at registration.
2. **Stage 2 (CNN):** when the gate opens, take the look-back plus the triggering frame (about 3 s)
   from the ring. Decimate 48 kHz to 16 kHz and compute a log-mel spectrogram (for example 32–40 mel
   bands, 32 ms window, 16 ms hop: about 190 × 40). Run the int8 CNN. Start the upload only if
   P(bird) is above a threshold the server can tune. While the gate stays open, re-run every ~1 s on
   new audio.
3. **Latency:** about 100–150 ms per decision. The upload starts that much later, which is small
   next to the 2.5 s look-back and the ~10.9 s ring. Nothing is lost.
4. **CPU placement:** capture must stay alone at high priority on core 0 (see the core-0 capture
   fix). Run inference in a low-priority task on core 1. Its timing is not critical, so the loopTask
   starvation that hurt capture does not matter here.
5. **Training data:** the server already runs BirdNET on every upload. Label historical uploads as
   bird or no-bird from those results. This gives site-specific data at our 48 kHz with our mic, and
   it includes exactly the false triggers we want rejected. DCASE 2018 Bird Audio Detection (about
   35k ten-second clips, labeled bird or no-bird) can be used for pre-training.
6. **Safety valve:** log stage-2 rejections, and have the server occasionally request rejected clips
   so the miss rate can be audited.

Approach A is cheap and could be added between the two stages to drop obvious wind and rain before
the CNN runs. It is optional.

### Inference time for the recommended approach

| Look-back window | Features (est.) | CNN (est.) | Total @160 MHz |
|---|---|---|---|
| 2.5 s look-back + 0.5 s trigger = 3 s | 3–5 ms | 25–145 ms | **~30–150 ms** |
| Re-check, 1 s hop while the gate is open | ~1–2 ms | 10–50 ms | **~10–50 ms per second** |

In cascade mode the CNN runs only when the gate opens. Even if it ran continuously at a 1 s hop, it
would load one core at 160 MHz by about 1–5%.

## Battery impact

- The radio dominates our power. `WiFi.setSleep(false)` keeps it awake, at roughly 90 mA in RX and
  280–340 mA peaks in TX (ESP32-S3 datasheet figures). The CPU runs at 160 MHz.
- An active core costs roughly 10–20 mA more than an idle one (estimate). In cascade mode the CNN
  runs for ~0.15 s per gate opening. At even 1,000 openings a day, that is about 2.5 CPU-minutes, or
  well under 0.05 mAh. That is **negligible**, below 0.1% of daily consumption.
- Worst case, continuous inference at a 1 s hop with about 5% duty: about +0.5–1 mA, or roughly 1%
  less battery life.
- Net effect: probably **positive**. Each rejected false trigger saves an upload (several seconds of
  TX at 48 kHz). If the gate currently fires mostly on noise, battery life should improve. Measure
  the drain (MAX17048 %/h in the heartbeat) before and after.
- This is an estimate. The real baseline current of the bps XIAO has not been measured for this
  document.

## Next steps (if approved)

1. Export about 2 weeks of uploads with their BirdNET results and build a bird/no-bird dataset.
   Measure how many current triggers have no bird in them. That number decides whether this is
   worth doing.
2. Train approach B in Keras or PyTorch, quantize to int8 TFLite, and check the size against the
   table above.
3. Integrate `esp-tflite-micro` (with ESP-NN) through pioarduino's ESP-IDF component support. Measure
   the invoke time and free internal RAM on the bps XIAO.
4. Run an A/B on two devices: gate only versus gate + CNN, comparing BirdNET-confirmed detections
   and battery %/h.

## Sources

- BioDCASE 2025 Task 3, Bioacoustics for Tiny Hardware: https://biodcase.github.io/challenge2025/task3
- BioDCASE 2025 Task 3 results: https://biodcase.github.io/challenge2025/task3-results
- C. Walter, "Efficient CNNs for bioacoustics in tiny hardware" (BioDCASE 2025 tech report): https://biodcase.github.io/documents/challenge2025/technical_reports/walter_task3_technical_report.pdf
- BioDCASE-Tiny 2025 baseline (TFLM deployment to ESP32-S3): https://github.com/birdnet-team/BioDCASE-Tiny-2025
- BioDCASE 2026 Task 3: https://biodcase.github.io/challenge2026/task3
- Huang et al., "TinyChirp: Bird Song Recognition Using TinyML Models on Low-power Wireless Acoustic Sensors", IS2 2024: https://arxiv.org/abs/2407.21453
- Espressif esp-tflite-micro (ESP-NN benchmark): https://github.com/espressif/esp-tflite-micro
- Espressif ESP-DL / ESP-PPQ: https://github.com/espressif/esp-dl
- ESP-DL `imagenet_cls` latency (MobileNetV2 on S3): https://components.espressif.com/components/espressif/imagenet_cls/versions/0.4.1/readme
- DCASE 2018 Task 3, Bird Audio Detection: https://dcase.community/challenge2018/task-bird-audio-detection
- Enhanced MicroDSC pest-bird detector on XIAO ESP32S3 (PeerJ, abstract only): https://peerj.com/reviewer-match/128550
- ESP32-S3 datasheet (current consumption): https://documentation.espressif.com/esp32-s3_datasheet_en.html
- Local: `firmware/lib/bps_core/src/gate.c`, `firmware/scripts/bps_config.py` (bps_xiao defaults), `firmware/src/port/esp32.cpp`
