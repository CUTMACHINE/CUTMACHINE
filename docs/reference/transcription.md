# Transcription verbatim et nettoyage des hésitations

> Relu contre le code le 3 octobre 2026. Les mesures citées (durées, comptes
> de mots et de rushes, banc d'alignement, montages réels) ne sont pas
> vérifiées par lecture.

## Configurer le modèle

Le chemin du modèle Whisper est un **réglage local de la machine**, jamais une
donnée de projet : un chemin enregistré dans le projet n'existerait pas sur un
autre Mac, et le même montage doit s'ouvrir partout. Il se pose une fois, dans
`~/.config/cutmachine/.env` — le même fichier qui peut porter
`ANTHROPIC_API_KEY`, la clé de repli du panneau chat :

```sh
echo 'CUTMACHINE_WHISPER_MODEL=/chemin/vers/ggml-large-v3.bin' \
  >> ~/.config/cutmachine/.env
```

La variable d'environnement l'emporte sur le fichier, donc un essai ponctuel
se fait en la préfixant à la commande, sans rien modifier.

Une fois posé, le réglage sert à l'identique `--transcribe` sans chemin
explicite, l'outil MCP `transcribe_media`, y compris quand le panneau Agent
de l'application l'appelle, et les menus de transcription de la fenêtre. Tant
qu'il n'est pas posé, les outils refusent de la même façon, en nommant la
variable et le fichier à éditer ; les menus de la fenêtre demandent un
fichier par un dialogue. **CUTMACHINE → Modèle Whisper…** et ce dialogue
écrivent le modèle choisi dans `~/.config/cutmachine/.env`, comme
`CUTMACHINE_WHISPER_MODEL` : le reste du fichier est gardé tel quel, et un
fichier neuf n'est lisible que par son propriétaire. Une variable exportée
dans l'environnement de l'application garde la priorité, et la ligne d'état
le dit.

Il n'y a qu'un réglage depuis le 24 octobre 2026 (DEC23,
LIVRAISON-2026-09-18). Avant, les
menus de la fenêtre gardaient leur propre modèle dans les préférences macOS
(`CUTMACHINEWhisperModelPath`). Au premier lancement, ce choix est recopié
dans le fichier s'il n'y désigne encore aucun modèle, puis la préférence est
effacée ; si le fichier en désigne déjà un, c'est lui qui reste.

`transcribe_media` est ce qui rend le travail sur les mots atteignable par un
agent : `list_disfluencies`, `remove_words` et le montage d'interview lisent
tous un transcript, et aucun d'eux ne pouvait le faire exister. Il faut
`verbatim: true` pour pouvoir retirer les hésitations ensuite — le décodage par
défaut les supprime silencieusement, donc elles ne sont plus là pour être
coupées.

`list_source_words` (AGENT-2026-09 A4) lit les mots d'un rush tels que
l'aligneur les a posés (`word_timing` dit qui : `mfa`, `nemo` pour un
rush aligné avant le 1er novembre 2026, ou `whisper`) et,
pour chacun, l'image où entrer et celle où sortir, lues sur l'enveloppe du
rush par les règles du skill d'interview : un début en silence prend
l'attaque moins deux images d'air, un début sonore recule jusqu'au silence,
une fin sonore avance jusqu'au décrochage sans dépasser le mot suivant, une
fin déjà silencieuse recule au dernier son plus trois images ; `entry_rule`
et `exit_rule` nomment la règle, `voiced: false` un mot que l'enveloppe
n'entend pas. `find` cherche un mot et rend ses voisins, `from_frame` et
`to_frame` bornent, `max_words` plafonne. Sans enveloppe
(`analyze_speech_onset`), les images sont celles de l'aligneur. Les trois
analyses par rush — `analyze_speech_onset`, `analyze_shot_quality`,
`analyze_hesitations` — prennent `media_ids` pour analyser tous les rushes
en un appel, une ligne par rush sous `results` (A8). Avec cette enveloppe
en cache, `remove_words` et `clean_disfluencies` coupent chaque mot de son
entrée à sa chute par les mêmes règles, et le disent (`boundaries`,
`widened_frames`) ; `trim_boundary_air` et `close_junction_air` lisent la
même ligne de voix que `tighten_pauses` ; `ripple_trim` signale sous
`repeats_source` les images qu'un rallongement fait jouer deux fois, quand
le plan voisin les joue déjà (A9 à A12). `propose_exposure` (A16) mesure
tous les plans image d'une timeline sous gestion couleur et rend, pour
chacun dont la médiane de scène s'écarte de plus de `tolerance_stops` du
gris moyen (`target_stops`), un pas `set_clip_effects` avec sa propre
exposition corrigée, à passer à `prepare_edit_plan` ou à appliquer un à un ;
rien n'est appliqué par l'outil, et `summary` dit où le montage se tient.
`propose_grade` (A20 à A22) lit chaque plan là où l'étalonneur le lit : le
plus grand visage que le moteur trouve (Vision), ou un carré blanc ou gris
nommé en `region`, ou le cadre entier ; il propose l'exposition de l'ancre
vers `target_stops` (0,7 pour un visage) et sa balance sur la ligne de
peau ou au neutre, avec la température et la teinte du plan, et arrête une
remontée où le p98 du cadre passerait le blanc diffus
(`highlight_ceiling_stops`) ; un plan sans visage suit le montage.
`list_timeline_words` (A24) projette les alignements des rushes sur chaque
plan audio du montage, sans le transcrire : les mots et leurs images, un mot
coupé à l'entrée ou à la sortie, l'air aux bornes, le son hors mots et le
texte du montage en un appel ; `list_source_words` avec `levels` et une
plage rend l'enveloppe entière de la plage (A23).
`describe` prend `only` (sections) et `kinds` (genres de piste) pour ne
rendre que ce qu'on lui demande (A25).
`tighten_pauses` et `trim_boundary_air` prennent `clip_ids`,
`close_junction_air` des `pairs` : un appel pour tous les plans, un résultat
par plan (A26).
`transcribe_media` rend le texte de chaque rush transcrit (A27).
`create_project` crée un parent absent (A28) ; une sélection de parole
refusée nomme tous les segments fautifs d'un coup (A29).
`tighten_pauses` ne protège d'un mot aligné que ses fenêtres audibles, et
ferme le creux qu'une fin d'alignement tardive tenait ouvert (A30).
`propose_grade` étalonne par rush : les plans ancrés votent, tous les plans
du rush reçoivent le vote (A31).
La coupe suggérée d'une hésitation n'entame plus un mot aligné voisin (A33).
`generate_captions` allonge ou fond un carton trop court et ferme un trou
entre deux cartons (A32) ; un carton trop court ne fond jamais par-dessus
une fin de phrase, et celui qui ouvre le film ou une phrase prend la
majuscule (REGLES-2026-10 RM2) ; `ripple_trim` entraîne par défaut les pistes
sync-lockées (A34) ; `list_disfluencies` liste les marqueurs de discours
(A35) ; `trim_boundary_air` rend `ok` et `analyze_hesitations` compte aussi
en images (A36).
`range_levels` donne aussi l'enveloppe à ses fenêtres de 20 ms (A37) ; un
contrôle de cartons sans lecture du son monté le dit (A38).
`propose_grade` rogne une boîte de visage qui dépasse et n'échoue plus sur un
plan illisible (A39) ; `transcribe_media` rend son texte même sans enveloppe
(A40).
`propose_grade` prend `apply` et `verbose` (A41) ; le dernier carton
s'arrête avec la séquence (A42) ; `insert_clip` ne déplace plus les autres
pistes, `ripple_trim` si (A34).
`list_disfluencies` rend les redémarrages, un mot dit deux fois que la
transcription a écrit une fois (A43) ; `list_timeline_words` compte les
hésitations et redémarrages que chaque plan joue encore (A44).
`cut_words` signale un mot aligné sur une image contre une coupe (A45).
Chaque bord audio sans fondu propre reçoit un fondu de 3 ms, à l'export comme
à la lecture : plus de clic à chaque raccord (A46).
Une coupe d'hésitation garde deux images d'un mot ou d'une syllabe, pas
quatre, et un redémarrage emporte le « euh » qui le précède (A47).
La coupe d'une hésitation court jusqu'à la fin de la voix qui suit la tenue,
une tenue entre deux mots compte dès 100 ms, un mot posé dans une tenue est
nommé, un plan entré ou sorti dans la voix continue le dit
(`head_in_voice`, `tail_in_voice`), et une coupe ne rentre jamais dans un
mot aligné (A48).
`tighten_pauses` signale un mot aligné que l'enveloppe n'entend pas
(`heard: false`, crête et plancher publiés) et, avec `close_unheard`, ferme
le creux par-dessus (A17) ; `list_source_words` rend sous
`voice_without_words` les plages sonores qu'aucun mot ne couvre (A18).

## Le reste

`--transcribe` accepte une langue explicite et un mode `--verbatim`. Le chemin
du modèle y reste facultatif : donné, il l'emporte ; omis, c'est le réglage
local qui s'applique.

Il accepte aussi **plusieurs `media-id` séparés par des virgules** (MCP :
`media_ids`), et ne charge alors le modèle qu'une fois. Le chargement mesure
~8 s ; sur les 43 rushes parlés d'un projet réel, c'était près de six minutes
passées à relire le même fichier, contre 11 min d'inférence utile. Les médias
dont le document dit qu'ils sont muets sont **sautés et signalés** plutôt que
transcrits — 29 des 71 rushes du même projet étaient des plans de coupe à
−74 dBFS, tous passés à Whisper à 11× le temps réel pour ne rien produire.
`--include-silent` (MCP : `include_silent`) force le passage.

Ce niveau vient de `--ingest`, qui mesure désormais le niveau audio moyen de
chaque média et l'enregistre dans le document (`audio_level`), au même titre
que la cadence et la durée. C'est un décodage audio, donc un coût réel :
mesuré à 0,3 s sur un rush 4K de 268 Mo. Une entrée de bibliothèque écrite
avant ce champ se relit « non mesurée » — jamais « silencieuse » ; un
ré-`--ingest` la remplit.

L'auto-détection de langue se trompe sur un rush long et majoritairement non
parlé (mesuré : gallois détecté sur une interview française). Nomme la langue.

Sans `--verbatim`, Whisper nettoie ce qu'il entend : sur une interview de
7 min 35 il n'a conservé que 3 tics sur les 20 réellement prononcés. Le mode
verbatim place devant chaque fenêtre de 30 s un prompt qui biaise le décodage
vers les hésitations, les répétitions et les faux départs — d'où 607 mots
transcrits au lieu de 390, et les « euh », « heu », « ben » et « bah »
présents dans le cache. Le drapeau fait partie de l'identité du transcript :
une transcription verbatim n'est jamais réutilisée à la place d'une
transcription standard, ni l'inverse.

Le mode verbatim peut aussi vider une prise : sur des rushes en anglais et en
espagnol, 2 à 6 mots pour 13 à 22 s de voix, les mots de l'équipe en tête et
en queue, la phrase sautée (ITW9). Quand l'enveloppe de parole le signale
(`likely_incomplete`), `--transcribe` et `transcribe_media` refont ces
médias sans le prompt ; la passe ordinaire devient la transcription, avec
les seules hésitations de la passe verbatim tombées dans ses silences. Le
cache reste verbatim et dit `plain_pass_words` : ces mots-là ne portent pas
d'hésitation, que seule l'analyse du signal trouve alors (voir « Les
hésitations que personne n'a transcrites »). Le détail est dans
[Commandes headless](commandes-headless.md).

Le nettoyage se fait ensuite en deux temps, et jamais par un modèle :

1. `--disfluencies` (MCP : `list_disfluencies`) liste ce qui est *prouvable* —
   les syllabes qui ne sont pas des mots français, et les mots répétés
   immédiatement — sous forme d'index de mots, avec leur texte pour relecture.
   Ce qui relève du jugement (« ce "donc" est-il un tic ? ») est listé à part,
   comme marqueur de discours (`Marker`, A35), et jamais coupé d'office.
2. `--remove-words` (MCP : `remove_words`) applique la coupe. Il ne prend que
   des **index de mots** : c'est `ResolveWordRemoval` qui calcule les images,
   jamais l'appelant. La coupe est une seule `RemoveWordsOperation`,
   réversible, qui referme la timeline.

Un modèle peut donc proposer une sélection et la justifier, sans jamais
calculer un timecode.

`clean_disfluencies` (MCP) réunit les deux étapes en une seule intention :
CUTMACHINE détecte, calcule les images et applique **une** opération
réversible pour tout le plan. Le modèle n'énumère rien. Les répétitions ne
sont retirées que si `include_repetitions` est demandé — un mot répété peut
être une insistance voulue, une syllabe d'hésitation jamais.

## Choisir des passages : spans, blocs, sélection

> Écrit le 7 octobre 2026 d'après le catalogue du binaire (IA-2026-09 IA3).

`get_timeline_transcript` (MCP) lit le transcript en cache des plans
audibles sous forme de spans, chacun nommé par un `span_id`, et rend une
`transcript_revision`. Les identifiants sont des positions dans cette
lecture : un outil qui les reçoit avec une révision plus ancienne les refuse
en `StaleSelection`. Les temps exacts sont donnés pour qu'un humain les
vérifie, jamais pour être recopiés dans un autre outil. Un span porte
`straddles_cut` quand il contient un mot coupé à un raccord,
`likely_hallucinated` quand des mots existent sans groupe de parole mesuré
(les outils de sélection le refusent sans `force: true`), et
`likely_incomplete` quand une parole soutenue a donné trop peu de mots : le
texte manquant n'y est pas un silence.

`list_speech_blocks` regroupe ces spans en blocs de parole, coupés là où une
phrase finit, où la personne s'arrête plus longtemps qu'un souffle, ou où le
rush change : c'est là qu'on choisit des idées, plutôt que dans des cartons
de sous-titres. `opens_mid_sentence` et `closes_mid_sentence` signalent un
bloc qui n'est pas une phrase entière (un drapeau faux est omis).
`inspect_speech_selection` vérifie une sélection avant de la construire, sans
rien changer : pour chaque segment, sa plage et son texte, s'il ouvre ou
ferme au milieu d'une phrase, ce que le même rush dit juste avant et juste
après, avec `kept` (la sélection le garde ailleurs) et `heard_next_to_it` (le
segment voisin le joue), et `overlaps_segments` pour des segments qui
rejouent la même matière. Le moteur rapporte ; juger du sens revient à
l'appelant.

`create_interview_short` crée une timeline à partir de spans ou de blocs
nommés avec la `transcript_revision` lue en même temps, et l'active, sauf
`activate: false` ou mode strict ; la timeline d'origine ne change pas. Le
moteur résout chaque identifiant en plage source exacte, ce qui garde une
coupe hors d'un mot.

## Recaler les mots sur le signal, et refermer les silences

`--speech-onset` met aussi en cache les groupes de parole séparés par au
moins 200 ms de creux. `--speech-onset-report` (MCP :
`list_speech_onsets`) publie pour chaque groupe ses bornes source, son niveau
moyen et sa crête en dBFS. Le champ `dominant_onset` désigne le premier groupe
tenu au moins six fenêtres de 20 ms et situé à moins de 9 dB du 90e centile du
plan. Une question faible hors micro reste donc visible dans `groups`, mais
n'est plus confondue avec l'entrée du sujet équipé d'un micro-cravate. Le
seuil séparant les groupes et leur plancher au-dessus du bruit sont
paramétrables dans les outils MCP (`group_gap_ms`, `group_floor_db`).

Les horodatages de Whisper sont excellents la plupart du temps et
occasionnellement faux d'une seconde. `--align-transcripts` (MCP :
`align_transcript`) compare chaque frontière de mot à l'enveloppe d'énergie
produite par `--speech-onset`, corrige celles qui tombent dans le silence, et
**refuse** les autres plutôt que de les déplacer au jugé — une frontière
ambiguë, sans front de parole à portée, ou qui demanderait un déplacement
au-delà du plafond est signalée et laissée où elle est. Sans `--write`, c'est
un rapport ; avec, la correction est écrite dans le cache de transcription que
lisent `remove_words`, `clean_disfluencies` et le montage d'interview. C'est
ce qui rend une coupe par mot fiable ; sans ça, la seule façon de vérifier une
borne était d'extraire un fragment et de le retranscrire.

`--tighten-pauses` (MCP : `tighten_pauses`) referme les silences **internes**
d'un plan sans lire un seul mot : il cherche les creux de l'enveloppe d'au
moins `min_gap_ms` millisecondes (320 par défaut), ramène chacun à
`keep_frames` images (4 par défaut, réparties entre les deux bords, pour que
le raccord tombe au plus profond du silence), et referme en ripple. Ces
défauts sont le réglage que la procédure d'interview passait explicitement
depuis septembre ; ils remplacent 400 ms et 6 images, qui laissaient le film
mou, depuis le 4 novembre 2026. Sous 300 ms, un creux à l'intérieur d'un mot
(une occlusive) a déjà été refermé. Une seule `RemoveWordsOperation`, donc un seul `undo`, et
la paire A/V liée est emportée avec. L'air de tête et de queue n'est pas
touché : c'est un rognage, la question de `--speech-onset`, et il est publié
dans le rapport plutôt que coupé en douce.

`--trim-boundary-air` (MCP : `trim_boundary_air`) ferme précisément ces
silences de tête et de queue. Les deux nombres optionnels sont, dans l'ordre,
les images source conservées près de la parole (3 par défaut) et la durée
minimale d'air traitée en millisecondes (300 par défaut). Les bornes viennent
des groupes de parole mesurés par `--speech-onset`, jamais des mots Whisper.
La paire A/V et les autres pistes en sync-lock suivent dans une seule
`TrimBoundaryAirOperation`, donc un seul `undo`. La paire, c'est le seul
partenaire de chaque bord rogné : sur chaque autre piste, le membre du groupe
lié dont le même bord est le plus proche, comme pour `ripple_trim`. Les autres
morceaux d'un bloc déjà découpé (par `remove_source_ranges` ou
`--tighten-pauses`, qui leur laissent un seul groupe) ne bougent pas.

À une coupe, utiliser `--close-junction-air` (MCP :
`close_junction_air`) avec le plan sortant puis le plan entrant. Le moteur
additionne l'air de queue du premier et l'air de tête du second avant de
comparer le seuil, puis rogne les deux côtés atomiquement. Ainsi deux moitiés
de 280 ms sont bien reconnues comme un creux audible de 560 ms, et une seconde
passe ne déplace plus la jonction. Les outils MCP acceptent en plus
`sync_track_ids` pour remplacer explicitement les pistes sync-lock déduites.

Quand un alignement forcé des mots est en cache, `--tighten-pauses` ne referme
jamais un creux qui recouvre un mot aligné : le creux est découpé autour du mot
et chaque morceau est jugé comme un creux à part entière. Un « à » d'une image
resté sous la ligne de parole entre deux silences courts n'est plus avalé avec
eux. Sans alignement, la fermeture est inchangée et le rapport le dit
(`words_checked: false`) ; les mots protégés sont nommés dans `kept_words`.

Toute édition qui retire du son de la timeline nomme les mots qu'elle emporte.
Le résultat de `--apply-op`, des commandes de coupe et des outils MCP
`tighten_pauses`, `trim_boundary_air`, `close_junction_air`,
`remove_source_ranges`, `ripple_trim`, `remove_words` et `clean_disfluencies`
porte `cut_words` : par source, les images retirées et les mots alignés
touchés, entiers ou entamés (`whole: false`), séparés entre `targeted` (les
mots demandés à `remove_words`, les hésitations de `clean_disfluencies`) et
`carried` (tous les autres). Une source sans alignement forcé est déclarée
`checked: false` avec la raison, plutôt que rendue comme une liste vide. La
matière retirée se lit sur les pistes son : rogner l'image seule ne retire
aucun mot entendu.

## Frontières alignées et mots non entendus

Whisper fournit le texte, un aligneur forcé fournit les frontières : c'est la
conclusion du banc d'alignement. L'aligneur
est Montreal Forced Aligner (MFA 3.4.3) depuis le 1er novembre 2026
(ALIGNEMENT-2026-11) : contre C-PROM, 70 minutes de français vérifiées à la
main, ses frontières sont à 31 ms de la référence en moyenne, celles de NeMo
Forced Aligner, qu'il remplace, à 73 ms, et celles de Whisper à près de
300 ms. Le moteur lit ce résultat
lui-même. Un alignement se dépose sous
`.cutmachine/word-alignments/<media-id>/` au format du banc (manifeste en
version 1, mots en images source `[frame_start, frame_end)`) ; l'outil
`align_words` (MCP) et `--align-words` (CLI) l'y écrivent, en lançant
`scripts/mfa_align.py` avec le Python de l'environnement de MFA sous le
verrou du projet (voir [Où MFA est cherché](lancement.md#où-mfa-est-cherché)).
Chaque rush est d'abord aligné d'un seul tenant ; un rush de plus de 90 s
que MFA laisse sans alignement est repris découpé au milieu de ses silences
(`segmented: true` dans la réponse), et un rush qu'il n'aligne toujours pas
est écarté (`not_aligned`) et garde ses mots d'avant. Les caches écrits par
NeMo restent lus.
Chaque rush est aligné dans la langue de sa transcription, avec les modèles
MFA de cette langue (`french_mfa`, `english_mfa`, `spanish_mfa` : modèle
acoustique, dictionnaire de prononciation et G2P des mots absents du
dictionnaire) : la langue
donnée à `transcribe_media` quand elle était forcée, sinon celle que whisper
a détectée (`detected_language`, gardée dans le cache depuis ITW9 et rendue
par `transcribe_media`), sinon celle que désignent les mots du texte, le
français quand rien ne tranche. Les chiffres sont écrits en toutes lettres
dans cette langue. Un rush dans une autre langue est écarté
(`unsupported_language`). Pour un tournage dans une langue que la détection
confond, transcris avec la langue forcée : c'est aussi elle qu'on aligne.
`remove_words`, `clean_disfluencies`, `list_disfluencies`,
`get_timeline_transcript`, `create_interview_short` et `--export-srt` passent
tous par le même lecteur : l'alignement le plus récent remplace les mots
Whisper tant que leurs lettres concordent. Une transcription refaite après
l'alignement retombe donc sur Whisper plutôt que de garder des frontières
périmées. Chaque réponse dit qui a placé les frontières (`word_timing` :
`whisper`, `mfa`, `nemo`…).

Un mot peut être placé dans le montage par la transcription du rush sans être
entendu dans le son monté. `transcribe_timeline` garde côte à côte une lecture
standard et une lecture verbatim du montage, tant qu'elles correspondent à son
agencement audible. Un mot qu'aucune de ces lectures ne contient à moins de
deux secondes est écarté des spans et du SRT, et listé dans `unheard_words`
avec le nombre de lectures consultées (`montage_readings`). Sans lecture, rien
n'est écarté : l'absence de preuve ne retire aucun mot.

## Les hésitations que personne n'a transcrites

Whisper, même en verbatim, n'écrit pas toutes les hésitations : sur une
interview réelle, il n'en a transcrit aucune des quatorze que le montage final
a coupées. Elles se rangent en trois familles : un « euh » détaché, une
syllabe étirée (« la maa…rée ») et un murmure entre deux mots.

`analyze_hesitations` (MCP) les cherche dans le son d'un rush, sans lire les
mots. La mesure est la **stabilité du timbre** : la distance moyenne entre
vecteurs MFCC successifs sur ±50 ms. La parole change d'enveloppe spectrale
toutes les quelques dizaines de millisecondes, une voyelle tenue presque pas.
Une portion est tenue quand sa stabilité passe sous 76 % de la médiane des
fenêtres voisées du rush. Le seuil est relatif, parce que l'échelle absolue
dépend de l'analyse, alors que le rapport s'est conservé d'une mesure à
l'autre. Une tenue de moins de 200 ms en fin de mot est la prosodie d'une
virgule et n'est pas listée ; un murmure se situe entre 22 et 36 dB sous le
niveau de parole du rush. Chaque candidat est placé contre les mots alignés,
avec sa famille, sa plage source, sa mesure et les mots voisins. Le résultat
est mis en cache dans `.cutmachine/hesitations/<media-id>.json`.

`list_disfluencies` y ajoute alors, pour le plan, un tableau `hesitations`
(`hesitation_analysis` vaut `missing` tant que le rush n'est pas analysé) :
plage en images source, mesures, mots autour et `suggested_cut`. Pour une
syllabe étirée, la coupe proposée laisse les deux premières images de la
tenue, pour que la syllabe s'entende encore (A47).
`remove_source_ranges` applique cette coupe : plages en images du rush,
`[first_frame, end_frame)`, une seule opération réversible qui emporte la
paire liée et les pistes en sync-lock.

Recoupé sur les quatre rushes d'un montage réel : 20 candidats, dont 15 dans
la portion utilisée et 11 dans ce que le monteur a finalement coupé. Les
quatre autres sont soit de fausses alertes, soit des hésitations gardées :
seule l'écoute les départage. C'est une mesure, pas une décision de coupe.

```sh
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  analyze_hesitations '{"media_id":"<media-id>"}'
./build/cutmachine --call-tool Projet.cutmachine-project/project.cutmachine.json \
  remove_source_ranges \
  '{"clip_id":"<clip-id>","ranges":[{"first_frame":1412,"end_frame":1427}]}'
```

## Qui parle : les voix d'un rush

`analyze_speakers {media_id, speakers}` dit qui parle quand dans un rush
(PARITE-2026-11 PA3). `speakers` est le nombre de voix du rush, de 1 à 8 :
tu le connais, le modèle ne le devine pas. Deviné, il était faux sur cinq
dialogues de quinze.

Un modèle d'empreinte vocale (WeSpeaker ResNet34-LM, lancé par sherpa-onnx,
tous deux sous Apache 2.0) lit le son sur la machine : rien n'en sort. Il
donne un vecteur à chaque fenêtre de 1,5 s de parole, une fenêtre toutes les
0,25 s, et les vecteurs sont rangés en autant de groupes qu'il y a de voix.
Transcris le rush d'abord : les passages de parole sont alors ses mots, et
une fenêtre ne passe jamais d'un passage au suivant (`speech_from: "words"`).
Sans transcription, les passages sont ceux où le son est fort
(`"loudness"`).

Le résultat est un cache, `.cutmachine/speakers/<media-id>.json`, pas un
état du document. Deux lectures :

- `list_speakers {media_id}` rend, par voix, sa durée et son nombre de
  tours, puis les tours dans l'ordre : la voix, les temps dans le rush, le
  nombre de mots et les douze premiers. Une liste longue se pagine
  (`max_turns`, `from_turn`, `next_from_turn`).
- `list_source_words` ajoute `speaker` à chaque mot : la voix qui parle sur
  la plus grande part du mot.

Une voix est un numéro, 0, 1, 2 dans l'ordre d'apparition, propre à son
rush : rien ne dit qui c'est, et la voix 0 d'un rush n'est pas celle d'un
autre. Tu la nommes en lisant les mots.

`similarity` dit à quel point deux voix se ressemblent, en millièmes. Des
voix bien séparées ont mesuré de 380 à 800 ; une seule voix demandée en deux
a mesuré 877, et les paires que le modèle a confondues de 850 à 900. À
partir de 850, le partage n'est pas fiable : soit c'est une seule voix
coupée en deux, soit deux voix qu'il ne distingue pas. L'outil rend le
nombre, pas un verdict.

Mesuré le 12 novembre 2026 sur quinze dialogues fabriqués avec deux voix
françaises réelles chacun, tous les tours connus
(`tools/speaker-diarization-benchmark/bench.py`) :

| | part du temps donnée à la mauvaise voix |
|---|---|
| en moyenne | 5,9 % |
| à la médiane | 4,2 % |
| la pire paire (deux voix très proches) | 25,5 % |
| trois voix à tour de rôle | 4,1 % |

Un changement de voix est trouvé à 0,25 s près, et une seconde de son est
lue en 0,06 s. La chaîne habituelle de sherpa-onnx (segmentation pyannote
3.0 puis regroupement), essayée d'abord, donnait 26 % en moyenne sur les
mêmes dialogues : elle séparait un homme d'une femme et manquait presque
toujours le passage entre deux femmes dans la même pièce.

Ce que la mesure ne dit pas : ces dialogues sont des tours collés bout à
bout, sans pause ni chevauchement, et aucun vrai rush à deux voix dont la
réponse soit connue n'a été lu. Deux personnes qui parlent en même temps ne
sont pas traitées : un passage a une voix.

Pour un multicam monté à la voix, coupe le groupe aux temps des tours
(`split_multicam_group`) et donne à chaque morceau son angle
(`set_multicam_active_angle`) : l'outil mesure, le choix de l'angle est le
tien. Où le modèle est cherché :
[lancement](lancement.md#où-la-diarisation-est-cherchée).

## Adresser la timeline par le rush

Une décision de montage se prend dans le rush (« couper juste après le nom »,
« cette prise commence à 1412 »), mais toutes les opérations prennent une
position **timeline**. La conversion est une soustraction et une addition —
c'est-à-dire exactement ce qu'on rate une fois sur dix.

`--locate-source-frame` (MCP : `locate_source_frame`) répond « où joue
l'image N de ce rush » : quel plan, quelle piste, quelle position. Une même
image peut être sur la timeline plusieurs fois — l'image et son son détaché,
ou un rush utilisé deux fois — donc toutes les correspondances sont rendues,
jamais une choisie.

Et les outils de coupe et de rognage acceptent la même adresse :
`split_clip`, `trim_clip` et `ripple_trim` prennent `source_frame` **à la
place** de `timeline_position` ou de `delta` — jamais les deux, refus explicite
sinon. Sur un rognage, `Head` veut dire « le plan démarre sur cette image » et
`Tail` « c'est la dernière image jouée » : la borne est inclusive, parce que
c'est ce qu'un numéro d'image veut dire pour un monteur, et le +1 appartient au
moteur.

Une paire A/V liée est coupée des deux côtés dans la **même** opération.
`RemoveWordsOperation` porte un champ `linked_clip_ids` : chaque clip nommé
perd exactement les mêmes images source et referme sa propre piste. Sans ça,
nettoyer le son d'une interview raccourcirait la bande et laisserait l'image,
et un seul `undo` ne suffirait plus à revenir en arrière. Les outils
`clean_disfluencies` et `remove_words` résolvent eux-mêmes les membres du
groupe de liaison qui couvrent réellement la coupe ; l'opération, elle, reste
stricte — un clip nommé explicitement qui ne contient pas les plages est
refusé (`SourceOutOfBounds`), jamais coupé à moitié.
