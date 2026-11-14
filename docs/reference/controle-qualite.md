# Contrôle qualité des plans

> Relu contre le code le 3 octobre 2026. Les mesures sur des prises réelles
> (valeurs, corpus, balayage des seuils) ne sont pas vérifiées par lecture :
> seuls les seuils qui en sortent le sont.

Un plan flou ou bougé ne se juge pas, il se mesure. `--shot-quality` analyse
un média, quatre échantillons par seconde, et écrit un cache dans
`.cutmachine/shotquality/<media-id>.json` ; `--shot-quality-report` (MCP :
`list_shot_quality`) note chaque clip de la timeline active.

Deux mesures, aucune inférence :

- **Netteté** — variance du laplacien sur le plan de luminance, notée par
  rapport à la médiane du média lui-même. Le relatif est volontaire : la
  variance du laplacien n'a pas de sens absolu d'un contenu à l'autre, alors
  qu'à l'intérieur d'un même rush, même caméra et même optique, la comparaison
  tient. Le prix de ce choix est énoncé plutôt que caché — un média flou de
  bout en bout note tous ses plans « Sharp », d'où la publication des médianes
  absolues à côté de la note.
- **Bougé** — différence absolue moyenne de luminance entre deux échantillons,
  notée en absolu : la grandeur veut dire la même chose sur n'importe quel
  rush.

Le grade porte sur le **décile le plus mauvais**, pas sur la médiane : un plan
qui perd le point à mi-course reste sain en médiane et reste inutilisable.

Mesuré sur une prise réelle (interview à la main, Sony FX30, 6,72 s, quatre
échantillons par seconde, 1,7 s d'analyse) : le corps du plan tient entre
4 055 et 4 481 de netteté, et les cinq derniers échantillons s'effondrent à
3 499, 3 691, 2 616, 1 461 puis 336 pendant que le bougé passe de 5 000–36 000
à 67 624. Vérifié à l'image : la caméra quitte le sujet en fin de prise, le
cadre part sur un mur et la dernière image n'est plus qu'une bouillie. C'est
exactement ce que la mesure doit attraper. Un clip posé sur cette fin est noté
`Blurry` / `Shaky`, celui posé sur le corps `Sharp` / `Steady`.

Les seuils de bougé sont calés sur cette prise unique. C'est un point
d'ancrage, pas un étalonnage : chaque note est publiée avec le nombre qui l'a
produite, précisément pour qu'ils se déplacent sur preuve.

Un média jamais analysé apparaît sous `unanalyzed`, jamais parmi les plans
propres. L'absence de mesure n'est pas un feu vert : `analyze_shot_quality`
(MCP) lance l'analyse depuis l'agent, sans passer par la ligne de commande.

Un cache écrit dans un format que ce binaire ne lit plus (version 1 ou 2, ou
une version plus récente) n'est pas un média jamais analysé : ses plans
restent sous `unanalyzed` avec une raison qui le dit, et `unreadable_caches`
nomme chaque source, la version trouvée, les versions lisibles et l'appel qui
reconstruit le cache (`analyze_shot_quality`, `--shot-quality` au CLI) ; les
autres sources sont notées comme d'habitude. Rien n'est réanalysé sans qu'on
le demande. `list_stable_windows`, qui ne porte que sur une source, refuse
(`"error":"UnreadableCache"`, même `result.unreadable_caches`).

La note tient compte du recouvrement. Un plan mou entièrement caché par une
illustration posée au-dessus n'est pas un défaut à corriger, puisque personne
ne le voit — chaque entrée porte donc sa note (`clean`, ce qui est mesuré), sa
durée encore visible (`visible`, ce que dit le document) et `needs_attention`,
qui est la conjonction des deux. Sans ça, la vue réclamait de recouper des
plans invisibles.

## Découper un rush en plans

Noter ce qui est déjà monté est une chose ; savoir ce que les rushes
contiennent en est une autre. Un fichier sorti de la caméra en une seule prise
peut tenir six prises, et tant qu'elles ne sont pas nommées il n'y a rien à
sélectionner, à noter ou à décrire d'autre que le fichier entier.

`--shot-quality-report` publie donc, à côté de la liste des clips, un tableau
`sources` : pour chaque média analysé, les plans détectés à l'intérieur, avec
leur début, leur fin et l'échantillon le plus net — l'image à extraire quand
une seule doit représenter le plan. Les temps sont dans le domaine temporel de
la source, donc utilisables tels quels comme `source_in`.

Reste une mesure, sans modèle : une coupe est une discontinuité de l'image.
Une troisième grandeur par échantillon s'ajoute aux deux précédentes, la
**distance d'histogramme de luma** — la fraction des pixels ayant changé de
casier de luminance. Le cache passe en version 3 ; les anciens sont refusés,
à réanalyser, plutôt que complétés par une valeur jamais mesurée.

L'idée de départ était qu'un panoramique déplace l'image sans changer sa
distribution, contrairement à une coupe. **Mesurée, cette idée est fausse.**
Sur la prise réelle ci-dessus, l'instant où la caméra quitte le sujet atteint
70 433 de bougé et 292 944 de distance d'histogramme ; une vraie coupe montée
dans la même matière mesure 48 489 et 30 900. Le mouvement de caméra note
*plus haut que la coupe sur les deux grandeurs*. Aucun couple de seuils
absolus ne les sépare.

Ce qui les sépare n'est pas l'amplitude du changement mais sa forme. Une
caméra a de l'inertie : un mouvement s'étale sur plusieurs échantillons. Une
coupe est instantanée : elle tombe sur un seul. Rapportés à la médiane de leur
voisinage, les mêmes événements se rangent — les mouvements de caméra montent
à 153 %, 194 %, 201 %, 202 %, 210 %, 232 % et 299 %, les deux coupes réelles à
408 % et 3 956 %. Le seuil est à 350 %, à égale distance des deux, et 162 des
combinaisons balayées autour de ce point donnent le même résultat sur les
quatre fixtures : la règle est sur un plateau, pas sur une arête.

Les limites sont connues et énoncées : un fondu enchaîné n'est pas trouvé,
puisqu'il n'a aucun échantillon isolé sur lequel piquer ; une coupe entre deux
plans réellement semblables peut passer sous les planchers absolus ; et
l'étalonnage repose sur quatre fixtures dont une seule est de la vraie matière.
Élargir ce corpus est ce qui ferait cesser d'être provisoires les nombres
ci-dessus.

## Mouvement de caméra et fenêtres stables

Le bougé ne répond pas à la question « la caméra bouge-t-elle » : il note un
intervenant qui gesticule sur pied comme un plan à l'épaule, et ne voit pas un
zoom. L'analyse mesure donc aussi le **mouvement global** entre deux
échantillons, par corrélation de phase sur une grille de neuf blocs :
translation horizontale et verticale, et zoom déduit de l'écart entre les
déplacements des blocs. Un échantillon où moins de quatre blocs sont fiables
(image uniforme ou qui change trop vite) est déclaré non mesuré, jamais
immobile. Le cache passe en version 4 ; un cache en version 3 reste lisible,
sans mouvement de caméra.

`list_shot_quality` ajoute à chaque plan un bloc `camera` : vitesse médiane,
vitesse du 90e centile et pic de zoom. `list_stable_windows` (MCP) liste les
portions d'un rush où la caméra tient : vitesse sous `max_speed`, zoom sous
`max_zoom_per_mille` par seconde et netteté au-dessus de
`min_sharpness_percent` de la médiane du rush, pendant au moins
`min_duration_ms`. Les vitesses sont en pixels par seconde rapportés à une
image de 1 080 pixels de large, quelle que soit la définition du rush. Chaque
fenêtre est rendue en images du rush avec un `source_in` et une durée exacts,
prêts pour un plan de coupe.

La réponse porte aussi un bloc `rush`, qu'il y ait une fenêtre ou non :
vitesse médiane, part des intervalles mesurés au-delà de chaque limite
(`too_fast_percent`, `too_much_zoom_percent`, `too_soft_percent`), plus longue
plage dans toutes les limites (`longest_still`, sur la grille de l'analyse,
même plus courte que `min_duration_ms`), intervalles mesurés et non mesurés.
Avant le 6 novembre 2026, un rush sans fenêtre ne rendait qu'une liste vide,
et un agent l'a lue comme « inutilisable » : il a laissé hors du film les
meilleurs plans d'un shooting, des suivis à la main à 110–260 px/s en médiane.
La mesure dit que la caméra bouge ; elle ne dit pas si c'est voulu. Sur ce
shooting, un suivi qui garde son sujet et une fin de prise qui le perd se
mesurent pareil, et le suivi y paraît même le moins régulier des deux, à
quatre échantillons par seconde comme à la cadence des images : aucune
mesure n'en décide, c'est l'image qui tranche.

Les défauts (80 px/s, 150 ‰/s, 72 %, 1,2 s) sont calés sur les 23 rushes à
l'épaule d'une interview réelle, où la caméra bougeait à 73 px/s en médiane :
ils ouvrent une fenêtre dans 11 rushes. Ils suivent le montage que l'auteur a
jugé le meilleur sur ces rushes (ITW6b, 6 octobre) ; les premiers défauts,
60 px/s et 1,6 s, n'en ouvraient que 6 et retiraient des dérives naturelles.
L'estimation du zoom confond la parallaxe et un sujet qui bouge avec un zoom :
sa médiane y était de 54 ‰/s, et une limite sous 100 environ écarte tout plan
à l'épaule. 150 arrête encore un zoom volontaire.

## Images isolées

Quatre échantillons par seconde ne voient pas une image seule. Sur un film
tourné pendant un shooting photo, un flash de studio a levé la luminance
moyenne d'une image de 104 à 182 avant de redescendre : à 50 i/s, l'analyse
ci-dessus le manque onze fois sur douze, et seul le rendu de contrôle l'a
trouvé.

`--isolated-frames` (MCP : `list_isolated_frames`, `analyze_isolated_frames`)
passe donc **image par image**, en lecture seule, mais seulement sur les
**plages source que la timeline lit**, à la vitesse de chaque plan et
poignées de transition comprises : un plan de deux secondes à 2/1 est mesuré
des images source 0 à 98, plus la marge (PRD38). Pour un film de 74 s tiré de 104 rushes, c'est une poignée de secondes par rush au
lieu de leur totalité. Deux grandeurs par image, en entiers : la **luminance
moyenne** en millièmes de niveau (104,0 s'écrit 104000) et l'**histogramme de
luma** en pour-mille de pixels par casier. Le cache
(`.cutmachine/isolatedframes/<media-id>.json`) ne contient aucun flottant et
deux passes écrivent les mêmes octets.

Est signalé un écart d'**une ou deux images** qui **revient au niveau de ses
voisines**. La règle sépare les trois faux positifs par la forme, pas par
l'amplitude : une coupe déplace le niveau et l'y laisse, une montée
d'éclairage l'écarte d'un voisinage entier, un panoramique ne se pose jamais.
Les deux déclencheurs — écart de niveau d'au moins 18 niveaux, ou 15 % des
pixels changeant de casier — sont aussi rapportés au mouvement propre du
voisinage, comme le sont les frontières de plans plus haut.

Pour le **dérush**, avant que rien ne soit monté, `--media <id>` (répété ;
MCP : `media_ids`) mesure ces rushes **en entier**, qu'une timeline les lise
ou non. C'est là qu'on repère les flashs d'un shooting pour les garder ou les
écarter ; il n'y a plus besoin de monter chaque rush dans une timeline de
dérush pour avoir des plages à mesurer.

L'outil **situe, il ne juge pas**. Les deux versions plus rythmées du même
film font de ces flashs une figure : huit plans commencent sur l'image de
flash, calée sur un temps. Rien n'est donc nommé « erreur » ; chaque image
est rendue avec son écart, le niveau retrouvé et les clips qui la jouent.
Sur un rush dont un plan n'est pas à 1/1, ces clips sont trouvés par la règle
de vitesse du moniteur et de l'export : une image qu'un plan accéléré saute
n'est jouée par aucun clip, et un plan à 1/1 d'un rush à cadence double n'en
joue qu'une sur deux. Un cache écrit avant PRD38 apparaît sous `unscanned`.

La même mesure tourne sur le fichier livré, dans `verify_export`, sur les
images que sa comparaison compte `unverifiable` — c'est précisément là qu'un
flash se cache, à demi-opacité dans une poignée de fondu, où il reste trouvé.

Ces seuils sont calés sur le seul cas mesuré (le flash ci-dessus, entier puis
divisé par deux) et sur des séries synthétiques. Contrairement à ceux du
contrôle qualité, aucun corpus de rushes ne les a balayés : c'est ce qui
cesserait de les rendre provisoires.
