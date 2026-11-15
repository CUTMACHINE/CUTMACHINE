# Conversational sidecar

The Python driver uses only the standard library. The backend is selected
without changing the REPL:

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

The sidecar also loads the `.env` file at the repository root automatically,
without overriding a variable already exported by the shell. Git ignores this
file.

The planner asks the model for a trim intent (`Shorten` or `Extend`) and a
positive quantity in frames or seconds; the sign and the timebase of the
`TrimClip` are computed locally. Explicit references to a track, a clip, an
alias and a source name are also resolved from the view before validation. A
ULID proposed by the model is used only as a fallback when the wording does
not give a single deterministic resolution.

Optional variables: `CUTMACHINE_BINARY`, `CUTMACHINE_OLLAMA_URL`,
`CUTMACHINE_ANTHROPIC_URL`, `CUTMACHINE_OLLAMA_MODEL` and
`CUTMACHINE_ANTHROPIC_MODEL`. The model-specific variables take precedence
over `CUTMACHINE_MODEL`. Each instruction is independent: the sidecar keeps no
conversation, proposes only one operation and retries only once after a named
engine error.

The fixed corpus of 15 French instructions makes it possible to measure the
effect of changes of prompt or model:

```sh
python3 -m sidecar.eval --backend ollama
python3 -m sidecar.eval --backend anthropic
python3 -m sidecar.eval --backend all
```

The report prints each comparison and the success rate separately for each
backend. A perfect evaluation returns 0; any divergence returns 1.

## Rendering sound

`sidecar/sound.py` (SN3, SON-2026-10) launches
`node tools/cutmachine-sound/cli.mjs` as a subprocess and reads its JSON
report, as `binary.py` does for the engine. Node 18 or later is required
(`CUTMACHINE_NODE`, otherwise the `PATH`).

```python
from sidecar.sound import SoundRenderer
sound = SoundRenderer()
edit = sound.edit("Film.cutmachine-project")          # coupes et plans, avec leurs id
rendu = sound.render("impacts.js", "Film.cutmachine-project", seed=3,
                     settings={"impact": 0.6})
rendu.stem       # Film.cutmachine-project/Sound/impacts-seed3.wav
rendu.report     # sha256, pics, graine, réglages, timeReference, montage lu
```

In the REPL, `:son <ditty.js> [graine=N] [nom=valeur]...` renders against the
document's active timeline and prints the stem and the summary of the report.
No model is involved: the ditty says which sound goes where, the render places
it.
