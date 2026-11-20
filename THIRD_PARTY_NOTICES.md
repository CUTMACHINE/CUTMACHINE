# Avis de tiers

CUTMACHINE est distribué sous la GNU Affero General Public License, version 3
ou toute version ultérieure (`AGPL-3.0-or-later`) : voir [`LICENSE`](LICENSE).

Ce fichier relève le code et les ressources d'autres projets que le build de
CUTMACHINE intègre, avec leur version, leur licence et l'endroit où ils
interviennent. Le build le copie dans
`CUTMACHINE.app/Contents/Resources/licenses/`, avec les fichiers de la
dernière colonne. Relevé le 3 octobre 2026 sur ce qui est réellement compilé
(PRODUIT-2026-10b, PRD23).

Les chemins de la colonne « Texte de la licence » sont ceux d'un build
configuré dans `build/`. Ceux de `build/_deps/*-src/` et de `build/ext/`
existent dès la configuration ; ceux des bibliothèques qu'OpenColorIO
construit, notés `ext/…`, sont sous `build/_deps/opencolorio-build/ext/build/`
et n'existent qu'après la première compilation.

| Composant | Version | Licence | Où il intervient | Texte de la licence | Dans `Resources/licenses/` |
|---|---|---|---|---|---|
| [whisper.cpp](https://github.com/ggml-org/whisper.cpp), avec ggml | v1.7.4, commit `8a9ad7844d6e2a10cddf4b92de4089d7ac2b14a9` | MIT | binaire, lié statiquement | `build/_deps/whispercpp-src/LICENSE` | `whisper.cpp-LICENSE` |
| [OpenColorIO](https://github.com/AcademySoftwareFoundation/OpenColorIO) | v2.5.2, commit `c52966a6677723d5bd2dbef0ccec3fed9cbc3790` | BSD-3-Clause | binaire, lié statiquement | `build/_deps/opencolorio-src/LICENSE` | `OpenColorIO-LICENSE` |
| SampleICC, version allégée incluse dans OpenColorIO | 1.2.6 | ICC Software License 0.2 | binaire | `build/_deps/opencolorio-src/THIRD-PARTY.md` | `OpenColorIO-THIRD-PARTY.md` |
| xxHash, inclus dans OpenColorIO | 0.8.1 | BSD-2-Clause | binaire | `build/_deps/opencolorio-src/THIRD-PARTY.md` | `OpenColorIO-THIRD-PARTY.md` |
| Lecteur CDL d'OpenColorIO (`FileFormatCDL.cpp`), Cinesite VFX | celle d'OpenColorIO | BSD-3-Clause | binaire | en-tête de `build/_deps/opencolorio-src/src/OpenColorIO/fileformats/FileFormatCDL.cpp` | `OpenColorIO-FileFormatCDL-NOTICE` |
| [expat](https://github.com/libexpat/libexpat), construit par OpenColorIO | 2.7.2, tag `R_2_7_2` | MIT | binaire, lié statiquement | `ext/libexpat/src/expat_install/COPYING` | `expat-LICENSE` |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp), construit par OpenColorIO | 0.8.0, tag `0.8.0` | MIT | binaire, lié statiquement | `ext/yaml-cpp/src/yaml-cpp_install/LICENSE` | `yaml-cpp-LICENSE` |
| [pystring](https://github.com/imageworks/pystring), construit par OpenColorIO | 1.1.4, tag `v1.1.4` | BSD-3-Clause | binaire, lié statiquement | `ext/pystring/src/pystring_install/LICENSE` | `pystring-LICENSE` |
| [Imath](https://github.com/AcademySoftwareFoundation/Imath), construit par OpenColorIO | 3.2.1, tag `v3.2.1` | BSD-3-Clause | binaire, lié statiquement | `ext/Imath/src/imath_install/LICENSE.md` | `Imath-LICENSE` |
| [zlib](https://github.com/madler/zlib), construit par OpenColorIO | 1.3.1, tag `v1.3.1` | Zlib | binaire, lié statiquement | `ext/zlib/src/ZLIB_install/LICENSE` | `zlib-LICENSE` |
| [minizip-ng](https://github.com/zlib-ng/minizip-ng), construit par OpenColorIO | 4.0.10, tag `4.0.10` | Zlib | binaire, lié statiquement | `ext/libminizip-ng/src/minizip-ng_install/LICENSE` | `minizip-ng-LICENSE` |
| [sse2neon](https://github.com/DLTcollab/sse2neon), récupéré par OpenColorIO | commit `227cc413fb2d50b2a10073087be96b59d5364aea` | MIT | binaire, sur Apple Silicon seulement (en-tête) | `build/ext/build/sse2neon/sse2neon-src/LICENSE` | `sse2neon-LICENSE` |
| [Yoga](https://github.com/facebook/yoga) | v3.2.1, commit `042f5013152eb81c1552dec945b88f7b95ca350f` | MIT | binaire, lié statiquement | `build/_deps/yoga-src/LICENSE` | `Yoga-LICENSE` |
| [SQLite](https://github.com/sqlite/sqlite) | 3.53.4, commit `b09c88c14082339b66c7b7158d609a771e64ca69` | domaine public (cœur) ; notices BSD des scripts de build dans LICENSE.md | cœur lié statiquement, sans shell ni extensions chargeables | `build/_deps/sqlite-src/LICENSE.md` | `SQLite-LICENSE.md` |
| [zstd](https://github.com/facebook/zstd) | v1.5.7, commit `f8745da6ff1ad1e7bab384bd1f9d742439278e99` | BSD-3-Clause (le projet est aussi publié sous GPLv2 ; c'est la licence BSD qui est reprise) | le décodeur et l'encodeur sans fils d'exécution, liés statiquement : le décodeur lit les étalonnages d'un projet Resolve (DEC15, `src/media/ResolveGrade.cc`), l'encodeur et le décodeur rangent les morceaux de l'archive des projets (DEC44, `src/engine/ArchiveManifest.cc`) | `build/_deps/zstd-src/LICENSE` | `zstd-LICENSE` |
| [Strudel](https://codeberg.org/uzu/strudel), `@strudel/core` et `@strudel/mini` | 1.2.6, commit `d198910a86fcaecd658ef528bfa6d7884e93490b`, archive du miroir GitHub https://github.com/tidalcycles/strudel/archive/d198910a86fcaecd658ef528bfa6d7884e93490b.tar.gz (même commit que Codeberg) | AGPL-3.0-or-later | ni dans le binaire ni dans l'app : sources téléchargées par le build, que l'outil Node `tools/cutmachine-sound` charge pour écrire un motif en partition (SN9, `strudel.mjs`) | `build/_deps/strudel-src/LICENSE` | `Strudel-LICENSE` |
| [Fraction.js](https://github.com/rawify/Fraction.js), utilisé par Strudel | v5.2.2, commit `859b36ee3de85172343b1cee23f3fb5d018ed091` | MIT | avec Strudel, pour l'outil Node seulement | `build/_deps/fractionjs-src/LICENSE` | `Fraction.js-LICENSE` |
| [Lucide](https://lucide.dev/), douze icônes | synchronisées le 17 août 2026 depuis `lucide-icons/lucide`, branche `main` | ISC ; MIT pour les icônes issues de Feather | bundle : PNG rastérisés au build dans `Resources/icons/` | [`assets/icons/LICENSE`](assets/icons/LICENSE), copié aussi en `Resources/icons/LICENSE` | `Lucide-LICENSE` |
| [FilmCraft](https://github.com/storytold/filmcraft), code porté en C++ | commit `09aaf9b` du 16 octobre 2026 | MIT ou Apache-2.0, au choix ; reprise sous MIT | binaire : `src/ui/TimelineView.cc` (aimant, `TimelineHitAt`), `src/TrimSession.*`, `src/engine/Operations.cc` (slide, rate stretch), `src/AudioDsp*`, `src/Edl.*`, `FormatVtt` de `src/media/Subtitles.cc` ; tests : `tests/golden_image.*`. Chaque fichier cite sa source en en-tête (FILMCRAFT-2026-10) | [`third_party/filmcraft/LICENSE-MIT`](third_party/filmcraft/LICENSE-MIT) | `FilmCraft-LICENSE-MIT` |
| [claude-remotion-skill](https://github.com/haidrrrry/claude-remotion-skill), règles et motifs adaptés | commit non relevé, adapté le 29 septembre 2026 | MIT | procédures : le skill `motion-design` (`.claude/skills/motion-design/`), que le binaire embarque (`get_procedure`). Les dix règles, la boucle de vérification, les motifs et les règles de design y sont réécrits pour le format de composition ; aucun code n'est repris. L'avis de copyright est celui que le skill cite ; il n'a pas été relu sur le dépôt d'origine | [`third_party/claude-remotion-skill/LICENSE`](third_party/claude-remotion-skill/LICENSE) | `claude-remotion-skill-LICENSE` |
| [FFmpeg](https://ffmpeg.org/) | celle de la machine | LGPL ou GPL, selon sa configuration | ni dans le binaire ni dans le bundle : chargé à l'exécution | celui de la distribution installée | — |
| Fira Sans Condensed, Medium et SemiBold | 4.203 | OFL-1.1 | tests seulement | [`tests/fixtures/fonts/OFL.txt`](tests/fixtures/fonts/OFL.txt) | — |
| [Outfit](https://github.com/google/fonts/tree/47d54ca1ebb14fd864695f1d39dc734f39017c33/ofl/outfit), police variable `Outfit[wght].ttf` | `google/fonts`, commit `47d54ca1ebb14fd864695f1d39dc734f39017c33` | OFL-1.1, sans nom réservé | tests seulement | [`tests/fixtures/fonts/outfit/OFL.txt`](tests/fixtures/fonts/outfit/OFL.txt) | — |

## Cas particuliers

- **FFmpeg.** Le build ne compile ni n'embarque FFmpeg : `cutmachine` et
  `CUTMACHINE.app` chargent à l'exécution les bibliothèques `libavformat`,
  `libavcodec`, `libavutil`, `libswresample` et `libswscale` de la machine,
  et lancent l'exécutable `ffmpeg` qu'elle fournit. Leur licence est celle de
  cette distribution : LGPL 2.1 ou ultérieure par défaut, GPL quand elle a été
  configurée avec `--enable-gpl`, option sans laquelle le `configure` de
  FFmpeg refuse `libx264` et `libx265`. Celle du Mac de l'auteur (conda-forge,
  FFmpeg 8.1.2) porte `--enable-gpl`, `--enable-version3`, `libx264` et
  `libx265`.
- **Polices.** Les deux Fira Sans Condensed de `tests/fixtures/fonts/` ne
  servent qu'aux tests de rendu du motion (`MOTION_FONT_DIR`) : elles ne sont
  ni dans le binaire ni dans le bundle. Outfit, dans son propre dossier
  (`tests/fixtures/fonts/outfit/`, `MOTION_VARIABLE_FONT_DIR`), ne sert
  qu'aux tests de la graisse d'une police variable (MOTION-WEB-2026-10 MW7),
  en motion et en légende ; le fichier est celui du commit noté, inchangé.
- **ggml.** Le code de ggml compilé avec whisper.cpp contient un extrait
  signalé en commentaire « MIT licensed. Copyright (c) 2023 Jeffrey Quesnelle
  and Bowen Peng. » (`ggml/src/ggml-cpu/ggml-cpu.c` et
  `ggml/src/ggml-metal/ggml-metal.metal`).
- **Les archives.** whisper.cpp, OpenColorIO, Yoga et zstd sont téléchargés
  à la première configuration sous forme d'archive du commit noté, dont CMake
  vérifie l'empreinte SHA-256 (`URL_HASH` dans `CMakeLists.txt`) avant de
  s'en servir. De zstd, seuls les fichiers du décodeur et de l'encodeur
  sans fils d'exécution sont compilés (`cmake/CutmachineZstd.cmake`) ;
  l'encodeur est entré le 24 octobre 2026 pour l'archive des projets
  (DEC44), de la même archive, sans changer ni le commit ni la licence.
- **Le format des étalonnages de Resolve.** `src/media/ResolveGrade.cc` lit les
  identifiants de paramètres décrits dans les notes de
  [davinci-resolve-mcp](https://github.com/samuelgursky/davinci-resolve-mcp)
  (MIT, commit `a07f594`, `resolve-advanced/vendor/drx-parameters`), comme
  une donnée : une quarantaine de nombres et le nom de ce qu'ils règlent.
  Aucun code n'en est repris ; le décodeur a été écrit d'après la
  description du format et la lecture de cinq bases copiées
  (GRADE-RESOLVE-2026-10, GR15 et DEC15). Le texte de sa licence n'est pas
  joint : le dépôt n'en garde pas de copie, et ce qui est repris est une
  table de faits, sans texte ni code.
- **HyperFrames.** `tools/motion-web-import` ouvre les pages du registre de
  [HyperFrames](https://github.com/heygen-com/hyperframes) (HeyGen), sous
  Apache-2.0 d'après ce que le dépôt en a relevé au commit `8bca7c4`, pour
  en mesurer les mouvements ; `page.mjs` y monte une composition comme le
  fait son runtime. Rien du registre n'est versionné ici, l'outil n'est lié
  à aucune cible, et les trois pièces de `tools/motion-web-import/generic/`
  sont écrites à partir des mouvements comptés. Le texte de la licence
  Apache-2.0 n'est pas joint : le dépôt n'en a pas de copie, et il n'a pas
  été relu sur le dépôt d'origine.
- **Les bibliothèques construites par OpenColorIO.** OpenColorIO 2.5.2 les
  clone par git pendant la compilation, au tag noté dans le tableau
  (`OCIO_INSTALL_EXT_PACKAGES ALL`), et clone sse2neon pendant la
  configuration, au commit noté. Aucune empreinte ne vérifie leur contenu.
- **Hors du build.** Les modèles Whisper, l'environnement de Montreal Forced
  Aligner (MFA 3.4.3, licence MIT, avec Kaldi, Apache 2.0, depuis
  conda-forge) et ses modèles `french_mfa`, `english_mfa` et `spanish_mfa`
  (CC BY 4.0), DaVinci Resolve et After Effects, que CUTMACHINE peut
  appeler, restent sur la machine de l'utilisateur : ni le dépôt ni le
  bundle ne les contiennent.
- **Le dépôt.** Le relevé n'a trouvé aucun code tiers recopié dans `src/`,
  `scripts/`, `sidecar/`, `tools/` ou `.claude/skills/`, en dehors de ce
  que ce tableau nomme : le code porté de FilmCraft et les règles adaptées
  du skill Remotion.

## Tenir ce fichier à jour

Une dépendance déclarée par `FetchContent_Declare` dans `CMakeLists.txt` est
épinglée par `URL` et `URL_HASH`, a sa ligne ici avec le commit de son
archive, et sa licence copiée dans le bundle par `cutmachine_bundle_license` ;
chaque fichier copié dans `Resources/licenses/` est nommé dans la dernière
colonne. `tests/architecture_tests.py` échoue sinon, et aussi quand un commit
change dans `CMakeLists.txt` sans changer ici. Ce test ne connaît les
bibliothèques qu'OpenColorIO construit que par `CMakeLists.txt` : leur liste
se relit quand OpenColorIO change de version.
