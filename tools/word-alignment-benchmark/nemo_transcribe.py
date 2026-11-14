#!/usr/bin/env python3
"""Transcribe a NeMo-style manifest with a NeMo ASR checkpoint.

The transcription bench compares what each model writes, fillers and
repetitions first, on the same chunks. This script runs in NeMo's own
environment (build/nemo-env), never in the harness: it reads the manifest's
`audio_filepath` lines and writes `<output-dir>/<decoder>.jsonl`, one line
per chunk with its stem and text. A hybrid checkpoint (RNNT and CTC heads)
is run once per head.
"""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path

from nemo.collections.asr import models

# A checkpoint whose config names no target class cannot be restored through
# ASRModel; the hybrid French FastConformer is one.
MODEL_CLASSES = {
    "asr": models.ASRModel,
    "hybrid": models.EncDecHybridRNNTCTCBPEModel,
}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True)
    parser.add_argument("--checkpoint", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--decoders", nargs="+", default=["default"],
                        choices=["default", "rnnt", "ctc"])
    parser.add_argument("--model-class", default="asr",
                        choices=sorted(MODEL_CLASSES))
    args = parser.parse_args(argv)

    lines = [json.loads(line) for line in
             args.manifest.read_text(encoding="utf-8").splitlines() if line]
    paths = [line["audio_filepath"] for line in lines]
    model = MODEL_CLASSES[args.model_class].restore_from(
        str(args.checkpoint), map_location="cpu")
    model.eval()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    for decoder in args.decoders:
        if decoder != "default":
            model.change_decoding_strategy(decoder_type=decoder)
        started = time.monotonic()
        hypotheses = model.transcribe(paths, batch_size=1, verbose=False)
        if isinstance(hypotheses, tuple):
            hypotheses = hypotheses[0]
        wall_ms = round((time.monotonic() - started) * 1000)
        with open(args.output_dir / f"{decoder}.jsonl", "w",
                  encoding="utf-8") as output:
            for path, hypothesis in zip(paths, hypotheses):
                text = getattr(hypothesis, "text", hypothesis)
                output.write(json.dumps({"stem": Path(path).stem,
                                         "text": text},
                                        ensure_ascii=False) + "\n")
        print(f"{args.checkpoint.stem} {decoder}: {len(paths)} chunks, "
              f"{wall_ms} ms")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
