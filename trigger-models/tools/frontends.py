"""Model front ends: audio window -> model input, in plain NumPy.

These are the specification the firmware's C front end must match (#14). Each
model's model.json names its front end and gives every constant; nothing here
is tuned for a particular model.

- ``logmel_db_minmax`` (DrongoNet): librosa's melspectrogram (center=False,
  periodic Hann, power 2, Slaney mel scale and norm), power_to_db with
  ref=max and top_db=80, then per-window min-max to [0, 1]; time-major
  ``(frames, mel_bands)``.
- ``stft_sqrt_minmax`` (BirdNET-Tiny "hybrid"): centred frames (zero-padded by
  fft_length/2), periodic Hann, |rfft| without the Nyquist bin, square root,
  per-window min-max to [0, 1]; frequency-major ``(fft_length/2, frames)``.
  As birdnet-stm32's reference/ (MIT).
"""

from __future__ import annotations

import numpy as np


def hann_periodic(n: int) -> np.ndarray:
    return (0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(n) / n)).astype(np.float64)


def _hz_to_mel_slaney(f):
    f = np.asarray(f, dtype=np.float64)
    f_sp = 200.0 / 3
    mels = f / f_sp
    min_log_hz = 1000.0
    min_log_mel = min_log_hz / f_sp
    logstep = np.log(6.4) / 27.0
    return np.where(f >= min_log_hz, min_log_mel + np.log(np.maximum(f, 1e-12) / min_log_hz) / logstep, mels)


def _mel_to_hz_slaney(m):
    m = np.asarray(m, dtype=np.float64)
    f_sp = 200.0 / 3
    freqs = f_sp * m
    min_log_hz = 1000.0
    min_log_mel = min_log_hz / f_sp
    logstep = np.log(6.4) / 27.0
    return np.where(m >= min_log_mel, min_log_hz * np.exp(logstep * (m - min_log_mel)), freqs)


def mel_filterbank(sample_rate: int, n_fft: int, n_mels: int, fmin: float, fmax: float) -> np.ndarray:
    """librosa.filters.mel(htk=False, norm='slaney'): (n_mels, n_fft // 2 + 1)."""
    fft_freqs = np.linspace(0, sample_rate / 2, n_fft // 2 + 1)
    mel_pts = _mel_to_hz_slaney(np.linspace(_hz_to_mel_slaney(fmin), _hz_to_mel_slaney(fmax), n_mels + 2))
    fdiff = np.diff(mel_pts)
    ramps = mel_pts[:, None] - fft_freqs[None, :]
    weights = np.zeros((n_mels, len(fft_freqs)))
    for i in range(n_mels):
        lower = -ramps[i] / fdiff[i]
        upper = ramps[i + 2] / fdiff[i + 1]
        weights[i] = np.maximum(0, np.minimum(lower, upper))
    enorm = 2.0 / (mel_pts[2 : n_mels + 2] - mel_pts[:n_mels])
    return weights * enorm[:, None]


def _frames(x: np.ndarray, n_fft: int, hop: int, count: int) -> np.ndarray:
    idx = np.arange(n_fft)[None, :] + hop * np.arange(count)[:, None]
    return x[idx]


def logmel_db_minmax(window: np.ndarray, cfg: dict) -> np.ndarray:
    n_fft, hop, frames = cfg["fft_length"], cfg["hop_length"], cfg["frames"]
    x = np.asarray(window, dtype=np.float64)
    avail = 1 + (len(x) - n_fft) // hop
    fr = _frames(x, n_fft, hop, min(avail, frames)) * hann_periodic(n_fft)
    power = np.abs(np.fft.rfft(fr, axis=1)) ** 2  # (t, bins)
    fb = mel_filterbank(cfg["sample_rate"], n_fft, cfg["mel_bands"], cfg["mel_fmin_hz"], cfg["mel_fmax_hz"])
    mel = power @ fb.T  # (t, mels)
    amin, top_db = 1e-10, 80.0
    db = 10.0 * np.log10(np.maximum(amin, mel))
    db -= 10.0 * np.log10(max(amin, float(mel.max())))
    db = np.maximum(db, db.max() - top_db)
    if db.shape[0] < frames:  # as the training scripts: zero-pad short windows
        db = np.pad(db, ((0, frames - db.shape[0]), (0, 0)))
    return ((db - db.min()) / (db.max() - db.min() + 1e-8)).astype(np.float32)


def stft_sqrt_minmax(window: np.ndarray, cfg: dict) -> np.ndarray:
    n_fft, frames = cfg["fft_length"], cfg["frames"]
    hop = len(window) // frames
    x = np.pad(np.asarray(window, dtype=np.float64), (n_fft // 2, n_fft // 2))
    fr = _frames(x, n_fft, hop, frames) * hann_periodic(n_fft)
    mag = np.abs(np.fft.rfft(fr, axis=1))[:, : n_fft // 2]  # drop Nyquist
    s = np.sqrt(mag).T  # (bins, frames)
    return ((s - s.min()) / (s.max() - s.min() + 1e-10)).astype(np.float32)


FRONTENDS = {
    "logmel_db_minmax": logmel_db_minmax,
    "stft_sqrt_minmax": stft_sqrt_minmax,
}


def model_input(window: np.ndarray, cfg: dict) -> np.ndarray:
    return FRONTENDS[cfg["frontend"]](window, cfg)
