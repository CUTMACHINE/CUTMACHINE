# La pub parlée

Quelqu'un dit un texte face caméra, en vertical, et l'image part par moments
sur ce qu'il nomme. C'est une interview très courte : la transcription, les
bornes, le resserrement, les plans de coupe et les sous-titres sont ceux du
skill `monter-une-itw`. Ce fichier ne garde que ce qui est propre au
format. Ce sont des repères tirés de pubs montées dans le moteur, pas des
cibles, d'avant le cadrage et la vitesse.

## Ce qui revient d'un film à l'autre

- **Trois pistes** : la parole (image et son liés), les plans de coupe ;
  rien d'autre dans la timeline.
- **La voix ouvre le film** : son premier mot dès les premières images.
- **Le film finit sur son dernier mot**, un appel à l'action dit au visage.
- **Une seule prise**, presque entière, resserrée en quelques fragments.
- **Peu de plans de coupe**, chacun sur ce que la personne nomme ; une
  énumération, un plan par élément.
- **Les coupes dans la prise ne sont pas toutes couvertes** : certaines
  restent à l'image.

## Ce qui se fait souvent hors de la timeline

- **Des sous-titres incrustés**
  ([sous-titres.md](../monter-une-itw/sous-titres.md)).
- **Une musique**, coupée à la longueur du film, jamais l'inverse ; un de
  ses événements tombe sur une coupe ou un mot, nettement sous la voix.
- **Un logo de fin** : une vidéo, ingérée comme un rush.
- **D'autres formats** : un carré recadré, un 16:9 aux côtés floutés
  ([moteur.md](moteur.md), « Décliner en formats »).

## Ce qui se regarde avant de rendre

Un visage de moins de 0,7 s entre une coupe de parole et un plan de coupe
se regarde (`inspect_cut`) : prolonge le plan de coupe jusqu'à la coupe, ou
garde le mot à l'image.
