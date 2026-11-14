# cutmachine-sound

Rendu sonore hors-ligne et déterministe pour CUTMACHINE. Le module prend une ditty (le format de code de [Dittytoy](https://dittytoy.net)) et un montage (EDL ou JSON). Il rend un stem WAV calé au timecode, où chaque son tombe sur sa frame.

Zéro dépendance, Node ≥ 18. Seule l'écriture d'une partition depuis un motif [Strudel](#un-motif-strudel-écrit-en-partition) charge Strudel, que le build télécharge.

```sh
node cli.mjs render ditties/impacts.js --edl examples/demo.edl --fps 25 --out impacts.wav --stems stems/
node cli.mjs render ditties/impacts.js --project ~/Films/teaser.cutmachine-project --out impacts.wav
node cli.mjs edit --edl examples/demo.edl --fps 25      # le montage tel que la ditty le voit
node cli.mjs strudel motif.js --edl examples/demo.edl --fps 25   # un motif Strudel écrit en partition
npm test
```

Dans le dépôt CUTMACHINE, le module vit dans `tools/cutmachine-sound/` ; sa suite tourne sous CTest
(`cutmachine_sound_tests`, et `cutmachine_sound_project_tests` contre le moteur). Le lot qui l'a
branché est SON-2026-10.

Le moteur rend aussi sans Node : l'outil MCP `render_sound` joue ce même JavaScript dans
JavaScriptCore, par `cutmachine-sound-host` (construit avec le moteur, macOS seulement), et écrit
les mêmes octets que cette CLI. Le rendu entier y passe par `job.mjs` (montage, rendu, encodage,
rapport), que le worker Node appelle aussi ; `cmake/SoundBundle.cmake` en fait un script unique à
la construction. Sous Linux, c'est cette CLI qui rend.

## Pourquoi c'est rangé comme ça

L'agent écrit du son comme il écrit une coupe : du texte, relisible et versionnable. Tout le calcul de temps (timecode, drop-frame, durées de plans, centre des fondus) est fait par le module et livré tout prêt dans `edit`. L'agent ne fait jamais d'arithmétique de timecode : il choisit quel son va où.

## Ce que le module ajoute à Dittytoy

| | |
|---|---|
| `edit` | le montage, en lecture seule (voir plus bas) |
| `at(quand, fn)` | joue ce que `fn` déclenche à un instant absolu, dans n'importe quel ordre, y compris avant 0 s (pré-roll) |
| `sleepUntil(quand)` | avance la boucle jusqu'à un instant absolu |
| `Math.random()` | hasard seedé : un flux par boucle, par voix et par filtre ; même ditty + montage + graine + réglages = mêmes échantillons, et même fichier à l'octet près |
| boucle sans `sleep` | tourne une seule fois (sur Dittytoy, elle bloquerait) |

`quand` est un nombre de secondes, ou une coupe, un plan, ou `{ frame, offset }` du montage : `at(c, …)` place le son sur la frame exacte de la coupe `c`, convertie en échantillons une seule fois, en entiers (`time.mjs`). Des secondes passent, elles, par un arrondi au µs puis un second en échantillons, qui peut tomber à côté : à 23,976 i/s et 44,1 kHz, la frame 40 tombe sur l'échantillon 73 573,5, arrondi à 73 574 depuis la frame, à 73 573 depuis 1,668333 s. `offset` est une durée en secondes, choisie, pas un instant calculé : `at({ frame: c.frame, offset: -0.4 }, …)` démarre un élan 0,4 s avant la coupe.

Une ditty qui n'utilise ni `edit`, ni `at`, ni `sleepUntil` tourne aussi sur dittytoy.net. `ditties/brume.js` en est un exemple, rendu avec `--duration 120`.

### L'objet `edit`

Les temps sont en secondes depuis le début du montage, les `frame` en frames depuis le début.

- `edit.duration`, `edit.frames`, `edit.fps` (`{num, den}`), `edit.frameRate`, `edit.dropFrame`, `edit.start` (`{frame, timecode}`)
- `edit.shots[]` : `index`, `id`, `start`, `end`, `duration`, `frame`, `frames`, `endFrame`, `timecode`, `name`, `reel`
- `edit.cuts[]` : `index`, `id`, `time`, `frame`, `timecode`, `type` (`cut`, `dissolve`, `wipe`, `key`, `dip`), `transition` (durée en s), `center` et `end` (milieu et fin de la transition), `centerFrame` et `endFrame` (les mêmes en frames, le milieu pouvant tomber sur une demi-frame), `before` et `after` (durée des plans de part et d'autre), `gap` (noir avant le plan), `from` et `to` (noms des plans)
- `edit.timeline` (lu dans un projet seulement) : `id`, `name`, `rule` (`composite` ou `track`), et `track`, `trackNumber` quand une piste a été nommée ; `edit.titles`, les clips de motion

Un plan porte l'`id` de son clip quand le montage vient d'un projet (un ULID), `shot-<n>` par position sinon (une EDL n'a pas d'identités). Une coupe porte les `id` de ses deux plans, `<avant>><après>` : elle nomme cette jonction et aucune autre.

### Le montage lu dans un projet

`--project` lit un projet CUTMACHINE (le dossier `.cutmachine-project` ou son `project.cutmachine.json`, ou un document seul), sa timeline active ou celle de `--timeline <id>`. Les règles sont celles du moteur, et la suite les compare à l'EDL qu'il exporte de la même timeline : mêmes coupes, et même fichier WAV à l'octet.

- Seules les pistes vidéo font des plans ; une piste masquée et un angle multicam inactif n'en font aucun.
- Un clip se lit en frames entières de la timeline, exactement : un clip qui n'y tombe pas est refusé, jamais arrondi.
- Une transition est placée comme le moteur la place : centrée sur `n` frames, elle commence `⌊n/2⌋` frames avant la coupe. Comme dans l'EDL, le plan entrant commence et le plan sortant finit au début de la transition. Un fondu au noir en fin de piste est un fondu enchaîné vers un plan noir (bobine `BL`) ; un fondu depuis le noir en tête n'est pas une coupe, et `edit.warnings` le dit.
- La timeline commence à `01:00:00:00`, comme l'export EDL du moteur (le document n'a pas de timecode de début) ; `--start` le change. 29,97 et 59,94 comptent en drop-frame.
- **Plusieurs pistes vidéo : l'image composée**, par la règle de `timeline_stats` (décidé par l'auteur le 22 octobre 2026) : à chaque instant, le plan de la piste visible la plus haute est celui qu'on voit, et un clip de motion (un titre) n'est ni un plan ni une coupe. Un plan de coupe en V2 fait donc deux coupes, un titre en V2 aucune. Un plan V1 interrompu par un plan de coupe est vu deux fois : son `id`, puis `<id>~2`. Une transition qu'une piste plus haute recouvre ne fait pas de coupe (`warnings` le dit). `--track <id>` lit les clips d'une seule piste à la place, clips de motion compris.
- **Les titres** sont dans `edit.titles` : chaque clip de motion des pistes visibles, avec `frame`, `endFrame`, `frames`, `start`, `end`, `duration`, `name` (sa composition) et `trackNumber`. Une ditty peut y poser un son, `at({ frame: t.frame }, …)`.

## Partition et instruments

Une ditty mêle les notes et la synthèse. Pour garder les unes et changer l'autre, on les sépare :

- **la partition**, en JSON, dit ce qui joue et quand : des pistes, chacune nommant un instrument, qui portent des événements ancrés au montage (par l'`id` d'une coupe ou d'un plan, jamais par un temps écrit à la main) et des notes sur une grille de tempo, avec des paramètres libres ;
- **les instruments**, en code, au format ditty, disent comment ça sonne : `instrument(nom, synth, { play, chain, note })`. N'importe quel synthé fait un instrument ; la liste n'est pas fermée.

```sh
node cli.mjs render instruments/impacts.js --partition partitions/impacts-demo.json --edl examples/demo.edl --fps 25
node cli.mjs render instruments/verre.js   --partition partitions/impacts-demo.json --edl examples/demo.edl --fps 25 --triggers
node partitions/impacts.mjs --project Film.cutmachine-project > impacts.json   # les choix d'impacts.js, pour un autre montage
```

`ditties/impacts.js` est découpé ainsi : `partitions/impacts-demo.json` (ses choix sur `demo.edl`) et `instruments/impacts.js` (ses synthés) rendent le même fichier, à l'octet ; `instruments/verre.js` joue la même partition avec d'autres sons, et chaque déclenchement tombe sur le même échantillon.

```json
{ "format": "cutmachine-partition", "version": 1,
  "tempo": { "bpm": 96, "at": { "cut": "A>B" } },
  "tracks": [
    { "name": "impacts", "instrument": "impact",
      "events": [ { "at": { "cut": "A>B" }, "params": { "poids": 0.8 } },
                  { "at": { "shot": "C", "point": "end" }, "lead": 0.4, "length": 1.2 } ] },
    { "name": "basse", "instrument": "basse",
      "notes": [ { "beat": 0, "note": "c2", "beats": 1.5 }, { "beat": "4/3", "note": "eb2", "velocity": 0.7 } ] } ] }
```

- **Ancre** : `{ "cut": id, "point": "start" | "center" | "end" }` (le début, le milieu et la fin de la transition), `{ "shot": id, "point": … }`, ou `{ "edit": "start" | "end" }`, chacune avec un `offset` en secondes si besoin.
- **Événement** : `at` (une ancre), `lead` (il commence autant de secondes avant), `length` (en secondes, `"transition"`, `"shot"`, ou une ancre où il finit), `note`, `params`.
- **Note** : `beat` (depuis l'origine de la grille, `"1/3"` pour un triolet), `note` (MIDI ou nom, `c4` = 60 ; sans elle, la note de l'instrument, comme pour un événement : une frappe de batterie n'a pas de hauteur), `beats`, `velocity`, `params`. La grille (`tempo`) est celle de la partition ou de la piste ; son origine est une ancre.
- Secondes et temps se donnent en nombres ou en textes `"p/q"` ; tout est sommé exactement, puis arrondi en échantillons une seule fois. `comment` est permis partout ; toute autre clé inconnue est refusée.
- Un `id` que le montage n'a plus est refusé par son nom : une partition écrite pour une autre version du film ne retombe jamais sur la coupe qui a pris la place.

Côté instruments, `play(params, ev)` reçoit les paramètres libres de l'événement (ceux de la piste, puis les siens) et ce que la partition en dit : `ev.start`, `ev.anchor`, `ev.lead`, `ev.length` en secondes, `ev.cut` ou `ev.shot`, `ev.beat`, `ev.velocity`, `ev.index`, `ev.track`. Il rend les options de `play()` ; sans `duration`, la longueur de l'événement la donne, sans `amp`, la vélocité. `chain()` rend les filtres par où passe une piste de cet instrument (un filtre `.create()` par piste, ou un filtre partagé). Chaque piste de la partition devient une boucle, et donc un stem (`--stems`).

## Un motif Strudel écrit en partition

[Strudel](https://strudel.cc) écrit la musique en motifs : `s("bd*2 [~ sd]")`, `note("<c2 eb2>").s("sawtooth")`. `cm-sound strudel` interroge le moteur de motifs de Strudel et écrit chaque événement comme une note de partition ; les instruments d'un jeu la jouent. Strudel dit ce qui joue et quand, jamais comment ça sonne (SN9).

```sh
node cli.mjs strudel motif.js --cycles 8 --out motif.json
node cli.mjs strudel motif.js --project Film.cutmachine-project --at '{"cut":"A>B"}'   # de la coupe à la fin du film
node cli.mjs render instruments/strudel.js --partition motif.json --project Film.cutmachine-project
```

- **Le temps** : un cycle de Strudel vaut quatre temps de la partition (`setcpm(100/4)` donne 100 bpm). Il est exact (fraction.js) : un triolet s'écrit `"4/3"`, et le rendu l'arrondit en échantillons une seule fois. Sans `setcpm` ni `setcps`, le tempo de strudel.cc (120 bpm). Avec un montage, la partition va de l'origine (`--at`, une ancre ; le début par défaut) à la fin, exactement ; sinon `--cycles`.
- **Les pistes** : une par son (`s`), ou par label quand le motif en a un (`basse: note(…)`). Une note sans son joue `triangle`, comme dans Strudel.
- **Les notes** : la hauteur suit la règle des synthés de Strudel (`freq`, puis `note`, nom ou numéro MIDI, octave 3 par défaut, puis `octave`) ; la longueur est celle de l'événement, fois `clip` ; `velocity` est celle de la partition ; tous les autres contrôles (`gain`, `cutoff`, `n`, `room`…) passent dans `params`, et le rapport les compte.
- **Le code** se lit comme sur strudel.cc : des motifs labellisés en début de ligne (`$:`, `nom:`, `_$:` pour taire), les lignes du dessus exécutées avant ; sans label, la dernière instruction. Deux écarts : toute chaîne est de la mini-notation (strudel.cc ne lit ainsi que les guillemets doubles), et seuls le cœur et la mini-notation de Strudel sont chargés (pas `scale`, `voicing`, ni les banques d'échantillons ; `samples()` est ignoré).
- **Le hasard** (`degradeBy`, `sometimes`) est une fonction du cycle : un motif écrit toujours la même partition.

[`instruments/strudel.js`](instruments/strudel.js) répond aux noms de Strudel : ses oscillateurs (`sine`, `triangle`, `square`, `sawtooth` et leurs alias) et une batterie de synthèse (`bd`, `sd`, `hh`, `oh`, `cp`, `rim`), avec `gain`, l'enveloppe, `cutoff`, `resonance` et `pan`. Mêmes notes et mêmes instants que strudel.cc, pas le même son : ses batteries sont des échantillons. Un son que le jeu ne connaît pas est refusé par son nom ; un autre jeu peut répondre à d'autres noms.

Strudel (AGPL-3.0-or-later, comme CUTMACHINE) n'est chargé que pour écrire une partition. Le build le télécharge (`cmake -S . -B build`, sous `build/_deps/strudel-src`) ; `--strudel <dossier>` ou `CUTMACHINE_STRUDEL` en donnent un autre. Sans lui, `npm test` saute les tests de SN9.

## CLI

`render <ditty.js>` puis :

- **Montage** : `--edl <fichier> --fps <cadence>` (25, 24, 23.976, 29.97, 24000/1001…), `--start <TC>` facultatif, ou `--edit <fichier.json>`, ou `--project <projet>` avec `--timeline <id>`, `--track <id>` et `--start <TC>` facultatifs, ou `--duration <s>` pour une ditty sans montage
- `--out <fichier.wav>` : WAV 24 bits 48 kHz, chunk BWF avec le timecode de départ du montage
- `--stems <dossier>` : un WAV par boucle, avant le bus de sortie partagé
- `--partition <fichier.json>` : la ditty est alors un jeu d'instruments, et la partition dit quoi jouer quand
- `--seed <n>`, `--set nom=valeur` (répétable, règle les `input.*`), `--sr`, `--tail <s>` (queue de réverb après la fin), `--timeout <s>` (300 par défaut), `--triggers` (les échantillons de départ de chaque boucle et leurs notes, dans le rapport)

`strudel <motif.js>` puis `--cycles <n>` ou un montage (les mêmes options, `--at <ancre JSON>` pour l'origine de la grille), `--title`, `--out <partition.json>` (`<motif>.partition.json` par défaut), `--strudel <dossier>` : écrit la partition, et imprime un rapport (cycles, tempo, pistes, contrôles comptés, messages).

La sortie est un rapport JSON sur stdout : pics, RMS, hash SHA-256 de l'audio, échantillons écrêtés, messages de la ditty, et pour un montage `timeReference` (l'échantillon du début, écrit dans le BWF) et `edit` (coupes, plans, avertissements, timeline lue). Les erreurs sortent en JSON sur stderr, code 1. Le sidecar Python peut appeler la CLI en sous-processus et lire le rapport.

Format JSON simple pour `--edit`, avec `in` et `out` en frames depuis le début :

```json
{ "fps": "25", "start": "01:00:00:00",
  "shots": [ { "in": 0, "out": 100, "name": "A" },
             { "in": 100, "out": 212, "name": "B", "transition": { "type": "dissolve", "frames": 25 } } ] }
```

## Limites connues

- `node:vm` n'est pas une frontière de sécurité. Le worker et le timeout protègent des boucles infinies, pas d'un code hostile.
- Enveloppe `adsr` : attaque et décroissance vont toujours au bout, même si `duration` est plus courte. Sur Dittytoy, la durée exacte des notes courtes peut différer légèrement. `adsr2`, `segenv`, `env.def` et `voice.options.x = …` ne sont pas encore pris en charge ; le module le signale au lieu de se taire.
- Toutes les boucles partagent le même contexte. Sur Dittytoy, chaque boucle a ses propres copies des variables globales.
- Une seule couche vidéo est attendue dans l'EDL ; les chevauchements sont signalés dans `edit.warnings`. Dans un projet, plusieurs couches donnent l'image composée. Un plan de coupe réduit ou semi-transparent compte comme une coupe, et un carton plein écran en motion n'est pas un plan : c'est la règle de `timeline_stats`.
- Placement par timecode BWF : vérifié dans DaVinci Resolve 21.1 le 22 octobre 2026. Resolve lit comme « Start TC » le timecode de début écrit dans le chunk `bext`, à 25 et à 23,976 i/s, à 48 et à 44,1 kHz (01:00:00:00, 01:00:10:12, 00:59:58:07).
- Vitesse, mesurée le 22 octobre 2026 sur un Apple M5 : `impacts.js` 12 fois le temps réel, `brume.js` 4,8 fois (rendu échantillon par échantillon en JS). La moitié à sept huitièmes de ce temps est celle de `node:vm` ; le même runtime dans JavaScriptCore, avec le JIT, va de 18 à 57 fois le temps réel (SON-2026-10, SN6, et `jsc/`).
