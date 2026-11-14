# Banc de voix off — GN6

Ce qui a servi au banc d'écoute du 7 novembre 2026, avant de proposer une
voix off générée en local
(GN6, GENERATIF-2026-10).
Rien de ceci n'est dans le moteur : ce sont des scripts lancés à la main ;
leurs résultats ne sont pas publiés ici.

## Installer

Deux environnements séparés : `chatterbox-tts` épingle `transformers` 5.2.0,
`qwen-tts` 4.57.3. Python 3.12, tout sous `build/tts-bench/` (non suivi).

```sh
B=build/tts-bench
PY=python3.12   # 3.12.13 au banc
$PY -m venv $B/chatterbox-env
$PY -m venv $B/qwen-tts-env
PYTHONNOUSERSITE=1 $B/chatterbox-env/bin/python -m pip install \
  -r tools/voice-over-benchmark/chatterbox-requirements.txt
PYTHONNOUSERSITE=1 $B/qwen-tts-env/bin/python -m pip install \
  -r tools/voice-over-benchmark/qwen-tts-requirements.txt
```

Les deux listes sont le `pip freeze` des environnements du banc. Au banc, le
paquet a été installé depuis sa roue PyPI, empreinte vérifiée :
`chatterbox_tts-0.1.7-py3-none-any.whl`
`83782500e3ad4e7c919132e9d7eb8755f29f57c5bde5ec48c655ca23a4eb113c`,
`qwen_tts-0.1.1-py3-none-any.whl`
`11a290d8dabc7ef91a90c54478c8ab19b3edb1d85c0882313721892bdc4af15d`.

- **`setuptools` < 81 est nécessaire à Chatterbox** : son filigrane (Perth)
  importe `pkg_resources`. Sans lui, `PerthImplicitWatermarker` vaut `None`
  et le chargement échoue sur un `TypeError`.
- **`qwen-tts` n'épingle pas torch** : pip a pris torch 2.15.0 avec
  torchaudio 2.11.1. Le banc a tourné ainsi, sans erreur.

Les poids, à leur révision, dans un cache Hugging Face sous le banc
(`HF_HOME=$B/hf-cache`) :

| Dépôt | Révision | Fichiers |
|---|---|---|
| `ResembleAI/chatterbox` | `5bb1f6ee58e50c3b8d408bc82a6d3740c2db6e18` | `ve.pt`, `t3_mtl23ls_v2.safetensors`, `s3gen.pt`, `grapheme_mtl_merged_expanded_v1.json`, `conds.pt`, `Cangjie5_TC.json` (3,2 Go) |
| `Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign` | `5ecdb67327fd37bb2e042aab12ff7391903235d3` | tout (4,5 Go) |
| `Qwen/Qwen3-TTS-12Hz-1.7B-Base` | `fd4b254389122332181a7c3db7f27e918eec64e3` | tout (4,5 Go) |

```sh
HF_HOME=$B/hf-cache $B/qwen-tts-env/bin/python -c '
from huggingface_hub import snapshot_download
snapshot_download("Qwen/Qwen3-TTS-12Hz-1.7B-Base",
                  revision="fd4b254389122332181a7c3db7f27e918eec64e3")'
```

Au premier chargement, Chatterbox télécharge aussi le segmenteur de chinois
de spacy-pkuseg (`spacy_ontonotes`, 91 Mo), quelle que soit la langue, dans
`~/.pkuseg` sauf si `PKUSEG_HOME` dit ailleurs.

## Rejouer

Un modèle chargé à la fois : avec 16 Go de mémoire, deux modèles ou un
build en parallèle font monter le swap jusqu'à bloquer l'import de torch.

```sh
export HF_HOME=$B/hf-cache HF_HUB_OFFLINE=1 PYTHONNOUSERSITE=1 PKUSEG_HOME=$B/pkuseg
T=tools/voice-over-benchmark
S=$HF_HOME/hub/models--
# Une phrase, chaque modèle
$B/chatterbox-env/bin/python -I $T/smoke_chatterbox.py \
  ${S}ResembleAI--chatterbox/snapshots/5bb1f6ee58e50c3b8d408bc82a6d3740c2db6e18 \
  $B/out/smoke_chatterbox_default.wav mps
$B/qwen-tts-env/bin/python -I $T/smoke_qwen_design.py \
  ${S}Qwen--Qwen3-TTS-12Hz-1.7B-VoiceDesign/snapshots/5ecdb67327fd37bb2e042aab12ff7391903235d3 \
  $B/out/smoke_qwen_design.wav mps bf16
# La voix off : référence et témoins, puis clonage, puis similarité
$B/qwen-tts-env/bin/python -I $T/vo_design.py \
  ${S}Qwen--Qwen3-TTS-12Hz-1.7B-VoiceDesign/snapshots/5ecdb67327fd37bb2e042aab12ff7391903235d3 \
  $B/out $T/vo_texte.txt
$B/qwen-tts-env/bin/python -I $T/vo_clone.py \
  ${S}Qwen--Qwen3-TTS-12Hz-1.7B-Base/snapshots/fd4b254389122332181a7c3db7f27e918eec64e3 \
  $B/out $T/vo_texte.txt
$B/chatterbox-env/bin/python -I $T/vo_similarity.py \
  ${S}ResembleAI--chatterbox/snapshots/5bb1f6ee58e50c3b8d408bc82a6d3740c2db6e18/ve.pt \
  $B/out
```

Les mots se relisent par le Whisper du moteur, archive et journal coupés :

```sh
CUTMACHINE_PROJECT_ARCHIVE=off CUTMACHINE_LOG=0 ./build/cutmachine \
  --srt-from-media $B/out/vo_clone_take1.wav $B/out/vo_clone_take1.srt \
  --language fr --verbatim
```

La génération tire au sort : une autre exécution ne rend pas les mêmes
sons. Les chiffres du lot sont ceux d'une seule exécution ; leur variation
d'une exécution à l'autre n'a pas été mesurée.
