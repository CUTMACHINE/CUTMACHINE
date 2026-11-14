# Banc d'alignement mot à mot

Le harnais qui compare des aligneurs de mots sur le même son et le même
texte. Il ne télécharge ni modèle ni média et n'exécute aucun aligneur :
chaque modèle tourne dans son propre environnement, et la normalisation
comme le score sont les mêmes pour tous.

| Fichier | Rôle |
|---|---|
| `benchmark.py` | normalise les sorties de chaque chemin (`normalize-cutmachine`, `normalize-mfa`, `normalize-nemo`, `normalize-qwen`) en un manifeste commun, les combine (`ensemble`) et les note (`score`, `score-resolve`, `agreement`, `agreement-set`, `envelope-junctions`) |
| `word-alignment-manifest.schema.json` | le format de ce manifeste : des mots, leurs bornes en images, la cadence |
| `cprom.py` | la mesure contre C-PROM, un corpus français aligné et vérifié à la main (CC BY-NC-SA : jamais dans le dépôt) |
| `qwen_align.py` | Qwen3-ForcedAligner sur un manifeste au format de NeMo, en CTM par mot |
| `nemo_transcribe.py`, `hf_transcribe.py` | la transcription des mêmes morceaux par un modèle NeMo, Qwen3-ASR ou Voxtral |
| `phoneme_recognize.py` | un reconnaisseur de phonèmes CTC sur les mêmes morceaux |
| `filled_pauses.py` | un détecteur acoustique de « euh » : une voyelle tenue, stable en hauteur et en timbre |

Chaque script décrit ses arguments par `--help`. `benchmark.py` se teste
sans média : `python3 tests/word_alignment_benchmark_tests.py`.

Le corpus sur lequel le banc a été mesuré (des rushes d'interviews) et le
journal de ces mesures ne font pas partie de ce dépôt. Ce que le moteur en
a gardé est dans
[Transcription](../../docs/reference/transcription.md), « Frontières
alignées et mots non entendus », et dans
[`docs/TRANSCRIPTION_BENCH.md`](../../docs/TRANSCRIPTION_BENCH.md).
