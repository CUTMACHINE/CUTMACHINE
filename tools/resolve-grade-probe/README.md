# Sonde d'étalonnage Resolve (GR12)

Mesurer ce que DaVinci Resolve fait d'une CDL, de l'extérieur, et le
comparer à ce que fait CUTMACHINE. C'est la preuve que demande GR12
(`GRADE-RESOLVE-2026-10`) :
la parité d'échange, la seule parité de rendu que le lot promet.

## Pourquoi de l'extérieur

Resolve ne publie pas ses calculs. Le manuel (p. 3475) donne
`(Input × Slope + Offset)^Power` et rien d'autre : ni ce que devient une
valeur sous 0 avant la puissance, ni un écrêtage à 1, ni les poids de la
luma de la saturation. Sa licence interdit de les lire dans le binaire.

Ce que Resolve publie, en revanche, c'est un moyen de regarder le résultat :
`TimelineItem.ExportLUT` échantillonne tout l'étalonnage d'un plan dans un
`.cube`. Poser une CDL connue par `SetCDL`, exporter le cube, et chaque point
du réseau devient une mesure. Il n'y a ni rendu, ni codec, ni image entre les
deux, donc la comparaison tient à la précision des nombres que Resolve écrit
(six décimales).

## Avant de lancer

- DaVinci Resolve **Studio** ouvert : le scripting externe lui est réservé.
  Préférences > Système > Général > Scripting externe : **Local**.
- Le projet **CUTMACHINE sonde étalonnage**. `setup --create-project`
  l'ouvre ou le crée, mais seulement sur un Resolve sans travail non
  enregistré (Resolve qu'on vient de lancer, par exemple) : quitter un projet
  modifié peut ouvrir une fenêtre de dialogue que personne ne verra. Sans
  cette option, la sonde ne change jamais de projet, et toutes ses commandes
  refusent de tourner dans un autre projet que celui-là.
- La science couleur du projet : `setup --color-science` la règle et la
  relit. Une passe par réglage, chacune dans son dossier.

Python 3.9 suffit, celui du Mac. Aucune dépendance hors de la bibliothèque
standard et du module de Blackmagic.

## Le protocole

N'importe quel média fait l'affaire : un LUT exporté ne dépend pas de
l'image.

```sh
cd tools/resolve-grade-probe
ffmpeg -f lavfi -i testsrc2=size=1920x1080:rate=25 -t 2 \
       -c:v prores_ks -profile:v 3 ~/Movies/sonde.mov

open -a "/Applications/DaVinci Resolve/DaVinci Resolve.app"
python3 -u grade_probe.py setup --create-project --color-science davinciYRGB \
        --media ~/Movies/sonde.mov
python3 -u grade_probe.py check                         # version, projet, réglages couleur, GetCDL
```

L'export de LUT ne fonctionne que depuis la page Color : la sonde l'ouvre
avant d'exporter (Resolve rend `False` ailleurs, sans rien écrire).

**Passe 1, DaVinci YRGB non géré.** Le nœud travaille sur les valeurs telles
quelles : c'est la lecture la plus nette de la formule.

```sh
python3 -u grade_probe.py cdl --out ~/CUTMACHINE-mesures/gr12/yrgb
python3 grade_probe.py compare ~/CUTMACHINE-mesures/gr12/yrgb
```

**Passe 2, ACEScct.**

```sh
python3 -u grade_probe.py setup --color-science acescct
python3 -u grade_probe.py cdl --out ~/CUTMACHINE-mesures/gr12/acescct
python3 grade_probe.py compare ~/CUTMACHINE-mesures/gr12/acescct
```

**Passe 3, de vrais plans.** C'est le cœur de GR12 : un plan étalonné dans
CUTMACHINE (roues et offset, espace ACEScct), exporté par `export_cdl`. Garder
la réponse JSON de l'outil dans un fichier, puis :

```sh
python3 -u grade_probe.py cdl --from-export reponse_export_cdl.json \
        --out ~/CUTMACHINE-mesures/gr12/plans
python3 grade_probe.py compare ~/CUTMACHINE-mesures/gr12/plans
```

La sonde pose alors la table `resolve_set_cdl` que le moteur a écrite, telle
quelle : elle éprouve aussi cette table.

`--lut-size 65` donne le réseau le plus fin (environ 7 s de comparaison par
cas, contre 1 s en 33). `--only neutre pente` ne rejoue que les cas nommés.

## Les cas

Chaque cas tranche une question. Ensemble, ils séparent les douze formules
candidates deux à deux (le test le vérifie).

| Cas | Ce qu'il tranche |
|---|---|
| `neutre` | ce que le LUT exporté contient en plus de la CDL |
| `pente` | si une valeur au-dessus de 1 est écrêtée |
| `decalage_negatif` | si une valeur sous 0 est écrêtée, sans puissance |
| `decalage_colore` | un décalage par canal, les lumières de tirage |
| `puissance` | la puissance seule, entre 0 et 1 |
| `negatif_puissance` | la puissance d'une valeur négative : laissée, mise à 0 ou autre |
| `negatif_saturation` | une valeur négative dans la luma de la saturation |
| `saturation_0` | les poids de la luma, lus directement |
| `saturation_05` | une désaturation |
| `saturation_15` | une saturation qui sort de 0–1 |
| `depassement_saturation` | une valeur au-dessus de 1 avant la puissance, vue par la luma |
| `combinee` | un étalonnage ordinaire, tout ensemble |
| `forte` | une correction forte, des deux côtés de 0–1 |

À la fin, la sonde remet une CDL neutre sur le nœud 1.

## Lire le résultat

`compare` écrit `report.json` et résume chaque cas : la formule la plus
proche, l'écart de la formule du moteur, et les formules qui tiennent dans la
tolérance (2·10⁻⁴, un cinquième de code 10 bits).

Quand le cas `saturation_0` a été mesuré, `compare` en lit les poids de la
luma (chaque primaire en sort en gris de son poids) et ajoute les formules
`…/mesure/…` qui les prennent.

Une formule se lit `base/poids/sortie` :

- **base**, ce que devient `x × slope + offset` avant la puissance :
  `pass` laisse une valeur négative ou nulle telle quelle (le style sans
  écrêtage d'OpenColorIO, celui du moteur), `zero` la ramène à 0, `unit`
  écrête à [0, 1] (le style v1.2 de l'ASC) ;
- **poids** de la luma de la saturation : `rec709` ou `rec601` ;
- **sortie** : `none`, ou `unit` pour un écrêtage à [0, 1] après la
  saturation.

La formule du moteur est `pass/rec709/none` ([`ColorCdl.h`](../../src/ColorCdl.h)).
Les cubes que Resolve écrit ne sortent pas de [0, 1], en ACEScct non plus :
le moteur y est donc jugé écrêté de même, `pass/rec709/unit`, ce que rend
aussi son export sans gestion couleur (`BuildColorGradeCube`). L'écart non
écrêté reste dans `report.json` (`engine_unclamped`).

**Lire `neutre` d'abord.** S'il n'est pas l'identité, le LUT exporté
emporte autre chose que le nœud (la gestion couleur du projet, par exemple),
et les autres cas de cette passe ne se lisent pas directement.

`check` et chaque cas relèvent aussi ce que rend `GetCDL`. Resolve 21.1 ne
l'a pas.

Ce que la mesure du 10 octobre 2026 a donné (Resolve Studio 21.1.0.17) est
dans GR12 : en ACEScct, la
formule du moteur tient à 0,03 code ; en DaVinci YRGB, la saturation prend
les poids 0,21 / 0,70 / 0,09.

## Relevés à la main : les roues

L'API ne règle ni Lift, ni Gamma, ni Gain. Pour relever une roue, la régler à
la main dans la palette Primaries, puis exporter le plan tel quel :

```sh
python3 -u grade_probe.py export --label "gamma maître -0,10" \
        --out ~/CUTMACHINE-mesures/gr12/roues
python3 grade_probe.py compare ~/CUTMACHINE-mesures/gr12/roues
```

`compare` en donne l'écart à l'identité, la réponse le long de l'axe des gris
et ce que chaque canal doit aux deux autres. C'est un relevé, pas un
engagement : le lot a écarté la « parité de nombres » le 1er octobre 2026.
Adopter les nombres de Resolve rouvrirait cette décision.

## Si Resolve ne répond plus

Chaque appel à Resolve passe sous un délai de garde (`--timeout`, 30 s par
défaut). S'il expire, la sonde s'arrête avec le code 124 et dit sur quel
appel ; `journal.txt`, dans le dossier de sortie, le note avant chaque appel.
**Ne pas relancer** : une fenêtre attend peut-être dans Resolve. Regarder
l'écran d'abord.

Le client Python de Blackmagic sort parfois en erreur 139 (segfault) *après*
son dernier appel. Le travail est fait : `records.jsonl` le dit, il n'y a
rien à rejouer.

La sonde n'appelle que `GetProjectManager`, `GetCurrentProject`, `GetName`,
`GetSettings`, `GetCurrentTimeline`, `GetItemListInTrack`, `GetMediaPool`,
`ImportMedia`, `CreateTimelineFromClips`, `GetCurrentPage`, `OpenPage`,
`SetCDL`, `GetCDL` s'il existe et `ExportLUT` ; sur demande,
`GetProjectListInCurrentFolder`, `LoadProject`, `CreateProject` et
`SetSettings`. `ImportMedia` essaie d'abord la forme que le README de
Blackmagic recommande, `[{"FilePath": …}]`, qui n'importe rien en 21.1.0.17,
puis la forme dépréciée `[chemin]`, qui fonctionne. Elle ne fait ni suppression avec décalage, ni ajout ou
verrouillage de piste, ni insertion de titre : les appels du gel du
29 septembre 2026.

## Fichiers

| Fichier | |
|---|---|
| `grade_probe.py` | la sonde : lecture des cubes, formules candidates, comparaison (pures), puis les appels à Resolve |
| [`tests/resolve_grade_probe_tests.py`](../../tests/resolve_grade_probe_tests.py) | les tests, contre un faux Resolve ; `ctest -R resolve_grade_probe` |

Dans le dossier de sortie : un `.cube` par cas, `records.jsonl` (une ligne
par mesure, précédée des réglages de la session), `journal.txt`, et
`report.json` après `compare`.
