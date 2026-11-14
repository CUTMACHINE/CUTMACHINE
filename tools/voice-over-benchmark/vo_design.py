"""Step 1: VoiceDesign makes a reference clip, plus two uncloned control takes.

The control takes use the same description as the reference: they show how
much the voice drifts between calls when nothing is cloned.

Usage: vo_design.py <model_dir> <out_dir> <texte.txt>
"""
import pathlib
import sys
import time

import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel

INSTRUCT = (
    "A warm, calm French male voice in his thirties, documentary voice-over "
    "style, moderate pace, clear diction."
)
REF_TEXT = (
    "Bonjour. Je vais vous raconter une histoire. Prenez le temps d'écouter, "
    "et laissez-vous porter par les images."
)

model_dir, out_dir, text_path = sys.argv[1:4]
out = pathlib.Path(out_dir)
text = pathlib.Path(text_path).read_text(encoding="utf-8").strip()

t0 = time.time()
model = Qwen3TTSModel.from_pretrained(model_dir, device_map="mps",
                                      dtype=torch.bfloat16)
print(f"load={time.time() - t0:.1f}s", flush=True)

jobs = [("vo_reference", REF_TEXT), ("vo_design_take1", text),
        ("vo_design_take2", text)]
for name, body in jobs:
    t = time.time()
    wavs, sr = model.generate_voice_design(text=body, instruct=INSTRUCT,
                                           language="French",
                                           max_new_tokens=2048)
    sf.write(out / f"{name}.wav", wavs[0], sr)
    dur = len(wavs[0]) / sr
    print(f"{name}: gen={time.time() - t:.1f}s audio={dur:.1f}s", flush=True)

(out / "vo_reference.txt").write_text(REF_TEXT, encoding="utf-8")
