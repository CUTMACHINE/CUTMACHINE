# Motifs — du skill Remotion au format de CUTMACHINE

Chaque motif : ce qu'il fait, son écriture dans le format de composition
([docs/MOTION_FORMAT.md](../../../../docs/MOTION_FORMAT.md)), ce qui change
par rapport à Remotion. Les courbes d'entrée, de sortie et de déplacement
sont celles des règles de SKILL.md ; en plus :

| Nom | Valeur | Usage |
|---|---|---|
| surcourse (marque, jeu) | `cubic-bezier(0.34, 1.56, 0.64, 1)` | pop avec dépassement, à doser |

Ressorts, équivalents des préréglages de l'original :

| Nom | `spring` | Usage |
|---|---|---|
| vif | `"damping": 14, "stiffness": 160, "mass": 0.6` | mots, petits éléments |
| doux | `"damping": 20, "stiffness": 90, "mass": 1` | grands éléments |
| rebond | `"damping": 11, "stiffness": 170, "mass": 0.7` | accents, logo |

Un `spring` n'anime qu'un nombre : l'opacité passe par un `interpolate`.

## Pièges du moteur

Rencontrés en vrai, et que le moteur ne refuse pas :

- Une police de titre peut n'avoir que des chiffres bas de casse
  (« 1 » lu « I ») : `fontVariantNumeric: "lining-nums"`,
  `tabular-nums` pour un compteur.
- Un `easing` unique vaut pour tous les segments : avec entrée, tenue et
  sortie, donne une liste (motif 11).
- **Un `spring` tient sa valeur `from` avant son `delay`.** Pratique pour
  attendre hors cadre. Sa surcourse fait déborder l'élément sur son voisin :
  dans une pile, `damping` d'au moins 18 à 21.
- **Un texte dans une boîte de hauteur fixe ou animée rétrécit** (flexbox,
  `flexShrink` à 1 par défaut) : ses lignes se chevauchent. Mets
  `"flexShrink": 0` sur le texte.
- Un texte ne dépasse jamais la hauteur de sa boîte (il déborde par le bas,
  un `filter` le coupe) : pour gonfler une ligne, anime `scaleY`, pas la
  boîte (motif 24).
- **Une boîte est en ligne par défaut**, comme en CSS : `justifyContent`
  y pousse à gauche ou à droite. Pour pousser en bas, `flexDirection:
  "column"`.
- **Une `sequence` est un calque plein cadre** posé dans son parent : elle
  ignore le `padding` du parent. Mets une boîte à marges dans la séquence.
- Un geste qui dépend de l'instance se lie à une prop dans `values`
  (`{"prop": …}`, avec `map`), pas en copies du nœud ; `frames`, `easing`
  et `spring` restent écrits.
- Une police variable n'expose qu'une graisse : un fichier statique par
  graisse.

## Sommaire

1. Entrée à trois propriétés
2. Cascade
3. Mot à mot, avec un mot fort
4. Fond maillé
5. Étalonnage
6. Grain
7. Vignette
8. Ken Burns
9. Compteur
10. Logo
11. Sorties
12. Respiration
13. Parallaxe
14. Transitions
15. Flou de mouvement
16. Son et temps
17. Sous-titres

Propres à CUTMACHINE :

18. Calage sur les coupes
19. Crans
20. Fenêtre et texte-image
21. Kinetic typo : fente, cartouche comédien, bandeau, trappe
22. Déclinaisons formats × versions
23. Habillage pour Resolve
24. Kinetic warp : le maillage d'After

---

## 1. Entrée à trois propriétés

```json
"animate": {
  "opacity":    { "interpolate": { "frames": [18, 26], "values": [0, 1], "easing": "cubic-bezier(0.16, 1, 0.3, 1)" } },
  "translateY": { "spring": { "from": 40, "to": 0, "delay": 18, "damping": 20, "stiffness": 90, "mass": 1 } },
  "scale":      { "spring": { "from": 0.94, "to": 1, "delay": 18, "damping": 20, "stiffness": 90, "mass": 1 } }
}
```

Pour une glissade contrôlée plutôt qu'un ressort : un `interpolate` de
`translateX` avec la courbe d'entrée. Exemple :
[entree-cascade-sortie.json](../exemples/entree-cascade-sortie.json).

## 2. Cascade

Un texte se découpe en `line`, `word` ou `char` et chaque unité reçoit
l'animation, décalée :

```json
{ "type": "text", "content": "…", "units": "word", "stagger": { "each": 3 },
  "animate": { "opacity": { "interpolate": { "frames": [3, 9], "values": [0, 1],
                                             "easing": "cubic-bezier(0.16, 1, 0.3, 1)" } },
               "translateY": { "spring": { "from": 30, "to": 0, "delay": 3,
                                           "damping": 14, "stiffness": 160, "mass": 0.6 } } } }
```

Au-delà d'une vingtaine d'images pour tout le groupe, la queue
traîne : réduis le pas. Des boîtes sœurs se décalent par leurs propres clés,
ou par `repeat` et son `stagger`.

## 3. Mot à mot, avec un mot fort

La cascade du motif 2 en `word`, plus une plage :

```json
"ranges": [ { "unit": "word", "start": { "prop": "mot_fort" }, "count": 1,
              "style": { "color": { "prop": "accent" } } } ]
```

La plage suit la prop : aucun index à recalculer quand le texte change. Un
seul mot fort par titre. Le halo de l'original (`textShadow`) s'écrit
`"filter": "glow(20px #FFD23F 2)"` sur une boîte qui ne tient que ce mot.

**Le surligneur** (MB3, 24 octobre 2026) : un trait de feutre sous une
plage de mots, d'un seul tenant par ligne, espaces compris, qui avance de
gauche à droite et passe à la ligne. Ce n'est pas une plage à fond
(`backgroundColor` peint une boîte dure par mot, espaces nus) :

```json
"highlights": [ { "unit": "word", "start": { "prop": "mot_fort" }, "count": 2,
                  "color": { "prop": "accent" }, "top": 0.5, "bottom": 0.95, "bleed": 8,
                  "progress": { "interpolate": { "frames": [6, 24], "values": [0, 1],
                                                 "easing": "power2.out" } } } ]
```

Texte sombre sur un trait clair : un texte blanc sur du jaune ne se lit
plus.

## 4. Fond maillé

Deux taches floues qui dérivent lentement sous le contenu, jamais un aplat
mort en plein cadre :

```json
{ "type": "box", "style": { "position": "absolute", "top": -550, "left": -350, "width": 1400, "height": 1400,
    "borderRadius": 1400, "filter": "blur(60px)",
    "backgroundImage": "radial-gradient(circle, #7C3AED33 0%, transparent 62%)" },
  "animate": { "translateX": { "interpolate": { "frames": [0, 75], "values": [0, 50],
                                                "easing": "cubic-bezier(0.65, 0, 0.35, 1)" } } } }
```

L'original fait dériver par un sinus : pas d'expression ici, une
interpolation lente en déplacement suffit. **Pas en flat design**, pas sous
un habillage transparent. Exemple : [plein-cadre.json](../exemples/plein-cadre.json).

## 5. Étalonnage

Au-dessus du contenu, sous le grain : un voile de la couleur forte en
lumière douce, et un léger assombrissement haut et bas.

```json
{ "type": "fill", "style": { "backgroundColor": "#7C3AED", "mixBlendMode": "soft-light", "opacity": 0.18 } },
{ "type": "fill", "style": { "backgroundImage": "linear-gradient(180deg, rgba(0, 0, 0, 0.10) 0%, transparent 28%, transparent 72%, rgba(0, 0, 0, 0.2) 100%)" } }
```

Opacité 0,10 à 0,15 sur fond clair, 0,18 à 0,25 sur fond sombre.

## 6. Grain

- Plein cadre : `"grain": { "amount": 0.05, "size": 2, "seed": 3 }` au niveau
  de la composition, nouveau à chaque image.
- Sur un calque transparent, par nœud :
  `"filter": "grain(0.03 2px 7 0)"`, et la phase animée d'une unité par
  image pour qu'il vive :
  `"filter": { "interpolate": { "frames": [0, 75], "values": ["grain(0.03 2px 7 0)", "grain(0.03 2px 7 75)"] } }`.

Pas de grain en flat design.

## 7. Vignette

La couche du dessus :

```json
{ "type": "fill", "style": { "backgroundImage": "radial-gradient(ellipse at 50% 50%, transparent 56%, rgba(0, 0, 0, 0.22) 100%)" } }
```

## 8. Ken Burns

Toute image fixe dérive, en courbe de déplacement, sur la durée du plan :

```json
{ "type": "box", "style": { "width": 860, "height": 860, "borderRadius": 32, "overflow": "hidden" },
  "children": [ { "type": "image", "src": "photo.jpg", "style": { "width": "100%", "height": "100%", "objectFit": "cover" },
    "animate": { "scale":      { "interpolate": { "frames": [0, 75], "values": [1, 1.08], "easing": "cubic-bezier(0.65, 0, 0.35, 1)" } },
                 "translateX": { "interpolate": { "frames": [0, 75], "values": [0, -25], "easing": "cubic-bezier(0.65, 0, 0.35, 1)" } } } } ] }
```

Alterne zoom avant et arrière d'une image fixe à l'autre, pas d'un plan de
montage à l'autre (skill `monter-un-reel`). Une vidéo passe par un
nœud `video` (`media` par ULID, ou `src`), à l'image près ; `muted: true`
si son son ne doit pas jouer.

## 9. Compteur

Le format n'interpole pas un nombre dans un texte (le seul contenu calculé
est le timecode). Un compteur se fait donc en **rouleaux** : chaque chiffre
est une colonne `0…9` (deux tours, pour qu'il tourne vraiment) dans une
fenêtre de la hauteur d'une ligne, qui défile jusqu'à son chiffre ; les
rouleaux s'arrêtent en cascade.

```json
{ "type": "box", "style": { "height": 220, "overflow": "hidden" }, "children": [
  { "type": "text", "content": "0\n1\n2\n3\n4\n5\n6\n7\n8\n9\n0\n1\n2\n3\n4\n5\n6\n7\n8\n9",
    "style": { "fontSize": 220, "lineHeight": 1.0, "textAlign": "center", "flexShrink": 0 },
    "animate": { "translateY": { "interpolate": { "frames": [4, 35], "values": [0, -2640],
                                                  "easing": "cubic-bezier(0.16, 1, 0.3, 1)" } } } } ] }
```

`-2640` = −(10 + 2) × 220 : le chiffre 2 au deuxième tour. En `step-end`
avec une clé par image, c'est un compteur mécanique. Exemple :
[compteur.json](../exemples/compteur.json).

## 10. Logo

Une seule technique par révélation : construire (les morceaux en cascade),
glisser (le logo monte dans une boîte `overflow: hidden`, comme un mot dans
son cartouche), ou apparaître d'une échelle **uniforme**. Jamais déformé,
incliné ni recoloré ; la zone de protection de la charte reste vide de
mouvement. Un logo livré en vectoriel (`.ai`, `.pdf`, `.svg`) se rastérise
en PNG transparent à au moins deux fois sa taille d'affichage.

Des rayons autour d'un centre (l'étincelle de l'original) : aucun calcul
n'est possible dans la composition, et une table ne s'applique pas à
`{"index": true}` (refus `InvalidValue` : « map applies to item, not to
index »). Les angles viennent donc d'une prop `list` parcourue par
`repeat` ; et, faute de `transform-origin`, chaque rayon est une barre deux
fois plus longue, **colorée sur sa moitié haute seulement**, qui tourne
autour de son centre : le rayon part du centre commun.

```json
"props": { "angles": { "type": "list", "of": "number", "default": [0, 45, 90, 135, 180, 225, 270, 315] } },
…
{ "type": "box", "style": { "position": "absolute", "left": 540, "top": 540 }, "children": [
  { "type": "repeat", "each": { "prop": "angles" }, "stagger": { "each": 1 },
    "template": { "type": "box",
      "style": { "position": "absolute", "left": -12, "top": -200, "width": 24, "height": 400,
                 "backgroundImage": "linear-gradient(180deg, #FFD23F 0%, #FFD23F 38%, transparent 38%)",
                 "rotate": { "item": true } },
      "animate": { "scaleY": { "spring": { "from": 0, "to": 1, "damping": 14, "stiffness": 160, "mass": 0.6 } } } } } ] }
```

Le `scaleY` part du centre de la barre : la moitié colorée pousse du centre
vers l'extérieur, un rayon par image.

La révélation finit sur un accent de la musique : le logo est en place, à
pleine opacité, **sur** le temps, pas après.

## 11. Sorties

Plus rapides que les entrées, en accélération, sur une boîte qui enveloppe
l'élément déjà entré :

```json
{ "type": "box", "id": "sortie",
  "animate": { "translateY": { "interpolate": { "frames": [63, 72], "values": [0, -42], "easing": "cubic-bezier(0.7, 0, 0.84, 0)" } },
               "opacity":    { "interpolate": { "frames": [63, 72], "values": [1, 0], "easing": "cubic-bezier(0.7, 0, 0.84, 0)" } } },
  "children": [ … l'élément et son entrée … ] }
```

Quand l'entrée et la sortie animent la même propriété du même élément, une
seule animation suffit, avec une courbe par segment (la liste a une courbe
de moins que de clés ; la tenue prend `linear`) :

```json
"animate": { "translateY": { "interpolate": { "frames": [18, 26, 63, 72], "values": [40, 0, 0, -42],
  "easing": ["cubic-bezier(0.16, 1, 0.3, 1)", "linear", "cubic-bezier(0.7, 0, 0.84, 0)"] } } }
```

La boîte enveloppante garde son usage : sortir d'un bloc plusieurs
éléments entrés chacun à son heure.

## 12. Respiration

Par défaut, pas de respiration (règle 6 de SKILL.md) : un fond qui doit
vivre dérive lentement (motif 4), il ne bat pas.

## 13. Parallaxe

Trois plans, la même courbe, des amplitudes 0,3 / 0,6 / 1 :

```json
"animate": { "translateX": { "interpolate": { "frames": [0, 90], "values": [0, -36], "easing": "cubic-bezier(0.65, 0, 0.35, 1)" } } }
```

(`-36` pour le fond, `-72` pour le milieu, `-120` pour le premier plan.)

## 14. Transitions

Le format a les transitions de Remotion, entre deux séquences d'une
`series` :

```json
{ "type": "series", "children": [
  { "type": "sequence", "duration": 36, "children": [ … ] },
  { "type": "transition", "presentation": "slide", "direction": "from-right",
    "timing": { "duration": 10, "easing": "cubic-bezier(0.65, 0, 0.35, 1)" } },
  { "type": "sequence", "duration": 36, "children": [ … ] },
  { "type": "transition", "presentation": "fade", "timing": { "duration": 8 } },
  { "type": "sequence", "children": [ … ] } ] }
```

Présentations : `fade`, `slide`, `wipe`, `iris`. Une transition fait se
chevaucher les deux séquences de sa durée. À la main, 8 à 14 images :

- **fouetté** : le contenu entre par `translateX` de 1300 à 0 en 7 images
  pendant que `filter` passe de `blur(12px)` à `blur(0px)` ;
- **traversée d'échelle** : la scène sortante grandit de 1 à 1,3 en
  s'effaçant, l'entrante passe de 0,8 à 1 dessous ;
- **volet à la forme de la marque** : un `clipPath` qui balaie.

Exemple : [transitions.json](../exemples/transitions.json). Avant d'en
mettre, demande : certains commanditaires n'en veulent aucune (« pas de
volet »).

## 15. Flou de mouvement

`"motionBlur": { "samples": 6, "shutterAngle": 180 }` au niveau de la
composition. Il coûte autant de rendus que d'échantillons (six fois plus
long). L'obturateur **s'ouvre sur l'image** et échantillonne vers l'avant :
voir le calage sur les coupes (motif 18). Les clés `step-end` restent nettes.

## 16. Son et temps

- **Jouer** dans une composition : nœud `audio` (`src` ou `media`),
  `volume` gain linéaire de 0 à 4 (0,126 ≈ −18 dB pour un lit musical sous
  une voix), animable pour un fondu.
- **Bruitages** : un nœud `audio` par bruitage, dans une séquence qui
  démarre deux images avant l'image (`direction.md`, « Son »).

## 17. Sous-titres

Pas une composition : les légendes sont des pistes de CUTMACHINE
(`generate_captions`), découpées et vérifiées par les règles du projet et
incrustées à l'export. Voir le skill `monter-une-itw`,
[sous-titres.md](../../monter-une-itw/sous-titres.md).

---

## 18. Calage sur les coupes

Un habillage marque les coupes du montage : chaque entrée **visible sur**
la coupe, chaque sortie **terminée sur** la suivante.

- Ouvre la séquence du moment une image avant la coupe
  (`"from": coupe - 1`), et pose la première clé d'entrée à l'image locale 0 :
  à la coupe (image locale 1), l'élément a déjà fait un sixième de son
  entrée et se voit.
- La sortie finit à l'image locale `coupe_suivante - (coupe - 1)`, soit la
  fin de la séquence.
- Un texte qui suit son cartouche démarre une image **après** lui, jamais
  avant la coupe : sous flou de mouvement, il paraîtrait en fantôme sur le
  plan précédent.
- Vérifie sur planche `coupe - 1`, `coupe`, `coupe + 1`, et les trois
  dernières images de la sortie.

## 19. Crans

Pour qu'une forme avance par pas nets (les lignes d'une grille de charte,
un rideau qui s'ouvre cran par cran) : une clé par pas, `"easing":
"step-end"`. La valeur de chaque clé tient jusqu'à la suivante, le flou de
mouvement ne l'étale pas.

```json
"animate": { "clipPath": { "interpolate": { "frames": [0, 1, 2, 3],
  "values": ["inset(540px 540px 540px 540px)", "inset(486px 486px 486px 486px)",
             "inset(432px 432px 432px 432px)", "inset(378px 378px 378px 378px)"],
  "easing": "step-end" } } }
```

## 20. Fenêtre et texte-image

Deux caches, chacun dans un groupe isolé (`"isolation": "isolate"`) :

- **Fenêtre** (un aplat percé qui laisse voir l'image) : l'aplat, puis une
  boîte `"mixBlendMode": "mask-out"` dont le `clipPath` dessine le trou.
  Sur un calque alpha, le trou est transparent et le montage s'y voit dans
  Resolve.
- **Texte-image** (l'image à travers les lettres) : l'image, puis le texte
  en `"mixBlendMode": "mask-in"`. Ce second cache a besoin de l'image **dans**
  la composition : il ne se fait pas en calque posé sur Resolve.

Exemple : [fenetre-et-texte-image.json](../exemples/fenetre-et-texte-image.json).

## 21. Kinetic typo

Le texte n'apparaît pas : il arrive, pousse, glisse et repart. Une forme de
la charte le porte. Quatre mécaniques, chacune dans un exemple neutre :

- **Fente** ([kinetic-fente.json](../exemples/kinetic-fente.json)) — un mot
  par temps dans une fenêtre fixe (`overflow: hidden`) ; le cartouche de
  chaque mot pousse le précédent vers le haut (même clé de temps : l'un
  sort de −hauteur pendant que l'autre entre de +hauteur). La forme change
  de taille avec le mot. Très énergique ; la phrase entière ne se lit qu'à
  la fin.
- **Cartouche comédien** ([kinetic-cartouche.json](../exemples/kinetic-cartouche.json))
  — le cartouche entre par un côté avec la première ligne ; chaque ligne
  suivante y glisse depuis l'autre côté pendant que sa `height` grandit ; il
  sort par l'autre côté. `flexShrink: 0` sur les textes, sinon les lignes se
  chevauchent pendant la croissance. La plus lisible.
- **Bandeau** ([kinetic-bandeau.json](../exemples/kinetic-bandeau.json)) —
  des bandes pleine largeur entrent de côtés opposés ; le texte répété y
  défile, freine et se cale sur un temps ; elles se croisent en sortant.
  Graphique, moins lisible (le texte répété distrait).
- **Trappe** ([kinetic-trappe.json](../exemples/kinetic-trappe.json)) —
  chaque ligne monte du bas du cadre dans son cartouche sur son temps, par
  un `spring` qui attend hors cadre (`from` tenu avant `delay`), et se cale
  en escalier ; `damping` 21 pour que la surcourse ne recouvre pas la ligne
  du dessus. Elles retombent par le bas.

## 22. Déclinaisons formats × versions

Une composition porte ses `formats` (tailles, et les props qui adaptent la
carte) et ses `variants` (les textes) ; `render_motion_video` rend chaque
version dans chaque format, `output` nommé par `{variant}` et `{format}`.
Quand la mise en page change avec
le format (coupe des lignes, placement en bas à gauche en 16:9, sous le
visage en 9:16), déclare `fmt` réglé par chaque format et un sous-arbre par
format :

```json
{ "type": "fill", "visible": { "prop": "fmt", "map": { "9x16": true, "16x9": false } }, "children": [ … ] }
```

Exemple : [declinaisons.json](../exemples/declinaisons.json).

## 23. Habillage pour Resolve

- Les **plaques** : le montage de Resolve rendu **sans** ses titres, pour
  juger l'habillage dessus. Par l'API de Resolve 21.1, `SetTrackEnable`
  a répondu `True` sans que `GetIsTrackEnabled` change, et
  `SetClipEnabled` n'agit que sur la timeline **courante** : rends chaque
  timeline courante (`SetCurrentTimeline`), désactive les clips des pistes
  de titres (`SetClipEnabled(False)`), vérifie avec `GetClipEnabled`, rends,
  puis réactive-les. Voir aussi le skill `monter-une-itw`,
  [resolve.md](../../monter-une-itw/resolve.md).

## 24. Kinetic warp : le maillage d'After

Le texte se déforme par zones, comme une feuille qu'on étire : une moitié
du mot gonfle pendant que l'autre s'écrase en fine bande, puis l'inverse.
C'est le Mesh Warp d'After, que le moteur a sous le nom `mesh()` dans
`filter` (depuis septembre 2026, voir « Maillage » dans le format).

```json
"filter": "mesh(2 4, 0 0, 25% 0, 50% 0, 75% 0, 100% 0, 0 50%, 25% 50%, 50% 50%, 75% 50%, 100% 50%, 0 100%, 25% 100%, 50% 100%, 75% 100%, 100% 100%)"
```

- La grille couvre la boîte du nœud : `mesh(<lignes> <colonnes>, …)`, puis
  (lignes + 1) × (colonnes + 1) sommets **ligne par ligne**, en px ou en %.
  Au repos, les sommets tombent sur une grille régulière.
- **Déplace des lignes entières** (tous les sommets d'une colonne, ou d'une
  rangée) : les lignes restent droites et les cellules de part et d'autre
  s'étirent et s'écrasent en douceur. Déplacer un sommet seul donne un
  gonflement local.
- Des colonnes **fixes** de part et d'autre du mot (ici 25 % et 75 % sur une
  boîte plein cadre) bornent l'étirement au mot.
- Anime le `filter` entre maillages de même grille : chaque sommet va en
  ligne droite. Une clé par geste, et le retour au repos en fin de boucle.
- La courbe qui fait le style : l'aisance d'After puis la poignée d'arrivée
  tirée à fond, soit `cubic-bezier(0.325, 0, 0, 1)` (mesurée sur le rendu
  d'un tutoriel : départ vif, longue arrivée).
- Une police très grasse et serrée (Passion One Bold dans le tutoriel), les
  lettres qui se touchent : l'étirement se lit dans les contreformes.
- **Un bloc de plusieurs lignes** (une question justifiée, une ligne par
  croche) : un maillage `1 4` par ligne, sur une boîte à la hauteur de la
  ligne plus une marge pour les accents (`padding`, annulé par des marges
  négatives). La ligne gonfle par `scaleY` et les autres rendent la place
  par `translateY`, calculés à chaque clé pour que la somme reste
  constante : les deux sont linéaires dans les échelles, donc une même
  courbe garde les lignes jointives à chaque instant. Réserve au-dessus de
  la ligne gonflée la place de ses accents étirés (environ
  (échelle − 1) × 0,2 em), sinon ils mordent sur la ligne du dessus.

Exemple : [kinetic-warp.json](../exemples/kinetic-warp.json). Le même
mouvement, rendu avec la police et le crénage du tutoriel, recouvre le rendu
After à 94 % au moins sur chaque image.
