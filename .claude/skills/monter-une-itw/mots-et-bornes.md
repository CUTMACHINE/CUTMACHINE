# Mots et bornes

whisper donne le texte, MFA les frontières (`align_words`) ; les bornes
qu'on coupe sont `entry_frame`/`exit_frame` de `list_source_words`, lues sur
l'enveloppe. Ce fichier garde ce que les outils ne disent pas.

## Aligner

- **Transcris un tournage étranger avec `language` forcée** : la détection
  se trompe sur un rush court (une remarque de l'équipe en français
  suffit). L'anglais et l'espagnol n'ont pas été mesurés avec MFA.
- **Un mot manqué ne se trouve pas tout seul** : ni l'aligneur ni le
  contrôle du son hors des mots (`voice_without_words`) ne le voient ;
  `uncovered_speech`, la relance avec `texts` et l'écoute restent
  nécessaires.
- **Pour un vrai trou, retranscris l'extrait seul**, du dernier mot couvert
  au premier après (`--srt-from-media --verbatim`), jamais le rush par
  morceaux (large-v3 a encore sauté les 15 premières secondes d'un morceau
  de 20 s), puis relance `align_words` avec `texts`.

## Poser les bornes

- **Ne fusionne jamais d'office deux plages de mots contiguës** : whisper
  oublie des « heu » que l'aligneur laisse entre elles. Coupe-les en deux
  segments distincts ; un « heu » resté seul dans son plan se traite dans
  [resserrement-et-finition.md](resserrement-et-finition.md).

## La tête de rush : question de l'intervieweur et premier mot

- **Relis les premiers mots de chaque rush avant de les monter** : whisper
  y transcrit la question de l'intervieweur ou y épingle le premier mot à
  l'image 0, et MFA aligne ce texte faux tel quel. Sur une série de huit
  montages, six ouvraient sur la question.
- **Le niveau donne un candidat, pas une identité** (`dominant_onset` de
  `list_speech_onsets`) : une réponse douce peut être plus faible qu'une
  question. Confirme par le contexte, et par l'écoute si possible, avant de
  supprimer de la parole.
- **Quand question et réponse ont le même niveau, c'est le creux qui les
  sépare.** `analyze_speech_onset` les fusionne si la personne enchaîne
  vite (un bloc est entré 65 images trop tard) : baisse `group_gap_ms`, ou
  lis les niveaux de la plage par `list_source_words` (`levels: true` avec
  `from_frame` et `to_frame`, dont `window_levels_dbfs` à 20 ms).
- **Compare le début du premier bloc de chaque rush au début mesuré**
  (`list_speech_onsets`). Un morceau entièrement dans la question, fréquent
  après le resserrement, se retire par `remove_linked_clips`.
