#!/usr/bin/env python3
"""GN6a -- docs/roadmap/GENERATIF-2026-10.md. scripts/voice_over.py with
Qwen3-TTS replaced by a fake backend: what it writes (a 24-bit mono WAV and
its report), the revision it reads, what it hands the model, and its
refusals. The real model only runs on a machine that has it."""

from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
import wave

ROOT = pathlib.Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location(
    "voice_over", ROOT / "scripts" / "voice_over.py")
voice_over = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(voice_over)

COMMIT = "fd4b254389122332181a7c3db7f27e918eec64e3"


class FakeBackend:
    """Stands in for QwenBackend and remembers what it was asked."""

    calls = []

    def __init__(self, model_dir):
        self.device = "cpu"
        FakeBackend.calls.append(("load", str(model_dir)))

    def seed(self, value):
        FakeBackend.calls.append(("seed", value))

    def design(self, text, description, language):
        FakeBackend.calls.append(("design", text, description, language))
        return [0.0, 0.5, -0.5, 1.5, -1.5], 24000

    def clone(self, text, reference, reference_text, language):
        FakeBackend.calls.append(
            ("clone", text, reference, reference_text, language))
        return [0.25] * 2400, 24000


class VoiceOverScriptTests(unittest.TestCase):

    def setUp(self):
        FakeBackend.calls = []
        self.directory = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.directory.name)
        self.model = self.root / "Qwen3-TTS-12Hz-1.7B-VoiceDesign"
        self.model.mkdir()
        (self.model / "config.json").write_text("{}", encoding="utf-8")
        self.out = self.root / "out" / "voice.wav"
        self.report = self.root / "out" / "voice.json"

    def tearDown(self):
        self.directory.cleanup()

    def design(self, *extra, text="Le 21 mars, à 10 h."):
        return ["design", "--model", str(self.model), "--text", text,
                "--language", "French", "--seed", "7", "--out", str(self.out),
                "--report", str(self.report), "--description",
                "A warm male voice", *extra]

    def run_main(self, argv):
        stderr = io.StringIO()
        with contextlib.redirect_stderr(stderr):
            code = voice_over.main(argv, FakeBackend)
        return code, stderr.getvalue()

    def test_design_writes_a_24_bit_wav_and_its_report(self):
        (self.model / "REVISION").write_text(COMMIT + "\n", encoding="utf-8")
        code, stderr = self.run_main(self.design())
        self.assertEqual(code, 0, stderr)
        with wave.open(str(self.out), "rb") as wav:
            self.assertEqual(wav.getnchannels(), 1)
            self.assertEqual(wav.getsampwidth(), 3)
            self.assertEqual(wav.getframerate(), 24000)
            self.assertEqual(wav.getnframes(), 5)
            frames = wav.readframes(5)
        # 0, half scale, minus half scale, then clipped to full scale.
        samples = [int.from_bytes(frames[i:i + 3], "little", signed=True)
                   for i in range(0, 15, 3)]
        self.assertEqual(samples, [0, 4194304, -4194304, 8388607, -8388607])
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report, {
            "sample_rate": 24000, "samples": 5,
            "model": "Qwen3-TTS-12Hz-1.7B-VoiceDesign", "revision": COMMIT,
            "device": "cpu", "seed": 7})
        self.assertEqual(FakeBackend.calls, [
            ("load", str(self.model)), ("seed", 7),
            ("design", "Le 21 mars, à 10 h.", "A warm male voice",
             "French")])

    def test_the_text_goes_to_the_model_as_written(self):
        text = "Venez à 17 h — « on vous attend »."
        code, stderr = self.run_main(self.design(text=text))
        self.assertEqual(code, 0, stderr)
        self.assertEqual(FakeBackend.calls[-1][1], text)

    def test_revision_from_the_hub_download_metadata(self):
        metadata = self.model / ".cache" / "huggingface" / "download"
        metadata.mkdir(parents=True)
        (metadata / "config.json.metadata").write_text(
            f"{COMMIT}\n\"etag\"\n1730000000.0\n", encoding="utf-8")
        self.assertEqual(voice_over.model_revision(self.model), COMMIT)
        (metadata / "config.json.metadata").write_text(
            "not-a-commit\n", encoding="utf-8")
        self.assertEqual(voice_over.model_revision(self.model), "")

    def test_no_revision_says_empty(self):
        self.assertEqual(voice_over.model_revision(self.model), "")

    def test_clone_hands_the_reference_and_its_words(self):
        reference = self.root / "reference.wav"
        voice_over.write_wav_24(reference, [0.1] * 10, 24000)
        argv = ["clone", "--model", str(self.model), "--text", "Bonjour.",
                "--language", "French", "--seed", "0", "--out", str(self.out),
                "--report", str(self.report), "--reference", str(reference),
                "--reference-text", "Prenez le temps d'écouter."]
        code, stderr = self.run_main(argv)
        self.assertEqual(code, 0, stderr)
        self.assertEqual(FakeBackend.calls[-1], (
            "clone", "Bonjour.", str(reference), "Prenez le temps d'écouter.",
            "French"))
        report = json.loads(self.report.read_text(encoding="utf-8"))
        self.assertEqual(report["samples"], 2400)

    def test_refusals_start_nothing(self):
        cases = {
            "empty text": self.design(text="   "),
            "seed": [*self.design()[:7], "--seed", "-1", *self.design()[9:]],
        }
        for name, argv in cases.items():
            with self.subTest(name):
                FakeBackend.calls = []
                code, stderr = self.run_main(argv)
                self.assertEqual(code, 1)
                self.assertTrue(stderr.startswith("voice_over.py: "), stderr)
                self.assertEqual(FakeBackend.calls, [])
                self.assertFalse(self.out.exists())

        (self.model / "config.json").unlink()
        code, stderr = self.run_main(self.design())
        self.assertEqual(code, 1)
        self.assertIn("is not a model folder", stderr)

    def test_clone_refuses_a_missing_reference(self):
        argv = ["clone", "--model", str(self.model), "--text", "Bonjour.",
                "--language", "French", "--seed", "0", "--out", str(self.out),
                "--report", str(self.report), "--reference",
                str(self.root / "absent.wav"), "--reference-text", "Mots."]
        code, stderr = self.run_main(argv)
        self.assertEqual(code, 1)
        self.assertIn("does not exist", stderr)
        self.assertEqual(FakeBackend.calls, [])

    def test_numpy_and_plain_paths_write_the_same_bytes(self):
        try:
            import numpy as np
        except ImportError:
            self.skipTest("numpy is not installed here")
        # What the model returns: float32. The plain path is given the same
        # values as Python floats.
        values = np.array([0.0, 0.123, -0.987, 1.0, -1.0, 2.0, 1e-7],
                          dtype=np.float32)
        plain = self.root / "plain.wav"
        fast = self.root / "fast.wav"
        voice_over.write_wav_24(plain, [float(v) for v in values], 24000)
        voice_over.write_wav_24(fast, values, 24000)
        self.assertEqual(plain.read_bytes(), fast.read_bytes())

    def test_no_sound_is_refused(self):
        with self.assertRaises(voice_over.VoiceOverError):
            voice_over.write_wav_24(self.out, [], 24000)


if __name__ == "__main__":
    unittest.main()
