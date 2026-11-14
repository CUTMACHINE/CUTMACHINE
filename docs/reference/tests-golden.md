# Images de référence dans les tests

FC9, FILMCRAFT-2026-10. Un rendu fait sur
le CPU se compare à une image PNG regardée par quelqu'un, rangée sous
`tests/fixtures/golden/`, et non plus seulement à quelques pixels choisis par
le test. Le module est `tests/golden_image.h` (repris de FilmCraft,
`crates/testkit/src/golden.rs`, MIT ou Apache-2.0) ; il se teste lui-même dans
`cutmachine_golden_image_tests`, et sert aujourd'hui aux effets de motion
(flou, ombre portée, halo, déplacement ; grain, goo et tranches depuis MA7)
dans `cutmachine_motion_parity_tests`.

## S'en servir

```cpp
#include "golden_image.h"

GoldenCheck check;
check.name = "motion_effects_blur";            // tests/fixtures/golden/<name>.png
check.reference_dir = GOLDEN_REFERENCE_DIR;
check.output_dir = GOLDEN_OUTPUT_DIR;
check.depth = GoldenDepth::Sixteen;            // ou Eight
check.tolerance = {90.0, 1.0, 0.0};            // PSNR min, écart max, p99
const GoldenResult result = CheckGoldenImage(
    GoldenImageFromFloat(width, height, 4, rgba), check);
Check(result.passed, result.message);
```

La suite qui s'en sert compile `tests/golden_image.cc` avec ses sources et
reçoit les deux dossiers par `target_compile_definitions` (voir
`cutmachine_motion_parity_tests` dans `CMakeLists.txt`). Une image est en
flottants dans [0, 1], RVB ou RVBA, entrelacée, lignes du haut ; des octets ou
des mots de 16 bits s'y convertissent (`GoldenImageFromBytes`,
`GoldenImageFromWords`). Le PNG passe par libavcodec, que le modèle lie déjà :
aucune dépendance de plus.

## Les trois mesures

Le rendu est d'abord arrondi à la profondeur de la référence (8 ou 16 bits),
comme elle l'a été en l'écrivant : un rendu inchangé se compare à zéro sur la
machine qui l'a écrite. Les écarts sont en niveaux de cette profondeur (0 à
255, ou 0 à 65 535).

- **PSNR** : sur tous les échantillons, en dB, contre la pleine échelle ;
  infini pour deux images identiques.
- **Écart max** : la plus grande différence d'un échantillon.
- **p99** : le 99ᵉ centile, sur les pixels, de la plus grande différence de
  chaque pixel. Ce que 99 % de l'image respecte : quelques pixels sur un bord
  d'arrondi ne cachent pas un décalage de toute l'image, ni l'inverse.

Chaque test déclare sa tolérance ; elle est exacte par défaut.

## Un échec

Le message donne les trois mesures, la tolérance, et deux chemins, dans le
dossier de sortie du test (`build…/golden-output/`, ou
`CUTMACHINE_GOLDEN_OUTPUT` s'il est posé) :

- `<name>.actual.png`, le rendu tel qu'il a été comparé ;
- `<name>.diff.png`, la différence de chaque canal multipliée par huit, la
  différence d'alpha reportée sur les trois couleurs, opaque.

Une référence absente fait échouer le test, qui écrit quand même le rendu
pour qu'on le regarde.

## Réécrire les références

```sh
CUTMACHINE_BLESS=1 ./build-headless/cutmachine_motion_parity_tests
```

`CUTMACHINE_BLESS` à `1`, `true`, `yes` ou `on` réécrit chaque référence au
lieu de la comparer. Une référence de plus de 64 Kio est refusée : rendre plus
petit. Regarde l'image réécrite, et sa différence avec l'ancienne dans la
revue, avant de la committer : une référence réécrite sans être regardée ne
prouve plus rien.

## Pourquoi ces tolérances

Le principe : tant qu'un rendu
n'est pas exact d'une machine à l'autre, il se compare à une tolérance
déclarée et mesurée.

Les effets de motion sont des boucles en `float` et en `double`, plus
`std::exp` (le noyau gaussien) et `std::floor` (la grille du bruit). D'une
machine à l'autre changent les derniers bits : le compilateur qui fond une
multiplication et une addition en une FMA là où la cible en a une (toujours
sur arm64, sur x86-64 seulement si on compile pour), et l'`exp` de la libm.
Un tel écart déplace un échantillon de bien moins d'un niveau ; il ne se voit
qu'où le flottant tombe sur un bord d'arrondi : un niveau, sur quelques
pixels. La tolérance de ces trois tests laisse donc passer un niveau sur
n'importe quel échantillon, garde le p99 à zéro et un PSNR d'au moins 60 dB
(8 bits) ou 90 dB (16 bits). Un vrai changement (un noyau d'un pixel plus
large, une ombre décalée d'un pixel, une autre graine) déplace des bords
entiers de beaucoup de niveaux et échoue sur les trois.

Mesuré le 17 octobre 2026, sous Linux x86-64 (clang 18) : `MotionEffects.cc`
et les scènes recompilés autrement que le build par défaut (`-O3`, qui a
écrit les références), rendu brut en flottants comparé à celui du build par
défaut, puis rendu arrondi comparé aux références.

| Build | Flou σ 4 | Ombre et halo | Déplacement | Contre les références |
|---|---|---|---|---|
| `-O0` | identique | identique | identique | 0 partout |
| `-march=x86-64-v3 -ffp-contract=fast` (FMA) | identique | 790 échantillons, au plus 6,0 × 10⁻⁸ | identique | 0 partout |
| `-march=x86-64-v3 -ffast-math` | identique | 1 232 échantillons, au plus 1,2 × 10⁻⁷ | identique | 0 partout |

Le pire écart brut fait 3 × 10⁻⁵ niveau en 8 bits et 8 × 10⁻³ niveau en
16 bits : aucun arrondi n'a bougé. La tolérance d'un niveau a donc une
marge que la mesure ne consomme pas ; elle couvre le cas, possible mais non
observé, d'un échantillon pile sur un bord d'arrondi.

Dans l'autre sens, ce que la tolérance arrête, mesuré de la même façon : une
ombre décalée d'un pixel (PSNR 31,9 dB, écart max 58, p99 49), un halo de
force 2,05 au lieu de 2 (47,3 dB, max 6, p99 4), une autre graine de
déplacement (14,2 dB, max 255), un flou de σ 5 au lieu de 4 (39,7 dB, max
2 570 en 16 bits). Un flou de σ 4,1 ou 4,5 ne change rien : le flou à trois
boîtes de SVG arrondit σ à une largeur de boîte entière, la même (8) de
σ 3,99 à 4,52.

Ce n'est pas encore la mesure d'un Mac : ni l'`exp` d'Apple ni arm64 n'y
entrent. Le premier passage de la suite sur un Mac la donne ; si elle dépasse
la tolérance, c'est la tolérance qu'on revoit, chiffres à l'appui, pas la
référence qu'on réécrit sur le Mac.

Sur un Mac (arm64, Apple clang 17), le 19 octobre 2026, les trois
références écrites sous Linux passent à zéro. Ce jour-là, MA7
(MOTION-AFTER-2026-09) en ajoute
trois, écrites sur ce Mac : `motion_effects_grain` (grain de 0,3, cellules
de 2 pixels), `motion_effects_goo` (goo de σ 3) et `motion_effects_slice`
(tranches de 6 pixels sur des bandes de 8), même scène, même tolérance.
Elles n'ont pas été comparées sous Linux. Le grain ne fait que des calculs
exacts (cellules entières, hachage) ; le goo passe le flou par une rampe
raide autour de la moitié de la couverture, qui multiplie un écart par
moins de 20 ; une tranche décale une ligne d'un nombre entier de pixels
arrondi : un écart d'un ulp sur une valeur à 0,5 près d'un entier
décalerait une ligne entière. Si le premier passage sous Linux en trouve
un, c'est là qu'il faut regarder.

Réécrire les références (`CUTMACHINE_BLESS=1`) sur un Mac réécrit aussi
les trois écrites sous Linux, avec d'autres octets pour les mêmes pixels
(constaté ce jour-là) : ne committe que celles que tu as voulu changer.
