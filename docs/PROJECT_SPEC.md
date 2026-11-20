# Objet projet

> Relu contre le code le 3 octobre 2026 (`src/engine/Project.h`, `src/engine/Operations.h`,
> `src/engine/DocumentFormat.cc`, `src/engine/ProjectBin.cc`, `src/app/main.mm`). Non vérifié :
> le choix de récupération d’une autosave au lancement et le chutier tel que
> la fenêtre le présente, qui se jugent à l’écran.

La disposition des fichiers, les règles de partage, les limites de concurrence
et la cible de portabilité sont spécifiées dans
[`PROJECT_STORAGE_SPEC.md`](PROJECT_STORAGE_SPEC.md).

`Project` est l’agrégat d’un montage, comparable au projet de Premiere ou à la
bibliothèque de projet de Resolve. Une instance possède :

- une identité et un nom stables ;
- les rushes et chutiers partagés ;
- les sources montables ;
- les réglages communs, notamment la gestion de couleur ;
- une ou plusieurs timelines et l’ULID de la timeline ouverte par défaut.

Les dimensions et la cadence restent des réglages de timeline : deux exports
horizontal et vertical peuvent donc partager les mêmes rushes sans dupliquer le
projet.

Le moteur de timeline édite un `Document`, snapshot d’une timeline
adressée et des ressources partagées. `MakeDocument(timeline_id)` ouvre ce
contexte ; `CommitDocument(timeline_id, document)` le valide puis le réintègre
au projet. `MakeActiveDocument()` et `CommitActiveDocument()` adressent la
timeline active sans introduire un second format de persistance.

Cette frontière est maintenant utilisée par l’application. `AppState` possède
le `Project` complet et un `Document` focalisé. Les timelines sont affichées
comme des objets du chutier ; un double-clic sélectionne leur document de
montage, tandis que `Cmd+Option+N` en crée une au format de la timeline
courante. La sélection courante, la position de lecture et l’undo de chaque
timeline sont conservés séparément pendant la session.

Sélectionner une timeline est une navigation, comme déplacer le playhead : ce
n’est ni une `ProjectOperation`, ni une entrée d’undo, ni une réécriture du
fichier projet. `active_timeline_id` reste dans le fichier projet comme choix
d’ouverture par défaut, mais
`AppState::activeTimelineId` porte la sélection de session. Hors de la
fenêtre, ce choix d’ouverture se change par `SetActiveProjectTimelineOperation`
(outil `set_active_timeline`, `--apply-project-op`), qui est, elle, une
opération projet journalisée.

## Opérations et historique projet

Les mutations persistantes au-dessus d’une timeline utilisent un second
journal, `ProjectEditLog`, indépendant du `EditLog` de montage :

- `AddProjectTimelineOperation` et `RemoveProjectTimelineOperation` ajoutent ou
  retirent une timeline ; les ULID de timeline et de pistes sont générés à la
  première application puis conservés au redo ;
- `CloneProjectTimelineOperation` duplique une timeline en donnant de
  nouvelles identités à tous ses objets, et
  `CreateProjectTimelineVariantsOperation` en crée un lot, avec des
  remplacements de texte comptés ;
- `CreateProjectTimelineFromSegmentsOperation` assemble une nouvelle timeline
  depuis des plages source ;
- `SetActiveProjectTimelineOperation` change la timeline ouverte par défaut ;
- `SetProjectBinMetadataOperation` modifie description, note, tags et ordre
  d’un rush ou d’une timeline ;
- `SetProjectTimelineBinOperation` classe une timeline dans un chutier ;
- `RenameProjectItemOperation` renomme une timeline ou le nom d’affichage
  d’un rush sans modifier le nom de son fichier source ;
- `RelinkProjectMediaOperation` reconnecte atomiquement un ou plusieurs rushes
  partagés par toutes les timelines et invalide leurs proxies ;
- `ChooseProjectTimelineOperation` écrit le choix entre des timelines (la
  retenue et ses alternatives) ; il ne change rien au projet, sinon la
  timeline active si on le demande (MOTIF-2026-10 MOTIF6).

Une entrée de ce journal porte au plus un motif, pourquoi l'édition a été
faite (`EditMotif.h`, MOTIF6) ; un journal du projet qui en porte un s'écrit
en version 2, sans motif en version 1, à l'octet comme avant.

Chaque opération est validée sur une copie, sérialisable et enrichie du delta
exact de ce qu’elle change (`ProjectDelta.h`). Undo restitue donc les octets
projet d’origine, redo conserve toutes les identités, et l’un comme l’autre ne
touchent que ce que l’opération a changé : annuler le renommage d’une timeline
garde les éditions faites depuis sur cette timeline. L’application route `Cmd+Z` et
`Cmd+Shift+Z` vers le journal timeline ou projet concerné. Les commandes
headless `--apply-project-op`, `--undo-project-op` et `--redo-project-op`
emploient exactement le même moteur et le sidecar
`<projet>.project-editlog.json`.

## Persistance

Le fichier projet utilise l’enveloppe `cutmachine-project`, version 3. Elle
conserve l’identité du projet, la timeline active, la gestion couleur, la
bibliothèque, les bins et les sources une seule fois, puis toutes les
timelines, les métadonnées du chutier et leur placement. Chaque partie est
écrite et relue par le code de `Document`, afin que le moteur, le CLI et
l’export utilisent toujours le même format validé. La version 2, qui répétait
la bibliothèque dans chaque timeline, reste lue et devient une version 3 à la
sauvegarde suivante.

Les compositions motion (MOTION-2026-09 M3, `docs/MOTION_FORMAT.md`)
appartiennent au projet et figurent, quand il y en a, sous `compositions`,
après les sources. Un projet qui en porte déclare `document_version` 8 au
moins ; un projet qui ne porte rien de postérieur à la version 7 reste en 7,
octet pour octet, et un binaire antérieur l’ouvre encore. La version déclarée
est la plus ancienne qui porte tout ce que le projet contient, jusqu’à 17 au
3 octobre 2026 (`DocumentFormatVersion`, `src/engine/Document.h`).

Les fichiers document autonomes, enveloppes projet v1 et packages v1 sont
refusés. Il n’existe plus de promotion ni de migration implicite.

Avant chaque commit, une autosave atomique du projet complet est écrite. Elle
n’est supprimée qu’après réussite du remplacement transactionnel du projet, du
journal de la timeline focalisée et du journal projet. Au lancement, une
autosave valide plus récente déclenche un choix explicite de récupération.

## Modèle de chutier

Le chutier suit la séparation utilisée par Kdenlive entre un arbre de données
et sa projection filtrée. `ProjectBinModel` expose dans une même hiérarchie les
dossiers, rushes et timelines, sans dépendre d’AppKit. Les métadonnées de
présentation (description, note de zéro à cinq, tags et ordre d’insertion)
restent au niveau projet et ne polluent pas les données techniques du média.

Le filtrage accepte les tags, notes et types en logique **OU** à l’intérieur
d’une catégorie et combine les catégories en logique **ET**. Un dossier reste
visible si un descendant correspond. Les filtres utilisé/inutilisé sont
calculés à partir de toutes les timelines, avec un compteur séparé pour la
timeline active. Le tri naturel place donc `Clip 2` avant `Clip 10` et garde les
dossiers avant leurs éléments.
