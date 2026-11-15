# Resserrement et finition

## Sommaire

- [Resserrer](#resserrer)
- [La boucle de finition](#la-boucle-de-finition)
- [Les « heu » que personne n'a écrits](#les--heu--que-personne-na-écrits)
- [Relire le texte monté](#relire-le-texte-monté)

## Resserrer

- **C'est le creux le plus long qui compte, pas le total** : douze
  respirations de 0,2 s ne s'entendent pas, un trou de 0,6 s s'entend. Vise
  aucun creux au-delà de ~0,3 s (`analyze_timeline_audio`).
- **Ne descends pas `tighten_pauses` sous 300 ms** : à 250 ms, il referme
  le creux qu'une occlusive laisse à l'intérieur d'un mot (« notre »). Un
  mot aligné est protégé, un rush non aligné ne l'est pas
  (`words_checked: false`).
- **`cut_words` ne voit que les mots que MFA a placés** : relis le texte
  monté après chaque changement de réglage.
- **Chaque creux refermé coupe le plan en deux** : autant de sauts à
  traiter ([plans-de-coupe.md](plans-de-coupe.md), « Aucun saut à nu »).

## La boucle de finition

Boucle bornée : `analyze_timeline_audio` pour les creux du son monté,
`trim_boundary_air` sur une bordure, `close_junction_air` (`pairs`) sur un
raccord. Borne le nombre de passes et signale ce qui ne converge pas. Une
parole utile ne se supprime pas pour sa seule durée.

Pour déplacer un raccord entre deux plans contigus du même rush, c'est
`roll_edit`, pas `ripple_trim`.

## Les « heu » que personne n'a écrits

Un « heu » que personne n'a écrit survit au resserrement, seul entre deux
coupes, dans un plan de quelques images : aucun contrôle par le texte ne le
voit, `transcribe_timeline` passant par le même whisper.
`list_timeline_words` le trouve, avec les autres défauts, en un appel :

- un plan `silent` ou sans mot se supprime ; un `head_cut`/`tail_cut` se
  corrige sur `entry_frame`/`exit_frame` ; un `voice_without_words` se
  coupe par `remove_source_ranges`, après écoute ;
- une hésitation s'écoute, puis se coupe sur sa `suggested_cut`, jamais
  sur la plage du candidat. Si l'oreille entend encore une fin de mot
  coupée, l'aligneur a mal posé le mot (`at_risk`, `voiced: false`) :
  seule l'écoute tranche ;
- **un redémarrage (`restarts`) se vérifie sur les mots avant d'être
  coupé** : le détecteur marque aussi des mots longs dits une seule fois,
  où une occlusive fait le creux (« spectateurs », « traditionnel »), et sa
  `suggested_cut` les couperait en deux. Un mot court répété, ou une
  répétition que la transcription montre, se coupe ;
- **ne coupe pas la liste des candidats en bloc** : une part d'entre eux
  sont de fausses alertes, ou des hésitations à garder.

Un candidat laissé (une hésitation gardée, un redémarrage qui n'en est pas
un) se dit dans le compte rendu.

## Relire le texte monté

Relire le texte monté trouve ce que rien d'autre ne voit : un mot amputé
par une coupe (« On l'a **mis** » pour « On l'a remis »), un bloc sorti
trop tôt.

- **Texte choisi** : relis le script retenu sans les rushes (antécédents,
  causes, fidélité à ce que la personne voulait dire).
- **Texte du projet** : `get_timeline_transcript` avec l'ID du montage ; il
  vérifie la sélection (ordre, plages), pas les coupes.
- **Texte du son monté** : `transcribe_timeline`, verbatim et sans, deux
  fois en tout ([SKILL.md](SKILL.md), étape 12). Une différence est une
  alerte à examiner ; un accord ne prouve ni l'absence de question ni
  l'intelligibilité.

**Lis les deux lectures du son monté** : le verbatim invente des « donc »
ou des « et » en tête de plan ; un mot qu'il est seul à entendre peut être
un artefact, examine le raccord avant de le couper.

Les seuils de `review_cuts` sont réglés sur des médias synthétiques : sur
une parole continue, ce qu'il prend pour l'ambiance d'un côté peut être la
voix.
