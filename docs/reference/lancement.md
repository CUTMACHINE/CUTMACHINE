# Lancement

> Relu contre le code le 3 octobre 2026. Ce que fait le Finder et ce que
> montre l'écran d'accueil ne sont pas vérifiés par lecture.

La compilation produit une application macOS dans `build/CUTMACHINE.app`.
Ouvrez-la par double-clic dans le Finder, depuis Spotlight après l’avoir copiée
dans Applications, ou compilez-la si nécessaire puis lancez-la avec la cible
`run-app` :

```sh
cmake --build build --target run-app -j
```

Pour ouvrir directement un bundle déjà compilé :

```sh
open build/CUTMACHINE.app
```

Le binaire en ligne de commande reste également disponible et peut être lancé
sans argument :

```sh
./build/cutmachine
```

`./build/cutmachine --version` répond sans projet
`{"ok":true,"version":"0.1.0","commit":"<hash court>","dirty":false}` : le
numéro déclaré une fois dans `CMakeLists.txt`, que lit aussi le bundle, et le
commit dont sort le binaire, relevé à chaque build. `dirty` dit qu'un fichier
suivi différait de ce commit ; `unknown`, qu'il n'y avait pas de dépôt git.

## Ce qui manque à la machine

`./build/cutmachine --check-setup` répond sans projet ni fenêtre, en un objet
JSON `{"ok":…,"items":[…]}` (PRODUIT-2026-10b PRD21). Chaque élément porte un
identifiant stable (`id`), un état (`state`), s'il est requis (`required`),
ce que son absence empêche (`prevents`, et `tools` : les outils qui
refuseront), ce qui a été trouvé (`found`), ce qui lui manque (`missing`),
une phrase (`detail`), le nom du refus que donnent ses outils (`error`) et la
réparation (`repair`).

| `id` | Requis | Ce qui est vérifié |
|---|---|---|
| `ffmpeg` | oui | le binaire que le moteur retient et ses capacités, comme `--check-ffmpeg` |
| `whisper_model` | non | `CUTMACHINE_WHISPER_MODEL` : un fichier, lisible, non vide (le menu de la fenêtre garde son propre modèle, non lu) |
| `mfa` | non | le Python de MFA, son exécutable `mfa` et `scripts/mfa_align.py`, trouvés ; Python n'est pas lancé |
| `agent_key` | non | `ANTHROPIC_API_KEY` présente ou non, et d'où — jamais sa valeur ; le Trousseau du panneau Agent n'est pas consulté |
| `archive_backup` | non | `CUTMACHINE_ARCHIVE_BACKUP_DIR` : le dossier où l'archive des projets est copiée chaque jour, absolu et existant ; sans réglage, aucune copie (DEC44, [Archive SQL](archive-projets.md#la-copie-de-sécurité)) |
| `voice_over` | non | la voix off locale (GN6a, GENERATIF-2026-10) : le Python de son environnement, `scripts/voice_over.py` et le dossier des modèles, cherchés comme ceux de MFA (`CUTMACHINE_VOICE_PYTHON`, `CUTMACHINE_VOICE_SCRIPT`, `CUTMACHINE_VOICE_MODELS`, sinon `build/voice/env`, `scripts/` et `build/voice/models` à côté de l'exécutable), et ses deux modèles complets (`Qwen3-TTS-12Hz-1.7B-VoiceDesign` et `-Base`, chacun avec `config.json`, `model.safetensors` et `speech_tokenizer/model.safetensors`) ; Python n'est pas lancé. `optional_absent` quand rien n'est installé ni réglé, `incomplete` quand une partie l'est ; refus `VoiceOverNotFound` |

Les états :

- `ok` ;
- `missing` : absent, et ses outils refusent ;
- `incomplete` : présent, mais il lui manque quelque chose (un FFmpeg sans
  `libx265`, un modèle vide ou illisible, le Python de MFA sans son
  exécutable, un modèle de voix off sans ses poids) ;
- `optional_absent` : absent, et rien de requis n'en dépend.

Le code de sortie est 0 tant qu'aucun élément requis ne manque : un FFmpeg
incomplet monte et exporte, et son élément dit ce qu'il ne peut pas faire.
Sans FFmpeg, la réponse porte `"ok":false`, l'erreur `FfmpegNotFound` et sa
phrase à côté des éléments, et la commande sort en 1.

Dans la fenêtre, l'écran d'accueil (« Bienvenue dans CUTMACHINE ») lit ce
rapport en tâche de fond, sans retarder son affichage. Quand un élément requis
manque (aujourd'hui FFmpeg seul), une ligne apparaît au pied de l'écran, à
gauche de « Quitter », avec un bouton « Diagnostic… » : « FFmpeg
introuvable : les vignettes, les formes d'onde et l'export vidéo ne
fonctionneront pas. » Rien n'apparaît quand le requis est là, même si le
modèle Whisper, MFA, la clé ou la connexion manquent, ni pour un FFmpeg
incomplet (sans `libx265`, par exemple). Lancée avec un chemin de projet, l'application
saute cet écran.

**CUTMACHINE ▸ Diagnostic de l'installation…** (la barre de menus existe dès
qu'un projet est ouvert) et le bouton « Diagnostic… » ouvrent le rapport
entier, en français, relu à chaque ouverture. Il donne un paragraphe par
élément : son état (« prêt », « absent », « incomplet », « facultatif,
absent »), ce qui a été trouvé, ce que son absence empêche et la marche à
suivre — la variable, `~/.config/cutmachine/.env`, ou le menu quand il en
existe un (**Modèle Whisper…**, **Moteur IA…**).

Un réglage de la fenêtre y compte, que `--check-setup` ne lit pas : une clé
Anthropic enregistrée dans le Trousseau par **Moteur IA…**. Elle ou
`ANTHROPIC_API_KEY` suffit à dire l'élément prêt, et le texte dit laquelle
sert quoi. Le modèle Whisper n'est plus de ceux-là depuis le 24 octobre 2026
(DEC23) : **CUTMACHINE ▸ Modèle Whisper…** écrit `CUTMACHINE_WHISPER_MODEL`
dans `~/.config/cutmachine/.env`, le réglage que lisent aussi les agents et
la ligne de commande (voir
[Configurer le modèle](transcription.md#configurer-le-modèle)). « Copier le
diagnostic » met ce texte dans le presse-papiers, suivi du JSON de
`--check-setup`, pour l'envoyer à quelqu'un ; la valeur d'une clé n'y est
jamais.

### Où MFA est cherché

`align_words` cherche chacun des trois fichiers de MFA dans cet ordre, et le
premier trouvé l'emporte :

1. Sa variable (`CUTMACHINE_MFA_PYTHON`, `CUTMACHINE_MFA_ALIGNER`,
   `CUTMACHINE_MFA_SCRIPT`), dans l'environnement puis dans
   `~/.config/cutmachine/.env`. Une valeur qui ne désigne pas un fichier est
   refusée : elle ne fait pas chercher ailleurs.
2. À côté de l'exécutable : `<dossier>/mfa/env/bin/python`,
   `<dossier>/mfa/env/bin/mfa`, `<dossier>/../scripts/mfa_align.py`, soit le
   `build/` et le `scripts/` d'un checkout, quel que soit le dossier courant.
   Pour l'application, le dossier qui contient `CUTMACHINE.app` compte aussi.
3. Là où le build qui a produit le binaire les installe : une app copiée
   dans Applications retrouve ainsi le MFA de son checkout.
4. Le dossier courant.

Le script lit les modèles dans `models/` à côté de l'environnement
(`build/mfa/models`) : pour chaque langue, le modèle acoustique, le
dictionnaire de prononciation et le modèle G2P des mots que le dictionnaire
n'a pas (voir [Frontières alignées](transcription.md#frontières-alignées-et-mots-non-entendus)).

Introuvable, l'outil refuse avec l'erreur `MfaNotFound` (avant le
1er novembre 2026, `NemoNotFound`), qui dit où chaque fichier a été cherché
et quelle variable régler. Rien n'est téléchargé ni installé. L'environnement
(conda, Kaldi, environ 1,3 Go) et les modèles (environ 310 Mo) vivent dans
`build/mfa/` : un `rm -rf build` les emporte. Ils se refont à la main :
micromamba 2.9.0 crée `build/mfa/env` depuis conda-forge
(`montreal-forced-aligner=3.4.3`), et les modèles se téléchargent depuis les
versions de `MontrealCorpusTools/mfa-models` (commandes dans le README du
banc d'alignement).

### Où la diarisation est cherchée

`analyze_speakers` cherche trois fichiers, dans le même ordre que MFA :

1. Sa variable : `CUTMACHINE_DIARIZATION_PYTHON` (le Python de
   l'environnement), `CUTMACHINE_DIARIZATION_SCRIPT` (`scripts/diarize.py`),
   `CUTMACHINE_DIARIZATION_MODEL` (le modèle d'empreinte vocale).
2. À côté de l'exécutable : `<dossier>/diarization/env/bin/python`,
   `<dossier>/../scripts/diarize.py`,
   `<dossier>/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx`.
3. Là où le build qui a produit le binaire les installe.
4. Depuis le dossier courant : `build/diarization/…` et `scripts/diarize.py`.

Rien n'est téléchargé ni installé par le moteur. Ce qui manque est le refus
`DiarizationNotFound`, qui nomme chaque endroit regardé. `--check-setup` ne
le signale pas encore.

L'environnement se fait à la main, une fois, sous `build/diarization/` :

```sh
python3 -m venv build/diarization/env
build/diarization/env/bin/pip install sherpa-onnx==2.0.1 numpy==2.0.2
curl -L -o build/diarization/models/wespeaker_en_voxceleb_resnet34_LM.onnx \
  https://github.com/k2-fsa/sherpa-onnx/releases/download/speaker-recongition-models/wespeaker_en_voxceleb_resnet34_LM.onnx
```

Les empreintes relevées le 12 novembre 2026 (SHA-256) : le modèle
`e9848563da86f263117134dfd7ad63c92355b37de492b55e325400c9d9c39012`, la
roue `sherpa_onnx-2.0.1-cp39-cp39-macosx_11_0_arm64`
`a18f47204f6b2a1d81769e736ddf9fdfb23cff01021c17491f2faf097ced6677`,
`sherpa_onnx_core-2.0.1` pour macOS arm64
`82ab505a835e616f568ba6804555969e1b5699341d2b18eb27f0083b068b61ff`.

## Quel FFmpeg

Les bibliothèques FFmpeg sont liées à la compilation ; le **binaire** `ffmpeg`,
lui, est lancé pour l'export, les vignettes, les proxies et les analyses. Le
moteur le cherche une fois par processus et garde un chemin absolu, dans cet
ordre :

1. `CUTMACHINE_FFMPEG`, dans l'environnement puis dans
   `~/.config/cutmachine/.env` (le fichier des réglages locaux de la machine) ;
2. un `ffmpeg` posé à côté de l'exécutable ;
3. les dossiers du `PATH`.

Une application ouverte depuis le Finder reçoit le `PATH` de `launchd`, où
FFmpeg n'est pas : c'est le cas où le réglage sert. Depuis PRD21, l'écran
d'accueil le dit dès l'ouverture.

```sh
echo "CUTMACHINE_FFMPEG=$(command -v ffmpeg)" >> ~/.config/cutmachine/.env
```

Une valeur qui ne désigne pas un exécutable est refusée, elle ne fait pas
retomber sur le `PATH`. Sans binaire, chaque commande et chaque outil qui en
a besoin refuse avec l'erreur `FfmpegNotFound`, qui dit où il a cherché et
quelle variable régler.

Tous les FFmpeg n'ont pas ce que le moteur demande : les encodeurs
`h264_videotoolbox`, `hevc_videotoolbox`, `libx264`, `libx265` et
`prores_ks`. Le filtre `drawtext`, que le FFmpeg de `brew bundle` n'a pas,
n'est plus demandé depuis le 9 octobre 2026 : les légendes sont dessinées par
le moteur (LEGENDES-2026-10). Le moteur
lit une fois les listes du binaire (`-filters`, `-encoders`) et vérifie avant
chaque lancement : un export ou un outil qui a besoin d'une capacité absente
refuse avec l'erreur `FfmpegCapabilityMissing`, le nom de l'encodeur et ce
que son absence empêche, sans rien lancer d'autre.

```sh
./build/cutmachine --check-ffmpeg
```

répond sans projet : le binaire retenu, d'où il vient, sa version, et chaque
capacité avec `available`. Le code de sortie est 0 dès qu'un binaire est
trouvé, comme pour `--check-setup` (DEC32, 24 octobre 2026) : `complete` dit
s'il a tout, et sinon `error` (`FfmpegCapabilityMissing`, ou
`FfmpegProbeFailed` pour un binaire qui ne répond pas), `detail` et `missing`,
chaque capacité absente avec ce qu'elle empêche, sont dans la réponse. Sans
binaire, il sort en 1 avec `FfmpegNotFound`. Côté
agent, les réponses d'`open_project` et de `create_project` portent une
section `ffmpeg` quand le binaire manque ou qu'il lui manque quelque chose,
et seulement dans ce cas.

## Boucle de développement

Le script de développement fournit une boucle proche de `npm run dev` pour
l’application native : chaque modification des sources déclenche une
compilation incrémentale, ferme proprement l’ancienne instance, puis ouvre le
nouveau bundle. Il n’a besoin ni de Homebrew ni de `fswatch`.

```sh
./scripts/dev.sh
```

Il ne s’agit pas d’un hot reload dans le processus : AppKit et les shaders
Metal sont reconstruits, puis l’application redémarre. Le projet récent peut
ensuite être rouvert depuis l’écran d’accueil.

Une page d’accueil permet de créer un projet, d’ouvrir un fichier existant ou
de reprendre l’un des huit projets récents. La création demande le nom et
l’emplacement du package `.cutmachine-project`, initialise une première timeline,
puis ouvre directement l’espace de montage.

Depuis le 24 octobre 2026 (DEC39, PHASES-2026-08), on change de projet sans
repasser par l’écran d’accueil : **Fichier → Nouveau projet…** (le nom sur
une feuille de la fenêtre, puis l’emplacement) et **Fichier → Ouvrir un
projet…**. La fenêtre ne remplace pas son projet en place : elle vérifie
l’autre (`src/ProjectSwitch.h` : lisible, pas ouvert ailleurs, pas le même),
enregistre le sien, puis se ferme et un nouvel éditeur s’ouvre sur l’autre
projet, une fois le verrou du premier rendu (`process::StartDetached`). Un
export ou une tâche en cours dans la barre des tâches retient la fenêtre sur
son projet, la raison dite sur une feuille ; le panneau Agent et un agent
relayé repartent de zéro avec le nouvel éditeur.

Pour le stockage, les sauvegardes, les médias liés et le partage entre machines,
voir la [spécification de stockage des projets](../PROJECT_STORAGE_SPEC.md).
**Fichier → Collecter le projet…** crée un package `.cutmachine-project`
autonome contenant une copie vérifiée des originaux, un manifeste SHA-256 et
un historique distinct par timeline, réécrit comme l'état : annuler dans la
copie ne ramène pas un fichier du projet source. Les fichiers `src` des compositions motion
voyagent aussi avec lui ; les limites des liaisons de chemins externes sont
décrites dans la spécification de stockage. Ce package peut être transféré puis ouvert
directement par CUTMACHINE sur un autre Mac. À l’ouverture, les empreintes sont
revérifiées et tout original manquant ou modifié déclenche un avertissement.
Un verrou de session empêche également deux instances d’écrire simultanément
le même projet. Dans le package v2, chaque timeline vit dans son propre fichier
`Timelines/<ULID>.json` et est sauvegardée transactionnellement avec le snapshot
projet et les historiques. Les anciens fichiers JSON et packages v1 ne sont
plus acceptés.

Il reste possible d’ouvrir directement un projet depuis la ligne de commande en
visant son fichier interne :

```sh
./build/cutmachine ./Film.cutmachine-project/project.cutmachine.json
```

Les chemins relatifs des sources sont résolus relativement au fichier JSON,
pas au répertoire courant du processus.
