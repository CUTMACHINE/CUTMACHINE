#!/usr/bin/env python3
"""Who speaks when, in one sound file, on this machine.

PA3 -- docs/roadmap/PARITE-2026-11.md. The engine runs this script with the
Python of the diarization environment (SpeakerDiarization.h); running it by
hand works too:

    python3 diarize.py --audio WAV --embedding ONNX --speakers N \
        [--speech JSON] --report JSON

THE METHOD, and why it is not the usual one. sherpa-onnx's own pipeline
(pyannote segmentation-3.0, then a clustering of what it segmented) was
measured first, on 12 November 2026, on fifteen dialogues made of two real
French voices each, every turn known: it told a man from a woman (about 1 %
of the time given to the wrong voice) and missed the change between two
women recorded in the same room nearly every time (up to 49 %; 26 % on
average, the count of voices given). The speaker embedding alone does
better: one vector per 1.5 s window, a window every 0.25 s, the vectors
gathered into N groups (spherical k-means). Same fifteen dialogues: 5.9 %
on average, 4.2 % at the median, a change of voice found within 0.25 s; one
pair of very close voices stays at 25 %; three voices in turn, 4.1 %. It
works through a second of sound in 0.06 s. So this script runs the
embedding model and nothing else.

--speakers N is how many voices there are, and it is required: guessed, the
count was wrong on five dialogues of fifteen. N of 1 answers at once, every
stretch of speech to the one voice.

WAV is PCM 16 bits, mono, 16 kHz: what the model was trained on. The engine
makes it with FFmpeg; another format is refused rather than read wrong.

--speech is a JSON list of [start, end] pairs, in samples: the stretches
where someone speaks, which the engine takes from the words it already has.
A window never crosses from one stretch to the next: a voice changes in a
pause far more often than inside a word. Without --speech, the stretches
are where the sound is within 35 dB of its loud parts.

The report is a JSON object:

    sample_rate   16000
    samples       the length of the sound
    speakers      N
    segments      [{"start": S, "end": E, "speaker": K}], in samples, in
                  order; K counts from 0 in order of first appearance
    seconds       how long each voice speaks, by K
    similarity    for each pair of voices, how alike their mean vectors are
                  (cosine, per mille): [[a, b, value], ...]. Two voices
                  told apart measured 380 to 800; one voice asked for two
                  measured 877, and the pairs the model confused 850 to
                  900. From 850 up, the two groups are one voice cut in
                  two or two voices it cannot tell apart: not to be relied
                  on either way.
    engine, engine_version, embedding, window_ms, hop_ms

Times are whole samples, never seconds in floating point: the engine turns
them into exact times (RationalTime).

Nothing is downloaded: the model file must be there.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
import wave

SAMPLE_RATE = 16000
# The shortest piece of sound a vector is taken from.
MIN_WINDOW = SAMPLE_RATE // 2


class DiarizeError(Exception):
    """A refusal the engine reports as it is."""


def read_frames(path: pathlib.Path) -> bytes:
    """The sound's samples as stored, once its format is the one the model
    takes. No numpy: a wrong file is refused before anything is loaded."""
    try:
        with wave.open(str(path), "rb") as handle:
            channels = handle.getnchannels()
            width = handle.getsampwidth()
            rate = handle.getframerate()
            frames = handle.readframes(handle.getnframes())
    except (wave.Error, OSError, EOFError) as error:
        raise DiarizeError(f"cannot read the sound '{path}': {error}")
    if channels != 1 or width != 2 or rate != SAMPLE_RATE:
        raise DiarizeError(
            f"'{path}' is {channels} channel(s), {8 * width} bits, {rate} Hz: "
            f"the model takes mono, 16 bits, {SAMPLE_RATE} Hz"
        )
    if len(frames) // 2 < MIN_WINDOW:
        raise DiarizeError("the sound is shorter than half a second")
    return frames


def loud_stretches(samples):
    """Where the sound is within 35 dB of its loud parts, by 20 ms frames;
    holes under 0.3 s closed, stretches under 0.3 s dropped."""
    import numpy

    frame = SAMPLE_RATE // 50
    count = len(samples) // frame
    if count == 0:
        return []
    power = (samples[: count * frame].reshape(count, frame) ** 2).mean(axis=1)
    loud = float(numpy.percentile(power, 95))
    if loud <= 0.0:
        return []
    speech = power > loud * 10.0 ** (-35.0 / 10.0)
    stretches = []
    start = None
    for index in range(count + 1):
        on = index < count and bool(speech[index])
        if on and start is None:
            start = index
        if not on and start is not None:
            stretches.append([start * frame, index * frame])
            start = None
    merged = []
    for stretch in stretches:
        if merged and stretch[0] - merged[-1][1] < 0.3 * SAMPLE_RATE:
            merged[-1][1] = stretch[1]
        else:
            merged.append(stretch)
    return [s for s in merged if s[1] - s[0] >= 0.3 * SAMPLE_RATE]


def read_stretches(path: pathlib.Path, length: int):
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise DiarizeError(f"cannot read the speech stretches '{path}': {error}")
    stretches = []
    for item in raw if isinstance(raw, list) else []:
        if (
            not isinstance(item, list)
            or len(item) != 2
            or not all(isinstance(value, int) for value in item)
        ):
            raise DiarizeError(
                "--speech must be a list of [start, end] pairs of samples"
            )
        start, end = max(0, item[0]), min(length, item[1])
        if end > start:
            stretches.append([start, end])
    stretches.sort()
    merged = []
    for stretch in stretches:
        if merged and stretch[0] <= merged[-1][1]:
            merged[-1][1] = max(merged[-1][1], stretch[1])
        else:
            merged.append(stretch)
    return merged


def spherical_kmeans(vectors, count: int):
    """`vectors` (unit length) in `count` groups: the centres are means
    brought back to unit length, a vector goes to the centre it is most
    alike. Eight starts, each from the vectors least alike; the start whose
    vectors end nearest their centres is kept. Seeded: the same sound gives
    the same answer."""
    import numpy

    random = numpy.random.default_rng(0)
    best = None
    for _ in range(8):
        centres = [vectors[int(random.integers(len(vectors)))]]
        while len(centres) < count:
            nearest = numpy.max(
                numpy.stack([vectors @ centre for centre in centres]), axis=0
            )
            centres.append(vectors[int(numpy.argmin(nearest))])
        centres = numpy.stack(centres)
        labels = numpy.zeros(len(vectors), dtype=int)
        for _ in range(50):
            labels = numpy.argmax(vectors @ centres.T, axis=1)
            moved = centres.copy()
            for group in range(count):
                members = vectors[labels == group]
                if len(members):
                    mean = members.mean(axis=0)
                    norm = float(numpy.linalg.norm(mean))
                    if norm > 0.0:
                        moved[group] = mean / norm
            if numpy.allclose(moved, centres):
                break
            centres = moved
        score = float(numpy.sum(numpy.max(vectors @ centres.T, axis=1)))
        if best is None or score > best[0]:
            best = (score, labels.copy(), centres.copy())
    return best[1], best[2]


def diarize(arguments) -> dict:
    embedding = pathlib.Path(arguments.embedding)
    if not embedding.is_file():
        raise DiarizeError(f"the model '{embedding}' is not there")
    frames = read_frames(pathlib.Path(arguments.audio))

    import numpy

    samples = numpy.frombuffer(frames, dtype="<i2").astype(numpy.float32) / 32768.0
    stretches = (
        read_stretches(pathlib.Path(arguments.speech), len(samples))
        if arguments.speech
        else loud_stretches(samples)
    )
    if not stretches:
        raise DiarizeError("no stretch of speech in this sound")
    window = int(round(arguments.window * SAMPLE_RATE))
    hop = int(round(arguments.hop * SAMPLE_RATE))
    count = arguments.speakers

    # The windows: inside each stretch, one every hop, cut at its end. A
    # stretch shorter than a window is one window; shorter than half a
    # second, it is widened to that, evenly, to have a voice to read.
    windows = []
    for index, (start, end) in enumerate(stretches):
        if end - start < MIN_WINDOW:
            middle = (start + end) // 2
            low = max(0, min(middle - MIN_WINDOW // 2, len(samples) - MIN_WINDOW))
            windows.append((index, start, end, low, low + MIN_WINDOW))
            continue
        at = start
        while True:
            stop = min(at + window, end)
            if stop - at >= MIN_WINDOW or at == start:
                windows.append((index, at, stop, at, stop))
            if stop >= end:
                break
            at += hop

    engine_version = ""
    if count == 1:
        # One voice: nothing to tell apart, and no model to load.
        labels = [0] * len(windows)
        centres = None
    else:
        import sherpa_onnx

        engine_version = sherpa_onnx.__version__
        extractor = sherpa_onnx.SpeakerEmbeddingExtractor(
            sherpa_onnx.SpeakerEmbeddingExtractorConfig(
                model=str(embedding), num_threads=arguments.threads
            )
        )
        vectors = []
        for done, (_, _, _, low, high) in enumerate(windows):
            stream = extractor.create_stream()
            stream.accept_waveform(SAMPLE_RATE, samples[low:high])
            stream.input_finished()
            vector = numpy.array(extractor.compute(stream), dtype=numpy.float64)
            norm = float(numpy.linalg.norm(vector))
            vectors.append(vector / norm if norm > 0.0 else vector)
            if done % 20 == 0:
                # One line per step, for the engine's task to follow.
                print(f"progress {done} {len(windows)}", file=sys.stderr, flush=True)
        vectors = numpy.stack(vectors)
        if len(vectors) < count:
            raise DiarizeError(
                f"{len(vectors)} window(s) of speech cannot hold {count} voices"
            )
        labels, centres = spherical_kmeans(vectors, count)

    # Each hop of each stretch goes to the voice most of the windows over it
    # carry; consecutive hops of one voice make a segment.
    segments = []
    for index, (start, end) in enumerate(stretches):
        mine = [
            (low, high, label)
            for (owner, low, high, _, _), label in zip(windows, labels)
            if owner == index
        ]
        at = start
        while at < end:
            stop = min(at + hop, end)
            votes = [0] * count
            for low, high, label in mine:
                if low < stop and high > at:
                    votes[int(label)] += 1
            speaker = max(range(count), key=lambda group: votes[group])
            if segments and segments[-1][2] == speaker and segments[-1][1] == at:
                segments[-1][1] = stop
            else:
                segments.append([at, stop, speaker])
            at = stop

    # Voices renumbered in order of first appearance: the grouping's own
    # numbers mean nothing, and a stable order is what a cache compares.
    order: dict[int, int] = {}
    listed = []
    seconds: dict[int, int] = {}
    for start, end, speaker in segments:
        voice = order.setdefault(speaker, len(order))
        listed.append({"start": int(start), "end": int(end), "speaker": voice})
        seconds[voice] = seconds.get(voice, 0) + (end - start)
    similarity = []
    if centres is not None:
        for a in order:
            for b in order:
                if order[a] < order[b]:
                    similarity.append(
                        [
                            order[a],
                            order[b],
                            int(round(1000.0 * float(centres[a] @ centres[b]))),
                        ]
                    )
        similarity.sort()
    return {
        "sample_rate": SAMPLE_RATE,
        "samples": int(len(samples)),
        "speakers": count,
        "speakers_heard": len(order),
        "segments": listed,
        "seconds": [
            round(seconds.get(voice, 0) / SAMPLE_RATE, 2) for voice in range(len(order))
        ],
        "similarity": similarity,
        "engine": "sherpa-onnx",
        "engine_version": engine_version,
        "embedding": embedding.name,
        "window_ms": int(round(arguments.window * 1000)),
        "hop_ms": int(round(arguments.hop * 1000)),
        "speech_from": "words" if arguments.speech else "loudness",
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--audio", required=True)
    parser.add_argument("--embedding", required=True)
    parser.add_argument("--speakers", type=int, required=True)
    parser.add_argument("--speech", default="")
    parser.add_argument("--window", type=float, default=1.5)
    parser.add_argument("--hop", type=float, default=0.25)
    parser.add_argument("--threads", type=int, default=2)
    parser.add_argument("--report", required=True)
    arguments = parser.parse_args()
    if arguments.speakers < 1 or arguments.speakers > 8:
        print("--speakers must lie in [1, 8]", file=sys.stderr)
        return 2
    if not 0.5 <= arguments.window <= 10.0 or not 0.05 <= arguments.hop <= arguments.window:
        print(
            "--window must lie in [0.5, 10] seconds and --hop in [0.05, window]",
            file=sys.stderr,
        )
        return 2
    try:
        report = diarize(arguments)
    except DiarizeError as error:
        print(str(error), file=sys.stderr)
        return 1
    except ImportError as error:
        print(
            f"this Python lacks a module the diarization needs: {error}",
            file=sys.stderr,
        )
        return 1
    pathlib.Path(arguments.report).write_text(
        json.dumps(report, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
