#!/usr/bin/env python3
"""Transcribe a NeMo-style manifest with Qwen3-ASR or Voxtral (transformers).

The transcription bench compares what each model writes on the same chunks.
This script runs in the model's own environment, never in the harness, reads
the manifest's 16 kHz mono WAVs itself (no audio library) and writes
`<output-dir>/transcripts.jsonl` (stem, text) and `run.json`. `--prompt`
hands Qwen3-ASR a context, as the engine hands Whisper its filler prompt.
"""

from __future__ import annotations

import argparse
import json
import sys
import time
import wave
from pathlib import Path

import numpy as np
import torch
from transformers import AutoModelForMultimodalLM, AutoProcessor

FAMILIES = {
    "qwen3-asr": "Qwen/Qwen3-ASR-1.7B-hf",
    "voxtral": "mistralai/Voxtral-Mini-3B-2507",
}


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        if (handle.getnchannels(), handle.getsampwidth(),
                handle.getframerate()) != (1, 2, 16000):
            raise SystemExit(f"{path}: expected 16 kHz mono 16-bit PCM")
        frames = handle.readframes(handle.getnframes())
    return np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--family", required=True, choices=sorted(FAMILIES))
    parser.add_argument("--revision", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--device", default="mps", choices=["cpu", "mps"])
    parser.add_argument("--prompt")
    args = parser.parse_args(argv)

    model_id = FAMILIES[args.family]
    dtype = torch.float32 if args.device == "cpu" else torch.bfloat16
    processor = AutoProcessor.from_pretrained(model_id, revision=args.revision)
    if args.family == "voxtral":
        from transformers import VoxtralForConditionalGeneration as Model
    else:
        Model = AutoModelForMultimodalLM
    model = Model.from_pretrained(model_id, revision=args.revision,
                                  dtype=dtype).to(args.device)
    model.eval()

    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = {"model": model_id, "revision": args.revision,
              "device": args.device, "dtype": str(dtype),
              "prompt": args.prompt, "chunks": []}
    lines = [json.loads(line) for line in
             args.manifest.read_text(encoding="utf-8").splitlines() if line]
    with open(args.output_dir / "transcripts.jsonl", "w",
              encoding="utf-8") as output:
        for line in lines:
            path = Path(line["audio_filepath"])
            audio = read_wav(path)
            started = time.monotonic()
            if args.family == "voxtral":
                inputs = processor.apply_transcription_request(
                    language="fr", audio=audio, format="wav",
                    sampling_rate=16000, model_id=model_id)
            else:
                extra = {"prompt": args.prompt} if args.prompt else {}
                inputs = processor.apply_transcription_request(
                    audio=audio, language="fr", **extra)
            inputs = inputs.to(args.device, dtype)
            with torch.inference_mode():
                ids = model.generate(**inputs, max_new_tokens=1024,
                                     do_sample=False)
            generated = ids[:, inputs["input_ids"].shape[1]:]
            if args.family == "voxtral":
                text = processor.batch_decode(
                    generated, skip_special_tokens=True)[0]
            else:
                text = processor.decode(
                    generated, return_format="transcription_only")[0]
            wall_ms = round((time.monotonic() - started) * 1000)
            output.write(json.dumps({"stem": path.stem, "text": text.strip()},
                                    ensure_ascii=False) + "\n")
            output.flush()
            report["chunks"].append({"stem": path.stem, "wall_time_ms": wall_ms})
            print(f"{path.stem}: {wall_ms} ms", file=sys.stderr)
    (args.output_dir / "run.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
