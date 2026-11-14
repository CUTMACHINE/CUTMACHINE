# Import MOTION-WEB (MW4)

Outils de MOTION-WEB-2026-10,
MW4 : les compositions du registre HyperFrames deviennent des pièces de la
bibliothèque de motion (`motion-piece.v1`). Ils ne font pas partie du moteur
et ne sont liés à aucune cible CMake.

| Fichier | Rôle |
|---|---|
| `page.mjs` | ouvrir une page comme le runtime HyperFrames : bibliothèques du CDN servies en local, sous-compositions montées |
| `extract.js` | dans la page, lire la composition comme des données : arbre flexbox du format, clés aux bornes de chaque tween GSAP, courbes nommées, textes en props |
| `import.mjs` | une pièce par page, validée par le moteur (`VALIDATOR`) ; une valeur refusée est retirée et notée |
| `pack.py` | ranger les pièces (prêtes / à revoir), noter leurs écarts, signaler les imitations d'interfaces de marque |
| `mechanisms.py` | les mouvements que les pièces partagent, comptés par famille |
| `generic/` | trois pièces génériques écrites à partir de ces mouvements |
| `check-bake.mjs` | vérifier contre Chromium que les tracés dont `extract.js` cuit la transformation SVG couvrent les mêmes pixels |
| `compare.mjs`, `compare-on-mac.sh` | sur le Mac : la page rendue par Chromium contre la pièce rendue par CUTMACHINE, aux mêmes images |

## Importer

```sh
node import.mjs <hyperframes>/registry <cdn>/node_modules <sortie>
python3 -I pack.py <sortie> <classes.json> <dossier-bibliotheque>
```

`classes.json` vient de `tools/motion-web-measure/analyze.py`. `VALIDATOR`
désigne un binaire qui lit une pièce (`ParseMotionPiece`) et en évalue
toutes les images ; il a servi depuis un build Linux de la partie portable.
Sans lui, les pièces sont écrites sans être vérifiées.

`pack.py` remplace son dossier de sortie après avoir écrit toutes les
pièces et le rapport dans un dossier temporaire voisin. Une erreur conserve
la sortie précédente. Un rapport sans pièce validée est refusé, ainsi
qu'une sortie qui remplacerait les entrées ou qui serait un lien symbolique.
Si une interruption survient pendant la publication, la copie précédente
peut rester à côté sous `.nom.previous-<identifiant>` : elle se récupère
par déplacement, après avoir vérifié les dossiers présents.

## Comparer à Chrome, sur le Mac

Après `cmake --build build`, depuis la racine du dépôt :

```sh
tools/motion-web-import/compare-on-mac.sh ~/Downloads/bibliotheque-hyperframes
```

Le script récupère HyperFrames au commit de l'import et les bibliothèques du
CDN (sous `~/.cache/cutmachine-motion-web` ; le Chromium de Playwright va,
lui, dans `~/Library/Caches/ms-playwright`, environ 480 Mo), puis compare dix-huit pièces de contrôle à six images chacune. Le
rapport est écrit dans `build/motion-web-comparison/comparaison.md`, avec
une image côte à côte par mesure (Chrome à gauche). Les deux côtés doivent
voir les mêmes polices : installe celles que les pièces nomment (Inter
d'abord). Sinon Chrome en prend une autre sans le dire, et CUTMACHINE
refuse la pièce.

La capture de Chrome ne garde que la pièce : la page autour est masquée, et
l'habillage de démo d'un composant monté (fond, ombre, bordure, coins) est
ôté, comme l'importeur le laisse hors de la pièce.

Le SSIM ne suffit pas à juger : une pièce que le moteur ne dessinait pas du
tout a obtenu 0,95 (du noir contre du noir). Les images côte à côte font foi.
Le SSIM et le PSNR sont mesurés sur fond gris, dans le cadre de ce que l'un
ou l'autre rendu dessine. Sur l'image entière, un lower third contre une
image vide obtiendrait déjà 0,99.

La comparaison rend un code non nul si une pièce manque, si un rendu ou
FFmpeg échoue, ou si une mesure ou l'image comparative manque. Le détail
reste dans les rapports JSON et Markdown. Un score faible reste à juger
visuellement ; deux images transparentes sont indiquées comme vides.
La comparaison peut être relancée dans le même dossier de sortie.

## Tests locaux, sans registre ni réseau

CTest contient les tests Python d'empaquetage. Lorsque Node est trouvé à la
configuration, il ajoute les refus d'entrées de comparaison et les scènes
GSAP locales confrontées au moteur. Ces dernières vérifient les répétitions,
les délais entre cycles, les allers-retours, les vitesses imbriquées, puis
une comparaison complète et ses erreurs de moteur et de FFmpeg.

Pour préparer leurs dépendances, celles que la comparaison utilise déjà :

```sh
WEB_TESTS="$HOME/.cache/cutmachine-motion-web-tests"
npm install --prefix "$WEB_TESTS" --no-save playwright@1.56.1 gsap@3.14.2
"$WEB_TESTS/node_modules/.bin/playwright" install chromium
export MOTION_WEB_NODE_MODULES="$WEB_TESTS/node_modules"
cmake -S . -B build
cmake --build build -j
ctest --test-dir build -R motion_web --output-on-failure
```

`PLAYWRIGHT_MODULE` et `CHROMIUM_PATH` peuvent désigner une installation
existante. Les tests n'installent et ne téléchargent rien. L'intégration
est « Skipped » (77) lorsque Playwright, GSAP ou Chromium manque : cela ne
vaut pas une comparaison réussie.
