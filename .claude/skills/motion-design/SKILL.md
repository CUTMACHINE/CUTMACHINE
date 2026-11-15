---
name: motion-design
description: Créer, animer ou corriger du motion design avec le moteur de CUTMACHINE — cartons, titres, kinetic typo, logo animé, habillage posé sur un montage, déclinaisons en formats (9:16, 4:5, 1:1, 16:9) et en versions, calques transparents pour Resolve. À utiliser dès qu'on demande d'animer un texte, une forme ou un logo, de faire une vidéo « en motion », de rendre un carton moins générique ou moins « IA », de proposer une direction artistique animée, ou d'exporter un habillage. À lire avant d'écrire la moindre composition.
---

# Faire du motion design avec CUTMACHINE

Une composition est une donnée ([docs/MOTION_FORMAT.md](../../../docs/MOTION_FORMAT.md)),
sans expression : ce qui se calcule se calcule avant, dans le script qui
l'écrit.

## La règle qui prime : on ne livre que ce qu'on a regardé

**Rendu → planche d'images numérotées → correction → nouveau rendu.** Jamais
de livraison sur la foi du JSON. Le moteur accepte une composition fausse
tant qu'elle est bien formée ; seule l'image dit si c'est juste.

La planche montre en une seconde ce que le JSON cache : une entrée en
retard, une sortie trop tôt, un texte en fantôme.

Regarde donc toujours, pour chaque moment, l'image **avant** la coupe, **la**
coupe, l'image **après**, et les trois dernières images de la sortie.
[`outils/planche.py`](outils/planche.py) les tire par numéro exact et les
étiquette de leur numéro dans la vidéo.

## Avant d'animer : la direction

1. La vraie charte d'abord (le PDF, les logos vectoriels, les polices
   livrées), pas le site : son principe graphique devient le vocabulaire du
   mouvement.
2. Mesure la musique avant de caler (`list_tempo_grid` sur une fenêtre) ;
   une grille refusée donne encore une période candidate à confronter aux
   coupes.
3. Propose deux à quatre directions animées sur le même moment de 3 s, sur
   la vraie image et avec le son : un mouvement ne se juge pas sur une
   planche fixe.
4. Tiens un seul langage de mouvement : une famille de courbes, une unité
   de temps, une famille de transitions.
5. Trois corps par rôle, déclarés une fois : des cartons dimensionnés un
   par un finissent avec presque autant de corps que de cartons.

À éviter sans demande : les marie-louises (une mise en page d'imprimé), la
pulsation au tempo, les volets plein écran, les apparitions par pop ou par
fondu. Ce qui tient : partir du principe graphique de la charte, chaque
entrée sur une coupe et chaque sortie sur la suivante, des aplats.

Le détail (typographie, temps de lecture, son, liste de contrôle) est dans
[references/direction.md](references/direction.md).

Avec la personne qui signe : [brief.md](../monter-une-itw/brief.md). En
motion, une version de plus est une composition de plus, pas la précédente
modifiée.

## Avant d'écrire : l'étagère

Le moteur a une bibliothèque de **pièces** : des compositions prêtes, à
props, qu'on prend, qu'on adapte et qu'on pose (`list_motion_templates`).
Regarde-la avant d'écrire un générateur, une fois la direction fixée, pas à
sa place.

- **Choisis un mécanisme, pas un look** : si son `avoid` décrit ton cas,
  compose sans pièce (`add_motion_composition`).
- **Adapte par les props** (`set_props`), puis par les nœuds ; une limite
  de la fiche se vérifie à la planche.
- **Une charte de marque** : un passage hors charte voulu se dit dans le
  compte rendu (`check_timeline` avec `policy.kit` le nomme). Une nouvelle
  charte s'écrit en `<library.directory>/kits/<marque>.json`
  (`brand-kit.v1`), chaque valeur sourcée, jamais dans le dépôt ; une
  valeur sans source est une proposition à faire valider. Ce que la marque
  veut d'une pièce en particulier (la seconde ligne de `nom`, son fichier
  de logo) va dans sa section `pieces`, une pièce qu'elle refuse dans
  `withdrawn` avec la raison, jamais dans la pièce livrée.
- **Range ce qui a tenu à l'image** (`save_motion_template`, `origin` : le
  film), dans les seuls formats vus ; demande avant d'écrire dans la
  bibliothèque, jamais une pièce de client dans le dépôt.

## Les règles, version CUTMACHINE

1. Jamais `linear` hors défilement : entrée `cubic-bezier(0.16, 1, 0.3, 1)`,
   sortie `cubic-bezier(0.7, 0, 0.84, 0)`, déplacement
   `cubic-bezier(0.65, 0, 0.35, 1)`.
2. Une entrée anime deux ou trois propriétés, jamais un fondu seul ; en
   kinetic typo, un mouvement sans fondu.
3. Rien n'entre en même temps : `units` + `stagger` sur un texte (mots :
   3 images, lettres : 1) ; décalages explicites entre blocs (4 à 8 images,
   ou la croche).
4. Les sorties existent, durent environ deux tiers de l'entrée, en
   accélération, et finissent sur la coupe suivante.
5. Plein cadre : fond, médias, texte, étalonnage, grain ; un habillage
   transparent n'a ni fond ni grain de composition ; en flat design, ni
   grain ni dégradé.
6. Mouvement franc, immobilité, mouvement suivant ; jamais de pulsation au
   tempo non demandée.
7. Un script écrit la composition : coupes et temps de la musique, couleurs,
   courbes, géométrie par format et textes par version y sont des tables ;
   aucun nombre magique dans les nœuds.

## Le temps de lecture se calcule

Seul compte le temps où le texte est lisible en entier : une à deux
secondes ne suffisent pas à une phrase. Lisible ne veut pas dire immobile :
dans un film tout en texte, il vit pendant qu'on le lit ; sur un habillage,
l'énergie reste à l'entrée et à la sortie. Le tempo ne raccourcit jamais
une lecture : si le texte déborde la mesure, le carton prend la mesure
suivante, ou le texte perd des mots. Calcul et planchers :
[direction.md](references/direction.md), « Temps de lecture ».

## Déroulé

1. **Cadrer.** Fixe durée, cadence (25 i/s), formats, versions et
   ressources : polices en fichiers statiques, logos en PNG transparents,
   plaques (le montage sans ses titres).
2. **Écrire le générateur.** Il produit le fichier ; `add_motion_composition`
   le charge (`definition_file`), `edit_motion_composition` `replace` le
   remplace à chaque itération ; pars d'une pièce quand elle porte le
   mécanisme. Les motifs et les pièges du moteur, que rien ne refuse :
   [references/motifs.md](references/motifs.md).
3. **Régler une image.** `render_motion_frame` pour une position ou une
   taille, pas pour un calage (sa réponse est lourde).
4. **Rendre et vérifier.** `render_motion_video`, puis
   `python3 outils/planche.py rendu.mp4 planche.jpg 24 25 26 …` aux coupes,
   entrées et sorties. Pour un habillage, compose d'abord sur la plaque :
   ```sh
   ffmpeg -i plaque.mp4 -i calque.mov -filter_complex \
     "[0:v][1:v]overlay=format=auto,format=yuv420p[v]" -map "[v]" -map 0:a \
     -c:v libx264 -crf 17 -c:a copy apercu.mp4
   ```
   Une composition rendue seule pour les réseaux se contrôle avec
   `zone: "social_vertical"` ; sur une timeline 9:16, c'est d'office. Un
   nœud que le cadre coupe exprès (les échos d'un bandeau) se marque
   `decor: true` : le contrôle le liste sans le juger. Jamais un texte
   qu'on doit lire.
5. **Livrer.**
   - dans Resolve : le calque alpha sur une piste au-dessus des plans, les
     anciens titres désactivés, puis l'export habituel ;
   - dans une timeline CUTMACHINE : `place_motion_clip` ;
   - pour un motion designer : `export_motion_after`, puis lis ses `notes`
     et compare des images d'After à celles du moteur.

Partir d'un travail fait dans After : `import_after_comp {comp_id}` (le
projet ouvert dans After, en copie) en fait une composition du projet, un
nœud par calque. Lis ses `notes` avant tout : une expression y devient des
valeurs figées, une précomposition, un masque ou un animateur de texte n'y
sont pas. Puis compare quelques images au rendu d'After (`aerender`).

## Fichiers

- [references/motifs.md](references/motifs.md) — les pièges du moteur, les
  dix-sept motifs du skill Remotion transposés au format de CUTMACHINE, plus
  les motifs propres à ce moteur (calage sur coupe, crans, fenêtre,
  texte-image, kinetic typo, kinetic warp, déclinaisons, habillage pour
  Resolve), chacun avec son JSON.
- [references/direction.md](references/direction.md) — couleurs,
  typographie, temps de lecture, son, liste de contrôle avant livraison.
- [exemples/](exemples/) — onze compositions neutres, chargées et rendues dans
  le moteur avant d'être rangées ici. Elles utilisent la Fira Sans Condensed
  des tests : `font_dirs: ["<dépôt>/tests/fixtures/fonts"]`. Ce sont des
  démonstrations d'écriture ; ce qui se pioche est sur l'étagère
  (`list_motion_templates`).
- [outils/planche.py](outils/planche.py) — planche d'images numérotées tirée
  d'un rendu (ffmpeg seul).
- [outils/generer_exemples.py](outils/generer_exemples.py) — le script qui
  écrit les exemples : on corrige le script, pas les JSON, puis on recharge,
  rend et planche.

## Origine

Adapté de [haidrrrry/claude-remotion-skill](https://github.com/haidrrrry/claude-remotion-skill)
(licence MIT, © 2025 haidrrrry) : les dix règles, la boucle de vérification,
les motifs et les règles de design en sont repris et réécrits pour le format
de composition de CUTMACHINE. Le code React et Remotion de l'original ne
s'applique pas ici et n'a pas été repris. Sa licence est reproduite dans
`third_party/claude-remotion-skill/LICENSE` du dépôt.
