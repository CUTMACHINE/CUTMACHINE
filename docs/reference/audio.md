# Audio

> Relu contre le code le 3 octobre 2026. Les gestes à la souris et ce qu'on
> entend ne sont pas vérifiés par lecture ; les couleurs sont celles des
> jetons de `src/UiTheme.h`.

Les flux audio des sources sont décodés par FFmpeg, puis
convertis en PCM 16 bits stéréo 48 kHz par `libswresample`. Un `AVAudioSourceNode`
mixte en temps réel uniquement les clips placés sur des pistes audio. Les clips
vidéo sont toujours muets : aucun fallback de lecture ou d'export ne récupère
leur audio. Un clip audio peut porter un gain exact en dB et des fondus
d'entrée/sortie exacts :
`SetClipAudioOperation` remplace atomiquement `gain_db`, `fade_in` et
`fade_out`. Les deux durées sont des `RationalTime`; la même enveloppe est
appliquée par la lecture et par l'export FFmpeg. Le schéma document v7 les
sérialise pour chaque clip. Les documents v4 à v6 restent lisibles : ils
reçoivent sans ambiguïté 0 dB et deux fondus nuls, puis sont réémis en v7.
Le callback audio lit un plan immuable construit depuis la
timeline et ne touche jamais directement au document éditable. `Espace` et
`J/K/L` pilotent simultanément image et son, y compris la navette accélérée
avant/arrière à 2× et 4× (échantillonnage accéléré, sans correction de pitch) ;
un seek, un trim, un move, une
fermeture de trou ou un undo reconstruit le plan de mixage au raccord exact.
Les échantillons additionnés sont limités dans `[-1, 1]` pour éviter un
dépassement numérique lors du mixage multipiste.

À l'ouverture d'un ancien projet, tout clip vidéo dont le drapeau de migration
indique encore du son embarqué est séparé. L'application crée au besoin des
pistes audio et émet une
`DetachAudioOperation` par clip dans l'event log : le rectangle audio reçoit
son propre ULID mais conserve exactement les mêmes `source_in`, durée et
`timeline_in`. Tous les nouveaux montages créent directement cette paire avec
`include_audio:false` sur la vidéo ; le son n'est donc jamais embarqué ni joué
deux fois. Audio et image peuvent ensuite être déplacés, trimés,
coupés ou supprimés indépendamment. Cette normalisation est persistée et reste
annulable avec `Cmd+Z`. La fenêtre n'a ni bouton ni raccourci pour séparer un
clip à la main : l'outil MCP `detach_audio` reste disponible pour migrer
manuellement un ancien clip.

Les rectangles vidéo utilisent une palette grise, tandis que les rectangles
audio utilisent une palette verte, même lorsqu'ils proviennent du même média.
Chaque paire issue d'une séparation partage un `link_group_id` stable et une
référence de phase rationnelle exacte. Avec **Sélection liée** active, cliquer,
englober au lasso ou éditer un membre agit sur la paire ; le drag affiche les
deux aperçus et émet une seule opération atomique :
`MoveLinkedClipsOperation` ou `TrimLinkedClipsOperation` ; la suppression de
la paire est une `ClearClipsOperation`. Avec le
toggle désactivé, chaque rectangle reste indépendant. `Cmd+Shift+L` change le
mode et `Option` le suspend pour un geste. Un déplacement audio indépendant
affiche sur le rectangle un badge signé en images (`+3f`) ou, pour un décalage
sub-frame, en échantillons (`-240smp`). Le retour à zéro est magnétique. Les
anciens projets déjà séparés sont migrés par une `SetClipLinkOperation`
atomique lorsque les temps source et timeline correspondent exactement.

Le déplacement manuel du playhead produit un scrub audio : chaque clic, drag
dans la règle ou dans un trou, et chaque pas au clavier déclenche un grain de
60 ms à la position demandée. Une enveloppe de 5 ms à l'entrée et à la sortie
évite les clics ; un nouveau mouvement remplace immédiatement le grain en cours
sans démarrer la lecture continue.
En mode Image, les événements souris restent dédupliqués après quantification :
tant que le curseur demeure dans la même frame, le grain n'est joué qu'une
seule fois. Il n'est réarmé qu'en entrant dans une autre frame, ce qui évite les
redémarrages très bruyants causés par les micro-mouvements sub-frame.
En mode Image, les flèches déplacent le playhead d'une image (`Shift` : dix) ;
en mode Échantillon, elles le déplacent d'un échantillon (`Shift` : dix). Le
bandeau inférieur rappelle en permanence la grille active.

## Mesurer le décalage entre deux sons d'une même prise

`measure_audio_offset` (MCP ; CLI `--measure-audio-offset`, SYNCHRO-2026-10
SY1) dit de combien le son d'un rush est décalé de celui d'un autre : le
fichier d'un enregistreur contre le son de la caméra, ou deux caméras sur la
même prise. Il ne modifie rien. `offset` est la position du premier
échantillon de `other_id` sur le temps de `reference_id`, exacte à
48 000 échantillons par seconde (négative quand l'autre a démarré avant) ;
`offset_frames` le redit en images de la référence, arrondi à la plus
proche, pour la lecture seulement. La réponse donne aussi le recouvrement des
deux sons, une confiance (`significance` : le pic au-dessus du hasard, en
écarts ; `peak_ratio` : contre le meilleur pic ailleurs) et la dérive des
deux horloges, mesurée sur trois fenêtres (tête, milieu, queue). Chaque son
se lit sur le canal demandé (`mix`, `left`, `right`) : un enregistreur porte
souvent la cravate d'un côté et la perche de l'autre.

La mesure se fait en deux passes : une grossière, sur les attaques du son
(là où il monte, ce qu'une réverbération ne déplace pas) et sur tous les
décalages à la fois ; une fine, à l'échantillon, sur le son lui-même, pondéré
par la phase (GCC-PHAT), qui garde l'arrivée du son direct nette dans une
pièce qui résonne. Elle ne devine jamais : `NoOverlap` (moins de 2 s en
commun), `WeakCorrelation` (pic à moins de 10 écarts du hasard : pas la même
prise, ou trop peu), `AmbiguousPeak` (un autre décalage convient presque
autant : un son tenu, une boucle, un tic), `ClockDrift` (plus d'une
demi-image de dérive sur le recouvrement, à la cadence de l'image de la
référence : un décalage unique serait faux en bout de prise ; aucune
correction de vitesse n'est faite, décision du 8 octobre 2026).

Vérifié le 8 octobre sur des signaux de synthèse (`audio_offset_tests` :
exact à l'échantillon malgré le gain, le bruit, un filtre et une pièce ; les
refus) et sur deux fichiers : une voix de synthèse posée 1 s plus tôt dans
un WAV d'enregistreur que dans une caméra MP4 (AAC) au son réverbéré, mesurée
à −48 000 échantillons sans filtre et à −47 997 avec un passe-bas à 3,5 kHz,
dont c'est le retard propre. Les seuils (10 écarts, rapport 1,5) restent à
éprouver sur de vrais tournages : les six interviews de PRJC (le
critère 2 de SY1) attendent le disque où sont leurs fichiers `Mix`.

### Poser le son séparé sous l'image (SY2)

`sync_audio` (MCP ; CLI `--sync-audio`) pose le son d'un enregistreur
(`media_id`) sous un plan image (`clip_id`), sur une piste audio
(`track_id`), en une seule annulation. Le décalage est mesuré entre le rush
du plan et l'enregistreur comme le mesure `measure_audio_offset` (canaux
`picture_channel`, `recorder_channel`), ou repris de sa réponse (`offset`) ;
un refus de la mesure est le refus de l'outil, sous le même nom. Le son
couvre la part du plan que l'enregistreur contient, sur les images de la
timeline : un enregistreur lancé ou arrêté pendant le plan est rogné vers
l'intérieur jusqu'à l'image suivante, et son point d'entrée garde
l'échantillon mesuré. Il est lié au groupe du plan image avec sa phase
exacte : déplacer, rogner ou couper l'image l'emporte. Le son de la caméra
reste, lié, à sa place, et sa piste est coupée (`mute_camera_sound`, vrai
par défaut ; décision du 8 octobre 2026). Le lot ne recouvre jamais ce que
la piste contient déjà (`Overlap`). La réponse nomme le plan posé
(`placed`) et, quand il a mesuré, la mesure (`measure`). Rien de nouveau dans
le document : un collage, une liaison, une sortie de piste
(`SeparateSound.h`).

Avec `sound_clip_id` à la place de `media_id` et `track_id` (CLI
`--sync-audio-clip`, 19 octobre 2026), le son de l'enregistreur est un plan
déjà sur la timeline, comme l'Auto Align Clips de Resolve le prend. Il glisse
sur sa piste jusqu'à être calé sous le plan image. Il garde son identifiant,
son gain et ses fondus, et de sa propre plage ce qui tombe sous le plan, sur
les images de la timeline, sans jamais lire plus de son média qu'avant. Il
est lié de la même façon, la piste caméra coupée, en une annulation. Le lot
ne recouvre jamais un autre plan de la piste (`Overlap`). Un son déjà calé
et lié répond `unchanged`. Refusés avant toute mesure : le son du plan
lui-même, un son lié à un autre plan, une vitesse, une piste verrouillée
(`CheckSeparateSoundPair`).

Dans la fenêtre, on sélectionne le plan image et le son de l'enregistreur
(Maj-clic), puis clic droit ou menu Clip : « Aligner automatiquement les
plans › Selon la forme d'onde ». Un clic droit sur un plan sélectionné garde
la sélection. La mesure tourne en tâche de fond, et la barre des tâches la
montre. Un refus se dit dans la barre d'état, en français, et rien n'est
posé. Le geste n'a pas encore été fait à la main.

Essayé de bout en bout le 8 octobre, sur la voix de synthèse : posé à
47 997/48 000 s de l'enregistreur sous un plan de 20 s, puis le film exporté
(piste caméra coupée, seul le son posé audible) mesuré contre le rush de la
caméra par `measure_audio_offset` : décalage 0, dérive 0 ; `verify_export`
le dit exact, aucune image divergente.

### Les coupes au mot emportent le son séparé (SY3)

Un son d'enregistreur lié au plan image (même groupe de liaison, autre
source) est coupé avec lui par `remove_words`, `remove_source_ranges`,
`clean_disfluencies` et `tighten_pauses` (et leurs commandes au CLI) : aux
mêmes instants, c'est-à-dire aux plages du plan image décalées de la
différence exacte des phases des deux plans (`timeline_in − source_in`).
Le son de la caméra, de même source, perd comme avant les mêmes images
source. L'opération garde les plages traduites de chaque son séparé
(`translated_links`), écrites seulement quand il y en a un : un journal
d'avant, ou une coupe sans son séparé, garde ses octets. Les réponses les
nomment sous `also_cut`. Un son séparé qui ne couvre pas toutes les plages
n'est pas coupé, comme un membre de même source. Un son séparé sur une piste
verrouillée refuse la coupe plutôt que de décrocher. Les rognages de bord
(`ripple_trim`, `trim_boundary_air`, `close_junction_air`) prenaient déjà le
membre le plus proche de chaque piste sans regarder sa source : le son séparé
y perd la même tête ou la même queue, et garde sa phase.

`create_interview_short` lit les liens de la timeline où les spans ont été
lus (14 octobre 2026). Quand le plan qui tient un segment est lié au son
d'un enregistreur, le film le garde : le son de l'enregistreur sur une
troisième piste, aux mêmes instants (la plage du segment décalée de la
différence exacte des phases), dans le groupe de liaison du segment et ancré
sur son image, si bien que `timeline_stats` n'y mesure aucune dérive ; le son
de la caméra reste, lié, sur sa piste coupée, comme `sync_audio` le laisse.
Un segment lu sur le son de l'enregistreur devient la plage de l'image, et
le son de l'enregistreur est gardé à côté. L'opération écrit la piste et,
pour chaque segment, la source et l'entrée du son séparé, seulement quand il
y en a un : un journal d'avant garde ses octets. Refusés plutôt que
devinés : un segment que l'enregistreur ne couvre pas en entier
(`SourceOutOfBounds`), deux enregistreurs liés au même plan, un plan avec
une vitesse.


## Le traitement du son du moteur

> FC5, FILMCRAFT-2026-10, 17 octobre 2026. Porté de FilmCraft
> (`crates/audio-dsp`, MIT ou Apache-2.0, commit `09aaf9b`).

Le moteur a sa propre bibliothèque de traitement du son (`src/AudioDsp*.h`),
portable, sans dépendance : un égaliseur paramétrique de six bandes
(`parametric_eq` : cloche, plateaux grave et aigu, passe-haut, passe-bas,
sections RBJ du *Audio EQ Cookbook*), un passe-haut de Butterworth à 12 ou
24 dB par octave (`high_pass`), un compresseur (`compressor` : seuil,
rapport, genou, attaque, relâche, gain de compensation), un de-esser
(`de_esser` : un plateau aigu dynamique), un limiteur de crête vraie à
anticipation (`limiter`), un débruitage spectral (`denoise`) et un
peigne de réjecteurs du secteur à 50 ou 60 Hz et de ses harmoniques
(`de_hum`). **Rien n'y est branché** : ni l'export ni la lecture ne s'en
servent. Le mixeur unique que la lecture et l'export partageraient
(MOTION-MARQUE-2026-10) se décide avec l'auteur ; cette bibliothèque est ce
sur quoi il se construira.

Ce que chaque effet garantit, et que `audio_dsp_tests` vérifie :

- il travaille en place sur des blocs flottants planaires, de toute
  longueur, et sa sortie ne dépend pas du découpage en blocs, au bit près
  (blocs de 1, 64, 1 000 échantillons, de taille variable, vides compris,
  un réglage changé en cours) ;
- il n'alloue rien en traitant, en recevant un réglage ou en se remettant
  à zéro ;
- il déclare son retard (`Latency()`, en échantillons), et une impulsion
  sort là où il le dit : 271 échantillons pour le limiteur à 48 kHz (5 ms
  d'anticipation et les deux demi-longueurs du filtre de crête vraie),
  2 048 pour le débruitage (une trame), 0 pour les autres ;
- à son réglage neutre (toutes les bandes à 0 dB, rapport 1, réduction
  nulle, plafond à 0 dBTP sur un son qui ne l'atteint pas, quantité nulle,
  passe-haut coupé), il rend le son tel quel, retardé de son retard, au bit
  près ; le débruitage à 0 dB à 1e-6 près, la transformée et son inverse ;
- chaque réglage a un identifiant stable, une plage, une unité et une
  valeur par défaut, et rejoint une nouvelle valeur par une rampe de 20 ms,
  échantillon par échantillon ;
- un état récursif sous 1e-30 est remis à zéro : un long silence après un
  son ne laisse aucun nombre dénormal.

Mesuré le 17 octobre sous Linux : la réponse de chaque section, mesurée sur
un sinus, suit la formule à moins de 5e-7 dB, et les filtres `equalizer`,
`highpass`, `lowpass`, `lowshelf`, `highshelf` et `bandreject` de FFmpeg
(les mêmes sections, en double ; FFmpeg 6.1.1 d'Ubuntu) donnent, sur une
seconde de bruit, les mêmes échantillons flottants pour six d'entre eux et
7e-12 d'écart au plus pour le réjecteur ; la tolérance du test est 1e-6
(`audio_dsp_ffmpeg_tests`, sauté et dit sauté sans FFmpeg) ; le limiteur,
poussé de 12 dB, ne laisse aucune crête vraie au-dessus de son plafond
selon le mètre de `Loudness.h`, dont il partage le filtre de
suréchantillonnage (et `NormalizeLoudness` garde ses sorties au bit près) ;
le débruitage descend le bruit seul de 15,7 dB et porte le rapport signal
sur bruit d'un sinus de 25,2 à 39,5 dB ; le de-hum descend 50 Hz de 60 dB et
laisse 1 kHz à 0,001 dB près. Rien n'a été écouté.

## Proposer un ducking sous une voix

`propose_ducking` (MCP ; CLI `--propose-ducking`, FC6, FILMCRAFT-2026-10)
lit le son d'un rush de voix (`media_id`, sur le canal `channel` : `mix`,
`left` ou `right`) et dit où la voix parle, et quelle courbe de gain une
musique posée dessous devrait suivre. **Il ne modifie rien** : le gain
animé d'un plan n'existe pas encore (REEL-2026-09, R2), la courbe est une
proposition, pour l'agent ou pour qui mixe. Porté de FilmCraft
(`crates/audio-dsp/src/ducking.rs`).

Le son est décodé à 48 kHz et lu par trames de 10 ms (leur niveau RMS). Le
plancher de bruit est un centile bas des niveaux (`floor_percentile`, 10 par
défaut), trames de silence numérique (sous −100 dBFS) exclues : le seuil se
place au-dessus du bruit de la pièce, pas à un niveau absolu, comme les
creux d'`analyze_timeline_audio`. La parole commence quand le niveau passe
`threshold_db` au-dessus du plancher (10 dB) et s'arrête quand il retombe
`hysteresis_db` sous ce seuil (4 dB) : le creux d'un mot ne coupe pas la
plage. Les trous plus courts que `bridge_ms` (300 ms) sont refermés, puis les
plages plus courtes que `minimum_speech_ms` (200 ms) écartées : une toux,
une porte.

La courbe (`gain_curve`) est à 0 dB hors de la voix, descend de `depth_db`
(12 dB) à partir de `attack_ms` (150 ms) avant chaque plage jusqu'à son
début, tient jusqu'à sa fin, remonte en `release_ms` (400 ms). Entre deux
points, la courbe est une droite en dB, et chaque point vaut 0 ou
−`depth_db`. Deux plages dont les rampes se rejoindraient restent baissées
entre elles. Une rampe qui commencerait avant le son commence avec lui ; une
qui finirait après lui finit avec lui : la rampe est alors plus courte.

Chaque temps est exact : un échantillon à 48 kHz, en `{value, rate}`, dans
le temps du rush. Une durée qui ne tombe pas sur un échantillon entier est
refusée (`InvalidInput`), jamais arrondie ; à 48 kHz, chaque milliseconde
entière y tombe. La réponse donne les niveaux lus (`levels` : plancher,
voix au 95ᵉ centile, seuils d'entrée et de sortie, en dBFS au centième), les
réglages appliqués, les plages (`speech`), la courbe et `applied: false`.
Un rush muet répond `silent: true`, sans plage. Comme `measure_audio_offset`,
l'outil décode tout le son : il tourne en tâche de fond.

Vérifié le 17 octobre sur des voix de synthèse (`audio_dsp_ducking_tests` :
plages et courbe à l'échantillon, trou refermé, toux écartée, creux tenu par
l'hystérésis, rampes fusionnées et coupées aux bords, refus). Pas encore sur
une vraie voix, ni écouté sous une musique.

## Rendre un stem sur les coupes

`render_sound` (MCP ; CLI `--call-tool <projet> render_sound`, SN7,
SON-2026-10) rend une ditty (le format de
[Dittytoy](https://dittytoy.net), avec `edit`, `at()` et `sleepUntil()` :
voir [`tools/cutmachine-sound`](../../tools/cutmachine-sound/README.md)) sur
les coupes de la timeline telle qu'elle est en mémoire, sans sauvegarde
préalable. Avec `partition`, `ditty` est un jeu d'instruments et la
partition dit ce qui joue et quand. Le stem est un WAV 24 bits dont le
timecode BWF est le début de la timeline (`01:00:00:00`, ou
`start_timecode`) ; sans `out`, il va dans `Sound/<nom>-seed<n>.wav` du
paquet, et `stems: true` y ajoute un WAV par boucle.

Le rendu est le JavaScript de `tools/cutmachine-sound`, joué par
`cutmachine-sound-host` dans JavaScriptCore : un processus à part, posé à
côté du moteur, seul porteur du droit `allow-jit`, arrêté après `timeout`
secondes (`SoundTimeout`). La réponse est le rapport de `cm-sound render`
(`out`, `sha256`, pics, `loops` et leurs `triggers` avec leurs `notes` si
demandés, `edit`), et le fichier est celui que la CLI Node écrit, à l'octet
(vérifié par `sound_project_tests`). Une ditty qui lève une erreur, une partition qui
nomme une coupe absente sont refusées avec les mots du rendu
(`SoundRenderFailed`). Sans hôte, sous Linux, `SoundHostMissing`. Ensuite :
`ingest_media` du stem, puis `place_sound_stem`.

Une partition peut s'écrire depuis un motif [Strudel](https://strudel.cc) :
`cm-sound strudel` (CLI Node seulement, SN9) en fait les notes, au temps
exact de chaque cycle et, avec le montage, jusqu'à sa fin ;
`instruments/strudel.js` répond aux noms de ses sons.

## Poser un stem rendu à son timecode

`place_sound_stem` (MCP ; CLI `--call-tool <projet> place_sound_stem`, SN4,
SON-2026-10) pose sur la timeline un stem que
`tools/cutmachine-sound` a rendu depuis ce projet (`render --project`),
là où son timecode BWF le dit, en une seule annulation. Le stem est d'abord
ingéré (`ingest_media`) ; l'outil prend son `media_id`, jamais un temps.

Le rendu écrit dans le chunk `bext` l'échantillon de la première image de
la timeline, au timecode de début qu'utilise l'export EDL (`01:00:00:00`,
`start_timecode` sinon), par une seule règle : la frame `f` tombe sur
l'échantillon `round(f × den × fréquence / num)`, un demi arrondi vers le
haut. L'outil inverse cette règle : la frame dont l'échantillon est le
timecode du stem, ou un refus nommé quand aucune ne l'a (un stem rendu pour
une autre cadence, un autre début ou une autre timeline n'est jamais
rapproché de la frame voisine). Un stem qui commencerait avant la première
image est refusé, comme un fichier dont la longueur n'est plus celle de la
médiathèque, un média qui a une image ou un WAV sans `bext`.

Le plan couvre les images entières que le stem recouvre, depuis la
première : la fin du stem dans sa dernière image (une queue de réverbération,
une fréquence qui ne divise pas la cadence) n'est pas posée, et
`placed.not_laid` dit combien. Sans `track_id`, une piste audio est ajoutée
au-dessus de toutes les autres ; avec, le plan y est collé sans jamais
recouvrir ce qu'elle contient (`Overlap`). La réponse nomme le plan, la
piste, la première image et leur nombre (`placed`). Rien de nouveau dans le
document : un ajout de piste et un collage, en un lot (`SoundStem.h`).

Vérifié par `sound_stem_tests` (en-tête BWF lu avant ou après les données,
inversion exacte à 25, 23,976 et 29,97 drop-frame, refus, annuler et refaire
à l'octet) et `mcp_tools_tests` ; sur un vrai projet à 23,976 i/s, un stem
à 44,1 kHz posé sur ses 234 images, puis retiré à l'octet par `undo`. Rien
n'a été écouté, ni vérifié dans Resolve.

