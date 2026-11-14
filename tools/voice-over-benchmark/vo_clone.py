"""Step 2: Base clones the designed reference and says the text twice.

The clone prompt is built once and reused, as Qwen documents for a voice
that must stay the same across lines; each take is a separate call.

Usage: vo_clone.py <model_dir> <out_dir> <texte.txt>
"""
import pathlib
import sys
import time

import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel

model_dir, out_dir, text_path = sys.argv[1:4]
out = pathlib.Path(out_dir)
text = pathlib.Path(text_path).read_text(encoding="utf-8").strip()
ref_text = (out / "vo_reference.txt").read_text(encoding="utf-8").strip()

t0 = time.time()
model = Qwen3TTSModel.from_pretrained(model_dir, device_map="mps",
                                      dtype=torch.bfloat16)
print(f"load={time.time() - t0:.1f}s", flush=True)

prompt = model.create_voice_clone_prompt(
    ref_audio=str(out / "vo_reference.wav"), ref_text=ref_text)

for name in ("vo_clone_take1", "vo_clone_take2"):
    t = time.time()
    wavs, sr = model.generate_voice_clone(text=text, language="French",
                                          voice_clone_prompt=prompt,
                                          max_new_tokens=2048)
    sf.write(out / f"{name}.wav", wavs[0], sr)
    dur = len(wavs[0]) / sr
    print(f"{name}: gen={time.time() - t:.1f}s audio={dur:.1f}s", flush=True)
