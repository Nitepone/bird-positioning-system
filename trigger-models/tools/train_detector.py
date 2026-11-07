#!/usr/bin/env python3
"""Trains a bird / no-bird detector for the client's validation step and
exports it as an int8 model folder (model.tflite + model.json).

Architectures follow DrongoNet (Zabidi et al. 2026, CC BY 4.0), written anew
from its MODEL_REFERENCE.md:
  micro  FrequencyEmphasis -> Conv(6,3x3) -> MaxPool -> Conv(12,3x3) -> Conv(12,1x1)
         -> GAP -> Dropout(0.1) -> Dense(2, softmax); 16 mel bands
  edge   3 x [Conv -> BatchNorm -> ReLU] (16/32/64, pooling after the first two)
         -> GAP -> Dense(32) -> Dropout(0.2) -> Dense(2, softmax); 80 mel bands
The front end is tools/frontends.py's logmel_db_minmax (as the client runs it).

Data: a split CSV (path,label; see make_split) of clips of any length and rate.
Each clip gives up to three 3 s windows (start, middle, end), all with the
clip's label: weak labels, as clip-level annotations are.

Usage:
  train_detector.py split --warblr ~/datasets/warblrb10k      # writes train.csv / test.csv there
  train_detector.py train --train DIR/train.csv --arch micro --out trigger-models/bps-micro-warblr
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import os
import sys
from concurrent.futures import ProcessPoolExecutor
from fractions import Fraction
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
from frontends import logmel_db_minmax  # noqa: E402

SR, WIN = 16000, 48000


def make_split(args):
    root = Path(args.warblr).expanduser()
    rows = list(csv.DictReader(open(root / "labels.csv")))
    out = {"train": [], "test": []}
    for r in rows:
        # A fixed split by file id: the same clips are held out every run.
        h = int(hashlib.sha256(r["itemid"].encode()).hexdigest()[:8], 16) % 100
        out["test" if h < args.test_pct else "train"].append((f"wav/{r['itemid']}.wav", r["hasbird"]))
    for name, items in out.items():
        with open(root / f"{name}.csv", "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["path", "label"])
            w.writerows(items)
        print(f"{root / (name + '.csv')}: {len(items)} clips, {sum(int(y) for _, y in items)} bird")


def _features(job):
    path, mels = job
    import soundfile as sf
    from scipy.signal import resample_poly

    audio, rate = sf.read(path, dtype="float32", always_2d=True)
    audio = audio.mean(axis=1)
    if rate != SR:
        f = Fraction(SR, rate)
        audio = resample_poly(audio, f.numerator, f.denominator).astype(np.float32)
    if len(audio) < WIN:
        audio = np.pad(audio, (0, WIN - len(audio)))
    starts = sorted({0, (len(audio) - WIN) // 2, len(audio) - WIN})
    cfg = dict(sample_rate=SR, fft_length=1024, hop_length=256, frames=184, mel_bands=mels,
               mel_fmin_hz=100.0, mel_fmax_hz=8000.0)
    return [logmel_db_minmax(audio[s : s + WIN], cfg) for s in starts]


def load_features(csv_path: Path, mels: int, cache: Path):
    rows = list(csv.DictReader(open(csv_path)))
    key = hashlib.sha256((str(csv_path.resolve()) + str(mels) + str(len(rows))).encode()).hexdigest()[:12]
    cached = cache / f"feat_{csv_path.stem}_{mels}_{key}.npz"
    if cached.exists():
        d = np.load(cached)
        return d["x"], d["y"], d["clip"]
    jobs = [(str(csv_path.parent / r["path"]), mels) for r in rows]
    xs, ys, clips = [], [], []
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 2) - 2)) as ex:
        for i, wins in enumerate(ex.map(_features, jobs, chunksize=32)):
            for w in wins:
                xs.append(w)
                ys.append(int(rows[i]["label"]))
                clips.append(i)
            if (i + 1) % 1000 == 0:
                print(f"  features: {i + 1}/{len(jobs)}", file=sys.stderr)
    x, y, clip = np.stack(xs)[..., None], np.array(ys), np.array(clips)
    cache.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(cached, x=x, y=y, clip=clip)
    return x, y, clip


def build(arch: str, mels: int):
    import tensorflow as tf
    from tensorflow import keras
    from tensorflow.keras import layers, regularizers

    class FrequencyEmphasis(layers.Layer):
        """A learnt gate per mel band: x * sigmoid(w * scale). Folds to one MUL."""

        def build(self, shape):
            self.w = self.add_weight(name="freq_weights", shape=(1, 1, shape[2], 1), initializer="zeros")
            self.scale = self.add_weight(name="scale", shape=(), initializer="ones")

        def call(self, x):
            return x * tf.sigmoid(self.w * self.scale)

    inp = keras.Input((184, mels, 1))
    if arch == "micro":
        l2 = regularizers.l2(1e-4)
        x = FrequencyEmphasis()(inp)
        x = layers.Conv2D(6, 3, activation="relu", kernel_regularizer=l2)(x)
        x = layers.MaxPool2D(2)(x)
        x = layers.Conv2D(12, 3, activation="relu", kernel_regularizer=l2)(x)
        x = layers.Conv2D(12, 1, activation="relu")(x)
        x = layers.GlobalAveragePooling2D()(x)
        x = layers.Dropout(0.1)(x)
    else:
        x = inp
        for ch, pool in ((16, True), (32, True), (64, False)):
            x = layers.Conv2D(ch, 3)(x)
            x = layers.BatchNormalization()(x)
            x = layers.ReLU()(x)
            if pool:
                x = layers.MaxPool2D(2)(x)
        x = layers.GlobalAveragePooling2D()(x)
        x = layers.Dense(32, activation="relu")(x)
        x = layers.Dropout(0.2)(x)
    out = layers.Dense(2, activation="softmax")(x)
    return keras.Model(inp, out)


def train(args):
    import tensorflow as tf
    from tensorflow import keras

    mels = 16 if args.arch == "micro" else 80
    cache = Path(args.cache).expanduser()
    x, y, clip = load_features(Path(args.train).expanduser(), mels, cache)
    # Validation: 10% of the training clips (whole clips, not windows).
    rng = np.random.default_rng(42)
    val_clips = set(rng.choice(np.unique(clip), size=len(np.unique(clip)) // 10, replace=False).tolist())
    is_val = np.array([c in val_clips for c in clip])
    xt, yt, xv, yv = x[~is_val], y[~is_val], x[is_val], y[is_val]
    print(f"{len(xt)} training windows ({yt.mean():.2f} bird), {len(xv)} validation", file=sys.stderr)

    def augment(a, b):
        a = tf.clip_by_value(a + tf.random.normal(tf.shape(a), stddev=0.02), 0.0, 1.0)
        a = tf.roll(a, shift=tf.random.uniform([], -10, 11, dtype=tf.int32), axis=0)
        return a, b

    ds = (tf.data.Dataset.from_tensor_slices((xt, keras.utils.to_categorical(yt, 2)))
          .shuffle(len(xt), seed=42).map(augment).batch(64).prefetch(2))
    dv = tf.data.Dataset.from_tensor_slices((xv, keras.utils.to_categorical(yv, 2))).batch(256)
    # Weights for the class imbalance (warblr is 3/4 bird).
    w1 = len(yt) / (2 * max(1, yt.sum()))
    w0 = len(yt) / (2 * max(1, (yt == 0).sum()))
    model = build(args.arch, mels)
    model.compile(optimizer=keras.optimizers.AdamW(1e-3, weight_decay=1e-4),
                  loss="categorical_crossentropy", metrics=[keras.metrics.AUC(name="auc")])
    model.fit(ds, validation_data=dv, epochs=args.epochs, class_weight={0: w0, 1: w1}, verbose=2,
              callbacks=[keras.callbacks.ReduceLROnPlateau("val_auc", mode="max", patience=5, factor=0.5, min_lr=1e-5),
                         keras.callbacks.EarlyStopping("val_auc", mode="max", patience=12, restore_best_weights=True)])

    # Full int8, as the client runs it; calibrated on training windows.
    def representative():
        for i in rng.choice(len(xt), size=min(500, len(xt)), replace=False):
            yield [xt[i : i + 1].astype(np.float32)]

    conv = tf.lite.TFLiteConverter.from_keras_model(model)
    conv.optimizations = [tf.lite.Optimize.DEFAULT]
    conv.representative_dataset = representative
    conv.target_spec.supported_ops = [tf.lite.OpsSet.TFLITE_BUILTINS_INT8]
    conv.inference_input_type = tf.int8
    conv.inference_output_type = tf.int8
    tfl = conv.convert()

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    (out / "model.tflite").write_bytes(tfl)
    name = out.name
    cfg = dict(model_name=name, model_file="model.tflite", sample_rate=SR, window_samples=WIN,
               frontend="logmel_db_minmax", fft_length=1024, hop_length=256, frames=184,
               mel_fmin_hz=100.0, mel_fmax_hz=8000.0, layout="time_freq", output_kind="probability",
               bird_output_first=1, bird_output_count=1, mel_bands=mels, score_threshold=args.threshold,
               arena_bytes=32768 if args.arch == "micro" else 327680, license="CC-BY-4.0 (architecture: DrongoNet)",
               source=f"tools/train_detector.py --arch {args.arch} on {Path(args.train).name} ({len(np.unique(clip))} clips)",
               params=int(model.count_params()), model_bytes=len(tfl))
    (out / "model.json").write_text(json.dumps(cfg, indent=2) + "\n")
    print(f"wrote {out}/model.tflite ({len(tfl)} bytes, {model.count_params()} params)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("split")
    s.add_argument("--warblr", required=True)
    s.add_argument("--test-pct", type=int, default=20)
    t = sub.add_parser("train")
    t.add_argument("--train", required=True)
    t.add_argument("--arch", choices=["micro", "edge"], default="micro")
    t.add_argument("--out", required=True)
    t.add_argument("--epochs", type=int, default=60)
    t.add_argument("--threshold", type=float, default=0.5)
    t.add_argument("--cache", default="~/.cache/bps-trigger-features")
    args = ap.parse_args()
    {"split": make_split, "train": train}[args.cmd](args)


if __name__ == "__main__":
    main()
