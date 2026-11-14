#!/usr/bin/env python3
"""Recognize phones over a NeMo-style manifest with a CTC phone model.

The transcription bench looks for the words a transcript leaves out (« euh »,
repetitions) in what a phone recognizer hears. This script runs in its own
environment, never in the harness, reads 16 kHz mono WAVs itself and writes
`<output-dir>/phones.jsonl`: per chunk, each emitted phone with the seconds
of its run of frames (greedy CTC: argmax per frame, repeats collapsed,
blanks dropped). Symbols come from the checkpoint's vocab.json, so no
phonemizer backend is needed.
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
from huggingface_hub import hf_hub_download
from transformers import AutoFeatureExtractor, AutoModelForCTC


def read_wav(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as handle:
        if (handle.getnchannels(), handle.getsampwidth(),
                handle.getframerate()) != (1, 2, 16000):
            raise SystemExit(f"{path}: expected 16 kHz mono 16-bit PCM")
        frames = handle.readframes(handle.getnframes())
    return np.frombuffer(frames, dtype="<i2").astype(np.float32) / 32768.0


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", required=True)
    parser.add_argument("--revision", required=True)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args(argv)

    vocab_path = hf_hub_download(args.model, "vocab.json",
                                 revision=args.revision)
    vocab = json.loads(Path(vocab_path).read_text(encoding="utf-8"))
    symbols = {index: symbol for symbol, index in vocab.items()}
    extractor = AutoFeatureExtractor.from_pretrained(args.model,
                                                     revision=args.revision)
    model = AutoModelForCTC.from_pretrained(args.model, revision=args.revision)
    model.eval()
    blank = model.config.pad_token_id
    stride = int(np.prod(model.config.conv_stride))
    seconds_per_frame = stride / 16000

    args.output_dir.mkdir(parents=True, exist_ok=True)
    lines = [json.loads(line) for line in
             args.manifest.read_text(encoding="utf-8").splitlines() if line]
    started_all = time.monotonic()
    with open(args.output_dir / "phones.jsonl", "w", encoding="utf-8") as out:
        for line in lines:
            path = Path(line["audio_filepath"])
            inputs = extractor(read_wav(path), sampling_rate=16000,
                               return_tensors="pt")
            with torch.inference_mode():
                logits = model(**inputs).logits[0]
            best = logits.argmax(dim=-1).tolist()
            phones, frame = [], 0
            while frame < len(best):
                token = best[frame]
                run = frame
                while run < len(best) and best[run] == token:
                    run += 1
                if token != blank:
                    phones.append([symbols.get(token, "?"),
                                   round(frame * seconds_per_frame, 3),
                                   round(run * seconds_per_frame, 3)])
                frame = run
            out.write(json.dumps({"stem": path.stem, "phones": phones},
                                 ensure_ascii=False) + "\n")
            print(f"{path.stem}: {len(phones)} phones", file=sys.stderr)
    (args.output_dir / "run.json").write_text(json.dumps({
        "model": args.model, "revision": args.revision,
        "seconds_per_frame": seconds_per_frame, "blank_id": blank,
        "wall_time_ms": round((time.monotonic() - started_all) * 1000),
        "torch": torch.__version__,
    }, indent=2) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
