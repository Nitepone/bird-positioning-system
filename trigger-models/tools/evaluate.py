#!/usr/bin/env python3
"""Scores trigger models on labelled clips, before they go to a device.

Each clip is cut into the model's windows (hop = half a window, the last one
right-aligned, as BirdNET-Tiny's reference), each window is scored, and the
clip's score is the highest. A clip counts as "bird" when that score reaches
the model's score_threshold (model.json), or --threshold.

Datasets:
  --labels FILE.csv  with columns path,label (label 1 = bird, 0 = no bird;
                     paths relative to the CSV), as export_dataset.py writes
  --warblr DIR       DCASE 2018 warblrb10k: DIR/labels.csv and DIR/wav/

Usage:
  evaluate.py --warblr ~/datasets/warblrb10k --limit 1000 trigger-models/drongonet-*
  evaluate.py --labels ours/test.csv --json results.json trigger-models/*/
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import random
import sys
import time
from fractions import Fraction
from pathlib import Path

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

sys.path.insert(0, str(Path(__file__).resolve().parent))
from frontends import model_input  # noqa: E402

try:
    from ai_edge_litert.interpreter import Interpreter
except ImportError:  # pragma: no cover
    from tensorflow.lite import Interpreter  # type: ignore


class Model:
    def __init__(self, folder: Path):
        self.folder = folder
        self.cfg = json.loads((folder / "model.json").read_text())
        path = folder / self.cfg["model_file"]
        if not path.exists():
            raise FileNotFoundError(f"{path} (see tools/fetch-birdnet-tiny.sh)")
        self.it = Interpreter(model_path=str(path), num_threads=1)
        self.it.allocate_tensors()
        self.inp = self.it.get_input_details()[0]
        self.out = self.it.get_output_details()[0]
        self.name = self.cfg["model_name"]
        self.infer_s = 0.0
        self.windows = 0

    def score_window(self, window: np.ndarray) -> float:
        x = model_input(window, self.cfg)
        x = x.reshape(self.inp["shape"])
        if self.inp["dtype"] == np.int8:
            scale, zero = self.inp["quantization"]
            x = np.clip(np.round(x / scale + zero), -128, 127).astype(np.int8)
        else:
            x = x.astype(self.inp["dtype"])
        t = time.perf_counter()
        self.it.set_tensor(self.inp["index"], x)
        self.it.invoke()
        y = self.it.get_tensor(self.out["index"]).reshape(-1).astype(np.float32)
        self.infer_s += time.perf_counter() - t
        self.windows += 1
        if self.out["dtype"] == np.int8:
            scale, zero = self.out["quantization"]
            y = (y - zero) * scale
        first, count = self.cfg["bird_output_first"], self.cfg["bird_output_count"]
        y = y[first : first + count]
        if self.cfg["output_kind"] == "logit":
            y = 1.0 / (1.0 + np.exp(-y))
        return float(y.max())

    def score_clip(self, audio: np.ndarray, rate: int) -> float:
        sr = self.cfg["sample_rate"]
        if rate != sr:
            f = Fraction(sr, rate)
            audio = resample_poly(audio, f.numerator, f.denominator).astype(np.float32)
        n = self.cfg["window_samples"]
        if len(audio) < n:
            audio = np.pad(audio, (0, n - len(audio)))
        hop = n // 2
        starts = list(range(0, len(audio) - n + 1, hop))
        if starts[-1] != len(audio) - n:
            starts.append(len(audio) - n)
        return max(self.score_window(audio[s : s + n]) for s in starts)


def load_clips(args) -> list[tuple[Path, int]]:
    clips = []
    if args.warblr:
        root = Path(args.warblr).expanduser()
        with open(root / "labels.csv") as f:
            for row in csv.DictReader(f):
                clips.append((root / "wav" / f"{row['itemid']}.wav", int(row["hasbird"])))
    else:
        root = Path(args.labels).expanduser().parent
        with open(args.labels) as f:
            for row in csv.DictReader(f):
                clips.append((root / row["path"], int(row["label"])))
    if args.limit and len(clips) > args.limit:
        random.Random(args.seed).shuffle(clips)
        clips = clips[: args.limit]
    return clips


def read_mono(path: Path) -> tuple[np.ndarray, int]:
    audio, rate = sf.read(str(path), dtype="float32", always_2d=True)
    return audio.mean(axis=1), rate


def auc(scores: np.ndarray, labels: np.ndarray) -> float:
    """Probability that a bird clip outscores a no-bird clip (ties count half)."""
    pos, neg = scores[labels == 1], scores[labels == 0]
    if not len(pos) or not len(neg):
        return math.nan
    order = np.argsort(np.concatenate([pos, neg]), kind="mergesort")
    ranks = np.empty(len(order))
    allv = np.concatenate([pos, neg])[order]
    i = 0
    while i < len(allv):  # average ranks over ties
        j = i
        while j + 1 < len(allv) and allv[j + 1] == allv[i]:
            j += 1
        ranks[order[i : j + 1]] = (i + j) / 2 + 1
        i = j + 1
    return float((ranks[: len(pos)].sum() - len(pos) * (len(pos) + 1) / 2) / (len(pos) * len(neg)))


def at_threshold(scores, labels, thr) -> dict:
    pred = scores >= thr
    tp = int((pred & (labels == 1)).sum())
    fp = int((pred & (labels == 0)).sum())
    fn = int((~pred & (labels == 1)).sum())
    tn = int((~pred & (labels == 0)).sum())
    div = lambda a, b: a / b if b else math.nan  # noqa: E731
    return {
        "threshold": thr,
        "precision": div(tp, tp + fp),
        "recall": div(tp, tp + fn),
        # Of the recordings with no bird, the share the model stops (uploads saved).
        "false_trigger_rejection": div(tn, tn + fp),
        # Of the recordings with a bird, the share the model would drop.
        "miss_rate": div(fn, tp + fn),
        "tp": tp, "fp": fp, "fn": fn, "tn": tn,
    }


def recall_target(scores, labels, recall: float) -> dict:
    """The highest threshold that still keeps `recall` of the bird clips."""
    pos = np.sort(scores[labels == 1])
    if not len(pos):
        return {}
    thr = float(pos[int(math.floor((1 - recall) * len(pos)))])
    return at_threshold(scores, labels, thr)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("models", nargs="+", type=Path, help="model folders (with model.json)")
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--labels")
    src.add_argument("--warblr")
    ap.add_argument("--limit", type=int, default=0, help="score a random sample of this many clips")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--threshold", type=float, help="instead of each model's score_threshold")
    ap.add_argument("--json", type=Path, help="write per-model results here")
    args = ap.parse_args()

    models = []
    for folder in args.models:
        try:
            models.append(Model(folder))
        except FileNotFoundError as e:
            print(f"skipping {folder}: {e}", file=sys.stderr)
    clips = load_clips(args)
    labels = np.array([y for _, y in clips])
    scores = {m.name: np.zeros(len(clips)) for m in models}
    t0 = time.time()
    for i, (path, _) in enumerate(clips):
        audio, rate = read_mono(path)
        for m in models:
            scores[m.name][i] = m.score_clip(audio, rate)
        if (i + 1) % 200 == 0:
            print(f"  {i + 1}/{len(clips)} clips, {time.time() - t0:.0f} s", file=sys.stderr)

    print(f"\n{len(clips)} clips: {int(labels.sum())} bird, {int((labels == 0).sum())} no bird\n")
    head = f"{'model':26} {'AUC':>6} {'thr':>6} {'prec':>6} {'recall':>6} {'reject':>6} {'miss':>6}   {'@95% recall: thr reject':>24}  {'ms/win':>7}"
    print(head)
    print("-" * len(head))
    results = {}
    for m in models:
        s = scores[m.name]
        thr = args.threshold if args.threshold is not None else m.cfg["score_threshold"]
        r = at_threshold(s, labels, thr)
        r95 = recall_target(s, labels, 0.95)
        a = auc(s, labels)
        ms = 1000 * m.infer_s / max(1, m.windows)
        results[m.name] = {"auc": a, "at_threshold": r, "at_95_recall": r95, "host_ms_per_window": ms}
        print(f"{m.name:26} {a:6.3f} {thr:6.3f} {r['precision']:6.3f} {r['recall']:6.3f} "
              f"{r['false_trigger_rejection']:6.3f} {r['miss_rate']:6.3f}   "
              f"{r95.get('threshold', math.nan):11.3f} {r95.get('false_trigger_rejection', math.nan):12.3f}  {ms:7.2f}")
    print("\nreject: share of no-bird clips the model stops; miss: share of bird clips it drops.")
    if args.json:
        args.json.write_text(json.dumps({"clips": len(clips), "results": results}, indent=2) + "\n")


if __name__ == "__main__":
    main()
