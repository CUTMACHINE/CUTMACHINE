# EDL CMX 3600 et WebVTT

FC7 — FILMCRAFT-2026-10. Porté de
FilmCraft (`crates/interchange/src/edl.rs`, `crates/captions/src/vtt.rs`,
MIT ou Apache-2.0, commit `09aaf9b`), dans `src/Edl.h` et `src/Subtitles.h`.

**Vérifié dans Resolve 21.1.0.17 le 4 novembre 2026** : coupes, fondus, noir,
appariement par noms de fichier avec bobine `AX` et demi-vitesse M2, sur les
fixtures de régression et des médias synthétiques. La voie des marqueurs par
son menu spécialisé reste à jouer ; l'import de timeline ne les pose pas.

## Les EDL d'une timeline

```sh
./build/cutmachine --export-edl ./Film.cutmachine-project/project.cutmachine.json \
  ./livraison/film.edl --timeline '<timeline-id>' [--start-timecode 01:00:00:00]
```

MCP : `export_edl` (`path` absolu, `start_timecode` facultatif).

**Une EDL par piste vidéo visible**, numérotées depuis la piste du bas comme
les couches du renvoi vers Resolve : `film_V1.edl`, `film_V2.edl`… à côté du
chemin donné, qui doit finir en `.edl`. Le numéro est celui de la piste dans
la fenêtre : une piste masquée, ou sans plan vidéo, garde le sien sans écrire
de liste, et `film_V2.edl` reste la piste V2 même quand V1 est masquée. Toutes les listes ou aucune : une
destination qui existe déjà refuse l'appel entier (`OutputExists`), et rien
n'est écrit.

Ce qu'une liste porte :

- les coupes, chaque plan en un événement, bobine `AX` (la source auxiliaire
  des listes de fichiers), nommé par un commentaire `* FROM CLIP NAME:` qui
  porte le nom de son fichier ;
- les fondus enchaînés (`D` et leur durée en images), le plan sortant lu au
  début du fondu, l'entrant dans ses poignées (`* TO CLIP NAME:`) ;
- l'ouverture au noir et la fermeture au noir (`BL`), les `dip_to_color`
  noirs au bord d'une piste ;
- une vitesse constante, en ligne `M2`, **seulement si elle s'écrit
  exactement** : la ligne écrit `ddd.d` images par seconde, soit
  vitesse × base du timecode. 1/2 à 25 i/s fait `012.5`, 2 fait `050.0`,
  1/2 à 23,976 fait `012.0` ; 2/3 à 25 i/s (16,666…) est refusé par son nom.
  Comme le veut la convention CMX, la durée source d'un événement ralenti est
  celle de l'enregistrement, et la ligne M2 dit la vitesse.

**Les timecodes.** Le côté enregistrement part de `start_timecode`,
`01:00:00:00` par défaut (le début de timeline de Resolve). Le côté source est
le temps du rush compté depuis `00:00:00:00` : le document ne connaît pas le
timecode de la caméra (voir `kUnknownStartTimecode`, [Resolve](resolve.md)),
et la réponse le dit (`source_timecodes: "media_time_from_zero"`). Un rush
dont la caméra a écrit un autre départ ne se conformera pas sur le timecode
tant que ce départ n'est pas connu. 29,97 et 59,94 s'écrivent en drop-frame
(`;` avant les images, `FCM: DROP FRAME`) ; 23,976 compte sur une base de 24,
sans drop-frame ; une autre cadence non entière est refusée.

**Refusé par son nom**, parce que l'écrire changerait le montage : une
cadence sans timecode, un plan qui ne tombe pas sur des images entières, un
rush à une autre cadence que la séquence (une liste compte source et
enregistrement dans une seule base), une vitesse sans écriture M2, une image
figée, un fondu touchant une rampe de vitesse, un fondu qui sort d'un plan
ralenti (avec toutes les bobines `AX`, deux lignes M2 dans un événement ne
diraient pas de quel côté elles parlent), un `dip_to_color` entre deux plans
ou vers une autre couleur que le noir, plus de 999 événements dans une liste,
une timeline sans plan vidéo.

**Compté et nommé** dans `losses` (`kind`, `id`), avec un compte par famille
dans la réponse : l'étalonnage (`grades_not_sent`), le cadrage
(`framing_not_sent`), l'opacité (`opacity_not_sent`), le flou
(`blurs_not_sent`, EFFETS-2026-10 EF3), les rampes de vitesse,
écrites à leur vitesse moyenne — leurs points de coupe restent exacts, la
courbe est la perte (`speed_ramps_flattened`) —, les plans motion, les angles
multicam inactifs, les pistes son, de légendes et masquées, le LUT de
séquence (`look_lut_not_sent`) et les marqueurs (`markers_not_sent` :
`export_edl_markers` les écrit).

## Les marqueurs

```sh
./build/cutmachine --export-edl-markers ./Film.cutmachine-project/project.cutmachine.json \
  ./marqueurs.edl [--style loc|resolve] [--timeline '<timeline-id>'] [--start-timecode 01:00:00:00]
./build/cutmachine --import-edl-markers ./Film.cutmachine-project/project.cutmachine.json \
  ./marqueurs-resolve.edl [--timeline '<timeline-id>'] [--start-timecode 01:00:00:00]
```

MCP : `export_edl_markers` et `import_edl_markers`.

Un événement par marqueur, et l'une de deux écritures :

- `loc` (par défaut) : `* LOC: 01:00:05:00 YELLOW  nom`, les huit couleurs
  d'Avid (`RED`, `GREEN`, `BLUE`, `CYAN`, `MAGENTA`, `YELLOW`, `WHITE`,
  `BLACK`). Le commentaire n'a pas de durée : un passage s'y écrit en point,
  compté dans `durations_not_sent` ;
- `resolve` : la ligne que la liste de marqueurs de Resolve écrit sous chaque
  événement, ` |C:ResolveColorBlue |M:nom |D:50`, avec ses seize couleurs
  (Blue, Cyan, Green, Yellow, Red, Pink, Purple, Fuchsia, Rose, Lavender,
  Sky, Mint, Lemon, Sand, Cocoa, Cream). `D` est la longueur en images : un
  point s'écrit `D:1`, un passage garde la sienne.

Une couleur du document sans mot à elle (l'orange `#ff8a3d` des questions de
l'agent, par exemple) s'écrit comme la plus proche, et son marqueur est nommé
dans `colors_approximated`. Un nom qui contient un retour à la ligne, ou un
`|` dans l'écriture de Resolve (son séparateur), est refusé.

**La lecture** prend les deux écritures dans une même liste, `* LOC:` et
`|C:` (rattachée au timecode d'enregistrement de son événement), et honore la
ligne `FCM:` et le `;` de chaque timecode. Le timecode de départ se relit dans
le compte de la liste : à 29,97, une liste écrite sans drop-frame
(`FCM: NON-DROP FRAME`) pose ses marqueurs à leur place, pas 108 images plus
loin par heure. Un marqueur sans nom prend celui que la fenêtre donne,
« Marqueur ». Chaque mot de couleur se relit
dans le document par cette table, majuscules indifférentes :

| Mot | Couleur du document |
|---|---|
| `YELLOW`, `Yellow` | `#f5c542` (le défaut du document) |
| `BLUE`, `Blue` | `#4da3ff` (le bleu des marqueurs de la fenêtre) |
| `RED`, `Red` | `#ff4d4d` |
| `GREEN`, `Green` | `#4caf50` |
| `CYAN`, `Cyan` | `#3fd0d4` |
| `MAGENTA`, `Fuchsia` | `#d050c8` |
| `WHITE` | `#ffffff` |
| `BLACK` | `#000000` |
| `Pink` | `#ff7fbf` |
| `Purple` | `#9a5cd0` |
| `Rose` | `#ff9aa8` |
| `Lavender` | `#b9a6e8` |
| `Sky` | `#8fd3ff` |
| `Mint` | `#8fe0b8` |
| `Lemon` | `#f0f07a` |
| `Sand` | `#d2b48c` |
| `Cocoa` | `#8b5a3c` |
| `Cream` | `#f5ecd0` |

**Une couleur inconnue est refusée par sa ligne**, jamais devinée. Les
marqueurs entrent dans la catégorie `note`, comme une `AddMarkerOperation`
chacun, groupés en **une seule entrée d'annulation** (un `BatchOperation`
« marqueurs EDL ») : un Annuler retire tout l'import. Un marqueur que la
timeline porte déjà (même temps, même longueur, même nom, même couleur) est
sauté et compté dans `already_present` : une liste importée deux fois ne
double pas. Un marqueur avant le timecode de départ, un drop-frame sur une
base qui n'en a pas, une ligne `|C:` sous aucun événement, un événement dont
le timecode d'enregistrement ne se lit pas dans cette base, un caractère de
contrôle dans un nom, une liste sans marqueur sont refusés, par leur ligne. Au-delà de 256 marqueurs à ajouter, l'import est
refusé plutôt que coupé en plusieurs annulations.

## WebVTT

```sh
./build/cutmachine --export-vtt ./Film.cutmachine-project/project.cutmachine.json \
  ./film.vtt --timeline '<timeline-id>' [--word-by-word]
```

Les mêmes cues que `--export-srt`, la même réponse, plus `format: "webvtt"` :
`WEBVTT`, une ligne vide, puis pour chaque cue son numéro,
`HH:MM:SS.mmm --> HH:MM:SS.mmm` et son texte. Même arrondi à la milliseconde
la plus proche que le SRT, le seul du chemin, compté dans
`rounded_timestamps`. Le texte reste du texte : `&`, `<` et `>` s'échappent
(`&amp;`, `&lt;`, `&gt;`), puisque WebVTT les lit comme des balises. Un texte
qui contient une ligne vide est refusé, une ligne vide finissant une cue en
WebVTT. L'export SRT n'a pas d'outil MCP ; le WebVTT non plus.

## Vérifier dans Resolve

Le 4 novembre, le point 1 ci-dessous passe par l'API dans un projet jetable.
Les points 2 et 3 restent à jouer par les menus spécialisés, écran libre :

1. **Une timeline.** `--export-edl` sur une timeline de coupes simples, puis
   d'une timeline avec un fondu, un fondu au noir et un plan à 1/2. Dans
   Resolve : *File › Import › Timeline…* (ou clic droit dans le Media Pool,
   *Timelines › Import › AAF/EDL/XML…*), choisir `film_V1.edl`, régler la
   cadence de la timeline sur celle de la séquence, et **décocher**
   *Automatically import source clips into media pool* pour qu'il cherche
   dans le Media Pool. Regarder : les plans sont-ils reliés aux bons rushes
   (le nom de fichier du `* FROM CLIP NAME:` suffit-il avec une bobine
   `AX`) ? Tombent-ils aux bonnes images (le timecode source est compté
   depuis zéro : un rush tourné avec un timecode de caméra restera hors
   ligne) ? Le fondu, le noir et la ligne M2 sont-ils lus ? Un plan ralenti
   qui entre dans un fondu écrit sa ligne M2 sous deux lignes `AX` : Resolve
   la donne-t-il au plan entrant (le timecode de la ligne M2 est le sien) ?
2. **Des marqueurs vers Resolve.** `--export-edl-markers --style resolve`,
   puis dans Resolve, sur la timeline ouverte : clic droit sur la timeline
   dans le Media Pool, *Timelines › Import › Timeline Markers from EDL…*.
   Recommencer avec `--style loc`. Noter laquelle des deux Resolve lit, ses
   couleurs et la longueur d'un passage.
3. **Des marqueurs depuis Resolve.** Dans Resolve, poser quelques marqueurs
   de couleurs différentes, dont un passage, puis clic droit sur la timeline
   dans le Media Pool, *Timelines › Export › Timeline Markers to EDL…*, et
   `--import-edl-markers` sur ce fichier : un Annuler dans CUTMACHINE doit
   tout retirer.

Les intitulés de menus ci-dessus sont ceux qu'on attend de Resolve 21, pas
des intitulés relevés : la première passe les vérifie aussi. Ce qui en
ressort se date ici.
