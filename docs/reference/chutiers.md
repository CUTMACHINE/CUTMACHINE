# Chutiers

> Relu contre le code le 3 octobre 2026. Les gestes à la souris
> (glisser-déposer, sélection, clic droit) ne sont pas vérifiés par lecture.

Le panneau **Projet / Chutiers** occupe la gauche de la fenêtre, hors de
la surface de timeline Metal. Depuis le 31 octobre 2026, il s'ouvre en grand
sur l'onglet **Médias** (Maj+2), et sur les autres onglets d'un clic sur
**Médiathèque**, en bas à droite (ONGLETS-2026-10). La séquence active y apparaît comme un objet
adressable distinct des médias ; un double-clic revient au moniteur Record.
Une arborescence affiche les chutiers imbriqués, la racine et une vue de tous
les objets. Un sélecteur propose une liste de
métadonnées ou une grille d'icônes ; le champ de recherche filtre les deux.
Dans les deux vues, `Cmd+clic` ajoute ou retire un objet de la sélection et
`Shift+clic` sélectionne une plage, comme dans le Finder. Le clic droit conserve
la multisélection et **Déplacer vers un chutier…** classe ensemble tous les
rushes sélectionnés, dans le chutier que son dialogue demande.
**Nouveau chutier** crée un enfant du chutier courant et **Supprimer** refuse un
chutier contenant encore des médias, des enfants ou une timeline. Une timeline
se range par glisser-déposer, ou par `set_timeline_bin` (`timeline_id`, et
`bin_id` vide pour la racine) : une entrée du journal du projet
(`SetProjectTimelineBinOperation`), que `list_timelines` et `describe`
relisent dans le `bin_id` de la timeline. Avant le 4 novembre 2026, seul le
glissé de l'app rangeait une timeline. Les chutiers se déplacent
et s'imbriquent par glisser-déposer, à la manière du Finder ; le dépôt sur
**Projet** (ou sur **Tous**) les replace à la racine. Ces déplacements utilisent
`MoveBinOperation`, rejettent les cycles et suivent donc
`Cmd+Z`/`Cmd+Shift+Z`. Les champs `bins`,
`parent_id` et `bin_id` sont persistés et exposés par `--describe`. Voir la
[`spécification des chutiers`](../BINS_SPEC.md).
Le menu contextuel **Nouveau chutier** crée immédiatement l'élément dans
l'arborescence et active son renommage inline, sans ouvrir de dialogue. Un clic
droit dans la zone vide crée à la racine ; **Renommer** édite également le nom
directement dans la vue.

**Fichier → Importer des rushes…** (`Cmd+I`) accepte plusieurs vidéos ou un
dossier complet et range directement les nouveaux médias dans le chutier
sélectionné. Des fichiers du Finder et des rushes déjà présents peuvent aussi
être déposés sur un chutier. Depuis le 24 octobre 2026 (DEC32), un import
est une seule `IngestMediaOperation` : les nouveaux rushes et les rushes
connus déplacés dans le chutier s'annulent ensemble par `Cmd+Z`. Le CLI
(`--ingest … --bin <id>`) et le MCP (`ingest_media` avec `bin_id`) rangent
dans un chutier de la même façon. Le menu contextuel **Déplacer vers un
chutier…** propose la même organisation sans drag-and-drop via
`SetMediaBinOperation` : son dialogue demande la destination, **Sans chutier**
pour la racine.

L’analyse FFmpeg des imports passe par un `MediaTaskManager` à concurrence
bornée. Chaque tâche possède un ULID, un type, un état, une progression, un
détail et une erreur éventuelle. Le panneau Média montre la tâche active et
permet son annulation coopérative. Les workers ne mutent jamais le projet : un
batch terminé est validé, appliqué et sauvegardé sur le thread principal.

Les proxies utilisent ce même ordonnanceur. Quand **Timeline → Générer
automatiquement les proxies** est coché (il ne l'est pas par défaut), les
rushes supérieurs à 1920 px, HEVC/H.265 ou AV1 déclenchent un transcodage
ProRes Proxy silencieux, limité à 1280 px de large, dans
`.cutmachine/proxies/`. Le chemin
du proxy est persisté dans le projet sans remplacer celui de l'original. Le
menu contextuel d'un rush permet de le générer, le recréer ou le supprimer ;
**Timeline → Utiliser les proxies** bascule immédiatement entre proxy et
original. Un proxy absent, illisible ou incompatible retombe sur l'original.
L'audio et l'export final utilisent toujours les médias originaux : aucun son
n'est embarqué dans le clip vidéo ni dans son proxy.

Les médias contenant de l'audio déclenchent aussi une analyse waveform en
arrière-plan. Le cache binaire, régénérable, vit dans
`.cutmachine/waveforms/` et n'alourdit pas le JSON du projet. Les clips des
pistes audio affichent la portion exacte correspondant à leur `source_in` ;
les pics sont agrégés selon le zoom afin de conserver les transitoires. Les
clips vidéo restent silencieux et n'affichent jamais de waveform.

La vue grille des chutiers utilise des thumbnails vidéo 320×180 générées par
le même scheduler et stockées dans `.cutmachine/thumbnails/`. L'image est
extraite à 10 % du rush (au plus à 10 secondes), respecte l'orientation puis
est letterboxée sans déformation. Une vignette absente ou corrompue est
recréée automatiquement ; **Régénérer la vignette** force son remplacement.
Les médias offline conservent leur avertissement visuel.

Le menu contextuel **Reconnecter le média…** remplace le fichier d'un rush
sans changer son ULID, ses clips, ses liens A/V ni son chutier. Le nouveau
fichier est probé par une tâche `Relink` avant toute mutation. Une cadence
différente, une durée inférieure à l'original ou l'absence d'une piste audio
déjà utilisée est refusée. Après sauvegarde transactionnelle, les workers
vidéo et audio sont reconstruits et les proxy, waveform et thumbnail obsolètes
sont invalidés puis régénérés depuis le remplacement.

**Fichier → Reconnecter les médias offline…** effectue la même opération en
lot depuis un dossier parcouru récursivement. La correspondance utilise le nom
de fichier sans tenir compte de la casse. Un nom absent reste offline ; un nom
présent plusieurs fois est signalé comme ambigu et n'est jamais choisi au
hasard. Tous les remplacements compatibles trouvés sont sauvegardés dans une
seule transaction, puis les workers et caches ne sont reconstruits qu'une
fois pour le lot entier.

Reconnecter change le fichier, pas la façon dont le projet lit son image : la
balance de tournage et l'entrée couleur du média restent celles qu'il avait,
et le chemin rangé est celui du fichier tel qu'il a été choisi, lien compris
(relatif au paquet s'ils partagent un dossier sous la racine, absolu sinon,
comme à l'import ; voir IMPORT-2026-10).
Tout ce que le fichier dit de lui-même (taille, format, étiquettes, durée)
vient du nouveau sondage. Pour adopter les valeurs du fichier, il faut le
demander : `set_media_white_balance` avec `from_camera`,
`set_media_color_input` avec `from_file` (voir
[Mesurer et étalonner](mesurer-etalonnage.md)). Avant le 4 octobre 2026, la
reconnexion en lot donnait aux rushes Sony d'un projet ingéré en septembre la
balance lue dans la caméra (4000 K) au lieu de la balance inconnue contre
laquelle leurs plans avaient été étalonnés : l'image jaunissait, au moniteur
comme à l'export.

Un double-clic sur un média, ou le bouton **Source**, l'ouvre dans le moniteur
Metal. Source et Record possèdent leurs propres points In/Out : `I`, `O` et
`Alt+X` agissent sur le moniteur actif. `F9` insère la zone Source sur la piste
vidéo ciblée en ouvrant les pistes en sync lock ; `F10` écrase la même durée sans
décaler l'aval. Ces montages sont une seule opération exacte, annulable, même
lorsqu'un clip au point de montage doit être scindé. Un drag depuis la liste
ou la grille vers une piste vidéo y insère le rush entier, par ce même montage
Source vers Record journalisé (`TimelineSourceEditOperation`, en insertion),
avec l'ULID de la source, son `source_in`
zéro et sa durée rationnelle complète. Cliquer ensuite dans la timeline rend
le moniteur au programme. `--ingest` crée désormais le `DocumentSource`
montable ayant le même ULID stable que chaque nouveau média de bibliothèque.

L'application installe une barre de menus macOS native : **CUTMACHINE**,
**Fichier**, **Édition**, **Clip**, **Couleur**, **Timeline**, **Lecture** et
**Affichage**. Le clic droit est contextuel : chutiers et médias
dans la médiathèque, clips, gaps et pistes dans la timeline. Les commandes de
montage utilisent les mêmes opérations que le CLI ; le renommage d'un chutier
est notamment une `RenameBinOperation` réversible. Un clip peut aussi être
retrouvé dans la médiathèque depuis son menu contextuel, comme le `Find in
Media Pool` des NLE classiques.
