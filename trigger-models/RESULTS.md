# Results

## warblrb10k held-out test split (2026-11-05)

1,617 clips (1,214 bird, 403 no bird), UK, recorded on smartphones, 10 s each:
the 20% of DCASE 2018's warblrb10k that `train_detector.py split` holds out
(by file id; none of it was trained on). Each clip is scored as the best of its
windows (hop half a window), as `tools/evaluate.py` does.

| Model | AUC | Threshold | Precision | Recall | Stops (no-bird) | Misses (bird) | At 95% recall: threshold, stops |
|---|---|---|---|---|---|---|---|
| drongonet-nano | 0.570 | 0.37 | 0.746 | 0.828 | 15% | 17% | 0.285, 5% |
| drongonet-micro | 0.685 | 0.35 | 0.784 | 0.796 | 34% | 20% | 0.254, 7% |
| drongonet-edge | 0.603 | 0.425 | 0.758 | 0.999 | 4% | 0.1% | 0.992, 18% |
| birdnet-tiny-2.0-hybrid | 0.864 | 0.25 | 0.931 | 0.689 | 85% | 31% | 0.059, 50% |
| **bps-micro-warblr** | 0.837 | 0.50 | 0.822 | 0.957 | 38% | 4% | 0.508, 40% |
| **bps-edge-warblr** | **0.905** | 0.50 | 0.834 | 0.989 | 41% | 1% | 0.625, **56%** |

"Stops" is the share of no-bird recordings the model would keep from being
uploaded; "misses" the share of bird recordings it would drop.

- **DrongoNet's shipped weights do not transfer** from Southeast Asia to the UK
  (AUC 0.57-0.69), as its paper reports. Its architecture does: trained on
  warblrb10k, Micro reaches 0.84 and Edge 0.91 (the paper: 0.85 and 0.94 on DCASE).
- **BirdNET-Tiny** works out of the box (0.86) but is 30× slower than Edge on the
  device and at its default 0.25 misses 31% of bird clips.
- The bps models' `score_threshold` is set to about their 95%-recall point
  (Micro 0.50, Edge 0.62). Those were read off this test split, so the
  figures at them are slightly optimistic; choose thresholds on a validation
  split once there is data from our sites.

**Caveat: none of this is our sites' audio.** Smartphone recordings in UK
gardens differ from our microphone (IM73D122), placement, species and noise.
`evaluate.py --labels` scores any labelled set; the numbers that matter come
from our own uploads labelled by the server's BirdNET.

## Device timings (bps XIAO, ESP32-S3 @ 160 MHz)

| Model | Invoke | Arena |
|---|---|---|
| drongonet-nano | 39 ms | 26 KB |
| drongonet-micro / bps-micro-warblr | 47 ms | 26 KB |
| drongonet-edge / bps-edge-warblr | 465 ms | 297 KB (PSRAM) |
| birdnet-tiny-2.0-hybrid | 1,584 ms | 933 KB (PSRAM) |

Model invoke only, random input, model in PSRAM; the front end (resampling and
spectrogram) comes on top (#14 measures it).
