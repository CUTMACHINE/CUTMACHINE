# Import depuis DaVinci Resolve

> Relu contre le code le 3 octobre 2026. Ce qui se passe dans Resolve (le
> menu Scripts, la limite de la version gratuite) et les mesures citées ne
> sont pas vérifiés par lecture.

Un chutier Resolve n'est pas un fichier : il vit dans la base du projet, et un
`.drp` est une archive opaque. Il n'y a donc rien à analyser — la seule entrée
honnête est de faire parler Resolve.

## Depuis Resolve, en un clic

```sh
tools/resolve-plugin/install.sh
```

Puis dans Resolve : `Workspace → Scripts → Utility → CUTMACHINE`. La fenêtre
importe les chutiers du projet ouvert et sait interroger le moteur
(`--describe`) pour montrer ce que verra l'agent. Voir
[`tools/resolve-plugin/README.md`](../../tools/resolve-plugin/README.md).

C'est le chemin recommandé : le script tourne **dans** Resolve, donc il ne
réclame pas le scripting externe réservé à Resolve Studio, et n'a besoin
d'aucun interpréteur Python.

## Depuis un terminal

```sh
# 1. Resolve Studio est lancé, le projet ouvert. Le pont lit le Media Pool.
python3 -m sidecar.resolve_bridge -o manifest.json

# 2. CUTMACHINE reproduit l'arborescence et ingère les rushes.
./build/cutmachine --import-resolve \
  ./Film.cutmachine-project/project.cutmachine.json ./manifest.json
```

Cette voie-là exige **DaVinci Resolve Studio** : la version gratuite n'exécute
des scripts que depuis l'application. En échange, elle s'automatise.

Les deux producteurs écrivent le même schéma de manifeste et passent par le
même importeur — vérifié sur un projet de 408 rushes, où les deux manifestes
sont équivalents. Ni l'un ni l'autre ne modifie le projet Resolve : ils lisent
le Media Pool.

Le manifeste ne transporte **que l'identité et l'organisation** — chemin, nom,
chutier. Aucune métadonnée technique : Resolve renvoie sa cadence en flottant,
et un flottant ne devient jamais un `RationalTime`. C'est la sonde FFmpeg de
`--ingest` qui fait autorité sur la cadence et la durée, si bien qu'un rush
importé depuis Resolve est identique au même rush ingéré depuis un dossier,
niveau sonore compris : depuis le 24 octobre 2026 (DEC32), l'import le mesure
comme `--ingest`, et un plan muet se reconnaît avant Whisper.

Les chutiers sont créés par `AddBinOperation`, les nouveaux rushes entrent
chacun dans son chutier par une seule `IngestMediaOperation`, et les rushes
déjà connus sont reclassés par `SetMediaBinOperation` : l'import complet
s'annule par `Cmd+Z` ou l'outil `undo`, étape par étape. Réimporter le même manifeste ne duplique rien — les chutiers sont
appariés par nom et par parent, les rushes par chemin absolu résolu. Un Media
Pool qui grossit se réimporte donc en n'ajoutant que le nouveau. Attention :
un rush déjà présent est reclassé dans son chutier Resolve, donc un
réimport écrase le classement fait à la main dans CUTMACHINE.

Les entrées sans fichier — timelines, clips composés, générateurs — sont
écartées par le pont et rapportées dans son `skipped`. Côté import, tout rush
refusé est **nommé avec son motif** dans le tableau `errors`, comme pour
`--ingest` : sur un Media Pool de plusieurs centaines de rushes, un simple
compteur obligerait à comparer le chutier à la main. Un rush déjà présent dans
la médiathèque compte dans `skipped` sans figurer dans `errors` — c'est le
résultat attendu d'un réimport, pas un échec.

```json
{"ok":true,"added":406,"skipped":2,"bins_created":22,"bins_reused":0,
 "detail":"<nom du projet Resolve>","errors":[]}
```

Un média hors ligne reste refusé : le volume doit être monté au moment de
l'import. Les fichiers audio seuls — voix off, musique, ambiance, son stock —
sont en revanche ingérés normalement avec `has_video: false`. Leur cadence
exacte est celle des échantillons audio ; les commandes d'image (`read_frame`,
analyse qualité, vignette et proxy vidéo) les refusent explicitement.

Ce qui reste hors périmètre de cet import : les mots-clés, drapeaux et
commentaires de clip, et le timecode de départ — `LibraryMedia` n'a pas de
champ pour l'accueillir. Les timelines Resolve elles-mêmes se lisent à part,
par `--import-resolve-timeline` (MCP : `import_resolve_timeline`), et le
trajet retour vers Resolve passe par `send_to_resolve` (plus bas) ou par
`--export-resolve-xml` (MCP : `export_resolve_xml`).

L'export XML exige une destination nouvelle : un fichier ou un lien déjà
présent est refusé avec `OutputExists`, y compris le projet et les rushes.
Choisir un nouveau nom pour chaque livraison. Le XML complet est préparé
à côté de la destination, puis publié sans remplacement (FI6).

## Renvoyer un montage dans Resolve, entier

> Écrit le 13 novembre 2026 (PARITE-2026-11, PA16). Chaque appel d'API sur
> lequel repose le renvoi a été mesuré dans Resolve Studio 21.1.0.17 le même
> jour, dans un projet jetable, sur des rushes portant leur numéro d'image
> incrusté, puis un montage de test renvoyé, rendu par Resolve et comparé à
> l'export du moteur image par image (PA16). Le
> renvoi d'un vrai film n'a pas encore été fait.

Un seul appel envoie la timeline active dans le projet ouvert dans Resolve,
comme une timeline neuve, puis la relit :

```sh
./build/cutmachine --send-resolve ./Film.cutmachine-project/project.cutmachine.json \
  --name 'Film v3'
```

MCP : `send_to_resolve` (`name`, `grade`, `resolve_node`, `dry_run`) ; dans
la fenêtre, **Fichier ▸ Envoyer dans Resolve**, qui envoie la timeline à
l'écran sans demander de nom et dit le résultat dans la ligne d'état (une
feuille quand quelque chose n'est pas parti ou que l'envoi est refusé). Il
faut Resolve **Studio** 21.1 ou plus récent, ouvert sur le projet, avec les
rushes du montage dans son Media Pool (`--import-resolve` les y relie dans
l'autre sens). Le moteur écrit le descripteur (schéma
`cutmachine.resolve-timeline.v3`) dans
`~/Library/Application Support/CUTMACHINE/resolve-send/` (ou
`CUTMACHINE_RESOLVE_SEND_DIR`), puis lance
`sidecar/resolve_bridge.py --send` et rend son rapport. Le pont se trouve par
`CUTMACHINE_RESOLVE_BRIDGE`, puis à côté de l'exécutable, puis dans l'arbre
du build ; son Python par `CUTMACHINE_RESOLVE_PYTHON`, sinon
`/usr/bin/python3`.

**Ce qui passe**, par l'API de Resolve 21.1 :

| Dans CUTMACHINE | Dans Resolve |
|---|---|
| plan vidéo | image seule (`mediaType` 1), sur sa piste V |
| plan son | son seul (`mediaType` 2), sur sa piste A ; une piste qui ne porte que le son des rushes est **mono**, une musique reste stéréo. Une piste mono est centrée à −3 dB dans le bus stéréo de Resolve : la voix y sort 2,5 à 2,8 dB plus bas que dans le moteur (mesuré) |
| image et son d'une même séparation | reliés (`SetClipsLinked`) |
| vitesse constante | `SetSpeed` : le plan est posé sur sa longueur à vitesse 1, puis ralenti ou accéléré ; position et durée ne bougent pas |
| arrêt sur image | `SetSpeed` à 0, qui fige la **dernière** image de la plage posée |
| fondu enchaîné, fondu croisé du son | `AddTransition` (« Cross Dissolve », « Cross Fade +3 dB ») sur la fin du plan sortant, centré, à gauche ou à droite de la coupe |
| ouverture ou fermeture au noir, creux au noir | fondus d'opacité des plans (`SetFades`), sans poignée, comme le voile du moteur |
| cadrage | Zoom, Pan, Tilt, calculés pour « Scale entire image to fit » **et** pour « Scale full frame with crop » ; le pont prend celui du projet |
| opacité, plan désactivé | `Opacity`, `SetClipEnabled` |
| gain et fondus d'un plan son | `AudioVolume`, `SetFades` |
| piste masquée, coupée, ou hors du solo | piste désactivée |
| marqueurs | `AddMarker`, couleur la plus proche parmi les seize de Resolve ; deux marqueurs sur la même image n'en font qu'un, leurs noms joints |
| cartons de la première piste de légendes | piste de sous-titres, posée **en premier** |
| étalonnage, avec `grade: "cdl"` | `SetCDL` sur le nœud `resolve_node`, la CDL qu'`export_cdl` replie : juste seulement si ce nœud étalonne les valeurs que le moteur étalonne |

**Ce qui ne passe pas** n'est pas refusé : c'est nommé dans `not_sent`, plan
par plan, et le reste du montage part. Les images clés (aucune API n'en
pose : le plan garde le cadrage et l'opacité de sa première image ; un
cadrage animé passe par `export_resolve_xml`), un recadrage, un flou, un
masque, une rampe de vitesse (envoyée à sa vitesse moyenne), un plan de
motion (sa place reste vide : `render_motion_video` en fait une plaque à
poser), une seconde piste de légendes, le style des cartons (il se greffe
par le `.drt`, voir le skill), le look de la séquence, l'étalonnage sauf
demande, un creux par une autre couleur que le noir.

**Ce qui est refusé avant de rien créer dans Resolve** : un rush absent du
Media Pool, un projet à une autre cadence, un montage cadré dans un projet
qui ni n'ajuste ni ne remplit, une borne qui ne tombe pas sur une image
entière, un arrêt sur image trop près du début de son rush, un ralenti trop
près de sa fin (tous deux liraient hors du rush à vitesse 1). Une timeline
d'une autre taille que le projet prend ses propres réglages.

**La relecture.** Le pont relit chaque plan (position, durée, vitesse,
activation), chaque transition (ses bornes), les marqueurs, les
sous-titres, la longueur de la timeline. Un écart ne défait rien : il est
listé dans `mismatches` et `verified` est faux. `GetSourceStartFrame` n'est
pas relu : il se trompe d'une image sur la moitié des plans (mesuré ; l'image
affichée, elle, est juste).

**Le nom.** Sans `name`, la timeline prend le nom de la séquence, ou le
premier « nom 2 », « nom 3 »… que le projet Resolve n'a pas encore : un
second envoi du même montage ne bute pas sur le premier. Un `name` donné et
déjà pris est refusé, comme un nom qui contient « : ». Une fenêtre ouverte dans Resolve
le fait taire : le renvoi abandonne au bout de dix minutes
(`ResolveTimeout`), la timeline commencée reste dans Resolve.

**Le temps.** Le rapport porte `timings_ms`, étape par étape, et
`elapsed_ms`, le tout. Mesuré le 13 novembre sur un montage de la taille
d'une interview (220 plans, 100 sous-titres, 10 marqueurs) : 1,7 à 1,9 s,
dont 0,8 s pour l'`AppendToTimeline` de Resolve, 0,25 à 0,34 s pour la
timeline et ses sous-titres, 0,23 à 0,26 s pour les réglages des plans,
0,17 s pour les marqueurs, 0,10 à 0,14 s pour la relecture, 7 ms pour la
connexion.

`--export-resolve-timeline` écrit le même descripteur sans rien envoyer ;
`dry_run` aussi, en rendant ce qui partirait et ce qui resterait.

## Relire une timeline, et ce qui y a été étalonné

> Écrit le 24 octobre 2026 (GRADE-RESOLVE-2026-10, DEC15). La lecture a été
> vérifiée sur des copies de cinq bases du Mac de l'auteur, jamais avec Resolve
> ouvert sur elles.

Une timeline Resolve se relit dans une timeline vide du projet ouvert, à
partir du manifeste que le pont écrit (`resolve_bridge.py --read-timeline
NOM`, Resolve Studio ouvert sur le projet) :

```sh
./build/cutmachine --import-resolve-timeline \
  ./Film.cutmachine-project/project.cutmachine.json ./timeline.json \
  --resolve-database '<…>/Resolve Projects/Users/guest/Projects/Film/Project.db'
```

MCP : `import_resolve_timeline` avec `manifest_path`, `timeline_id` et
`resolve_database`, chemins absolus. Sans `resolve_database`, la réponse est
celle d'avant : plans, pistes, fondus, plans ralentis, `unpublished`.

**L'étalonnage.** Avec la base du projet (`Project.db`, dans la
bibliothèque de projets de Resolve), la réponse porte aussi `grades` : ce qui
a été étalonné dans Resolve, plan par plan. Depuis le 25 octobre 2026
(GR16), un étalonnage que l'étape `color.resolve_primaries` rend entièrement
est aussi écrit sur son plan, dans le journal de la relecture (plus bas) ;
les autres restent une lecture, et la réponse dit pourquoi. Pour ceux-là,
l'agent ramène l'image avec l'étalonnage du moteur : il sait ce qui a été
fait, et ce que la lecture n'a pas su lire.

- **La base n'est jamais ouverte.** Le moteur la copie (avec son `-wal` et
  son `-shm` quand ils existent) dans un dossier temporaire, lit la copie,
  puis l'efface. Resolve peut tenir la base pendant ce temps ; si une
  écriture tombe pendant la copie, la copie peut être illisible : enregistrer
  le projet dans Resolve, puis relancer.
- **L'appariement.** Chaque plan vidéo de l'import est cherché dans la
  timeline de la base qui porte le nom du manifeste : même piste (V1 est V1),
  même place (l'origine de la timeline plus son début, sur autant d'images),
  même fichier. Un plan sans correspondant est nommé dans `unmatched` avec sa
  raison (souvent une base plus ancienne que la timeline), et le montage est
  relu quand même. Quand la base porte plusieurs timelines de ce nom, c'est
  celle qui porte tout le montage ; si aucune ou plusieurs le portent, l'appel
  est refusé et rien n'est écrit (renommer la timeline dans Resolve).
- **Ce qui est lu.** Pour chaque plan étalonné (`clips`, avec le `clip_id`
  qu'il reçoit ici) et pour la timeline elle-même (`timeline_grade`, `null`
  quand elle n'en a pas) : la version active, ses nœuds dans l'ordre où ils
  s'appliquent (`serial`), et dans chaque nœud les primaires réglées --
  `lift`, `gamma`, `gain` (r, g, b, master), `offset` (r, g, b), `contrast`,
  `pivot`, `saturation`, `hue`, `temperature`, `tint`, `midtone_detail`,
  `color_boost`, `shadows`, `highlights`, `lum_mix`, les roues Log (`log.*`).
  Un plan sans correction est seulement compté (`ungraded`).
- **Les valeurs sont celles que Resolve stocke**, non converties : ni celles
  de son panneau, ni celles des roues du moteur. Ce que chacune fait, sur
  les valeurs où le nœud travaille (DaVinci Intermediate dans un projet en
  gestion couleur v2), a été mesuré dans Resolve 21.1 le 25 octobre (GR15),
  à 0,05 code 10 bits près :
  - `gain` multiplie, et `lift` pose le noir en tenant le blanc. Ensemble,
    la sortie vaut a + b·x, avec a = lift·gain / (1 − lift + lift·gain) et
    b = gain·(1 − lift) / (1 − lift + lift·gain).
  - `offset` ajoute 0,1832 fois sa valeur, après le gain.
  - `gamma` élève à la puissance 1 / (1 + γ) si γ ≥ 0, et 1 − γ sinon, après
    l'offset.
  - Les `master` agissent sur la luma (poids de Rec.709) au poids du
    `lum_mix` : 1, ou absent, c'est le 100 du panneau. Au même poids, une
    couleur qui déplace un canal garde la luma de l'image.
  - `saturation` est un facteur autour de la luma, aux poids
    0,21 / 0,70 / 0,09 (50 au panneau, l'unité, vaut 1).
  - `temperature` et `tint` sont une matrice 3×3 qui garde la luma d'un gris.
  - `contrast` (2 au plus) s'applique après le gamma, autour de `pivot`
    (0,435 quand il est absent). Sa courbe dépend de deux réglages du
    projet : la courbe en S et les outils « color space aware ».
    - Sous 1, ou sans la courbe en S, c'est une droite autour du pivot :
      pente c sous 1, 1 / (2 − c) au-dessus.
    - Au-dessus de 1, avec la courbe en S (le réglage par défaut), les
      bouts restent à 0 et à 1, et aucune formule ne la donne.
    - Avec les outils allumés, le réglage de PRJH, le contraste agit sur
      le code HLG de la lumière divisée par 12, et il a sa propre part Y.

  Au panneau, le lift s'affiche à la moitié de sa valeur stockée, le gamma
  au quart, et l'offset à 25 plus 25 fois sa valeur (lu par l'auteur le
  25 octobre). La loi complète, et ce qui ne passe pas dans les roues du
  moteur, sont dans
  GRADE-RESOLVE-2026-10 (GR15).
- **Ce qui n'est pas lu est nommé** dans `unread`, nœud par nœud (`node` 0
  pour l'étalonnage entier) : un greffon OFX (avec son identifiant), une
  fenêtre, un qualificateur, une clé, des courbes, une LUT (avec son chemin),
  un Shot Match, un réglage animé ou suivi, un correcteur désactivé, un nœud
  composé, un câblage qui n'est pas une simple chaîne, un réglage hors des
  nœuds, l'étalonnage d'un groupe, une version distante étalonnée elle aussi
  (laquelle des deux le plan montre ne se lit pas), et tout paramètre au
  numéro inconnu. Nommé ne veut pas dire actif : la lecture ne sait pas dire
  si une fenêtre est allumée.
- **Les refus.** Une base introuvable (`IoError`), illisible ou d'un autre
  schéma (`ParseError`), ou qui n'a pas la timeline (`ValidationFailed`),
  refuse l'appel avant toute écriture : il demandait les deux.

**Rendre un nœud comme Resolve** (GR16, depuis le 25 octobre 2026).
L'étape `color.resolve_primaries` d'un plan prend les primaires d'un nœud
telles que Resolve les stocke. Ce sont les mêmes noms que `grades` (`lift_r`
… `lift_master`, `gamma_*`, `gain_*`, `offset_r`, `offset_g`, `offset_b`,
`saturation`, `lum_mix`, `contrast`, `pivot`, `temperature`, `tint`), en
fractions exactes, et l'étape les rend par la loi ci-dessus. Trois
paramètres de plus disent les réglages du projet, à 0 ou 1 : ceux dont
dépend le contraste, `color_space_aware` (0 quand il est absent, comme dans
Resolve) et `contrast_s_curve` (1 quand il est absent), et `acescct` (0
quand il est absent), qui dit qu'un nœud a été étalonné dans un projet en
ACEScct.

Le contraste au-dessus de 1, avec la courbe en S, n'a pas de formule : le
moteur porte une table mesurée dans Resolve 21.1 le 25 octobre 2026. Elle
couvre 33 contrastes de 1 à 1,8 et 33 pivots, et le moteur interpole
entre eux. Le moniteur reçoit la courbe de chaque nœud en 65 valeurs.

- **Où elle agit.** Sous gestion couleur, la valeur passe en lumière
  linéaire, en DaVinci Wide Gamut (Bradford depuis le blanc d'AP1), puis en
  DaVinci Intermediate, où Resolve étalonne ; la loi s'applique, et la valeur
  revient. Sans gestion couleur, la loi s'applique au signal affiché, comme
  dans un projet Resolve non géré.
- **Ce qui est prouvé.** La mesure du 25 octobre compte 143 cas, celle du
  6 novembre (la température et la teinte) 152 de plus, 125 points du
  réseau chacun (`tests/fixtures/resolve_primaries/`). Au pire du cube de
  Resolve, le moteur tombe à :
  - 0,03 code 10 bits sans contraste ;
  - 0,044 code avec une température ou une teinte, seules, ensemble, avec
    chaque autre primaire, et sur 12 nœuds tirés au hasard ;
  - 0,75 code avec le contraste, outils « color space aware » allumés ;
  - 1,14 code outils coupés, et 1,73 sans la courbe en S ;
  - 3,5 codes avec une température et un contraste sous 1 au Lum Mix 100,
    où le contraste seul manque déjà le cube de Resolve de 2,7 codes ;
  - en ACEScct (211 cas, 14 novembre 2026), 0,03 code pour les primaires
    et la saturation, nœuds complets compris ; jusqu'à 1,9 code avec une
    température ou une teinte, sur des valeurs de plus de 100 en lumière
    linéaire où Resolve arrondit (0,035 au 99ᵉ centile) ; le contraste comme
    en YRGB ; avec un contraste et une température, jusqu'à 23 codes là où
    la température pousse un canal loin sous 0, où Resolve prolonge le
    contraste d'une autre pente que la table (4 codes ailleurs).

  Entre les réglages de la table, 16 courbes mesurées hors grille tombent à
  0,06 code pour un pivot de 0,25 à 0,75, et jusqu'à 4,8 aux pivots
  extrêmes. Le moniteur (Metal) et l'export (le cube) rendent la même
  chose : à 2·10⁻⁶ près sur le signal affiché, et à 10⁻³ en ACEScct ou
  pour un nœud ACEScct.

  Sur de vrais étalonnages, ceux de 11 plans de PRJH, l'étape tombe à
  1,1 code au pire des cubes que Resolve a sortis pour ces plans, et à 0,28
  au 99ᵉ centile. `cutmachine_resolve_grade_tests --compare` refait la
  comparaison pour un autre projet, à partir de ses cubes et d'une copie de
  sa base.
- **Une seule luminance de travail mesurée.** Avec les outils « color
  space aware » allumés, la luminance de travail de la timeline change le
  contraste, de plusieurs centaines de codes (258 pour un contraste de 1,2
  en SDR 100). La table et le domaine HLG valent pour HDR 1000, celle
  de PRJH et de 266 des 284 projets du disque. Outils coupés, la
  luminance ne change rien.
- **Les bornes**, qui tiennent tous les étalonnages relevés chez l'auteur :
  - lift dans [−2, 2], gamma dans [−4, 4], gain dans [0, 16] ;
  - offset dans [−8, 8], saturation dans [0, 2], `lum_mix` et `pivot`
    dans [0, 1] ;
  - `temperature` dans [−4000, 4000] et `tint` dans [−100, 100], les
    butées du panneau de Resolve ;
  - `contrast` dans [0, 9/5] avec la courbe en S, où la table s'arrête, et
    dans [0, 2[ sans elle ;
  - un lift et un gain qui mettraient le noir au-dessus du blanc
    (1 − lift + lift·gain ≤ 0) sont refusés.
- **La température et la teinte** (depuis le 7 novembre 2026). Elles
  agissent en premier, sur les valeurs du nœud, avant le lift, le gamma et
  le gain, comme une matrice 3×3. Celle-ci est une adaptation de Bradford,
  dans les primaires BT.709, du blanc de 6 500 K vers le blanc de
  6 500 K moins la température, déplacé de la teinte divisée par 10 000
  le long de sa ligne d'isotempérature (lignes de Robertson). Resolve
  revient du domaine des cônes par l'inverse de Bradford arrondi à quatre
  décimales, et garde la coquille de la table de Wyszecki et Stiles à
  325 mired : le moteur fait de même. La suite de la loi lit les valeurs
  ainsi équilibrées, part Y comprise.
- **En ACEScct** (`acescct` à 1, mesuré les 7 et 14 novembre 2026). Le
  nœud étalonne des codes ACEScct en AP1 : sous gestion couleur, l'étape y
  prend la valeur de travail (telle quelle quand le moteur étalonne en
  ACEScct), et non DaVinci Intermediate. La loi y change en trois points :
  - une valeur négative traverse le gamma inchangée, au lieu de garder son
    signe sous la puissance ;
  - la saturation tourne autour de la luminance Rec.709 (0,2126, 0,7152,
    0,0722), et non des poids de DaVinci YRGB ;
  - la température et la teinte : la même matrice, sur la lumière
    ACES2065-1 (AP0) portée à la puissance 1/2,4, une valeur négative
    gardant son signe ; un gris bouge d'une constante en log.

  Le contraste suit la loi des outils coupés, et la luminance de travail
  n'y change rien. Outils allumés, le contraste suit une autre loi, jamais
  mesurée : l'étape le refuse par son nom.
- **Où elle se place.** En tête de l'ordre canonique : ce qu'on ajoute ensuite
  à la main vient après le nœud importé, comme un second nœud.
- **Le format.** Un document qui porte l'étape passe en version 22. Un
  binaire plus ancien, qui l'aurait sautée et rendu le plan sans étalonnage,
  refuse alors le fichier.
- **`export_cdl`** la refuse par son nom : la part Y et le passage en
  DaVinci Intermediate n'ont pas de forme CDL.

**Écrite par la relecture.** Avec `resolve_database`, chaque plan dont
l'étalonnage se rend entièrement reçoit ses nœuds, un
`color.resolve_primaries` par nœud allumé dans l'ordre de la chaîne. Ils
viennent en tête de la pile, par un `SetClipEffects` dans le journal de la
relecture. Les valeurs stockées s'écrivent en fractions exactes, celles de
leur décimale la plus courte (1,106 stocké donne 553/500).

Les réglages du projet se lisent dans la même copie de la base
(`SM_Config`) : la science couleur, les outils « color space aware », la
courbe en S et la luminance de travail. Un nœud au contraste réglé s'écrit
avec eux. La réponse les donne dans `grades.project_settings` (`read`,
`color_space_aware`, `contrast_s_curve`, `working_luminance`,
`color_science`, et `unread` quand ils ne se lisent pas).

La science couleur décide de ce qui se rend (mesuré le 6 novembre 2026) :
- en DaVinci YRGB et en DaVinci YRGB Color Managed, tout ce que l'étape
  prend ; leurs cubes sont identiques au dernier chiffre ;
- en ACEScct (depuis le 14 novembre 2026), tout ce que l'étape prend,
  chaque étape portant `acescct` à 1, sauf un contraste outils « color
  space aware » allumés, qu'aucun projet ACEScct n'allume ;
- ailleurs (Color Managed v1, ACEScc, jamais mesurés), aucun nœud.

Le 7 novembre 2026, sur les 293 projets du disque : 101 en Color Managed,
97 en ACEScct et 95 en YRGB.

Un étalonnage s'écrit entier ou pas du tout : un rendu à moitié serait une
image que Resolve n'a jamais montrée. Rien n'est écrit quand :
- la lecture a nommé quelque chose qu'elle ne lit pas (`unread`) ;
- un nœud porte un réglage que l'étape ne rend pas encore (rotation de
  teinte, roues Log, Color Boost, Midtone Detail, hautes lumières ou
  ombres, hors de leur neutre) ;
- un nœud porte un contraste, une saturation, une température ou une
  teinte, et les réglages du projet ne se lisent pas, ou sa science
  couleur n'est pas l'une de celles où ce réglage a été mesuré ;
- la science couleur du projet n'a jamais été mesurée ;
- un nœud porte un contraste, outils « color space aware » allumés, et la
  luminance de travail n'est pas HDR 1000 ;
- une valeur sort des bornes de l'étape.

Sur une copie du projet PRJH, 214 des 263 plans étalonnés s'écrivent
(202 avant la température et la teinte).

Dans `grades.clips`, `written_stages` compte les étapes écrites sur le plan,
`unrendered` dit en français pourquoi rien ne l'a été, et `written` compte
les plans écrits. L'étalonnage de la timeline elle-même n'est jamais écrit
(`timeline_unrendered`) : une pile appartient à un plan.

L'Inspecteur n'a pas de contrôle pour l'étape, et « Réinitialiser
l'étalonnage » la retire comme les autres.

Depuis le panneau Agent et pour un agent relayé à la fenêtre, l'import passe
par les mêmes étapes, sur le document et le journal de la fenêtre, avec la
même réponse (depuis le 24 octobre 2026, PHASES-2026-08 F2.4 ; pas vu à
l'écran).
