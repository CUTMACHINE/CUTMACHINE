#!/usr/bin/env python3
"""Run Qwen3-ForcedAligner over a NeMo-style manifest and write word CTMs.

The bench compares aligners on the same audio and the same text, so this
script takes the JSONL manifest NeMo Forced Aligner reads (`audio_filepath`,
`text`) and writes one CTM per line that `benchmark.py normalize-qwen` turns
into a bench manifest. It runs in its own environment (torch and
transformers), never in the engine's or NeMo's: the harness itself stays free
of model dependencies.

The model predicts each word boundary as a class of `timestamp_segment_time`
milliseconds (80 for the checkpoint measured), so its output sits on an 80 ms
grid whatever the frame rate of the video.
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
from transformers import AutoModelForTokenClassification, AutoProcessor

MODEL_ID = "Qwen/Qwen3-ForcedAligner-0.6B-hf"

# The aligner keeps letters, digits and the ASCII apostrophe only: a
# typographic apostrophe would glue « l’objectif » into one unknown word.
APOSTROPHES = str.maketrans({"’": "'", "ʼ": "'"})


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        if handle.getnchannels() != 1 or handle.getsampwidth() != 2:
            raise SystemExit(f"{path}: expected mono 16-bit PCM")
        if handle.getframerate() != 16000:
            raise SystemExit(f"{path}: expected 16 kHz")
        frames = handle.readframes(handle.getnframes())
    return np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--language", default="fr")
    parser.add_argument("--device", default="cpu", choices=["cpu", "mps"])
    args = parser.parse_args(argv)

    dtype = torch.float32 if args.device == "cpu" else torch.bfloat16
    loading = time.monotonic()
    processor = AutoProcessor.from_pretrained(MODEL_ID, revision=args.revision)
    model = AutoModelForTokenClassification.from_pretrained(
        MODEL_ID, revision=args.revision, dtype=dtype
    ).to(args.device)
    model.eval()
    load_ms = round((time.monotonic() - loading) * 1000)

    args.output_dir.mkdir(parents=True, exist_ok=True)
    report = {"model": MODEL_ID, "revision": args.revision,
              "device": args.device, "dtype": str(dtype),
              "torch": torch.__version__, "load_ms": load_ms, "samples": []}
    for line in args.manifest.read_text(encoding="utf-8").splitlines():
        if not line.strip():
            continue
        entry = json.loads(line)
        audio_path = Path(entry["audio_filepath"])
        sample_id = audio_path.stem
        audio = read_wav(audio_path)
        text = entry["text"].translate(APOSTROPHES)

        started = time.monotonic()
        inputs, word_lists = processor.prepare_forced_aligner_inputs(
            audio=audio, transcript=text, language=args.language
        )
        inputs = inputs.to(args.device)
        if "input_features" in inputs:
            inputs["input_features"] = inputs["input_features"].to(dtype)
        with torch.inference_mode():
            logits = model(**inputs).logits
        items = processor.decode_forced_alignment(
            logits=logits.float(),
            input_ids=inputs["input_ids"],
            word_lists=word_lists,
            timestamp_token_id=model.config.timestamp_token_id,
        )[0]
        wall_ms = round((time.monotonic() - started) * 1000)

        lines = []
        for item in items:
            start = item["start_time"]
            duration = max(0.0, item["end_time"] - start)
            lines.append(f"{sample_id} 1 {start:.3f} {duration:.3f} "
                         f"{item['text']}")
        ctm = args.output_dir / f"{sample_id}.ctm"
        ctm.write_text("\n".join(lines) + "\n", encoding="utf-8")
        report["samples"].append({"sample_id": sample_id,
                                  "words": len(items),
                                  "audio_seconds": len(audio) / 16000,
                                  "wall_time_ms": wall_ms})
        print(f"{sample_id}: {len(items)} mots, {wall_ms} ms", file=sys.stderr)

    (args.output_dir / "run.json").write_text(
        json.dumps(report, ensure_ascii=False, indent=2) + "\n",
        encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
