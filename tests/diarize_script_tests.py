#!/usr/bin/env python3
"""PA3 -- docs/roadmap/PARITE-2026-11.md. scripts/diarize.py without its
model: what it refuses before loading anything (a count out of range, a
sound in another format, a model that is not there), how it reads the
stretches of speech the engine hands it, and -- where numpy is installed --
the grouping on vectors made for the purpose and a whole run for one voice.
The speaker embedding model only runs on a machine that has it; what it is
worth is measured by tools/speaker-diarization-benchmark."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import struct
import sys
import tempfile
import unittest
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "diarize", ROOT / "scripts" / "diarize.py")
diarize = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(diarize)

try:
    import numpy
except ImportError:
    numpy = None


def write_wave(path, seconds=2.0, rate=16000, channels=1, loud=(0.0, 2.0)):
    """A tone between loud[0] and loud[1] seconds, silence elsewhere."""
    import math

    with wave.open(str(path), "wb") as handle:
        handle.setnchannels(channels)
        handle.setsampwidth(2)
        handle.setframerate(rate)
        frames = bytearray()
        for index in range(int(seconds * rate)):
            at = index / rate
            value = (
                int(12000 * math.sin(2 * math.pi * 220 * at))
                if loud[0] <= at < loud[1]
                else 0
            )
            frames += struct.pack("<h", value) * channels
        handle.writeframes(bytes(frames))


def run(arguments):
    """main() on `arguments`: its exit code and what it printed as errors."""
    errors = io.StringIO()
    saved = sys.argv
    sys.argv = ["diarize.py", *arguments]
    try:
        with contextlib.redirect_stderr(errors):
            try:
                code = diarize.main()
            except SystemExit as stop:
                code = stop.code
    finally:
        sys.argv = saved
    return code, errors.getvalue()


class Refusals(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.folder.name)
        self.sound = self.root / "sound.wav"
        self.model = self.root / "model.onnx"
        self.report = self.root / "report.json"
        write_wave(self.sound)
        self.model.write_bytes(b"weights")

    def tearDown(self):
        self.folder.cleanup()

    def arguments(self, **changes):
        values = {
            "--audio": str(self.sound),
            "--embedding": str(self.model),
            "--speakers": "2",
            "--report": str(self.report),
        }
        values.update(changes)
        return [item for pair in values.items() for item in pair]

    def test_a_count_of_voices_out_of_range(self):
        for count in ("0", "9"):
            code, errors = run(self.arguments(**{"--speakers": count}))
            self.assertEqual(code, 2)
            self.assertIn("[1, 8]", errors)
        self.assertFalse(self.report.exists())

    def test_a_window_the_model_cannot_read(self):
        code, errors = run(self.arguments(**{"--window": "0.2"}))
        self.assertEqual(code, 2)
        self.assertIn("--window", errors)

    def test_a_model_that_is_not_there(self):
        code, errors = run(
            self.arguments(**{"--embedding": str(self.root / "absent.onnx")}))
        self.assertEqual(code, 1)
        self.assertIn("is not there", errors)

    def test_a_sound_in_another_format(self):
        stereo = self.root / "stereo.wav"
        write_wave(stereo, channels=2)
        code, errors = run(self.arguments(**{"--audio": str(stereo)}))
        self.assertEqual(code, 1)
        self.assertIn("2 channel(s)", errors)
        fast = self.root / "fast.wav"
        write_wave(fast, rate=48000)
        code, errors = run(self.arguments(**{"--audio": str(fast)}))
        self.assertEqual(code, 1)
        self.assertIn("48000 Hz", errors)
        short = self.root / "short.wav"
        write_wave(short, seconds=0.2)
        code, errors = run(self.arguments(**{"--audio": str(short)}))
        self.assertEqual(code, 1)
        self.assertIn("shorter than half a second", errors)
        code, errors = run(
            self.arguments(**{"--audio": str(self.root / "absent.wav")}))
        self.assertEqual(code, 1)
        self.assertIn("cannot read the sound", errors)


class Stretches(unittest.TestCase):
    def read(self, value, length=100000):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "speech.json"
            path.write_text(json.dumps(value), encoding="utf-8")
            return diarize.read_stretches(path, length)

    def test_stretches_are_ordered_merged_and_held_inside_the_sound(self):
        self.assertEqual(
            self.read([[5000, 9000], [0, 4000], [8000, 12000], [99000, 200000]]),
            [[0, 4000], [5000, 12000], [99000, 100000]])
        self.assertEqual(self.read([[10, 10], [200000, 300000]]), [])

    def test_anything_else_is_refused(self):
        for wrong in ([[1, 2, 3]], [[0.5, 2]], ["0-10"]):
            with self.assertRaises(diarize.DiarizeError):
                self.read(wrong)


@unittest.skipIf(numpy is None, "numpy is not installed in this Python")
class Grouping(unittest.TestCase):
    def test_two_groups_of_vectors_are_told_apart(self):
        random = numpy.random.default_rng(1)
        first = numpy.array([1.0, 0.0, 0.0]) + 0.05 * random.normal(size=(40, 3))
        second = numpy.array([0.0, 1.0, 0.0]) + 0.05 * random.normal(size=(25, 3))
        vectors = numpy.concatenate([first, second])
        vectors /= numpy.linalg.norm(vectors, axis=1, keepdims=True)
        labels, centres = diarize.spherical_kmeans(vectors, 2)
        self.assertEqual(len(set(labels[:40])), 1)
        self.assertEqual(len(set(labels[40:])), 1)
        self.assertNotEqual(labels[0], labels[40])
        self.assertLess(float(centres[0] @ centres[1]), 0.2)
        again, _ = diarize.spherical_kmeans(vectors, 2)
        self.assertEqual(list(labels), list(again))

    def test_the_loud_stretches_of_a_sound(self):
        with tempfile.TemporaryDirectory() as folder:
            path = pathlib.Path(folder) / "burst.wav"
            write_wave(path, seconds=3.0, loud=(1.0, 2.0))
            frames = diarize.read_frames(path)
            samples = numpy.frombuffer(frames, dtype="<i2").astype(
                numpy.float32) / 32768.0
            stretches = diarize.loud_stretches(samples)
            self.assertEqual(len(stretches), 1)
            self.assertAlmostEqual(stretches[0][0] / 16000, 1.0, delta=0.03)
            self.assertAlmostEqual(stretches[0][1] / 16000, 2.0, delta=0.03)

    def test_one_voice_needs_no_model(self):
        with tempfile.TemporaryDirectory() as folder:
            root = pathlib.Path(folder)
            sound = root / "voice.wav"
            write_wave(sound, seconds=3.0, loud=(0.5, 2.5))
            model = root / "model.onnx"
            model.write_bytes(b"weights")
            speech = root / "speech.json"
            speech.write_text("[[8000, 20000], [24000, 40000]]", encoding="utf-8")
            report = root / "report.json"
            code, errors = run([
                "--audio", str(sound), "--embedding", str(model),
                "--speakers", "1", "--speech", str(speech),
                "--report", str(report)])
            self.assertEqual(code, 0, errors)
            answer = json.loads(report.read_text(encoding="utf-8"))
            self.assertEqual(answer["speakers"], 1)
            self.assertEqual(answer["speakers_heard"], 1)
            self.assertEqual(answer["speech_from"], "words")
            self.assertEqual(
                answer["segments"],
                [{"start": 8000, "end": 20000, "speaker": 0},
                 {"start": 24000, "end": 40000, "speaker": 0}])
            self.assertEqual(answer["similarity"], [])
            self.assertEqual(answer["samples"], 48000)


if __name__ == "__main__":
    unittest.main()
