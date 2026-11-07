# Trigger models

Models for the client's **validation** step (#14): after the energy trigger
opens a recording, a small model checks whether a bird is calling before the
recording is uploaded. Each folder is one model, laid out as it goes on a
client's storage card:

```
/models/<model_name>/model.tflite   int8 TFLite (TFLite Micro + ESP-NN on the ESP32-S3)
/models/<model_name>/model.json     how to feed it and read it (below)
/models/<model_name>/LICENSE...     its licence, kept with it
```

Copy a folder to the card's `/models/`, then pick it on the server's config
page under **Recording validation**.

| Model | Detects | Size | Arena | Invoke on ESP32-S3 @160 MHz | Licence | Source |
|---|---|---|---|---|---|---|
| `drongonet-nano` | any bird present (binary) | 5.2 KB | 26 KB | **39 ms** / 3 s window | CC BY 4.0 | [DrongoNet](https://github.com/mun3im/drongonet), Zabidi et al. 2026, [arXiv 2607.19721](https://arxiv.org/abs/2607.19721) |
| `drongonet-micro` | any bird present (binary) | 6.4 KB | 26 KB | **47 ms** / 3 s window | CC BY 4.0 | same |
| `drongonet-edge` | any bird present (binary) | 33 KB | 297 KB (PSRAM) | **465 ms** / 3 s window | CC BY 4.0 | same |
| `bps-micro-warblr` | any bird present (binary) | 6.3 KB | 26 KB | 47 ms (as drongonet-micro) | CC BY 4.0 | ours: DrongoNet Micro layout trained on warblrb10k ([NOTICE](bps-micro-warblr/NOTICE.md)) |
| `bps-edge-warblr` | any bird present (binary) | 35 KB | 297 KB (PSRAM) | ~465 ms (as drongonet-edge) | CC BY 4.0 | ours: DrongoNet Edge layout trained on warblrb10k ([NOTICE](bps-edge-warblr/NOTICE.md)) |
| `birdnet-tiny-2.0-hybrid` | 1,007 bird species (+ 6 non-bird classes) | 2.0 MB | 933 KB (PSRAM) | **1,584 ms** / 2.5 s window | Apache 2.0 (+ [acceptable use](birdnet-tiny-2.0-hybrid/ACCEPTABLE_USE.md)) | [BirdNET-Tiny N6 GLOBAL 1K v2.0 Hybrid](https://github.com/birdnet-team/birdnet-stm32/releases/tag/v2.0), K. Lisa Yang Center / BirdNET team |

Invoke times and arena use were measured on a bps XIAO (2026-11-05) with the
Arduino core's bundled esp-tflite-micro 1.4.0 and esp-nn 1.3.2, the model in
PSRAM, random input. They exclude the front end (spectrogram), which is
measured separately once it is ported (#14). At 240 MHz they are about 1.3×
faster (Nano 26 ms, Micro 32 ms, BirdNET-Tiny 1,219 ms).

All four use only TFLite Micro built-in ops: CONV_2D, DEPTHWISE_CONV_2D,
FULLY_CONNECTED, MAX_POOL_2D, MEAN, SOFTMAX, MUL, ADD, QUANTIZE, DEQUANTIZE,
TRANSPOSE, STRIDED_SLICE. ESP-NN has optimised kernels for all but MEAN,
TRANSPOSE and STRIDED_SLICE.

`birdnet-tiny-2.0-hybrid/model.tflite` is 2 MB and not kept in git:
`tools/fetch-birdnet-tiny.sh` downloads it and checks its SHA-256.

## Expected use

- **bps-edge-warblr:** the best detector here so far (AUC 0.91 on held-out
  warblrb10k; stops 56% of no-bird recordings while keeping 95% of bird ones).
- **bps-micro-warblr:** 10× cheaper, AUC 0.84; for checking every trigger.

- **DrongoNet Nano/Micro:** cheap enough to check every trigger, and to re-check
  every second while the gate stays open. Trained on Southeast Asian (tropical)
  recordings; the paper reports it does not transfer to other regions without
  retraining, so treat the shipped weights as a starting point.
- **DrongoNet Edge:** more accurate than Micro on temperate data in the paper
  (AUC 0.94 vs 0.85 on DCASE 2018), at ~10× the cost; still well inside the
  ring buffer's ~10.9 s.
- **BirdNET-Tiny:** a species classifier used as a detector (score = the
  highest bird-class probability, the six non-bird classes ignored). 1.6 s per
  window: fine for an occasional second opinion, too slow to check every trigger
  in a dawn chorus.

## model.json

Every key is unique and flat (or a plain number list), so the firmware can read
it with its small JSON helpers.

| Key | Meaning |
|---|---|
| `model_name`, `model_file` | folder name; the TFLite file in it |
| `model_bytes` | the TFLite file's size: the client allocates this much and checks it read all of it |
| `sample_rate`, `window_samples` | what one model input covers (the client resamples from 48 kHz) |
| `frontend` | `logmel_db_minmax` or `stft_sqrt_minmax`, specified in [`tools/frontends.py`](tools/frontends.py) |
| `fft_length`, `hop_length`, `frames` | the STFT (`stft_sqrt_minmax` derives its hop from `frames`) |
| `mel_bands`, `mel_fmin_hz`, `mel_fmax_hz` | the mel filterbank (`logmel_db_minmax`) |
| `layout` | `time_freq` (frames × bins) or `freq_time` |
| `output_kind` | `probability` (as is) or `logit` (sigmoid first) |
| `bird_output_first`, `bird_output_count` | the outputs that mean "bird"; the score is their maximum |
| `score_threshold` | default: upload when the score reaches it (the server can override) |
| `arena_bytes` | tensor arena to allocate (measured use plus margin) |
| `measured_*` | as measured on the device (informational) |
| `license`, `source` | provenance |

## Evaluating on the host

```bash
python3 -m venv .venv-ml && .venv-ml/bin/pip install numpy scipy soundfile ai-edge-litert
trigger-models/tools/fetch-birdnet-tiny.sh
.venv-ml/bin/python trigger-models/tools/evaluate.py --warblr ~/datasets/warblrb10k --limit 1500 trigger-models/*/
.venv-ml/bin/python trigger-models/tools/evaluate.py --labels our-sites/test.csv trigger-models/*/
```

`evaluate.py` scores each clip as the best of its windows and reports AUC, and
at the model's threshold its precision, recall, **false-trigger rejection**
(no-bird clips it stops) and **miss rate** (bird clips it drops), plus the
rejection it reaches while keeping 95% of bird clips.

## Results

See [RESULTS.md](RESULTS.md).

## Attribution

- DrongoNet: © 2026 Mun'im Zabidi, CC BY 4.0
  (<https://creativecommons.org/licenses/by/4.0/>). Model files unmodified.
- BirdNET-Tiny N6 GLOBAL 1K v2.0 Hybrid: BirdNET team, Apache License 2.0
  (`LICENSE-MODELS.md`). Model file unmodified; `labels.txt` is the bundle's
  label list. Not endorsed by the BirdNET project.
- Front end `stft_sqrt_minmax` follows birdnet-stm32's `reference/` (MIT).
- bps-micro-warblr, bps-edge-warblr: trained on warblrb10k (DCASE 2018 Bird
  Audio Detection, CC BY 4.0); see their NOTICE.md.
