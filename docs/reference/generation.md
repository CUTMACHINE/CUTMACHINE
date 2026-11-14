# Génération payante (fal.ai)

> Écrit le 21 octobre 2026 avec le code (GENERATIF-2026-10).
> Prouvé sous Linux contre un faux fal.ai ; jamais appelé contre le vrai
> service. La moitié macOS (`GenerationTransport.mm`) compile depuis le
> 21 octobre, sans avoir été exécutée.

Le moteur peut demander à un service payant de fabriquer un média, avec la
clé de la personne. Sept usages : l'extension d'une image à un autre format
(`expand_image`, modèle `fal-ai/bria/expand`), et depuis le 11 novembre 2026
[six usages créatifs](#les-usages-créatifs). Chaque génération est facturée
sur le compte fal.ai de la personne. Aucun service central : le moteur parle
directement à fal.ai.

La voix off, elle, se génère sur la machine, sans service et sans dépense
(GN6, DEC47) : voir [la voix off, en local](#la-voix-off-en-local).

## La règle

**On ne génère que si la personne l'a demandé explicitement.** Un agent peut
proposer une génération (`propose_generation`, gratuit) ; il ne la lance
(`generate_media`) qu'après le oui de la personne, coût compris. Sans
demande, il propose et attend. Il ne met jamais `confirmed_by_person: true`
à sa place.

## Ce que la génération ne fait pas

Tranché par l'auteur le 23 octobre 2026 (DEC2 de
GENERATIF-2026-10), et écrit dans la
description de `generate_media`, de `propose_generation` et dans les skills
`monter-un-reel` et `monter-une-itw` :

- **rien qui fasse dire ou faire à une personne filmée ce qu'elle n'a ni
  dit ni fait, sans son accord** : ni voix, ni visage, ni corps. Amendé par
  l'auteur le 11 novembre 2026 (DEC51,
  PARITE-2026-11) : le doublage, la
  synchronisation des lèvres et un avatar tiré d'une image que le moteur
  n'a pas générée agissent sur une personne filmée, et ne partent qu'avec
  `consent_of_filmed_person: true`. Confirmé par l'auteur le 12 novembre
  (« Je confirme DEC51 »). Avant le 11, la règle n'avait pas d'exception ;
- **l'extension d'image (`expand_image`) seulement sur un plan sans
  personne** ; le 9:16 d'une interview passe par le cadrage
  (`set_clip_framing`, `track_subject_framing`) ;
- **une musique générée seulement d'un modèle dont la licence couvre la
  publicité payante**, ce qui n'est vérifié ni pour Stable Audio ni pour
  Lyria (aucun modèle de musique n'est branché aujourd'hui).

Le moteur ne vérifie pas ces règles lui-même : il ne sait pas encore dire
de façon fiable si un plan montre une personne. C'est une consigne dite à
l'agent, et l'invite de permission de `generate_media` est l'endroit où la
personne voit ce qui partirait.

## Les deux outils

**`propose_generation`** ne dépense rien. Arguments : `kind`
(`"expand_image"`), `media_id`, `aspect_ratio` (`"9:16"`), `source_time`
(l'image d'un rush, en temps du média, exact), et en option `prompt`,
`negative_prompt` (en anglais), `seed`, `frames` (longueur du plan produit,
125 par défaut). Elle rend :

- `proposal_id`, valable trente minutes dans cette session, une seule fois ;
- `request` : ce qui partirait chez fal.ai (`canvas_size`,
  `original_image_size`, `original_image_location` calculés par le moteur,
  le prompt, la graine), l'image source exceptée ;
- `announced_cost` (`null` : le moteur ne connaît aucun prix) et
  `cost_note`, qui renvoie à la page du modèle ;
- `source`, `clip`, `verified` (ce qui a été vérifié de l'API et ce qui ne
  l'a pas été), `ready.key` et `ready.transport` ;
- `next` : montrer la proposition à la personne et attendre.

Le canevas est le plus petit au format demandé, côtés pairs, qui contient
l'image à sa taille, centrée ; refusé s'il dépasse 5000×5000 pixels ou si
l'image en couvrirait moins de 15 %, les deux bornes du modèle.

**`generate_media`** dépense. Arguments : `proposal_id`,
`confirmed_by_person: true`, et `timeline_id` comme tout outil d'édition (la
provenance entre dans l'historique de cette timeline). Il envoie l'image
source sur le stockage de fal.ai (expiration demandée : une heure), lance le
modèle, télécharge l'image, la garde dans le paquet
(`generated/<proposal_id>.png`) et en tire un plan
(`generated/<proposal_id>.mp4`, H.264, BT.709), qu'il ajoute à la
bibliothèque par l'ingest ordinaire, puis il pose sa provenance. Le plan
tenait lieu d'image fixe, que la bibliothèque ne prenait pas ; elle la prend
depuis le 24 octobre 2026 (REEL-2026-09 R4), mais la génération n'a pas
changé et pose toujours ce plan.

Refus, **sans rien dépenser**, et la proposition reste valable :
`ConfirmationRequired` (pas de `confirmed_by_person: true`),
`UnsupportedOperation` (pas de transport : la version sans interface, sous
Linux, ne génère rien ; ou pas de projet ouvert), `GenerationKeyMissing`
(pas de clé). Refus sans rien dépenser, la proposition close :
`ProposalUnavailable` (inconnue dans cette session, expirée ou déjà servie).
Après l'envoi, `GenerationFailed` ou `IoError` disent si fal.ai a reçu la
demande, et qu'elle peut être facturée.

## Les usages créatifs

> PA12, PARITE-2026-11, 11 novembre 2026.
> Requêtes et réponses lues dans la documentation publique de chaque modèle
> ce jour-là ; prouvé contre un faux fal.ai. `stock_image`, `avatar` et
> `dubbing` ont été appelés pour de vrai le 13 novembre 2026
> (ce qui est revenu) ;
> `stock_video` et `lipsync` jamais.

| `kind` | Ce qu'il fait | Ce qu'il lit | Modèle | Prix annoncé le 11 novembre 2026 |
|---|---|---|---|---|
| `stock_video` | un plan muet fabriqué d'après un texte | `prompt` ; `duration_seconds` (3 à 15, 5 par défaut) ; `aspect_ratio` (`16:9`, `9:16`, `1:1`, celui de la timeline par défaut) | `fal-ai/kling-video/v3/pro/text-to-video` | 0,112 $ par seconde |
| `stock_image` | une image à la taille de la timeline | `prompt` ; `seed` | `fal-ai/flux-2-pro` | 0,03 $ le premier mégapixel, 0,015 $ les suivants |
| `avatar` | un présentateur de synthèse qui dit un son | `media_id` (une image, ou une image d'un rush à `source_time`) ; `audio_media_id` et sa plage | `fal-ai/kling-video/ai-avatar/v2/standard` | 0,0562 $ par seconde |
| `lipsync` | les lèvres d'une personne filmée recalées sur un autre son | `media_id` et sa plage ; `audio_media_id` et sa plage | `fal-ai/sync-lipsync/v2` | 3 $ par minute |
| `dubbing` | les voix d'un film dans une autre langue, lèvres inchangées | `media_id` et sa plage ; `target_language` (deux lettres) | `fal-ai/elevenlabs/dubbing` | 0,6 $ par minute entamée |
| `edit_video` | un plan retouché par un texte, son mouvement gardé : une personne ou une chose remplacée, un lieu ou une allure changés | `media_id` et sa plage (3 à 30 s) ; `prompt` (obligatoire) ; `reference_media_ids` (0 à 4 images fixes) ; `resolution` (`720p`, `1080p` par défaut) | `kling/v4/video-to-video/edit` | 0,094 $ la seconde en 720p, 0,141 $ en 1080p, sur l'entrée plus la sortie (lu le 13 novembre 2026) |

Une plage est `source_time` et `duration` (`audio_source_time` et
`audio_duration` pour le son) : le média entier quand on ne dit rien,
300 secondes au plus. Le moteur découpe l'extrait lui-même (H.264 et AAC,
1920 au plus sur le grand côté ; WAV pour un son seul) et l'envoie sur le
stockage de fal.ai ; un extrait de plus de 90 Mo est refusé avant toute
dépense. L'image part telle que le fichier l'encode : un rush en log reste
en log, et ce que le modèle en fait n'a pas été regardé.

**Ce qui revient n'a pas toujours la taille demandée** (relevé le
13 novembre 2026) : `stock_image` a rendu 1072×1920 pour 1080×1920,
`avatar` un film de 720×1280 à 30 images par seconde, avec des gestes des
mains ; `dubbing` garde la taille, la cadence et la durée du rush, rend le
son en mono, et double toutes les voix qu'on y entend, hors champ compris.
Le prix facturé ne revient pas dans la réponse.

**`edit_video`** (PA14, 13 novembre 2026 ; appelé pour de vrai ce
jour-là). Le texte dit ce qui change et ce qui reste ; il nomme le plan
`@Video1` et les images `@Image1`, `@Image2`… dans l'ordre. Sans image, le
texte suffit.

- **Les bornes du modèle sont dites avant tout envoi** : une plage de 3 à
  30 secondes, aucun côté sous 700 pixels (après la réduction à 1920 du
  grand côté), 4 images fixes au plus (le modèle ne publie pas sa propre
  borne ; quatre est celle de ses voisins).
- **Le prix est calculé dans la proposition** (`estimated_cost`) : le tarif
  fois la durée d'entrée plus celle de sortie, en secondes entières.
  5 secondes en 1080p : environ 1,41 $.
- **Un plan HLG ou log part converti en Rec.709.** Le modèle répond
  « Failed to load video » à une vidéo marquée HLG (mesuré, deux fois) et
  peint en Rec.709 quoi qu'on lui donne. Le moteur envoie donc l'image
  Rec.709 du plan, par la transformation que `read_frame` montre sous
  gestion couleur, que le projet gère la couleur ou non. Les autres usages
  envoient toujours l'image telle que le fichier l'encode.
- **Ce qui revient** (un essai) : la taille du plan (1080×1920), en
  Rec.709, à **24 images par seconde** pour un plan à 25, avec une piste
  son. Ce qui n'est pas nommé dans le texte est resté en place sur cet
  essai ; rien dans la documentation du modèle ne le promet.
- **L'accord de la personne filmée** est requis dès que le plan ou une
  seule image de référence n'a pas été généré par le moteur.

Un extrait vidéo, pour tout usage, est une image et un son, rien d'autre :
la piste de timecode du rush et ses métadonnées ne partent pas.

Un argument d'un autre usage est refusé par son nom, plutôt qu'ignoré une
fois l'argent dépensé. Ce que l'usage a fabriqué entre dans la bibliothèque
tel que le service le rend (`generated/<proposal_id>.mp4` ou `.png`), avec
sa provenance.

**L'accord de la personne filmée.** `lipsync` et `dubbing` agissent toujours
sur une personne filmée ; un `avatar` aussi, sauf quand son image est un
média que le moteur a généré. La proposition le dit (`needs_consent`), et
`generate_media` refuse alors (`ConsentRequired`, sans rien dépenser, la
proposition restant valable) tant que l'appel ne porte pas
`consent_of_filmed_person: true`. Le moteur ne peut pas vérifier un accord :
comme `confirmed_by_person`, c'est une affirmation de l'appelant, que seule
la personne qui signe le film peut faire. L'accord n'est pas écrit dans le
document : la provenance n'a pas de champ pour lui.

**Pas de correction du regard.** fal.ai ne propose aucun modèle qui le
fasse (cherché le 11 novembre 2026 ; `fal-ai/eye-correct` répond 404).

## La provenance

Un média généré porte `generation` dans le document (`describe` le rend) :
`service`, `model`, `kind`, `prompt`, `seed` (celle que le service a
rendue), `created_at` (UTC), `announced_cost`, `request_id`,
`source_media_id`, et pour une voix fabriquée par le moteur
`reference_text`, les mots qu'elle dit. Elle est posée par
`SetMediaGenerationOperation`, réversible comme toute opération.

Depuis le 24 octobre 2026 (DEC36 de
GENERATIF-2026-10) :

- **une provenance ne s'efface que par Annuler** : une opération qui la
  viderait ou la remplacerait par une autre est refusée (`InvalidOperation`,
  « only undo removes it »), par quelque surface qu'elle arrive, `--apply-op`
  compris ; reposer la même provenance ne change rien et passe ; l'inverse
  d'un ingest (`RemoveIngestedMedia`, DEC32) appliqué comme une édition, qui
  retirerait le média et sa provenance, est refusé de même ;
- **chaque plan d'un média généré porte le badge `GÉNÉRÉ`** dans la
  timeline, après la marque de l'agent et avant la vitesse ;
- **`check_timeline` liste ces plans sous `generated`**, toujours présent,
  vide quand rien n'est généré : le compte rendu de l'agent en tire une
  ligne par plan. Aucune mention n'est imposée à la livraison.

Pas de code de confirmation dans la fenêtre : l'invite de permission de
`generate_media` est le verrou. Un plafond de dépense par projet viendra en
préférence locale, une fois les prix relevés (GN5). Un document qui en porte une déclare la
version 20 : un binaire plus ancien le refuse au lieu de perdre la marque.
L'image et le plan sont dans le paquet : le projet se rouvre et s'exporte
sans fal.ai.

## La voix off, en local

> Écrit le 8 novembre 2026 avec le code (GN6b de
> GENERATIF-2026-10). Prouvé contre un
> faux script, puis avec les vrais modèles du banc sur un projet jetable.
> L'installation sous `build/voice` est GN6c.

Qwen3-TTS (Apache 2.0) tourne sur la machine, lancé par le moteur comme MFA
(`scripts/voice_over.py`, voir `--check-setup`, élément `voice_over`). Rien
ne part sur un serveur, rien ne se paie.

**`design_voice`** fabrique une voix. Arguments : `description` (la voix, en
anglais : genre, âge, timbre, débit, style), `language` (code ISO 639-1,
`fr` par défaut, l'une des dix langues du modèle), `seed` facultative. Le
modèle VoiceDesign dit une phrase de référence fixée par le moteur dans la
langue choisie ; le WAV va dans `Sound/voix-<id>.wav` du paquet, entre dans
la bibliothèque et reçoit sa provenance : `service: "local"`,
`kind: "voice_design"`, `model` le dépôt et la révision des poids,
`prompt` la description, `reference_text` la phrase dite. Deux appels avec
la même description donnent deux voix différentes : on garde celle que la
personne retient.

**`generate_voice_over`** fait dire un texte par cette voix. Arguments :
`voice_media_id`, `text`, `language` (celle du texte, `fr` par défaut ; elle
peut différer de celle de la voix), `seed`. Le modèle Base clone la voix à
partir de son WAV et de ses mots, ce qui la garde d'une prise à l'autre ; le
WAV va dans `Sound/voix-off-<id>.wav`, marqué `kind: "voice_over"`,
`source_media_id` la voix, `prompt` le texte. Le texte part tel quel : les
chiffres, les dates et les heures s'écrivent comme ils doivent se dire quand
c'est important. Le plan se pose ensuite par `insert_clip`.

**Le moteur ne clone que ses propres voix** (DEC49) :
`generate_voice_over` refuse (`NotAnEngineVoice`) tout média qui n'est pas
une voix fabriquée par `design_voice` — un enregistrement, un rush, une
interview, une voix off. Le refus tombe avant que le modèle ne tourne. Une
provenance ne s'écrit que par une génération et ne s'efface que par Annuler
(DEC36) : elle ne se fabrique pas par les outils.

**Pas de proposition ni d'invite de permission** (DEC48) : rien ne se
dépense, et les deux outils n'entrent pas dans la liste de ceux qui vont à
la personne. Mais on ne génère une voix que si la personne l'a demandé ; les
descriptions le disent. Chaque appel bloque pendant que le modèle tourne
(environ une minute pour une voix, deux fois la durée du texte pour une voix
off) : une tâche de fond ne peut ni ingérer ni marquer un média, donc ni
l'un ni l'autre ne tourne en fond. Le calcul ne se voit pas dans la barre
des tâches de la fenêtre et ne s'y annule pas, même quand l'appel passe par
l'app.

Un document qui porte une voix et ses mots déclare la version 25 : un
binaire plus ancien le refuse au lieu de perdre les mots, sans lesquels la
voix ne se clone plus. Les WAV sont dans le paquet : le projet se rouvre et
s'exporte sans le modèle. Un plan de montage (`prepare_edit_plan`) ne peut
pas prendre `design_voice`, `generate_voice_over` ni `generate_media` comme
étape : ils ingèrent ce qu'ils fabriquent, ce que la copie du plan ne sait
pas faire.

## La clé

Lue dans le trousseau de macOS, puis dans `FAL_KEY`. Jamais écrite dans un
fichier suivi, un journal, une trace ou une réponse ; une erreur de fal.ai
qui la contiendrait sort masquée. Pour l'enregistrer une fois :

```sh
security add-generic-password -s com.cutmachine.generation -a fal-api-key -w
```

La clé ne part que vers `queue.fal.run` et `rest.fal.ai`, jamais vers
l'adresse de dépôt de l'image ni vers celle du résultat.

## Qui empêche un agent de dépenser seul

Le moteur ne distingue pas le oui de la personne de celui de l'agent : un
agent qui écrit `confirmed_by_person: true` sans avoir demandé a menti. Ce
qui l'arrête est le client qui montre l'appel à la personne :

- **Claude Code, par MCP** : la permission de l'outil
  `mcp__cutmachine__generate_media`, côté utilisateur, fait foi. Elle doit
  être demandée à chaque appel : ne mets pas cet outil dans une règle
  `allow`, et pas de `bypassPermissions` pour une session qui peut générer.
  Les annotations MCP de l'outil (`External`) disent qu'il n'est pas en
  lecture seule et touche un service extérieur.
- **Le panneau Agent, par un agent de code (ACP)** : `generate_media` n'est
  plus approuvé d'office comme les autres outils du moteur ; il va à la
  personne, sans l'option « toujours ». Sans personne pour répondre, il est
  refusé. Mais un adaptateur réglé pour tout permettre ne demande rien au
  panneau.
- **La boucle intégrée du panneau** (protocole Anthropic ou Ollama) ne
  montre aucun appel avant de le lancer : elle refuse `generate_media`
  (`ConfirmationRequired`).

Le CLI (`--call-tool`) n'a pas de session : une proposition n'y survit pas
à l'appel, et `generate_media` y refuse toute proposition.
