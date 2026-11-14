# Ce que le moteur fait, refuse et ne dit pas

Faits du moteur joués le 3 octobre 2026 sur des mires, pas sur un film ; la
liste de ce qui a tourné est en bas.

## Cadrer

- En `fill`, deux tiers d'un 16:9 partent : choisis sur l'image cadrée
  (`read_frame` avec `clip_id`) ; sur une série d'interviews, un tiers des
  plans retenus sur l'image entière ne tenaient pas
  ([resolve.md](../monter-une-itw/resolve.md)).
- Un zoom sur un plan suivi se pose après `track_subject_framing`, par
  `set_clip_animation` en reprenant ses clés de position, jamais sous
  l'échelle suivie.
- Un écran partagé ou une image dans un rond se fait par `set_clip_mask`
  sur le plan du dessus : la forme est en fractions de l'image du plan, pas
  du cadre, et suit son cadrage. `read_frame` montre le plan seul, ce que
  le masque retire en noir ; juge le résultat sur `cut_sheet` ou sur
  l'export. Le masque n'a été vu sur aucun film.
- Un plan tenu à la main qui tremble se stabilise par `stabilize_clip`,
  avant `track_subject_framing` (il remplace les clés de position). Lis sa
  réponse : `scale` est le zoom que la stabilisation coûte,
  `shake.left_rms_px` ce qui reste, `frames_at_edge` au-dessus de zéro dit
  que le plan tremble plus que `max_scale` ne permet. Il ne corrige que la
  translation, pas le roulis, et n'a été vu sur aucun film : relis le plan
  (`inspect_cut`, `contact_sheet`) et annule si le sujet a été pris pour la
  caméra.
- `track_subject_framing` rend `frames_at_edge` (au-dessus de zéro, le
  sujet sort de ce que le rush permet : agrandis par `scale`, ou garde le
  plan fixe) et `moves`, le nombre de mouvements du cadre. Ses valeurs par
  défaut (zone morte 1/10 du cadre, 2/5 du cadre par seconde) n'ont été
  vues sur aucun film, et il n'a jamais tourné avec Vision (lot
  CADRAGE-2026-10).

## Vitesse

- Ralentis un rush à 50 i/s, pas à 25 (rien n'est interpolé) : le monteur
  du film de lookbook a écarté ses rushes à 25 i/s.
- `list_clip_framing` avec `offset` rend `source_frame`, l'image du rush
  montrée à cet instant : 74 à la dernière image d'un plan de 50 images
  ouvert sur l'image 25 d'un rush à 50 i/s et ralenti à `1/2`.

## Décliner en formats

- Après `update_sequence`, relis `list_clip_framing` sur chaque plan du
  clone : un plan décalé en 9:16 découvre le cadre en 1:1.
- Un clip de motion garde la taille de sa composition : pour chaque format,
  ajoute la pièce à la taille de la timeline et repose-la.
- Le 16:9 aux côtés floutés des pubs livrées s'est fait ailleurs. Le flou
  d'un plan existe depuis le 30 octobre (`set_clip_blur`) ; ce montage-là
  n'a pas encore été fait dans le moteur.

## Regarder et vérifier

- `timeline_stats` compte les plans visibles, les coupes et les clips de
  motion. Il ne rend ni les durées, ni les rushes repris, ni les
  traitements : c'est `outils/releve.py`, qui lit la réponse de
  `describe`, ou `--describe`, qui répond pendant que le serveur tient le
  projet.
- Sur le titre du film de salon (81 s), la grille de tempo est refusée sur
  le morceau entier (cohérence 0,11) et acceptée à 105 BPM sur chaque
  fenêtre de 20 s.

## Ce qui a tourné, ce qui n'a pas tourné

Joués sur le projet d'essai, par `--call-tool` ou par la commande
équivalente (`--create-project`, `--ingest`, `--prepare-edit-plan`) :
`create_project`, `ingest_media`, `conform_sequence` (aperçu),
`add_timeline`, `insert_clip`, un plan préparé puis appliqué, `roll_edit`,
`trim_clip`, `set_clip_framing`, `list_clip_framing`, `set_clip_speed`,
`set_clip_animation`, `add_transition`, `add_track`,
`list_motion_templates`, `add_motion_template`, `place_motion_clip` (`at`),
`set_clip_motion_props`, `check_motion_safe_area`, `render_motion_frame`,
`set_sequence_lut` (une LUT identité, sans gestion couleur), `clone_timeline`,
`update_sequence`, `timeline_stats`, `contact_sheet`, `cut_sheet`,
`media_sheet`, `read_frame`, `inspect_cut`, `list_isolated_frames`,
`export_timeline`, `export_timelines` (aperçu).

Relus sur le cache d'un vrai titre : `list_tempo_grid`,
`list_clip_tempo_beats`, `list_audio_beats`.

Cités d'après le catalogue seulement : `analyze_audio_beats`,
`list_music_structure`, `analyze_shot_quality`, `list_stable_windows`,
`analyze_isolated_frames`, `conform_color_management`,
`propose_exposure`, `propose_grade`, `set_clip_audio`,
`analyze_timeline_audio`, `place_motion_clip` sur un temps (`beat`),
`export_resolve_xml`.
