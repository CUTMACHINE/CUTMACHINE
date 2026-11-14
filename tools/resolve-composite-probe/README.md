# Sonde de composition Resolve (PA8)

Dans quel signal DaVinci Resolve mélange-t-il deux images ? Le manuel 21.1
place la composition des pistes dans l'espace de la timeline (ch. 144) et ne
dit pas si le mélange se fait sur les valeurs codées ou en lumière linéaire.
Cette sonde le mesure, pour
`PARITE-2026-11` PA8 : un masque
ou une clé adoucie se mélange au fond exactement comme une opacité.

## Le protocole

Un plan de seize barres de gris à des niveaux connus, seul sur V1, donc
au-dessus du noir. Une image fixe à 100 % d'opacité, une à 50 %, sous trois
réglages couleur. Dans un codage `e`, 50 % au-dessus du noir donne le niveau
`e⁻¹(e(x) / 2)` : le code divisé par deux dans le gamma de la timeline, la
moitié de la lumière en linéaire.

```sh
cd tools/resolve-composite-probe
python3 bars.py /tmp/barres.ppm
ffmpeg -loop 1 -framerate 24 -i /tmp/barres.ppm -t 2 \
  -vf "scale=out_color_matrix=bt709:out_range=tv,format=yuv422p10le" \
  -c:v prores_ks -profile:v 3 -color_primaries bt709 -color_trc bt709 \
  -colorspace bt709 -color_range tv ~/Movies/barres.mov

open -a "/Applications/DaVinci Resolve/DaVinci Resolve.app"
perl -e 'alarm 420; exec @ARGV' python3 -u probe.py /tmp/sonde ~/Movies/barres.mov
python3 analyse.py /tmp/sonde
```

`probe.py` ouvre ou crée le projet **CUTMACHINE sonde composition** : à ne
lancer que sur un Resolve qu'on vient d'ouvrir, sans travail non enregistré.
Il ne pose qu'un plan, règle son opacité par `SetProperty("Opacity")` et
exporte par `ExportCurrentFrameAsStill` depuis la page Color. Aucune piste
ajoutée, aucun fondu : ces appels ont déjà figé Resolve.

## Relevé du 12 novembre 2026 (Resolve Studio 21.1.0.17)

Entrée et sortie en Rec.709 Gamma 2.4 dans les trois passes ; à 100 %,
chaque barre ressort à son niveau.

| Réglage | À 50 %, la barre blanche garde | Une barre sombre (15 %) garde | Ce qui l'explique |
|---|---|---|---|
| DaVinci YRGB, non géré | 19 % de sa lumière | 19 % | le code divisé par deux, à 0,001 près sur les quinze barres |
| RCM, timeline DaVinci WG/Intermediate | 15 % | 47 % | le code Intermediate divisé par deux, à 0,003 près, le blanc posé vers 0,24 en linéaire |
| RCM, timeline Rec.709 Gamma 2.4 | 10 % | 16 % | rien de ce qui a été essayé : ni le code d'un gamma 2,4 (0,037 d'écart), ni la lumière linéaire (0,17) |

En lumière linéaire, chaque barre garderait 50 % de sa lumière. **Resolve ne
mélange donc en lumière linéaire dans aucun des trois réglages** : il
mélange les valeurs codées de sa timeline, et la même opacité assombrit
plus ou moins selon ce codage.

Ce qui n'est pas expliqué : la passe RCM en timeline Rec.709 Gamma 2.4, où
le code divisé par deux était attendu. Les images sont en 8 bits (un pas de
0,004) ; l'écart est dix fois plus grand.

## Ce que ça dit de CUTMACHINE

- Sans gestion couleur, l'export mélange par `overlay` dans le signal
  livré : comme Resolve en YRGB.
- Sous gestion couleur, il mélange en lumière linéaire (F1.3, 18 septembre
  2026, à la parité du moniteur) : une opacité de 50 %, un fondu ou le bord
  adouci d'un masque gardent 50 % de la lumière, plus clair que Resolve
  dans ses deux réglages gérés. C'est un choix, pas un défaut ; il vaut pour
  l'opacité et les fondus depuis septembre, et pour PA8 demain.
