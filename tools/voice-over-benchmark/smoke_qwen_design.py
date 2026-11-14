"""Smoke test: Qwen3-TTS VoiceDesign, voice from a description, French.

Usage: smoke_qwen_design.py <model_dir> <out.wav> <device> <dtype>
"""
import sys
import time

import soundfile as sf
import torch
from qwen_tts import Qwen3TTSModel

TEXT = (
    "La première traversée part à 8 h 40. Le café est servi au pont "
    "supérieur, et les vélos se rangent à l'avant, près de la rampe."
)
INSTRUCT = (
    "A warm, calm French male voice in his thirties, documentary voice-over "
    "style, moderate pace, clear diction."
)

model_dir, out_path, device, dtype_name = sys.argv[1:5]
dtype = {"bf16": torch.bfloat16, "fp16": torch.float16,
         "fp32": torch.float32}[dtype_name]
t0 = time.time()
model = Qwen3TTSModel.from_pretrained(model_dir, device_map=device,
                                      dtype=dtype)
t1 = time.time()
wavs, sr = model.generate_voice_design(text=TEXT, instruct=INSTRUCT,
                                       language="French")
t2 = time.time()
sf.write(out_path, wavs[0], sr)
dur = len(wavs[0]) / sr
print(f"device={device} dtype={dtype_name} load={t1 - t0:.1f}s "
      f"gen={t2 - t1:.1f}s audio={dur:.1f}s rtf={(t2 - t1) / dur:.2f} sr={sr}")
