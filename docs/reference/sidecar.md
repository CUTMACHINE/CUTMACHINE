# Sidecar conversationnel

> Relu contre le code le 3 octobre 2026. Les réponses des modèles et le
> rapport d'évaluation ne sont pas vérifiés par lecture.

Le pilote Python utilise uniquement la bibliothèque standard. Le backend est
sélectionné sans modifier le REPL :

```sh
# Ollama local
export CUTMACHINE_BACKEND=ollama
export CUTMACHINE_MODEL=qwen3:8b
python3 -m sidecar.repl ./Film.cutmachine-project/project.cutmachine.json

# API Anthropic
export CUTMACHINE_BACKEND=anthropic
export CUTMACHINE_MODEL=claude-sonnet-4-5
export ANTHROPIC_API_KEY=...
python3 -m sidecar.repl ./Film.cutmachine-project/project.cutmachine.json
```

Le sidecar charge aussi automatiquement le fichier `.env` à la racine du
dépôt, sans remplacer une variable déjà exportée par le shell. Ce fichier est
ignoré par Git.

Le planner demande au modèle une intention de trim (`Shorten` ou `Extend`) et
une quantité positive en frames ou secondes ; le signe et le timebase du
`TrimClip` sont calculés localement. Les références explicites de piste, de
clip, d'alias et de nom de source sont également résolues depuis la vue avant
validation. Un ULID proposé par le modèle n'est utilisé qu'en fallback lorsque
la formulation ne fournit pas une résolution déterministe unique.

Variables optionnelles : `CUTMACHINE_BINARY`, `CUTMACHINE_OLLAMA_URL`,
`CUTMACHINE_ANTHROPIC_URL`, `CUTMACHINE_OLLAMA_MODEL` et
`CUTMACHINE_ANTHROPIC_MODEL`. Les variables de modèle spécifiques prennent le
pas sur `CUTMACHINE_MODEL`. Chaque instruction est indépendante : le sidecar ne
conserve aucune conversation, ne propose qu'une opération et ne réessaie qu'une
fois après une erreur nommée du moteur.

Le corpus fixe de 15 instructions françaises permet de mesurer les changements
de prompt ou de modèle :

```sh
python3 -m sidecar.eval --backend ollama
python3 -m sidecar.eval --backend anthropic
python3 -m sidecar.eval --backend all
```

Le rapport affiche chaque comparaison et le taux de réussite séparément pour
chaque backend. Une évaluation parfaite retourne 0 ; toute divergence retourne 1.

## Rendre du son

`sidecar/sound.py` (SN3, SON-2026-10) lance
`node tools/cutmachine-sound/cli.mjs` en sous-processus et lit son rapport
JSON, comme `binary.py` le fait pour le moteur. Node 18 ou plus est
nécessaire (`CUTMACHINE_NODE`, sinon le `PATH`).

```python
from sidecar.sound import SoundRenderer
sound = SoundRenderer()
edit = sound.edit("Film.cutmachine-project")          # coupes et plans, avec leurs id
rendu = sound.render("impacts.js", "Film.cutmachine-project", seed=3,
                     settings={"impact": 0.6})
rendu.stem       # Film.cutmachine-project/Sound/impacts-seed3.wav
rendu.report     # sha256, pics, graine, réglages, timeReference, montage lu
```

Dans le REPL, `:son <ditty.js> [graine=N] [nom=valeur]...` rend contre la
timeline active du document et imprime le stem et le résumé du rapport.
Aucun modèle n'y intervient : la ditty dit quel son va où, le rendu place.

