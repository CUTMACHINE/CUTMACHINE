#!/usr/bin/env python3
"""Find filled pauses by their sound: a held vowel, steady in pitch and timbre.

Every transcription model and phone recognizer measured drops the « euh »
(docs/TRANSCRIPTION_BENCH.md): they learnt from text where hesitations are
not written. This detector learns nothing. A « euh » or a vowel held while
the speaker looks for a word keeps its pitch and its spectral envelope for
200 ms or more, where speech changes every few tens of milliseconds (Goto,
Itou and Hayamizu, 1999, filled-pause detection from small F0 fluctuation and
small spectral deformation).

Per 10 ms frame: voicing and F0 by YIN (de Cheveigné and Kawahara, 2002)
on 40 ms, smoothed over five frames,
twelve mel-cepstral coefficients on 25 ms, and loudness against the engine's
voice line (25 % of the 90th percentile amplitude). A frame is steady when
every frame of a centred window is voiced and loud, its F0 spans at most a
few semitones, and its cepstrum moves little from frame to frame; a steady
run long enough is a detection.

`--tune` searches the thresholds on the tuning recordings of C-PROM
(`cprom.py is_tuning_recording`) for the best F1 on hesitation syllables,
then writes detections for every chunk with them; without it, `--params`
gives the thresholds. Needs numpy; runs outside the harness.
"""

from __future__ import annotations

import argparse
import importlib.util
import itertools
import json
import sys
import wave
from pathlib import Path

import numpy as np

HERE = Path(__file__).resolve().parent
RATE = 16000
HOP = 160  # 10 ms
FFT_FRAME = 400  # 25 ms
PITCH_FRAME = 640  # 40 ms: the integration window plus the longest lag
F0_MIN, F0_MAX = 70.0, 400.0
YIN_THRESHOLD = 0.15
VOICE_LINE = 0.25
SPEECH_PERCENTILE = 90
MEL_BANDS = 24
CEPSTRA = 12


def load_cprom():
    spec = importlib.util.spec_from_file_location("cprom", HERE / "cprom.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        if (handle.getnchannels(), handle.getsampwidth(),
                handle.getframerate()) != (1, 2, RATE):
            raise SystemExit(f"{path}: expected 16 kHz mono 16-bit PCM")
        frames = handle.readframes(handle.getnframes())
    return np.frombuffer(frames, dtype="<i2").astype(np.float64) / 32768.0


def frames_of(signal: np.ndarray, size: int, count: int) -> np.ndarray:
    padded = np.pad(signal, (size // 2, size + count * HOP))
    windows = np.lib.stride_tricks.sliding_window_view(padded, size)[::HOP]
    return windows[:count]


def mel_filters() -> np.ndarray:
    def mel(hz):
        return 2595 * np.log10(1 + hz / 700)

    def hz(m):
        return 700 * (10 ** (m / 2595) - 1)

    edges = hz(np.linspace(mel(60), mel(7600), MEL_BANDS + 2))
    bins = np.fft.rfftfreq(512, 1 / RATE)
    filters = np.zeros((MEL_BANDS, len(bins)))
    for band in range(MEL_BANDS):
        low, middle, high = edges[band:band + 3]
        rising = (bins - low) / (middle - low)
        falling = (high - bins) / (high - middle)
        filters[band] = np.clip(np.minimum(rising, falling), 0, None)
    return filters


FILTERS = mel_filters()
DCT = np.cos(np.pi / MEL_BANDS * (np.arange(MEL_BANDS) + 0.5)[None, :]
             * np.arange(1, CEPSTRA + 1)[:, None])


def features(signal: np.ndarray) -> dict[str, np.ndarray]:
    count = max(1, len(signal) // HOP)
    pitch = frames_of(signal, PITCH_FRAME, count)
    pitch = pitch - pitch.mean(axis=1, keepdims=True)
    longest = int(RATE / F0_MIN)
    shortest = int(RATE / F0_MAX)
    width = PITCH_FRAME - longest
    # YIN's difference function from one FFT correlation and running
    # energies: d(t) = e(0) + e(t) - 2 c(t) over the integration window.
    size = 2 * PITCH_FRAME
    head = np.fft.rfft(pitch[:, :width], n=size, axis=1)
    whole = np.fft.rfft(pitch, n=size, axis=1)
    cross = np.fft.irfft(np.conj(head) * whole, n=size, axis=1)[:, :longest + 1]
    squares = np.concatenate(
        [np.zeros((count, 1)), np.cumsum(pitch ** 2, axis=1)], axis=1)
    lags = np.arange(longest + 1)
    energy = squares[:, lags + width] - squares[:, lags]
    difference = np.maximum(energy[:, :1] + energy - 2 * cross, 0.0)
    running = np.cumsum(difference[:, 1:], axis=1)
    normalized = np.ones_like(difference)
    normalized[:, 1:] = difference[:, 1:] * lags[1:] / np.maximum(running, 1e-12)
    band = normalized[:, shortest:longest + 1]
    below = band < YIN_THRESHOLD
    first = np.where(below.any(axis=1), below.argmax(axis=1), band.argmin(axis=1))
    rows = np.arange(count)
    for _ in range(band.shape[1]):
        step = np.minimum(first + 1, band.shape[1] - 1)
        better = band[rows, step] < band[rows, first]
        if not better.any():
            break
        first = np.where(better, step, first)
    strength = np.clip(1 - band[rows, first], 0.0, 1.0)
    f0 = RATE / (first + shortest)
    padded = np.pad(f0, 2, mode="edge")
    f0 = np.median(np.lib.stride_tricks.sliding_window_view(padded, 5), axis=1)

    short = frames_of(signal, FFT_FRAME, count)
    power = (short ** 2).mean(axis=1)
    ranked = np.sort(power)
    speech_power = ranked[max(0, int(np.ceil(SPEECH_PERCENTILE / 100
                                             * len(ranked))) - 1)]
    loud = power >= VOICE_LINE ** 2 * speech_power
    emphasized = short - 0.97 * np.concatenate([short[:, :1], short[:, :-1]],
                                               axis=1)
    magnitude = np.abs(np.fft.rfft(emphasized * np.hanning(FFT_FRAME),
                                   n=512, axis=1)) ** 2
    cepstra = (DCT @ np.log(magnitude @ FILTERS.T + 1e-10).T).T
    change = np.r_[0.0, np.linalg.norm(np.diff(cepstra, axis=0), axis=1)]
    return {"strength": strength, "f0": f0, "loud": loud, "change": change}


def window_stats(feature: dict[str, np.ndarray],
                 window: int) -> dict[str, np.ndarray] | None:
    """Per window of `window` frames: weakest voicing, all loud, F0 spread
    in semitones, mean cepstral change."""
    if len(feature["f0"]) < window:
        return None
    views = np.lib.stride_tricks.sliding_window_view
    tone = 12 * np.log2(feature["f0"] / 100.0)
    return {
        "weakest": views(feature["strength"], window).min(axis=1),
        "loud": views(feature["loud"], window).all(axis=1),
        "spread": np.ptp(views(tone, window), axis=1),
        "moving": views(feature["change"], window).mean(axis=1),
    }


def steady_runs(stats: dict[str, np.ndarray] | None, voicing: float,
                semitones: float, change: float, window: int,
                minimum: float) -> list[list[float]]:
    if stats is None:
        return []
    steady = ((stats["weakest"] >= voicing) & stats["loud"]
              & (stats["spread"] <= semitones) & (stats["moving"] <= change))
    edges = np.diff(np.r_[0, steady.astype(np.int8), 0])
    starts = np.flatnonzero(edges == 1)
    ends = np.flatnonzero(edges == -1)
    first = starts * HOP / RATE
    last = (ends - 1 + window) * HOP / RATE
    keep = last - first >= minimum
    return [[round(float(a), 3), round(float(b), 3)]
            for a, b in zip(first[keep], last[keep])]


GRID = {
    "voicing": [0.6, 0.7, 0.8, 0.9],
    "semitones": [1.0, 1.5, 2.0, 3.0],
    "change_percentile": [10, 20, 30, 40, 50],
    "window": [6, 8, 12, 16],
    "minimum": [0.12, 0.15, 0.2, 0.25],
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-dir", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--tune", action="store_true")
    parser.add_argument("--params", type=Path)
    args = parser.parse_args(argv)

    cprom = load_cprom()
    gold = json.loads((args.run_dir / "gold.json").read_text(encoding="utf-8"))
    targets = cprom.hesitation_targets(args.source, gold)
    feature = {}
    for chunk_id in gold:
        feature[chunk_id] = features(
            read_wav(args.run_dir / "chunks" / f"{chunk_id}.wav"))
        print(f"{chunk_id}: {len(feature[chunk_id]['f0'])} frames",
              file=sys.stderr)
    tuning = [c for c in gold if cprom.is_tuning_recording(gold[c]["source"])]
    stats = {
        (chunk_id, window): window_stats(feature[chunk_id], window)
        for chunk_id in gold
        for window in GRID["window"]
    }

    if args.tune:
        changes = np.concatenate([
            feature[c]["change"][feature[c]["loud"]] for c in tuning])
        best = None
        for values in itertools.product(*GRID.values()):
            params = dict(zip(GRID, values))
            params["change"] = float(np.percentile(
                changes, params["change_percentile"]))
            counts = [
                cprom.filled_pause_counts(targets[c], steady_runs(
                    stats[c, params["window"]], params["voicing"],
                    params["semitones"], params["change"], params["window"],
                    params["minimum"]))
                for c in tuning
            ]
            summary = cprom.filled_pause_summary(
                counts, sum(gold[c]["duration"] for c in tuning) / 60)
            if best is None or summary["f1"] > best[1]["f1"]:
                best = (params, summary)
        params, summary = best
        print(f"tuned: {params} -> {summary}", file=sys.stderr)
    else:
        params = json.loads(args.params.read_text(encoding="utf-8"))
        summary = None

    args.output_dir.mkdir(parents=True, exist_ok=True)
    with open(args.output_dir / "detections.jsonl", "w",
              encoding="utf-8") as output:
        for chunk_id in gold:
            segments = steady_runs(
                window_stats(feature[chunk_id], params["window"]),
                params["voicing"], params["semitones"], params["change"],
                params["window"], params["minimum"])
            output.write(json.dumps({"stem": chunk_id, "segments": segments})
                         + "\n")
    (args.output_dir / "params.json").write_text(
        json.dumps(params, indent=2) + "\n", encoding="utf-8")
    if summary is not None:
        (args.output_dir / "tuning.json").write_text(
            json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
