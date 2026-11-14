# Plans de coupe

## Ils servent le sens, pas le compteur

Un plan de coupe montre d'abord ce dont la personne parle, au moment où elle
en parle, puis couvre les faux raccords du resserrement. Posé pour un quota
et hors du propos, il est pire que pas de plan de coupe : il détourne
l'attention au lieu de la porter.

Un objet montré à l'instant où il est nommé est son propre antécédent : le
plan de la maquette sur « quand on fait pivoter cette maquette » règle la
référence, s'il tombe sur le mot (`list_timeline_words` donne l'image du mot
sur la séquence).

## Aucun saut à nu

Sur un bench du 31 octobre 2026, le montage livré laissait 19 jump cuts à nu
en 57 s, et l'auteur l'a relevé. Chaque coupe dans un même rush se traite :

1. **un plan de coupe** qui montre le propos à cet endroit la couvre, ou
   commence ou finit sur elle ; on ne pose pas une illustration hors sujet
   pour cacher un saut ;
2. sinon, **le plan qui suit change de cadre** : un recadrage fixe du plan
   d'interview (`set_clip_framing`, `scale` entre 4/3 et 7/4, position
   `f × (1 − scale)` pour un visage à `f` du centre), en alternant large et
   serré, vérifié à `read_frame`. C'est ce que l'auteur fait dans Resolve, dans
   7 films sur 11 repris.

Un saut gardé est un choix, dit dans le compte rendu avec sa raison ;
`check_timeline` avec `jump_cuts: "forbidden"` les compte.

## Règles de pose vérifiées

- **Laisse-les courir** : cinq plans de 5 à 8 s valent mieux que douze de
  2 s, sans laisser à nu les sauts qu'ils ne couvrent pas.
- **Garde le visage quand la personne dit ce qui la touche.**
- **Pose tous les plans de coupe en un `prepare_edit_plan`** (un pas
  `insert_clip` par plan, positions croissantes, sur une piste vide) puis
  `apply_edit_plan` ; pour en changer un sur une piste remplie,
  `paste_clips` avec `overwrite: true`.
- **Sources à 50 i/s** : `source_in` au débit de la source, `duration` au débit
  de la séquence.
- **Un rush d'illustration ne sert qu'une fois par film** : deux plages du
  même rush, ou deux rushes de la même scène au même cadre, se voient comme
  le même plan (un film a été renvoyé pour un rush posé cinq fois). Entre
  stabilité et variété, la variété gagne : ne resserre pas les défauts de
  `list_stable_windows`, relâche-les (`max_speed` 110, voire 160 pour un
  détail de maquette), et dis lesquels bougent plus.
- **La mesure dit que la caméra bouge, pas que c'est voulu** : un plan qui
  suit son sujet et montre le travail fini se prend même sans fenêtre stable
  (le shooting d'un maquillage, retenu par l'auteur) ; une fin de prise qui
  décroche ne se prend pas, même lente. C'est l'image qui tranche.

## Décrire une fois, chercher ensuite

Décris une fois les plages utiles des rushes d'illustration
(`describe_shots`), puis cherche (`search_shots`) ; vérifie toujours à
l'image avant de poser.
