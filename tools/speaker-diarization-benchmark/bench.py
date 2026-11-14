#!/usr/bin/env python3
"""How often scripts/diarize.py gives a stretch of speech to the wrong voice.

PA3 -- docs/roadmap/PARITE-2026-11.md. No recording of two voices with a
known answer was at hand, so this makes some: it takes sound files that
each hold ONE voice, splices turns of two of them end to end (each turn 2
to 12 seconds, drawn with a fixed seed), and so knows who speaks at every
instant. Every pair of the files given makes one dialogue.

    python3 bench.py --python ENV/bin/python --embedding MODEL.onnx \
        VOICE.wav VOICE.wav [VOICE.wav ...]

The files are any sound FFmpeg reads... once made 16 kHz mono 16 bits:

    ffmpeg -i in.wav -ac 1 -ar 16000 -c:a pcm_s16le VOICE.wav

For each dialogue it prints the confusion -- the share of the time given to
the wrong voice, the voices matched the best way -- and `similarity`, what
the script itself reports of how alike the two voices are. Then the mean,
the median and the worst.

What it does not measure: a real conversation. Spliced turns have no pause
at the change of voice and no overlap; the rushes are whatever one-voice
files were given. On 12 November 2026, with six voices of one delivered
film (five women, one man, same room): mean 5.9 %, median 4.2 %, worst
25.5 %, three voices in turn 4.1 %.
"""

from __future__ import annotations

import argparse
import itertools
import json
import pathlib
import random
import statistics
import subprocess
import sys
import tempfile
import wave

RATE = 16000
SCRIPT = pathlib.Path(__file__).resolve().parents[2] / "scripts" / "diarize.py"


def read(path: str) -> bytes:
    with wave.open(path, "rb") as handle:
        if (
            handle.getnchannels() != 1
            or handle.getsampwidth() != 2
            or handle.getframerate() != RATE
        ):
            sys.exit(f"{path}: make it mono, 16 bits, {RATE} Hz first")
        return handle.readframes(handle.getnframes())


def write(path: pathlib.Path, frames: bytes) -> None:
    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(frames)


def dialogue(voices: list[bytes], seed: int):
    """Turns of each voice in rotation, until one runs out: the sound, and
    [(start, end, voice)] in samples."""
    draw = random.Random(seed)
    at = [0] * len(voices)
    pieces: list[bytes] = []
    turns = []
    length = 0
    who = 0
    while True:
        samples = int(draw.uniform(2.0, 12.0) * RATE)
        if at[who] + samples > len(voices[who]) // 2:
            break
        pieces.append(voices[who][2 * at[who] : 2 * (at[who] + samples)])
        at[who] += samples
        turns.append((length, length + samples, who))
        length += samples
        who = (who + 1) % len(voices)
    return b"".join(pieces), turns


def confusion(report: dict, turns, count: int) -> float:
    step = RATE // 100
    frames = max(end for _, end, _ in turns) // step
    truth = [-1] * frames
    heard = [-1] * frames
    for start, end, voice in turns:
        for frame in range(start // step, min(frames, end // step)):
            truth[frame] = voice
    for segment in report["segments"]:
        for frame in range(
            segment["start"] // step, min(frames, segment["end"] // step)
        ):
            heard[frame] = segment["speaker"]
    both = [(t, h) for t, h in zip(truth, heard) if t >= 0 and h >= 0]
    if not both:
        return 1.0
    best = max(
        sum(1 for t, h in both if order[t] == h)
        for order in itertools.permutations(range(count))
    )
    return 1.0 - best / len(both)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--python", required=True)
    parser.add_argument("--embedding", required=True)
    parser.add_argument("--voices", type=int, default=2)
    parser.add_argument("files", nargs="+")
    arguments = parser.parse_args()
    if len(arguments.files) < arguments.voices or arguments.voices < 2:
        sys.exit("give at least as many one-voice files as --voices (2 or more)")
    sounds = [read(path) for path in arguments.files]
    results = []
    with tempfile.TemporaryDirectory() as folder:
        work = pathlib.Path(folder)
        groups = itertools.combinations(range(len(sounds)), arguments.voices)
        for number, group in enumerate(groups):
            frames, turns = dialogue([sounds[i] for i in group], number)
            sound = work / f"dialogue-{number}.wav"
            report = work / f"dialogue-{number}.json"
            write(sound, frames)
            done = subprocess.run(
                [
                    arguments.python,
                    str(SCRIPT),
                    "--audio",
                    str(sound),
                    "--embedding",
                    arguments.embedding,
                    "--speakers",
                    str(arguments.voices),
                    "--report",
                    str(report),
                ],
                capture_output=True,
                text=True,
            )
            if done.returncode:
                sys.exit(done.stderr[-500:])
            answer = json.loads(report.read_text(encoding="utf-8"))
            wrong = confusion(answer, turns, arguments.voices)
            results.append(wrong)
            names = " + ".join(f"file {i + 1}" for i in group)
            print(
                f"{names}: {len(turns)} turns, confusion {100 * wrong:.1f} %, "
                f"similarity {[pair[2] for pair in answer['similarity']]}"
            )
    print(
        f"{len(results)} dialogue(s): mean {100 * statistics.mean(results):.1f} %, "
        f"median {100 * statistics.median(results):.1f} %, "
        f"worst {100 * max(results):.1f} %"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
