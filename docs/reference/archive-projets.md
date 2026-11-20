# Project SQL archive

DB1–DB3 — `DONNEES-2026-10`. Schema 3, "git in SQLite" (DEC44).

A local SQLite database keeps every saved state of the projects so that they
can be queried afterwards. Each snapshot is immutable; read back, it returns
the same bytes as `Project::SaveToString()` at the time of capture. Since
schema 3, a snapshot is a list of digests: each piece of the project (each
timeline, each composition, the rest of the document) is stored once under its
SHA-256 digest, compressed. The project and its operation journal keep working
without an archive.

## Automatic archiving

The engine's saves archive the whole project, from the app, the CLI and the
MCP. Creating and collecting a package capture their state only after the final
folder is published. Unvalidated autosaves are not archived.

The shared database is by default in
`~/Library/Application Support/CUTMACHINE/projects.sqlite`, and under Linux in
`~/.local/share/cutmachine/projects.sqlite` (HEADLESS-2026-10). The folder is
created at the first automatic delivery. `CUTMACHINE_PROJECT_ARCHIVE` allows
choosing another **absolute** path; `off` disables new snapshots and keeps
those already pending. This setting stays local to the machine, outside the
project JSON. The MCP tools use this database when `database` is omitted; the
query CLI receives its path explicitly.

A save prepares a `.cutmachine-archive-<ULID>.json` file in the package, with
exactly the project's canonical JSON. This file takes part in the same
transaction as the edit and its journals. SQLite then receives the snapshot
(`step:"save"`). The pending file is removed only after the SQL commit. A stop
after this commit finds the same row again by its ULID; a stop during the file
commit also restores the queue.

If SQLite is inaccessible, the save of the edit succeeds and the snapshots stay
in the package. A later open or save retries delivering them. The app shows one
alert per period of pending snapshots; the CLI and the servers write the
refusal to stderr. The status and the catch-up are also available explicitly:

```sh
./build/cutmachine --project-archive-status /chemin/film.cutmachine-project
./build/cutmachine --sync-project-archive /chemin/film.cutmachine-project
```

Through the MCP: `project_archive_status` and `sync_project_archive`, with no
arguments, for the open project. They return `database`, `enabled`, `archived`,
`pending` and `complete`, with `error` and `detail` if delivery fails, and
`backup`, the state of the [backup copy](#the-backup-copy).
A synchronization's `complete:false` means that the snapshots have not all been
delivered, not that the edit must be redone. The synchronization CLI then exits
with code 1, keeping the JSON report. Do not delete the pending files: they may
carry earlier states that the current project no longer contains. Their
recovery uses the database configured at the time of delivery. `created_at`
dates the SQL insertion, not the initial save if delivery was deferred.

## Importing existing projects

```sh
./build/cutmachine --import-project-archive /chemin/projets.sqlite \
  /chemin/film.cutmachine-project /chemin/autre.cutmachine-project
```

Through the MCP, `import_project_archive` takes `projects` (1 to 100 absolute
paths of packages or of their `project.cutmachine.json`) and optionally
`database`. It works without an open project. Each project is locked then
validated, and the canonical JSON is kept with `step:"import"`. Media are not
copied. A project held by the app is refused: use `archive_project` to capture
the project that is currently open.

Reimporting the same bytes reuses the import row (`replayed:true`); a different
state adds a row. Since schema 3, bytes are compared through the manifest: the
same digests in the same places are the same document. The lookup and the
insertion share one SQL transaction, including for concurrent imports. The
report returns `imported`, `refused`, `complete` and the result of each
project. The CLI exits with 1 if some projects are refused; the successes
remain acquired and visible, and rerunning the import does not duplicate them.

## Capturing a state

```sh
./build/cutmachine --archive-project /chemin/film.cutmachine-project \
  /chemin/projets.sqlite '{"run_id":"portrait-1","step":"selection"}'
```

The CLI accepts a package or its `project.cutmachine.json`. It takes the
project lock to read a consistent state; if the application already holds the
project, use its MCP server. Archiving edits neither the project nor its
history. The database is created at the first archiving; its folder must
already exist.

Through the MCP, `archive_project` takes `database` (absolute path), then the
optional fields `snapshot_id`, `run_id` and `step`. It captures the open
project, all timelines included, without changing the active timeline.

The response names `snapshot_id`, `project_id`, `revision_hash`, `created_at`
(UTC), `run_id`, `step` and `replayed`. The hash is that of the project's
complete JSON, not that of a single timeline. Supplying a ULID in
`snapshot_id` allows resending a snapshot after an interruption: the same
bytes and the same context return the first snapshot (`replayed:true`), even
from another process. Other content under this identifier is refused with
`SnapshotIdReused`. Without a supplied identifier, each call creates a
snapshot.

## Querying the JSON

```sh
./build/cutmachine --query-project-archive /chemin/projets.sqlite \
  "SELECT project_id, created_at, step,
          json_extract(document_json, '$.name') AS name,
          json_array_length(document_json, '$.timelines') AS timelines
   FROM project_snapshots ORDER BY created_at, snapshot_id" \
  '{"max_rows":100}'
```

Through the MCP: `query_project_archive` with `database` (absolute path),
`sql` and optionally `max_rows`. No open project is needed.

A query is a single `SELECT`, optionally with `WITH`, and can use
`json_extract`, `json_each`, `json_tree` and the other SQLite JSON functions.
The result gives `columns`, `rows` (arrays of values in column order) and
`truncated`. 64-bit integers stay exact; column names may repeat. To retrieve a
whole project:

```sql
SELECT document_json FROM project_snapshots
WHERE snapshot_id = '01…';
```

The JSON is returned as a string in the JSON response: once that string is
decoded, its bytes are those of the canonical document.

`project_snapshots` is, since schema 3, a **view**: the same seven columns in
the same order (`snapshot_id`, `project_id`, `revision_hash`, `created_at`,
`run_id`, `step`, `document_json`), and `document_json` rebuilt on reading,
to the byte. What changes for the caller:

- a view has no `rowid`: order by `created_at, snapshot_id`;
- `document_json` is computed only if it is read: `count(*)` or a filter on
  `project_id` rebuilds nothing;
- a query over all snapshots is faster than with schema 2 (shared pieces are
  decompressed only once per connection): on a copy of a real archive, the
  names of the 3,222 snapshots are read in 1.3 s, where schema 2 exceeded the
  5 s budget.

Two functions, present on the engine's connections, serve the view and the
queries that want to go deeper:

- `archive_document(manifest_json)`: the document of a snapshot;
- `archive_chunk(digest)`: a single piece, as text. For example, every
  version of a timeline without rebuilding whole projects:

```sql
SELECT s.created_at,
       json_extract(archive_chunk(part.value->>'digest'), '$.name')
FROM archive_snapshots AS s, json_each(s.manifest_json, '$.parts') AS part
WHERE part.value->>'kind' = 'timeline' AND part.value->>'id' = '01…'
ORDER BY s.created_at;
```

Each piece read is checked against its digest: a piece modified behind the
archive's back is refused (`ArchiveError`), never read as another document.

## Tables and limits

Schema 3:

- `archive_chunks`: `digest` (primary key, SHA-256 as 64 lowercase hexadecimal
  digits of the decompressed piece), `bytes` (its decompressed size), `codec`
  (`zstd`) and `content` (BLOB, a complete zstd frame, level 9);
- `archive_snapshots`: `snapshot_id` (ULID primary key), `project_id`,
  `revision_hash`, `created_at`, `run_id`, `step` and `manifest_json`, the
  snapshot's manifest;
- `project_snapshots`: the view described above;
- `project_run_steps`: the step receipts, unchanged, linked to
  `archive_snapshots` (see [Durable steps](orchestration-montage.md)).

The manifest is a canonical JSON, described once by its format table
(`DocumentTable<ArchiveManifest>`, `src/engine/DocumentFormat.h`) and frozen by
`tests/fixtures/document_format/`:

```json
{"format":"cutmachine.archive-manifest.v1","bytes":474928,
 "rest":"<sha256 du document sans ses parties>",
 "parts":[{"kind":"timeline","id":"01…","at":61542,"digest":"<sha256>"}]}
```

`bytes` is the size of the document, `at` the byte where the part starts in the
document, `kind` is `timeline` or `composition` (the elements of the project's
`timelines` and `compositions` arrays), `id` the element's identifier (empty if
it has none). The split is done in the bytes themselves, never by
reserializing: a document written by an older serializer, or one that does not
split, keeps its bytes (it is then stored whole, with no part). It is proven at
each snapshot: the rebuilt document must be the one that was captured.

The indexes cover project/date and run/step. Triggers refuse updates, deletions
and a second row under the same key, in `archive_chunks`, `archive_snapshots`
and `project_run_steps`, including in an ordinary SQLite client; a snapshot
that would name a missing piece is refused. No piece is ever deleted, even if
it is no longer used. Clip properties stay in the JSON, not in separate
tables.

An ordinary SQLite client (macOS's `sqlite3`, for example) reads the metadata,
the manifests and the receipts, but not `project_snapshots`: it does not have
`archive_document` ("no such function"). The pieces are standard zstd frames,
which the `zstd` tool decompresses; the engine returns the documents through
`--query-project-archive` and `query_project_archive`.

The reader opens the database read-only and refuses writes, `ATTACH`,
`PRAGMA`, extension loading and multiple statements. Limits: 100 rows by
default, 1000 at most; SQL of 64 KiB at most; column contents and serialized
rows of 4 MiB at most; a budget of about two million SQLite instructions or
five seconds. Exceeding the rows returns `truncated:true`; exceeding the size
or the computation returns `ArchiveLimit`. BLOBs (including
`archive_chunks.content`) and non-finite numbers are refused. A query never
creates a missing database.

Snapshots accept a JSON of less than 64 MiB (with a reserve for the metadata)
and at most 1024 bytes for each of the run/step fields. The schema carries
`application_id` and `user_version`; a foreign database or an unknown version
is refused with `ArchiveSchemaMismatch`.

## Migration to schema 3

A writer (a snapshot, an import, the delivery of a save, a backup copy) that
opens a schema 1 or 2 archive migrates it first, in a transaction of its own:
each snapshot is split, its pieces stored, its manifest inserted with its
original date, then the document is rebuilt from what the database now
contains and compared with the old bytes; a single difference cancels
everything (`ArchiveMigrationFailed`). The step receipts move to a table that
names `archive_snapshots`, the old table becomes the view, then the database is
compacted (`VACUUM`) when the migration has freed more than half the file. A
process killed during the migration leaves SQLite's journal to give back the
whole schema 2 database; the next writer starts again. A reader consults a
schema 1 or 2 database as it is, without migrating it.

Measured on a copy of a real archive (3,222 snapshots, schema 2):
1,325,101,056 bytes before, 19,861,504 after, in 2.9 s, migration, compaction
and first copy included, 298 MB of memory at most. Each snapshot read back is
identical to the byte, through the engine's view and through an independent
rebuild (Python and the `zstd` tool). During the migration, the other writers
wait five seconds then leave their snapshots pending in their package,
delivered at the next save.

Once migrated, the archive is refused by an older binary
(`ArchiveSchemaMismatch`): its snapshots stay pending in the packages, without
loss, until an up-to-date binary delivers them.

## The backup copy

The archive lives on the projects disk: it survives the death of a source-clip
disk, not that of the internal disk. A daily copy therefore goes elsewhere, to
a folder that a sync client (OneDrive) carries off the machine; the engine
talks to no service there, it writes a file.

The folder is set once in `~/.config/cutmachine/.env` (or in the environment,
which takes precedence):

```sh
CUTMACHINE_ARCHIVE_BACKUP_DIR='/Users/…/CloudStorage/OneDrive-…/Sauvegardes/CUTMACHINE'
```

An absolute path, to a folder that exists: it is never created (a missing
folder is often a sync client that is not mounted). With no setting, or `off`,
no copy; `--check-setup` says so (element `archive_backup`, optional) without
failing.

- **When.** After a delivery that archived at least one snapshot (a save, the
  recovery of a pending snapshot), at most once a day, local date. And on
  demand: `cutmachine --backup-project-archive`, or `backup_project_archive`
  through the MCP, with no open project, which rewrites the day's copy.
- **What.** `CUTMACHINE_projects_<AAAAMMJJ>.sqlite`, next to the Resolve
  database backups of the same folder: a whole database, schema 3, openable by
  the engine like the original (`--query-project-archive` on the copy). It is
  made by `VACUUM INTO` into a temporary file of the folder
  (`.CUTMACHINE_projects_<AAAAMMJJ>.sqlite.tmp`), synced to disk, checked
  (`PRAGMA quick_check`), then renamed: the dated name never designates a
  half-written database, and the copy is a single state of the archive even if
  agents write while it is being made. The temporary file serves as a lock:
  another process that is copying skips its turn; a temporary file more than an
  hour old, left by a dead process, is replaced.
- **How many.** The seven most recent dated copies; the oldest are removed after
  a successful copy. No other file in the folder is touched, nor a copy dated
  after today. About 20 MB each on a real archive; the copy itself takes
  0.07 s.
- **What it carries.** The structure of the edits, the media paths and the
  texts placed; no picture or sound.

A copy that fails never makes a save fail: the refusal is written to stderr and
in the status's `backup` (`configured`, `directory`, `latest`, `due`,
`performed`, `path`, `removed`, `error`, `detail`).

## Planned next

DB1 and DB2 cover explicit capture, automatic saves and the import of existing
projects. DB3 links the snapshots to the step receipts, plans and journals; any
MCP client can use it to [resume without double editing](orchestration-montage.md).
Schema 3 (DEC44) stores each piece once and copies the archive every day off the
disk. General indexing of the analyses stays in the tracks; the pruning proposed
by DB3 is only an option, if the database grows again.
