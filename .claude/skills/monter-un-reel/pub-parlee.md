# La pub parlée

Quelqu'un dit un texte face caméra, en vertical, et l'image part par moments
sur ce qu'il nomme. C'est une interview très courte : la transcription, les
bornes, le resserrement, les plans de coupe et les sous-titres sont ceux du
skill `monter-une-itw`. Ce fichier ne garde que ce qui est propre au
format ; ses chiffres sont des repères, pas des cibles.

## La forme

- **Trois pistes** : la parole (image et son liés), les plans de coupe ;
  rien d'autre dans la timeline.
- **La voix ouvre le film** : son premier mot dans les six premières
  images.
- **Le film finit sur son dernier mot**, un appel à l'action dit au visage
  (0 à 3 images d'air).
- **Une seule prise**, presque entière, resserrée en 3 à 6 fragments.
- **Deux à quatre plans de coupe**, chacun sur ce que la personne nomme ;
  une énumération, un plan par élément.
- **Les coupes dans la prise ne sont pas toutes à couvrir** : une partie
  reste à l'image.

## Ce qui s'ajoute à la timeline

- **Des sous-titres incrustés**
  ([sous-titres.md](../monter-une-itw/sous-titres.md)).
- **Une musique**, coupée à la longueur du film, jamais l'inverse ; un de
  ses événements tombe sur une coupe ou un mot ; −22 à −27 LUFS sous la
  voix.
- **Un logo de fin** de 4 s, s'il est demandé : une vidéo, ingérée comme un
  rush.
- **Trois formats** : 1080×1920, 1080×1080 recadré, 1920×1080 aux côtés
  floutés ([moteur.md](moteur.md), « Décliner en formats »).

## Ce qui se regarde avant de rendre

Un visage de moins de 0,7 s entre une coupe de parole et un plan de coupe
se regarde (`inspect_cut`) : prolonge le plan de coupe jusqu'à la coupe, ou
garde le mot à l'image.
