# Mesurer l'étalonnage

> Relu contre le code le 3 octobre 2026. L'écart de 0,385 % de swscale est une
> mesure de GRADE-2026-09, pas vérifiée par lecture.

`--measure-color` (MCP : `measure_color`) donne les chiffres qu'un étalonneur
lit sur ses scopes, pour une image de chaque plan demandé (`Start`, `Middle`
ou `End`), telle que l'export livre ce plan seul : étalonnage, look de la
séquence et transformation de sortie compris, sans opacité, fondu, carton ni
plan du dessus. Seule la partie de la source que la séquence montre est
mesurée.

- `display` : luminance aux centiles 2, 50 et 98 en pourcentage, part de
  pixels écrêtés et bouchés, saturation moyenne et au centile 95 (unité de
  `SATAVG` de `signalstats`), et balance des ombres, tons moyens et hautes
  lumières (`warmth`, `green_magenta`) ;
- `scene`, sous gestion couleur : la luminance linéaire de l'espace de
  travail en diaphragmes par rapport au gris 18 %, l'unité de
  `color.exposure` sous ACEScct ;
- `region`, si on la donne (fractions exactes de l'image) : les mêmes chiffres
  sur la zone, plus sa teinte au vectorscope et son écart à la ligne peau.

Pour accorder un plan à un plan de référence, `--match-color` (MCP :
`match_color`) calcule la correction d'exposition, de balance et de
saturation, et rend l'appel `set_clip_effects` qui l'appliquerait, sans rien
modifier : il règle les réglages que le plan a déjà et n'ajoute que ceux qui
manquent. La réponse donne les chiffres avant, après et de la référence, et
l'écart restant. L'étape se passe telle quelle à `prepare_edit_plan`.

Pour tous les plans d'une timeline d'un coup (ou ceux de `clip_ids`), sous
gestion couleur, deux outils MCP proposent sans rien appliquer
(AGENT-2026-09 A16, A20 à A22 ; écrit le 7 octobre d'après le catalogue) :

- `propose_exposure` mesure chaque plan comme `measure_color` et, quand sa
  médiane de scène s'écarte de plus de `tolerance_stops` (0,5 par défaut)
  de `target_stops` (0, le gris moyen), rend sous `steps` une étape
  `set_clip_effects` qui ne bouge que son exposition ; `clips` dit pour
  chacun ce qui a été mesuré et pourquoi il a une étape ou non
  (`proposed`, `within_tolerance`, `no_scene_measure`). Les étapes se
  passent à `prepare_edit_plan`, ou à `set_clip_effects` une par une.
- `propose_grade`, à préférer sur une interview, propose l'exposition et la
  balance, lues là où un étalonneur les lit : sur le plus grand visage de
  l'image (`anchor: "face"`, par défaut, cible +0,7 diaphragme, +1,5 quand
  la séquence sort en HLG), sur une
  zone blanche ou grise nommée (`neutral`), ou sur toute l'image, exposition
  seule (`median`). Un plan sans visage trouvé prend l'exposition que les
  visages trouvés ont laissée au montage, et aucune balance. Une montée
  s'arrête où les blancs de l'image iraient (`highlight_ceiling_stops`,
  raison `limited_by_highlights`) ; en HLG, sans ce plafond nommé, où
  l'image rendue écrêterait plus qu'avant (`highlight_ceiling:
  display_clipping`, GR15). `apply: true` pose toutes les
  propositions ; une scène voulue sombre ou claire se laisse hors de
  `clip_ids`. Le visage trouvé est la plus grande chose qui en a l'air : sur
  un sujet maquillé ou masqué, c'est le maquillage.

Après application, on remesure.

`color.temperature` est une vraie température de couleur : la lumière qui
éclairait la scène, adaptée (Bradford, en lumière linéaire) vers la balance
des blancs à laquelle la caméra a tourné. L'ingest lit cette balance dans les
fichiers Sony (piste `rtmd`) ; `set_media_white_balance` la fixe à la main
ou la relit (`from_camera`) pour un média ingéré avant. À la balance de
tournage, la température est neutre.

La balance de tournage et l'entrée couleur d'un média appartiennent au
projet. Elles sont lues une seule fois dans le fichier, quand le média entre
dans la médiathèque, et par la même sonde quel que soit le chemin : import
dans la fenêtre, `--ingest` et `ingest_media`, `--import-resolve`. Un même
rush entre donc lu de la même façon par n'importe lequel. Ensuite, elles ne
changent que sur demande (`set_media_white_balance`,
`set_media_color_input`) : une reconnexion, ou un ingest qui complète une
entrée d'un projet ancien, les garde. Une `color.temperature` est posée
contre la balance de son média : changer la balance d'un média déjà
étalonné change l'image de tous ses plans, et `propose_grade` est alors à
refaire. Un média ingéré avant que la balance soit lue (septembre 2026) a
une balance inconnue, et 6500 K en tient lieu.

Une correction primaire s'échange au format CDL de l'ASC (pente, offset et
puissance par canal, puis saturation). `--export-cdl` (MCP : `export_cdl`)
replie l'exposition, le contraste, la teinte, les roues et la saturation d'un
plan en une CDL, écrit un `.cc`, `.ccc` ou `.cdl` par OpenColorIO et donne la
table que prend `TimelineItem.SetCDL` dans le scripting de DaVinci Resolve ;
un étage sans forme CDL (température, courbes...) refuse le plan en le
nommant. `--apply-cdl` (MCP : `apply_cdl`) fait l'inverse : la CDL, lue dans
un fichier ou donnée par les chaînes de Resolve, remplace le primaire des
plans (en `color.wheels` et `color.saturation`). L'espace dans lequel la CDL
a été faite se déclare (`--space acescct`...) et doit être celui où les plans
sont étalonnés.

Éprouvé dans Resolve Studio 21.1 le 10 octobre 2026
(GR12) : dans un projet ACEScct, la
table posée par `SetCDL` donne la même image qu'ici, à 0,03 code 10 bits
près. Dans un projet DaVinci YRGB, géré ou non, Resolve calcule la
saturation de la CDL avec les poids 0,21 / 0,70 / 0,09 au lieu de ceux de
Rec.709 : une saturation différente de 1 n'y rend pas la même image (1,8 code
pour 1,1, 9 codes pour 0,5). Depuis le 24 octobre 2026 (DEC30, GR12), la
réponse d'`export_cdl` et de `--export-cdl` le dit : chaque plan dont la
saturation n'est pas 1 porte `resolve_yrgb_gap_codes`, l'écart le plus grand
qu'il aura dans un projet YRGB, en codes 10 bits (|1 − s| × 0,0178 × 1023,
arrondi au dixième), et la réponse porte un `warning` et la liste
`warning_clip_ids`. Le moteur ne sait pas en quelle science couleur le projet
Resolve travaille : l'avertissement vient pour toute saturation différente de
1, quel que soit l'espace de la CDL. Resolve n'a pas de `GetCDL` : les
chaînes se recopient depuis ce qu'on a posé, pas depuis Resolve.

Un étalonnage se copie d'un plan sur d'autres (GR11, 24 octobre 2026) :
`paste_clip_grade` (`from_clip_id`, `clip_ids`), ou dans la fenêtre Couleur ›
Copier l'étalonnage du plan puis Coller l'étalonnage sur la sélection. La pile
entière du plan source remplace celle de chaque plan, en une annulation ; un
clip de motion, un son ou un graphisme sous gestion couleur sont laissés et
nommés.

Pour voir ce qu'on mesure, `read_frame` (MCP) prend `graded: true` avec un
`clip_id` : l'image du plan étalonné, look compris, dans la vue SDR de Record.
Les planches d'un montage le prennent aussi (DEC29, 24 octobre 2026) :
`contact_sheet` et `cut_sheet` avec `graded: true` (`--contact-sheet` et
`--cut-sheet` avec `--graded`) montrent chaque cellule comme `read_frame`
étalonné montre son plan, par la même chaîne, pixel pour pixel ; une planche
de raccords étalonnée juge l'accord des couleurs d'un plan à l'autre.
`media_sheet` reste sans étalonnage : un rush n'en a pas tant qu'un plan ne
le pose pas.

La mesure ne juge rien et ne modifie pas le projet. La source est décodée
comme le moniteur la décode, pas par la conversion YCbCr de swscale, qui lit
les codes 0,385 % trop bas (voir
GRADE-2026-09).
