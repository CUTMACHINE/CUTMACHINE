# Agent intégré et clé utilisateur

> Relu contre le code le 3 octobre 2026 ; « Le projet du panneau », le 19
> octobre. Ce que répond un modèle et ce que montre le panneau ne sont pas
> vérifiés par lecture.

Le panneau **Agent** contacte directement soit l’API Messages d’Anthropic, soit
l’API locale native d’Ollama, soit un agent de code lancé par sa commande
(**Agent de code (ACP)** : Claude Code, Codex…, qui se connecte avec son propre
compte). Le bouton **Configurer le moteur
IA…**, placé sous la conversation, permet de choisir le fournisseur, le modèle,
l’URL de base, la clé API et la commande ACP. Le même dialogue est disponible
dans **CUTMACHINE → Moteur IA…**.

Depuis le 1er novembre 2026 (ONGLETS-2026-10, ON3), le panneau est une
fenêtre à lui, qui flotte au-dessus de l'éditeur : il n'est plus l'onglet du
dock de droite où il prenait la place de l'Inspecteur, sur tous les onglets.
On le montre et on le cache depuis n'importe quel onglet, par le bouton
**Agent** en bas à droite de la fenêtre, par **Affichage → Agent** ou par
`⌥⌘A`. La première fois, il s'ouvre en haut à droite de l'éditeur, sous sa
barre de titre ; ensuite là où on l'a laissé, tant qu'un écran le porte
encore : un écran débranché le ramène près de l'éditeur
(`src/AgentWindow.h`). Sa place, et s'il était montré, sont des préférences
locales. Montré par le bouton ou la touche, il prend le clavier dans son
champ ; caché, il le rend à l'éditeur. Dans son champ, `⌘C`, `⌘X`, `⌘V`,
`⌘Z` et `⌘A` agissent sur le texte, jamais sur le montage. Caché, il reçoit
toujours ses réponses et le journal de l'agent relayé : un point sur le
bouton (« Agent • ») le signale jusqu'à ce qu'on le montre. Il se cache avec
l'application quand une autre passe devant, et la vidéo plein écran le
recouvre. Le dialogue **Moteur IA** reste une feuille sur la fenêtre de
l'éditeur.

Le dialogue n'active que les champs du protocole choisi : modèle, URL de base
et clé pour Anthropic et pour Ollama, la commande seule pour l'agent de code ;
les autres sont grisés, et leurs invites suivent le menu. Il s'ouvre sur ce
qui est enregistré ; un champ laissé vide n'enregistre rien et prend la
valeur proposée en gris. Pour utiliser le modèle Qwen Coder déjà installé sur
la machine, choisissez **Ollama local** : modèle `qwen2.5-coder:7b` et URL
`http://localhost:11434` par défaut. Ollama local ne nécessite aucune clé ;
une clé saisie part en `Authorization: Bearer` (un Ollama derrière un proxy,
ou ollama.com). Les appels d’outils restent disponibles : le modèle peut donc
observer et modifier le montage via les mêmes opérations réversibles que le
moteur Anthropic.

Le transport Ollama utilise `/api/chat` et demande un contexte de 32K tokens.
Avant de renvoyer `describe` au modèle local, l’app conserve la timeline, les
sources utilisées, les aliases de médiathèque, les chutiers et les marqueurs,
mais retire les métadonnées média redondantes. Pour les modèles anciens qui
écrivent un appel sous la forme JSON `{name, arguments}` au lieu d’un vrai
`tool_calls`, ce format n’est exécuté que si l’objet est strict et désigne un
outil réellement publié par CUTMACHINE.

Pour monter une interview, `get_timeline_transcript` renvoie des spans
sélectionnables et `create_interview_short` **n'accepte que leurs `span_id`**
(ou les `block_id` de `list_speech_blocks`), avec la `transcript_revision` lue
en même temps.
Le modèle ne recopie aucun temps : c'est le moteur qui résout chaque
identifiant en position exacte, ce qui rend une coupe au milieu d'un mot
inexprimable au lieu de simplement déconseillée. Si un mot chevauche une coupe,
il n'apparaît qu'une fois, dans le plan qui joue son début (ou, si ce début a
été coupé, dans celui qui joue la suite), et son span porte
`straddles_cut: true` pour signaler que son rendu peut être approximatif.
L'attribution au plus grand recouvrement faisait passer un mot à cheval sur
une coupe dans le plan suivant, où il n'était pas prononcé en premier.
Quand une idée dépasse un span — ils sont découpés pour la lisibilité d'un
sous-titre, environ
42 caractères, ce qui n'est pas une unité de montage — `span_id` et
`end_span_id` désignent un intervalle contigu que le moteur fusionne en une
seule plage. Un intervalle qui enjambe un silence ou change de source est
refusé, jamais recousu en silence.

Pour les raccourcissements A/V courants, l’outil MCP
`shorten_linked_clip` reçoit seulement un clip, un bord et une quantité en
images ou secondes. CUTMACHINE résout lui-même tous les membres liés, le signe
du trim et le `RationalTime` exact, puis applique une seule opération atomique.
Avec `preview=true`, la même opération est validée sur une copie via `EditLog`
et renvoyée sans modifier le projet.

Le modèle et l’URL sont des préférences locales `NSUserDefaults`. La clé est
conservée séparément dans le Trousseau macOS : elle n’est écrite ni dans le
document projet, ni dans son journal, ni dans les préférences. Sans modèle
enregistré, le panneau parle à `claude-opus-5-5` sur
`https://api.anthropic.com`, avec au plus 16 000 tokens par appel (réflexion
comprise ; chaque appel d'un tour qui enchaîne des outils a les siens) et
600 s d'attente par réponse. `CUTMACHINE_ANTHROPIC_MODEL`, à défaut
`CUTMACHINE_MODEL`, remplace ce modèle, `CUTMACHINE_ANTHROPIC_URL` l'URL et
`ANTHROPIC_API_KEY` la clé ; un modèle ou une URL enregistrés dans le
dialogue passent devant. Le dialogue enregistrait autrefois le modèle en
service à chaque validation : qui l'a validé avant le 3 octobre 2026 garde ce
modèle (`defaults read com.cutmachine.editor AiEngine.Model.v1`) tant qu'il
ne vide pas le champ Modèle.

Le protocole **Agent de code (ACP)** lance par défaut
`npx -y @agentclientprotocol/claude-agent-acp@0.85.1` : une version épinglée,
sans quoi `npx` exécutait à chaque lancement la dernière publication du
paquet. Une commande enregistrée dans le dialogue n'est jamais réécrite ; la
procédure pour monter la version est dans `src/AiEngineForm.h`.

Un agent de code demande la permission avant chaque appel d'outil. Le
panneau la donne lui-même pour les outils du moteur, sauf pour ceux qu'il
montre à la personne, une fois chacun et sans l'option « toujours » :
`generate_media`, qui dépense sur son compte (GENERATIF-2026-10 GN2). Les
appels `after_*` passent sans demander. Sans personne pour répondre, un appel montré est refusé. Un
adaptateur réglé pour tout permettre ne demande rien au panneau, qui n'a
alors rien à montrer.

## Le projet du panneau

Le panneau Agent travaille sur le projet de sa fenêtre, et sur lui seul,
tant qu'elle est ouverte ; un agent relayé à la fenêtre aussi, qu'il soit
l'agent de code du panneau ou une session Claude Code ouverte à côté
(`--mcp-serve --stdio`, voir [Commandes headless](commandes-headless.md)).
La boucle intégrée et le point d'accès de la fenêtre passent par le même
adaptateur (`MainThreadBackend.h`) et ont les mêmes réponses.

Une édition, une annulation, un rétablissement ou la clôture d'une
intervention (`end_intervention`) demandés par le panneau ou par un agent
relayé sont enregistrés dans le paquet avant que la fenêtre ne les prenne,
comme un geste de la souris. Si le stockage refuse l'écriture (paquet en
lecture seule, disque plein), l'appel est refusé par `IoError`, le nom que
lui donnent `--apply-op` et le serveur MCP, avec la raison du système ; la
fenêtre, le paquet et le journal restent ce qu'ils étaient, et la ligne sous
la timeline dit « Modification de l'agent refusée, rien n'a changé ».
Jusqu'au 24 octobre 2026, l'appel répondait `ok` avec le delta d'une édition
que la fenêtre venait de défaire.

`ingest_media` importe des rushes comme **Fichier → Importer des rushes…** :
les fichiers sont lus en tâche de fond (« Import N files » dans la barre des
tâches média, qui l'annule), puis ajoutés par l'import de la fenêtre, à la
racine de la médiathèque, avec leur niveau sonore, leur vignette et leur
forme d'onde ; la ligne sous le navigateur dit ce qui est entré. Le chemin
est absolu ; `recursive` descend dans les sous-dossiers. La réponse est celle
de `--ingest` (`added`, `skipped`, `ignored`, `errors`), et un import qui
n'a rien pu lire est refusé de même (`IoError`). Depuis le 24 octobre 2026
(DEC32), il est, comme l'import du menu, une étape du journal de la
timeline : `Cmd+Z` le reprend ; `bin_id` range les rushes dans un chutier.

Depuis le 24 octobre 2026 (DEC10, AX-2026-09 AX7), les tâches de fond d'un
agent relayé à la fenêtre — un export ou une analyse demandés avec
`async: true` — tournent sur le gestionnaire de tâches de l'éditeur : la
barre des tâches les montre (« Agent · export_timeline », par exemple) et
les annule, comme l'import ; `get_task` et `cancel_task` les lisent et les
annulent au même endroit. Elles tournaient avant sur un gestionnaire propre
au point d'accès, que rien ne montrait. Un export demandé sans `async` ne
passe toujours pas par la barre des tâches.

`create_project`, et `open_project` d'un autre projet, sont refusés
(`UnsupportedOperation`) : le refus nomme le projet de la fenêtre et dit
comment en changer : la personne choisit **Nouveau projet…** ou **Ouvrir un
projet…** dans le menu **Fichier** de la fenêtre, qui se rouvre sur l'autre
projet (DEC39, 24 octobre 2026 ; avant, il fallait quitter CUTMACHINE et
passer par la fenêtre d'accueil). `open_project` du projet de
la fenêtre répond, avec `app: true`.

`import_resolve_timeline` relit une timeline Resolve dans la timeline de la
fenêtre (celle que nomme `timeline_id`, sinon celle à l'écran), avec la
réponse de `--import-resolve-timeline`, l'étalonnage lu dans la base Resolve
compris, et écrit sur ses plans quand il se rend entier
([Resolve](resolve.md)). Le manifeste et la copie de la base sont
lus en tâche de fond ; le plan et les opérations passent par le journal de
la fenêtre, enregistrés une fois : l'import s'annule par `Cmd+Z`. Un refus
ne change rien à la fenêtre (depuis le 24 octobre 2026, PHASES-2026-08 F2.4 ;
pas encore vu à l'écran).
