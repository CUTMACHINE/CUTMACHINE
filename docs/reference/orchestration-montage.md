# Durable steps and agent measurements

DB3, OR1–OR3 — `DONNEES-2026-10`.

The engine can record an editing step with its exact plan, its journals and
an SQL snapshot, then find it again after an interruption without editing
again. Any MCP client can use it: an agent driving the engine directly, the
app's Agent panel, or a future driver. The panel also writes an ACP trace
that can be measured afterwards.

The external LangGraph driver that used these receipts was removed (decision
and reasons in
`DONNEES-2026-10`).
Its last state, graph v3 included, remains in the Git history at commit
`1f60686`.

## What prevents a double edit

The client first prepares its exact plan (`prepare_edit_plan`, then
`get_edit_plan`) and keeps its request. The engine then commits, in its usual
multi-file transaction, the project, the journals, a
`.cutmachine-run-step-<ULID>.json` receipt and the pending snapshot with its
context. The receipt keeps the request, the before/after revisions, the exact
plan, the timeline journal and the project journal.

`commit_run_step` takes `request_json`, a string containing `run_id`,
`step_id`, `stage`, `timeline_id`, `base_revision`, `base_history_hash`,
`base_project_history_hash`, `plan_json` and `observations_json`. The IDs are
full ULIDs; the three hashes are the engine's. `get_run_step` takes
`step_id`. A missing receipt also returns `history_hashes` per timeline and
`project_history_hash` to prepare the request. A present receipt returns
`receipt_json`, `revision` and `matches_current`. A receipt whose timeline
has been deleted can still be read, with `matches_current` false.

If the commit succeeded but the response was lost, the same request finds the
receipt again. It edits nothing. A different request under the same ID is
refused with `RunStepIdReused`. A different revision or journal refuses with
`RunStateChanged` or `StaleRevision`, even if an undo has brought the
document back to the same bytes. The hashes distinguish states; they are not
a cryptographic authentication. Replaying the receipt also compares the
exact bytes of the two journals.

The CLI exposes `--commit-run-step <projet> '<demande-json>'` and
`--get-run-step <projet> <ULID>`. The in-memory backend refuses the commit: a
resume requires the saved package, not a cache of session responses. In the
app, the Agent panel reads the receipt's own before/after revisions: a human
edit that happened before the response does not make it attribute to the
agent a change its step did not produce, and a step with no plan adds no edit
line.

## Captions prepared in a plan

`generate_captions` is prepared like the other edits: the backend projects
the words from the caches onto the working copy, after the steps that precede
it in the same plan. A reading of the edited-in sound is only used if its
identity matches this document. Preparing the captions changes neither the
project nor the journals; applying the plan adds a single undoable entry.
`generate_captions` with `preview:true` returns the title cards without
editing anything.

## Snapshots and SQL queries

The native archive has moved to schema 2 for receipts, then to schema 3
(DEC44, [SQL archive](archive-projets.md#migration-to-schema-3)):
snapshots are stored there by hash, `project_snapshots` is a view that
returns each document byte for byte, and `project_run_steps` keeps its rows,
linked to `archive_snapshots`. A writer migrates a v1 or v2 database; its old
snapshots are read back to the byte. A reader can consult v1 or v2 without
modifying it. An older version of the engine then refuses v3.

`project_run_steps` links `step_id`, `run_id`, `stage`, `project_id`,
`timeline_id`, `snapshot_id`, `base_revision`, `result_revision`,
`receipt_json`. The snapshot and this row share the SQL commit. Triggers
refuse replacements, modifications and deletions. In the package,
`.cutmachine-archive-<ULID>.context.json` accompanies the pending JSON. An
SQL refusal leaves the edit and the receipt acquired; opening/synchronisation
retry delivering both. `CUTMACHINE_PROJECT_ARCHIVE=off` keeps the receipts in
the package but creates no SQL snapshot.

```sql
SELECT r.run_id, r.stage, s.created_at, r.result_revision,
       json_extract(r.receipt_json, '$.request_json') AS request_json,
       s.document_json
FROM project_run_steps r
JOIN project_snapshots s USING(snapshot_id)
ORDER BY r.run_id, s.created_at, s.snapshot_id;
```

The intentions and the context are JSON in `request_json`; the observations
are in turn a JSON string. The database contains no copy of the media. The
growth of the archive is handled by schema 3 (DEC44); what remains open is
the complete indexing of analyses and the links with edits made outside
durable steps.

## Measuring an Agent panel session

ACP (Agent Client Protocol) is the protocol by which the Agent panel hands
instructions to Claude Code, Codex or another agent, which then calls the
engine over MCP. The panel writes its traces under
`~/Library/Logs/CUTMACHINE/agent/`.

```sh
python3 tools/acp-report/acp_report.py --trace /chemin/horodatage.acp.jsonl
```

The tool only requires Python 3.9 or later, with no engine and no dependency.
It reads the trace without modifying it and invents no measurement that an
agent did not provide.

The summary counts **turns**, not the model calls internal to the agent. Its
coverage remains that of the file (`trace_only`). A response with no kept
request keeps its counters, with an unknown duration; a tool identifier
reused in a new turn counts as a new call. Context-occupancy notifications
are not billed usage. Counters that are absent, negative or invalid do not
count as zero tokens: `known_totals` keeps the known counters,
`reported_by_field` says how many responses carry each field,
`lower_bound_fields` names those that are missing from some of the responses,
and `totals_are_lower_bounds` and `duration_is_lower_bound` flag incomplete
summaries. The names of the counters remain those of the provider or of ACP,
cache categories included; numeric subfields appear with their path
separated by a dot. Some fields include other counters: do not add them
together. Durations are inclusive and may overlap. `api_cost` remains
unknown and `acceptance` not recorded.

## Verification

`cutmachine_run_step_tests` is portable; `cutmachine_run_step_storage_tests`
exercises the receipts, the archive and the package's transaction protocol.
`cutmachine_mcp_tools_tests` covers the receipt MCP tools;
`cutmachine_mcp_endpoint_tests` the events that a receipt passes on to the
Agent panel (observation step, edited step, replay); `cutmachine_mcp_tests`
the preparation of captions in a plan. `cutmachine_acp_report_tests`, which is
portable, reads real, incomplete ACP traces and goes through the command
line. No test replays a durable step through the app window.

```sh
ctest --test-dir build --output-on-failure -R 'run_step|acp_report|mcp_'
```
