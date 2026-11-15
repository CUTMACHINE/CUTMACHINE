# Ce que le moteur fait, refuse et ne dit pas

## Cadrer

- En `fill`, deux tiers d'un 16:9 partent : choisis sur l'image cadrée
  (`read_frame` avec `clip_id`), pas sur l'image entière
  ([resolve.md](../monter-une-itw/resolve.md)).
- Un zoom sur un plan suivi se pose après `track_subject_framing`, par
  `set_clip_animation` en reprenant ses clés de position, jamais sous
  l'échelle suivie.
- Un écran partagé ou une image dans un rond se fait par `set_clip_mask`
  sur le plan du dessus : la forme est en fractions de l'image du plan, pas
  du cadre, et suit son cadrage. `read_frame` montre le plan seul, ce que
  le masque retire en noir ; juge le résultat sur `cut_sheet` ou sur
  l'export.
- Un titre derrière quelqu'un, un fond remplacé ou flouté se fait par la
  roto : `analyze_person_matte` sur le rush (une fois ; `async` sur un rush
  long), puis le même plan sur une piste au-dessus avec `set_clip_mask
  {shape: "person"}`, le titre ou le fond entre les deux. Vision garde
  toutes les personnes de l'image, pas une seule : regarde `read_frame`
  avant de compter sur le détourage d'une seule personne.
- Un plan tenu à la main qui tremble se stabilise par `stabilize_clip`,
  avant `track_subject_framing` (il remplace les clés de position). Lis sa
  réponse : `scale` est le zoom que la stabilisation coûte,
  `shake.left_rms_px` ce qui reste, `frames_at_edge` au-dessus de zéro dit
  que le plan tremble plus que `max_scale` ne permet. Il ne corrige que la
  translation, pas le roulis : relis le plan (`inspect_cut`,
  `contact_sheet`) et annule si le sujet a été pris pour la caméra.
- `track_subject_framing` rend `frames_at_edge` (au-dessus de zéro, le
  sujet sort de ce que le rush permet : agrandis par `scale`, ou garde le
  plan fixe) et `moves`, le nombre de mouvements du cadre. Ses valeurs par
  défaut (zone morte 1/10 du cadre, 2/5 du cadre par seconde) ne sont qu'un
  départ : relis le plan avant de le garder.

## Vitesse

- Ralentis un rush à 50 i/s, pas à 25 : rien n'est interpolé.
- `list_clip_framing` avec `offset` rend `source_frame`, l'image du rush
  montrée à cet instant : 74 à la dernière image d'un plan de 50 images
  ouvert sur l'image 25 d'un rush à 50 i/s et ralenti à `1/2`.

## Décliner en formats

- Après `update_sequence`, relis `list_clip_framing` sur chaque plan du
  clone : un plan décalé en 9:16 découvre le cadre en 1:1.
- Un clip de motion garde la taille de sa composition : pour chaque format,
  ajoute la pièce à la taille de la timeline et repose-la.
- Un 16:9 aux côtés floutés passe par le flou d'un plan (`set_clip_blur`) :
  juge-le sur l'image composée avant de le livrer.

## Regarder et vérifier

- `timeline_stats` compte les plans visibles, les coupes et les clips de
  motion. Il ne rend ni les durées, ni les rushes repris, ni les
  traitements : c'est `outils/releve.py`, qui lit la réponse de
  `describe`, ou `--describe`, qui répond pendant que le serveur tient le
  projet.
- La grille de tempo peut être refusée sur un morceau entier (cohérence
  trop basse) et acceptée sur chacune de ses fenêtres de 20 s : mesure par
  fenêtres.
