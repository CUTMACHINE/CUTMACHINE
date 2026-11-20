# Moteur sans fenêtre (Linux)

Le moteur, sa ligne de commande et son serveur MCP, compilés sans AppKit,
Metal ni aucun framework d'Apple, pour une machine sans fenêtre : un
conteneur Linux, un runner de CI, une session d'agent dans le cloud. Lot
HEADLESS-2026-10, ouvert le 14
octobre 2026.

Ce n'est pas un second build à tenir à la main. Le `CMakeLists.txt` d'ici
relit dans celui de la racine :

- les sources de `cutmachine_model` (le bloc `add_library(cutmachine_model
  ...)`) ;
- l'URL, l'empreinte SHA-256 et le `SOURCE_SUBDIR` de whisper.cpp,
  OpenColorIO, Yoga, zstd, Strudel et fraction.js, donnés à
  `FetchContent_Declare` comme là-bas (zstd, par la même recette :
  `cmake/CutmachineZstd.cmake`, depuis le 24 octobre 2026 ; le décodeur pour
  les étalonnages de Resolve, et l'encodeur sans fils d'exécution pour
  l'archive des projets, DEC44 ; Strudel et fraction.js, rien de compilé,
  pour la suite du son, SN9) ;
- les suites de tests (`portable_tests.py`), exécutables C++ comme scripts
  Python ou CMake, déclarées à la main ou par `cutmachine_add_test`
  (`cmake/CutmachineAddTest.cmake`, STRUCTURE-2026-11 ST2).

Un seul fichier est remplacé :

- `src/app/main.mm` (AppKit) devient `src/agent/HeadlessMain.cc`. Les deux passent la
  ligne de commande à `RunCliCommand` (`src/agent/CliMain.h`) : chaque commande
  existe ici, lue de la même façon.

`CommonCrypto` (SHA-256) est servi par OpenSSL, par un en-tête qui n'est que
sur le chemin d'inclusion de ce build (`compat/`).

## Compiler

Dépendances système (Ubuntu 24.04) : `cmake`, `ninja-build`, `clang`,
`pkg-config`, `ffmpeg` et ses bibliothèques de développement
(`libavformat-dev`, `libavcodec-dev`, `libavutil-dev`, `libswresample-dev`,
`libswscale-dev`), `libsqlite3-dev`, `libssl-dev`, ce qu'OpenColorIO reprend
au système (`libyaml-cpp-dev`, `libexpat1-dev`, `libimath-dev`,
`libpystring-dev`, `zlib1g-dev`), Python 3 et git. Le réseau, à la première
configuration : CMake télécharge les six dépendances et vérifie leur
empreinte. `-DFETCHCONTENT_SOURCE_DIR_<NOM>=<dossier>` (`WHISPERCPP`,
`OPENCOLORIO`, `YOGA`, `ZSTD`, `STRUDEL`, `FRACTIONJS`) remplace un
téléchargement par une copie locale, dont l'empreinte n'est alors pas
vérifiée. Dans une session cloud, la politique réseau refuse les archives
`.tar.gz` de GitHub (403) et de Codeberg, que les six épingles nomment :
le 30 octobre 2026, une session les a récupérées par `git` au commit épinglé
et passées par `FETCHCONTENT_SOURCE_DIR_*`, Strudel reconstruit à partir de
ses paquets npm 1.2.6 ; la vérification SHA-256 était contournée, et rien ne
dit que ces copies sont l'octet des archives. Ce build-là compile (914
cibles, aucune erreur), zstd compris, ajouté le 24 octobre ; ses suites n'y
ont pas été lancées. L'archive des projets en schéma 3 (DEC44) demande au
SQLite du système `VACUUM INTO`, `RETURNING`, `json_each` dans un trigger et
des fonctions déclarées `SQLITE_INNOCUOUS` : la 3.45 d'Ubuntu 24.04 les a,
et `project_archive` y passe (Docker, aarch64, 11 novembre 2026).

```sh
cmake -S headless -B build-headless -G Ninja \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DBUILD_TESTING=ON -DHEADLESS_ALL_SUITES=ON
cmake --build build-headless
ctest --test-dir build-headless --output-on-failure
```

Les suites que le build principal marque `portable` portent le même label
ici (`ctest -L portable`). `-DHEADLESS_ALL_SUITES=ON` déclare aussi toutes
les autres qui se passent de code Apple, sous le label `engine`. C'est ce que
fait le job `headless-linux` de `.github/workflows/ctest.yml`.

### Dans Docker, depuis un Mac

```sh
docker run -d --init --name cm-headless ubuntu:24.04 sleep infinity
# installer les paquets ci-dessus dans le conteneur, puis y copier l'arbre :
git ls-files -co --exclude-standard | grep -v '^build' \
  | COPYFILE_DISABLE=1 tar --no-mac-metadata -cf - -T - \
  | docker exec -i cm-headless sh -c 'mkdir -p /src && tar -xf - -C /src'
```

Trois pièges, les deux premiers constatés le 14 octobre 2026, le troisième
le 11 novembre :

- **`--init`** : sans processus init, un serveur orphelin reste zombie, son
  verrou de projet paraît tenu par un processus vivant, et
  `cutmachine_acp_agent_tests` échoue ;
- **les métadonnées de macOS** : sans `COPYFILE_DISABLE=1` et
  `--no-mac-metadata`, `tar` ajoute un fichier `._*` à côté de chaque
  fichier ; la garde de l'architecture lit un `._*.cc` binaire, et le
  catalogue des modèles de motion prend un `._bandeau.json` pour un modèle ;
- **`__muloti4`** : sur un Mac Apple Silicon le conteneur est en arm64, où
  clang délègue la multiplication vérifiée sur 128 bits de `ClipSpeed.cc` à
  compiler-rt, que l'édition de liens ne prend pas par défaut. Installer
  `libclang-rt-18-dev` et configurer avec
  `-DCMAKE_EXE_LINKER_FLAGS="--rtlib=compiler-rt --unwindlib=libgcc"`. En
  x86-64, l'architecture du runner de GitHub, clang fait le calcul en place.

`git init` et un commit dans la copie donnent à `--version` un commit à
nommer.

## Ce qui n'existe pas ici

- **Le rendu propre à Apple** : légendes (Core Text), motion (Core Text,
  Core Graphics), visages (Vision), polices. Les outils et les commandes qui
  en ont besoin refusent par leur nom (`UnsupportedOperation`) et disent où
  le trouver. Les suites `cli`, `mcp`, `export`, `caption_generation` et les
  autres qui lient ce rendu ne sont pas déclarées ici.
- **L'encodeur matériel** (VideoToolbox) : l'export passe par `--software`
  (libx264, libx265). Sans `--software`, il est refusé avant que FFmpeg soit
  lancé (`FfmpegCapabilityMissing`).
- **La génération payante** (fal.ai, GENERATIF-2026-10) : pas de transport
  HTTPS non plus. `propose_generation` répond (`ready.transport: false`),
  `generate_media` refuse par son nom, sans rien dépenser.
- **Les préférences locales** : le mode strict de timeline n'est allumé que
  par `--mcp-serve --require-timeline` ou le `require_timeline`
  d'`open_project`.

Les données de l'utilisateur vont sous `~/.local/share/cutmachine` (archive
des projets, bibliothèque de motion) et le journal sous
`~/.local/state/cutmachine` (`src/engine/UserDirectories.h`).
