"""Step 3: speaker similarity between the takes, with Chatterbox's encoder.

Cosine similarity of L2-normalised speaker embeddings: 1.0 is the same voice.
It is a proxy (one encoder, trained mostly on English), not a listening test.

Usage: vo_similarity.py <ve.pt> <out_dir>
"""
import pathlib
import sys

import librosa
import numpy as np
import torch
from chatterbox.models.voice_encoder import VoiceEncoder

NAMES = ["vo_reference", "vo_design_take1", "vo_design_take2",
         "vo_clone_take1", "vo_clone_take2"]

ve_path, out_dir = sys.argv[1:3]
out = pathlib.Path(out_dir)
ve = VoiceEncoder()
ve.load_state_dict(torch.load(ve_path, map_location="cpu", weights_only=True))
ve.eval()

embeds = {}
for name in NAMES:
    wav, _ = librosa.load(out / f"{name}.wav", sr=16000)
    embeds[name] = ve.embeds_from_wavs([wav], sample_rate=16000, as_spk=True)

short = {n: n.replace("vo_", "") for n in NAMES}
print(" " * 16 + "".join(f"{short[n]:>16}" for n in NAMES))
for a in NAMES:
    row = "".join(f"{float(np.dot(embeds[a], embeds[b])):16.3f}"
                  for b in NAMES)
    print(f"{short[a]:16}{row}")
