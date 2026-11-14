#!/usr/bin/env python3
"""Reads the stills probe.py exported and says in which encoding Resolve
blended: for each bar, the level it reads at 50 % opacity, and how much of
its light that keeps. Then how well each candidate encoding explains it.

    python3 analyse.py OUT_DIR

The stills are read through FFmpeg (`ffmpeg` on the PATH, or FFMPEG). A
blend with the black under a clip at opacity 1/2 gives, in an encoding e,
the level e^-1(e(x) / 2): halved code in the timeline's own gamma, half the
light in linear light. Levels are decoded with a 2.4 gamma, the input and
output of every pass."""

import math
import os
import struct
import subprocess
import sys

from bars import LEVELS, WIDTH


def read(path):
    ffmpeg = os.environ.get("FFMPEG", "ffmpeg")
    raw = subprocess.run(
        [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", path,
         "-frames:v", "1", "-pix_fmt", "rgb48le", "-f", "rawvideo", "-"],
        check=True, capture_output=True).stdout
    height = len(raw) // (WIDTH * 6)
    return [
        struct.unpack_from(
            "<H", raw,
            (height // 2 * WIDTH + int((bar + 0.5) * WIDTH / len(LEVELS))) * 6
        )[0] / 65535
        for bar in range(len(LEVELS))
    ]


def intermediate(light):
    if light <= 0.00262409:
        return light * 10.44426855
    return (math.log2(light + 0.0075) + 7.0) * 0.07329248


def intermediate_inverse(code):
    if code <= 0.02740668:
        return code / 10.44426855
    return 2 ** (code / 0.07329248 - 7.0) - 0.0075


def error(half, encode, decode):
    errors = [
        abs(max(decode(0.5 * encode(level ** 2.4)), 0.0) ** (1 / 2.4) - read_level)
        for level, read_level in zip(LEVELS, half) if level >= 0.1
    ]
    return sum(errors) / len(errors), max(errors)


def main() -> int:
    out = sys.argv[1]
    for name in ("yrgb", "rcm-rec709", "rcm-intermediate"):
        full = read(os.path.join(out, f"{name}-100.png"))
        half = read(os.path.join(out, f"{name}-50.png"))
        print(f"== {name}")
        print("   level at 100 %, at 50 %, light kept:")
        for level, whole, mixed in zip(LEVELS, full, half):
            if level < 0.1:
                continue
            print(f"     {level:.3f}  {whole:.4f}  {mixed:.4f}  "
                  f"{mixed ** 2.4 / level ** 2.4:.3f}")
        fits = {
            "the code of a 2.4 gamma": error(
                half, lambda light: light ** (1 / 2.4), lambda code: code ** 2.4),
            "linear light": error(half, lambda light: light, lambda code: code),
        }
        scale = min(
            (error(half, lambda light, s=s: intermediate(s * light),
                   lambda code, s=s: intermediate_inverse(code) / s), s)
            for s in [10 ** (k / 40) for k in range(-120, 41)])
        fits[f"DaVinci Intermediate, white at {scale[1]:.3f}"] = scale[0]
        for label, (mean, worst) in fits.items():
            print(f"   blended in {label}: mean error {mean:.4f}, worst {worst:.4f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
