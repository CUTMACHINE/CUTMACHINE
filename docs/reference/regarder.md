# Regarder un plan

> Relu contre le code le 3 octobre 2026. Le constat qui ouvre la section et
> ce que montrent les images ne sont pas vérifiés par lecture.

Mesurer ne suffit pas, et le manque s'est constaté sur un vrai montage : un
plan noté `Sharp` / `Steady` — donc parfaitement propre — était inutilisable
en coupe parce qu'il montrait quelqu'un **en train de parler**. Le spectateur
voyait une bouche articuler autre chose que ce qu'il entendait. Aucun seuil
sur la variance du laplacien n'attrape ça ; un monteur l'attrape d'un coup
d'œil.

`read_frame` (MCP) renvoie donc l'image elle-même, en JPEG, à côté de sa
réponse texte. Soit `clip_id` avec une `position` (`Start`, `Middle`, `End`
— jamais un timecode, le moteur résout l'image), soit `media_id` avec un
`source_in` exact pour inspecter un rush qui n'est pas encore monté. Un rush
absent de son chemin est refusé `MediaMissing`, avec le chemin cherché. En
mode strict, `read_frame` sur un `media_id` n'a pas besoin de `timeline_id`.

Le partage du travail reste le même que partout ailleurs : ce qui est
**prouvable** est décidé par le code ([`ShotQuality.h`](../../src/ShotQuality.h)),
ce qui demande un regard reçoit une image
([`FrameCapture.h`](../../src/FrameCapture.h)). Rien dans `FrameCapture` ne décide,
il rend.

Côté transport, l'image traverse MCP comme un bloc `image` à côté du bloc
`text`, et le chat intégré la fait suivre au modèle dans le `tool_result`.
Elle n'est transmise qu'aux fournisseurs dont l'API accepte une image dans un
résultat d'outil ; ailleurs le texte passe et l'image est écartée, plutôt que
remplacée par une description que le modèle prendrait pour une observation.

# Regarder le montage en planches

Une image isolée ne permet pas de juger le rythme ou la continuité d'un
montage. `--contact-sheet` (MCP : `contact_sheet`) rend donc les milieux des
plans réellement visibles, après occultation par les pistes vidéo supérieures.
`--cut-sheet` (MCP : `cut_sheet`) place côte à côte, pour chaque raccord
visible retenu, la dernière image avant la coupe et la première image après.

Les deux commandes produisent un JPEG et un JSON qui relie chaque cellule à
son `clip_id`, son `source_id`, sa position timeline et son `source_in`, tous
exprimés en `RationalTime`. Elles passent par la vue SDR du pipeline couleur du
document : un rush log est présenté comme dans le moniteur, pas comme une image
délavée tirée directement du fichier. Aucune donnée du projet n'est modifiée.

Par défaut, une cellule montre la source du plan, sans son étalonnage ni le
look de séquence. Avec `--graded` (MCP : `graded: true`), chaque cellule est
le plan tel que `read_frame` avec `graded: true` le montre : étalonné, look
compris, dans la vue SDR de Record, calculé par le moteur par la même chaîne
(DEC29, 24 octobre 2026). Le JSON porte alors `"graded":true`. Une planche
étalonnée décode une image par cellule, là où la planche ordinaire les lit
toutes dans un seul appel à FFmpeg : elle est plus lente. `media_sheet` ne la
prend pas : un rush n'a pas d'étalonnage tant qu'un plan ne le pose pas.

Par défaut, une planche contient au plus 24 images. Si le montage en offre
davantage, le moteur échantillonne uniformément du début à la fin ; une planche
de raccords ne casse jamais une paire. `--max-images` ajuste la borne en CLI,
et `max_images` l'ajuste en MCP (1 à 64 pour `contact_sheet`, 2 à 64 pour
`cut_sheet`).

Une timeline sans plan visible n'a rien à montrer en planche de contact, ni
une timeline sans raccord visible (un seul plan, des plans séparés par des
trous, un titre posé sur un plan) en planche de raccords : la commande et
l'outil refusent en `ValidationFailed`, par une phrase qui commence par
`nothing to show:` et dit pourquoi, et n'écrivent rien (PRD39).

Une cellule a la forme de la séquence (320 × 180 en 16:9, 180 × 320 en 9:16) :
pas de bandeau noir, et un rush d'une autre forme y est ajusté comme dans le
moniteur.

Pour le **dérush**, avant tout montage, `--media-sheet` (MCP : `media_sheet`)
regarde des rushes plutôt qu'une timeline : un seul rush donne `max_images`
images réparties sur la prise, plusieurs rushes donnent l'image du milieu de
chacun ; 24 au plus par défaut, comme les autres planches, par la commande
comme par l'outil. Les cellules prennent la forme du premier rush, et chaque
cellule donne son `source_id` et son `source_in`, et son `shot_id` : la
description de plan qui la couvre, ou `null` si personne ne l'a encore
décrite (PLANS-2026-10, ci-dessous).

# Décrire les plans une fois, les chercher ensuite

> Écrit le 21 octobre 2026 (PLANS-2026-10, PL1 à PL4). Prouvé par les
> tests sous Linux ; pas encore employé sur un vrai projet.

La parole des rushes est indexée (mots datés, blocs) ; l'image l'est par
des descriptions de plan, écrites une fois par qui a regardé — un modèle ou
une personne — et gardées dans le projet. Une description porte sur une
plage d'un rush, en temps exact (`source_in`, `duration`, dans le temps du
rush, comme un plan de la timeline), et dit :

| Champ | Valeurs |
|---|---|
| `size` (fermé) | `extreme_wide` (très large), `wide` (large, ensemble), `full` (plan moyen, en pied), `medium` (américain, taille), `medium_close` (rapproché poitrine), `close` (gros plan, serré), `extreme_close` (très gros plan, très serré), `insert` (objet, détail) |
| `movement` (fermé) | `static`, `handheld` (épaule), `pan`, `tilt`, `zoom_in`, `zoom_out`, `push_in` et `pull_out` (travelling avant, arrière), `tracking` (travelling, suivi), `crane` (grue), `aerial` (drone) |
| `quality` (fermé) | `good` par défaut ; `usable` et `poor` exigent une `reserve` qui dit pourquoi |
| `subjects`, `keywords` (libres) | 8 sujets de 80 octets, 16 mots-clés de 40 au plus, sans doublon à la casse et aux accents près |
| `action`, `place`, `reserve` (libres) | 300, 120 et 200 octets au plus |
| `author_kind`, `author`, `described_at` | `model` ou `person`, son nom, l'heure UTC `AAAA-MM-JJTHH:MM:SSZ` |

Les plages d'un rush ne se chevauchent pas, restent dans le rush, et un rush
sans image n'en a pas. Tout refus nomme le champ.

**Écrire** : `describe_shots` (MCP, ou `--call-tool`) prend `media_id`,
`shots` (une entrée par plage ; `shot_id` pour remplacer une description
existante), `remove_shot_ids`, `author` (obligatoire avec `shots`),
`author_kind` (`model` par défaut) et `described_at` (maintenant par
défaut). Une plage qui chevauche une description qu'on ne remplace ni ne
retire est refusée, en nommant les deux. Un appel est une opération,
`SetMediaShots` (la liste entière du rush, l'inverse est la précédente),
et un seul `undo`. En ligne de commande, `--apply-op` la prend telle que le
journal l'écrit.

**Chercher** : `search_shots` filtre sur les champs fermés (`sizes`,
`movements`, `qualities`, `author_kind`, `min_duration`, `media_ids` ;
n'importe laquelle des valeurs d'un filtre, tous les filtres à la fois) et
sur des mots (`text`) : chacun doit paraître dans les sujets, l'action, le
lieu, les mots-clés ou la réserve, sans égard à la casse ni aux accents
français, comme sous-chaîne (« velo » trouve « vélos »). La réponse suit
l'ordre de la médiathèque puis du temps, jamais un classement ; `total`
compte tout, `limit` (50 par défaut, 500 au plus) borne la liste. Chaque
plan donne sa plage exacte à couper et `look` : les arguments de
`read_frame` (l'image du milieu de la plage) et de `media_sheet` pour le
voir.

**Ce qui n'est pas décrit** : `describe` donne `described_shots` pour
chaque rush qui a une image (0 : à décrire), `media_sheet` le `shot_id` de
chaque cellule, `list_stable_windows` les `shot_ids` qui couvrent chaque
fenêtre et `undescribed_windows`, `search_shots` les `undescribed_media` de
sa portée.

Les descriptions vivent dans le document (version 19 dès qu'un rush en a
une) et non dans un cache à côté du média comme la transcription ou la
qualité de plan : elles ne se recalculent pas gratuitement. Un binaire
antérieur refuse donc le projet au lieu de les perdre.

# Regarder un raccord tel qu'il est livré

> Écrit le 7 octobre 2026 d'après le catalogue du binaire (IA-2026-09 IA4),
> et joué au CLI sur un projet d'essai.

Une planche montre ce que les rushes contiennent aux bornes des plans ; elle
ne montre ni le fondu, ni l'opacité, ni le cadrage animé, ni le carton, ni
le son. `inspect_cut` (MCP, ou `--call-tool`) rend une courte fenêtre autour
d'un raccord par le graphe de l'export lui-même — toutes les couches,
opacités, étalonnages, cadrages et clés, transitions, légendes, look et le
mix avec ses gains et ses fondus — puis la relit : ce qu'il montre est le
film livré à cet endroit.

Le raccord se nomme par `clip_id` (sa tête, ou sa queue avec `at_tail`) ou
par une `position` de la timeline ; `before` et `after` disent combien de
montage rendre de chaque côté (1 s par défaut, 5 s au plus, en images
entières de la timeline). La réponse donne :

- les images choisies (`images`, 2 à 12, 6 par défaut) en une bande JPEG,
  avec leurs images et temps exacts ; la dernière image avant la coupe et
  la première à la coupe y sont toujours ;
- le niveau du mix image par image (dBFS) et de chaque côté de la coupe ;
- les mots que le montage doit dire dans la fenêtre (`expected_words`) ;
  sans transcription en cache pour les pistes son, la liste est vide et
  `expected_words_unavailable` dit pourquoi ;
- des `alerts` à examiner : un mot que la coupe traverse et, avec
  `transcribe` et un modèle Whisper local configuré (refusé sinon), les mots
  attendus mais pas entendus dans une lecture neuve du mix de la fenêtre, ou
  entendus mais pas attendus. Une alerte n'est pas un verdict : une question
  faible peut être audible pour un spectateur et pas pour le modèle.

Une livraison HLG est montrée par la vue SDR, comme Record la montre. Rien
du projet n'est modifié.

La fenêtre est rendue à deux fois la taille de la plus grande image qu'on en
tire (une image de la bande ou une image d'analyse), sans jamais dépasser
celle de la séquence : le graphe compose légendes, motion, flou et cadrages
pour la taille qu'il livre, comme pour un export en 1080 d'une séquence 4K.
Avant le 6 novembre 2026, elle était rendue en pleine définition, en virgule
flottante 32 bits puis sans perte : 5,5 s par jonction pour une revue de
36 coupes sur un short 1080×1920, alors que ses images font 240 pixels.

Avec `band: true` (RACCORDS-2026-10 RC2), les images sont rendues en
**bande** : celles d'avant la coupe sur une ligne, une barre rouge, celles
depuis la coupe, et dessous la forme d'onde du mix de toute la fenêtre, la
coupe en rouge et un trait gris là où tombe chaque image.

# Visionner les raccords

> Écrit le 21 octobre 2026 (RACCORDS-2026-10 RC1, RC2). Joué sous Linux sur
> des médias synthétiques seulement ; ni sur Mac, ni sur un vrai film.

Un agent ne regarde pas le film et ne l'écoute pas. `review_cuts` (MCP ;
CLI : `--review-cuts <projet> <image.jpg> [--timeline <id>] [--frame <n>]...
[--max-bands <n>]`) le fait pour lui à chaque **jonction** : une image où,
sur une piste vidéo visible ou une piste son audible, un plan touche le
suivant. Chaque fenêtre est rendue comme celle d'`inspect_cut`, par le
graphe de l'export (1 s de chaque côté par défaut, `side` jusqu'à 5 s), puis
mesurée :

| Signal | Mesure | Alerte |
|---|---|---|
| Ambiance | 10ᵉ centile des niveaux de 10 ms du mix, de chaque côté | `ambience_jump` : ≥ 6 dB d'écart |
| Voix | 90ᵉ centile, quand il dépasse l'ambiance de 10 dB des deux côtés | `voice_jump` : ≥ 6 dB d'écart |
| Bord | les 30 ms qui touchent la coupe, contre l'ambiance de leur côté | `sound_cut_live` : ≥ 12 dB au-dessus, et ≥ −50 dBFS |
| Mots | les mots des transcriptions que la coupe traverse | `word_crosses_cut` |
| Image | dernière image avant / première après : écart moyen et histogramme, en ‰ | `jump_cut` : ≤ 80 ‰ et ≤ 150 ‰ |
| Mouvement | les deux images de chaque côté | `motion_interrupted` : ≥ 30 ‰ avant, ≤ 8 ‰ après, même scène (≤ 250 ‰) |

Un **raccord franc** (le plan suivant lit sa source là où le précédent
s'arrête, à vitesse 1) est mesuré et ne lève rien. Les alertes de son ne
valent que là où le son coupe, celles d'image là où l'image coupe. Le
rapport déclare ses seuils (`thresholds`) ; ils ne se règlent pas à l'appel.
**Une alerte est une mesure à examiner, pas un verdict** : un jump cut ou
une coupe sèche dans l'ambiance peuvent être voulus, et l'absence d'alerte
ne dit pas qu'un raccord est bon.

Par défaut, toutes les jonctions sont revues (60 au plus ; les autres sont
listées dans `not_reviewed`) ; `positions` ou `clip_ids` (têtes de plans)
en nomment certaines. La réponse joint une planche de **bandes**
(`max_bands`, 6 par défaut, 12 au plus ; `images_per_side`, 3 par défaut),
les jonctions qui ont une alerte d'abord, dans l'ordre du film ; chaque
jonction dit sa `band`. Chaque fenêtre est un rendu : compter quelques
secondes par jonction. Si le projet change pendant la revue, l'outil refuse
plutôt que de mêler deux montages. Rien du projet n'est modifié.
