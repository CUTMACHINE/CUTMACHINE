# Commandes headless

> Relu contre le code le 3 octobre 2026 ; les exemples de `--apply-op`,
> `--apply-project-op`, des plans d'édition et de `--create-project` ont été
> rejoués sur un projet d'essai. Non vérifiés : ce que fait Claude Code de
> `.mcp.json`, et After Effects.

Ces commandes s'exécutent avant toute initialisation d'AppKit, de Metal ou du
décodage média :

```sh
./build/cutmachine --create-project ./Film.cutmachine-project "Film"
./build/cutmachine --create-project ./ITW.cutmachine-project "ITW" \
  --format 1080x1920@25
./build/cutmachine --after-request \
  '{"schema":"after-request.v2","method":"list_comps","arguments":{}}'
./build/cutmachine --transcribe \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>[,<media-id>…]' \
  ./models/ggml-large-v3.bin fr --verbatim
./build/cutmachine --transcribe-timeline \
  ./Film.cutmachine-project/project.cutmachine.json \
  --timeline '<timeline-id>' --language fr --verbatim
./build/cutmachine --srt-from-media ./film.mp4 ./film.srt --language fr
./build/cutmachine --align-transcripts \
  ./Film.cutmachine-project/project.cutmachine.json --write
./build/cutmachine --tighten-pauses \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' 320 4
./build/cutmachine --trim-boundary-air \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' 3 300
./build/cutmachine --close-junction-air \
  ./Film.cutmachine-project/project.cutmachine.json \
  '<clip-gauche-id>' '<clip-droit-id>' 3 300
./build/cutmachine --locate-source-frame \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>' 1412
./build/cutmachine --disfluencies \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>'
./build/cutmachine --remove-words \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  '[{"start_word_index":40,"end_word_index":40}]'
./build/cutmachine --shot-quality \
  ./Film.cutmachine-project/project.cutmachine.json '<media-id>'
./build/cutmachine --shot-quality-report \
  ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --isolated-frames \
  ./Film.cutmachine-project/project.cutmachine.json --analyze
./build/cutmachine --isolated-frames \
  ./Film.cutmachine-project/project.cutmachine.json --analyze \
  --media '<media-id>' --media '<media-id>'
./build/cutmachine --contact-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./contacts.jpg \
  --timeline '<timeline-id>' --max-images 24
./build/cutmachine --cut-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./raccords.jpg \
  --timeline '<timeline-id>' --max-images 24
./build/cutmachine --review-cuts \
  ./Film.cutmachine-project/project.cutmachine.json ./bandes.jpg \
  --timeline '<timeline-id>' --frame 250 --max-bands 6
./build/cutmachine --measure-color \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>,<clip-id>' \
  --position Middle --region 2/5,1/10,1/5,1/5
./build/cutmachine --match-color \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  '<reference-clip-id>' --match exposure,balance,saturation
./build/cutmachine --export-cdl \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>,<clip-id>' \
  --file ./scene.ccc
./build/cutmachine --apply-cdl \
  ./Film.cutmachine-project/project.cutmachine.json '<clip-id>' \
  --space acescct --file ./scene.ccc --id '<correction-id>'
./build/cutmachine --media-sheet \
  ./Film.cutmachine-project/project.cutmachine.json ./derush.jpg \
  --media '<media-id>' --max-images 12
./build/cutmachine --describe ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"TrimClip","clip_id":"01K00000000000000000000003","edge":"Tail","delta":{"value":-1,"rate":25},"exact_clip":null}'
./build/cutmachine --apply-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"SetClipAudio","clip_id":"01K00000000000000000000007","gain_db":{"num":10,"den":1},"fade_in":{"value":12,"rate":25},"fade_out":{"value":12,"rate":25}}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '{"type":"AddProjectTimeline","name":"Vertical","width":1080,"height":1920,"frame_rate":{"num":25,"den":1},"timeline_id":"","video_track_id":"","audio_track_id":"","exact_project_hex":null}'
./build/cutmachine --apply-project-op ./Film.cutmachine-project/project.cutmachine.json \
  '[{"type":"RenameProjectItem","item_id":"<timeline-id>","name":"Version courte","exact_project_hex":null},{"type":"SetActiveProjectTimeline","timeline_id":"<timeline-id>","exact_project_hex":null}]'
./build/cutmachine --undo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --redo-project-op ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --help
./build/cutmachine --help export
./build/cutmachine --version
./build/cutmachine --check-setup
./build/cutmachine --procedure
./build/cutmachine --procedure monter-une-itw mots-et-bornes.md
./build/cutmachine --tools
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json list_timelines
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json clone_timeline \
  '{"source_timeline_id":"<timeline-id>","name":"Déclinaison courte"}'
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --recursive
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./voix-off.wav
./build/cutmachine --ingest ./Film.cutmachine-project/project.cutmachine.json ./rushes --bin <bin-id>
./build/cutmachine --mcp-serve --port 8765
./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --port 8765
./build/cutmachine --mcp-serve --stdio
./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --stdio --require-timeline
./build/cutmachine --mcp-serve --stdio --profile motion
./build/cutmachine --tools --profile motion
CUTMACHINE_MCP_TOKEN=<jeton> ./build/cutmachine --mcp-serve ./Film.cutmachine-project/project.cutmachine.json --port 8765
curl -sS http://127.0.0.1:8765/mcp -H 'Content-Type: application/json' \
  -H "Authorization: Bearer $CUTMACHINE_MCP_TOKEN" -d '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
./build/cutmachine --import-resolve ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
./build/cutmachine --import-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json ./timeline.json \
  --resolve-database '<…>/Projects/Film/Project.db'
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./film.mp4
./build/cutmachine --export ./Film.cutmachine-project/project.cutmachine.json ./vertical.mp4 \
  --timeline '<timeline-id>'
./build/cutmachine --export-srt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.srt --timeline '<timeline-id>'
./build/cutmachine --export-srt ./Film.cutmachine-project/project.cutmachine.json \
  ./controle-mots.srt --timeline '<timeline-id>' --word-by-word
./build/cutmachine --export-vtt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.vtt --timeline '<timeline-id>'
./build/cutmachine --export-edl ./Film.cutmachine-project/project.cutmachine.json \
  ./film.edl --timeline '<timeline-id>'
./build/cutmachine --export-edl-markers \
  ./Film.cutmachine-project/project.cutmachine.json ./marqueurs.edl --style resolve
./build/cutmachine --export-otio ./Film.cutmachine-project/project.cutmachine.json \
  ./film.otio --timeline '<timeline-id>'
./build/cutmachine --export-fcpxml ./Film.cutmachine-project/project.cutmachine.json \
  ./film.fcpxml --timeline '<timeline-id>'
./build/cutmachine --import-edl-markers \
  ./Film.cutmachine-project/project.cutmachine.json ./marqueurs-resolve.edl
./build/cutmachine --export-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json --timeline '<timeline-id>'
./build/cutmachine --send-resolve ./Film.cutmachine-project/project.cutmachine.json \
  --timeline '<timeline-id>' --name 'Film v3' --grade cdl
./build/cutmachine --add-multicam-group \
  ./Film.cutmachine-project/project.cutmachine.json '<plan-large>,<plan-serré>' \
  --name Interview --angle-name Large --angle-name Serré --active '<plan-large>'
./build/cutmachine --set-multicam-angle \
  ./Film.cutmachine-project/project.cutmachine.json '<plan-ou-angle>'
./build/cutmachine --set-multicam-angle \
  ./Film.cutmachine-project/project.cutmachine.json none --group '<groupe>'
./build/cutmachine --list-multicam-groups \
  ./Film.cutmachine-project/project.cutmachine.json
./build/cutmachine --remove-multicam-group \
  ./Film.cutmachine-project/project.cutmachine.json '<groupe>'
```

`--create-project` accepte `--format LARGEURxHAUTEUR@CADENCE` (par exemple
`1080x1920@25` ou `3840x2160@24000/1001`) pour créer directement la séquence
au format de livraison, et rend l'identifiant de sa timeline. Cette timeline
porte une piste image et une piste son, comme une timeline ajoutée par
`add_timeline`, et la réponse les nomme, chacune avec son alias (`T1`,
`T2`) : un premier plan s'insère sans créer de piste.

`cutmachine --help` liste toutes les commandes avec leurs arguments ;
`cutmachine --help <commande>` (avec ou sans les deux tirets) ou
`cutmachine <commande> --help` donne l'usage d'une seule, en texte brut,
code 0. Une ligne de commande refusée répond `ValidationFailed`, code de
sortie 2, commandes motion comprises : une commande inconnue est nommée, avec
la plus proche quand c'est une faute de frappe ; une commande connue dit ce
qui ne va pas (`--create-project is missing <folder> <name>`, `unknown
--describe option '--frob'`, `--export option '--codec' needs a value`,
`--contact-sheet option '--timeline' is given twice`, `--export-resolve-xml
option '--resolve-scaling' does not take 'zoom'`) et donne sa ligne d'usage,
celle de `--help`. `--describe`, `--shot-quality-report`, `--isolated-frames`
et `--locate-source-frame` mettent `"ok":true` devant leur rapport, inchangé ;
les outils MCP qui rendent les mêmes rapports répondent sans (PRD39). Un chemin de projet où rien n'existe est refusé en `IoError` avec le
chemin ; un dossier qui existe sans être un paquet garde `unsupported project
package`. Un projet écrit par un CUTMACHINE plus récent le dit, avec la
version trouvée et la plus haute que ce binaire lit. `--ingest` répond
`ok:true` avec `added`, `skipped`, `ignored` et `errors` ; quand il n'a rien
pu lire (aucun fichier ajouté, aucun déjà connu), il refuse en `IoError`,
code 1, avec ces quatre champs sous `result`. Un dossier dont une partie
seulement échoue reste un succès. `ingest_media` répond de même. Depuis le
24 octobre 2026 (DEC32, PRODUIT-2026-10),
un ingest est une opération, `IngestMediaOperation` : les nouveaux rushes et
les rushes connus reclassés forment une seule entrée du journal de la
timeline active, que l'outil `undo` (ou `Cmd+Z` dans l'app) reprend et que
`redo` rejoue, octet pour octet. `--bin <id>` (l'argument `bin_id` de
`ingest_media`, ULID ou alias ; `""` pour la racine) range ce qui est
ingéré dans ce chutier, y compris un rush déjà connu, qui y est déplacé ;
sans lui, les nouveaux rushes vont à la racine et les connus restent où ils
sont. Un chutier que le projet n'a pas est refusé avant toute lecture
(`UnknownBin` au CLI, `ValidationFailed` au MCP, qui résout l'identifiant
comme pour tout outil). Seules deux réparations restent hors du journal,
comme avant : une entrée de version 1 complétée par la sonde, et une entrée
complète sans source qui en reçoit une.

`--mcp-serve` ouvre le serveur MCP local (boucle locale uniquement). Le projet
est facultatif : lancé sans projet, le serveur ne répond qu'à `create_project`,
`open_project`, `get_procedure`, à la lecture de l'archive des projets et aux
outils `after_*`, qui adressent un projet After et non le projet CUTMACHINE
(MOTION-AFTER-2026-09 MA10) ; il refuse tout autre outil avec `NoProjectOpen`. Un run peut
ainsi partir d'un dossier de rushes sans quitter MCP : `create_project`, puis
`ingest_media`, qui fait ce que fait `--ingest`. Le serveur tient le verrou de
session du projet ouvert et un seul : ouvrir ou créer un autre projet
verrouille et lit le nouveau d'abord, puis relâche le précédent ; un refus
(chemin faux, projet tenu ailleurs, paquet illisible) laisse le précédent
ouvert et le dit. `--ingest` prend désormais ce même verrou : il est refusé tant que
l'application, un serveur ou `--call-tool` tient le projet, parce qu'un ingest
écrit à côté laissait périmé le projet qu'un serveur garde en mémoire.

`--profile <nom>` choisit les outils que le serveur sert, en stdio comme sur
HTTP (PROFIL-2026-11). Un client charge dans
le contexte de son modèle chaque outil listé : le catalogue entier pèse
environ 87 000 tokens. `full`, le défaut, sert tout ; `motion` sert le motion,
les clips de motion et le pont After en entier, plus ce qu'il faut autour
(projet, timelines, clips, musique, regard, export, annulation, tâches) : 63
outils sur 210 le 9 novembre 2026, environ 23 000 tokens. Un outil hors
profil est refusé par son nom, `ToolNotInProfile`, qui dit comment l'avoir ;
les consignes du serveur disent le profil et ne citent que ce qu'il sert, et
stderr l'écrit au démarrage (« Profile 'motion': 63 of 210 tools »). Deux
autres profils (PA10, PARITE-2026-11) :
`itw` sert en entier la parole, les légendes et la session, plus ce que cite
la procédure `monter-une-itw` et ce qu'un film demande autour ; `reel` sert en
entier les réglages d'un plan, les images, le son, la génération, les clips
de motion et la session, plus ce que cite `monter-un-reel`. Chacun sert un
peu plus de la moitié du catalogue : le gain est moindre que pour `motion`.
Relayé
vers la fenêtre, qui sert tout le catalogue, le serveur stdio répond lui-même
à `initialize`, à `tools/list` et aux appels hors profil. Un nom inconnu, ou
l'option sans valeur, arrête le démarrage (`ValidationFailed`, avec les
noms possibles). `--tools --profile <nom>` imprime ce que le profil liste.
Un profil choisit ce qu'on montre au client ; ce n'est pas une permission.

Sur HTTP, le serveur ne répond qu'à un client local. Écouter sur la boucle
locale écarte les autres machines, pas une page web ouverte dans un
navigateur de celle-ci : chaque requête est donc contrôlée avant d'atteindre
un outil. `Host`, s'il est envoyé, doit désigner `127.0.0.1`, `localhost` ou
`[::1]`, avec ou sans port. `Origin`, s'il est envoyé, doit être une origine
de boucle locale (`http://127.0.0.1`, `http://localhost` ou `http://[::1]`,
quel que soit le port) : toute autre valeur, `null` compris, reçoit 403. Un
`POST` doit annoncer `Content-Type: application/json`, sinon 415. Le serveur
ne renvoie jamais d'en-tête `Access-Control-*`. Le corps JSON d'un refus dit
quoi corriger. Un client qui n'est pas un navigateur (curl, un client MCP)
n'envoie pas d'`Origin` et passe comme avant, pourvu qu'il annonce
`application/json`.

`--token <jeton>` exige en plus `Authorization: Bearer <jeton>` sur chaque
requête ; sans lui, ou avec un autre, la réponse est 401 avec
`WWW-Authenticate: Bearer`. Sans l'option, le jeton est lu dans
`CUTMACHINE_MCP_TOKEN` (environnement, ou `~/.config/cutmachine/.env`) ;
l'option l'emporte. La variable est préférable : les arguments d'un
processus se lisent avec `ps` depuis n'importe quel compte de la machine, et
le binaire ne peut qu'effacer la valeur après l'avoir lue. Le jeton n'est
jamais affiché ni journalisé ; vide ou inutilisable (blanc, caractère de
contrôle, non ASCII), il empêche le serveur de démarrer. Sans jeton, tout
processus de la machine qui peut ouvrir le port reste accepté : les
configurations existantes pointent sur l'URL nue. `--stdio` refuse `--token`
et ne lit pas la variable.

`initialize` répond la version du protocole demandée si le serveur la
connaît (`2024-11-05`, `2025-03-26`, `2025-06-18`, `2025-11-25`), et
`2025-11-25` sinon ; `serverInfo` porte la version que rend
`cutmachine --version`.

Le serveur sert aussi les procédures de montage que le binaire embarque, les
skills du dépôt (`monter-une-itw`, `motion-design`, `monter-un-reel`).
`get_procedure` répond sans projet ouvert : sans argument la liste (nom,
description, fichiers et tailles), avec `name` le `SKILL.md` et ses
fichiers, avec `name` et `file` ce fichier ; un fichier Markdown vient avec
ses liens résolus (`links`). Un chemin absolu ou qui contient `..` est
refusé. Chaque procédure est aussi un prompt MCP (`prompts/list`,
`prompts/get`) ; `initialize` déclare la capacité `prompts`.
`--procedure [<nom> [<fichier>]]` rend les mêmes réponses.

`train_caption_breaks` répond lui aussi sans projet ouvert : il entraîne la
découpe apprise des cartons sur des sous-titres livrés et la mesure contre la
règle (RM3, voir [Export](export.md#légendes)). En ligne de commande :
`--train-caption-breaks <corpus>... [--evaluate <corpus>]... [--install]
[--folds <n>] [--epochs <n>] [--step <n>]` rend la même réponse, chaque
chemin relatif lu depuis le dossier courant.

Le serveur reçoit les lots JSON-RPC quelle que soit la version : un tableau
de réponses, aucune pour une notification ; `initialize` est refusé dans un
lot. Sur HTTP, un `MCP-Protocol-Version` qui ne nomme pas l'une des quatre
versions reçoit 400 ; sans l'en-tête rien ne change. Une réponse JSON-RPC
postée par un client reçoit 400 (elle est ignorée sur stdio). Un corps se lit
par son seul `Content-Length` : deux valeurs différentes, ou autre chose qu'un
nombre, reçoivent 400 ; un corps `chunked` reçoit 411. Un `GET` reçoit 405
avec `Allow: POST`. `--port` prend un entier de 1 à 65535, ou 0 pour un port
libre choisi par le système et affiché dans la bannière.

`--mcp-serve --stdio` sert le même protocole sur l'entrée et la sortie
standard, un message JSON-RPC par ligne : c'est le transport qu'un client MCP
lance lui-même. Le dépôt en déclare un dans [`.mcp.json`](../../.mcp.json), si bien
qu'une session Claude Code ouverte ici voit les outils du moteur
(`mcp__cutmachine__describe`, `mcp__cutmachine__insert_clip`…) sans serveur à
lancer à la main ni `--call-tool` dans un shell — à condition que
`build/cutmachine` soit compilé ; Claude Code demande une fois d'approuver ce
serveur, et `claude mcp reset-project-choices` revient sur ce choix. Ce
transport ne prend pas de `--port` ; la sortie standard ne porte que le
protocole, tout autre message (la bannière, les journaux) va sur la sortie
d'erreur, et fermer l'entrée standard arrête le serveur, qui relâche le
projet. Un serveur par session, un projet ouvert à la fois : tout ce que dit
le paragraphe précédent s'applique, sauf pour un projet que l'application tient
ouvert : `open_project` n'est pas refusé, le serveur relaie alors chaque appel
à la fenêtre qui le tient (AX1), et sa réponse le dit (`app: true`). La
fenêtre garde son projet jusqu'à ce que CUTMACHINE se ferme : `create_project`
et `open_project` d'un autre projet y sont refusés en `UnsupportedOperation`,
avec le chemin vers un autre projet, et `ingest_media` passe par l'import de
la fenêtre, avec la réponse de `--ingest` (F2.4, voir
[Agent intégré](agent-integre.md#le-projet-du-panneau)). Dans ce
mode, `get_editor_view` lit ce que l'éditeur regarde : la timeline à l'écran,
la tête de lecture (exacte, et en image de la séquence), les plans sous la
tête de lecture sur chaque piste avec l'image source que chacun y joue, la
sélection, les marqueurs sur lesquels ou dans lesquels se trouve la tête de
lecture (un marqueur avec `duration_frames` est un passage), et la plage
In/Out que l'éditeur a posée (`marked_range` : `in` et `out` tels que
`delete_timeline_range` les prend, la sortie exclue, `valid` quand les deux
sont posés dans l'ordre). C'est ce qu'un agent lit quand l'éditeur dit « ce
plan », « ici », « ce passage », « entre l'entrée et la sortie ». État
d'interface, en lecture seule ; un serveur sans fenêtre le refuse en
`UnsupportedOperation` (AX5, D5).

`capture_editor_window` montre la même fenêtre en image (VQA5) : l'app la
dessine elle-même, hors écran, avec la barre de titre, les panneaux, Source,
Record et la timeline, si bien que la capture marche fenêtre couverte ou
derrière une autre application, sans permission d'enregistrement de l'écran.
`region` choisit la fenêtre entière (`window`, par défaut) ou une surface
(`timeline`, `record`, `source` ; une surface masquée est refusée en
`ValidationFailed`), `scale` un pixel par point (1, par défaut) ou deux. La
réponse porte l'image en JPEG et un texte : sa taille, et où chaque surface
s'y trouve, en pixels depuis le coin haut gauche. Ce que macOS ajoute en
composant l'écran manque : le verre des bords de défilement et du champ de
recherche, et l'atténuation des libellés secondaires, plus clairs ici qu'à
l'écran. Record garde un temps son image précédente quand un plan attend son
décodeur : la capture est alors refusée (`IoError`) plutôt que rendue avec un
trou. Un serveur sans fenêtre la refuse en `UnsupportedOperation`.

### Sortie vidéo de l'éditeur

`get_video_output` liste les écrans connectés et l'état du clean feed :
`selected_screen_id` conserve le choix local, `active_screen_id` nomme la
sortie effectivement allumée, `enabled` dit si elle l'est. Chaque écran
porte son UUID stable (`id`), son nom, ses dimensions en pixels et
`editor_screen`. `set_video_output` choisit cet UUID avec `screen_id`, ou une
chaîne vide pour arrêter la sortie. L'écran de l'éditeur et un écran absent
sont refusés en `ValidationFailed`, sans perdre le choix précédent. Le
choix est enregistré dans les préférences de la machine, sans opération de
montage. La présentation est `rec709_sdr`, comme Record. Les deux outils
demandent un projet relayé à la fenêtre (`app: true`) ; un serveur sans
fenêtre les refuse en `UnsupportedOperation`.

### Journal du moteur

Un serveur (`--mcp-serve`, stdio ou HTTP) et l'application écrivent une ligne
par événement dans `~/Library/Logs/CUTMACHINE/cutmachine.log`, et sous Linux
dans `~/.local/state/cutmachine/cutmachine.log` (PRODUIT-2026-10b PRD35,
HEADLESS-2026-10). L'heure est en UTC, à la milliseconde.

- **Au démarrage** : la surface (`mcp-stdio`, `mcp-http`, `app`), la version,
  le commit (`-dirty` si l'arbre différait) et le pid.
- **Pour chaque appel d'outil** : l'outil, `ok` ou le nom de l'erreur, la
  durée en millisecondes, et la timeline quand l'appel nomme un
  `timeline_id`.
- **À l'arrêt** : `stop`, ou `stop signal=15` si le processus est arrêté par
  `SIGTERM`, `SIGINT` ou `SIGHUP`.
- **Si le processus tombe** : `crash signal=11` ou `crash terminate`. Le
  rapport de plantage du système suit comme avant, et le pid le relie à la
  ligne.

Jamais les arguments ni les réponses : un nom d'outil ou un `timeline_id` qui
n'a pas la forme d'un identifiant est écrit `?`. Les commandes ponctuelles
(`--describe`, `--call-tool`, `--export`…) n'écrivent rien. Ce que la session
du serveur répond seule (`get_task`, `cancel_task`, `list_tasks`, rejeu d'une
clé d'idempotence) n'a pas de ligne.

Plusieurs processus écrivent le même fichier. Il passe à `cutmachine.log.1`
à 5 Mo, et un seul ancien fichier est gardé. `CUTMACHINE_LOG_DIR` remplace
le dossier (les suites de test écrivent dans `build/test-logs`), et
`CUTMACHINE_LOG=0` coupe tout.

Une intervention d'agent s'annule d'un seul Annuler. `--apply-op` accepte un
lot, `{"type":"Batch","label":"…","operations":[…]}` : au plus 256
opérations, appliquées dans l'ordre et refusées en entier dès que l'une l'est,
qui forment une seule entrée du journal. Un client dont l'intervention couvre
plusieurs appels d'outil l'encadre : `begin_intervention` rend un jeton sans
rien écrire, `end_intervention` regroupe en une entrée nommée les pas appliqués
depuis à cette timeline — un seul pas aussi, qui devient un lot d'une
opération sous le libellé (PRD40) —, et refuse, en laissant l'historique tel quel, si
celui-ci a été défait sous le début de l'intervention. Le panneau Agent de
l'application le fait à chaque consigne, sauf si le montage a été modifié à la
main pendant le tour.

```sh
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json \
  begin_intervention '{}'
./build/cutmachine --call-tool ./Film.cutmachine-project/project.cutmachine.json \
  end_intervention '{"intervention":"<jeton>","label":"Resserrer la question 2"}'
```

Un agent peut aussi **proposer** une intervention au lieu de l'appliquer : c'est
un second mode de travail, à côté du montage direct, pas à sa place.
`prepare_edit_plan` exécute des étapes — des outils d'édition du catalogue avec
leurs arguments — sur une copie de la timeline : rien n'est sauvegardé,
journalisé ni activé. Les alias (`A1`, `A2`…) sont résolus une seule fois, sur
la timeline telle qu'elle est au départ : supprimer `A1` puis `A2` supprime les
deux plans vus, jamais l'ancien `A3`. La réponse est mesurée par le moteur :
nombre d'opérations, durées avant et après, delta (identifiants créés compris),
compte rendu de chaque étape, et `base_doc_hash`, la révision de la timeline sur
laquelle le plan a été préparé. `apply_edit_plan` l'applique en un seul pas
d'annulation, avec les mêmes identifiants que l'aperçu ; si la timeline a changé
entre-temps, il refuse avec `StaleRevision` sans rien appliquer.
`get_edit_plan` rend le plan sérialisé par le moteur (`plan_json`, ses
opérations exactes et les identifiants générés), sans rien écrire : c'est ce
qu'on garde avant `commit_run_step`, pour qu'un redémarrage du serveur ne
change pas l'édition proposée (voir
[Étapes durables](orchestration-montage.md)).
`discard_edit_plan` l'oublie. Qui pilote décide : un client MCP peut appliquer
ses propres plans. Un serveur garde au plus 8 plans par timeline en mémoire
(64 Mio en tout), jamais dans le projet. Chaque appel de la CLI étant une session à part, la CLI garde le plan
dans un fichier qu'on lui nomme, hors du projet, et `--call-tool` refuse
`prepare_edit_plan`, `apply_edit_plan` et `discard_edit_plan` (il ne trouve
aucun plan à rendre par `get_edit_plan`, `UnknownPlan`) :

```sh
./build/cutmachine --prepare-edit-plan ./Film.cutmachine-project/project.cutmachine.json \
  '{"timeline_id":"<timeline-id>","label":"Resserrer le début","steps":[{"tool":"remove_clip","arguments":{"clip_id":"A1"}},{"tool":"remove_clip","arguments":{"clip_id":"A2"}}]}' \
  ./plan.json
./build/cutmachine --apply-edit-plan ./Film.cutmachine-project/project.cutmachine.json ./plan.json
./build/cutmachine --discard-edit-plan ./plan.json
```

Sans `--word-by-word`, le SRT part des limites exactes de la transcription par
mot, puis les regroupe par proximité temporelle et unité syntaxique française.
Le découpage global vise **18 caractères par seconde en moyenne** : ce n'est
pas un plafond par carton, donc une proposition peut ponctuellement dépasser
18 CPS pour ne pas séparer un déterminant de son nom, un sujet de son verbe ou
une préposition de son complément. Les fins de phrase et les pauses d'au moins
700 ms restent des frontières obligatoires. `--word-by-word` conserve, lui,
chaque limite brute pour le contrôle d'alignement dans Resolve.

`--after-request` pilote l'instance locale d'After Effects avec le même
contrat strict que les outils MCP et le chat intégré. Le workflow sûr ouvre
d'abord une nouvelle copie de travail avec `open_project_copy`, puis inventorie
les métrages avec `list_footage` — un média hors ligne se rend en mire SMPTE
sans lever d'erreur — et les IDs avec `list_comps` et `describe_comp` avant
toute mutation. `render_frame` écrit une frame en PNG : c'est le seul moyen de
regarder le résultat d'une modification au lieu de le déduire d'un arbre de
propriétés. Voir
[`tools/after-bridge/README.md`](../../tools/after-bridge/README.md).

Les outils MCP qui modifient une timeline acceptent eux aussi le champ
optionnel `timeline_id`. Sans ce champ, ils conservent le comportement
historique et ciblent la timeline active. Pour rendre l'identifiant obligatoire
sur cette machine — utile aux scripts qui pilotent plusieurs montages — activer
le garde-fou local puis relancer CUTMACHINE ou `--mcp-serve` :

```sh
defaults write com.cutmachine.editor CMRequireExplicitTimeline -bool true  # CUTMACHINE.app
defaults write cutmachine CMRequireExplicitTimeline -bool true  # build/cutmachine
```

L'application lit le domaine `com.cutmachine.editor` ; le binaire
`build/cutmachine` (`--mcp-serve`, `--call-tool`, plans d'édition) lit le
domaine `cutmachine`.

Pour une session : `open_project` ou `create_project` avec
`require_timeline: true` (la réponse dit `require_timeline`), ou
`./build/cutmachine --mcp-serve <projet> --stdio --require-timeline`. C'est
le mode d'une série, plusieurs agents sur plusieurs timelines d'un projet
(skill `monter-une-itw`, `serie.md`).

Tout outil qui édite **ou lit** une timeline est alors refusé sans
`timeline_id` (`TimelineRequired`, qui nomme les timelines du projet). Ce
réglage reste dans `NSUserDefaults` ou dans la session et n'est jamais écrit
dans le projet. Lire un rush n'est pas lire une timeline : `read_frame` avec
`media_id` (sans `clip_id`) passe sans `timeline_id`. `media_sheet`,
`list_source_words`, `list_stable_windows` et les analyses audio ne visent
aucune timeline ; `locate_source_frame` et `list_clip_tempo_beats` placent
leur rush sur une timeline et la nomment.

Les lectures d'une timeline — `describe`, `get_timeline_transcript`,
`timeline_stats`, `check_timeline`, `list_shot_quality`, `contact_sheet`,
`cut_sheet`, `list_disfluencies`, `list_speech_onsets`, `locate_source_frame`,
`read_frame` et `list_multicam_groups` — prennent elles aussi un `timeline_id` optionnel, et
lisent la timeline nommée **sans l'activer** ; en mode strict, elles aussi
doivent nommer leur timeline. En CLI, `--describe`, `--timeline-stats` et
`--check-timeline` prennent `--timeline <id>`. Le panneau Agent de l'application ne lit que la timeline à
l'écran : il refuse d'en lire une autre plutôt que d'en changer.

### Historique d'une timeline

`describe` avec `only: ["history"]` rend le journal de la timeline lue
(`timeline_id`, sinon l'active), accompagné de `total` et `applied`. Ses
entrées viennent dans l'ordre, chacune avec :

- son rang ;
- son type tel que le journal l'écrit, ou `Batch` ;
- son libellé s'il en a un ;
- le nombre d'opérations qu'elle applique ;
- `undone: true` si elle est au-delà du curseur d'annulation.

La réponse ne porte que les 200 dernières entrées, et l'historique n'est
jamais dans la réponse par défaut. Une entrée `DeleteGap` est toute édition
qui écrit le contenu de pistes : un blanc fermé, une insertion ou un
écrasement depuis le moniteur Source, une suppression de plage.
`clips_added` et `clips_removed` les distinguent (0 et 0 pour un blanc
fermé). Le journal ne garde ni date ni auteur. La création d'une timeline
(`add_timeline`, `clone_timeline`, `create_timeline_variants`,
`create_interview_short`) est un pas du journal du projet : l'historique de
la timeline commence à sa première édition. Une intervention d'un seul pas y
figure comme un `Batch` d'une opération, avec son libellé. `--describe` n'a pas
d'équivalent : `--call-tool <projet> describe '{"only":["history"]}'`.
Une entrée qui porte un motif (ci-dessous) liste ses raisons sous `motifs`.

### Ce qui a changé depuis un état

> PA9, PARITE-2026-11, 11 novembre 2026.

Un agent garde son modèle de la timeline et le corrige avec le delta de
chaque édition. Entre deux de ses tours, la personne monte dans la fenêtre :
le modèle est alors faux sans que rien le dise. `list_changes` prend dans
`since` le `doc_hash` du dernier résultat d'édition que l'agent a vu, et
rend **un** delta de cet état à maintenant (les plans créés, modifiés,
retirés, décalés, comme le delta d'une édition), avec les entrées du journal
entre les deux. Sans `since`, il nomme seulement l'état courant, à redonner
la fois suivante.

- `changed` dit si les deux états diffèrent. Le delta n'a pas de mot pour
  tout : un marqueur renommé, le réglage d'un style, une piste verrouillée
  laissent un delta vide sous `changed: true`, et le type des entrées dit
  quoi relire.
- `undone: true` : l'état demandé est au-delà du curseur d'annulation. La
  timeline est revenue en arrière, les entrées listées ont été annulées.
- `found: false` : le journal ne mène plus à cet état (annulé, puis
  d'autres éditions ; ou à plus de 500 entrées). Ce n'est pas une erreur :
  on relit la timeline avec `describe`.

Le journal ne garde pas d'auteur : les entrées qui suivent la dernière de
l'agent sont celles de quelqu'un d'autre. Rejeter d'un coup ce qu'un agent
a fait reste `undo` sur son intervention repliée (`end_intervention`).

### Le motif d'une édition

Le document garde ce qui a été fait ; le journal garde aussi pourquoi
(MOTIF-2026-10). Chaque outil qui écrit dans le journal d'une timeline —
toutes les éditions, `end_intervention` et `apply_edit_plan`, mais ni `undo`
ni `redo`, qui déplacent des entrées — prend un argument `motif` :

```json
{"reason": "duplicate", "note": "garde la 2e prise", "author": "agent",
 "marker_id": "K3"}
```

- `reason`, un vocabulaire fermé : `hesitation`, `pause`, `boundary_air`,
  `duplicate` (doublon de propos), `selection` (choix du propos), `cutaway`,
  `rhythm`, `brief`, `marker`, `format`, `captions`, `technical`, `other` (qui
  demande une note) ;
- `note`, une ligne de 280 caractères au plus ;
- `author` : `agent` (par défaut), `person` ou `procedure` ;
- `marker_id`, le marqueur auquel l'édition répond, résolu comme tout
  identifiant (alias, ID complet, préfixe) ;
- `params`, un objet, pour un appelant qui veut noter ses propres seuils.

Il faut une `reason` ou une `note`. `rule` est refusé : seul le code d'un
outil nomme sa règle. Un outil qui n'écrit dans aucun journal de timeline
refuse `motif` par son nom ; le catalogue le lit lui-même, aucun outil ne le
voit. `tighten_pauses`, `clean_disfluencies`, `trim_boundary_air`,
`close_junction_air`, `generate_captions` et `conform_sequence` écrivent
leur propre raison, leur `rule` (leur nom) et leurs `params` (seuils lus,
valeurs par défaut comprises) ; l'appelant n'y ajoute que `author`, `note` et
`marker_id`, et une autre raison est refusée avant toute coupe. Un pas de
plan prend son motif dans ses `arguments` ; `apply_edit_plan` couvre de son
motif les membres qu'aucun pas n'a expliqués, `end_intervention` de même pour
les entrées qu'il replie, chacune gardant le sien. `--apply-op <projet> <op>
'<motif>'` pose un motif sur une opération écrite à la main.

Les éditions du projet le prennent aussi (MOTIF6) : `add_timeline`,
`clone_timeline`, `create_timeline_variants`, `create_interview_short`,
`rename_timeline`, `set_timeline_bin`, `remove_timeline`,
`set_active_timeline` et `choose_timeline_variant`. Une entrée du journal du
projet porte au plus un
motif (`add_timeline` avec `activate` en écrit deux, chacune sous le motif
de l'appel) ; son `marker_id` se résout sur la timeline active.
`--apply-project-op <projet> <op> '<motif>'` pose un motif sur une
opération de projet écrite à la main.

`choose_timeline_variant` écrit au journal du projet le choix entre des
timelines (variantes, clones, déclinaisons) : `timeline_id`, la timeline
retenue ; `alternative_timeline_ids`, celles parmi lesquelles elle l'a été
(au moins une, présentes, chacune une fois) ; `activate` pour la rendre
active. Il exige `motif`, avec son `author` : `person` quand la personne a
choisi, `agent` quand le choix a été laissé à l'agent. Le choix est une
trace : il ne change rien au projet, sinon la timeline active si on le
demande.

```json
{"timeline_id": "01K…", "alternative_timeline_ids": ["01K…", "01K…"],
 "motif": {"reason": "rhythm", "author": "person",
           "note": "la plus serrée tient la chute"}}
```

`list_motifs` (`timeline_id`, `reason`, `rule`, `journal`, tous facultatifs)
relit le journal en effet, regroupé par raison, règle et paramètres : chaque
groupe liste ses entrées (rang dans l'historique, auteur, note, marqueur) et
ce que chaque membre a touché (plan, plages de source coupées, rognages
d'air). `operations_without_motif` compte ce que personne n'a expliqué. Les
entrées annulées n'y sont pas. Avec `journal: "project"`, il relit le
journal du projet : chaque entrée dit les timelines qu'elle nomme (créée,
source d'un clone, variantes, retenue et alternatives d'un choix), par leur
nom dans le projet tel qu'il est, `removed: true` pour une timeline retirée
depuis ; il ne prend alors pas de `timeline_id`.

Le motif est une trace, pas un état : il vit dans le journal
(`.editlog.json`), jamais dans le document, et ne change rien à ce qu'une
opération fait. Un journal sans motif s'écrit en version 1, à l'octet comme
avant ; un journal qui en porte un s'écrit en version 2. Un binaire antérieur
à MOTIF-2026-10 refuse un journal en version 2, comme il refuse un document
plus récent que lui : il lit encore la timeline mais refuse de l'éditer
(`ParseError`), et sa fenêtre refuse d'ouvrir le projet ; ce binaire-ci
refuse une version 3 en le disant (`edit log version 3 is newer than this
build reads`). Le journal du projet suit la même politique : version 1 à
l'octet sans motif, version 2 avec, version 3 refusée par son nom (`project
edit log version 3 is newer than this build reads`) ; un binaire antérieur à
MOTIF6 refuse le journal du projet en version 2.

`transcribe_timeline` décode directement les plages source des plans audio,
les replace sur leur grille temporelle exacte et transmet le PCM assemblé à
Whisper sans export vidéo intermédiaire. Son cache dépend de l'identité de la
timeline, des bornes et positions de ses plans audibles, du modèle, de la
langue et du mode verbatim : une coupe audio ne peut donc pas réutiliser un
texte périmé. `--srt-from-media` emploie le même chemin de transcription pour
produire un SRT depuis un livrable sans créer de projet jetable.

Le SRT compte en millisecondes, et une image à 24 ou 29,97 i/s n'y tombe
presque jamais juste. L'écriture d'un SRT (`--export-srt`, `--srt-from-media`,
l'export de l'application) arrondit donc chaque borne à la milliseconde la
plus proche : au plus 0,5 ms, moins d'une demi-image à toute cadence sous
1000 i/s, si bien que le fichier relu retombe sur la même image. C'est le seul
arrondi du chemin, et il se déclare : `rounded_timestamps` compte les bornes
arrondies. Un sous-titre plus court qu'une milliseconde se refuse, et un refus
ne touche jamais au fichier de destination.

`--export-vtt` écrit les mêmes cues en WebVTT, avec le même arrondi et le
même compte ; `--export-edl`, `--export-edl-markers` et
`--import-edl-markers` écrivent et relisent des listes CMX 3600. Les deux
sont décrits dans [EDL CMX 3600 et WebVTT](edl.md), non vérifiés dans
Resolve. `--export-otio` et `--export-fcpxml` écrivent la timeline, toutes
ses pistes dans un fichier, pour un autre éditeur :
[OTIO et FCPXML](echange.md), ouverts dans aucun éditeur à ce jour.

Lorsqu'une enveloppe de parole B1 est disponible, le cache de transcription
indique `speech_assessed`, `measured_speech_duration`,
`likely_hallucinated`, `likely_incomplete` et
`known_hallucination_phrase`. Des mots sans aucun
groupe de parole mesuré sont signalés comme probablement inventés ; une
tournure connue de sous-titrage ou d'abonnement ne fait que renforcer ce
diagnostic et ne suffit jamais à elle seule. Les outils MCP
`list_disfluencies`, `remove_words` et `create_interview_short` refusent ces
transcriptions, sauf demande explicite avec `force: true`.
À l'inverse, au moins dix secondes de parole mesurée sous le plancher
conservateur de 30 mots/minute déclenchent `likely_incomplete` : le texte ne
doit alors pas être interprété comme la preuve que le rush ne dit rien.

Une nouvelle transcription ne remplace pas le cache quand elle paraît moins
bonne que lui (PRD40) : signalée `likely_incomplete` ou `likely_hallucinated`
alors que le cache ne l'est pas, ou, faite de la même façon (verbatim ou
non), avec moins de la moitié de ses mots. Le cache est gardé et le résultat
du média le dit (`cache_kept: true`, `cached_words`, `cache_detail`) ;
`words` et `text` décrivent ce qui vient d'être fait. `overwrite: true` (MCP)
ou `--overwrite` (`--transcribe`) l'installe quand même. La transcription
lancée depuis le menu Média de l'application écrit toujours le cache.

Une passe verbatim que l'enveloppe de parole signale `likely_incomplete` est
refaite sans le prompt verbatim, sur ces médias seulement (ITW9) : sur des
prises en anglais et en espagnol, la passe verbatim a rendu 2 à 6 mots pour
13 à 22 s de voix, la passe ordinaire le texte entier. Quand celle-ci décode
plus de mots et n'est pas `likely_hallucinated`, ses mots deviennent la
transcription, avec les hésitations de la passe verbatim (« euh », « uh »,
« um »… de deux secondes au plus) là où elle n'entendait rien ; le résultat
est évalué de nouveau contre l'enveloppe. Le cache garde `verbatim: true`, ce
qui a été demandé, et dit `plain_pass_words`, les mots venus de la passe
ordinaire : ils ne portent pas d'autre hésitation. La réponse le dit sous
`plain_pass` (`used`, `verbatim_words`, `plain_words`, puis
`kept_verbatim_words`, ou `detail` quand la passe ordinaire n'a pas été
retenue). `list_source_words`, `list_disfluencies` et `clean_disfluencies`
rendent `plain_pass_words` à côté de `verbatim`. Sans enveloppe
(`analyze_speech_onset` d'abord), rien n'est évalué ni refait ; la
transcription d'une timeline et celle du menu Média ne le font pas.

`--apply-project-op` accepte soit une opération canonique, soit un tableau.
Dans ce second cas, les opérations sont appliquées dans l'ordre et forment un
seul pas de `--undo-project-op` ; un refus annule le lot entier. Une opération
projet ne charge plus les journaux des timelines qu'elle ne modifie pas, et la
sauvegarde préserve leurs octets existants.

## Multicam

Un groupe multicam réunit des plans déjà posés sur des pistes image, un par
caméra synchronisée, et désigne un angle actif. Tant qu'un angle est actif,
les autres angles du groupe disparaissent de l'image sur toute leur durée,
comme si leur piste était masquée à cet endroit ; ce qui se trouve dessous
reste visible là où l'angle actif ne couvre pas. Sans angle actif, tous les
angles se composent comme des plans ordinaires. La même règle
(`src/Multicam.h`) sert le moniteur, l'export, `verify_export`,
`timeline_stats`, les planches, `list_shot_quality` et le retour vers
Resolve ; celui-ci refuse un angle masqué dont le son joue encore, parce
qu'il ne sait pas envoyer ce son sans son image.

Le multicam ne touche qu'à l'image : les plans son, liés ou non, continuent de
jouer. Couper le son d'une caméra se fait en coupant sa piste.

Les quatre commandes résolvent les identifiants par ULID complet ou par
préfixe non ambigu, prennent `--timeline` et rendent, en plus du résultat de
`--apply-op`, l'identifiant du groupe, ceux de ses angles, l'angle actif et
les plans masqués. `--set-multicam-angle` accepte un angle ou le plan qu'il
montre ; `none` efface l'angle actif et demande `--group`. Chaque commande
d'écriture est un pas d'annulation et prend le verrou de session du projet.
Les outils MCP correspondants sont `add_multicam_group`,
`set_multicam_active_angle`, `remove_multicam_group` et
`list_multicam_groups` ; dans l'application, le menu Clip propose « Activer
l'angle multicam du clip » et « Afficher tous les angles multicam ».

Couper un groupe (décision du 8 octobre 2026) : `--split-multicam-group
<projet> <groupe> <position>` (images de la timeline, ou `valeur/cadence`),
MCP `split_multicam_group` (`timeline_position`). Chaque angle qui traverse
la position y est coupé, et ce qui suit forme un second groupe, qui peut
montrer un autre angle : c'est le montage multicam de Resolve, dans un
schéma où un groupe n'a qu'un angle actif. Le groupe garde les angles
d'avant la coupe ; le second prend les morceaux de droite et les angles
entièrement après, et montre l'angle qui était actif, ou celui que nomme
`--active` / `active_angle_id` (un angle du groupe). Le son n'est pas coupé :
il ne suit pas l'angle. Un seul pas d'annulation ; refusé quand aucun angle
n'est coupé à cette position. La réponse nomme le second groupe, ses angles
et celui qu'il montre. Dans la fenêtre, le geste n'existe pas encore.
