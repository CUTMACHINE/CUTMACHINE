# Archive SQL des projets

DB1–DB3 — `DONNEES-2026-10`, 5 octobre 2026. Schéma 3, « git dans SQLite »
(DEC44), 24 octobre 2026.

Une base SQLite locale conserve chaque état sauvegardé des projets pour les
interroger après coup. Chaque capture est immuable ; relue, elle rend les
mêmes octets que `Project::SaveToString()` au moment de la capture. Depuis le
schéma 3, une capture est une liste d'empreintes : chaque morceau du projet
(chaque timeline, chaque composition, le reste du document) est rangé une
seule fois sous son empreinte SHA-256, compressé. Le projet et son journal
d'opérations continuent de fonctionner sans archive.

## Archivage automatique

Les sauvegardes du moteur archivent le projet complet, depuis l’app, le CLI
et le MCP. La création et la collecte d’un paquet capturent leur état
seulement après publication du dossier final. Les autosauvegardes non
validées ne sont pas archivées.

La base commune se trouve par défaut dans
`~/Library/Application Support/CUTMACHINE/projects.sqlite`, et sous Linux
dans `~/.local/share/cutmachine/projects.sqlite` (HEADLESS-2026-10). Le dossier est
créé à la première livraison automatique. `CUTMACHINE_PROJECT_ARCHIVE`
permet de choisir un autre chemin **absolu** ; `off` désactive les nouvelles
captures et conserve celles déjà en attente. Ce réglage reste local à la
machine, hors du JSON projet. Les outils MCP utilisent cette base lorsque
`database` est omis ; le CLI de requête reçoit son chemin explicitement.

Une sauvegarde prépare un fichier `.cutmachine-archive-<ULID>.json` dans le
paquet, avec exactement le JSON canonique du projet. Ce fichier participe à
la même transaction que le montage et ses journaux. SQLite reçoit ensuite
la capture (`step:"save"`). Le fichier d’attente est retiré seulement après
le commit SQL. Un arrêt après ce commit retrouve la même ligne par son ULID ;
un arrêt pendant le commit de fichiers restaure aussi la file d’attente.

Si SQLite est inaccessible, la sauvegarde du montage réussit et les captures
restent dans le paquet. Une ouverture ou sauvegarde suivante réessaie de les
livrer. L’app affiche une alerte par période de captures en attente ; le
CLI et les serveurs écrivent le refus sur stderr. Le statut et le rattrapage
sont aussi accessibles explicitement :

```sh
./build/cutmachine --project-archive-status /chemin/film.cutmachine-project
./build/cutmachine --sync-project-archive /chemin/film.cutmachine-project
```

Au MCP : `project_archive_status` et `sync_project_archive`, sans arguments,
pour le projet ouvert. Ils rendent `database`, `enabled`, `archived`,
`pending` et `complete`, avec `error` et `detail` si la livraison échoue,
et `backup`, l'état de la [copie de sécurité](#la-copie-de-sécurité).
Un `complete:false` de synchronisation signifie que les captures ne sont pas
toutes livrées, pas que le montage doit être réédité. Le CLI de
synchronisation sort alors avec le code 1, en conservant le rapport JSON.
Ne pas supprimer les fichiers d’attente : ils peuvent porter des états
antérieurs que le projet courant ne contient plus. Leur reprise utilise la
base configurée au moment de la livraison. `created_at` date l’insertion SQL,
pas la sauvegarde initiale si la livraison a été différée.

## Importer les projets existants

```sh
./build/cutmachine --import-project-archive /chemin/projets.sqlite \
  /chemin/film.cutmachine-project /chemin/autre.cutmachine-project
```

Au MCP, `import_project_archive` prend `projects` (1 à 100 chemins absolus de
paquets ou de leurs `project.cutmachine.json`) et éventuellement `database`.
Il fonctionne sans projet ouvert. Chaque projet est verrouillé puis validé,
et le JSON canonique est conservé avec `step:"import"`. Les médias ne sont
pas copiés. Un projet tenu par l’app est refusé : utiliser `archive_project`
pour capturer le projet actuellement ouvert.

Réimporter les mêmes octets réutilise la ligne d’import (`replayed:true`) ;
un état différent ajoute une ligne. Depuis le schéma 3, les octets se
comparent par le manifeste : les mêmes empreintes aux mêmes places sont le
même document. La recherche et l’insertion partagent une transaction SQL, y
compris pour des imports concurrents. Le rapport rend `imported`, `refused`,
`complete` et le résultat de chaque projet. Le CLI sort 1 si certains
projets sont refusés ; les succès restent acquis et visibles, et relancer
l’import ne les duplique pas.

## Capturer un état

```sh
./build/cutmachine --archive-project /chemin/film.cutmachine-project \
  /chemin/projets.sqlite '{"run_id":"portrait-1","step":"selection"}'
```

Le CLI accepte un paquet ou son `project.cutmachine.json`. Il prend le verrou
du projet pour lire un état cohérent ; si l'application tient déjà le projet,
utiliser son serveur MCP. L'archivage n'édite ni le projet ni son historique.
La base est créée au premier archivage ; son dossier doit déjà exister.

Par MCP, `archive_project` prend `database` (chemin absolu), puis les champs
facultatifs `snapshot_id`, `run_id` et `step`. Il capture le projet ouvert,
toutes timelines comprises, sans changer la timeline active.

La réponse nomme `snapshot_id`, `project_id`, `revision_hash`, `created_at`
(UTC), `run_id`, `step` et `replayed`. Le hash est celui du JSON complet du
projet, pas celui d'une timeline seule. Fournir un ULID dans `snapshot_id`
permet de renvoyer une capture après une coupure : les mêmes octets et le même
contexte rendent la première capture (`replayed:true`), même depuis un autre
processus. Un autre contenu sous cet identifiant est refusé
`SnapshotIdReused`. Sans identifiant fourni, chaque appel crée une capture.

## Interroger les JSON

```sh
./build/cutmachine --query-project-archive /chemin/projets.sqlite \
  "SELECT project_id, created_at, step,
          json_extract(document_json, '$.name') AS name,
          json_array_length(document_json, '$.timelines') AS timelines
   FROM project_snapshots ORDER BY created_at, snapshot_id" \
  '{"max_rows":100}'
```

Par MCP : `query_project_archive` avec `database` (chemin absolu), `sql` et
éventuellement `max_rows`. Aucun projet ouvert n'est nécessaire.

Une requête est un seul `SELECT`, éventuellement avec `WITH`, et peut utiliser
`json_extract`, `json_each`, `json_tree` et les autres fonctions JSON SQLite.
Le résultat donne `columns`, `rows` (tableaux de valeurs dans l'ordre des
colonnes) et `truncated`. Les entiers 64 bits restent exacts ; les noms de
colonnes peuvent se répéter. Pour récupérer un projet entier :

```sql
SELECT document_json FROM project_snapshots
WHERE snapshot_id = '01…';
```

Le JSON est rendu comme une chaîne dans la réponse JSON : après décodage de
cette chaîne, ses octets sont ceux du document canonique.

`project_snapshots` est, depuis le schéma 3, une **vue** : mêmes sept
colonnes dans le même ordre (`snapshot_id`, `project_id`, `revision_hash`,
`created_at`, `run_id`, `step`, `document_json`), et `document_json`
reconstitué à la lecture, à l'octet près. Ce qui change pour l'appelant :

- une vue n'a pas de `rowid` : ordonner par `created_at, snapshot_id` ;
- `document_json` n'est calculé que s'il est lu : `count(*)` ou un filtre sur
  `project_id` ne reconstitue rien ;
- une requête sur toutes les captures est plus rapide qu'avant (les morceaux
  communs ne sont décompressés qu'une fois par connexion) : sur la copie de
  l'archive de l'auteur du 24 octobre, les noms des 3 222 captures se lisent en
  1,3 s, quand le schéma 2 dépassait le budget de 5 s.

Deux fonctions, présentes sur les connexions du moteur, servent la vue et
les requêtes qui veulent descendre plus bas :

- `archive_document(manifest_json)` : le document d'une capture ;
- `archive_chunk(digest)` : un morceau seul, en texte. Par exemple, chaque
  version d'une timeline sans reconstituer les projets entiers :

```sql
SELECT s.created_at,
       json_extract(archive_chunk(part.value->>'digest'), '$.name')
FROM archive_snapshots AS s, json_each(s.manifest_json, '$.parts') AS part
WHERE part.value->>'kind' = 'timeline' AND part.value->>'id' = '01…'
ORDER BY s.created_at;
```

Chaque morceau lu est vérifié contre son empreinte : un morceau modifié
derrière le dos de l'archive est refusé (`ArchiveError`), jamais lu comme un
autre document.

## Tables et limites

Schéma 3 :

- `archive_chunks` : `digest` (clé primaire, SHA-256 en 64 chiffres
  hexadécimaux minuscules du morceau décompressé), `bytes` (sa taille
  décompressée), `codec` (`zstd`) et `content` (BLOB, une trame zstd
  complète, niveau 9) ;
- `archive_snapshots` : `snapshot_id` (clé primaire ULID), `project_id`,
  `revision_hash`, `created_at`, `run_id`, `step` et `manifest_json`, le
  manifeste de la capture ;
- `project_snapshots` : la vue décrite plus haut ;
- `project_run_steps` : les reçus d'étapes, inchangés, liés à
  `archive_snapshots` (voir [Étapes durables](orchestration-montage.md)).

Le manifeste est un JSON canonique, décrit une fois par sa table de format
(`DocumentTable<ArchiveManifest>`, `src/DocumentFormat.h`) et figé par
`tests/fixtures/document_format/` :

```json
{"format":"cutmachine.archive-manifest.v1","bytes":474928,
 "rest":"<sha256 du document sans ses parties>",
 "parts":[{"kind":"timeline","id":"01…","at":61542,"digest":"<sha256>"}]}
```

`bytes` est la taille du document, `at` l'octet où la partie commence dans
le document, `kind` vaut `timeline` ou `composition` (les éléments des
tableaux `timelines` et `compositions` du projet), `id` l'identifiant de
l'élément (vide s'il n'en a pas). La découpe se fait dans les octets
eux-mêmes, jamais en resérialisant : un document écrit par un sérialiseur
plus ancien, ou qui ne se découpe pas, garde ses octets (il est alors rangé
entier, sans partie). Elle est prouvée à chaque capture : le document
reconstitué doit être celui qui a été capturé.

Les index portent sur projet/date et run/étape. Des triggers refusent les
mises à jour, les suppressions et une seconde ligne sous la même clé, dans
`archive_chunks`, `archive_snapshots` et `project_run_steps`, y compris dans
un client SQLite ordinaire ; une capture qui nommerait un morceau absent est
refusée. Aucun morceau n'est jamais supprimé, même s'il ne sert plus. Les
propriétés des clips restent dans le JSON, pas dans des tables séparées.

Un client SQLite ordinaire (le `sqlite3` de macOS, par exemple) lit les
métadonnées, les manifestes et les reçus, mais pas `project_snapshots` : il
n'a pas `archive_document` (« no such function »). Les morceaux sont des
trames zstd standard, que l'outil `zstd` décompresse ; le moteur, lui, rend
les documents par `--query-project-archive` et `query_project_archive`.

Le lecteur ouvre la base en lecture seule et refuse écritures, `ATTACH`,
`PRAGMA`, chargement d'extensions et plusieurs instructions. Limites : 100
lignes par défaut, 1000 au maximum ; SQL de 64 Kio au plus ; contenu des
colonnes et lignes sérialisées de 4 Mio au plus ; budget d'environ deux
millions d'instructions SQLite ou cinq secondes. Un dépassement de lignes
rend `truncated:true` ; les dépassements de taille ou de calcul rendent
`ArchiveLimit`. Les BLOB (dont `archive_chunks.content`) et nombres non
finis sont refusés. Une requête ne crée jamais une base absente.

Les captures acceptent un JSON de moins de 64 Mio (avec une réserve pour les
métadonnées) et 1024 octets au plus pour chacun des champs run/étape. Le schéma
porte `application_id` et `user_version` ; une base étrangère ou une version
inconnue est refusée `ArchiveSchemaMismatch`.

## La migration vers le schéma 3

Un écrivain (une capture, un import, la livraison d'une sauvegarde, une
copie de sécurité) qui ouvre une archive de schéma 1 ou 2 la migre d'abord,
dans une transaction à elle : chaque capture est découpée, ses morceaux
rangés, son manifeste inséré avec sa date d'origine, puis le document est
reconstitué depuis ce que la base contient désormais et comparé aux anciens
octets ; une seule différence annule tout (`ArchiveMigrationFailed`). Les
reçus d'étapes passent dans une table qui nomme `archive_snapshots`, la
vieille table devient la vue, puis la base est compactée (`VACUUM`) quand la
migration a libéré plus de la moitié du fichier. Un processus tué pendant la
migration laisse le journal de SQLite rendre la base de schéma 2 entière ;
l'écrivain suivant recommence. Un lecteur consulte une base de schéma 1 ou 2
telle quelle, sans la migrer.

Mesuré sur une copie de l'archive de l'auteur du 24 octobre 2026 (3 222
captures, schéma 2) : 1 325 101 056 octets avant, 19 861 504 après, en 2,9 s
migration, compactage et première copie compris, 298 Mo de mémoire au plus.
Chaque capture relue est identique à l'octet près, par la vue du moteur et
par une reconstitution indépendante (Python et l'outil `zstd`). Pendant la
migration, les autres écrivains attendent cinq secondes puis laissent leurs
captures en attente dans leur paquet, livrées à la sauvegarde suivante.

Une fois migrée, l'archive est refusée par un binaire plus ancien
(`ArchiveSchemaMismatch`) : ses captures restent en attente dans les paquets,
sans perte, jusqu'à ce qu'un binaire à jour les livre.

## La copie de sécurité

L'archive vit sur le disque des projets : elle survit à la mort d'un disque
de rushes, pas à celle du disque interne. Une copie quotidienne va donc
ailleurs, dans un dossier qu'un client de synchronisation (OneDrive) porte
hors de la machine ; le moteur n'y parle à aucun service, il écrit un fichier.

Le dossier se règle une fois dans `~/.config/cutmachine/.env` (ou
l'environnement, qui l'emporte) :

```sh
CUTMACHINE_ARCHIVE_BACKUP_DIR='/Users/…/CloudStorage/OneDrive-…/Sauvegardes/CUTMACHINE'
```

Un chemin absolu, vers un dossier qui existe : il n'est jamais créé (un
dossier absent est souvent un client de synchronisation pas monté). Sans
réglage, ou `off`, aucune copie ; `--check-setup` le dit (élément
`archive_backup`, facultatif) sans échouer.

- **Quand.** Après une livraison qui a archivé au moins une capture (une
  sauvegarde, la reprise d'une capture en attente), au plus une fois par
  jour, date locale. Et à la demande : `cutmachine --backup-project-archive`,
  ou `backup_project_archive` au MCP, sans projet ouvert, qui réécrit la
  copie du jour.
- **Quoi.** `CUTMACHINE_projects_<AAAAMMJJ>.sqlite`, à côté des sauvegardes
  de bases Resolve du même dossier : une base entière, de schéma 3, ouvrable
  par le moteur comme l'originale (`--query-project-archive` sur la copie).
  Elle est faite par `VACUUM INTO` dans un fichier temporaire du dossier
  (`.CUTMACHINE_projects_<AAAAMMJJ>.sqlite.tmp`), synchronisé sur le disque,
  vérifié (`PRAGMA quick_check`), puis renommé : le nom daté ne désigne jamais
  une base à moitié écrite, et la copie est un seul état de l'archive même
  si des agents écrivent pendant qu'elle se fait. Le fichier temporaire sert
  de verrou : un autre processus qui copie fait passer son tour ; un
  temporaire de plus d'une heure, laissé par un processus mort, est remplacé.
- **Combien.** Les sept copies datées les plus récentes ; les plus anciennes
  sont retirées après une copie réussie. Aucun autre fichier du dossier n'est
  touché, ni une copie datée d'après aujourd'hui. Environ 20 Mo chacune le
  24 octobre 2026 ; la copie elle-même prend 0,07 s.
- **Ce qu'elle emporte.** La structure des montages, les chemins des médias
  et les textes posés ; ni image ni son.

Une copie qui échoue ne fait jamais échouer une sauvegarde : le refus est
écrit sur stderr et dans `backup` du statut (`configured`, `directory`,
`latest`, `due`, `performed`, `path`, `removed`, `error`, `detail`).

## Suite prévue

DB1 et DB2 couvrent la capture explicite, les sauvegardes automatiques et
l’import des projets existants. DB3 lie les captures aux reçus d’étapes,
plans et journaux ; tout client MCP peut s’en servir pour
[reprendre sans double édition](orchestration-montage.md). Le schéma 3
(DEC44) range chaque morceau une fois et copie l'archive chaque jour hors du
disque. L’indexation générale des analyses reste dans les pistes ; l'élagage
proposé par DB3 n'est qu'une option, si la base regrossit.
