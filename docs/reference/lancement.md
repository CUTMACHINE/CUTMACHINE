# Launching

The build produces a macOS application in `build/CUTMACHINE.app`. Open it by
double-clicking in the Finder, from Spotlight after copying it into
Applications, or build it if needed then launch it with the `run-app`
target:

```sh
cmake --build build --target run-app -j
```

To open an already built bundle directly:

```sh
open build/CUTMACHINE.app
```

The command-line binary also remains available and can be launched with no
argument:

```sh
./build/cutmachine
```

`./build/cutmachine --version` answers without a project
`{"ok":true,"version":"0.1.0","commit":"<hash court>","dirty":false}`: the
number declared once in `CMakeLists.txt`, which the bundle also reads, and the
commit the binary comes from, recorded at each build. `dirty` says that a
tracked file differed from that commit; `unknown`, that there was no git
repository.

## What the machine is missing

`./build/cutmachine --check-setup` answers without a project or a window, in
one JSON object `{"ok":…,"items":[…]}` (PRODUIT-2026-10b PRD21). Each item
carries a stable identifier (`id`), a state (`state`), whether it is required
(`required`), what its absence prevents (`prevents`, and `tools`: the tools
that will refuse), what was found (`found`), what is missing (`missing`), a
sentence (`detail`), the name of the refusal its tools give (`error`) and the
repair (`repair`).

| `id` | Required | What is checked |
|---|---|---|
| `ffmpeg` | yes | the binary the engine retains and its capabilities, like `--check-ffmpeg` |
| `whisper_model` | no | `CUTMACHINE_WHISPER_MODEL`: a file, readable, non-empty (the window's menu keeps its own model, which is not read) |
| `whisper_hesitations_model` | no | `CUTMACHINE_WHISPER_HESITATIONS_MODEL`, the s075 weights French verbatim decodes with ([hesitation profile](transcription.md#french-hesitation-profile)): a readable file, and whether its SHA-256 is the evaluated one (`verified`, `evaluated`), read to verify it, about 10 s for 3 GB; `incomplete` for other weights, `optional_absent` unset |
| `mfa` | no | MFA's Python, its `mfa` executable and `scripts/mfa_align.py`, found; Python is not launched |
| `agent_key` | no | `ANTHROPIC_API_KEY` present or not, and from where — never its value; the Agent panel's Keychain is not consulted |
| `archive_backup` | no | `CUTMACHINE_ARCHIVE_BACKUP_DIR`: the folder where the project archive is copied every day, absolute and existing; with no setting, no copy (DEC44, [SQL archive](archive-projets.md#the-backup-copy)) |
| `voice_over` | no | the local voice-over (GN6a, GENERATIF-2026-10): the Python of its environment, `scripts/voice_over.py` and the models folder, searched like MFA's (`CUTMACHINE_VOICE_PYTHON`, `CUTMACHINE_VOICE_SCRIPT`, `CUTMACHINE_VOICE_MODELS`, otherwise `build/voice/env`, `scripts/` and `build/voice/models` next to the executable), and its two complete models (`Qwen3-TTS-12Hz-1.7B-VoiceDesign` and `-Base`, each with `config.json`, `model.safetensors` and `speech_tokenizer/model.safetensors`); Python is not launched. `optional_absent` when nothing is installed or set, `incomplete` when part of it is; refusal `VoiceOverNotFound` |

The states:

- `ok`;
- `missing`: absent, and its tools refuse;
- `incomplete`: present, but something is missing (an FFmpeg without
  `libx265`, an empty or unreadable model, MFA's Python without its
  executable, a voice-over model without its weights);
- `optional_absent`: absent, and nothing required depends on it.

The exit code is 0 as long as no required item is missing: an incomplete
FFmpeg opens and exports, and its item says what it cannot do. Without
FFmpeg, the response carries `"ok":false`, the error `FfmpegNotFound` and its
sentence next to the items, and the command exits with 1.

In the window, the welcome screen ("Bienvenue dans CUTMACHINE", i.e. Welcome
to CUTMACHINE) reads this report in the background, without delaying its
display. When a required item is missing (FFmpeg is the only one), a line
appears at the foot of the screen, to the left of "Quitter" (Quit), with a
"Diagnostic…" button: "FFmpeg introuvable : les vignettes, les formes d'onde
et l'export vidéo ne fonctionneront pas." (FFmpeg not found: thumbnails,
waveforms and video export will not work.) Nothing appears when the required
item is there, even if the Whisper model, MFA, the key or the connection are
missing, nor for an incomplete FFmpeg (without `libx265`, for example).
Launched with a project path, the application skips this screen.

**CUTMACHINE ▸ Diagnostic de l'installation…** (Installation diagnostic…)
(the menu bar exists as soon as a project is open) and the "Diagnostic…"
button open the whole report, in French, reread at each opening. It gives a
paragraph per item: its state ("prêt", "absent", "incomplet", "facultatif,
absent", i.e. ready, absent, incomplete, optional, absent), what was found,
what its absence prevents and the steps to follow — the variable,
`~/.config/cutmachine/.env`, or the menu when there is one (**Modèle
Whisper…** (Whisper model…), **Moteur IA…** (AI engine…)).

One window setting counts there that `--check-setup` does not read: an
Anthropic key recorded in the Keychain by **Moteur IA…**. It or
`ANTHROPIC_API_KEY` is enough to call the item ready, and the text says which
one is used for what. The Whisper model is not one of these (DEC23):
**CUTMACHINE ▸ Modèle Whisper…** writes `CUTMACHINE_WHISPER_MODEL` into
`~/.config/cutmachine/.env`, the setting that agents and the command line
also read (see [Configuring the model](transcription.md#configuring-the-model)).
"Copier le diagnostic" (Copy the diagnostic) puts this text on the clipboard,
followed by the JSON of `--check-setup`, to send it to someone; a key's value
is never in it.

### Where MFA is searched

`align_words` searches each of MFA's three files in this order, and the first
one found wins:

1. Its variable (`CUTMACHINE_MFA_PYTHON`, `CUTMACHINE_MFA_ALIGNER`,
   `CUTMACHINE_MFA_SCRIPT`), in the environment then in
   `~/.config/cutmachine/.env`. A value that does not designate a file is
   refused: it does not make the search go elsewhere.
2. Next to the executable: `<dossier>/mfa/env/bin/python`,
   `<dossier>/mfa/env/bin/mfa`, `<dossier>/../scripts/mfa_align.py`, that is
   the `build/` and `scripts/` of a checkout, whatever the current folder.
   For the application, the folder that contains `CUTMACHINE.app` counts too.
3. Where the build that produced the binary installs them: an app copied into
   Applications thus finds the MFA of its checkout.
4. The current folder.

The script reads the models in `models/` next to the environment
(`build/mfa/models`): for each language, the acoustic model, the
pronunciation dictionary and the G2P model for the words the dictionary does
not have (see [Aligned boundaries](transcription.md#aligned-boundaries-and-unheard-words)).

When not found, the tool refuses with the error `MfaNotFound`, which says
where each file was searched and which variable to set. Nothing is downloaded
or installed. The environment (conda, Kaldi, about 1.3 GB) and the models
(about 310 MB) live in `build/mfa/`: an `rm -rf build` takes them away. They
are rebuilt by hand: micromamba 2.9.0 creates `build/mfa/env` from
conda-forge (`montreal-forced-aligner=3.4.3`), and the models are downloaded
from the releases of `MontrealCorpusTools/mfa-models` (commands in the README
of the alignment bench).

### Where diarization is searched

`analyze_speakers` searches three files, in the same order as MFA:

1. Its variable: `CUTMACHINE_DIARIZATION_PYTHON` (the environment's Python),
   `CUTMACHINE_DIARIZATION_SCRIPT` (`scripts/diarize.py`),
   `CUTMACHINE_DIARIZATION_MODEL` (the voiceprint model).
2. Next to the executable: `<dossier>/diarization/env/bin/python`,
   `<dossier>/../scripts/diarize.py`,
   `<dossier>/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx`.
3. Where the build that produced the binary installs them.
4. From the current folder: `build/diarization/…` and `scripts/diarize.py`.

Nothing is downloaded or installed by the engine. What is missing is the
refusal `DiarizationNotFound`, which names each place looked at.
`--check-setup` does not report it yet.

The environment is made by hand, once, under `build/diarization/`:

```sh
python3 -m venv build/diarization/env
build/diarization/env/bin/pip install sherpa-onnx==2.0.1 numpy==2.0.2
curl -L -o build/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx \
  https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx
```

The checksums (SHA-256): the model
`e9848563da86f263117134dfd7ad63c92355b37de492b55e325400c9d9c39012`, the
wheel `sherpa_onnx-2.0.1-cp39-cp39-macosx_11_0_arm64`
`a18f47204f6b2a1d81769e736ddf9fdfb23cff01021c17491f2faf097ced6677`,
`sherpa_onnx_core-2.0.1` for macOS arm64
`82ab505a835e616f568ba6804555969e1b5699341d2b18eb27f0083b068b61ff`.

## Which FFmpeg

The FFmpeg libraries are linked at build time; the `ffmpeg` **binary**, on
the other hand, is launched for export, thumbnails, proxies and analyses. The
engine searches for it once per process and keeps an absolute path, in this
order:

1. `CUTMACHINE_FFMPEG`, in the environment then in
   `~/.config/cutmachine/.env` (the file of the machine's local settings);
2. an `ffmpeg` placed next to the executable;
3. the `PATH` folders.

An application opened from the Finder receives the `PATH` of `launchd`, where
FFmpeg is not: this is the case where the setting is useful. With PRD21, the
welcome screen says so as soon as it opens.

```sh
echo "CUTMACHINE_FFMPEG=$(command -v ffmpeg)" >> ~/.config/cutmachine/.env
```

A value that does not designate an executable is refused; it does not fall
back to the `PATH`. Without a binary, every command and every tool that needs
one refuses with the error `FfmpegNotFound`, which says where it searched and
which variable to set.

Not every FFmpeg has what the engine asks for: the encoders
`h264_videotoolbox`, `hevc_videotoolbox`, `libx264`, `libx265` and
`prores_ks`. The `drawtext` filter, which the FFmpeg of `brew bundle` does not
have, is not required: captions are drawn by the engine
(LEGENDES-2026-10). The engine reads the
binary's lists once (`-filters`, `-encoders`) and checks before each launch:
an export or a tool that needs a missing capability refuses with the error
`FfmpegCapabilityMissing`, the name of the encoder and what its absence
prevents, without launching anything else.

```sh
./build/cutmachine --check-ffmpeg
```

answers without a project: the binary retained, where it comes from, its
version, and each capability with `available`. The exit code is 0 as soon as
a binary is found, as for `--check-setup` (DEC32): `complete` says whether it
has everything, and otherwise `error` (`FfmpegCapabilityMissing`, or
`FfmpegProbeFailed` for a binary that does not answer), `detail` and
`missing`, each missing capability with what it prevents, are in the
response. Without a binary, it exits with 1 and `FfmpegNotFound`. On the
agent side, the responses of `open_project` and `create_project` carry an
`ffmpeg` section when the binary is missing or something is missing from it,
and only in that case.

## Development loop

The development script provides a loop close to `npm run dev` for the native
application: each change to the sources triggers an incremental build,
cleanly closes the old instance, then opens the new bundle. It needs neither
Homebrew nor `fswatch`.

```sh
./scripts/dev.sh
```

This is not an in-process hot reload: AppKit and the Metal shaders are
rebuilt, then the application restarts. The recent project can then be
reopened from the welcome screen.

A home page lets you create a project, open an existing file or resume one
of the eight recent projects. Creation asks for the name and location of the
`.cutmachine-project` package, initialises a first timeline, then opens the
editing workspace directly.

You change project without going back through the welcome screen (DEC39,
PHASES-2026-08): **Fichier → Nouveau projet…** (File → New project…) (the
name on a sheet of the window, then the location) and **Fichier → Ouvrir un
projet…** (File → Open a project…). The window does not replace its project
in place: it checks the other one (`src/ProjectSwitch.h`: readable, not open
elsewhere, not the same), saves its own, then closes and a new editor opens
on the other project, once the first one's lock is released
(`process::StartDetached`). An export or a task in progress in the task bar
holds the window on its project, the reason stated on a sheet; the Agent
panel and a relayed agent start again from zero with the new editor.

For storage, saves, linked media and sharing between machines, see the
[project storage specification](../PROJECT_STORAGE_SPEC.md). **Fichier →
Collecter le projet…** (File → Collect the project…) creates a standalone
`.cutmachine-project` package containing a verified copy of the originals, a
SHA-256 manifest and a distinct history per timeline, rewritten like the
state: undoing in the copy does not bring back a file of the source project.
The `src` files of motion compositions travel with it too; the limits of
external path bindings are described in the storage specification. This
package can be transferred then opened directly by CUTMACHINE on another Mac.
On opening, the checksums are verified again and any original that is missing
or modified triggers a warning. A session lock also prevents two instances
from writing the same project simultaneously. In the v2 package, each
timeline lives in its own `Timelines/<ULID>.json` file and is saved
transactionally with the project snapshot and the histories. Old JSON files
and v1 packages are no longer accepted.

It is still possible to open a project directly from the command line by
pointing to its internal file:

```sh
./build/cutmachine ./Film.cutmachine-project/project.cutmachine.json
```

Relative source paths are resolved relative to the JSON file, not to the
process's current directory.
