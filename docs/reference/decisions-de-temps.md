# Décisions de temps

> Relu contre le code le 3 octobre 2026.

CUTMACHINE charge un package projet v2, ouvre chaque source média par
son ULID, puis résout le scrub en clés de cache `(source_id, source_frame)`.
Les intervalles des clips sont semi-ouverts : `[timeline_in, timeline_in +
duration)`. Un trou ne déclenche aucun décodage et est rendu en noir.

- `RationalTime::rescale` refuse les conversions non exactes afin de ne jamais
  déplacer silencieusement un raccord.
- `to_frames` arrondit vers le bas vers la frame qui contient la position et
  accepte un taux rationnel, par exemple `30000/1001`.
- `TimelineViewport` est l'unique frontière temps/pixels. `TimeToX` conserve
  le calcul intermédiaire en `long double`; `XToTime(x, rate)` arrondit au tick
  le plus proche du timebase demandé, avec les demi-ticks à l'opposé de zéro.
  Dans le timebase NTSC `30000`, les frontières de frames `30000/1001` sont
  les multiples de `1001` ticks : leur aller-retour est donc exact. Ce choix
  symétrique rend aussi déterministes les vues dont `view_start` est négatif.
- `sources[].rate` est interprété comme une cadence rationnelle, tandis que le
  `rate` d'un `RationalTime` est un timebase entier. Ainsi, une frame à
  `30000/1001` dure `1001` ticks dans un timebase `30000`; cette convention est
  nécessaire car `RationalTime::rate` ne peut lui-même contenir un quotient.
- Le playhead possède deux grilles, basculées par `Alt+Shift+M`. En mode Image, sa
  position est arrondie à l'image la plus proche selon la cadence rationnelle
  de référence (celle de la séquence ; à défaut, la première source), y
  compris les multiples de `1001` à `30000/1001`. En mode
  Échantillon, elle est arrondie à l'échantillon 48 kHz le plus proche. Les
  demi-pas sont arrondis à l'opposé de zéro par calcul entier 128 bits ; aucun
  temps ne transite en `double` pour cette quantification.
- Les pistes vidéo sont classées par leur champ `index` et compositées du bas
  vers le haut. Un trou sur une piste supérieure révèle les pistes inférieures.
- Les médias vidéo portant un canal alpha sont composités dans Metal. Chaque
  clip vidéo possède aussi une opacité exacte `num/den`, modifiable dans
  l’Inspecteur ou par l’opération `SetClipOpacity`; les fondus multiplient cette
  valeur et l’export opaque en tient compte.
