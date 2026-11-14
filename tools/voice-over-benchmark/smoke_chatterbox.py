"""Smoke test: Chatterbox multilingual, default voice, one French sentence.

Usage: smoke_chatterbox.py <ckpt_dir> <out.wav> <device>
"""
import sys
import time

import soundfile as sf
import torch
from chatterbox.mtl_tts import ChatterboxMultilingualTTS

TEXT = (
    "La première traversée part à 8 h 40. Le café est servi au pont "
    "supérieur, et les vélos se rangent à l'avant, près de la rampe."
)

ckpt_dir, out_path, device = sys.argv[1], sys.argv[2], sys.argv[3]
t0 = time.time()
model = ChatterboxMultilingualTTS.from_local(ckpt_dir, device)
t1 = time.time()
wav = model.generate(TEXT, language_id="fr")
t2 = time.time()
audio = wav.squeeze(0).cpu().numpy()
sf.write(out_path, audio, model.sr)
dur = len(audio) / model.sr
print(f"device={device} load={t1 - t0:.1f}s gen={t2 - t1:.1f}s "
      f"audio={dur:.1f}s rtf={(t2 - t1) / dur:.2f} sr={model.sr}")
