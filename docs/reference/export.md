# Export vidéo final

> Relu contre le code le 3 octobre 2026. Les mesures citées (contrastes,
> loudness, morceau du reel) et le rendu à l'écran ne sont pas vérifiés par
> lecture.

Le menu **Fichier → Exporter la vidéo finale…** ouvre le module de rendu de
CUTMACHINE, audio AAC stéréo 48 kHz. Le dialogue propose cinq presets, la
liste unique du moteur (`Exporter::Presets()`) que `--export --preset` et
l'argument `preset` d'`export_timeline` et d'`export_timelines` nomment par
leur identifiant : **Haute qualité** (`hevc_high_quality`), **Master x265**
(`hevc_master`), **Web 1080p** (`hevc_web_1080`), **Livraison UHD**
(`hevc_uhd_delivery`) et **H.264 High — Diffusion** (`h264`). Sans codec ni
preset, toutes les surfaces prennent le défaut du moteur : H.264 High 8 bits
pour une timeline SDR, HEVC Main10 quand la gestion couleur livre du HLG ;
le dialogue s'ouvre sur ce défaut. Un preset qui contredit le codec nommé est
refusé. Des menus déroulants permettent de
surcharger ponctuellement le conteneur, la résolution, la cadence ou le moteur
d’encodage ; aucun débit ou timebase n’a besoin d’être saisi manuellement.
**VideoToolbox** privilégie la vitesse et l'encodeur **logiciel** (x264 ou
x265, selon le codec du preset) la qualité constante.
Le rendu est effectué hors du thread de l’interface, expose sa progression et
peut être annulé.

La commande headless équivalente est :

```sh
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --software
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie.mp4 --overwrite
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json sortie-h264.mp4 \
  --codec h264 --software --entire --timeline '<timeline-id>'
./build/cutmachine --export Projet.cutmachine-project/project.cutmachine.json master.mp4 \
  --preset hevc_master
```

H.264 High est le défaut d'une timeline SDR, au CLI comme au MCP ;
`--codec hevc` redonne le HEVC Main10 que `--export` écrivait seul jusqu'au
3 octobre 2026.

Dans un serveur (`--mcp-serve`, relais de l'app), `export_timeline`,
`export_timelines` et `render_motion_video` prennent `async: true` : la
réponse porte un `task_id`, `get_task` lit les images encodées sur le total —
pour un lot, le fichier en cours et le nombre de fichiers finis ; un export
rendu par segments dit sa passe (« segment 2/3 », « assembly ») et la compte
sur les images du film — puis la réponse de l'outil, et `cancel_task` arrête FFmpeg et retire le fichier
partiel. L'export travaille sur le projet tel qu'il était à l'appel ;
`stale: true` dit que la timeline a changé avant la fin. Un chemin de sortie
relatif se résout contre le dossier qui contient le paquet du projet ;
`--call-tool` garde le dossier courant.

Les deux outils d'export prennent `idempotency_key` (et `expected_revision`) :
renvoyé sous la même clé, l'appel rend sa première réponse, et en
`async: true` le même `task_id` (PRD40). Un rush introuvable est refusé avant
FFmpeg, en `MediaMissing`, avec son chemin (« source media is missing:
<chemin> »), à l'export comme pour `read_frame`, les planches et les mesures
de couleur.
Il conserve la définition et la cadence rationnelle de la timeline ciblée,
sur sa durée entière. `--software` sélectionne x264 ; VideoToolbox est utilisé
par défaut. Les durées qui terminent entre deux images d'export sont refusées.
`--tools` publie le catalogue MCP et `--call-tool` l'invoque sans serveur HTTP,
avec le verrou du projet et les mêmes schémas et refus. Chaque outil y porte,
comme dans `tools/list`, des `annotations` MCP déduites de ce qu'il fait au
projet (lecture, session, analyse, édition journalisée, édition de projet,
écriture de fichier, hors journal, lecture et action hors machine) :
`readOnlyHint` pour une lecture, `destructiveHint: false` pour ce qu'`undo`
défait, `openWorldHint: false` sauf pour After Effects.
Seules les valeurs qui diffèrent des défauts du protocole sont écrites.
Aucune description d'outil ne dépasse 2 000 caractères : Claude Code coupe
au-delà de 2 048.

Chaque plan est placé à un numéro d'image entier : FFmpeg n'évalue plus aucune
division flottante pour la position ou la fenêtre d'activation d'un plan, qui
décalait auparavant un plan sur dix environ d'une image. `verify_export`
compare ensuite le fichier livré au document, image par image, et nomme les
plages qui divergent : décalage d'image (`offset`), image étrangère au plan
(`wrong_picture`), image dans un trou (`picture_in_gap`) ou image noire
(`black_frame`). La comparaison porte sur des vignettes de luminance
normalisées, donc un étalonnage n'est pas compté comme une différence ; les
fondus et les plans translucides sont déclarés invérifiables, et les images
d'un plan fixe trop semblables à leurs voisines sont comptées comme ambiguës
sans jamais être signalées comme décalées.

`exact` ne vaut `true` que si le fichier a exactement les images attendues,
que chacune a été comparée et qu'aucune ne diverge (PRD38). Une seule image
invérifiable — fondu, plan translucide ou dont l'opacité bouge, clip de
motion sur une image, rampe d'un fondu à la couleur, source trop courte — le
laisse à `false`, et le rapport dit à côté la part des images comparées
(`verified_fraction`, tronquée au millième, `1` seulement si toutes l'ont
été) et pourquoi (`not_exact_because`). `divergent_frames` à zéro dit
qu'aucune image comparée n'est fausse : un montage avec fondus répond
`exact: false` sans être faux. Les images ambiguës d'un plan fixe comptent
comme vérifiées : `exact: true` y veut dire « cohérent avec le document »,
et `ambiguous_frames` dit combien d'images ne sont pas prouvées à l'image
près.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  verify_export '{"timeline_id":"<timeline-id>","path":"sortie.mp4"}'
```

`export_timeline` avec `verify: true` fait la même comparaison dans le même
appel et rend son rapport sous `verification` (AGENT-2026-09 A7) ; l'export
tient quoi que dise la vérification. `timeline_stats` compte par ailleurs
les paires image/son liées et nomme, sous `sync.out_of_sync`, celles qui ne
lisent pas leur rush au même décalage, avec la dérive exacte (A5).

### Contrôler un montage contre une politique

`check_timeline` (CLI : `--check-timeline <projet> [--timeline <id>]
[--policy '<json>'] [--file <export>]`) juge une timeline sans la modifier,
contre une politique nommée (DONNEES-2026-10 OR4). Il réunit en un appel ce
que le pilote LangGraph faisait en Python avant son retrait : sept contrôles,
chacun `passed`, `failed` ou `not_run` avec sa raison. Un contrôle non joué
n'est jamais compté comme passé.

| Contrôle | Jugé sur | Échoue quand |
|---|---|---|
| `links` | le document | l'image et le son d'un même rush jouent ensemble sans être liés ; chaque paire est nommée avec sa dérive |
| `sync` | le document (`timeline_stats`) | une paire liée ne lit pas son rush au même décalage |
| `duration` | le document | la durée sort de `duration_min` ou `duration_max` ; non joué sans borne |
| `final_black` | le document, et le fichier s'il est donné | une image de la fin est noire ou en route vers le noir |
| `caption_contrast` | le fichier seulement | un carton est sous le contraste minimum, mesuré comme `check_caption_legibility` ; non joué sans fichier |
| `charter` | le document | une police ou une couleur sort de la charte de marque que nomme `kit` ; non joué sans `kit` (MOTION-BIBLIOTHEQUE-2026-10, DEC6) |
| `jump_cuts` | le document | une coupe à l'intérieur d'un même rush se voit telle quelle ; non joué sauf si `jump_cuts` vaut `forbidden` |

Deux contrôles de plus ne figurent au rapport que si la politique les
demande (PA2 et PA11, PARITE-2026-11), pour
qu'un rapport sans eux garde ses sept contrôles :

| Contrôle | Jugé sur | Échoue quand |
|---|---|---|
| `loudness` | le fichier seulement | la sonie intégrée du fichier s'écarte de plus d'une unité (1 LU) de `loudness_target`, sa crête vraie dépasse `true_peak_max` (−1 dBTP par défaut), ou le fichier est muet ; non joué sans fichier |
| `format` | le document | la séquence n'a pas la forme de `aspect_ratio` (`"9:16"`) |

La politique par défaut s'appelle `default` : noir final interdit, cartons à
3:1, jump cuts permis, aucune borne de durée. Toutes ses clés sont
facultatives : `name`, `duration_min`, `duration_max`, `final_black`
(`forbidden` ou `allowed`), `jump_cuts` (`forbidden` ou `allowed`),
`final_window` (en images entières, une seconde par défaut, arrondie à
l'image supérieure : 30 images à 29,97), `minimum_contrast` et `kit`, le
nom d'une charte de marque de la bibliothèque (`list_motion_templates`,
`kits`), `loudness_target` (LUFS entiers, de −40 à −5), `true_peak_max`
(dBTP entiers, de −20 à 0, avec `loudness_target` seulement) et
`aspect_ratio`. Une politique changée sans nom s'appelle `custom`, et le rapport
la répète en tête.

**Les jump cuts** (`jump_cuts`, depuis le 31 octobre 2026, hors lot :
L'auteur, sur le bench contre Descript). Une coupe à l'intérieur d'un même rush
est un jump cut quand le film la montre telle quelle. Il faut le même rush
des deux côtés, sans raccord continu, avec le même cadre fixe. Il faut aussi
que, sur l'image d'avant la coupe et sur celle de la coupe, l'image composée
soit le plan de cette piste et rien d'autre. Un plan de coupe qui couvre la
coupe, ou qui commence ou finit dessus, la cache. Un fondu enchaîné, un
`dip_to_color` ou un plan à opacité partielle mêlent deux images et la
cachent aussi. Un recadrage d'un côté la casse, cadre animé compris : c'est
le zoom que l'auteur pose dans Resolve pour casser un saut sans illustration.
Un clip de motion est dessiné par-dessus la coupe, pas à sa place : un
titre ou un logo ne cache pas un saut. Les coupes sont celles de
`review_cuts`, qui juge le saut sur les pixels ; ce contrôle les compte
toutes sur le document. Le rapport les liste toujours sous `jump_cuts`
(image, position, piste, les deux plans, le rush), quelle que soit la
politique. Le contrôle ne les juge que si la politique les interdit : un
jump cut peut être un choix (principe 12), et c'est la règle de
l'interview, pas celle d'un reel.

**La charte de marque** (`charter`, depuis le 24 octobre 2026). Le contrôle
lit, dans le document dont le fichier est exporté, la police et les
couleurs (texte, contour s'il a une largeur, ombre si elle a un décalage,
fond) de chaque style de légende qu'un carton visible utilise — un carton
sans style est jugé sur les valeurs d'un style neuf, comme l'export le
dessine —, et les polices et les couleurs de chaque clip de motion d'une
piste visible : styles, plages de texte, cycles de répétition, clés
d'animation, arrêts d'un dégradé, couleur d'un filtre ou d'un contour de
texte, valeurs de props de l'instance, et ce que le moteur prend quand
personne n'a rien écrit (un texte sans couleur est noir, sans famille
`system-ui` ; un tracé sans remplissage est rempli de noir). Un nœud que
l'instance cache ne compte pas. Ce sont les valeurs écrites, jamais les
mélanges qu'une animation traverse : un fondu du bleu au blanc de la
charte passe par des bleus qu'elle ne liste pas, et personne ne les a
choisis. Une couleur se compare par son RVB sur huit bits, l'opacité à
part ; une police par son nom, sans la casse. La charte autorise les
couleurs et les polices de ses props, de sa palette, de ses `fonts` et de
ses légendes (`MotionKitColors`, `MotionKitFonts`). Chaque valeur hors
charte est nommée où elle est écrite (`outside` : le `style_id`, le
réglage, le nombre de cartons et le premier ; ou le `clip_id`, la
composition, le nœud et la propriété), et `outside_values` la donne une
fois avec le nombre d'endroits. Les pixels du fichier ne sont pas lus : une
livraison HLG ou un étalonnage les déplacent, et `verify_export` prouve que
le fichier est la timeline. Une instance d'une autre composition se juge là
où elle est posée (`not_read`). Le rapport donne la charte et sa version ;
une charte que la bibliothèque n'a pas est refusée (`UnknownKit`). Le
document ne garde pas la charte, seulement les valeurs données (un projet
rouvert sans la bibliothèque rend le même film, et seul ce contrôle ne
tourne plus).

Le noir final se lit image par image sur la fin du film, composée comme
l'export la compose : de la piste visible la plus haute vers le bas, la
première qui montre un calque l'emporte. Une image sans rien est le noir du
fond, un `dip_to_color` noir à pleine force est noir, un voile noir en
route ou un plan dont l'opacité descend au-dessus de rien est un fondu ;
chaque plage nomme sa cause (`nothing_on_screen`, `dip_to_color` avec la
transition, `clip_opacity` avec le plan). Un voile d'une autre couleur
n'est pas du noir. Un clip de motion compte comme une image : le document ne
voit pas dans sa composition, et `motion_frames` dit combien d'images de la
fenêtre ne se jugent que sur le fichier. Avec `path` (`--file`), les
dernières images du fichier sont lues par FFmpeg, réduites en gris, et
jugées aux seuils par défaut de `blackdetect` (98 % des pixels sous le
dixième de l'échelle) : c'est ainsi que le bench mesurait « aucun fondu au
noir final ». Le teaser v7 y montre deux images noires que son document ne
voit pas, à l'intérieur de sa composition.

À côté des contrôles, et jugée par aucun, la liste `generated` nomme chaque
plan de la timeline dont le média a été fait par un service de génération
(GENERATIF-2026-10, DEC36) : `clip_id`, `track_id`, `media_id`,
`timeline_in`, `duration`, et ce que dit sa provenance (`service`, `model`,
`kind`, `created_at`). Elle est toujours là, vide quand rien n'est généré :
c'est d'elle que le compte rendu de l'agent tire sa ligne par plan généré.
Un plan généré n'est pas une faute, il ne fait échouer aucun contrôle.

Le CLI met `"ok"` à la valeur du verdict, comme `--check-setup`, et sort en
code 1 quand un contrôle échoue. Un chemin relatif donné au serveur se lit
dans le dossier du projet, comme pour `verify_export`. Sur la série PRJA
du 3 octobre, les quatre films passent liens, synchro et fin d'image, et
deux cartons de Camille sont sous 3:1 (médiane 2,87).

```sh
./build/cutmachine --check-timeline Projet.cutmachine-project/project.cutmachine.json \
  --policy '{"duration_max":{"value":60,"rate":1}}' --file sortie.mp4
./build/cutmachine --check-timeline Projet.cutmachine-project/project.cutmachine.json \
  --policy '{"kit":"marque"}' --file sortie.mp4
```

Pour un montage musical, `analyze_audio_beats` produit le cache d'attaques
d'un rush audio, puis `list_audio_beats` les lit par plage source exacte.
Le résultat borne le nombre d'attaques et fournit `next_source_in` pour
continuer la lecture. Le tempo et les forces sont des estimations ; les
outils de coupe existants placent ensuite les images choisies.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_audio_beats '{"media_id":"<ulid-rush-audio>"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  list_audio_beats '{"media_id":"<ulid-rush-audio>","max_onsets":64}'
```

`list_music_structure` lit, sur la grille de `list_tempo_grid` (mêmes
arguments), l'énergie de chaque temps en trois bandes : graves sous 150 Hz,
aigus au-dessus de 4 kHz, et spectre entier. Elle nomme les temps où les
graves montent (`rise`, la basse entre : un drop) ou tombent (`fall`, un
break) d'au moins 6 dB. Sur le morceau d'un reel à 124 BPM, la basse entre à
15,2 s (+20,3 dB), part à 79,1 s et revient à 84,9 s. Les bandes sont mesurées
pendant `analyze_audio_beats`, sur les mêmes spectres que les attaques, sans
second décodage ; un cache plus ancien demande de relancer cette analyse.
Mesuré, jamais jugé : couper ou non sur un drop reste à l'auteur.

`list_clip_tempo_beats` (mêmes arguments de grille que `list_tempo_grid`)
pose la grille d'un rush sur la timeline : pour chaque plan qui lit ce
média, l'image de séquence de chaque temps, et l'écart exact entre le temps
et le début de cette image, en échantillons et en millisecondes. L'arrondi à
l'image se fait à un seul endroit, si bien que l'outil répond l'image que le
moteur prendrait. À la jonction de deux plans adjacents du même média, il dit
si la grille traverse la coupe ; sinon, il nomme le décalage en échantillons,
et `beat_offset` le nombre de temps que la coupe a sautés. Il lit sans rien
modifier : c'est l'appelant qui pose les coupes.

L'outil `clone_timeline` remappe les IDs des objets et réutilise les sources ;
elle s'annule avec `--undo-project-op` et se rétablit avec `--redo-project-op`.
`create_timeline_variants` génère dates × formats en une opération projet :
les occurrences du texte sont comptées et tout le lot est refusé si un modèle
ne correspond plus. `export_timelines` valide toutes les timelines et leurs
destinations avant le premier rendu. Ces deux outils proposent un aperçu
par défaut ; `"preview":false` exécute le lot. Les identifiants et chemins
de sortie sont explicites.

L’export compose les pistes vidéo dans leur ordre, conserve les zones noires,
respecte les rotations et le ratio des sources, puis mixe les clips audio sans
normalisation automatique. Les pistes caption visibles sont composées avec
leur texte UTF-8 littéral, police, taille, couleur, position et opacité, sur
leurs plages exactes. Les sauts de ligne sont explicites et les lignes d'un
carton centrées ; les modèles MOGRT restent absents de ce rendu. Sans gestion
colorimétrique du projet, les effets `color.*` du moniteur (huit réglages,
les roues lift/gamma/gain/offset, les courbes, la rotation et les courbes de
teinte) sont appliqués
dans l'ordre de chaque clip vidéo avant la composition, via des LUT
temporaires partagées entre grades identiques. Depuis le 24 octobre 2026
(DEC28), les plans s'y composent en 4:4:4 10 bits, comme sous gestion
colorimétrique : un bord sur une ligne ou une colonne impaire tombe où
Record le dessine, et une livraison 10 bits garde ses 10 bits jusqu'au
format de sortie, atteint une seule fois en fin de chaîne. Une source BT.601
sans étiquette sort étiquetée BT.709, ses valeurs passant telles quelles :
c'est la règle depuis le 25 septembre. Il écrit d’abord un fichier
temporaire voisin et ne remplace la destination qu’après un encodage réussi.

Sous gestion colorimétrique, chaque plan reçoit avant la composition une seule
LUT 3D 65³ construite avec OpenColorIO depuis la configuration ACES Studio
intégrée : signal d'entrée → espace de travail, étalonnage là où le moniteur
l'applique (ACEScct), puis retour en ACEScg linéaire. Les plans se composent
en flottant dans cet espace linéaire, et la transformation d'affichage ne
s'applique qu'une fois, après la dernière couche : un fondu ou un plan
translucide se mélange en lumière, comme au moniteur. Un plan étalonné n'est
donc plus refusé sous gestion colorimétrique, et il n'est plus nécessaire de
la couper pour exporter un étalonnage. Pour un rush Sony, la chaîne est
S-Log3/S-Gamut3.Cine → ACEScct → ACEScg → Rec.2020 HLG. Le flux HEVC Main10
est marqué explicitement BT.2020 non constant, ARIB STD-B67 et plage légale.
Une image que son fichier n'étiquette pas (un FX30 n'écrit aucune matrice
dans son MP4) est lue comme au moniteur : matrice BT.709, et pleine échelle
sous une entrée S-Log3. swscale, laissé à lui-même, la lisait en BT.601 :
0,011 à 0,019 de vert en moins sur un visage que dans Record (mesuré le
4 octobre 2026).
Les planches (`contact_sheet`, `cut_sheet`) et `read_frame` passent par la
même vue couleur que le fichier, mais montrent la source d'un plan : ni son
étalonnage, ni la composition des pistes (sauf avec `graded: true`, qui
montre l'étalonnage et le look, toujours sans la composition). Pour juger
l'image livrée, relire l'export (`verify_export`).

Un étalonnage réglé sans gestion colorimétrique ne se transpose pas tel quel :
sur un montage réel, des réglages faits pour compenser l'absence de conversion
débordent une fois celle-ci activée. Ce sont les réglages qu'il faut reprendre.

## Légendes

`generate_captions` (MCP) construit les cartons depuis les mots du montage
(frontières alignées quand elles existent, mots non entendus écartés) et les
pose sur une nouvelle piste de légendes en sync-lock, en une seule opération
réversible. Chaque carton tient sur une ligne : une unité syntaxique autour de
18 caractères par seconde, coupée aux fins de phrase et aux longues pauses, de
`max_characters` au plus et jamais plus large que `max_line_width` **telle que
l'export la dessine**, contour compris. Par défaut, le nombre de caractères
suit le format de la timeline : 32 en paysage (plus large que haute), 20
sinon, en vertical et, faute de film livré pour en décider, en 1:1 et en 4:5.
Ce sont les réglages des films livrés : une ligne de 32 caractères en 16:9, 20
par carton en vertical (DEC4,
`REGLES-2026-10`, étendu au paysage le 4
novembre 2026). Un `max_characters` donné garde sa valeur. La largeur vaut par
défaut 60 % de celle de l'image ; le SRT de `--export-srt` garde 42
caractères. La largeur est mesurée
par le moteur qui dessine les cartons (voir plus bas), pas estimée par un
nombre de caractères. Un bord de carton à moins de `snap_frames` images (12
par défaut) d'une coupe visible s'y recale, sauf si le carton y perdrait sa
lisibilité ou si le bord passerait à l'intérieur d'un de ses mots : un carton
s'ouvre au plus tard au début de son premier mot et se ferme au plus tôt à la
fin de son dernier (DEC18, `BENCH-2026-09`).
`caption_gaps` liste les trous entre deux cartons pendant lesquels un mot est
dit, avec sa raison : `unheard` (un mot qu'aucune lecture du montage
n'entend) ou `aligned_in_removed_range` (un mot entendu au raccord, que
l'aligneur a placé dans la matière coupée).
Chaque carton est rendu avec ses caractères, son débit approximatif, sa
largeur et sa distance à la coupe la plus proche ; `preview:true` mesure sans
éditer. Le texte ne suit pas une coupe de lui-même : après avoir retiré des
mots, on relance la génération.

**Une piste dans une autre langue** (PA5,
`PARITE-2026-11`). `translate_captions`
pose à côté d'une piste de légendes sa traduction : l'agent donne un texte
par carton (`cartons`, des `{clip_id, text}`), le moteur les pose sur une
nouvelle piste aux mêmes temps et dans les mêmes styles, en une opération.
Chaque carton revient avec ses caractères, son débit
(`characters_per_second`), sa largeur telle que l'export la dessine et
`wraps` quand elle dépasse la ligne de la livraison (`max_line_width`, neuf
dixièmes de l'image) : le moteur mesure, il ne raccourcit rien. Un carton
sans texte est refusé par son nom. `preview:true` mesure sans éditer.

La nouvelle piste est **masquée** par défaut. L'export dessine toutes les
pistes de légendes visibles : deux langues laissées visibles sortiraient
l'une sur l'autre. Une version se livre en montrant sa piste et en masquant
les autres (`set_track_output`). La langue n'est pas dans le document, qui
n'a pas de champ pour elle : elle est gardée au journal avec l'édition
(`list_motifs`, règle `translate_captions`). Dans un processus sans le
rendu des légendes (un test, la version sans fenêtre), les largeurs ne sont
pas mesurées et la réponse le dit (`widths_measured: false`).

**Où un carton s'arrête** (RM3,
`REGLES-2026-10`). La règle écrite à la main
classe les frontières possibles : coût selon les mots de part et d'autre, la
ponctuation et la pause. Un modèle appris peut la corriger, à l'intérieur des
mêmes bornes : caractères, durée, fins de phrase, longues pauses.
`train_caption_breaks` l'entraîne sur des pistes de sous-titres livrées (un
fichier par corpus, une piste par ligne, les temps des mots estimés ou
donnés) et le mesure contre la règle sur des groupes exclus de son
entraînement et sur un corpus d'évaluation qu'il n'entraîne jamais
(`evaluation`). Avec `install:true`, qui exige cette évaluation, il
l'installe s'il reproduit davantage de cartons livrés, avec un meilleur F1
des frontières, des deux côtés, et s'il respecte les deux choix fermes de
l'auteur (l'article reste avec son nom, la conjonction ouvre le carton suivant).
La forge ne mesure que la découpe : avant d'installer, on rejoue
`generate_captions` sur les films du critère. Mesuré le 15 octobre 2026, aucun
modèle ne bat la règle sur la série PRJA, et aucun n'est installé. Le modèle installé vit dans le dossier
de données du moteur (`models/caption_breaks.json`), jamais dans le projet ni
dans le dépôt ; `CUTMACHINE_MODELS` le déplace, `off` le coupe. Dès qu'il est
installé, `generate_captions` coupe avec lui. Sa réponse `break_model` dit ce
qui a coupé : `source` vaut `rule` ou `model`, avec l'empreinte des poids
dans le second cas. `break_model:"rule"` demande la règle seule. Un fichier
installé illisible est signalé (`model_error`), et la règle décide. Le
document ne garde que les cartons : rouvrir le projet ailleurs, sans le
modèle, redonne le même montage.

**Des cartons découpés par l'appelant.** `cartons` donne le regroupement à
la place de la découpe : des textes dont les mots sont, dans l'ordre, ceux du
montage (les spans de `get_timeline_transcript`, à la casse et aux apostrophes
typographiques près). Le moteur pose les temps comme pour sa propre découpe,
les majuscules, la largeur, le calage sur les coupes, et ne fond jamais un
carton court dans son voisin. Il refuse, en nommant le carton et l'index du
mot, un mot qui diffère ou qui manque, une fin de phrase ou une pause de
0,7 s dans un carton, et un carton de plusieurs mots au-delà de
`max_characters`. `break_model.source` vaut alors `cartons`. Un agent qui
découpe à partir des mots, sans voir la proposition du moteur, a été préféré
par l'auteur à l'aveugle (REGLES-2026-10, RM3).

Un style de légende (`add_caption_style`, qui rend son `style_id`) accepte
désormais un contour (`outline_width`, `outline_color`), une ombre
(`shadow_offset`, `shadow_color`), un fond (`background_color`,
`background_padding`) et une position verticale exacte (`vertical_position`,
fraction de la hauteur mesurée depuis le haut, qui place le centre du bloc de
texte au-dessus de l'interface des applications). Ces champs ne sont écrits
dans le projet que lorsqu'ils sont réglés : un style existant garde ses
octets. `update_caption_style` modifie un style en une opération réversible,
et tous les cartons qui l'utilisent suivent. Les bornes sont celles de
`Document.h` (`kCaptionStyleMaxOutlineWidth` et `kCaptionStyleMaxShadowOffset`,
256 px, `kCaptionStyleMaxBackgroundPadding`, 512 px), que la validation du
document applique (`ValidateCaptionStyle`).

Depuis le 4 novembre 2026, un style nomme aussi sa graisse (`font_weight`,
de 1 à 1000 comme en CSS ; 400 par défaut, la regular, 500 la medium). La
face est choisie comme pour un texte de motion (MOTION-WEB-2026-10 MW7) : la
face de la famille que CSS prendrait, ou l'axe `wght` d'une police variable.
Avant, un carton était toujours dessiné en regular, alors qu'un style maison
réglé dans Resolve peut être en medium. Le champ n'est écrit que s'il ne vaut
pas 400 : un projet existant garde ses octets, et un binaire plus ancien relit
le projet en ignorant la clé, donc en regular. Une charte de marque peut le
régler (`captions.font_weight`). L'Inspecteur ne le montre pas encore.

Depuis le 12 novembre 2026 (PARITE-2026-11 PA4), un style peut animer les
mots d'un carton (`word_animation`) :

- `highlight` dessine le mot en train d'être dit dans une autre couleur
  (`highlight_color`, `#ffe600` par défaut). Le mot reste allumé jusqu'au
  suivant, et le dernier jusqu'à la fin du carton.
- `reveal` dessine chaque mot à partir du moment où il est dit. La ligne
  est placée comme si elle était pleine : elle ne bouge pas en se
  remplissant. Un fond (`background_color`) est dessiné entier dès
  l'ouverture du carton.
- `none` (le défaut) dessine le carton entier, comme avant.

Seul un carton dont les mots sont horodatés s'anime. `generate_captions`
écrit le temps de chaque mot sur chaque carton (`caption_word_times` dans le
fichier, `timed_words` dans `describe` et dans sa réponse). Un carton importé
d'un SRT, tapé à la main ou traduit (`translate_captions`) n'en a pas : il
est dessiné entier, quel que soit le style.

Les temps sont comptés sur l'horloge du carton, qui part de son début. Ils
suivent le montage : un carton déplacé, rogné, roulé contre son voisin ou
coupé en deux dit chaque mot à l'instant où il était dit, et une annulation
rend les mêmes octets. `set_clip_caption` garde les temps quand le nouveau
texte compte autant de mots (une faute corrigée, un style changé) et les
retire sinon ; il faut alors relancer `generate_captions` pour que le carton
s'anime de nouveau.

À l'export, un carton animé est une image par mot au lieu d'une : le mot
change sur l'image où il commence, au moniteur comme dans le fichier. Ce qui
n'est pas fait :

- rien n'a été vu à l'écran ni sur un film ; les tests lisent les pixels du
  rendu et d'un fichier exporté ;
- l'Inspecteur ne montre pas le réglage ;
- `check_caption_legibility` mesure la couleur du texte, pas celle du mot
  allumé ;
- un SRT, un VTT et le renvoi vers Resolve ne portent pas l'animation ;
- il n'y a pas d'animation continue (un mot qui grossit ou qui glisse) : un
  mot est allumé ou ne l'est pas ;
- une écriture de droite à gauche n'est pas traitée.

Depuis le 24 octobre 2026 (DEC6), `add_caption_style {kit}` part de la
charte de marque nommée (`brand-kit.v1` avec `captions`) : ses réglages,
puis ceux de son format de légende de la taille exacte de la timeline. Les
réglages donnés à côté l'emportent et sont rendus sous `kit.overridden` ;
`kit.set` dit ce que la charte a réglé et `kit.format` le format choisi.
Refus : `UnknownKit`, `KitMismatch` pour une charte sans légendes,
`UnknownFormat` quand ses formats de légende n'ont aucune mise en page de
la taille de la timeline (ses tailles sont des pixels de ces cadres). Le
style garde les valeurs, pas la charte.

Dans la fenêtre, depuis le 19 octobre 2026 (F2.3), la section « Style de
légende » de l'Inspecteur règle tout le style du carton sélectionné, sous
son texte. On y trouve la police, la taille, la couleur, le contour, l'ombre
portée, le fond et la position (trois préréglages ou une hauteur choisie, en
pourcentage exact du cadre). Chaque geste fait une `UpdateCaptionStyleOperation`,
et un glissé du curseur de hauteur n'en fait qu'une, au relâché. C'est là
que Resolve règle le Track Style d'une piste de sous-titres. L'onglet
Légendes du panneau Média garde la liste des styles, leur création, leur
suppression et l'habillage des cartons. Rien de cela n'a encore été regardé
à l'écran.

`check_caption_legibility` mesure la lisibilité dans le fichier exporté. Sur
l'image du milieu de chaque carton, il retrouve les lettres avec le dessin de
l'export et compare la couleur du texte à l'anneau de pixels qui les entoure
(le contour s'il y en a un, l'image sinon) : rapport de contraste WCAG médian
et dixième centile pour le pire passage, 3:1 minimum par défaut
(`minimum_contrast`). Un texte blanc sur une chemise blanche mesure environ
2:1 ; un contour le fait passer au-dessus de 10:1. Sur un montage réel, il a
retrouvé seul 11 cartons sur 58 sous 3:1. Un `path` relatif donné au serveur
se lit dans le dossier du projet, comme pour `verify_export` et
`check_timeline` (DEC19, `PRODUIT-2026-10b`) ;
`--call-tool` garde le dossier courant.

Depuis le 9 octobre 2026 (LEGENDES-2026-10),
un carton est dessiné une seule fois, par le moteur, avec Core Text, et non
plus par le filtre `drawtext` de FFmpeg. L'export pose ce dessin sur l'image,
le moniteur Record montre le même à sa taille, et la mesure des lignes comme
`check_caption_legibility` lisent la même mise en page. La signification d'un
style n'a pas changé :

- le contour est à l'extérieur des lettres ;
- l'ombre est décalée vers le bas à droite et porte le contour ;
- le fond entoure le texte de sa marge ;
- `position` garde un vingtième de la hauteur de marge ;
- `vertical_position` place le centre du bloc ;
- un carton qui sortirait de l'image y est ramené ;
- les couleurs sont écrites telles quelles dans le signal livré.

L'export n'a donc plus besoin que son FFmpeg ait `drawtext`. Hors des
cartons, l'image a les mêmes octets avec ou sans légendes.

Une famille est cherchée comme pour la motion : polices installées, dossiers
enregistrés, et celles qu'Adobe Fonts active tant que Creative Cloud tourne.
Un style de légende n'a pas de graisse : le carton prend la face à 400 de
la famille, et dans une police variable l'instance à 400 de son axe `wght`
(MOTION-WEB-2026-10 MW7). Quand la famille est introuvable, le carton est dessiné en Helvetica Neue, au
moniteur comme dans le fichier. L'export ne le refuse pas (décision du
8 octobre 2026) : sa réponse porte `caption_fonts_substituted`, chaque
famille avec celle qui la remplace
(`[{"family":"Poppins","exported_as":"Helvetica Neue"}]`, absent quand rien
ne manque), au MCP, au CLI et dans l'aperçu d'`export_timelines`. La fenêtre
avertit avant de lancer l'export (« Exporter quand même » ou « Annuler »).
`check_caption_legibility` (`fonts.missing`) et `list_fonts`
(`captions.missing`) le disent avant.

Record et le fichier ont le même dessin, mais **le moniteur n'a pas encore
été revu à l'écran depuis ce changement**.

## Livrer à une sonie

> PA2, PARITE-2026-11, 11 novembre 2026.
> Mesuré sur des sons de synthèse ; aucun film livré ainsi, rien d'écouté.

Par défaut, l'export mixe les sons sans normalisation : le film sort au
niveau que ses rushes et sa musique additionnent. `export_timeline` et
`export_timelines` prennent `loudness_target`, en LUFS entiers de −40 à −5
(−14 pour le web, −23 pour la diffusion) ; en ligne de commande,
`--export … --loudness -14`.

La cible appartient à la livraison, comme le codec : elle n'est pas dans le
document, et le moniteur, `transcribe_timeline` et `analyze_timeline_audio`
continuent d'entendre le mix tel qu'il est monté. L'export procède en trois
temps :

1. il rend le film comme d'habitude, le son gardé sans perte (ALAC 24 bits) ;
2. il mesure ce son comme un sonomètre de diffusion (EBU R 128, le filtre
   `ebur128` de FFmpeg, celui d'`analyze_timeline_audio`) ;
3. il pose un seul gain vers la cible, repasse le limiteur du mix (crête à
   −3,5 dBFS), encode le son une seule fois en AAC et copie l'image telle
   quelle.

La mesure porte sur le fichier rendu, pas sur une prédiction : le son d'un
clip de motion et un film rendu par segments y sont. La réponse porte
`loudness` : la cible, la sonie et la crête du mix avant le gain
(`mix_lufs`, `mix_true_peak_dbfs`), le gain (`gain_db`), puis la sonie et la
crête du fichier livré, mesurées de nouveau (`delivered_lufs`,
`delivered_true_peak_dbfs`). Un gain vers le haut peut faire travailler le
limiteur : le fichier sort alors un peu sous la cible, et `delivered_lufs`
le dit. Un film muet est livré tel quel (`silent: true`).

`check_timeline` juge ensuite le fichier : voir `loudness_target` dans la
politique, plus haut.

## Mesurer le son monté

`analyze_timeline_audio` (MCP) mesure le son d'une timeline tel qu'il joue,
assemblé comme le font `transcribe_timeline` et l'export, sans rien exporter :
loudness intégrée, true peak et plage de loudness (EBU R128), niveau et crête
des premières secondes avec l'air avant la première voix, et chaque creux. Un
creux est une portion plus proche du bruit de fond du rush qui la joue que de
son niveau de parole (`dip_position_percent`, 50 % par défaut), ou un trou où
aucun son ne joue. Le plancher et le niveau de parole viennent de
`analyze_speech_onset` ; un rush sans cette analyse est listé dans
`sources_without_floor` et ses passages ne sont pas jugés. Aucun seuil absolu
en dBFS n'est employé : sur le montage mesuré, les planchers allaient de −36
à −45 dBFS sous une parole à −11, et une salle à −25 dBFS cache tout creux
sous −30.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  generate_captions '{"timeline_id":"<timeline-id>","preview":true}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  check_caption_legibility '{"timeline_id":"<timeline-id>","path":"sortie.mp4"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_timeline_audio '{"timeline_id":"<timeline-id>"}'
```
