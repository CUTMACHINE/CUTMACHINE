---
name: monter-un-reel
description: "Monter et livrer un film court pour les réseaux ou pour un écran (reel, pub verticale, promo, boucle de salon) dans un projet CUTMACHINE : cadrage 9:16, vitesse, coupes sur une musique, texte animé, déclinaisons en formats, export vérifié ; aussi en faire une version plus rythmée, la décliner ou en refaire l'export. Face caméra : lire aussi monter-une-itw ; texte animé : motion-design."
---

# Monter un reel ou une pub avec CUTMACHINE

Aucun chiffre de cette procédure n'est une cible : un compte se lit après,
il ne se vise pas. Les faits du moteur sont dans [moteur.md](moteur.md), la
façon de relever un montage dans mesures.md.

## La règle qui prime sur toutes les autres

**Un traitement posé sur chaque plan n'est plus du montage : c'est un
gabarit.** Un zoom avant puis arrière à chaque plan, un bruitage à chaque
carton : c'est à cela qu'il se reconnaît.

- La plupart des plans restent nus ; un zoom, un ralenti, un bruitage ou
  une transition a une raison et un endroit.
- Le rythme des coupes varie, des tenues aux plans courts ; rafales et
  bégaiements dépendent de la direction artistique du film.
- **Un bruitage marque un événement qui a un sens**, pas chaque carton.
- Une version plus spectaculaire passe par des transitions, du texte, le
  surligneur, des tailles de mots variées, pas par un traitement de plus
  sur chaque plan.
- Compte les plans traités avant de rendre
  ([outils/releve.py](outils/releve.py)) et écris le compte ; aucun
  chiffre ne juge le film.

## Quel film est-ce ?

- Face caméra : [pub-parlee.md](pub-parlee.md), puis le skill
  `monter-une-itw`.
- Des images sur une musique : ce fichier.
- Une boucle d'écran, sans son : ce fichier, sans l'étape 3.
- Tout en texte et en formes : le skill `motion-design`, puis les étapes
  11 à 13 d'ici.

## Ce qui se demande avant de commencer

Six questions changent le film ; ne pose que celles que le brief, la série
et les rushes ne tranchent pas ([brief.md](../monter-une-itw/brief.md)).

1. **Sera-t-il entendu ?** Une boucle d'écran ne l'est pas, et ne se monte
   pas sur la musique : la réponse décide de l'étape 3.
2. **Quels formats ?** Un 9:16 se décline souvent ensuite en 1:1 et en
   16:9.
3. **Quelle marque ?** Charte, logos, polices (`motion-design`) ; les
   transitions se construisent avec son système graphique.
4. **Quelle durée ?** Une boucle demandée « plus courte » peut perdre un
   tiers de sa durée.
5. **Le texte à l'écran est-il écrit, et lu par l'éditeur ?** Fais-le lire
   avant toute construction.
6. **Où le film se finit-il ?** Dans le moteur, ou dans Resolve
   ([resolve.md](../monter-une-itw/resolve.md)) : des sous-titres, une
   musique ou un logo de fin peuvent s'y ajouter sans que la timeline les
   porte.

Avec la personne qui signe : [brief.md](../monter-une-itw/brief.md).

## L'ordre de travail

Les outils sont ceux du serveur MCP du moteur
([pilotage.md](../monter-une-itw/pilotage.md)). Sur un reel, le `motif`
d'une coupe est le plus souvent `rhythm`, `brief`, `format` ou `marker`.

1. **Créer le projet au format livré** : `create_project` avec `format`
   (`1080x1920@25`), puis `ingest_media`.
2. **Dérusher à l'image** : `media_sheet`, `list_stable_windows` pour ce
   qui tient, `analyze_isolated_frames` sur un tournage à flashs. Décris
   une fois les plages regardées (`describe_shots`), puis cherche
   (`search_shots`) au lieu de refaire des planches.
3. **Mesurer la musique**, si le film est entendu : `analyze_audio_beats`,
   puis `list_tempo_grid` sur une fenêtre de 20 s (sur un titre entier, la
   grille peut être refusée) ; `list_clip_tempo_beats` une fois le morceau
   posé.
4. **Assembler** d'un bloc (`prepare_edit_plan`, un `insert_clip` par
   plan, puis `apply_edit_plan`) ; déplace ensuite une coupe par
   `roll_edit`, qui ne décale pas l'aval calé sur les temps.
5. **Cadrer** : `set_clip_framing`. Suis le sujet (`track_subject_framing`)
   seulement s'il sort de la fenêtre 9:16 ; reste fixe sur deux personnes
   que le 9:16 ne tient pas ou sur un plan de moins d'une seconde, et relis
   `inspect_cut` : ses réglages par défaut ne sont qu'un départ
   ([moteur.md](moteur.md), « Cadrer »).
6. **Régler la vitesse** : `set_clip_speed` ([moteur.md](moteur.md),
   « Vitesse ») ; une rampe se regarde à l'image avant d'être gardée.
7. **Animer un plan, rarement** (`set_clip_animation`), et relis
   `list_clip_framing` à sa dernière clé : un zoom arrière sous 100 %
   découvre le cadre sans refus.
8. **Raccorder** par la coupe franche, sans fondu enchaîné ; noir
   d'ouverture et de fin par `dip_to_color`, transition de marque en
   composition.
9. **Texte, logo, habillage** : skill `motion-design`, posés par
   `place_motion_clip`.
10. **Étalonner** comme une interview
    ([couleur-et-export.md](../monter-une-itw/couleur-et-export.md)).
11. **Régler le son** : `set_clip_audio`, puis `analyze_timeline_audio`
    pour le niveau intégré et la crête. Donne les chiffres mesurés : la
    procédure ne fixe pas de cible.
12. **Regarder** : `inspect_cut` sur chaque coupe qui porte un traitement
    ou un texte, puis `review_cuts`. Sous une musique continue, attends-toi
    à `sound_cut_live` ; un jump cut voulu pour le rythme reste.
13. **Livrer** : `export_timeline` avec `verify: true`. Pour un autre
    format : `clone_timeline`, `update_sequence`, puis les étapes 5, 9 et
    12 refaites dans le clone ; `export_timelines` rend le lot.

**Générer coûte de l'argent** : seulement sur demande, par
`propose_generation` puis le oui de la personne, et rien sur une personne
filmée sans son accord, que seule la personne qui signe peut affirmer ;
`generate_media` dit le reste.

## Les règles qui coûtent un film

- **Dans un film, un rush ne se montre qu'une fois**, ni deux prises
  voisines de la même action.
- **Être sur la grille ne juge rien** : un gabarit a toutes ses coupes sur
  un temps, et un montage qui tient peut en avoir moins d'un tiers.
- **L'énergie d'un texte est dans son entrée et sa sortie**, plus que dans
  son milieu ; son temps de lecture se calcule (skill `motion-design`).
- **Une pub parle à un inconnu** : une accroche, puis la promesse dans ses
  mots, pas les noms de fonctions.
- **Un cadrage se juge sur l'image composée** (`read_frame` avec
  `clip_id`) : `list_clip_framing` dit la géométrie, pas le pied de lumière
  dans le champ.

## Contrôles avant de rendre

- **Doublons** : `outils/releve.py` liste les rushes repris ; une
  composition qui montre des images compte aussi.
- **Segments courts** : tout segment visible de moins de 12 images se
  regarde : un visage de 2 images passe sinon dans le fichier livré.
- **Cadrage** : `list_clip_framing` sur chaque plan cadré, à l'image de
  chaque clé ; `uncovered` à zéro partout.
- **Zone des réseaux** : hors du 9:16, aucune zone n'est définie : dis-le.
  Des sous-titres centrés à 74 % de la hauteur (le style maison) passent
  sous la limite basse de la zone des Reels, et rien ne dit qu'elle vaut
  pour eux : dis-le aussi.
- **Musique** : `continuous` à chaque raccord du morceau, et les niveaux
  mesurés à l'étape 11.
- **Export** : lis `verification`, puis `ffprobe` le fichier livré (durée,
  taille, cadence, son).

Ces contrôles attrapent des défauts, jamais un gabarit ni l'ennui. Rends
avec le fichier ce que tu as regardé, ce que tu n'as pas pu juger (tu
n'écoutes pas) et tes comptes.
