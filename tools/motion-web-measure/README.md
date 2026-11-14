# Mesure MOTION-WEB

Outils de la mesure du 11 octobre 2026.
Ils ne font pas partie du moteur et ne sont liés à aucune cible CMake.

- `trace.mjs` ouvre chaque composition du registre HyperFrames dans Chromium
  headless. Il en relève les timelines GSAP enregistrées sur
  `window.__timelines` : chaque tween avec ses propriétés, sa courbe, son
  début, sa durée, son `stagger` et la nature de sa cible. Le relevé lit des
  données et ne capture aucun pixel.
- `analyze.py` classe ce relevé contre `docs/MOTION_FORMAT.md`, au niveau des
  noms de propriétés seulement.
- `easefit.py` donne l'écart entre les courbes polynomiales de GSAP et la
  meilleure `cubic-bezier` qu'accepte le format.

Le traceur exécute le JavaScript des pages du registre dans le bac à sable de
Chromium. Les bibliothèques demandées à `cdn.jsdelivr.net` sont servies depuis
un `node_modules` local ; tout autre accès réseau est coupé.

```sh
git clone --depth 1 https://github.com/heygen-com/hyperframes hf
mkdir cdn && (cd cdn && npm init -y && npm i gsap@3.14.2 three@0.181.0 \
  three147@npm:three@0.147.0 lottie-web d3 topojson-client d3-delaunay \
  clipper-lib us-atlas@3)
node trace.mjs hf/registry cdn/node_modules all.jsonl
python3 -I analyze.py all.jsonl
python3 -I easefit.py            # numpy, scipy
```

`PLAYWRIGHT_MODULE` désigne le module Playwright s'il n'est pas résolu
depuis ce dossier. `CHROMIUM_PATH` désigne le binaire de Chromium.
