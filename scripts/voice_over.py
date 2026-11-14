#!/usr/bin/env python3
"""A voice-over said by Qwen3-TTS, on this machine.

GN6a -- docs/roadmap/GENERATIF-2026-10.md. The engine runs this script with
the Python of the model's environment (VoiceOver.h); running it by hand
works too. Two modes, one model each:

    python3 voice_over.py design --model DIR --description TEXT \
        --text TEXT --language NAME --seed N --out WAV --report JSON
    python3 voice_over.py clone --model DIR --reference WAV \
        --reference-text TEXT --text TEXT --language NAME --seed N \
        --out WAV --report JSON

`design` runs Qwen3-TTS-12Hz-1.7B-VoiceDesign: TEXT said in a voice made
from a written description. It draws a new voice at every call. `clone` runs
Qwen3-TTS-12Hz-1.7B-Base: TEXT said in the voice of a reference clip whose
words are given, which keeps the voice from one take to the next (the
"voice design then clone" path Qwen documents; on the bench of 7 November
2026, two cloned takes were at 0.988 speaker similarity, two uncloned ones
at 0.850).

DIR is a model folder as `hf download --local-dir` writes it. NAME is the
language as Qwen3-TTS names it ("French"); the engine maps ISO codes to
these names and refuses the others before starting anything.

The WAV is PCM 24 bits, mono, at the model's rate (24 kHz). The report is a
JSON object: sample_rate, samples, model (the folder's name), revision (the
Hugging Face commit the folder was downloaded at, "" when it does not say),
device and seed.

The device is the Mac's GPU (mps) when torch has it, else CUDA, else the
CPU; CUTMACHINE_VOICE_DEVICE forces one. Weights are bfloat16 on a GPU and
float32 on the CPU. torch.manual_seed(seed) is set before generating; the
GPU does not promise the same samples twice for the same seed.

Nothing is downloaded: the model folders must be complete (the engine
checks config.json, model.safetensors and speech_tokenizer/model.safetensors
before starting this). HF_HUB_OFFLINE is set so that a missing file fails
here instead of being fetched.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import struct
import sys
import wave

# 12 codec frames per second: 4096 frames is about 5 minutes of speech, far
# beyond a voice-over line. Generation stops at its end-of-speech token.
MAX_NEW_TOKENS = 4096

FULL_SCALE_24 = 8388607


class VoiceOverError(Exception):
    """A refusal said in one sentence, on the error output."""


def model_revision(model_dir: pathlib.Path) -> str:
    """The Hugging Face commit the folder was downloaded at, or ""."""
    explicit = model_dir / "REVISION"
    if explicit.is_file():
        return explicit.read_text(encoding="utf-8").strip()
    # `hf download --local-dir` keeps, per file, the commit it came from.
    metadata = model_dir / ".cache" / "huggingface" / "download"
    metadata = metadata / "config.json.metadata"
    if metadata.is_file():
        first = metadata.read_text(encoding="utf-8").splitlines()[:1]
        if first and re.fullmatch(r"[0-9a-f]{40}", first[0].strip()):
            return first[0].strip()
    return ""


def write_wav_24(path: pathlib.Path, samples, sample_rate: int) -> int:
    """Writes mono PCM 24 bits; returns the number of samples written.

    Samples are floats in [-1, 1]; beyond, they are clipped."""
    if hasattr(samples, "dtype"):
        import numpy as np

        scaled = np.clip(np.asarray(samples, dtype=np.float64).reshape(-1),
                         -1.0, 1.0)
        ints = np.round(scaled * FULL_SCALE_24).astype("<i4")
        frames = ints.view(np.uint8).reshape(-1, 4)[:, :3].tobytes()
        count = int(ints.shape[0])
    else:
        packed = bytearray()
        count = 0
        for value in samples:
            value = max(-1.0, min(1.0, float(value)))
            packed += struct.pack("<i", int(round(value * FULL_SCALE_24)))[:3]
            count += 1
        frames = bytes(packed)
    if count == 0:
        raise VoiceOverError("the model returned no sound")
    path.parent.mkdir(parents=True, exist_ok=True)
    with wave.open(str(path), "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(3)
        output.setframerate(int(sample_rate))
        output.writeframes(frames)
    return count


def pick_device() -> str:
    forced = os.environ.get("CUTMACHINE_VOICE_DEVICE", "").strip()
    if forced:
        return forced
    import torch

    if torch.backends.mps.is_available():
        return "mps"
    if torch.cuda.is_available():
        return "cuda"
    return "cpu"


class QwenBackend:
    """Qwen3-TTS through the qwen-tts package, imported only when used."""

    def __init__(self, model_dir: pathlib.Path):
        os.environ.setdefault("HF_HUB_OFFLINE", "1")
        import torch
        from qwen_tts import Qwen3TTSModel

        self.device = pick_device()
        dtype = torch.float32 if self.device == "cpu" else torch.bfloat16
        self._torch = torch
        self._model = Qwen3TTSModel.from_pretrained(
            str(model_dir), device_map=self.device, dtype=dtype)

    def seed(self, value: int) -> None:
        self._torch.manual_seed(value)

    def design(self, text: str, description: str, language: str):
        wavs, rate = self._model.generate_voice_design(
            text=text, instruct=description, language=language,
            max_new_tokens=MAX_NEW_TOKENS)
        return wavs[0], rate

    def clone(self, text: str, reference: str, reference_text: str,
              language: str):
        prompt = self._model.create_voice_clone_prompt(
            ref_audio=reference, ref_text=reference_text)
        wavs, rate = self._model.generate_voice_clone(
            text=text, language=language, voice_clone_prompt=prompt,
            max_new_tokens=MAX_NEW_TOKENS)
        return wavs[0], rate


def parse_arguments(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    modes = parser.add_subparsers(dest="mode", required=True)
    for name in ("design", "clone"):
        mode = modes.add_parser(name)
        mode.add_argument("--model", required=True, type=pathlib.Path)
        mode.add_argument("--text", required=True)
        mode.add_argument("--language", required=True)
        mode.add_argument("--seed", required=True, type=int)
        mode.add_argument("--out", required=True, type=pathlib.Path)
        mode.add_argument("--report", required=True, type=pathlib.Path)
        if name == "design":
            mode.add_argument("--description", required=True)
        else:
            mode.add_argument("--reference", required=True)
            mode.add_argument("--reference-text", required=True)
    return parser.parse_args(argv)


def run(arguments, backend_factory=QwenBackend) -> dict:
    if not arguments.text.strip():
        raise VoiceOverError("the text to say is empty")
    if not 0 <= arguments.seed <= 0xFFFFFFFF:
        raise VoiceOverError(f"the seed {arguments.seed} is not a 32-bit "
                             "unsigned integer")
    if not (arguments.model / "config.json").is_file():
        raise VoiceOverError(f"'{arguments.model}' is not a model folder "
                             "(no config.json)")
    if arguments.mode == "clone":
        if not pathlib.Path(arguments.reference).is_file():
            raise VoiceOverError(f"the reference clip '{arguments.reference}'"
                                 " does not exist")
        if not arguments.reference_text.strip():
            raise VoiceOverError("the reference clip's words are empty")
    elif not arguments.description.strip():
        raise VoiceOverError("a voice is made from a description, and it is "
                             "empty")

    backend = backend_factory(arguments.model)
    backend.seed(arguments.seed)
    if arguments.mode == "design":
        samples, rate = backend.design(arguments.text, arguments.description,
                                       arguments.language)
    else:
        samples, rate = backend.clone(arguments.text, arguments.reference,
                                      arguments.reference_text,
                                      arguments.language)
    count = write_wav_24(arguments.out, samples, rate)
    report = {
        "sample_rate": int(rate),
        "samples": count,
        "model": arguments.model.name,
        "revision": model_revision(arguments.model),
        "device": getattr(backend, "device", ""),
        "seed": arguments.seed,
    }
    arguments.report.parent.mkdir(parents=True, exist_ok=True)
    arguments.report.write_text(json.dumps(report, ensure_ascii=False),
                                encoding="utf-8")
    return report


def main(argv=None, backend_factory=QwenBackend) -> int:
    arguments = parse_arguments(argv)
    try:
        run(arguments, backend_factory)
    except VoiceOverError as error:
        print(f"voice_over.py: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
