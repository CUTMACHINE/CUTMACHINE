# API d'édition

> Relu contre le code le 3 octobre 2026. Les trois dernières sections sont
> écrites le 7 octobre d'après le catalogue du binaire ; la vitesse, le
> cadrage, le fondu par une couleur et le refus d'une LUT illisible y ont
> été joués au CLI sur un projet d'essai.

Les outils partagés CLI/MCP `edit_source_range` et `delete_timeline_range`
exposent le montage des zones Source/Record sans ouvrir les moniteurs.
L'insertion ou l'écrasement prend les points Source et Record explicites ;
le code déduit le point manquant d'un montage trois points. L'effacement
conserve la zone vide ou l'extrait avec `ripple:true`, en respectant les
pistes ciblées, le verrouillage et le sync lock. `preview:true` valide sans
écrire et renvoie l'opération canonique ; appliquer le montage crée une seule
entrée annulable du journal.

Avec `ripple:true`, `followers` dit comment suivent les pistes en sync-lock
non ciblées. `cut` (par défaut) leur retire la même plage : un plan de coupe
qui l'enjambe perd des images en plein milieu. `continuous` leur applique le
même changement de temps en gardant les images de chaque plan continues depuis
sa tête : un plan de coupe posé sur des mots retirés reste entier et se
termine avec eux. `remove_words`, `clean_disfluencies`, `tighten_pauses` et
`remove_source_ranges` entraînent de la même façon, par défaut, toutes les
pistes en sync-lock non verrouillées ; `sync_track_ids` en donne une liste
explicite.

Une paire A/V coupée par mots garde un seul groupe de liaison sur tous ses
morceaux. `ripple_trim` ne rogne donc plus « tout le groupe » : sur chaque
autre piste, il prend le membre dont le même bord est le plus proche. C'est ce
qui permet de rallonger la queue du dernier morceau sans rallonger les autres.

Une coupe, un rognage ou une suppression par mots renouvelle l'identifiant des
morceaux qu'elle crée. Le delta rendu par chaque outil nomme ces identités :
les plans créés portent `created: true` et les disparus sont listés dans
`removed_clip_ids` ; les styles de légende, marqueurs, transitions et chutiers
ont leurs `created_<type>_ids` et `removed_<type>_ids`, et
`--apply-project-op` rend `created_timeline_ids` et `removed_timeline_ids`.
L'appelant n'a pas à relire le projet pour retrouver ce qu'il vient de créer.
Un plan du delta porte aussi sa `speed` (`{"num":2,"den":1}`) quand elle
n'est pas 1/1 ou que l'édition l'a changée, retour à 1/1 compris : absente,
elle vaut 1/1 et n'a pas bougé. `speed_ramp` vaut `true` tant que le plan
suit une rampe (ses clés sont dans `describe`), `false` quand l'édition l'a
retirée. Avant le 18 octobre 2026, un agent qui tenait son modèle par les
deltas gardait l'ancienne vitesse après un `rate_stretch` ou un
`set_clip_speed`, et en déduisait une plage de rush fausse. `enabled` vaut
`false` tant que le plan est désactivé, `true` quand l'édition l'a réactivé ;
absent, le plan est actif et n'a pas changé.
Tout outil d'édition journalisée qui a changé la timeline rend un `delta`,
même quand son backend n'en donne pas : le catalogue le calcule alors des
deux états de la timeline (PRD40). Un aperçu, une proposition ou une
intervention repliée, qui ne changent rien, n'en ont pas ; `apply_edit_plan`
garde son `delta_as_prepared`.

`Operations.h` expose `InsertClipOperation`, `RemoveClipOperation`,
`TrimClipOperation`, `MoveClipOperation`, `DeleteGapOperation` et
`SplitClipOperation`, ainsi que `RippleTrimOperation`, `RollEditOperation`,
`SlipEditOperation`, les variantes liées de move, trim et remove et
`SetClipAudioOperation` (gain dB exact et fondus `RationalTime`),
`AddTrackOperation` pour le
multipiste. Les marqueurs de projet sont des objets
adressables persistants ; `AddMarkerOperation`, `RemoveMarkerOperation` et
`UpdateMarkerOperation` passent par le même journal et apparaissent dans
`--describe` sous les aliases `K1`, `K2`, etc. Les pistes ont les alias
`T1`, `T2`… (leur rang dans `describe`) ; les plans de la première piste sont
`A1`, `A2`…, ceux de la deuxième `B1`… Ajouter une piste à un index plus bas,
ou en retirer une, renumérote les suivantes ; une vingtième piste n'a pas
d'alias (les plans de la vingtième piste s'appellent déjà `T1`, `T2`…). Les
outils acceptent ces alias, `--apply-op` non : il prend l'opération telle
quelle. Les réglages de la séquence
passent eux aussi par `UpdateSequenceOperation`, avec validation atomique et
undo/redo sans remplacer son ULID ni sa timeline ; `update_sequence` ne change
que les champs donnés (`name`, `width`, `height`, `frame_rate`) et garde les
autres. La feuille de route
comportementale est décrite dans
[`docs/NLE_TIMELINE_SPEC.md`](../NLE_TIMELINE_SPEC.md).
`JoinClipOperation` est l'inverse exact persisté d'une coupe. `EditLog::Apply`,
`Undo` et `Redo` renvoient un `EditError` nommé et garantissent l'atomicité du
document.

Le log conserve, dans les inverses Insert/Remove, les représentations exactes
des `timeline_in` affectés par le ripple. Cette métadonnée est nécessaire pour
restaurer les octets canoniques d'origine lorsque des timebases différentes
représentent le même instant. Elle ne constitue pas une opération supplémentaire.

Deux fondations restent volontairement indépendantes de l'interface :
`ProjectRecovery` écrit et inspecte un autosave canonique dérivé sans jamais
remplacer implicitement le document principal, et `KeyframeTrack` fournit des
keyframes à ULID stable avec interpolation Hold, Linear ou CubicBezier. Le
temps des keyframes reste rationnel jusqu'au calcul de la fraction
d'interpolation ; seules les valeurs de paramètres sont flottantes.

## Les autres gestes de montage

Au-delà d'`edit_source_range` et de `delete_timeline_range`, chaque geste de
montage a son outil, et chacun est une opération réversible de
`Operations.h` :

- poser et retirer : `insert_clip` (pousse les plans suivants de la piste),
  `paste_clips` (un lot de plans, liés entre eux ou non, insérés ou, avec
  `overwrite: true`, posés par-dessus ce qui est dessous sans rien décaler),
  `remove_clip` (referme le trou, et les pistes synchronisées nommées),
  `clear_clip` et `clear_clips` (laissent le trou), `delete_gap` (retire un
  trou de durée exacte et décale la suite, sur la piste et sur
  `linked_track_ids`) ;
- bouger et rogner : `move_clip` (écrase ce sur quoi il tombe),
  `move_clips` (au moins deux plans, une destination exacte chacun, un seul
  Annuler), `trim_clip` (un bord, le plan seul), `ripple_trim`, `roll_edit`,
  `slip_edit`, `slide_edit` et `rate_stretch` (section suivante) ;
- couper : `split_clip` (strictement à l'intérieur du plan),
  `split_at_interval` (toutes les N secondes en un appel : l'intervalle se
  donne en durée et le moteur le convertit en images de la séquence ; les
  partenaires liés sont coupés aux mêmes endroits) ;
- lier et délier : `detach_audio` (le son d'un plan vidéo sur une piste son,
  lié en synchro), `set_clip_link` (lie deux plans en paire, ou défait le
  lien), `split_link_groups` (sépare les liens fusionnés, voir plus bas).

Un lien réunit une image et ses sons là où ils sont ensemble : les membres
d'un groupe se chevauchent dans le temps (une image et son son, un J ou un L
cut, le son d'un enregistreur à côté de celui de la caméra). Avant le
30 octobre 2026 (#213), une découpe (`remove_source_ranges`,
`tighten_pauses`) laissait tous les morceaux d'un bloc sous un seul lien : un
clic prenait le bloc entier, et un glissé ou un trim emportait tous ses
morceaux. `split_link_groups` donne à chaque position son propre lien, en une
annulation (deux au-delà de 256 liens) : la première garde l'ancien, chaque
membre est relié à l'image de sa position, décalage de synchro compris, et un
plan seul à sa position perd son lien. Une position qu'un lien refuse (un
gel, une rampe) garde l'ancien lien. `preview: true` compte sans rien changer.
La fenêtre applique cette réparation quand elle ouvre une timeline, et le dit
dans la ligne d'état (`LinkGroupRepair.h`).

Un groupe image/son lié se manipule d'un seul geste : `move_linked_clips` et
`trim_linked_clips` (une destination ou un bord par membre),
`shorten_linked_clip` (« retire N images ou secondes » : le moteur résout
chaque membre, le sens du bord et le temps exact ; `preview: true` valide
sans écrire), `split_linked_clips` (tous les membres au même instant, deux
nouveaux groupes), `remove_linked_clips` et `clear_linked_clips`.

## Slip, slide et rate stretch

Le slide et le rate stretch sont deux gestes de finition repris de FilmCraft
(FILMCRAFT-2026-10 FC3, 17 octobre 2026), chacun une opération de
`Operations.h`, un outil MCP et, au CLI, `--apply-op` avec l'opération
entière. Ils sont dans la fenêtre depuis le 19 octobre
([`timeline-graphique.md`](timeline-graphique.md), « Mode trim et Trim
Monitor »).

**`slip_edit`** (`SlipEditOperation`, type `SlipEdit`) fait glisser la plage
de rush sous un plan de `delta`, sans bouger le plan ni changer sa durée.
Depuis le 24 octobre 2026 (DEC40), l'outil MCP emmène le son lié comme
`slide_edit` : sans `linked_clip_ids`, chaque plan nommé glisse avec son
partenaire sur chaque autre piste (`SlipClipsWithLinkedPartners`, la règle
du slide), et l'image et le son restent sur les mêmes images. `[]` garde le
recalage d'une piste seule contre l'autre ; avant ce jour, c'était le seul
comportement, sans un mot. `SlipEditOperation`, à `--apply-op`, prend la
liste telle qu'elle est.

```json
{"clip_ids":["A2"],"delta":{"value":2,"rate":25},"linked_clip_ids":[]}
```

**`slide_edit`** (`SlideEditOperation`, type `SlideEdit`) déplace un plan de
`delta` entre les deux plans qui le touchent sur sa piste. Le plan garde sa
plage de rush et sa durée ; le voisin de gauche s'allonge (ou raccourcit)
par sa queue, celui de droite raccourcit (ou s'allonge) par sa tête, du même
`delta`. Rien d'autre ne bouge sur la piste, et la durée du programme reste
la même. Plusieurs plans nommés doivent partager un groupe de liaison ;
chacun glisse entre ses propres voisins.

Par l'outil MCP, le son lié suit, comme pour `ripple_trim` : sans
`linked_clip_ids`, chaque plan nommé emmène son partenaire sur chaque autre
piste (le membre de son groupe dont la tête est la plus proche, la règle de
`LinkedEdgePartnerIds`). Un tableau présent, même vide, est le choix exact
de l'appelant : `[]` fait glisser l'image seule, hors de synchro avec son son.
`SlideEditOperation`, à `--apply-op`, prend la liste telle qu'elle est.

```json
{"clip_ids":["A2"],"delta":{"value":3,"rate":25}}
```

Refusé, par son nom : un plan sans voisin collé d'un côté ou de l'autre
(un vide n'est pas un voisin ; `move_clip` déplace un plan dans un vide), un
voisin qui tomberait à une durée nulle, un voisin qui n'a plus d'images de
rush pour s'allonger, un plan nommé deux fois ou un voisin partagé par deux
glissements, une piste verrouillée. Les vérifications sont celles de
`roll_edit` : le voisin de droite lit son rush à sa vitesse, et une tête
qui tomberait au milieu d'une image de rush d'un plan ralenti est refusée ;
une transition posée sur un des deux raccords suit le raccord, et elle est
refusée si les nouvelles poignées ne la nourrissent plus. Sur une piste de
légendes, un carton glisse entre deux cartons sans lire de rush.

**`rate_stretch`** (`RateStretchOperation`, type `RateStretch`) donne à un
plan une nouvelle `duration` sur la timeline en gardant la plage de rush
qu'il lit : sa vitesse devient plage / durée, fraction exacte réduite (dix
images étirées sur vingt : `1/2` ; sur quinze : `2/3`). `edge` dit quel bord
bouge : `Tail` (par défaut) garde la tête en place, `Head` garde la queue.
Tout le groupe de liaison du plan suit, comme pour `set_clip_speed` : image
et son gardent une seule vitesse, et le son est étiré à hauteur gardée.

```json
{"clip_id":"A2","duration":{"value":20,"rate":25},"edge":"Tail"}
```

Par défaut, rien ne se décale : un étirement qui mord sur le plan suivant
est refusé (`Overlap`), un plan raccourci laisse un vide, comme un
`trim_clip`.

Avec `ripple: true` (DEC40, 24 octobre 2026), l'étirement ripple, comme dans
le mode Trim de Resolve (manuel 21.1, p. 1219) : chaque membre garde sa tête
où elle est, quel que soit `edge` (comme un `ripple_trim` de tête), et tout
plan qui commence à la fin d'origine d'un membre, ou après, sur sa piste, se
décale de la différence de durée. Les autres pistes qui suivent sont
`sync_track_ids`, comme pour `ripple_trim` : sans ce champ, l'outil MCP
prend chaque piste synchronisée qui ne porte aucun membre, et nomme dans
`warning_track_ids` une piste non synchronisée qui a des plans après le
plan ; `[]` ne décale que les pistes du groupe. Sur une piste suivie, le temps
ajouté s'ouvre à la fin d'origine du plan nommé, le temps retiré est la
plage [nouvelle fin, ancienne fin), refermée comme par `ripple_trim`
(`CollapseClipThroughRange`). `sync_track_ids` sans `ripple` est refusé, et
un groupe qui a deux membres sur une même piste aussi, avec `ripple` : le
ripple de l'un décalerait l'autre. Une piste suivie verrouillée arrête
l'étirement (`LockedTrack`). Les deux champs ne sont écrits dans le journal
que posés : un étirement sans ripple garde ses octets.

```json
{"clip_id":"A2","duration":{"value":20,"rate":25},"ripple":true}
```

Refusé
aussi, par son nom : une vitesse dont la fraction réduite a un terme
au-delà de 1000 (dix images sur 1001 : `10/1001`), jamais arrondie ; un
plan à rampe de vitesse, un arrêt sur image, un carton, un plan de motion ;
un groupe dont les membres n'ont pas la durée et la vitesse du plan nommé
(un groupe coupé en morceaux par mots, par exemple : `detach_audio` ou
`set_clip_link` d'abord) ; une tête qui passerait avant le début de la
timeline ; une piste verrouillée sous l'un des membres. Les règles de
`set_clip_speed` valent ensuite : `source_in` sur une image du rush, et
aucun fondu enchaîné sur un plan dont la vitesse n'est pas 1. Les clés
d'animation d'un plan étiré restent à leur temps local : elles ne sont pas
mises à l'échelle.

Les deux opérations gardent dans le journal l'état exact des pistes
touchées : un Annuler rend le document octet pour octet.

## Réglages d'un plan

Ce qu'un plan porte au-delà de ses bornes se règle outil par outil, chacun
par une opération réversible (`SetClipSpeedOperation`,
`SetClipFramingOperation`, `SetClipAnimationOperation`,
`SetClipOpacityOperation`, `SetClipEnabledOperation`,
`SetClipBlurOperation`). Ils n'ont pas de commande à eux : au CLI, ils
passent par `--call-tool`, ou par `--apply-op` avec l'opération entière.
Toute valeur est une fraction exacte `{num, den}`, jamais un flottant.

- **Vitesse** (`set_clip_speed`, REEL-2026-09 R3) : une vitesse constante,
  `1/2` pour un ralenti de moitié, `2` pour deux fois plus vite. Le plan
  garde sa durée sur la timeline et lit durée × vitesse de son rush, sans
  interpoler d'image ; sur un plan lié, tout le groupe suit, et le son est
  étiré à hauteur gardée. Refusé quand le `source_in` du plan ne tombe pas
  sur une image de la source.
- **Rampe de vitesse** (`set_clip_speed` avec `speed_ramp`, REEL-2026-09
  R3, critère 4, 11 octobre 2026) : des clés `{time, value, interpolation}`
  en temps local du plan (0 est sa première image), chacune `linear` (par
  défaut) ou `hold` jusqu'à la suivante ; la vitesse de la première clé vaut
  avant elle, celle de la dernière après. Le plan lit l'intégrale de la
  courbe, exacte, sans interpoler d'image : `1` → `1/4` sur 10 images puis
  retour à `1` est un ralenti posé sur un temps. Au moins deux clés ; une clé
  `cubic-bezier` est refusée (son intégrale n'est pas exacte : une rampe douce
  s'écrit en clés linéaires rapprochées). Image seule : refusée sur une
  piste son et sur un plan lié (`detach_audio` d'abord). `speed` et
  `speed_ramp` s'excluent ; poser l'un efface l'autre. Un identifiant de clé
  omis est généré. `describe` rend `speed_ramp` sous la même forme. Le
  retour vers Resolve (XML) refuse un plan rampé.
- **Arrêt sur image** (REEL-2026-09 R4, décidé le 8 octobre 2026) : la
  vitesse `0/1`. Le plan tient l'image où tombe son `source_in` pendant toute
  sa durée, et ne lit aucune plage de son rush ; le moniteur, l'export, les
  planches et `verify_export` nomment tous cette image. `insert_clip` le pose
  avec `speed: {"num":0,"den":1}` (le `source_in` ramené au début de son
  image), y compris sur la toute dernière image d'un rush, ce qu'une
  insertion à vitesse 1 refuserait ; `set_clip_speed` fige un plan existant.
  Image seule : refusé sur une piste son, et sur un plan lié à un son, qui
  n'aurait rien à tenir. Le retour vers Resolve (XML) le refuse encore. Au
  CLI, par `--apply-op` (`InsertClip` avec `speed`). Une image fixe (PNG,
  JPEG, TIFF : un logo, un packshot) s'ingère depuis le 24 octobre 2026
  comme un média d'une image (R4) : posée à la vitesse 0/1, elle tient la
  durée voulue, son alpha gardé, et se rallonge ou se raccourcit comme un
  autre plan. Un logo se déclare graphisme (`set_media_color_input`,
  `graphics: true`, voir [Séquence](sequence.md)). La pièce `logo` reste là
  pour un logo animé.
- **Cadrage** (`set_clip_framing`, R1) : `fit` (défaut) garde toute l'image,
  `fill` couvre le cadre et coupe ce qui dépasse (un rush 16:9 dans une
  séquence 1080 × 1920, sans bandes). `scale` multiplie l'échelle du mode,
  `position_x` et `position_y` déplacent le centre d'une fraction du cadre
  (x vers la droite, y vers le bas), les quatre `crop_*` retirent une
  fraction de chaque bord de la source ; appliqué dans l'ordre rotation,
  recadrage, mode, échelle, position. C'est un cadrage complet : ce qui n'est
  pas donné revient à son défaut. `list_clip_framing` rend, pour un instant
  d'un plan, la région de source lue, le rectangle que l'image occupe, les
  bords du cadre laissés découverts en pixels (`uncovered`) et
  `covers_frame` ; le moniteur, l'export, les planches et `verify_export`
  posent le plan sur ce même rectangle. Dans la fenêtre, la section
  Composition de l'Inspecteur écrit la même opération : **Cadrage**
  (Ajuster ou Remplir), **Zoom** (de 1/8 à 8, au millième), **Position X**
  et **Position Y** en pixels de la séquence depuis le centre, Y compté vers
  le haut comme dans Resolve (l'inverse de `position_y`). Le moniteur Record
  suit le curseur pendant qu'on le tire ; l'opération est écrite au
  relâché, une par geste, et un double-clic remet 1 ou 0. Un zoom ou une
  position animés s'affichent « animé » et restent à leur animation.
- **Animation** (`set_clip_animation`, R2) : des clés sur `opacity`,
  `framing.scale`, `framing.position_x` et `framing.position_y`, au moins
  une par piste (une clé seule tient sa valeur sur tout le plan, depuis le
  31 octobre 2026, R2c), chacune à un temps local au plan (zéro est sa
  première image), avec `hold`, `linear` ou `cubic-bezier` (quatre points de contrôle
  en fractions exactes). L'appel donne l'animation entière : ce qu'il ne
  donne pas est retiré, et une liste vide retire toutes les clés. Les clés
  restent sur leurs images quand le plan est déplacé, rogné ou coupé. Sur un
  clip de motion, seule l'opacité s'anime : un cadrage animé est refusé à la
  pose, et l'export rend le fondu image par image, avec ou sans gestion
  couleur, depuis le 31 octobre 2026 (R2b). Dans la fenêtre, le losange de
  l'Inspecteur pose ou retire la clé de l'image sous la tête de lecture, et
  un curseur animé règle celle de l'image où on le lâche (R2c) : ce sont les
  mêmes opérations. L'opacité qu'un plan montre est celle de ses clés, qui
  remplace sa constante, au moniteur comme à l'export.
- **Cadrage qui suit le sujet** (`track_subject_framing`,
  CADRAGE-2026-10) : met chaque plan en
  `fill` (échelle et recadrages gardés, ou `scale` donné) et pose des clés
  linéaires sur `framing.position_x` et `framing.position_y` qui suivent un
  visage comme un cadreur. Le cadre reste immobile tant que le visage reste
  à moins de `dead_zone_x`/`dead_zone_y` de sa place (`target_x`,
  `target_y`) ; quand il en sort, un mouvement borné en vitesse
  (`max_speed`) et en accélération (`max_acceleration`) le ramène à sa place
  (à `settle` près) et s'arrête, sans revenir sur un dépassement que la zone
  tient encore. Un seul sujet : le visage le plus proche de `hint`, sinon le
  plus grand, puis le plus proche de sa dernière place (à moins de `gate`
  largeurs de visage). Un trou de détection de `bridge_gap` au plus est
  comblé, un trou plus long tient le cadre. Le cadre ne découvre jamais le
  bord : un sujet hors de portée est tenu au bord et compté
  (`frames_at_edge`). Les visages sont lus toutes les `every_frames` images
  par le détecteur de l'app (Vision, macOS seulement ; refusé par son nom
  ailleurs), ou donnés dans `detections` (temps local au plan, rectangles en
  fractions de l'image entière du rush). Les autres clés du plan sont
  gardées ; un plan de motion ou dont l'échelle est animée est refusé. Le
  résultat est un `SetClipFraming` et un `SetClipAnimation` dans un lot :
  une annulation, rejouée sans détecteur. La réponse donne, par axe, les
  clés, les mouvements, la marge et les images tenues au bord. Au CLI, par
  `--call-tool`.
- **Stabilisation d'un plan** (`stabilize_clip`,
  PARITE-2026-11 PA7) : lit dans le rush le
  déplacement de l'image entre chaque image du plan et la suivante, lisse
  le chemin de la caméra (une moyenne gaussienne d'écart-type `smoothing`,
  1/2 s par défaut), et écrit la différence en clés linéaires sur
  `framing.position_x` et `framing.position_y`. Un panoramique reste, le
  tremblement autour s'en va. Le plan est mis en `fill` et agrandi de la
  plus petite échelle qui tient toutes les corrections, jusqu'à `max_scale`
  (5/4 par défaut) ; au-delà, les corrections s'arrêtent au bord de la
  marge et sont comptées (`frames_at_edge`) plutôt que de montrer une
  bordure. Un `scale` donné est pris tel quel. La position propre du plan
  reste le centre autour duquel les corrections bougent ; ses clés de
  position sont remplacées, donc l'outil passe avant
  `track_subject_framing`, pas après. La réponse donne l'échelle choisie
  (`scale`, `scale_chosen`, `scale_needed`), le tremblement en pixels de
  la séquence avant et après (`shake.before_rms_px`, `shake.left_rms_px`),
  les images non mesurées (`frames_unmeasured`) et, par axe, les clés et
  les images tenues au bord. Les clés suivent le chemin à 1/5000 du cadre
  près (`tolerance`), soit une clé sur presque chaque image. Ce que l'outil
  ne fait pas :
  - seule la translation est corrigée : ni le roulis, ni le zoom entre deux
    images (mesuré et rendu sous `zoom_not_corrected_max_percent`), ni
    l'ondulation de l'obturateur ;
  - la mesure est celle de l'image, pas de la caméra : un sujet qui remplit
    le cadre et bouge est lu comme un mouvement de la caméra ;
  - un plan qui a une vitesse et un rush qui n'est pas à la cadence de la
    séquence sont refusés ; un plan de motion ou dont l'échelle est animée
    aussi ;
  - rien n'a été vu sur un film. Sur un rush de synthèse de 1280 × 720
    dont chaque déplacement est connu, la lecture est juste à 0,25 px en
    moyenne quadratique (0,43 px au pire), et un tremblement de 20 px est
    ramené à 0,04 px pour 4,4 % de zoom.
  Le résultat est un `SetClipFraming` et un `SetClipAnimation` dans un lot :
  une annulation, rejouée sans décoder une image. Au CLI, par
  `--call-tool`.
- **Opacité** (`set_clip_opacity`) : l'opacité de composition d'un plan
  vidéo.
- **Activer / désactiver** (`set_clip_enabled`, `clip_ids` et `enabled`,
  demandé par l'auteur le 29 octobre 2026) : le Clip > Enable/Disable Clip de
  Resolve, la touche D de la fenêtre. Un plan désactivé reste sur sa piste :
  il garde sa place, sa durée, ses liens, son étalonnage et ses réglages de
  son, les éditions le déplacent et le coupent comme un autre (ses deux
  moitiés restent désactivées), et la durée de la timeline le compte. Il
  n'est ni vu ni entendu : le moniteur, le son de la lecture, l'export,
  `verify_export`, `transcribe_timeline`, `analyze_timeline_audio`, la
  transcription de la timeline, les planches, `timeline_stats`,
  `review_cuts` et les sous-titres le lisent comme un trou dans sa piste (ce
  qu'il y a dessous, sinon noir et silence) ; dans un fondu enchaîné, il
  sort comme un angle multicam caché, l'autre côté gardant son fondu. Il
  vaut pour tout plan, image, son, motion ou carton ; désactiver une image
  laisse jouer son son lié, qu'il faut nommer aussi (la fenêtre les prend
  ensemble quand la sélection liée est active). `enabled: true` rend le
  montage octet pour octet ; refusé sur une piste verrouillée. Le fichier
  écrit `"enabled": false` sur le plan, en version 23 du document ;
  `describe` et le delta le rendent pareil, et `check_timeline` liste les
  plans désactivés (`disabled`). Au retour vers Resolve, le XML FCP7 écrit
  le plan `<enabled>FALSE</enabled>` à sa place (pas encore vérifié dans
  Resolve), l'EDL le compte comme perte (`disabled_clips_not_sent`), et
  l'ajout par `AppendToTimeline` le refuse par son nom. La règle est dans
  `ClipEnabled.h` ; un nouveau lecteur de l'image ou du son la demande.
- **Masque** (`set_clip_mask`,
  PARITE-2026-11 PA8) : une forme qui
  découpe l'image d'un plan vidéo. Dedans, le plan ; dehors, les pistes du
  dessous (`inverted` renverse). `shape` vaut `rectangle`, `ellipse` ou
  `none`, qui retire le masque. Pour un écran partagé, une image dans un
  rond, une fenêtre aux bords doux.
  - La forme est posée **sur l'image du plan**, pas sur le cadre :
    `center_x`, `center_y`, `width` et `height` sont des fractions du rush
    affiché (rotation appliquée, recadrages ignorés). Le masque suit donc le
    plan quand son cadrage bouge ou zoome, comme une fenêtre de la page
    Color de Resolve. Une ellipse est celle inscrite dans ce rectangle.
  - `feather` adoucit le bord **vers l'intérieur**, de `0` (bord net) à `1`
    (un fondu du bord jusqu'au centre) : la taille de la forme reste
    l'endroit où l'image finit.
  - Le masque est une couverture que les couleurs ne voient pas, multipliée
    à l'opacité et aux fondus. Il se mélange au fond comme eux : dans le
    signal livré sans gestion couleur, en lumière linéaire avec. Sous
    gestion couleur un bord adouci est donc plus clair que dans Resolve, qui
    mélange les valeurs codées de sa timeline
    ([mesure](../../tools/resolve-composite-probe/README.md)).
  - Chaque rendu évalue la même règle (`ClipMask.h`) : l'export, les
    planches et `read_frame` par un filtre FFmpeg tenu à la règle à 2 sur
    254 près (`clip_mask_tests`), Record dans son shader. Entre Record et
    le fichier, le masque ajoute au plus 0,017 à 0,026 d'écart par pixel
    selon les cas, et 0,079 en S-Log3 géré, où la transformation
    d'affichage amplifie la chrominance 4:2:0 du fichier
    (`monitor_export_parity_tests`).
  - `read_frame` et les planches montrent un plan seul : ce que le masque
    retire y est noir.
  - Refusé sur un plan son ou de sous-titres et sur un plan de motion.
    `describe` l'écrit sous `mask`, seulement sur un plan masqué ; un
    document qui en porte un passe en version 26, qu'un binaire plus ancien
    refuse au lieu de livrer le plan entier. `verify_export` compte les
    images d'un plan masqué comme non vérifiables. L'EDL, OTIO, FCPXML et
    le renvoi vers Resolve ne portent pas le masque et le disent.
  - Pas fait : un masque animé, une forme libre, un bouton dans
    l'Inspecteur, l'incrustation sur fond vert. Rien n'a été vu à l'écran.
- **Flou** (`set_clip_blur`, EFFETS-2026-10 EF3) : un flou gaussien de
  l'image d'un plan vidéo, `blur` étant son écart type (sigma) en pixels de
  la séquence, de `0` (aucun : le flou est retiré) à `200`. En pixels de la
  séquence pour qu'un plan se floute pareil quels que soient la taille de son
  rush et son cadrage ; chaque rendu le reporte dans ses propres pixels (la
  résolution de l'export, celle de Record). L'image est floutée dans son
  propre rectangle, bords prolongés, avant son étalonnage et son opacité :
  `gblur` de FFmpeg à l'export, les feuilles et `read_frame` (`ClipBlur.h`),
  un flou gaussien de Metal Performance Shaders dans Record ; l'écart entre
  les deux est mesuré et borné par `monitor_export_parity_tests`. La fraction
  est réduite (`20/2` est rangé `10/1`). Refusé sur un plan son ou de
  sous-titres, sur un plan de motion (sa composition floute ses propres
  calques) et sur un plan dont le cadrage est animé (une opacité animée est
  acceptée). `describe` l'écrit sous `blur`, seulement sur un plan flouté ;
  un document qui en porte un passe en version 24. `verify_export` compte les
  images d'un plan flouté comme non vérifiables ; l'EDL et le XML de Resolve
  ne l'emportent pas et le disent (`blurs_not_sent`) ; la route
  `AppendToTimeline` le refuse par son nom. `measure_color`, `match_color` et
  `propose_grade` mesurent le plan avant son flou ; `read_frame` et les
  feuilles le montrent flouté.

Les transitions (`add_transition`, `update_transition`, `remove_transition`)
sont de trois types. Sur une piste vidéo, `cross_dissolve`, par défaut,
enjambe le raccord entre deux plans adjacents, et `dip_to_color` (R5) voile
une jonction d'une couleur (`#rrggbb`, noir par défaut) ; sans plan sortant
et avec l'alignement `StartAtCut`, il ouvre la piste, sans plan entrant et
avec `EndAtCut`, il la ferme. Sur une piste son, `cross_fade` (EF2, le type
par défaut sur cette piste) fait se croiser deux sons : chacun est lu au-delà
du raccord dans ses poignées, comme un fondu enchaîné, l'un descend pendant
que l'autre monte, à puissance constante (le « Cross Fade +3 dB » de
Resolve). La lecture, l'export et `transcribe_timeline` l'entendent pareil.
Chaque type est refusé sur l'autre sorte de piste. `alignment` (`Center`, `StartAtCut`, `EndAtCut`) dit comment la
transition chevauche le raccord.

Le look d'une timeline (`set_sequence_lut`, R6) est une LUT `.cube`
appliquée une fois à toute la séquence, après l'étalonnage de chaque plan et
avant la transformation de sortie. Le fichier est copié dans le paquet sous
son SHA-256, et le document n'en garde que l'empreinte : le projet rend la
même image ailleurs. Sous gestion couleur, l'espace attendu se déclare :
`acescct` pour une LUT calculée sur les valeurs log de l'espace de travail,
`rec709` pour une LUT écrite pour un signal Rec.709, refusée si le projet ne
livre pas du Rec.709 SDR ; sans gestion couleur, `space` s'omet. Appelé sans
argument, l'outil retire le look. Une LUT 1D, une taille hors de 2 à 65, un
domaine autre que 0..1 ou une valeur hors de [0, 1] sont refusés par leur
nom.

## Le catalogue par famille

`cutmachine --tools` rend le catalogue, et c'est lui qui fait foi : chaque
outil y a sa description et son schéma. Ce tableau le range par famille et
dit où chaque famille est décrite. Relevé sur le binaire le 25 octobre 2026,
avec CF1 : 202 outils, `backup_project_archive`, `get_video_output` et
`set_video_output` compris.

| Famille | Outils | Décrite dans |
|---|---|---|
| Projet et timelines | `create_project`, `open_project`, `ingest_media`, `describe`, `list_timelines`, `add_timeline`, `clone_timeline`, `rename_timeline`, `remove_timeline` (jamais la dernière), `set_active_timeline`, `update_sequence`, `conform_sequence`, `create_timeline_variants`, `choose_timeline_variant` (le choix entre timelines, au journal du projet) | [Commandes headless](commandes-headless.md), [Séquence](sequence.md), [Format de séquence](format-de-sequence.md) |
| Pistes | `add_track`, `remove_track`, `set_track_lock`, `set_track_sync_lock`, `set_track_output` | cette page |
| Montage des plans | `insert_clip`, `edit_source_range`, `delete_timeline_range`, `paste_clips`, `remove_clip`, `clear_clip`, `clear_clips`, `trim_clip`, `move_clip`, `move_clips`, `split_clip`, `split_at_interval`, `delete_gap`, `detach_audio`, `ripple_trim`, `roll_edit`, `slip_edit`, `slide_edit`, `rate_stretch`, `set_clip_link`, `split_link_groups` | cette page |
| Groupes image/son liés | `move_linked_clips`, `trim_linked_clips`, `shorten_linked_clip`, `split_linked_clips`, `remove_linked_clips`, `clear_linked_clips` | cette page |
| Réglages d'un plan, transitions, look | `set_clip_speed`, `set_clip_framing`, `list_clip_framing`, `set_clip_animation`, `track_subject_framing`, `stabilize_clip`, `set_clip_opacity`, `set_clip_blur`, `set_clip_mask`, `add_transition`, `update_transition`, `remove_transition`, `set_sequence_lut` | cette page |
| Son d'un plan | `set_clip_audio` | [Audio](audio.md) |
| Son séparé | `measure_audio_offset`, `sync_audio` | [Audio](audio.md) |
| Son rendu | `render_sound`, `place_sound_stem` | [Audio](audio.md#rendre-un-stem-sur-les-coupes) |
| Ducking proposé | `propose_ducking` | [Audio](audio.md#proposer-un-ducking-sous-une-voix) |
| Transcription et mots | `transcribe_media`, `transcribe_timeline`, `align_words`, `align_transcript`, `get_timeline_transcript`, `list_speech_blocks`, `inspect_speech_selection`, `list_source_words`, `list_timeline_words`, `locate_source_frame`, `analyze_speakers`, `list_speakers` | [Transcription](transcription.md) |
| Montage par la parole | `create_interview_short`, `remove_words`, `remove_source_ranges`, `list_disfluencies`, `clean_disfluencies`, `analyze_hesitations`, `tighten_pauses`, `trim_boundary_air`, `close_junction_air`, `analyze_speech_onset`, `list_speech_onsets` | [Transcription](transcription.md) |
| Image des rushes | `analyze_shot_quality`, `list_shot_quality`, `list_stable_windows`, `analyze_isolated_frames`, `list_isolated_frames`, `timeline_stats` | [Contrôle qualité](controle-qualite.md), [Export](export.md) |
| Regarder | `read_frame`, `contact_sheet`, `cut_sheet`, `media_sheet`, `inspect_cut`, `review_cuts` | [Regarder](regarder.md) |
| Décrire et chercher des plans | `describe_shots`, `search_shots` | [Décrire les plans](regarder.md#décrire-les-plans-une-fois-les-chercher-ensuite) |
| Couleur | `set_color_management`, `conform_color_management`, `set_media_color_input`, `set_media_white_balance`, `set_clip_effects`, `propose_exposure`, `propose_grade`, `measure_color`, `match_color`, `export_cdl`, `apply_cdl`, `paste_clip_grade` (l'étalonnage d'un plan collé sur d'autres, une seule annulation) | [Mesurer l'étalonnage](mesurer-etalonnage.md), [Séquence](sequence.md) |
| Musique | `analyze_audio_beats`, `list_audio_beats`, `list_tempo_grid`, `list_music_structure`, `list_clip_tempo_beats` | [Export](export.md) |
| Son monté | `analyze_timeline_audio` | [Export](export.md) |
| Légendes | `add_caption_style`, `update_caption_style`, `remove_caption_style`, `set_clip_caption`, `generate_captions`, `translate_captions`, `train_caption_breaks`, `check_caption_legibility`, `import_srt` | [Export](export.md) |
| Marqueurs et chutiers | `add_marker`, `update_marker`, `remove_marker`, `add_bin`, `rename_bin`, `move_bin`, `remove_bin` (vide seulement), `set_media_bin`, `set_timeline_bin` (une timeline, par le journal du projet) | cette page, [Chutiers](chutiers.md) |
| Multicam | `add_multicam_group`, `set_multicam_active_angle`, `split_multicam_group`, `remove_multicam_group`, `list_multicam_groups` | [Commandes headless](commandes-headless.md#multicam) |
| Motion | `list_motion_compositions`, `describe_motion_composition`, `add_motion_composition`, `edit_motion_composition`, `remove_motion_composition`, `humanize_motion_composition`, `list_motion_templates`, `list_fonts`, `add_motion_template`, `save_motion_template`, `render_motion_frame`, `render_motion_video`, `insert_motion_clip`, `place_motion_clip`, `set_clip_motion_props`, `check_motion_safe_area`, `export_motion_after` | [`MOTION_FORMAT.md`](../MOTION_FORMAT.md) |
| After Effects | les quinze `after_*` | [`AFTER_BRIDGE_MVP.md`](../AFTER_BRIDGE_MVP.md) |
| DaVinci Resolve | `import_resolve_timeline`, `export_resolve_xml` | [Resolve](resolve.md), [Export](export.md) |
| EDL CMX 3600 | `export_edl`, `export_edl_markers`, `import_edl_markers` | [EDL et WebVTT](edl.md) |
| OTIO et FCPXML | `export_otio`, `export_fcpxml` | [OTIO et FCPXML](echange.md) |
| Livraison | `check_timeline`, `export_timeline`, `export_timelines`, `verify_export` | [Export](export.md) |
| Journal, interventions, plans | `undo`, `redo`, `begin_intervention`, `end_intervention`, `prepare_edit_plan`, `get_edit_plan`, `apply_edit_plan`, `discard_edit_plan`, `list_changes`, `list_motifs` (et l'argument `motif` de chaque édition, du projet comprise ; `journal: "project"` relit le journal du projet) | [Commandes headless](commandes-headless.md#le-motif-dune-édition) |
| Tâches de fond | `get_task`, `list_tasks`, `cancel_task` | [Commandes headless](commandes-headless.md) |
| Étapes durables et archive | `commit_run_step`, `get_run_step`, `archive_project`, `query_project_archive`, `project_archive_status`, `sync_project_archive`, `import_project_archive`, `backup_project_archive` (la copie du jour de l'archive, DEC44) | [Étapes durables](orchestration-montage.md), [Archive SQL](archive-projets.md) |
| L'app ouverte et le savoir-faire | `get_editor_view`, `capture_editor_window`, `get_video_output`, `set_video_output`, `get_procedure` | [Commandes headless](commandes-headless.md) |
| Génération payante, sur demande | `propose_generation` (gratuit), `generate_media` (dépense, après le oui de la personne) | [Génération payante](generation.md) |
| Voix off en local, sur demande | `design_voice` (une voix d'après une description), `generate_voice_over` (un texte dit par cette voix) ; gratuit, sur la machine | [Génération, la voix off](generation.md#la-voix-off-en-local) |
