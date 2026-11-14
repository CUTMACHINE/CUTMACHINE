# Format de composition motion — esquisse v0

> Relu contre le code le 3 octobre 2026, par les noms : les 744 noms cités
> en code (propriétés, opérations, outils, options de commande, chemins) ont
> été cherchés dans `src/`, `tests/` et le catalogue, et les seize qui n'y
> sont pas sont ceux que le texte donne lui-même pour absents ou extérieurs ;
> les types de nœud, les présentations de transition et la règle de version
> ont été relus dans `src/MotionComposition.cc` et `src/Document.h`. Non
> vérifié : chaque phrase autour de ces noms, les mesures citées, le rendu et
> l'export After Effects. Les exemples complets sont exécutés par
> `cutmachine_motion_tests`, qui n'a pas été lancé pour cette relecture.

Esquisse issue de M0 (MOTION-2026-09). **Non figée** : M1 et M1b
l'ont confrontée au code, M2 au rendu ; M3 l'a fait entrer dans le document
(voir « Dans le document »).
Les exemples sont des transpositions neutres de motions réels ; aucun texte,
marque ni police de commanditaire. Chaque exemple complet de ce fichier est
parsé et évalué par `cutmachine_motion_tests` : un exemple qui diverge du
moteur fait échouer la suite.

## Principes

- Une image est une fonction pure de la composition, de ses props et du
  temps. Aucun état, aucune horloge, aucun hasard non semé.
- L'auteur compose ; le moteur mesure. Positions et tailles se déclarent en
  valeurs ou en flexbox ; hauteur d'une ligne, boîte d'un mot, taille d'un
  bloc se **lisent**, elles ne se calculent pas côté appelant.
- Les noms viennent de CSS (mise en page, typographie, `clip-path`,
  `mask-image`) et de Remotion (`interpolate`, `spring`, séquences). Tout
  écart est signalé ici.
- Une composition est une donnée : ni expression, ni script.

## Composition

```json
{
  "width": 1080, "height": 1920, "fps": 25, "duration": 95,
  "props":   { "<nom>": { "type": "…", "default": "…" } },
  "tokens":  { "<table>": { "<clé>": "<valeur>" } },
  "formats": { "<nom>": { "width": 1920, "height": 1080, "props": { "<prop>": "<valeur>" } } },
  "variants":{ "<nom>": { "props": { "<prop>": "<valeur>" } } },
  "root": { "type": "fill", "children": [] }
}
```

Temps en **images entières**, locales à la séquence englobante, sauf les
clés d'`interpolate`, qui peuvent tomber entre deux images (`"5/2"`, MW1) ;
la cadence est rationnelle et la conversion en `RationalTime` exacte ou
refusée.

## Props

Types relevés sur les gabarits réels : `text` (police réglable), `color`,
`number` (`min`, `max`, `unit`), `bool`, `enum` (`values`), `point`, `size`,
`list` (`of`), `group` (présentation seulement). Une valeur hors bornes est
refusée à l'application, jamais bornée en silence.

Évalués depuis M1b : `text`, `number`, `color`, `bool`, `enum` et `list`
(`of` : `text`, `number` ou `color` ; `min`/`max` bornent alors chaque
nombre ; 1 000 éléments au plus). `unit` vaut `px` ou `%` : une prop
numérique à unité se lit comme la longueur CSS correspondante (`50` devient
`"50%"`), ce qui permet de lier une vitesse en pourcentage à `timeScale` ou
une largeur à `width`.

Depuis M3 :

- `point` : `{"x": nombre, "y": nombre}` (défaut `{"x": 0, "y": 0}`) ;
  `size` : `{"width": nombre, "height": nombre}` (défaut à zéro, jamais
  négatif). `min`, `max` et `unit` portent sur chaque composante. Une liaison
  lit une composante, comme JavaScript lit `props.position.x` :
  `{"prop": "position.x"}`. Lier un point entier, ou une composante qu'il
  n'a pas, est refusé (`PropTypeMismatch`) ; un nom de prop ne contient donc
  pas de point.
- `group` : `{"type": "group", "props": ["titre", "date"]}` rassemble des
  props pour qui les édite, comme un groupe de contrôles d'After Effects. Il
  n'a pas de valeur : on ne le lie pas, on ne le pose pas. Chaque prop
  appartient à un groupe au plus.
- « Texte avec police » (M0) : une prop `text` et une prop `enum` des
  familles permises, liée à `fontFamily`. Une police absente reste un refus
  `FontMissing` au rendu.
- Trois refus nommés, quel que soit le chemin (opération, outil MCP, rendu
  avec surcharge) : `UnknownProp` (aucune prop de ce nom), `PropOutOfRange`
  (hors bornes, hors liste), `PropTypeMismatch` (mauvais type, ou valeur
  pour un groupe).

## Valeurs et liaisons

Toute valeur de style peut être (une valeur d'animation aussi, dans les
limites dites sous « Animations ») :

- un littéral : `"#fff"`, `78`, `"45%"` ;
- une prop : `{"prop": "titre"}` ; une composante d'un point ou d'une
  taille : `{"prop": "position.x"}`, `{"prop": "logo.width"}` (M3) ;
- une prop passée par une table : `{"prop": "palette", "map": "charte"}`
  (table de `tokens`) ou `{"prop": "contour", "map": {"true": "6px solid #fff", "false": "none"}}` ;
- un élément de répétition : `{"item": true}` (éventuellement passé par une
  table : `{"item": true, "map": "palette"}`), `{"index": true}` (à partir
  de 0). Refusés hors d'un gabarit de `repeat` ; le plus proche l'emporte.

C'est ce qui remplace les expressions « menu → couleur » et « case →
opacité » : une table, pas du code.

- **Timecode** (MOTION-AFTER-2026-09) : `"content": {"timecode":
  "composition"}` affiche le timecode de l'image, `HH:MM:SS:II` à la cadence
  nominale (25 pour 25 i/s, 30 pour 30000/1001, jamais en drop-frame) ;
  `"local"` compte depuis le début de la séquence qui contient le texte, et
  `"offset"` ajoute des images : un entier de −2⁵³ à 2⁵³, ce que
  l'expression d'After tient exactement (MA7 : au-delà, la somme débordait
  dans le moteur, et un `offset` de 2⁶³ − 1 lisait 00:00:00:00 dès
  l'image 1). Une image qui tombe avant 0 se lit 00:00:00:00 ; les heures
  ne s'arrêtent pas à 99. La cadence nominale est la cadence arrondie à
  l'entier supérieur. Les instants d'une image floutée montrent son
  timecode. Seul le contenu d'un texte l'accepte. Vers After Effects, c'est
  une expression du texte source.

## Nœuds

| Type | Rôle | Champs propres |
|---|---|---|
| `fill` | couche plein cadre (`AbsoluteFill` : position absolue sur tout le parent, colonne flexbox) | mêmes styles que `box` |
| `box` | conteneur flexbox (direction `row` par défaut, comme en CSS) | style CSS : `flexDirection`, `justifyContent`, `alignItems`, `alignSelf`, `alignContent`, `flexWrap`, `flexGrow`, `flexShrink`, `flexBasis`, `gap`, `rowGap`, `columnGap`, `padding`, `margin`, `width`/`height` et bornes (px, %, `auto`), `position: absolute` + `top/right/bottom/left/inset`, `backgroundColor`, `borderRadius` ; M1b : `clipPath` ; REM2 : `overflow` ; REM3 : `backgroundImage`, `maskImage`, `filter` ; REM4 : `perspective`, `rotateX`, `rotateY` ; MW5 : `transformOrigin`, `skewX`, `skewY` ; plus tard : `border` |
| `text` | texte mesuré par le moteur | `content` ; style `fontFamily`, `fontWeight`, `fontStyle` (REM3), `fontVariantNumeric` (MM2), `fontSize`, `lineHeight`, `letterSpacing`, `wordSpacing`, `textTransform`, `textAlign`, `color`, `textStroke` ; `units`, `ranges`, `stagger`, `highlights` (MB3) ; plus tard : `boxDecoration` |
| `image` | image du paquet ou d'un fichier | `src`, `objectFit` |
| `video` | images (et son, sauf `muted`) d'un média, REM2 | `media` (ULID d'un média du projet) ou `src` (fichier), `startFrom`, `endAt`, `playbackRate`, `muted` ; style `objectFit`, `volume` |
| `audio` | son seul d'un média, sans boîte, REM2 | `media` ou `src`, `startFrom`, `endAt`, `playbackRate` ; style `volume` seulement |
| `composition` | instance d'une autre composition du projet, REM2 | `composition` (ULID), `props` (valeurs ou liaisons) ; style `objectFit` |
| `path` | forme vectorielle | `d` (données SVG, littéral ou liaison) ou `points` (`[[x, y], …]`) + `curve` (`linear`, `catmull-rom`) + `closed` ; `viewBox` ; style `fill`, `stroke`, `strokeWidth`, `strokeLinecap`, `strokeLinejoin`, `fillRule`, animables `trimStart`, `trimEnd` ; plus tard : `marker` |
| `sequence` | fenêtre de temps locale ; se met en page comme un `fill`, comme `Sequence` chez Remotion | `from`, `duration`, `timeScale` ; dans une `series` : `offset` au lieu de `from` |
| `series` | séquences bout à bout, comme `Series` chez Remotion ; se met en page comme un `fill` | enfants `sequence`, chacun avec `duration` sauf le dernier, et `transition` entre deux d'entre elles (REM3) |
| `transition` | passage d'une séquence d'une série à la suivante, REM3 | `presentation` (`fade`, `slide`, `wipe`, `iris`), `direction` (glissé, volet), `timing` (`duration` et `easing`, ou `spring`) |
| `repeat` | N copies d'un gabarit, à la place du `repeat` parmi ses frères | `count` ou `each`, `template`, `cycle`, `stagger` ; ni `id`, ni `style`, ni `animate` : il n'a pas de boîte |

Commun à tous (sauf `repeat`) : `id`, `visible`, `style`, `animate`, et
depuis M5b (7 novembre 2026) `decor` : `true` sur un nœud dessiné mais que
le contrôle de zone ne juge pas, ni ses descendants — les échos d'un
bandeau que le cadre coupe exprès, un mur de mots, une texture. Un
booléen littéral, jamais une liaison. Le rapport du contrôle liste ce
qu'il a laissé de côté (`decor`), rien n'est tu. Depuis
M3, tous, `repeat` compris, acceptent `uid` : l'identité persistante du
nœud dans le document (voir « Dans le document »). `id` reste le nom que
l'auteur donne au nœud ; `uid` est l'adresse que le moteur lui donne.

`visible: false` retire le nœud et sa descendance de la mise en page, comme
`display: none` (écart de nom assumé : c'est la cible de liaison la plus
fréquente des gabarits réels). Pour masquer sans libérer la place, animer
`opacity`.

Les propriétés typographiques (`fontFamily`, `fontWeight`, `fontSize`,
`lineHeight`, `letterSpacing`, `textTransform`, `textAlign`, `color`)
s'héritent des ancêtres comme en CSS, de même que `wordSpacing` et
`textStroke` ; `fill`, `stroke`, `strokeWidth`, `strokeLinecap`,
`strokeLinejoin` et `fillRule` s'héritent comme en SVG. Unités, comme en CSS et en React :
un nombre est en pixels, sauf `lineHeight` (multiplicateur du corps ;
`"72px"` pour des pixels), `opacity`, `flexGrow`, `flexShrink`, `scale` et
`fontWeight`. `rotate` est en degrés.

`fontFamily` nomme une famille que Core Text trouve : installée, active
(Adobe Fonts compris, tant que Creative Cloud tourne) ou enregistrée par
`--font-dir` / `font_dirs`. Une autre est refusée (`FontMissing`), jamais
remplacée. `cutmachine --list-fonts` et l'outil `list_fonts` donnent ces
familles (AX-2026-09 AX6). Depuis le 9 octobre 2026, un sous-titre trouve
les mêmes familles que la motion : il est dessiné par le même moteur
(LEGENDES-2026-10) ; une famille introuvable y est remplacée par Helvetica
Neue, et l'export le dit.

`fontWeight` choisit la face comme CSS (Fonts 4) : la graisse demandée si
une face l'a, sinon la plus proche dans l'ordre de CSS. Une face statique a
une graisse, celle de sa table OS/2. Une police variable couvre son axe
`wght` : elle est dessinée à la graisse demandée, bornée aux extrémités de
l'axe, entre ses instances nommées comme sur elles (MOTION-WEB-2026-10
MW7). Un sous-titre n'a pas de graisse : il prend 400, la face régulière de
sa famille, variable ou non.

Une propriété que le format ne connaît pas est refusée
(`UnknownStyleProperty`), jamais ignorée. Pour les réflexes CSS les plus
courants, le refus dit quoi écrire à la place (MB6) :

| CSS | Ici |
|---|---|
| `transform`, `translate` | des propriétés à part : `translateX`, `translateY`, `scale`, `scaleX`, `scaleY`, `rotate`, `rotateX`, `rotateY`, `skewX`, `skewY`, autour de `transformOrigin` |
| `skew` | `skewX` et `skewY`, en degrés |
| `white-space`, `text-wrap` | un texte passe à la ligne à la largeur de sa boîte ; pour garder une ligne entière, une boîte plus large que la ligne, `flexShrink: 0` |
| `box-shadow`, `text-shadow` | `filter` : `drop-shadow(x y flou couleur)` ou `glow(rayon couleur force)` |
| `display`, `visibility` | toute boîte est un conteneur flex ; `visible` masque en libérant la place, `opacity` en la gardant |
| `z-index` | les nœuds se peignent dans l'ordre de `children` |
| `transition`, `animation` | `animate` (`interpolate` ou `spring`) |
| `border`, `outline` | pas de bordure : deux boîtes imbriquées (le `padding` de l'extérieure fait la bordure), ou un `path` avec `stroke` |
| `background` | `backgroundColor`, ou `backgroundImage` pour un dégradé |
| `text-decoration` | un trait dessiné sous le texte (boîte ou `path`) |
| un nom à tiret (`background-color`) | le même en camelCase (`backgroundColor`) |

## Animations

```json
"animate": {
  "scale":   { "interpolate": { "frames": [2, 5], "values": [0, 1.2], "easing": "ease-out", "extrapolate": "clamp" } },
  "translateY": { "spring": { "from": 30, "to": 0, "delay": 4, "mass": 1, "damping": 10, "stiffness": 100, "overshootClamping": false } }
}
```

- `interpolate` : un ou plusieurs points (images strictement croissantes ;
  une clé seule tient sa valeur, REM5),
  et depuis M3 `uids`, l'identité de chaque clé dans l'ordre des `frames`,
  sorties numériques, couleurs (propriétés de couleur, `fill`, `stroke`) ou
  polygones (`clipPath`). `easing` :
  `linear`, `ease`, `ease-in`, `ease-out`, `ease-in-out`, `step-end`,
  `cubic-bezier(x1, y1, x2, y2)` ou une courbe GSAP (plus bas). Un nom seul vaut pour **chaque segment**
  entre deux points ; une liste donne **une courbe par segment** (MB5), dans
  l'ordre, une de moins que de points :
  `"frames": [0, 8, 60, 66], "easing": ["cubic-bezier(0.16, 1, 0.3, 1)", "linear", "cubic-bezier(0.7, 0, 0.84, 0)"]`
  est une entrée qui se pose, une tenue, une sortie qui accélère, dans la
  même animation. Une liste de la mauvaise longueur est refusée
  (`InvalidAnimation`, avec le compte attendu) ; une liste qui répète la
  même courbe est exactement l'animation que son nom seul donne.
  `extrapolateLeft` / `extrapolateRight` : `clamp` (défaut) ou
  `extend`, qui prolonge la droite des deux points extrêmes sans la courbe.
- **Courbes GSAP** (MOTION-WEB-2026-10 MW3) : `easing` lit aussi les noms
  de GSAP, seuls ou dans une liste par segment : `power1` à `power4` (et
  leurs alias `quad`, `cubic`, `quart`, `quint`, `strong`), `sine`, `expo`,
  `circ` et `back`, suivis de `.in`, `.out` ou `.inOut` ; un nom seul vaut
  `.out`, comme dans GSAP, et `back` prend son dépassement entre parenthèses
  (`"back.out(1.7)"`, de 0 à 10). Le moteur joue la formule de GSAP 3.14,
  pas une Bézier : `power3.out` à mi-segment vaut 0,9375. `power1` et
  `power2` (`in`, `out`) et `back` (`in`, `out`) sont des Bézier exactes ;
  les autres partent vers After comme la `cubic-bezier` la plus proche, et
  le bilan nomme chacune avec son écart (0,05 % pour `power3.out`, 4 % pour
  `circ.inOut`). `elastic`, `bounce` et `steps` ne sont pas lus.
- **Clé entre deux images** (MOTION-WEB-2026-10 MW1) : une image de
  `frames` est un entier ou une fraction exacte écrite `"n/d"` (réduite,
  `d > 1`) : `"frames": [0, "5/2", 10]`. Une animation venue du web est
  minutée en secondes décimales, et 0,1 s à 25 i/s tombe à l'image 5/2 :
  l'arrondir déplacerait la clé sans le dire. Le moteur l'évalue au temps
  exact (1/10 s), y compris à `30000/1001`. Un décimal (`2.5`), une fraction
  non réduite (`"4/2"`) ou entière (`"5/1"`) sont refusés
  (`InvalidAnimation`) : une seule écriture par instant. Le canvas place une
  clé ajoutée à sa vraie position et n'adresse que les clés posées sur une
  image ; l'export After la pose à son instant (MW2, voir « Vers After
  Effects ») ; `humanize` laisse telles quelles les pièces qui en ont.
- `spring` : `from`, `to`, `delay` (images), `mass`, `damping`, `stiffness`,
  `overshootClamping`, défauts de Remotion (1, 10, 100). Évalué en forme
  analytique : même valeur au même instant quelle que soit la cadence.
- Une valeur animée hors domaine est ramenée comme le fait CSS au calcul
  (opacité dans [0, 1], tailles négatives à 0) ; une valeur **littérale ou
  liée** hors domaine est refusée.
- **Valeur liée** (MB11) : une valeur de `values` se lie à une prop,
  directement (`{"prop": "chute"}`) ou par une table
  (`{"prop": "apparition", "map": {"true": "inset(0% 100% 0% 0%)", "false": "inset(0% 0% 0% 0%)"}}`),
  pour un nombre, une couleur ou une découpe `clipPath`. Chaque instance
  joue alors ses propres clés : une pièce qui n'entre que si on le lui
  demande n'a plus besoin d'une version par cas. Ce qui reste écrit :
  `frames`, `easing`, un `spring`, les valeurs d'un dégradé, d'un `filter`
  ou d'un tracé, et un élément de répétition (`item`, `index`), refusé ici.
  Le parseur juge la valeur par défaut de la prop et chaque valeur que sa
  table peut donner : un nombre pour un nombre, une couleur pour une
  couleur, une découpe de même forme et de même nombre de sommets que la
  clé écrite pour une découpe ; une prop sans table est jugée par sa
  valeur par défaut, puis à l'évaluation par celle de l'instance. Un
  nombre lié est pris tel quel, sans conversion d'unité. L'export After
  écrit les clés de l'instance qu'il exporte ; `humanize` et le canvas
  laissent la liaison telle qu'elle est écrite.
- Propriétés animables : transformations (`translateX/Y`, `scale`,
  `scaleX/Y`, `rotate`, puis `rotateX/Y`), `opacity`, tout style
  numérique (`letterSpacing`, `wordSpacing`, `strokeWidth`, `trimStart`,
  `trimEnd`…), toute couleur (`color`, `backgroundColor`, `fill`,
  `stroke`) et `clipPath`. Un `spring` n'anime qu'un nombre.
- Couleurs : interpolées en sRGB à alpha prémultiplié, comme CSS pour les
  couleurs historiques — à mi-chemin de `transparent` à `#ff0000`, on
  obtient un rouge à demi transparent, jamais un rouge sombre.
- Polygones : chaque sommet est résolu contre la boîte, puis déplacé en
  ligne droite ; px et % peuvent se mêler d'une clé à l'autre. Tous les
  polygones d'une interpolation ont **le même nombre de sommets**, sinon
  la composition est refusée (`InvalidAnimation`) : pas de morphing
  inventé.

## Vitesse et séries

`timeScale` sur une `sequence` : un nombre (multiplicateur, `0.5`), un
pourcentage (`"50%"`) ou une liaison (une prop numérique à `"unit": "%"`
se lit en pourcentage). Strictement positif, au plus 1 000. Le temps local
de la séquence vaut `(temps parent − from) × timeScale`, **en fraction
exacte** : à 125 %, l'image 3 est l'image locale 15/4, jamais 3,75
arrondie. `from` et `duration` restent en images du parent.

`series` : chaque séquence commence où finit la précédente ; `offset`
(entier, négatif pour chevaucher) décale cette séquence et toutes les
suivantes, comme chez Remotion. Une séquence qui commencerait avant la
série est refusée.

## Unités de texte

```json
{ "type": "text", "content": {"prop": "texte"}, "units": "word",
  "ranges":  [{ "unit": "word", "start": {"prop": "premier"}, "count": {"prop": "combien"}, "style": {"color": {"prop": "accent"}} }],
  "animate": { "opacity": { "interpolate": { "frames": [0, 6], "values": [0, 1] } } },
  "stagger": { "each": 3, "order": "forward" } }
```

`units` découpe en `line`, `word` ou `char` après mise en page ; `animate`
s'applique alors à chaque unité, décalée par `stagger` (`forward`, `reverse`,
`random` à graine). Couvre machine à écrire, mots qui apparaissent, plages
colorées.

Règles retenues en M1b :

- Le découpage part des grappes que place le mesureur (Core Text en rendu) :
  un mot est une suite de grappes sans espace, un caractère une grappe qui
  n'est pas un espace. Les espaces (y compris insécables) ne sont pas des
  unités : « projet ? » fait deux mots. La boîte d'une unité a la hauteur
  de sa ligne et la largeur de son avance, crénage et interlettrage compris.
- Avec `units`, `animate` porte **sur chaque unité** et ne peut viser que ce
  qui ne change pas la mise en page : `opacity`, `translateX/Y`, `scale`,
  `scaleX/Y`, `rotate`, `color`, `backgroundColor`. Transformations autour
  du centre de l'unité. Pour animer le bloc entier ou sa typographie
  (`letterSpacing`, `wordSpacing`…), animer une boîte englobante : ces
  propriétés s'héritent. De même pour un filtre sur tout le texte (un
  texte en cascade qui glitche) : `filter` sur la boîte englobante, animé
  ou non (MA5, décidé le 29 septembre 2026 : `animate.filter` du texte
  lui-même reste réservé à un filtre par unité).
- `stagger` : `each` (images entières ≥ 0), `order` (`forward` par défaut,
  `reverse`, `random`), `seed` obligatoire pour `random` (entier ≥ 0). L'unité
  de rang r démarre `r × each` images plus tard. L'aléatoire est une
  permutation fixée par la graine (SplitMix64 et Fisher-Yates, identiques
  sur toute machine).
- `ranges` : `unit`, `start` (à partir de 0), `count` (facultatif : jusqu'à
  la fin), `style` limité à `color`, `backgroundColor` et `opacity`.
  `start` et `count` sont des entiers ≥ 0, littéraux ou liés ; une plage qui
  dépasse le texte s'arrête à sa fin. Les plages s'appliquent dans l'ordre,
  la dernière l'emporte, puis l'animation de l'unité. Une plage ne peut pas
  être plus fine que `units` (des mots animés ne se colorient pas lettre par
  lettre) ; sans `units`, le texte est peint à la granularité de sa plage la
  plus fine, sans cascade.
- Un texte découpé en caractères est composé sans ligatures facultatives
  (« fi » reste deux glyphes), pour que chaque caractère se dessine seul à
  sa place.

## Le surligneur (MB3)

```json
{ "type": "text", "content": {"prop": "texte"},
  "highlights": [{ "unit": "word", "start": 7, "count": 4, "color": "#ffe14d",
                   "top": 0.52, "bottom": 0.92, "bleed": 10,
                   "progress": { "interpolate": { "frames": [8, 30], "values": [0, 1],
                                                  "easing": "power2.inOut" } } }] }
```

Un trait de feutre sous une plage d'unités, fait le 24 octobre 2026
(MOTION-BIBLIOTHEQUE-2026-10, DEC7). Une plage (`ranges`) peint son fond
unité par unité, une boîte dure par mot, les espaces nus ; un surlignage
peint **une bande par ligne**, du bord gauche de la première unité couverte
au bord droit de la dernière, **espaces compris**, prise dans les boîtes
que la mise en page donne aux unités : aucune position ne s'écrit à la main,
et le trait suit le texte quand il change ou passe à la ligne.

- `unit` (`line`, `word`, `char`), `start` (à partir de 0) et `count`
  (facultatif : jusqu'à la fin) comme une plage, littéraux ou liés ; une
  plage qui dépasse le texte s'arrête à sa fin, une plage vide ne peint
  rien. Une ligne couverte s'étend de son premier à son dernier mot.
- `color` : obligatoire, littérale ou liée.
- `top` et `bottom` : la bande, en fraction de la hauteur de la boîte de
  ligne (0 et 1 par défaut : toute la ligne ; 0,5 et 0,95 : la moitié
  basse, un trait de feutre). Littéraux, `top` sous `bottom`.
- `bleed` : pixels de débord au-delà de chaque bout de bande (0 à 1 000,
  0 par défaut).
- `progress` : la part de la longueur totale peinte, de 0 à 1 (1 par
  défaut), un nombre, une liaison ou une animation (`interpolate` ou
  `spring`, lue à l'image locale du texte, bornée à [0, 1]). Elle se
  répartit dans l'ordre de lecture : la première ligne se remplit de gauche
  à droite, puis la suivante.
- Peinte sous les glyphes, au-dessus du fond de la boîte du texte, dans sa
  transformation. Un surlignage ne découpe pas le texte en unités et ne
  change pas son dessin : c'est pourquoi il n'est pas une clé de `ranges`.
- La géométrie (`--describe-motion-frame`) donne `highlights` dans le texte :
  pour chaque bande, son surlignage, sa ligne, sa boîte (relative au texte)
  et la largeur peinte (`painted`).
- **L'export After le traduit** : une précomposition par texte surligné,
  qui tient le texte éditable et un rectangle éditable par bande, sous les
  glyphes. Les bandes suivent les boîtes du moteur ; leur largeur avance
  ligne après ligne, par les courbes exactes quand elles redonnent chaque
  image, sinon par une clé par image, annoncée au bilan. L'opacité, les
  transformations et la découpe s'appliquent ensemble au texte et au trait.
  Sous obturateur, After lit la progression à l'instant de l'image au lieu
  de la moyenner : le bilan le dit. Le contrôle de zone de sécurité ne
  compte pas les bandes.

## Lire la géométrie

Le moteur expose, pour un instant donné, la boîte de chaque nœud et de chaque
unité de texte (`--describe-motion-frame`) : pour un texte, `lines`, `words`
(avec leur ligne) et `chars` (avec leur ligne et leur mot), boîtes relatives
au texte ; s'il est peint par unités, `units` donne l'état de chacune
(retard, opacité, transformation, couleur, fond). Un tracé expose
`pathPaint` : longueur totale, bornes du tracé dessiné (après `viewBox` et
trim, contour exclu, comme `getBBox`), trims et peinture résolus. Ce champ
s'appelait `path` jusqu'au 24 octobre 2026, comme l'adresse du nœud dans
l'arbre, que tout nœud porte en premier : un tracé écrivait deux fois la même
clé (REMOTION-PARITY-2026-09 REM6). Un `clipPath` est exposé en sommets
résolus, une séquence par son image locale (`localFrame`).

Un nœud pourra s'y ancrer :

```json
{ "type": "box", "style": {"height": 3, "backgroundColor": "#fff"},
  "layoutFrom": { "node": "q", "unit": "word", "range": [5, 7], "edge": "bottom" } }
```

Seul cas du corpus (un soulignement qui suit des mots) : primitive la plus
avancée, repoussée après M1b. Non évalué.

## Tracés

Un `path` sans `viewBox` est dessiné dans les pixels du nœud, depuis son coin
haut-gauche : sa taille propre va jusqu'au plus loin qu'il atteint. Avec
`viewBox` (`"min-x min-y largeur hauteur"`), il est mis à l'échelle dans sa
boîte comme en SVG (`xMidYMid meet`), épaisseur de contour comprise ; une
seule dimension donnée garde les proportions du `viewBox`.

- `d` : commandes SVG `M L H V C S Q T A Z`, absolues et relatives ;
  quadratiques et arcs deviennent des cubiques. Une donnée mal formée est
  refusée avec sa position (`InvalidPath`).
- `points` + `curve: "catmull-rom"` passe par chaque point.
- Peinture SVG : `fill` noir par défaut (comme SVG : écrire `"none"` pour un
  trait seul), `stroke` absent par défaut, `strokeWidth` 1, `currentColor`
  accepté. Remplissage puis contour.
- `trimStart` / `trimEnd` (0 à 1) gardent la portion du tracé entre ces
  fractions de sa longueur totale, sous-chemins mis bout à bout ; inversés,
  ils sont permutés ; le trim s'applique au remplissage comme au contour
  (comme les « trim paths » d'un logiciel de motion ; écart assumé avec SVG,
  qui ne connaît que `stroke-dasharray`). La longueur des courbes est
  mesurée sur 64 cordes fixes : même tracé, même résultat partout.

## Découpes

`clipPath: "polygon(x y, …)"` (au moins trois sommets, px ou % de la boîte
du nœud, comme `clip-path` sur la `border-box`) découpe le nœud et sa
descendance, dans son repère : la découpe suit ses transformations.
`"none"` l'annule. `circle()`, `ellipse()` et `inset()` sont arrivées avec
REM3 (« Formes de découpe », plus bas) ; les autres formes CSS (`path()`,
`xywh()`, `rect()`) sont refusées.

## Médias et précompositions (REM2)

REMOTION-PARITY-2026-09, REM2. Une composition joue les médias du projet et
en montre d'autres, sans quitter le document : tout se désigne par ULID et
se juge contre le projet.

### Vidéo et son

```json
{ "type": "video", "media": "<ULID d'un média>", "startFrom": 10, "endAt": 30,
  "playbackRate": "50%", "muted": true,
  "style": { "width": "100%", "height": "100%", "objectFit": "cover", "borderRadius": 24 } }
```

- **Source** : exactement un de `media` (un média du projet, par ULID, qui
  suit un relink) ou `src` (un fichier, résolu comme une image : relatif au
  paquet ou au dossier de la composition). Tous deux acceptent une liaison.
- **Temps** : à l'image locale `t` du nœud, le média est au temps
  `(startFrom + t × playbackRate) / fps`, fraction exacte, où `fps` est la
  cadence de la composition qui tient le nœud. `startFrom` et `endAt` sont
  des images de cette cadence, comme chez Remotion ; `playbackRate` s'écrit
  comme `timeScale`. L'image montrée est celle du média qui **contient** ce
  temps, comptée à sa propre cadence depuis sa première image : la règle de
  tout clip de timeline, donc la même image qu'en montage.
  `startFrom` et `endAt` s'écrivent ou se lient à une prop, directement ou
  par une table (MB11) : `{"prop": "debut"}` fait commencer chaque instance
  à son propre point du média. La valeur, écrite ou liée, est un nombre
  entier d'images ≥ 0 et `endAt` vient après `startFrom` ; le parseur le
  vérifie pour la valeur par défaut, l'évaluation pour celle de l'instance.
  Le `from` d'une `sequence`, lui, reste écrit.
- **Fenêtre** : avant `t = 0`, après `endAt` ou après la fin du média, le
  nœud ne montre ni ne joue rien (`state` vaut `before` ou `after`) mais
  garde sa boîte. Pas de dernière image tenue : ni un média ni Remotion
  n'en ont la règle.
- **Taille** : celle des images du média, rotation du conteneur appliquée
  (un téléphone tenu droit reste droit), gardée en proportion comme une
  image. `borderRadius` découpe désormais l'image d'un `image` comme d'un
  `video` (CSS découpe le contenu d'un élément remplacé ; M2 n'arrondissait
  que le fond), et `overflow: "hidden"` découpe les descendants d'une boîte
  à sa boîte arrondie.
- **Orientation d'une image** (REM6) : une photo enregistrée sur le côté
  avec une orientation EXIF est mesurée et peinte droite, comme la montre
  un navigateur (`image-orientation: from-image`) ; sa taille naturelle,
  celle que prend `objectFit`, est la taille montrée.
- **Couleur** : un média est lu dans son signal, converti par sa propre
  matrice (BT.709 sauf mention contraire, BT.601 en définition standard) et
  sa plage, comme l'export le passe sans gestion couleur. Un rush log reste
  plat dans une composition : l'étalonnage d'un média ne s'y applique pas.
  Un ProRes 4444 avec alpha garde son alpha.
- **Son** : un `video` qui n'est pas `muted`, et tout `audio`, jouent leur
  son. `volume` est un gain linéaire de 0 à 4 (1 : tel qu'enregistré, 4 :
  +12 dB), animable : un fondu est une interpolation de `volume`. Le moteur
  mixe lui-même, à 48 kHz stéréo : chaque son commence au premier
  échantillon à ou après son premier instant, son gain est évalué au temps
  exact de chaque échantillon, les sons s'additionnent sans limiteur. Un
  son à une autre vitesse que l'enregistrement (`playbackRate`, ou un
  `timeScale` englobant) est refusé (`UnsupportedAudioRate`) plutôt que
  rééchantillonné vers une hauteur que personne n'a choisie : `muted` ou
  `volume: 0` gardent l'image sans le son.
- **Refus** : `MediaMissing` (fichier illisible ou média inconnu au rendu),
  `MediaHasNoVideo` (un son seul dans un `video`), `MediaHasNoAudio` (un
  `audio` sans son), `UnsupportedAudioRate`, et dans le document
  `UnknownMedia` pour un ULID que le projet n'a pas.

Le descripteur donne, pour chaque nœud média, sa source telle qu'écrite,
son état, son temps en fraction de secondes et, pour une vidéo, l'image
montrée : `"video":{"media":"…","state":"playing","time":"6/5","frame":30,…}`.

### Précomposition

```json
{ "type": "composition", "composition": "<ULID d'une composition>",
  "props": { "name": "Ana", "color": { "prop": "accent" } },
  "style": { "width": 240 } }
```

- L'instance a la taille du canevas de l'autre composition, gardée en
  proportion comme une image ; ce canevas est posé dans sa boîte par
  `objectFit` (`fill` par défaut) et découpé à ses bords, comme un précomp.
- Ses `props` sont des valeurs ou des liaisons de la composition qui la
  tient : chacune est résolue là, puis jugée contre les props typées de
  l'autre (`UnknownProp`, `PropTypeMismatch`, `PropOutOfRange`, au chemin
  `…props.<nom>`).
- **Temps** : même instant, l'autre cadence, en fraction exacte (une
  composition à 50 i/s sous une séquence à 25 est à l'image 34 quand son
  parent est à 17). Hors de sa durée, rien n'est montré (`before`,
  `after`).
- Le descripteur écrit l'instance (`"composition":{"id","state","frame",
  "canvas","scale","origin","root"}`) et son arbre **dans les pixels de son
  canevas** : ses nœuds gardent leurs `uid`, qu'une instance ou deux les
  montrent ; l'adresse d'un nœud montré est le couple (uid de l'instance,
  uid du nœud).
- **Cycle** : une composition qui se montre elle-même, directement ou par
  ses instances, est refusée (`CompositionCycle`) par le document, et par
  l'évaluation d'une composition lue d'un fichier (huit niveaux au plus).

### Dans le document, sur les surfaces

- Aucune opération ni version nouvelles : le vocabulaire grandit dans la
  définition (voir « Décisions de M3 »), et un binaire antérieur refuse
  `video`, `audio` ou `composition` au lieu de les ignorer.
  `Document::Validate` juge ce qu'une composition nomme du projet : médias
  (`UnknownMedia`), compositions montrées (`UnknownComposition`), valeurs
  littérales des instances, cycles. Une composition qu'une instance montre
  ne se retire pas, comme celle qu'un clip montre.
- CLI (`--describe-motion-frame`, `--render-motion-frame`,
  `--render-motion-video` sur un projet), MCP (`render_motion_frame`,
  `render_motion_video`, contrôles de zone de sécurité), moniteur Record et
  export évaluent avec la même portée : les compositions montrées, parsées,
  et les médias du projet par ULID. La clé du cache du moniteur inclut les
  définitions montrées et les chemins des médias joués : éditer un précomp
  ou relinker un média invalide l'image.
- `--render-motion-video` et `render_motion_video` écrivent le son d'une
  composition qui en joue (PCM 24 bits dans un `.mov`, AAC dans un `.mp4` ;
  le rapport le dit par `"audio"`). À l'export d'une timeline, le son d'un
  clip de motion est le mixage de la partie qu'il montre, posé où il
  commence, avec les fondus de 3 ms de tout clip du mix ; à la lecture, le
  même mixage entre dans le plan temps réel. Une piste vidéo masquée ne
  joue pas plus le son de ses clips de motion qu'elle ne les exporte.
- Limite connue : la transcription d'une timeline (`transcribe_timeline`)
  n'entend pas encore le son des compositions.

## Peinture, masques, effets et transitions (REM3)

REMOTION-PARITY-2026-09, REM3. Ce que le corpus demande de CSS et de
Remotion au-delà des boîtes : dégradés, formes de découpe, masques, une
pile d'effets raster, des transitions entre scènes, et l'italique.

### Dégradés et masques

`backgroundImage` peint un dégradé sous le contenu, par-dessus
`backgroundColor`, dans la boîte arrondie ; `maskImage` multiplie tout ce
que le nœud peint (lui et sa descendance) par la couverture d'un dégradé,
et rien ne se montre hors de sa boîte.

```json
"style": { "backgroundImage": "linear-gradient(135deg, #E4572E 0%, #3D5AFE 100%)",
           "maskImage": "linear-gradient(to right, #000, transparent)" }
```

- `linear-gradient([<n>deg | to <côté> [<côté>]], <couleur> [<n>%], …)` :
  l'angle part du haut dans le sens horaire (`180deg` par défaut) ; un coin
  (`to top right`) dépend des proportions de la boîte, comme en CSS. La
  ligne du dégradé passe par le centre et mesure `|L sin a| + |H cos a|`.
- `radial-gradient([circle | ellipse] [closest-side | closest-corner |
  farthest-side | farthest-corner] [at <x> <y>], <couleur> [<n>%], …)` :
  ellipse `farthest-corner` au centre par défaut.
- Arrêts : couleurs du format, position facultative en %, complétées comme
  CSS (premier à 0, dernier à 100 %, les autres répartis, jamais avant le
  précédent). Couleurs mélangées en sRGB **prémultiplié**, comme CSS : de
  `transparent` au rouge, le milieu est un rouge translucide.
- Animables : deux dégradés de même forme (même type, même angle ou même
  coin, autant d'arrêts) se mélangent arrêt par arrêt ; un dégradé linéaire
  **tourne** (l'angle est mélangé), un radial déplace son centre et ses
  rayons. Autrement : `InvalidAnimation`.
- Le descripteur donne le dégradé résolu : ligne (`"line":[x0,y0,x1,y1]`)
  ou centre et rayons, arrêts à leur position.

### Formes de découpe

`clipPath` prend, outre `polygon()` (M1b), `circle(<r> at <x> <y>)`,
`ellipse(<rx> <ry> at <x> <y>)` et `inset(<haut> [<droite> <bas> <gauche>]
[round <r>])`. Un rayon est une longueur, un pourcentage (de la diagonale
de la boîte sur racine de deux pour un cercle, de sa largeur ou hauteur
pour une ellipse) ou `closest-side` / `farthest-side` (par défaut). Une
interpolation ne mélange que des formes de même nature, sommet par sommet,
centre et rayons ; passer d'un cercle à un polygone se fait entre deux
séquences, pas par un morphing inventé. Le descripteur écrit un polygone
comme avant et les autres formes par leur nom : `{"circle":[cx,cy,r]}`.

### Effets raster

```json
"style": { "filter": "drop-shadow(0px 6px 4px rgba(0,0,0,0.6)) blur(2px) glow(10px #FFD23F 2)" }
```

Une pile, appliquée dans l'ordre à ce que le nœud peint (lui et sa
descendance), puis découpée par `clipPath`, masquée, rendue transparente :
l'ordre de CSS (filtre, découpe, masque, opacité).

- `blur(<σ>px)` : flou gaussien d'écart type σ, celui de `feGaussianBlur`
  en SVG (trois flous en boîte aux tailles de la spécification dès 2 px,
  le noyau lui-même en dessous).
- `drop-shadow(<dx>px <dy>px [<σ>px] [<couleur>])` : la silhouette du nœud,
  floutée, colorée, décalée, sous lui.
- `glow(<σ>px <couleur> [<force>])` : un halo de la silhouette, sa
  couverture multipliée par la force (0 à 16) et **bornée** à pleine
  couverture : une lueur forte sature, elle ne s'étend jamais au-delà de
  son flou. Pas un nom CSS : CSS n'a pas de lueur.
- `displace(<amplitude>px <longueur d'onde>px <graine> [<phase>])` : chaque
  pixel déplacé par un bruit lisse, **semé** (jamais de hasard non semé),
  la phase animable pour qu'il bouge. Pas un nom CSS non plus.
- Longueurs en pixels du nœud : elles suivent son échelle, et le décalage
  d'une ombre tourne avec lui (effets appliqués dans les pixels du cadre,
  paramètres transformés ; exact pour une échelle uniforme).
- Animable : deux piles des mêmes effets dans le même ordre (même graine
  pour un déplacement) se mélangent paramètre par paramètre.
- Le descripteur donne la pile résolue et son **domaine** : les pixels du
  cadre que les effets peuvent écrire — les boîtes transformées du nœud et
  de sa descendance, plus ce que peint un contour ou un glyphe au-delà
  (2 px, la moitié d'un contour, 15 % du corps d'un texte), plus la portée
  des effets (3σ par flou, lueur ou ombre, le décalage d'une ombre,
  l'amplitude d'un déplacement), à l'échelle du nœud. Le peintre n'écrit
  rien au-dehors.
- Refus : fonction inconnue (`sepia()`…), σ au-delà de 250 px, amplitude
  au-delà de 500 px.
- Les effets sont calculés par le moteur, en flottants et en boucles
  simples, sans bibliothèque : le même rendu donne les mêmes octets.

### Transitions

```json
{ "type": "series", "children": [
  { "type": "sequence", "duration": 30, "children": [ … ] },
  { "type": "transition", "presentation": "fade", "timing": { "duration": 10 } },
  { "type": "sequence", "duration": 30, "children": [ … ] },
  { "type": "transition", "presentation": "slide", "direction": "from-right",
    "timing": { "duration": 10, "easing": "ease-in-out" } },
  { "type": "sequence", "children": [ … ] } ] }
```

- Une transition se tient entre deux séquences d'une série, et nulle part
  ailleurs. Elle les fait **se chevaucher** de sa durée : la suivante
  commence plus tôt d'autant, la série est plus courte d'autant (trois
  scènes de 30 images et deux transitions de 10 font 70 images). Elle ne
  dure pas plus que chacune des séquences qu'elle joint.
- `timing` sépare le temps de la présentation, comme Remotion : une durée
  et une courbe (`easing`, celles d'`interpolate`), ou un ressort de 0 à 1
  (`spring`, ses réglages sans `from`, `to` ni `delay`) sur cette durée.
- Présentations, à la progression p : `fade` mélange les deux scènes,
  (1 − p) de la sortante **plus** p de l'entrante (addition des couleurs
  prémultipliées, `plus-lighter`) : deux scènes opaques ne s'assombrissent
  jamais au milieu, deux scènes transparentes se croisent sans saut ;
  `slide` fait entrer la suivante par le côté nommé en poussant la
  précédente ; `wipe` la découvre par un bord droit qui avance ; `iris` par
  un cercle qui s'ouvre du centre au coin.
- Le descripteur donne sur la série la transition en cours (`uid`,
  présentation, fenêtre, progression), et sur chaque scène son rôle
  (`exiting`, `entering`), la progression et ce qu'elle en reçoit
  (opacité, translation, `"blend":"plus-lighter"`, découpe du volet).
- Le son des deux scènes joue pendant le chevauchement : une transition ne
  fond pas le son.

### Italique

`fontStyle: "italic"` (hérité, comme en CSS) prend la face italique de la
famille. Une famille sans italique est refusée (`FontMissing`, « no italic
face ») : jamais un romain penché. Le descripteur le dit
(`"fontStyle":"italic"`).

### Chiffres (MOTION-MARQUE-2026-10 MM2)

`fontVariantNumeric` (hérité, comme en CSS) choisit les chiffres d'une
police : `lining-nums` (alignés) ou `oldstyle-nums` (elzéviriens), et/ou
`tabular-nums` (tous de même chasse) ou `proportional-nums` ; `normal` rend
ceux de la police. Core Text applique les fonctions OpenType `lnum`, `onum`,
`tnum`, `pnum` à la mise en page comme au dessin : une police de titre qui
n'a par défaut que des chiffres elzéviriens (« 1 point » se lit « I point »)
écrit « 1 point » en `lining-nums`, sans copie de la police. Une police sans la
fonction garde ses chiffres. Le descripteur le dit
(`"fontVariantNumeric":"lining-nums"`), seulement quand c'est réglé.
Vers After : After n'applique aucune fonction OpenType à un calque texte ;
le bilan de l'export nomme chaque texte dont les chiffres changeraient.

## Origine et inclinaison (MW5)

MOTION-WEB-2026-10, MW5. Ce que les pages web et After écrivent le plus
souvent et que le format n'avait pas : le point autour duquel un nœud
tourne, et l'inclinaison.

```json
{ "type": "box", "style": { "width": 40, "height": 160, "transformOrigin": "50% 0", "skewX": -8 },
  "animate": { "rotate": { "interpolate": { "frames": [0, 30], "values": [-20, 20] } } } }
```

- `transformOrigin` : le point autour duquel `scale`, `rotate`, `skewX` et
  `skewY` tournent (et `rotateX`, `rotateY`), comme `transform-origin` de
  CSS dans le plan : un ou deux mots parmi `left`, `center`, `right`,
  `top`, `bottom`, un pourcentage de la boîte ou des pixels (`"left top"`,
  `"50% 100%"`, `"0 12px"`). Un seul mot règle x (ou y pour `top`,
  `bottom`), l'autre axe reste au centre. Le centre par défaut. Fixe, pas
  animable (`InvalidAnimation`) ; une troisième valeur (la profondeur de
  CSS) est refusée. Les unités d'un texte (`units`) tournent toujours
  autour de leur propre centre. Vers After, c'est le point d'ancrage du
  calque.
- `skewX`, `skewY` : l'inclinaison de `skewX()` et `skewY()` de CSS, en
  degrés, animables. L'ordre est `translate() rotate() skew() scale()` :
  l'échelle d'abord, puis l'inclinaison, puis la rotation (celui de GSAP
  en `skewType: "simple"` et de Lottie). Le `skewX` par défaut de GSAP
  (`"compensated"`) est un `skewX` suivi d'un `scaleY` de cos(angle). Pas
  d'inclinaison dans les unités d'un texte.

## Perspective (REM4)

REMOTION-PARITY-2026-09, REM4. Une carte peut tourner en profondeur, ce
qu'un motion éditorial demande (C19) ; ce n'est pas un moteur 3D.

```json
{ "type": "fill", "style": { "perspective": 800 }, "children": [
  { "type": "box", "style": { "width": 320, "height": 180, "rotateX": 10 },
    "animate": { "rotateY": { "interpolate": { "frames": [0, 20], "values": [0, 35] } } } } ] }
```

- `rotateX`, `rotateY` : rotations en degrés autour des axes horizontal et
  vertical du nœud passant par le centre de sa boîte (son
  `transformOrigin`, MW5), animables. Avec `translateX/Y`, `rotate`,
  `skewX/Y` et `scale`, l'ordre est celui des fonctions CSS
  `translate() rotateX() rotateY() rotate() skew() scale()`.
- `perspective` (sur le **parent**, comme en CSS) : distance en pixels
  entre l'œil et le plan du parent, `none` par défaut ; elle donne de la
  profondeur à ses enfants, autour du centre du parent
  (`perspective-origin` par défaut). Sans perspective, une rotation en
  profondeur ne fait que raccourcir.
- Un nœud tourné est **aplati** dans le plan de son parent, comme
  `transform-style: flat` : il est peint à plat dans son propre plan (ses
  enfants, ses effets, sa découpe et son masque y compris, qui y sont donc
  exacts), puis posé par la projection de ce plan, chaque pixel couvert
  relu dans le plan, en bilinéaire. Un coin passé derrière l'œil n'est pas
  dessiné.
- Le descripteur donne, pour un nœud tourné, ses rotations, sa matrice 4×4
  (ligne par ligne, dans l'espace de son parent) et où tombent les coins de
  sa boîte dans le cadre, tous ancêtres appliqués
  (`"transform3d":{"matrix":[…],"quad":[[x,y],…]}`), et la perspective d'un
  parent (`"perspective":800`). Les bornes d'un sous-arbre et le domaine
  d'un effet passent par cette projection.
- Refus : une perspective nulle ou négative (`InvalidValue`).

Hors contrat, et pour cause : `translateZ`, `transform-style:
preserve-3d`, l'ordre de profondeur entre frères, l'éclairage. Lottie
n'entre pas (voir REMOTION-PARITY-2026-09, REM4) : aucun film ne l'a encore
demandé.

## Motion design : flou de mouvement, fusion, grain (MOTION-DESIGN-2026-09)

Outils ajoutés pour le teaser de CUTMACHINE, en programmation extrême, puis
revus en dur le 19 octobre 2026 (MA7,
MOTION-AFTER-2026-09) : bornes, refus et
cas limites ci-dessous sont testés. Le maillage (`mesh()`) n'est pas dans
cette revue.

- **Flou de mouvement**, sur toute la composition :
  `"motionBlur": { "samples": 8, "shutterAngle": 180 }`. Chaque image est
  la moyenne, en lumière prémultipliée, de `samples` instants (2 à 64)
  répartis sur l'obturateur (`shutterAngle` en degrés, 180 par défaut,
  360 : l'image entière). L'instant k sur n tombe à l'image + `shutterAngle`
  × k / (360 n), l'angle gardé au millième de degré : chaque instant est
  une fraction exacte d'image, et tous précèdent l'image suivante.
  L'obturateur **s'ouvre sur l'image** : un plan coupé net sur une image ne
  déborde jamais sur la voisine. La moyenne se fait sur les octets
  prémultipliés, arrondie au plus proche ; le descripteur reste celui de
  l'image elle-même. Refus (`InvalidValue`) : `samples` absent, non entier
  ou hors de 2 à 64, `shutterAngle` hors de ]0, 360] ; une autre clé est
  refusée. Une image coûte `samples` rendus. Le flou et le grain de
  composition (plus bas) sont ceux de la composition rendue : ceux d'une
  composition montrée en instance (REM2) sont ignorés, c'est l'obturateur
  de la composition qui la montre qui la floute.
- **Modes de fusion** : style `mixBlendMode` (`normal`, `multiply`,
  `screen`, `overlay`, `darken`, `lighten`, `color-dodge`, `color-burn`,
  `hard-light`, `soft-light`, `difference`, `exclusion`, `plus-lighter`),
  comme CSS : le nœud et sa descendance composés en groupe sur ce qu'il y a
  dessous, quel que soit le chemin qui le peint (effets, profondeur), et
  une seule fois : dans le calque où un nœud tourné en profondeur est peint
  à plat, il n'y a rien dessous (MA7 : le mode y était appliqué aussi, et un
  cache tourné effaçait tout son groupe). Un autre mot : `InvalidValue`.
  Vers After, chaque mode a son mode de calque (`plus-lighter` :
  Ajouter).
- **Grain** : `grain(<quantité> [<taille>px] [<graine>] [<phase>])` dans la
  pile `filter`. Bruit de luminance semé, par cellules de `taille` pixels
  (1 par défaut), ajouté en proportion de la couverture, `quantité` de la
  pleine échelle au plus ; le résultat reste prémultiplié (chaque canal
  borné à l'alpha). Les cellules partent du coin de la zone que peignent
  les effets du nœud, arrondie au pixel : le grain suit le nœud qui se
  déplace, même à moitié hors de l'image (MA7 : un nœud qui entrait par
  la gauche ou par le haut avait un grain fixé au cadre). `taille` suit
  l'échelle du nœud, jamais sous un pixel. Seule la
  partie entière de la phase change le grain, et une phase animée d'une
  unité par image le fait vivre. Une phase calculée qui tombe à quelques
  ulps sous un entier vaut cet entier (MA7 : de 0 à 22 en 22 images,
  l'image 15 valait 14,999999999999998 et remontrait le grain de
  l'image 14). Sous l'obturateur, les instants d'une image ont le même
  grain tant que la partie entière de la phase ne change pas pendant
  l'obturateur : vrai d'une phase entière à l'image qui avance d'au plus
  une unité par image, faux d'une phase décalée d'une demi-unité, dont
  l'image moyenne deux grains. Au-delà de ±2⁵³, la phase est tenue à
  ±2⁵³. Refus : quantité hors de [0, 1], taille sans `px` ou hors de 1 à
  64, graine négative, non entière ou au-delà de 2⁶³ − 1 (MA7 : elle
  valait alors 2⁶³ − 1), plus de quatre arguments.
- **Rendu parallèle** : l'export et `verify_export` rendent les images de
  motion sur tous les cœurs : un fil par cœur, douze au plus, jamais plus
  que d'images à rendre ; une liste vide ne rend rien et ne refuse rien.
  Chaque image distincte est rendue une fois, aux mêmes octets que rendue
  seule (testé avec flou, grain et tranches). Les images d'une même vidéo
  se décodent dans l'ordre du film (PERF11) ; au premier refus, les autres
  fils s'arrêtent avant leur image suivante, et le refus nomme son
  image. L'export rend à l'échelle (plus bas),
  `verify_export` à la taille de la composition, réduite ensuite par
  FFmpeg.
- **Morph de tracés** : `d` s'anime par `interpolate` entre tracés SVG de
  même structure (mêmes déplacements et fermetures, autant de segments
  entre eux ; une ligne compte comme une cubique aux poignées sur elle).
  La structure se compte après lecture : une quadratique devient une
  cubique, un arc une cubique par quart de tour entamé, si bien que deux
  arcs d'angles différents peuvent ne pas se morpher. Chaque tracé est
  comparé au précédent. Le mélange est point à point ;
  une courbe à dépassement fait dépasser la forme. Deux structures
  différentes : `InvalidAnimation`. Vers After : non traduit, le tracé de
  l'image de référence est posé et le bilan le dit.
- **Caches** : `mixBlendMode` prend aussi `mask-in` (garde ce qui est dessous,
  dans le nœud), `mask-out` (hors du nœud) et `atop` (peint le nœud sur ce
  qui est dessous seulement) ; `isolation: "isolate"` ferme un groupe, pour
  que le cache s'arrête à son bord. Sans groupe fermé, un `mask-in` efface
  tout ce qui est hors de lui jusqu'au bord de l'image. Le texte qui révèle
  une image, c'est un groupe isolé qui tient l'image puis le texte en
  `mask-in`. Un cache tourné en profondeur (`rotateX`, `rotateY`) découpe
  par sa forme projetée. Vers After : pas encore traduit (pas de cache de
  piste), le bilan dit « pas d'équivalent de calque, ignoré ».
- **Pointillés** : `strokeDasharray` sur un tracé : `none`, ou des longueurs
  en pixels (un nombre, ou suivi de `px`), aucune négative, séparées par
  des espaces ou des virgules (`"10 5"`, `"10px, 5px"`). Une liste impaire
  se répète, comme en SVG ; une liste toute à zéro dessine un trait plein.
  `strokeDashoffset` (pixels, animable) décale le motif ; les deux suivent
  la mise à l'échelle du `viewBox`. Refus (`InvalidValue`) : une autre
  unité, un pourcentage, un mot, une longueur négative, un élément vide
  entre deux virgules (MA7 : `"10px 5px"` dessinait 10 et 10, la lecture
  s'arrêtant au premier mot qu'elle ne savait pas lire). Vers After : non
  traduits, le bilan le dit.
- **Goo** : `goo(<σ>px)` dans `filter` : flou de déviation σ (0 à
  250 pixels, suivant l'échelle du nœud), puis bords durs autour de la
  moitié de la couverture (rampe de 0,46 à 0,54) ; deux formes plus proches
  qu'environ σ fondent l'une dans l'autre. La couleur reste celle du flou,
  ramenée à la nouvelle couverture. Vers After : non traduit.
- **Tranches** : `slice(<amplitude>px <bande>px <graine> [<phase>])` dans
  `filter` : le nœud coupé en bandes horizontales d'environ `bande` pixels,
  chacune fendue une fois à une hauteur semée (entre un cinquième et quatre
  cinquièmes), et trois tranches sur cinq décalées de côté d'au plus
  `amplitude` pixels entiers (glitch net). Les bandes partent du haut de la
  zone que peignent les effets du nœud, même à moitié hors de l'image
  (MA7) ; amplitude et bande suivent son échelle. Comme le grain, seule la partie entière de la phase change le
  motif, avec la même règle à quelques ulps d'un entier ; animer
  l'amplitude l'allume et l'éteint. Refus : amplitude hors de 0 à
  500 pixels, bande nulle ou négative, graine négative ou au-delà de
  2⁶³ − 1, moins de trois ou plus de quatre arguments. Vers After : Onde progressive carrée, approchée
  et dite au bilan (MA5).
- **Maillage** : `mesh(<lignes> <colonnes> [<qualité>], <x> <y>, …)` dans
  `filter`, le Mesh Warp d'After. La boîte du nœud devient une grille de
  `lignes` × `colonnes` cellules ; ses (lignes + 1) × (colonnes + 1) sommets
  s'écrivent ligne par ligne depuis le coin haut-gauche, chacun en pixels ou
  en pourcentage de la boîte. Un sommet écrit à sa place de repos laisse
  l'image telle quelle. Chaque cellule est un carreau de Coons bordé de
  quatre Béziers cubiques dont les poignées sont au tiers de la cellule au
  repos et suivent leur sommet, comme dans After tant qu'on ne les tire
  pas : une ligne de la grille déplacée entière reste droite et étire en
  douceur les cellules de part et d'autre (la *kinetic warp typography*).
  Au-delà de la boîte au repos, l'image suit le point le plus proche de son
  bord, comme si le maillage couvrait tout le calque (After n'a pas de
  dehors) : un accent qui dépasse d'une ligne serrée reste sur sa lettre,
  sans se déformer avec elle. `qualité` (1 à 10, 8
  par défaut) règle la finesse de la découpe de chaque cellule. Animable
  sommet par sommet entre deux maillages de même grille et de même qualité,
  px et % mêlés ; le descripteur donne les sommets à l'instant, en pixels
  du nœud, et le domaine grandit du plus grand déplacement d'un sommet.
  Vers After : pas encore traduit (le bilan le dit).
- Vers After, le flou de mouvement passe (MA12) : l'obturateur de chaque
  composition, phase 0, ses instants, et l'interrupteur de chaque calque
  qui montre quelque chose. After flou ce qui *bouge* un calque ; un fondu,
  une couleur ou une découpe qui changent sur place y prennent la valeur
  de l'image, que le moteur moyenne sur l'obturateur : le bilan le dit.
- **Grain de composition** : `"grain": {"amount": 0..1, "size": 1..64,
  "seed": n}` au niveau de la composition (`amount` obligatoire, `size` 1
  et `seed` 0 par défaut ; une autre clé, une graine négative ou une valeur
  hors bornes : `InvalidValue`), peint une fois sur l'image finie (après la
  moyenne du flou), nouveau à chaque image : la troisième coordonnée du
  bruit est le numéro de l'image. Les cellules sont en pixels de l'image
  rendue, depuis son coin haut-gauche, et ne touchent pas ce qui est
  transparent. Vers After : non traduit, le bilan le dit.
- **Rendu à l'échelle** : une composition plus grande que la séquence qui
  la montre est rendue directement à la taille montrée : une timeline
  d'aperçu en 960 × 540 rend un quart des pixels. L'échelle est la plus
  petite de 1, largeur de la séquence sur largeur de la composition, et
  hauteur sur hauteur : jamais agrandie. La mise en page reste celle de la
  pleine taille ; ce qui est peint (texte, tracés, effets, profondeur) est
  redessiné net à la nouvelle taille, arrondie au pixel le plus proche (au
  moins 1). Les longueurs des effets suivent l'échelle ; les cellules du
  grain ne descendent pas sous un pixel, si bien qu'un grain de taille 1
  rendu à moitié est deux fois plus gros, relativement à l'image, qu'en
  pleine taille. Le descripteur d'une image rendue ainsi garde les boîtes
  de la pleine taille.

## Vers After Effects (MOTION-AFTER-2026-09)

Une composition part vers After Effects en projet **modifiable** : des
précompositions, des calques et des images clés, jamais des images figées.
After est une sortie ; rien ne revient dans CUTMACHINE.

Par le MCP, `export_motion_after` (MA1) décrit la composition et la fait
construire par le pont : After doit tourner sur un projet vide et sans
titre, un projet qui contient du travail est refusé, jamais fermé.
`timeline_id` ajoute les sons de cette timeline en calques audio, tels
qu'elle les joue (muet, solo), sur les mêmes plages, gain et fondus en
images clés de niveau ; `build: false` n'écrit que la description, à
`description_path`.

Sans After sous la main, la même description s'écrit en ligne de commande :

```sh
cutmachine --export-motion-after <projet> <description.json> \
    --composition <id> [--timeline <id>] [--name <nom>] [--aep <projet.aep>] \
    [--boil <id>]...
```

et `tools/after-bridge/scripts/motion_import.jsx` la construit dans After
(Fichier > Scripts > Exécuter le fichier de script) puis enregistre le
`.aep`.
Une sortie qui désigne le fichier lu, la composition ou le projet, est
refusée (`OutputExists`).

Traduction, par règles d'équivalence exactes (vérifiées en rendant les deux
côtés) :

- une séquence devient une précomposition numérotée et nommée (« 03
  DÉPLACER ») ; un groupe n'en devient une que s'il a une opacité, un mode
  de fusion, un effet, un débordement masqué, ou s'il porte un `id` et au
  moins six enfants ; sinon un null, ou rien s'il est immobile ;
- un texte devient un calque texte à sa police réelle, posé sur la ligne de
  base du moteur ; centré (`textAlign` ou boîte qui le centre seul), il est
  justifié au centre et le reste si on change le mot ;
- une boîte qui n'est un groupe que par son `filter`, autour d'un seul
  texte sans filtre, opacité, fusion ni transformation propres, se replie
  dans ce texte : le calque texte porte l'effet, et sa cascade s'il en a
  une (MA5). Ce repli n'est pas exact dans After : l'Onde progressive qui
  approche `slice()` y tombe à d'autres bandes que sur une précomposition
  (constaté le 30 septembre) ;
- une animation par unités devient un animateur : un curseur par propriété
  porte les images clés, un sélecteur d'expression les lit au décalage de
  chaque unité (curseur « décalage (images) ») ;
- `letterSpacing` animé devient un animateur d'approche, en pixels, après
  chaque lettre ; le timecode, une expression du texte source ;
- les courbes `cubic-bezier` deviennent vitesse et influence d'After, qui
  tracent la même courbe sur une dimension ; `step-end`, des clés maintenues.
  Une courbe par segment (MB5) arrive telle quelle : chaque clé porte la
  sortie de son segment et l'arrivée du précédent ;
- un id `nom#portée` (les id sont uniques dans une composition) s'affiche
  `nom`.
- l'animation propre à une `sequence` court dans son temps local : ses clés
  sont décalées de son `from` sur son calque, qui vit dans le temps du
  parent (MA8) ;
- `letterSpacing` devient l'approche du paragraphe en millièmes de
  cadratin, arrondie à l'entier qu'After exige (MA9, dit au bilan).
- une clé entre deux images (`"5/2"`, MW1) part à son instant : la
  description l'écrit `"f": "5/2"` et `motion_import.jsx` la pose à 5/2 de
  durée d'image, qu'After garde (MW2, construit et rendu dans After 26 le
  14 novembre 2026 : 0,02 niveau d'écart moyen avec le moteur sur seize
  images, à 25 et à 29,97 i/s). After range les instants au 1/800 d'image
  près, mesuré à 29,97 i/s : une clé à 1/3 d'image y tombe à 267/800
  (0,02 ms). Ce que la mise en page bouge reste lu aux images entières, de
  l'image d'avant le début à l'image d'après la fin ;
- `transformOrigin` devient le point d'ancrage du calque, la position
  suivant l'ancre ; sur une boîte que la mise en page redimensionne,
  l'ancre reste au centre (dit au bilan) (MW5) ;
- `skewX`, `skewY` deviennent l'effet Transformer d'After, dernier de la
  pile, autour de l'ancre : `skewX(a)` une oblique de −a sur l'axe 90,
  `skewY(a)` une de a sur l'axe 0 (mesuré dans After 26). Sur un calque de
  forme ou de texte, que After pixellise après sa transformation, le pivot
  et l'axe suivent l'ancre et la rotation par expression ; sur un métrage
  ou une précomposition, l'angle est réglé pour une échelle non uniforme,
  exact tant qu'elle tient. Un groupe incliné devient une précomposition
  (un null ne transmet pas d'effet). Comparé au moteur, chaque forme tombe
  sur les mêmes pixels ; un texte incliné s'adoucit un peu, l'effet
  rééchantillonnant le texte pixellisé. Le bilan dit `skewX` et `skewY`
  ensemble (seul `skewX` passe) et une inclinaison sous une échelle non
  uniforme qui bouge, sur un métrage ou une précomposition (approchée)
  (MW5).
- une rangée de textes (une boîte en ligne, sans retour à la ligne, dont
  chaque enfant est un texte d'une ligne aligné à gauche que le flux
  place) reste une rangée : chaque texte reçoit une expression sur sa
  position x, et sur son ancre s'il tourne ou change d'échelle, qui le
  déplace de la variation de largeur des textes qui le poussent, mesurée
  par After (`sourceRectAtTime`, là où finissent les lettres) contre ce
  qu'il a mesuré à la construction. Sans retouche, chaque variation vaut 0
  et les pixels sont ceux du moteur ; un mot changé dans After pousse les
  autres comme le moteur, au pixel près tant que sa dernière lettre ne
  change pas (sinon de l'écart entre les approches latérales des deux
  dernières lettres : 5 px pour COU → COUR à 300 px). Les coefficients
  viennent du moteur : chaque texte élargi de 64 px, la mise en page
  refaite (1 pour un mot qui suit dans une rangée calée au début, ±0,5
  autour d'un mot centré) (MA3) ;
- un `clipPath` (`inset()`, `polygon()`, `circle()`, `ellipse()`) devient
  un masque du calque, à clés s'il s'anime : le soulignement qui s'écrit,
  le volet (MA11). Une boîte à `overflow: hidden` aux coins arrondis, ou
  que la mise en page redimensionne, découpe par un masque aussi ; une
  image en `objectFit: cover` est masquée à sa boîte. Une courbe qui
  dépasse ses clés (`y1` ou `y2` hors de [0, 1]) est bornée par After sur
  un masque : le bilan le dit ;
- une couleur animée (`backgroundColor` d'une boîte, `color` d'un texte, la
  sienne ou celle qu'il hérite, `fill` et `stroke` d'un tracé) devient des
  clés de couleur, et son alpha des clés d'opacité. CSS mêle les couleurs
  en alpha prémultiplié : c'est exact tant que l'alpha tient, ou quand un
  bout est transparent ; sinon le bilan le dit (MA11) ;
- deux échelles à la fois (`scale` et `scaleY`) : chaque dimension de
  l'échelle d'After suit sa piste, la courbe coupée exactement là où
  l'autre a une clé (MA11) ;
- ce que la **mise en page** bouge (une boîte dont `width`/`height`
  s'animent, l'image qui la couvre, le mot qu'un voisin pousse) est lu
  dans le moteur à chaque image où ça bouge : une clé à chaque bout, et
  entre deux la courbe d'une des animations en cause quand elle redonne
  chaque image, sinon une clé par image (dit au bilan). Une boîte qui ne
  fait que glisser par ses propres `left`/`top` garde ses clés ; une barre
  qui s'allonge depuis son bord, une échelle x ancrée sur ce bord. Un
  groupe emporte ses enfants ; un tracé que la mise en page déplace reste
  approché (dit au bilan) (MA11) ;
- un nœud `audio` ou une `video` sonore devient un calque de métrage, à
  l'instant du média que le moteur joue (`startFrom`, `endAt`), son volume
  un curseur « volume » (le gain linéaire du moteur, avec ses courbes) que
  les niveaux audio lisent en décibels. Une séquence qui ne joue que des
  sons n'est pas une scène : ses sons vont dans la composition du film.
  Une vidéo se cadre comme une image ; muette, ou à une autre vitesse
  (`stretch`), son son est coupé, comme dans le moteur (MA11).

Une boîte qui découpe, une séquence ou un groupe plus grand que la plus
grande composition d'After (30 000 px) est coupé à elle, centrée sur son
parent, et le bilan le dit (MA7).

**Vérifier (MA2).** `verify_after_export` (MCP) compare le projet que
l'export a construit au moteur, image par image : After rend chaque image
par la méthode `render_frame` du pont (le projet ouvert dans After, comme
l'export le laisse), le moteur rend la même, et l'écart est la luma
moyenne de Pillow (`convert("L")`, chaque image sur du noir), la mesure employée
depuis le 29 septembre. Une image au-delà de la tolérance (0,5 niveau
par défaut) est « annoncée » quand une note du bilan sur un nœud dessiné à
cette image l'explique (un `slice()` seulement tant qu'il coupe),
« inexpliquée » sinon ; les notes sur toute la composition sont listées à
part et n'expliquent aucune image. Les deux images de chaque image restent
dans `image_dir`. Sur le teaser v7 (le 14 novembre 2026) : 95 images sur 98
à 0,5 niveau près (médiane 0,22), les trois autres annoncées par
`slice()`, en 2 min 15 s.

Ce qui ne se traduit pas à l'identique est dit dans le bilan de la commande :
`slice()` devient un effet Onde progressive approché, un spring garde sa
valeur de référence, un tracé que la mise en page déplace garde sa
géométrie de référence, et After n'applique pas `fontVariantNumeric`. Un
texte dans une police variable sort à sa graisse : la description donne sa
famille, sa graisse et sa pente, et `motion_import.jsx` demande à After
l'instance à ce point de l'axe `wght` (`postScriptNameForDesignVector`),
nommée ou non, les autres axes à leur défaut (MW7, comparé dans After 26 :
même chasse que le moteur).
Un film de marque écrit sans détour (découpes, couleurs et tailles
animées, dix-huit sons) passe en 54 compositions et
292 calques, sans autre note que l'approche arrondie ; dix-huit images
rendues par After égalent le moteur à 0,4 niveau de luma près en moyenne. Sur le teaser v7 (19 compositions, 216 calques), 95 images sur 98
rendues par After égalent le moteur à 0,5 niveau de luma près ; les trois
autres sont des glitchs `slice()`.

**Textures qui bouillonnent (MA4).** Une texture qui saute toutes les n
images (`translateX`/`translateY` en `step-end` à pas constant) part par
défaut en clés maintenues, exactes : 393 par axe pour une texture du teaser
v7. `--boil <id>` (MCP : `boil`) nomme les nœuds dont ces pistes deviennent
une expression qu'un motion designer peut régler, un saut par
`posterizeTime` à la même cadence, tiré par `random` dans l'intervalle de
la piste, ajouté à la place du calque. Les sauts sont alors ceux d'After,
plus ceux du moteur : le bilan nomme chaque piste convertie. Les nœuds
sont nommés, jamais devinés d'après leurs clés ; un nœud nommé sans piste
en paliers réguliers est refusé. Sur le teaser v7, les deux textures
passent de 1 572 clés à quatre expressions (2 028 → 456 clés en tout).

## Depuis After Effects (MOTION-BIBLIOTHEQUE-2026-10 MB7)

L'autre sens : `import_after_comp` (MCP) lit une composition du projet
ouvert dans After par le pont (méthode `read_comp`, lecture seule) et
l'ajoute au projet, comme `add_motion_composition`. Une lecture gardée
(`reading_path`) se rejoue sans After :

```sh
cutmachine --import-after-comp <lecture.json> <composition.json> \
    [--font <PostScript d'After>=<PostScript>]... [--font-dir <dossier>]...
```

Par équivalences, comme l'export : un calque devient un nœud nommé comme
lui (ancre → `transformOrigin`, position − ancre → `translateX`/`Y`,
échelle, rotation, opacité), un parent une boîte autour de lui sans son
opacité, un calque qui ne couvre pas la composition une `sequence`. Les
clés gardent leurs instants et, par segment, la `cubic-bezier` que tracent
les vitesses et influences d'After ; un maintien devient `step-end`. Un
texte garde sa face (son nom PostScript résolu en famille, graisse et
pente), son corps, son interlignage en pixels et son approche, posé sur sa
ligne de base ; un rectangle seulement rempli devient une boîte, une autre
forme un `path` ; l'effet Remplir devient la couleur ; un cache de piste
alpha fait d'un seul rectangle devient un `clipPath`.

Ce qu'After calcule et que le format ne dit pas (expression, trajectoire
courbe, cache) est lu image par image : une clé où la valeur change, et la
propriété n'est plus vivante. Ce qui n'a pas d'équivalent n'est pas traduit
(précompositions, métrages, masques, animateurs de texte, autres effets,
modes de fusion, 3D). Les deux sont dans `notes`, calque par calque, comme
le crénage d'After, que le moteur ne fait pas, et une police absente (le
rendu la refuse, `FontMissing`) ou remplacée (`fonts`). Sur un cartouche de
titre fait à la main (14 novembre 2026) : la boîte et sa découpe aux mêmes
pixels qu'`aerender` à chaque image, l'écart du texte (0,64 niveau au plus)
dû à une paire crénée par After.

## Humaniser (MA6)

Une composition écrite par une machine (un générateur, un agent) peut
être réécrite au niveau où une personne l'aurait écrite, sans qu'un
pixel change. Trois règles :

- **Timecode.** Une rangée de colonnes de même largeur, où chaque chiffre
  est une bande des dix chiffres décalée d'une image à l'autre et chaque
  séparateur un caractère fixe, devient un texte lié au timecode
  (`{"timecode": "composition"}`, ou `"local"`, avec `offset` s'il le
  faut), espacé (`letterSpacing`) de la largeur des colonnes moins l'avance
  de la police. La règle lit, image par image, les chiffres que la rangée
  montre et ne s'applique que s'ils font le timecode à chaque image où elle
  est montrée ; la police doit être à chasse fixe.
- **Enveloppes.** Une boîte qui ne fait que placer un texte
  (`position: absolute`, `left`, `top`, rien d'autre) se replie dans ce
  texte, qui prend sa place, son `id` et ses animations (sauf si le texte
  s'anime déjà, ou se découpe en unités).
- **Lettres.** Des textes frères d'un caractère chacun, même style, même
  ligne, chacun là où le précédent finit selon les avances du moteur (une
  espace admise entre deux), animés de la même façon à un décalage près par
  rang, deviennent un texte découpé en caractères, le décalage un
  `stagger`. Des lettres posées avec d'autres chasses (celles de FreeType,
  pour le générateur du teaser v6) tombent à une fraction de pixel et
  restent à part ; le bilan dit de combien.

Les groupes qui ne font que contenir d'autres nœuds ne sont pas aplatis :
ceux du v6 disposent leurs enfants dans le flux, qu'aplatir déplacerait.

La réécriture ne s'applique que prouvée : toutes les images de la
composition sont rendues avant et après, et une seule image qui diffère la
refuse entière (`NotEquivalent`). Elle passe par une
`UpdateMotionComposition` : une annulation la reprend.

```sh
cutmachine --humanize-motion <composition.json> <sortie.json>
cutmachine --humanize-motion <projet> <sortie.json> --composition <id>
```

Comme pour `--export-motion-after`, la sortie ne peut pas être le fichier
lu (`OutputExists`).

Par le MCP, `humanize_motion_composition` réécrit, prouve et applique ;
`apply: false` rend la définition réécrite sans toucher au projet. Sur le
teaser v6 : 3 242 nœuds → 556 (14 timecodes, 146 enveloppes, 4 mots de
10 lettres), les 784 images identiques, 35 s sur huit cœurs.

## Édition visuelle (REM5)

REMOTION-PARITY-2026-09, REM5. Une composition se construit et se règle
au moniteur Record, sans écrire de JSON ; chaque geste est l'une des
opérations qui existent.

- **Sélection** : un clic sur Record choisit le nœud peint sous la souris,
  à travers transformations, perspective, instances et découpes, par les
  boîtes du descripteur (pas de seconde géométrie). Un calque plein cadre
  qui ne peint rien laisse passer le clic.
- **Glisser, poignées, flèches** : le déplacement est ramené dans l'espace
  du parent (un parent tourné de 90° et doublé change (10, 0) en (0, −5)),
  sous une perspective aussi ; les poignées règlent `width`/`height` le long
  des axes du nœud. Une propriété animée par clés prend une clé à l'image
  montrée ; une propriété animée par un ressort n'est pas glissée : son
  ressort se règle.
- **Un geste, une opération** : la composition réécrite entière
  (`UpdateMotionComposition`), qu'une annulation reprend ; ce que montre le
  geste en cours reste dans l'interface.
- **Inspector** : texte, transformation, apparence, mise en page, temps
  d'une séquence ou d'une transition ; ◇/◆ pose ou retire la clé à l'image
  montrée, la courbe se choisit, un ressort (départ, arrivée, retard,
  amortissement) se pose ou se retire. Un champ vidé rend la propriété à
  sa valeur CSS.
- **Menus** : texte, boîte, vidéo d'un rush, image ; scène à la suite et
  transition vers la suivante dans une série ; nouvelle composition (ou en
  scènes) posée au playhead, en une annulation.
- **Pour l'agent**, les mêmes règles dans `edit_motion_composition` :
  `key {node, property, frame, value}` (une clé posée ou déplacée, la
  première rendant la propriété animée), `unkey {node, property, frame}`
  (la dernière retire l'animation), `ease {node, property, easing}` pour
  tous les segments, ou `ease {node, property, easing, frame}` pour le seul
  segment qui part de la clé posée à cette image (MB5). Là où `easing` est
  une liste, une clé ajoutée coupe son segment en deux moitiés de la même
  courbe, et une clé retirée laisse au segment réuni la courbe de la clé
  qui le commence. L'Inspector montre « mixte » pour une liste de courbes
  différentes ; y choisir une courbe la donne à tous les segments.

## Exemple : carte titre et date déclinée

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 95,
  "props": { "date": { "type": "enum", "values": ["1er janvier", "1er février", "1er mars"] },
             "mention": { "type": "text", "default": "" },
             "corps": { "type": "number", "default": 62, "min": 20, "max": 200 },
             "interligne": { "type": "number", "default": 72, "min": 20, "max": 240, "unit": "px" },
             "ecart": { "type": "number", "default": 78, "min": 0, "max": 400 } },
  "formats": { "9x16": { "width": 1080, "height": 1920 },
               "16x9": { "width": 1920, "height": 1080,
                         "props": { "corps": 121, "interligne": 141, "ecart": 90 } } },
  "variants": { "janvier": { "props": { "date": "1er janvier" } },
                "fevrier": { "props": { "date": "1er février", "mention": "Complet" } },
                "mars": { "props": { "date": "1er mars" } } },
  "root": { "type": "fill", "style": { "justifyContent": "center", "alignItems": "center" }, "children": [
    { "type": "box",
      "style": { "flexDirection": "column", "alignItems": "center", "gap": { "prop": "ecart" },
                 "fontFamily": "Fira Sans Condensed", "fontWeight": 600, "fontSize": { "prop": "corps" },
                 "lineHeight": { "prop": "interligne" },
                 "textTransform": "uppercase", "textAlign": "center", "color": "#fff" },
      "children": [
      { "type": "text", "content": "Titre sur\ntrois\nlignes",
        "animate": { "scale": { "interpolate": { "frames": [2, 5], "values": [0, 1.2] } } } },
      { "type": "text", "content": { "prop": "date" },
        "animate": { "scale": { "interpolate": { "frames": [7, 10], "values": [0, 1.2] } } } },
      { "type": "text", "content": { "prop": "mention" }, "visible": { "prop": "mention", "notEmpty": true }, "style": { "fontSize": 36 } } ] } ] } }
```

Les écarts mesurés au pixel dans After pour placer ces lignes deviennent un
seul `gap`. La typographie posée sur la colonne est héritée par le titre et la
date : posée sur le seul titre, elle laisserait la date au corps par défaut,
comme en CSS. Trois versions (M6) changent la date et la mention ; deux
formats, le cadre et le corps : six fichiers d'une seule composition (voir
« Versions et formats »).

## Exemple : crédit nom et fonction sur palette

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 100,
  "props": { "nom": { "type": "text", "default": "Prénom Nom" }, "fonction": { "type": "text", "default": "Fonction" },
             "palette": { "type": "enum", "values": ["a", "b", "c"] } },
  "tokens": { "palette": { "a": "#E4572E", "b": "#4DABA1", "c": "#3D5AFE" } },
  "root": { "type": "fill", "style": { "justifyContent": "center", "paddingLeft": "8%" }, "children": [
    { "type": "box", "style": { "flexDirection": "column", "alignItems": "flex-start", "gap": 6 }, "children": [
      { "type": "box", "style": { "backgroundColor": { "prop": "palette", "map": "palette" }, "padding": "4px 10px" },
        "children": [ { "type": "text", "content": { "prop": "nom" }, "style": { "fontSize": 78, "color": "#fff" } } ],
        "animate": { "translateX": { "spring": { "from": -40, "to": 0, "damping": 14 } } } },
      { "type": "box", "style": { "backgroundColor": "#fff", "padding": "4px 10px" },
        "children": [ { "type": "text", "content": { "prop": "fonction" }, "style": { "fontSize": 58, "color": { "prop": "palette", "map": "palette" } } } ],
        "animate": { "translateX": { "spring": { "from": -40, "to": 0, "delay": 4, "damping": 14 } } } } ] } ] } }
```

La boîte suit son texte par flexbox : l'expression `sourceRectAtTime` d'After
n'a pas d'équivalent à écrire.

## Exemple : mots colorés, en cascade, à vitesse réglable

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 75,
  "props": { "texte": { "type": "text", "default": "Trois mots en couleur, pas plus" },
             "accent": { "type": "color", "default": "#FFD23F" },
             "premier": { "type": "number", "default": 1, "min": 0, "max": 20 },
             "combien": { "type": "number", "default": 2, "min": 0, "max": 20 },
             "vitesse": { "type": "number", "default": 100, "min": 25, "max": 400, "unit": "%" } },
  "root": { "type": "sequence", "timeScale": { "prop": "vitesse" },
    "style": { "justifyContent": "center", "padding": "0 8%" },
    "children": [
      { "type": "text", "content": { "prop": "texte" }, "units": "word",
        "style": { "fontFamily": "Fira Sans Condensed", "fontWeight": 600, "fontSize": 96, "color": "#fff" },
        "ranges": [ { "unit": "word", "start": { "prop": "premier" }, "count": { "prop": "combien" },
                      "style": { "color": { "prop": "accent" } } } ],
        "animate": { "opacity": { "interpolate": { "frames": [0, 6], "values": [0, 1] } },
                     "translateY": { "spring": { "from": 30, "to": 0 } } },
        "stagger": { "each": 3 } } ] } }
```

La plage suit ses props : aucun index à recalculer quand le texte change.
À `"vitesse": 50`, toute la séquence joue deux fois plus lentement.

## Exemple : titre à coins tracés

```json
{ "width": 1920, "height": 1080, "fps": 25, "duration": 50,
  "props": { "titre": { "type": "text", "default": "Titre de rubrique" },
             "marge": { "type": "number", "default": 40, "min": 0, "max": 200 },
             "coin": { "type": "color", "default": "#4DABA1" } },
  "root": { "type": "fill", "style": { "justifyContent": "center", "alignItems": "center" }, "children": [
    { "type": "box", "style": { "padding": { "prop": "marge" } }, "children": [
      { "type": "text", "content": { "prop": "titre" },
        "style": { "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 78, "color": "#fff" } },
      { "type": "path", "d": "M0 40 L0 0 L40 0",
        "style": { "position": "absolute", "top": 0, "left": 0, "fill": "none",
                   "stroke": { "prop": "coin" }, "strokeWidth": 4, "strokeLinecap": "square" },
        "animate": { "trimEnd": { "interpolate": { "frames": [0, 12], "values": [0, 1], "easing": "ease-out" } } } },
      { "type": "path", "d": "M40 0 L40 40 L0 40",
        "style": { "position": "absolute", "bottom": 0, "right": 0, "fill": "none",
                   "stroke": { "prop": "coin" }, "strokeWidth": 4, "strokeLinecap": "square" },
        "animate": { "trimEnd": { "interpolate": { "frames": [4, 16], "values": [0, 1], "easing": "ease-out" } } } } ] } ] } }
```

La marge est un `padding` lié à une prop : les coins suivent la boîte du
titre sans `sourceRectAtTime`.

## Exemple : titres empilés en alternance

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 60,
  "props": { "lignes": { "type": "list", "of": "text", "default": ["Votre texte ici", "Deuxième ligne", "Troisième ligne"] } },
  "root": { "type": "fill", "style": { "justifyContent": "center", "paddingLeft": "8%" }, "children": [
    { "type": "box", "style": { "flexDirection": "column", "alignItems": "flex-start", "gap": 8,
                                "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 64, "color": "#fff" },
      "children": [
      { "type": "repeat", "each": { "prop": "lignes" },
        "cycle": { "backgroundColor": ["#4DABA1", "#E4572E"] },
        "stagger": { "each": 3 },
        "template": { "type": "box", "style": { "padding": "2px 12px" },
          "animate": { "translateX": { "spring": { "from": -60, "to": 0, "damping": 14 } },
                       "opacity": { "interpolate": { "frames": [0, 4], "values": [0, 1] } } },
          "children": [ { "type": "text", "content": { "item": true } } ] } } ] } ] } }
```

Autant de bandeaux que d'éléments dans la liste ; `cycle` alterne les fonds,
`stagger` décale chaque copie de trois images.

## Exemple : question plein écran

```json
{ "width": 1920, "height": 1080, "fps": 25, "duration": 61,
  "props": { "question": { "type": "text", "default": "Quelle a été la plus belle surprise\nde cette année ?" },
             "contour": { "type": "bool", "default": false } },
  "root": { "type": "fill", "children": [
    { "type": "fill", "style": { "backgroundColor": "#E53935" },
      "animate": { "clipPath": { "interpolate": { "frames": [0, 12, 40, 55], "easing": "ease-in-out", "values": [
        "polygon(120% 0, 120% 0, 100% 100%, 100% 100%)",
        "polygon(0 0, 120% 0, 100% 100%, -20% 100%)",
        "polygon(0 0, 120% 0, 100% 100%, -20% 100%)",
        "polygon(0 0, 0 0, -20% 100%, -20% 100%)" ] } } },
      "children": [
        { "type": "sequence", "from": 4, "children": [
          { "type": "box", "style": { "position": "absolute", "left": "1%", "top": "30%",
                "fontFamily": "Fira Sans Condensed", "fontWeight": 500, "fontSize": 92, "lineHeight": 1.4,
                "color": { "prop": "contour", "map": { "true": "transparent", "false": "#fff" } },
                "textStroke": { "prop": "contour", "map": { "true": "3px #fff", "false": "none" } } },
            "animate": { "wordSpacing": { "interpolate": { "frames": [0, 20], "values": [60, 0], "easing": "ease-out" } } },
            "children": [
              { "type": "text", "content": { "prop": "question" }, "units": "word",
                "animate": { "opacity": { "interpolate": { "frames": [0, 3], "values": [0, 1] } } },
                "stagger": { "each": 1 } } ] } ] } ] } ] } }
```

Le volet est une découpe à quatre sommets interpolée sur quatre clés ; le
texte, enfant du volet, est découpé avec lui. La version rendue par la suite
de tests (`tests/fixtures/motion/question-plein-ecran.json`) ajoute le bord
blanc du volet, un second `fill` découpé dix pixels en avance. La séquence
englobe la boîte de texte et non l'inverse : une séquence est un calque
plein cadre, qui ne donne aucune largeur à une boîte qui l'envelopperait.

## Exemple : deux cartons en série

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 90,
  "root": { "type": "series", "style": { "fontFamily": "Fira Sans Condensed", "fontSize": 80, "color": "#fff" },
    "children": [
    { "type": "sequence", "duration": 45, "style": { "justifyContent": "center", "alignItems": "center" },
      "children": [ { "type": "text", "content": "Premier carton",
        "animate": { "opacity": { "interpolate": { "frames": [0, 5, 40, 45], "values": [0, 1, 1, 0] } } } } ] },
    { "type": "sequence", "offset": -5, "timeScale": "200%", "style": { "justifyContent": "center", "alignItems": "center" },
      "children": [ { "type": "text", "content": "Second carton",
        "animate": { "opacity": { "interpolate": { "frames": [0, 10], "values": [0, 1] } } } } ] } ] } }
```

Le second carton commence à l'image 40 (fondu enchaîné de cinq images) et
joue à double vitesse : son fondu d'entrée de dix images locales dure cinq
images.

## Exemple : mot étiré par maillage

```json
{ "width": 1920, "height": 1080, "fps": 30, "duration": 120,
  "root": { "type": "fill", "style": { "backgroundColor": "#000" }, "children": [
    { "type": "fill", "style": { "justifyContent": "center", "alignItems": "center",
        "filter": "mesh(2 4, 0 0, 25% 0, 50% 0, 75% 0, 100% 0, 0 50%, 25% 50%, 50% 50%, 75% 50%, 100% 50%, 0 100%, 25% 100%, 50% 100%, 75% 100%, 100% 100%)" },
      "animate": { "filter": { "interpolate": { "frames": [0, 20, 40, 60, 80, 100], "easing": "cubic-bezier(0.325, 0, 0, 1)", "values": [
        "mesh(2 4, 0 0, 25% 0, 50% 0, 75% 0, 100% 0, 0 50%, 25% 50%, 50% 50%, 75% 50%, 100% 50%, 0 100%, 25% 100%, 50% 100%, 75% 100%, 100% 100%)",
        "mesh(2 4, 0 0, 25% 0, 65% 0, 75% 0, 100% 0, 0 50%, 25% 50%, 65% 50%, 75% 50%, 100% 50%, 0 100%, 25% 100%, 65% 100%, 75% 100%, 100% 100%)",
        "mesh(2 4, 0 0, 25% 0, 65% 0, 75% 0, 100% 0, 0 79%, 25% 79%, 65% 79%, 75% 79%, 100% 79%, 0 100%, 25% 100%, 65% 100%, 75% 100%, 100% 100%)",
        "mesh(2 4, 0 0, 25% 0, 36% 0, 75% 0, 100% 0, 0 79%, 25% 79%, 36% 79%, 75% 79%, 100% 79%, 0 100%, 25% 100%, 36% 100%, 75% 100%, 100% 100%)",
        "mesh(2 4, 0 0, 25% 0, 36% 0, 75% 0, 100% 0, 0 24%, 25% 24%, 36% 24%, 75% 24%, 100% 24%, 0 100%, 25% 100%, 36% 100%, 75% 100%, 100% 100%)",
        "mesh(2 4, 0 0, 25% 0, 50% 0, 75% 0, 100% 0, 0 50%, 25% 50%, 50% 50%, 75% 50%, 100% 50%, 0 100%, 25% 100%, 50% 100%, 75% 100%, 100% 100%)" ] } } },
      "children": [ { "type": "text", "content": "MOT\nCLÉ",
        "style": { "fontFamily": "Fira Sans Condensed", "fontWeight": 600, "fontSize": 330, "lineHeight": 0.95, "color": "#fff" } } ] } ] } }
```

Transposition neutre de la *kinetic warp typography* d'un tutoriel After
(Mesh Warp à 2 × 4 cellules) : la ligne verticale du milieu part à droite,
la ligne horizontale descend, puis la verticale part à gauche, l'horizontale
remonte, et tout revient au repos. Les colonnes 25 % et 75 % restent fixes
et bornent l'étirement au mot. La courbe est celle du tutoriel, mesurée sur
son rendu : l'aisance d'After (influence de sortie à 33 %), puis la
poignée d'arrivée tirée à fond, soit `cubic-bezier(0.325, 0, 0, 1)` ; le
même mouvement, rendu par CUTMACHINE avec la police et le crénage du
tutoriel, recouvre la capture de son rendu After à 94 % au moins sur chaque
image, 97 % en moyenne (pixels blancs communs sur pixels blancs de l'un ou
l'autre, une image sur cinq de 0 à 110).

## Dans le document (M3)

Une composition est un élément **du projet**, comme un média de la
bibliothèque, et non d'une timeline : une carte se pose sur plusieurs
timelines (M4) et s'exporte en versions et formats à part (M6). Le document
et le fichier projet la portent sous `compositions` :

```
"compositions": [
  {"id": "<ULID>", "name": "Carton titre", "definition": { … }}
]
```

`definition` est la composition au format de ce fichier, telle que l'auteur
l'a écrite, plus les identités que le document lui donne : un `uid` sur
chaque nœud (placé juste après `type`) et, dans chaque `interpolate`, les
`uids` de ses clés. Tous sont des ULID de l'espace d'identifiants du
document. Les nombres décimaux y restent le texte écrit (`1.4` reste `1.4`,
`14e-1` reste `14e-1`) : c'est la seule partie du document où un décimal
est admis, et il n'est jamais réécrit depuis un `double`.

**Version.** Un document s'écrit à la plus ancienne version qui le contient.
Sans composition, il reste en version 7, octet pour octet, et un binaire
antérieur à M3 l'ouvre encore. Avec une composition, il passe en version 8,
que ce binaire refuse (`unsupported document version 8`, ou le refus des
décimaux s'il en rencontre un avant) plutôt que de perdre la composition.
Une version 7 qui porterait `compositions` est refusée. Même règle pour
`document_version` dans le fichier projet.

**Opérations** (`--apply-op`, journal de la timeline d'où l'on édite,
comme les chutiers) : `AddMotionComposition`, `RemoveMotionComposition`,
`UpdateMotionComposition` (nom et définition entière), `SetMotionNode`
(remplace le nœud dont l'`uid` existe, sinon l'insère sous `parent_uid`
avant `before_uid`), `RemoveMotionNode`, `SetMotionStyle` et
`SetMotionAnimation` (une propriété ; sans valeur, la retire),
`SetMotionKeyframe` (une clé, par son uid, vers une autre image et une autre
valeur), `SetMotionProps` (les valeurs propres de la composition : le
`default` de chaque prop, contrôlé contre son type et ses bornes ; `null` le
retire). La première application donne une identité à ce qui arrive sans,
et l'opération la garde : refaire redonne les mêmes. L'inverse de toute
modification interne est la composition d'avant, entière : l'annulation
restitue les octets, ordre des clés compris.

Refus : `UnknownComposition`, `UnknownMotionNode`, `UnknownKeyframe`,
`UnknownProp`, `PropOutOfRange`, `PropTypeMismatch`, et `InvalidMotion`
pour tout autre refus du format, dont le message commence par le code et le
chemin du refus motion (`UnknownStyleProperty at root.style.paddingX: …`).

**Lire et voir.** `--describe-compositions <projet> [<id>]` et les outils MCP
`list_motion_compositions` et `describe_motion_composition` rendent les
mêmes octets. `--describe-motion-frame` et `--render-motion-frame` prennent
`--composition <id>` avec un projet pour premier argument ; l'outil
`render_motion_frame` renvoie l'image et, en texte, la géométrie de la même
évaluation. Chaque boîte y porte l'`uid` de son nœud : c'est l'adresse à
donner à la modification suivante. L'outil `edit_motion_composition`
applique une liste de modifications comme un seul pas d'annulation, et
désigne un nœud par son `uid`, par son `id` d'auteur ou par un préfixe
d'`uid` sans ambiguïté.

## Exemple : carton stocké, avec ses identités

```json
{ "width": 1080, "height": 1920, "fps": 25, "duration": 60,
  "props": { "titre": { "type": "text", "default": "Titre sur deux lignes" },
             "police": { "type": "enum", "values": ["Fira Sans Condensed", "Inter"] },
             "position": { "type": "point", "default": { "x": 0, "y": -40 }, "min": -540, "max": 540 },
             "logo": { "type": "size", "default": { "width": 30, "height": 10 }, "unit": "%" },
             "texte": { "type": "group", "props": ["titre", "police"] } },
  "root": { "type": "fill", "uid": "01K80000000000000000000001",
    "style": { "justifyContent": "center", "alignItems": "center" }, "children": [
    { "type": "box", "uid": "01K80000000000000000000002", "id": "bloc",
      "style": { "flexDirection": "column", "alignItems": "center", "gap": 24,
                 "translateX": { "prop": "position.x" }, "translateY": { "prop": "position.y" } },
      "children": [
      { "type": "text", "uid": "01K80000000000000000000003", "id": "titre", "content": { "prop": "titre" },
        "style": { "fontFamily": { "prop": "police" }, "fontWeight": 600, "fontSize": 96, "lineHeight": 1.2, "color": "#fff" },
        "animate": { "opacity": { "interpolate": { "frames": [0, 6], "values": [0, 1],
                     "uids": ["01K80000000000000000000004", "01K80000000000000000000005"], "easing": "ease-out" } } } },
      { "type": "box", "uid": "01K80000000000000000000006", "id": "filet",
        "style": { "width": { "prop": "logo.width" }, "height": 4, "backgroundColor": "#FFD23F" } } ] } ] } }
```

La position du bloc et la largeur du filet sont des props ; le titre et sa
police sont groupés pour qui les édite. Les `uid` sont ceux que le document
a donnés à l'ajout : un agent qui déplace la seconde clé du fondu écrit
`{"op": "set_keyframe", "keyframe": "01K80000000000000000000005", "frame": 9,
"value": 1}`.

## Versions et formats (M6)

Une composition porte des **versions** (des jeux de props nommés : une date,
un message) et des **formats** (d'autres tailles de cadre, 9:16 ou 16:9, avec
les props qui y adaptent la carte). Une version se rend dans chaque format :
trois versions et deux formats font six rendus d'une seule composition.

```
"formats":  { "9x16": { "width": 1080, "height": 1920 },
              "16x9": { "width": 1920, "height": 1080, "props": { "corps": 121 } } },
"variants": { "janvier": { "props": { "date": "1er janvier" } },
              "fevrier": { "props": { "date": "1er février" } } }
```

- Un format donne une taille ; la mise en page reste celle de flexbox et des
  `%`, calculée par le moteur. Il porte aussi les valeurs de props qui
  changent avec le cadre : la carte de référence de M2 est la même intention
  avec un corps presque double en 16:9, et aucune unité CSS ne relie un corps
  à la taille du cadre sans calcul. C'est l'équivalent, en données, de deux
  `<Composition>` Remotion qui partagent un composant avec des `defaultProps`
  différentes.
- Une version donne des valeurs de props, contrôlées comme toute valeur
  (`UnknownProp`, `PropOutOfRange`, `PropTypeMismatch`, au chemin
  `variants.<nom>.props.<prop>`). Une modification qui rendrait une version
  invalide (une borne resserrée, une prop retirée) est refusée de même.
- Une prop se règle **par les formats ou par les versions, jamais les deux**
  (`InvalidComposition`) : sa valeur dépendrait sinon d'un ordre de priorité
  que personne n'a écrit. Le refus pourra s'assouplir plus tard sans changer
  le rendu d'aucune composition stockée ; un ordre choisi aujourd'hui, non.
- Valeurs d'une image : les props de la composition, puis celles du format,
  puis celles de la version, puis celles de l'appelant pour cette image
  seulement (`--props`, `props`).
- Noms : 1 à 64 lettres ASCII, chiffres, `-` ou `_`, puisqu'ils nomment des
  fichiers (`16:9` est refusé, `16x9` accepté). Ordre de déclaration =
  ordre du rendu en lot.
- Nom inconnu : `UnknownVariant` ou `UnknownFormat`, avec les noms déclarés.
- Écart avec l'esquisse de M0 : une version ne porte plus de `format` et ses
  valeurs sont sous `props`. Versions et formats se croisent au rendu au lieu
  d'être combinés à la main (`fevrier-169`), et une version pourra recevoir
  d'autres champs sans collision avec le nom d'une prop.

Dans le document, versions et formats sont dans la définition : ils se
posent et se modifient par `UpdateMotionComposition`, sans opération ni champ
nouveau ; l'outil `edit_motion_composition` les édite un par un
(`set_variant {variant, props?}`, `set_format {format, width?, height?,
props?}` ; sans `props`, ou sans taille, l'entrée est retirée).
`--describe-motion-frame` et `--render-motion-frame` prennent `--variant` et
`--format`, l'outil `render_motion_frame` `variant` et `format` ;
`list_motion_compositions` nomme les versions et les formats de chaque
composition.

### Rendu vidéo et lot

```
cutmachine --render-motion-video projet.cutmachine /Livraison/carte_{variant}_{format}.mov \
  --composition <id> [--variants janvier,mars] [--formats 9x16] [--alpha] [--preview] [--overwrite]
```

(ou un fichier `composition.json` à la place du projet, sans
`--composition`). L'outil MCP `render_motion_video` prend les mêmes
arguments (`output`, `variants`, `formats`, `alpha`, `overwrite`, `preview`,
`font_dirs`, `loudness`, `true_peak`) et rend le même rapport.

**Sonie de livraison (MOTION-MARQUE-2026-10 MM1).** Sans réglage, le son
est le mixage tel qu'il s'additionne, sans limiteur. Avec `--loudness -16`
(`loudness: -16`, de −40 à −5 LUFS) et un plafond `--true-peak -1`
(`true_peak`, de −9 à 0 dBTP, −1 par défaut), le mixage est mesuré comme
le fait un sonomètre de diffusion (UIT-R BS.1770-4 : pondération K, blocs de
400 ms, portes à −70 LUFS puis 10 LU sous la moyenne ; crête vraie sur le
signal suréchantillonné quatre fois), amené à la sonie demandée et limité
sous le plafond (anticipation de 5 ms, retour en 100 ms, deux canaux liés),
le gain repris tant que le limiteur en a mangé. Dans un `.mp4`, l'AAC
reconstruit des crêtes au-dessus de ses échantillons : le mixage est
encodé une fois seul, mesuré, et le plafond du limiteur abaissé de ce qu'il
a dépassé. Le rapport de chaque fichier donne `loudness` : la cible, le
mixage avant (`mix`), le gain, ce que le limiteur a pris, et ce que mesure
le son du fichier livré (`file`, `ceiling_kept`). Un film de marque
(mixage à −21,1 LUFS, crête à +0,6 dBTP) sort à −16,0 LUFS et −1,01 dBTP,
ce que le filtre `ebur128` de FFmpeg confirme.

- Une composition se rend seule, sans timeline, de l'image 0 à sa durée
  exclue, à sa cadence exacte : FFmpeg lit la suite d'images à
  `fps` rationnel et écrit exactement `duration` images.
- Par défaut, toutes les versions dans tous les formats, versions en
  premier. Sans version déclarée, les valeurs de la composition ; sans
  format, sa taille : `{variant}` ou `{format}` est alors refusé, puisqu'il
  ne nommerait rien.
- Avec `--alpha` : ProRes 4444 (`prores_ks`, 10 bits, alpha droit comme en
  PNG) dans un `.mov`, pour poser un carton sur un montage dans Resolve. Sans :
  H.264 High 8 bits 4:2:0, l'encodeur et le débit de la livraison H.264 des
  timelines, l'image composée sur noir (dimensions paires exigées). Les deux
  sont marqués BT.709 et convertis avec cette matrice.
- Tout ce qui peut être refusé l'est avant le premier encodage, comme pour
  le lot de timelines : version ou format inconnu, extension, chemin écrit
  deux fois, dossier absent, fichier existant sans `--overwrite`, dimensions
  impaires en H.264 (`InvalidExport` sinon). Chaque image de chaque rendu est
  aussi mise en page d'avance : une police absente arrête le lot avant qu'il
  n'écrive quoi que ce soit.
- Le rapport, `{"ok", "preview", "total", "completed", "outputs"}`, liste
  chaque fichier (version, format, chemin, codec, taille, cadence, images,
  état). Un échec en cours de lot (`ExportFailed`) garde et liste les
  fichiers déjà terminés. `--preview` (par défaut dans l'outil MCP) vérifie
  et liste sans encoder.
- Les images passent par une suite PNG temporaire, effacée après chaque
  fichier, puis par l'encodage de l'export des timelines (fichier temporaire
  validé au dernier moment, y compris sur un disque exFAT).

## Sur la timeline (M4)

Un clip vidéo peut avoir pour source une composition du projet au lieu
d'un média : c'est un **clip de motion**. Il nomme sa composition et, s'il
en a, les valeurs de props de cette instance :

```json
{"id": "<ULID>", "source_id": "", "source_in": {"value": 5, "rate": 25},
 "duration": {"value": 30, "rate": 25}, "timeline_in": {"value": 10, "rate": 25},
 "include_audio": false, "effects": [], "opacity": {"num": 3, "den": 4},
 "composition_id": "<ULID>", "motion_props": {"titre": "Léa Martin", "taille": 72.5}}
```

- **Le temps du clip est celui de la composition.** `source_in` est un
  temps de la composition, et sa durée borne le clip comme la durée d'un
  média borne un plan : rogner la tête avance l'animation, couper donne à
  la moitié droite le temps où la coupe l'a laissée, glisser déplace
  l'animation sous le clip, un fondu enchaîné lit les images de la
  composition autour de la coupe. L'image montrée à un instant est celle
  qui le contient, la règle de tout média : une composition à 50 images/s
  sur une séquence à 25 montre une image sur deux.
- **Props d'instance** : `null` ou un objet non vide, contrôlé contre les
  props typées de la composition, avec les refus de M3 (`UnknownProp`,
  `PropTypeMismatch`, `PropOutOfRange`) ; décimaux gardés comme écrits.
  Elles priment sur les `default` de la composition, pour ce clip seul.
- **Refus** : sur une piste qui n'est pas vidéo, avec un `source_id`, avec
  des effets `color.*`, hors de la composition. Une composition qu'un clip
  montre ne se retire pas, et une modification qui ferait sortir un clip de
  sa composition (durée raccourcie, prop retirée qu'une instance règle) est
  refusée. `RemoveWords` refuse un clip de motion ; un collé de clip de
  motion aussi (il se repose par `InsertMotionClip`).
- **Version** : un document qui porte un clip de motion s'écrit en version
  9, que le lecteur de M3 refuse (il ignorerait sinon `composition_id` et
  lirait un clip média sans source). Sans clip de motion, un document garde
  sa version et ses octets ; une version 7 ou 8 qui en porterait un est
  refusée.
- **Opérations** : `InsertMotionClip` (comme `InsertClip`, ripple compris)
  et `SetClipMotionProps` (toutes les props de l'instance, l'inverse les
  restitue à l'octet). Outils MCP : `insert_motion_clip` (durée par défaut
  : le reste de la composition), `set_clip_motion_props` (fusion ; `null`
  retire une valeur), `render_motion_frame` avec `clip_id` et `time`, et
  `read_frame` sur un clip de motion. CLI : `--apply-op`,
  `--render-motion-frame <projet> <image de la timeline> <png> --clip <id>`.
- **Rendu** : à l'export, chaque clip est rendu image par image à la
  cadence de la séquence, en suite PNG lue par FFmpeg comme une source avec
  alpha ; au moniteur, l'image entre comme une image décodée. Sous gestion
  couleur, une composition est du sRGB d'affichage : elle rejoint l'espace
  de travail par l'inverse de la vue SDR, et une couleur hexadécimale en
  ressort telle qu'écrite. En sortie HLG, elle est posée dans la vue HLG
  elle-même (BLANC-HLG-2026-10 BH1) : son
  blanc sort au pic (0,994), une couleur y garde sa teinte.
- **Lecteurs** : un clip de motion est dessiné par-dessus les plans. Il
  n'est ni un plan ni une coupe pour `timeline_stats` (compté à part dans
  `motion_clips`), les planches et `list_shot_quality` ; `verify_export`
  compte ses images invérifiables ; le retour Resolve le refuse, faute
  d'essai dans Resolve.

## Gabarits et placement (M5)

Trois **gabarits** sont livrés avec le binaire, leur composition dans le
format de ce document (`assets/motion-templates`, embarqués à la
compilation) : `nom`
(nom et fonction en bas à gauche, 100 images), `carton` (couleur plein
cadre, titre et sous-titre centrés, 75 images) et `logo` (une image dans un
coin, 250 images). Chacun déclare trois formats, `9x16`, `16x9` et `1x1`,
avec les props qui l'y adaptent. Ce ne sont pas des outils à part : un
gabarit devient une composition du projet, qui se modifie et se pose comme
une autre.

- `list_motion_templates` les décrit : props typées avec bornes et défauts,
  formats et tailles.
- `add_motion_template {template, name?}` ajoute le gabarit comme
  composition, **à la taille de la timeline** : le format de cette taille
  exacte est retenu et ses valeurs deviennent les défauts. Une taille sans
  format est refusée (`UnknownFormat`, avec les formats connus).

**La bibliothèque (MB1, MOTION-BIBLIOTHEQUE-2026-10).**
Un gabarit est une **pièce** : un fichier qui dit à quoi il sert autour de
sa composition.

```json
{
  "format": "motion-piece.v1",
  "description": "Ce que la pièce est et fait à l'écran, et ses props.",
  "use": "Quand elle convient.",
  "avoid": "Quand ne pas s'en servir.",
  "invariants": ["Ce qui doit rester vrai une fois la pièce adaptée."],
  "origin": "D'où elle vient : un film, une licence.",
  "composition": { "formats": { "9x16": { "width": 1080, "height": 1920 } }, … }
}
```

`format`, `description` et `composition` sont obligatoires ; un champ
inconnu est refusé, une composition nue aussi. La composition déclare au
moins un format et ne porte aucune identité (`uid`, `uids`) : chaque
projet qui ajoute la pièce tire les siennes.

Le catalogue lit **deux étagères** :

- les pièces livrées, tous les fichiers d'`assets/motion-templates`,
  embarqués à la compilation et évalués par les tests dans chacun de leurs
  formats : les trois gabarits de M5 et, depuis MB2, quatre mécanismes
  de kinetic typo — `fente` (des mots qui se relaient dans une fenêtre
  fixe, liste `mots`), `trappe` (des lignes qui montent en escalier sur
  leur carte, liste `lignes`), `bandeau` (deux bandes pleine largeur
  venues de côtés opposés, dont le texte défile puis se cale ; depuis
  M5b, l'occurrence du milieu, celle qu'on lit, porte un `id`,
  `haut-centre` et `bas-centre`, et le contrôle la juge, les autres sont
  `decor` ; les corps de chaque format tiennent une phrase d'environ 24
  caractères sur une ligne dans sa zone, et la prop `mesure` borne la
  largeur d'une ligne à celle de la zone pour un texte centré, 800 px en
  9:16 entre la marge gauche et le rail de TikTok, 1 728 en 16:9, 972 en
  1:1 : une phrase plus longue passe à la ligne au lieu de sortir) et
  `maillage` (quatre mots étirés par un `mesh()` dont les lignes passent
  entre eux). Depuis M5b aussi, `nom` a `fond_fonction` et
  `texte_fonction` (la cartouche et le texte de la seconde ligne, noir et
  blanc par défaut : rien ne change sans charte), et `logo` ses marges
  en props, `marge_gauche`, `marge_droite`, `marge_haut` et `marge_bas`
  (6 % partout, et en 9:16 celles de la zone sociale : 9 % à gauche, là
  où commence la carte de `nom`, 14 % à droite, avant le rail de TikTok,
  15 % en haut et 36 % en bas, pour que le picto reste dans la zone dans
  ses quatre coins) ;
- la bibliothèque de l'utilisateur. Depuis MB10, c'est une base SQLite
  personnelle, `~/Library/Application Support/CUTMACHINE/library.sqlite`
  (à côté de l'archive des projets, mais dans son fichier à elle), qui
  garde chaque version de chaque pièce et de chaque kit. Elle grandit sans
  compilation ; c'est un réglage local, jamais écrit dans un projet, et
  elle ne part chez personne d'autre.

**Le dossier, boîte de dépôt.** `~/.local/share/cutmachine/motion-library`
(où MB1 rangeait la bibliothèque) reste le format d'échange : un
`<nom>.json` par pièce, un `kits/<nom>.json` par kit. À chaque lecture de
la bibliothèque, un fichier dont la base n'a pas encore le contenu pour ce
nom y entre comme sa version suivante ; le moteur ne modifie ni ne retire
jamais rien du dossier. Un générateur écrit donc son fichier là, et une
bibliothèque d'avant MB10 entre dans la base à la première lecture.
`$CUTMACHINE_MOTION_LIBRARY` nomme un autre dossier : il est alors la
boîte de dépôt, et la base est `library.sqlite` dedans. Les versions ne
changent jamais une fois écrites (la base refuse toute modification et
toute suppression) ; l'étagère liste la dernière de chaque nom.

`list_motion_templates` donne pour chaque pièce sa `source` (`shipped` ou
`library`), sa `version` pour une pièce de la bibliothèque, et `use`,
`avoid`, `invariants` et `origin` quand elle les porte ; `library.base`
nomme la base, `library.directory` la boîte de dépôt, `library.imported`
ce que cette lecture vient d'y prendre, et `library.issues` les fichiers
qui ne sont ni des pièces ni des kits, avec la raison (et une base
inutilisable, auquel cas l'étagère montre le dossier seul). Une pièce de
bibliothèque ne remplace jamais un gabarit livré : le fichier est signalé.

**Chercher dans l'étagère** (MOTION-WEB-2026-10, DEC11, 24 octobre 2026) :
`list_motion_templates {query}` ne rend que les pièces dont le nom, la
description, `use`, `avoid`, les invariants ou `origin` contiennent chacun
des mots de `query`, comme sous-chaînes, sans casse ni accents (la règle de
`search_shots` : « entree ECRAN » trouve « Entrée à l'écran », « lower
third » trouve `lower-third`), dans l'ordre de l'étagère. La réponse dit
alors `query` et `total`, la taille de l'étagère entière ; une recherche
vaine rend une liste vide. Les kits sont listés dans tous les cas. Sans
`query`, l'étagère vient entière, comme avant.
`add_motion_template` prend dans les deux étagères, et sa réponse redonne
les invariants à qui va adapter. Le projet en garde une copie et ne dépend
plus de la bibliothèque.

**Les kits de charte (MB4).** Une pièce livrée est neutre ; la charte d'un
client s'y pose au moment où on l'ajoute. Un kit est une entrée de la
bibliothèque, déposée comme `kits/<nom>.json`, jamais dans le dépôt :

```json
{
  "format": "motion-kit.v1",
  "description": "Ce qu'est la charte, pour qui la choisit.",
  "principle": "Le principe graphique, en phrases : ce qu'aucune valeur ne dit.",
  "props": { "accent": "#0A7C66", "texte": "#FFFFFF", "fond": "#000000", "police": "Exemple Sans" }
}
```

Les pièces partagent quatre noms de props : `accent`, `texte` et `fond`
(couleurs) et `police` (texte, lié à `fontFamily`) ; un kit ne règle que
ceux-là, et un autre nom, une couleur illisible ou une police vide sont
refusés. Les six pièces livrées qui écrivent du texte ont la prop `police`,
Helvetica Neue par défaut. `list_motion_templates` donne les `kits` avec
leurs valeurs ; `add_motion_template {template, kit}` les fait devenir les
défauts des props de ces noms et leur valeur dans chaque format qui les
règle, la valeur propre d'une version (`variants`) restant la sienne. La
réponse dit sous `kit` ce qui a été réglé (`applied`), ce que la pièce n'a
pas (`not_in_piece`) et le `principle`. Refus : `UnknownKit`, et
`KitMismatch` quand une pièce donne à l'un de ces noms un autre type. Le
projet garde les valeurs, pas le kit.

**La charte de marque (DEC6, 24 octobre 2026).** Le kit s'étend en charte :
un fichier par marque, au même endroit (`kits/<marque>.json`, dans la
bibliothèque, jamais dans le dépôt), qui couvre aussi les légendes et
toutes les couleurs et polices permises, pour qu'un export se contrôle
contre elle (`check_timeline`, contrôle `charter`). Un nouveau format plutôt
que de nouveaux champs : un binaire d'avant DEC6 refuse le fichier au lieu
d'en appliquer la moitié sans rien contrôler. Un `motion-kit.v1` se lit
toujours, comme une charte de ses props seules.

```json
{
  "format": "brand-kit.v1",
  "description": "Ce qu'est la marque, pour qui la choisit.",
  "principle": "Le principe graphique, en phrases.",
  "props": { "accent": "#0A7C66", "texte": "#FFFFFF", "fond": "#0A7C66", "police": "Exemple Sans" },
  "palette": ["#E0A030", "#000000"],
  "fonts": ["Exemple Sans"],
  "captions": {
    "font_family": "Exemple Sans", "color": "#FFFFFF",
    "background_color": "#0A7C66", "background_padding": 16,
    "formats": {
      "9x16": { "width": 1080, "height": 1920, "font_size": 64, "vertical_position": { "num": 7, "den": 10 } },
      "16x9": { "width": 1920, "height": 1080, "font_size": 56 }
    }
  },
  "sources": ["Charte 2024, p. 3 : les couleurs.", "…"]
}
```

`props` est facultatif dans une charte (une marque peut ne régler que ses
légendes), mais une charte règle quelque chose. `palette` liste des
`#rrggbb` : l'opacité est celle du film. `captions` prend les réglages
d'`add_caption_style`, ses couleurs en `#rrggbb` ou `#rrggbb@a` (ce que
lisent à la fois l'export et le contrôle) ; `formats` y donne par nom une
largeur, une hauteur et ce qui change à cette taille. Refus nommés : un
champ de charte dans un `motion-kit.v1` (avec le format à écrire), une
couleur de palette qui n'est pas `#rrggbb`, une police vide, un réglage de
légende que le style n'a pas ou une valeur qu'il refuse, un format de
légende sans sa taille ou de la taille d'un autre, une charte qui ne règle
rien. `sources` dit d'où vient chaque valeur : une charte qu'on ne peut pas
retracer est une supposition. Ce que la charte permet : les couleurs de ses
props, de sa palette et de ses légendes, les polices de `police`, de
`fonts` et de ses légendes. `list_motion_templates` donne pour chaque kit
son `format`, et pour une charte ses `colors`, `fonts`, `captions` et
`sources` ; `add_motion_template {kit}` en pose les props comme avant ;
`add_caption_style {kit}` en fait un style de légende à la taille de la
timeline.

**Ce que la charte donne à une pièce en particulier** (M5b, 7 novembre
2026, MOTION-2026-09). Les quatre rôles
partagés ne disent pas tout d'une marque : l'une veut la seconde ligne de
`nom` en vert sur blanc, et une version compacte de son logo. Plutôt que de
changer le gabarit pour tous, la charte le dit pour elle :

```json
"pieces": {
  "nom":  { "fond_fonction": "#FFFFFF", "texte_fonction": "#0A7C66" },
  "logo": { "logo": "/Users/moi/Marques/marque/logo-compact.png" }
}
```

Par nom de pièce, les valeurs de ses props propres : une chaîne, un nombre
ou un booléen, ce qu'une prop de ce type prend. À la lecture de la charte,
seule la forme est vérifiée (un nom de pièce valide, une section non vide,
une valeur non vide, un rôle partagé qui garde son type) ; à l'ajout de
cette pièce (`add_motion_template {kit}`), chaque valeur est confrontée à la
prop qu'elle nomme : une prop que la pièce n'a pas, une couleur illisible,
un nombre hors bornes, un mot hors d'un `enum` sont refusés par leur nom
(`KitMismatch`) — une charte écrite pour une autre version de la pièce se
dit, elle ne s'applique pas à moitié. La valeur devient le défaut de la
prop et sa valeur dans chaque format qui la règle, comme un rôle ; une
valeur propre à la pièce l'emporte sur le rôle de même nom. Les couleurs
de `pieces` comptent parmi celles que la charte permet (le contrôle
`charter` les accepte), la `police` d'une pièce parmi ses polices ; un
chemin de fichier, lui, n'est rien de plus qu'une valeur. Un fichier
(le logo) s'écrit en chemin absolu, ou relatif au paquet du projet si on
l'y copie : le moteur ne le copie pas. `list_motion_templates` redonne
`pieces` pour chaque charte qui en a ; `motion-kit.v1` ne les prend pas.

Une charte retire aussi une pièce, avec sa raison, dans les mots de qui
l'a décidé :

```json
"withdrawn": {
  "carton": "« pas besoin », l'auteur, 6 novembre 2026",
  "maillage": "retiré à la planche du 6 novembre 2026"
}
```

`add_motion_template` refuse alors la pièce avec cette charte
(`WithdrawnByKit`, la raison dans le message) ; sans la charte, elle
s'ajoute, en connaissance de cause. Une pièce n'est pas à la fois retirée
et réglée dans `pieces`, et une raison vide est refusée.
`list_motion_templates` redonne `withdrawn` sous la charte.

`save_motion_template {composition_id, template, description, use?,
avoid?, invariants?, origin?, overwrite?}` est le chemin de retour : une
composition du projet devient une pièce de la bibliothèque. Ses identités
sont retirées ; si elle ne déclare aucun format de sa propre taille, un
format de cette taille est ajouté (`9x16`, `16x9`, `1x1`, `4x5`, sinon
`<largeur>x<hauteur>`), pour que la timeline d'où elle vient puisse la
reprendre. Refus : `ProjectBound` si un nœud joue un média du projet écrit
par son ULID ou instancie une autre composition — un `media` lié à une
prop et un fichier joué par `src` passent — ; `TemplateExists` si le nom
est celui d'un gabarit livré, ou
d'une pièce de bibliothèque sans `overwrite`. Avec `overwrite`, la pièce
devient la version suivante de ce nom, les précédentes restent dans la
base ; la réponse donne `base` et `version`. L'outil écrit hors du projet
et ne touche pas au document : ce n'est pas une opération, il n'y a rien
à annuler.

Le placement et le contrôle qui suivent valent pour toute composition,
venue d'une pièce ou non :

- `place_motion_clip` pose une instance là où on la nomme, et le moteur
  calcule la plage et la piste : sur un plan (`clip_id`), sur un passage
  transcrit (`span_id`, et `end_span_id` pour le borner, pris dans
  `get_timeline_transcript` ; ou `block_id`, et `end_block_id`, une phrase
  entière de `list_speech_blocks`), ou au début ou à la fin du film (`at`).
  Un passage transcrit vient avec la `transcript_revision` de sa lecture ;
  une lecture périmée est refusée (`StaleSelection`, IA3).
  Sur un **temps de la musique** (`beat : {media_id, index}`, l'index que
  donne `list_tempo_grid`, avec son `source_in`/`source_out` s'il avait une
  fenêtre) : le moteur trouve l'image où ce temps s'entend
  (`TempoBeatFramesOnTimeline`) et l'instance y démarre comme avec
  `at: "start"` ; l'agent n'a aucun temps à calculer. Refus
  `UnknownBeat` si le temps n'est pas dans la grille ou ne s'entend nulle
  part ; `AmbiguousBeat`, avec les images, si la musique est jouée deux
  fois : `beat.clip_id` désigne alors la lecture voulue.
  `offset` décale le début dans l'ancre. Un mot commence entre deux images :
  l'instance démarre sur l'image qui contient le début du premier mot, et,
  bornée par le passage, finit sur celle qui contient la fin du dernier.
  Durée par défaut : tout le passage s'il est borné (un bloc l'est
  toujours, sauf durée donnée), sinon autant de la
  composition que le permet la fin du plan qui joue l'ancre (celle du film
  pour `at`) — un nom ne survit pas au plan de la personne qu'il nomme. Un
  plan, ici, c'est le clip d'ancre et les clips de la même source qui le
  suivent sans trou sur sa piste : un jump cut ne coupe pas un nom.
  Piste par défaut : la plus basse piste vidéo au-dessus de l'ancre qui soit
  libre sur la plage. **Rien ne bouge** sur la timeline : une plage prise
  est refusée (`NoFreeTrack`), et quand un clip suit sur la piste, le clip
  est inséré après le dernier puis déplacé, en une seule annulation.
- **Avant de poser**, chaque image que montre l'instance est mise en page
  avec les vraies polices et contrôlée. Refus `OutsideSafeArea` : un texte,
  une image ou un fond sort de la zone de sécurité (5 % du cadre par côté
  par défaut ; `margin_percent` pour une autre marge égale ;
  `zone: "social_vertical"` pour une vidéo verticale de Reels ou TikTok :
  14 % en haut, 35 % en bas, 6 % à gauche et le bord droit à 940 px sur
  1080, en proportion du cadre ; `margins: {top, bottom, left, right}` pour
  des marges par côté, entiers de 0 à 45 ; mêler les trois est refusé).
  **Sur une timeline 9:16, sans aucun des trois, la zone est
  `social_vertical`** (DEC34, décidé par l'auteur le 23 octobre 2026) : le
  refus le dit (« the default on a 9:16 timeline ») et le rapport porte
  `zone_chosen_by: "timeline_9x16"`. `zone: "default"` redemande les 5 %.
  Le 9:16 se lit exactement (largeur × 16 = hauteur × 9 : 1080×1920,
  2160×3840, 720×1280) ; le 4:5 et toute autre taille gardent les 5 %.
  Refus `TextDoesNotFit` : un mot
  trop long pour sa boîte, que Core Text a coupé entre deux lettres. Le
  message nomme l'élément, le bord et les images ;
  `allow_layout_issues: true` pose quand même, rapport joint.
- `check_motion_safe_area {clip_id, zone?, margin_percent?, margins?}`
  refait ce contrôle sur un clip déjà posé, après `set_clip_motion_props` ou
  `edit_motion_composition`, avec le même défaut (`social_vertical` sur une
  timeline 9:16). En CLI, les quatre outils passent par
  `--call-tool`. Le rapport nomme la zone appliquée (`zone`, `margins`).
- MM3 -- `check_motion_safe_area {composition_id, props?, variant?,
  format?, …}` contrôle une composition **rendue seule** (celle de
  `render_motion_video`), chaque image, version et format résolus comme au
  rendu ; en CLI, `--check-motion-safe-area <composition.json|projet>
  [--composition <id>] [--variant] [--format] [--props] [--zone
  social_vertical | --margin-percent <n>] [--at-rest]`, `ok` en verdict et
  en code de sortie. Une composition rendue seule n'a pas de timeline :
  sans zone, elle garde les 5 % quelle que soit sa taille (DEC34 nomme une
  timeline ; un film vertical n'est pas toujours vu sous une appli, un
  écran de salon par exemple). Chaque sortie dit si l'élément se tenait immobile
  (`at_rest` : sa boîte celle de l'image d'avant) ; `at_rest: true`
  (`--at-rest`) ne compte que celles-là, les passages (une page qui glisse,
  un titre qui part) comptés à part dans `in_motion`. Un nœud sans `id` ni
  ULID (une composition lue d'un fichier) est nommé par son chemin
  (`path`). Un film de marque en 9:16 passe la zone des réseaux au repos,
  comme un script l'avait vérifié avant que le contrôle existe.

Ce qui compte dans le contrôle : ce qu'un spectateur voit, soit une
opacité cumulée d'au moins ½ (un titre qui glisse pendant son fondu
d'entrée n'est pas en faute), transformations appliquées comme le rendu les
applique. Un fond qui touche le bord du cadre d'un côté y déborde exprès
(carton plein cadre, bandeau pleine largeur) et n'est pas tenu à la marge
de ce côté ; un fond qui s'arrête entre la marge et le bord l'est. Non
contrôlés : tracés, découpes, et transformations propres aux mots ou
caractères animés, dont la ligne est contrôlée à la place. Une ligne que le
texte coupe lui-même (un retour à la ligne, une espace) n'est pas un mot
coupé : « 12 480↵9 731 » n'est pas « 480|9 ». Un nœud `decor` (M5b) et ce
qu'il contient ne sont pas jugés, et le rapport les nomme sous `decor` :
les échos du `bandeau`, que le cadre coupe exprès, ne masquent plus
l'occurrence du milieu, la seule qu'on lit, et c'est elle qui est refusée
quand la phrase ne tient pas (le 6 novembre 2026, l'auteur : « il sort de
l'écran »). Ce qu'une boîte à `overflow: hidden` cache ne compte pas : un
élément est jugé sur la part que ses boîtes et le cadre en laissent, et un
élément dont elles ne laissent rien (la seconde bande qui attend au-delà du
bord avant d'entrer, le texte qu'elle cache encore) n'est ni en faute, ni
compté en passage.

## Absent volontairement

- Effets raster au-delà de la pile de REM3 (prisme, couleurs, turbulence
  fractale) ; `mix-blend-mode` avec l'image **sous** un clip de motion : une
  composition se rend avec son alpha, puis se pose sur la timeline par cet
  alpha seul.
- Expressions, scripts, accès au temps système ou au réseau.
- Import de projets After. Lottie : écarté le 28 septembre faute d'usage,
  rouvert le 2 octobre comme **import** vers l'arbre du format, jamais comme
  lecteur (MB7, MOTION-BIBLIOTHEQUE-2026-10).

## Décisions de M1

- Noms CSS stricts : `paddingX`/`paddingY` refusés, raccourci CSS `padding`
  (`"4px 10px"`) accepté. Une propriété inconnue échoue avec son chemin.
- Couleurs en chaînes CSS analysées par le moteur : `#rgb`, `#rrggbb`,
  `#rrggbbaa`, `rgb()`, `rgba()`, `transparent`, `white`, `black`.
- Les keyframes d'`interpolate` sont des keyframes adressées par ULID
  (`Keyframes.h`) : les tableaux `frames`/`values` sont une écriture, pas un
  adressage. M1 dérive des identifiants synthétiques ; M3 les rend persistants.

## Décisions de M1b

- Unités animées : **mêmes champs `animate`** que le nœud, appliqués à chaque
  unité, limités à ce qui ne change pas la mise en page. Le bloc entier
  s'anime par une boîte englobante.
- Couleurs interpolées en sRGB prémultiplié (CSS) ; polygones sommet par
  sommet, à nombre de sommets constant, px et % résolus avant de mélanger.
- `timeScale` en fraction exacte ; une prop numérique à unité `%` ou `px`
  se lit comme une longueur CSS.
- `repeat` n'a pas de boîte : ses copies prennent sa place parmi ses frères
  (comme un tableau rendu en React). Les `id` du gabarit reçoivent le
  suffixe de la copie (`bandeau[2]`), les chemins `template[2]`.
- Noms hors CSS assumés : `trimStart`/`trimEnd` (SVG n'a que
  `stroke-dasharray`), `textStroke` (CSS : `-webkit-text-stroke`, même
  syntaxe `"2px #fff"`), `timeScale` (Remotion n'en a pas).
- Tailles plafonnées : 1 000 copies par `repeat`, 1 000 éléments par liste.

## Décisions de M3

- Compositions au niveau du projet, comme la bibliothèque, pas de la
  timeline : M4 (clip dont la source est une composition, sur n'importe
  quelle timeline) et M6 (versions nommées, formats) s'y ajoutent sans
  déplacer ce qui est stocké.
- Les valeurs de props posées par `SetMotionProps` sont les `default` de la
  composition, comme les `defaultProps` d'une composition Remotion. Les
  props d'instance de M4 iront sur le clip, les jeux nommés de M6 sur la
  composition (`variants`, déjà lu et non évalué), validés par la même
  fonction.
- La définition reste l'arbre du format, et non des structures du
  document : le vocabulaire motion grandit sans version du document, et un
  binaire qui ne connaît pas un mot nouveau refuse la composition au lieu de
  l'ignorer.
- Une animation s'adresse par l'`uid` de son nœud et le nom de sa propriété
  (une seule par propriété) ; ses clés, qui sont les éléments d'un tableau,
  portent chacune un `uid`.
- `point`, `size` : objets lus composante par composante, comme en
  JavaScript. `group` : présentation seulement.

## Décisions de M6

- Versions et formats sont deux axes qui se croisent au rendu, déclarés dans
  la composition, et non des tailles passées à la commande : le document
  reste la vérité, et la même commande rend les mêmes fichiers sur une autre
  machine.
- Un format porte des props : c'est la seule façon, en données et sans
  calcul, de changer un corps avec le cadre (la carte de référence de M2 a
  un corps presque double en 16:9). Des unités `vw`/`vh`/`vmin` en seraient
  une autre, qui toucherait tout le vocabulaire : non retenue.
- Une prop réglée par un format et par une version est refusée, plutôt que
  départagée par un ordre implicite.
- Aucune opération ni champ du document nouveaux : `UpdateMotionComposition`
  porte le changement, les outils MCP composent la définition.
- Rendu par une suite PNG et `Exporter::Run`, plutôt qu'un tube vers
  FFmpeg : on réutilise le lancement, l'annulation et surtout la validation
  du fichier final (exFAT) de l'export sans les réécrire, au prix d'un
  encodage PNG par image (quelques dizaines de ko par carton en 1080×1920).

## Décisions de M4

- Le clip de motion est un `DocumentClip` et non un nouvel objet : il se
  coupe, se rogne, se déplace et reçoit opacité et fondus par les
  opérations qui existent, sans en dupliquer une.
- La composition est une source : sa durée en images à sa cadence borne le
  clip. Tenir la dernière image au-delà aurait été possible, mais aurait
  inventé une règle que ni un média ni Remotion n'ont.
- Pas de version (M6) sur un clip : des props d'instance l'expriment déjà ;
  nommer une version sur un clip reste une piste.
- Suite PNG à l'export, comme M6 : un tube `rawvideo` par clip aurait
  demandé un fil d'écriture par entrée, et un fichier brut pèse 8 Mo par
  image 1080×1920 ; un PNG de carton en pèse une cinquantaine de ko et coûte
  9,5 ms à produire.

## Décisions de M5

- Les sous-titres incrustés ne sont pas un gabarit : ce sont les pistes de
  légendes (`generate_captions`), déjà découpées par les règles du projet,
  déjà comptées contre les cues et incrustées à l'export. Un gabarit de plus
  aurait été une seconde façon de faire la même chose.
- Un gabarit est instancié à la taille de la timeline plutôt que posé avec
  ses formats : une composition du projet a une taille, et un clip ne choisit
  pas de format (la question des versions sur un clip reste ouverte, M4).
- La cadence du gabarit (25 images/s) est gardée : changer la cadence
  changerait la vitesse de toutes les animations, qui comptent en images.
  Sur une séquence à une autre cadence, l'image montrée est celle qui
  contient l'instant, comme pour tout média.
- Le contrôle précède l'opération et refuse, plutôt que de poser et
  signaler : l'agent qui lit un refus corrige la prop ; celui qui lit un
  avertissement dans un résultat réussi le laisse passer.
- La coupe d'un mot se lit dans la mise en page : une ligne qui ne finit ni
  sur une espace, ni sur un trait d'union ou un tiret, a été coupée entre
  deux lettres. C'est la question ouverte ci-dessous, désormais signalée à
  défaut d'être évitée.

## Questions ouvertes

- `boxDecoration` n'est pas du CSS standard (`box-decoration-break`) : à
  trancher avec le fond par ligne.
- `layoutFrom` (ancrage à la boîte d'une unité) : les boîtes sont exposées,
  l'ancrage reste à faire.
- `marker` (pointe de flèche orientée) : refusé tant qu'il n'est pas dessiné.
- Un texte dans une boîte sans largeur (une boîte qui n'enveloppe qu'une
  séquence, par exemple) est coupé caractère par caractère par Core Text, là
  où CSS ne couperait qu'aux espaces. M5 le signale (`word_split`) sans
  l'éviter.
- ~~L'alpha du ProRes 4444 est droit (non prémultiplié) : à vérifier dans
  Resolve~~ — vérifié le 20 septembre 2026 sur deux montages : Resolve 20.3
  lit « Alpha mode : Straight » tout seul, sans réglage. Reste à écrire :
  en sortie HLG, un `#FFFFFF` sort au pic du signal alors que la référence
  pour le blanc graphique est 75 % (`#BFBFBF`).
