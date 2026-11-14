# CUTMACHINE

CUTMACHINE est un moteur de montage vidéo adressable : un modèle de document,
une algèbre d'opérations qui le transforme, et plusieurs surfaces qui exposent
les deux. La fenêtre macOS est une de ces surfaces, la ligne de commande une
autre, le serveur MCP par lequel un agent monte une troisième : un geste à la
souris et un appel d'outil produisent la même opération, passent par le même
journal et subissent les mêmes validations. Un projet tient dans des fichiers
JSON lisibles, chaque modification y est une opération réversible et nommée,
et les positions comme les durées sont des rationnels exacts, jamais des
flottants.

Ce fichier dit ce qu'il faut pour compiler,
donne un [premier parcours](#premier-parcours) à la ligne de commande, puis
mène à la référence : le [plan](#plan-de-la-référence) en nomme les sections.
L'application ne tourne que sur macOS ; le moteur, sa ligne de commande et
son serveur MCP se compilent aussi sous Linux, sans fenêtre
([`headless/`](headless/README.md)).

## Build et tests

Prérequis : macOS, CMake 3.24+, pkg-config et FFmpeg (`libavformat`,
`libavcodec`, `libavutil`, `libswresample`, `libswscale`).

S'y ajoutent le compilateur et le SDK d'Apple, Python 3, que la
configuration cherche pour la suite de tests,
et le réseau à la première configuration et à la première compilation.
CMake télécharge whisper.cpp, OpenColorIO, Yoga et SQLite sous forme d'archive d'un
commit précis, dont il vérifie l'empreinte SHA-256 avant de s'en servir : une
archive qui ne correspond pas arrête la configuration. OpenColorIO clone
ensuite par git, pendant la compilation, les six bibliothèques qu'il
construit lui-même. Le tout se compile avec le reste, ce qui n'est pas
instantané.

FFmpeg sert deux fois. Ses bibliothèques sont liées au moteur ; son exécutable
`ffmpeg` fait les exports et les analyses. Le moteur le cherche dans un ordre
fixe, décrit dans [Quel FFmpeg](docs/reference/lancement.md#quel-ffmpeg) : la
variable `CUTMACHINE_FFMPEG`, un binaire posé à côté de l'exécutable, puis le
`PATH`. Depuis le 9 octobre 2026, il n'a plus besoin du filtre `drawtext` :
les légendes sont dessinées par le moteur lui-même, avec Core Text
(LEGENDES-2026-10). Une fois le moteur
compilé, `./build/cutmachine --check-setup` fait sans projet le tour de ce
qui manque à la machine — FFmpeg, le modèle Whisper, MFA, la clé de l'agent
intégré, la copie de l'archive, la voix off
locale —, élément par élément, avec ce que
chaque absence empêche et comment y remédier : voir
[Ce qui manque à la machine](docs/reference/lancement.md#ce-qui-manque-à-la-machine).
`./build/cutmachine --check-ffmpeg` détaille FFmpeg seul : quel binaire le
moteur retient et quels encodeurs lui manquent. Les deux commandes sortent en
0 dès que FFmpeg est trouvé : ce qui lui manque se lit dans leur JSON, pas
dans le code de sortie. Sans l'un de ces encodeurs, le moteur
compile, monte et exporte, mais refuse cet encodeur avec l'erreur
`FfmpegCapabilityMissing`, avant que FFmpeg soit lancé.

La formule `ffmpeg` de Homebrew, celle que `brew bundle --file=Brewfile`
installe, n'a pas `drawtext` (constaté le 1er octobre
2026, avec FFmpeg 9.0.1) : ce n'est plus un manque. Compiler le moteur contre
elle n'a pas été essayé avec ce dépôt ; le Mac de l'auteur compile contre un
FFmpeg 8.1 de conda-forge.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

La suite passe sur le Mac de l'auteur ; elle n'a pas été rejouée sur une
machine propre. Le 1er octobre 2026, sous macOS 14 avec le FFmpeg de
Homebrew, 4 suites sur 115 échouaient : `cutmachine_cli_tests`,
`cutmachine_caption_generation_tests` et `cutmachine_export_tests` faute de
`drawtext`, et `cutmachine_color_effects_tests` sur l'empreinte d'une LUT
d'étalonnage, dont les derniers bits dépendent du compilateur et de la libm.
Les deux causes sont traitées depuis, sans que la CI l'ait rejoué : le test
de la LUT compare à une tolérance (7 octobre), et les légendes ne passent
plus par `drawtext` (9 octobre).

Sous Linux, sans fenêtre, voir [`headless/README.md`](headless/README.md) :
le moteur, la ligne de commande et le serveur MCP y compilent contre le
FFmpeg de la distribution. Les légendes, le motion et les visages, qui
passent par les frameworks d'Apple, y sont refusés par leur nom, et l'export
passe par `--software` (HEADLESS-2026-10).

Si `ccache` est installé (`brew install ccache`), CMake l’utilise
automatiquement : changer de branche ne recompile plus que les fichiers dont le
contenu a changé, whisper.cpp et OpenColorIO compris. Relancé sans aucun
changement, le build ne recompile ni ne relie rien.

Le test natif `cutmachine_ui_smoke_tests` ouvre l’éditeur sur un projet
jetable et vérifie les vrais contrôles AppKit/Metal : affichage/masquage du
moniteur Source, glissières de l’Inspecteur, montage Insérer/Écraser et dépôt
d’un média dans la timeline. Il couvre aussi le clic de playhead, le déplacement
d’un clip et la navette J/K/L à vitesses 1×, 2× et 4×.

```sh
ctest --test-dir build -R cutmachine_ui_smoke_tests --output-on-failure
```

Il s'exécute dans une session macOS graphique, sans autorisation
Accessibilité, et a besoin que sa fenêtre devienne la fenêtre active : certains
de ses clics ne passent que par elle, et ses captures QA ne voient que ce que
WindowServer affiche. Lancé pendant qu'on se sert du Mac (une autre
application devant, la fenêtre couverte ou sur un autre bureau), il ne joue
pas les vérifications qui en dépendent : elles sortent `SKIP:` avec la
raison. Il a trois issues :

- un échec réel : code 1, que CTest affiche « Failed » ;
- rien d'échoué mais des vérifications non jouées : code 77, que CTest
  affiche « Skipped », et dont la dernière ligne dit combien et pourquoi
  (`UI smoke: 0 failure(s), 9 not run: the editor window could not become
  key`) ;
- tout joué et vert : code 0.

`--output-on-failure` n'affiche pas la sortie d'un test sauté : lancez
`ctest -R cutmachine_ui_smoke_tests -V`, ou lisez
`build/Testing/Temporary/LastTest.log`. Un « Skipped » ne dit rien du code :
relancez-le écran libre. `CUTMACHINE_UI_SMOKE_SIMULATE=no-focus` (ou
`hidden`, qui tient aussi la fenêtre pour cachée) simule ce cas sur un écran
libre, sans rien mettre au premier plan (PRODUIT-2026-10b PRD28).

## Premier parcours

Cinq commandes mènent d'un dossier vide à un fichier exporté : créer un
projet, ingérer un rush, lire l'état, poser un plan, exporter. Elles se
lancent depuis la racine du dépôt, une fois `build/cutmachine` compilé, et
chacune répond un objet JSON sur la sortie standard. Les fichiers d'essai
vont dans un dossier à part ; un rush synthétique de quatre secondes suffit :

```sh
ESSAI=/tmp/cutmachine-essai
mkdir -p "$ESSAI"
ffmpeg -f lavfi -i testsrc=duration=4:size=1280x720:rate=25 \
  -pix_fmt yuv420p "$ESSAI/rush.mp4"
P="$ESSAI/Essai.cutmachine-project"
```

Créer le projet. La réponse nomme sa timeline et ses deux pistes, chacune
avec son alias (`T1` pour l'image, `T2` pour le son) et son identifiant ;
l'identifiant de la piste `video` sert plus bas :

```sh
./build/cutmachine --create-project "$P" "Essai" --format 1280x720@25
```

```json
{"ok":true,"path":"…/Essai.cutmachine-project/project.cutmachine.json","timeline_id":"01M3YF5EWXZD47D63SBJXJDFP5","width":1280,"height":720,"frame_rate":{"num":25,"den":1},"tracks":[{"alias":"T1","id":"01M3YF5EWXZD47D63SBJXJDFP6","kind":"video","index":0},{"alias":"T2","id":"01M3YF5EWXZD47D63SBJXJDFP7","kind":"audio","index":1}]}
```

Ingérer le rush. Il entre dans la médiathèque ; rien n'est encore monté :

```sh
./build/cutmachine --ingest "$P" "$ESSAI/rush.mp4"
```

```json
{"ok":true,"added":1,"skipped":0,"ignored":0,"errors":[]}
```

Lire l'état. Le rush figure dans `library` sous l'alias `M1`, avec son
identifiant ; les pistes sont dans `timeline.tracks`, sous les alias `T1`
et `T2`, encore vides :

```sh
./build/cutmachine --describe "$P" | python3 -m json.tool
```

Poser un plan : deux secondes du rush, prises une seconde après son début,
au début de la timeline. `<piste-video>` et `<rush>` sont à remplacer par
l'identifiant de la piste `video` et par celui du rush, lus plus haut. Les
temps sont des rationnels `value`/`rate`, et un `clip_id` vide laisse le
moteur nommer le plan :

```sh
./build/cutmachine --apply-op "$P" \
  '{"type":"InsertClip","track_id":"<piste-video>","source_id":"<rush>","source_in":{"value":25,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"clip_id":"","exact_timeline":[]}'
```

```json
{"ok":true,"doc_hash":"099c85544848cae5","delta":{"duration":{"value":50,"rate":25},"clips":[{"clip_id":"01M3YF5R3R78B0APY67FCTQNNH","track_id":"01M3YF5EWXZD47D63SBJXJDFP6","source_id":"01M3YF5EZRXBGW7AFENEHBBY4F","source_in":{"value":25,"rate":25},"duration":{"value":50,"rate":25},"timeline_in":{"value":0,"rate":25},"created":true}]}}
```

La réponse est le delta : ce que l'opération a changé, plan créé compris.
Relancé, `--describe` montre ce plan sur la piste, sous l'alias `A1`.

Exporter. Sans autre argument, le fichier est un MP4 H.264 High 8 bits au
format de la timeline — le défaut que le moteur donne à une timeline SDR, le
même qu'`export_timeline` — ici 50 images, avec une piste AAC stéréo,
silencieuse puisque rien n'est monté sur la piste son. `--codec hevc` ou
`--preset hevc_high_quality` écrit du HEVC Main10 :

```sh
./build/cutmachine --export "$P" "$ESSAI/essai.mp4"
```

```json
{"ok":true,"codec":"h264","profile":"high","timeline_id":"01M3YF5EWXZD47D63SBJXJDFP5","range":"entire","width":1280,"height":720,"frame_rate":{"num":25,"den":1},"frames":50,"path":"…/essai.mp4"}
```

Un second export vers le même fichier est refusé (`export destination
already exists`) tant que `--overwrite` n'est pas ajouté. Le dossier d'essai
se retire d'une commande :

```sh
rm -r "$ESSAI"
```

Ce parcours passe par `--apply-op`, c'est-à-dire par l'opération elle-même :
identifiants complets, orthographe exacte, aucun alias. Les outils du
moteur — `--tools` en rend le catalogue, `--call-tool` en appelle un,
`--mcp-serve` les sert à un agent, avec les procédures de montage que le
binaire embarque (`--procedure` les rend aussi) — font le même travail à un
niveau
au-dessus : ils acceptent les alias (`M1` pour le rush, `T1` et `T2` pour
les pistes), le nom `media_id` comme `source_id`, et les valeurs
d'énumération sans regarder la casse ; `edit_source_range` pose en un appel
l'image et le son liés d'un rush :

```sh
./build/cutmachine --call-tool "$P" edit_source_range \
  '{"media_id":"M1","track_id":"T1","audio_track_ids":["T2"],"mode":"insert","timeline_in":{"value":0,"rate":25}}'
```

Le même projet s'ouvre dans la fenêtre : voir [Lancement](docs/reference/lancement.md).

Un serveur (`--mcp-serve`) et l'application tiennent un journal,
`~/Library/Logs/CUTMACHINE/cutmachine.log` (sous Linux,
`~/.local/state/cutmachine/cutmachine.log`) : une ligne au démarrage
(surface, version, commit, pid), une par appel d'outil (outil, `ok` ou
l'erreur, durée, timeline nommée), une à l'arrêt, une si le processus tombe ;
jamais les arguments ni les réponses. `CUTMACHINE_LOG_DIR` change le
dossier, `CUTMACHINE_LOG=0` le coupe. `describe` avec `only: ["history"]`
rend l'historique d'une timeline.

Dans la fenêtre, le panneau Agent tient le journal de ce qu'un agent branché
sur l'application a fait : une ligne par édition, export, analyse lancée ou
refus, jamais pour une lecture, à son heure, avec ce qu'elle a changé (« 3
plans modifiés, 1 créé ») ; une intervention (`begin_intervention`,
`end_intervention`) y tient en une ligne à son libellé. Les plans que son
dernier appel a créés ou modifiés portent le badge « AGENT » sur la timeline
jusqu'au prochain geste de l'éditeur. Ce journal est un état de la fenêtre,
écrit ni dans le projet ni sur le disque.

## Plan de la référence

La référence est sous [`docs/reference/`](docs/reference/README.md), un
fichier par section :

- [Lancement](docs/reference/lancement.md) : ouvrir l'application, la boucle de
  développement, [quel FFmpeg](docs/reference/lancement.md#quel-ffmpeg) le moteur lance, le stockage et
  la collecte d'un projet.
- [Décisions de temps](docs/reference/decisions-de-temps.md) : ce que le temps exact impose
  au scrub, au playhead et à la composition des pistes.
- [Timeline graphique](docs/reference/timeline-graphique.md), [Audio](docs/reference/audio.md) et
  [Chutiers](docs/reference/chutiers.md) : la fenêtre.
- [API d'édition](docs/reference/api-edition.md) : les opérations et les outils de montage,
  les réglages d'un plan, et le [catalogue des outils par famille](docs/reference/api-edition.md#le-catalogue-par-famille).
- [Commandes headless](docs/reference/commandes-headless.md) : la ligne de commande, le
  serveur MCP et son journal, les interventions et les plans d'édition.
  `cutmachine --help` y renvoie.
- [Étapes durables et mesures d’agent](docs/reference/orchestration-montage.md) :
  reçus d’étapes sans double édition, captures SQL liées et rapport ACP.
- [Archive SQL des projets](docs/reference/archive-projets.md) : captures
  JSON automatiques à la sauvegarde, rangées par empreinte et compressées,
  import des projets existants, reprise après coupure, consultation SQL/JSON
  au CLI ou par MCP, et copie quotidienne hors du disque.
- [Import depuis DaVinci Resolve](docs/reference/resolve.md) et
  [Format de séquence](docs/reference/format-de-sequence.md).
- [Transcription verbatim et nettoyage des
  hésitations](docs/reference/transcription.md) : le
  modèle Whisper, les mots, les silences.
- [Contrôle qualité des plans](docs/reference/controle-qualite.md),
  [Regarder un plan](docs/reference/regarder.md#regarder-un-plan),
  [Regarder le montage en planches](docs/reference/regarder.md#regarder-le-montage-en-planches),
  [Regarder un raccord tel qu'il est livré](docs/reference/regarder.md#regarder-un-raccord-tel-quil-est-livré),
  [Visionner les raccords](docs/reference/regarder.md#visionner-les-raccords) et
  [Mesurer l'étalonnage](docs/reference/mesurer-etalonnage.md) : ce que le moteur mesure et
  montre.
- [EDL CMX 3600 et WebVTT](docs/reference/edl.md) : les listes d'une timeline
  et de ses marqueurs, la lecture des marqueurs, les sous-titres WebVTT.
- [OTIO et FCPXML](docs/reference/echange.md) : une timeline écrite pour
  un autre éditeur, toutes ses pistes dans un fichier.
- [Export vidéo final](docs/reference/export.md) : l'export, sa vérification, les
  légendes, la mesure du son.
- [Séquence](docs/reference/sequence.md) : le document, la gestion colorimétrique, les
  opérations projet.
- [Agent intégré et clé utilisateur](docs/reference/agent-integre.md) et
  [Sidecar conversationnel](docs/reference/sidecar.md) : le pilotage par modèle.
- [Génération payante](docs/reference/generation.md) : proposer puis générer
  avec fal.ai, sur la clé de la personne et seulement à sa demande ; la
  provenance.
- [Images de référence dans les tests](docs/reference/tests-golden.md) :
  comparer un rendu CPU à une image PNG, avec sa tolérance, et réécrire les
  références.
- [Licence](#licence).

## Licence

CUTMACHINE est distribué sous la GNU Affero General Public License, version 3
ou toute version ultérieure (`AGPL-3.0-or-later`). Consultez [LICENSE](LICENSE)
pour les conditions complètes.

Le code et les ressources d'autres projets que le build intègre — whisper.cpp,
OpenColorIO et les bibliothèques qu'il construit, Yoga, SQLite, les icônes
Lucide —
sont relevés, avec leur version, leur licence et l'endroit où ils
interviennent, dans [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). Le
build copie ce fichier et leurs textes de licence dans
`CUTMACHINE.app/Contents/Resources/licenses/`. FFmpeg n'est pas embarqué : sa
licence est celle de la distribution installée sur la machine.

