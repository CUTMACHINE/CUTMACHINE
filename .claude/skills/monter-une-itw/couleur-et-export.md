# Couleur et export

## Ce que les rushes déclarent

L'ingest lit l'entrée couleur de chaque rush (les métadonnées d'une caméra
Sony, sinon les balises du fichier), et `conform_color_management`, lancé
d'abord avec `preview: true`, règle l'entrée du projet sur ce que déclarent
les rushes et active la gestion couleur pour du S-Log3 ou du HLG.

**Pour vérifier qu'un rush log est bien converti, c'est la saturation qui
tranche, pas les noirs** : compare `measure_color` (`display.saturation.mean`)
avec et sans gestion couleur, au même instant. Mesuré sur une série : un
facteur deux à quatre entre rush et livrable ; les valeurs absolues ne
veulent rien dire (un studio blanc bien étalonné sort à 9).

## Étalonner sous gestion couleur

N'éteins jamais la gestion couleur pour exporter un étalonnage, et ne
compense pas la saturation à la main. Un réglage fait sans elle se reprend
une fois activée, il ne se convertit pas.

## Exposer sur le visage

Un montage livré sans étalonnage n'est pas fini, même quand la consigne n'en
parle pas : `propose_grade` expose et équilibre chaque rush sur les visages.
Écarte de `clip_ids` les plans dont la scène doit rester sombre ou claire,
applique, puis remesure.

**En HLG, le visage vise +1,5 et le plafond est la sortie** (choix de l'auteur
sur écran HDR, le 6 novembre 2026, contre +0,7 en SDR) : `propose_grade` le
fait seul quand la séquence sort en HLG, et une remontée tient tant qu'elle
n'envoie pas plus de l'image sur le code haut. Passer
`highlight_ceiling_stops` remet la règle en diaphragmes de scène.

**La cible est la tienne, pas celle de l'outil, et le gris moyen est un
plancher, pas une cible** : un premier run à +1,4/+2,4 diaphragmes a été
jugé un peu surexposé, un second à +0,5 encore un peu bas. Garde les plans à
moins d'un demi-diaphragme entre eux, et ne relève `highlight_ceiling_stops`
que sur un plan que tu as regardé.

## La musique sous une interview

Une musique sous une interview reste à plat, à un gain fixe sous la voix
(−20 dB sur un film validé), sans ducking : l'auteur a jugé le 6 novembre 2026
que la musique qui remonte dans les pauses pompe, même adoucie.
`propose_ducking` sert une voix off sur une musique présente, pas une
interview.

## Exporter

- **Pour livrer en 1080×1920 depuis une séquence 2160×3840** :
  `update_sequence`, export, puis remets la séquence à son format de
  travail — sauf si elle porte des sous-titres stylés : leurs tailles sont
  en pixels de séquence, et la remettre en 2160×3840 les diviserait par deux
  au prochain export ; laisse-la alors en 1080×1920.
- Ne passe **jamais** `overwrite` sur un nom de livrable existant : supprime
  d'abord l'ancien fichier.
