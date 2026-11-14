#pragma once

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The project archive's schemas 1
// and 2, frozen as the engine wrote them before schema 3: the engine only
// ever writes schema 3 now, and a migration is proved on archives it can no
// longer make. Copied from ProjectArchive.cc at main 490f481; never edit
// them to follow the engine.

namespace archive_fixtures {

// DB1 -- one table, the whole JSON per capture.
inline constexpr const char* kSchemaOne =
    "CREATE TABLE project_snapshots ("
    "snapshot_id TEXT PRIMARY KEY NOT NULL,"
    "project_id TEXT NOT NULL,"
    "revision_hash TEXT NOT NULL,"
    "created_at TEXT NOT NULL DEFAULT "
    "(strftime('%Y-%m-%dT%H:%M:%fZ','now')),"
    "run_id TEXT NOT NULL, step TEXT NOT NULL,"
    "document_json TEXT NOT NULL "
    "CHECK(json_valid(document_json))"
    ");"
    "CREATE INDEX snapshots_by_project ON project_snapshots"
    "(project_id,created_at,snapshot_id);"
    "CREATE INDEX snapshots_by_run ON project_snapshots"
    "(run_id,step);"
    "CREATE TRIGGER snapshots_no_replace BEFORE INSERT ON "
    "project_snapshots WHEN EXISTS(SELECT 1 FROM "
    "project_snapshots "
    "WHERE snapshot_id=NEW.snapshot_id) BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;"
    "CREATE TRIGGER snapshots_no_update BEFORE UPDATE ON "
    "project_snapshots BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;"
    "CREATE TRIGGER snapshots_no_delete BEFORE DELETE ON "
    "project_snapshots BEGIN SELECT "
    "RAISE(ABORT,'project snapshots are immutable'); END;"
    "PRAGMA application_id=1129137218; PRAGMA user_version=1;";

// DB3 -- schema 2 adds the run receipts, linked to their capture.
inline constexpr const char* kSchemaOneToTwo =
    "CREATE TABLE project_run_steps ("
    "step_id TEXT PRIMARY KEY NOT NULL, run_id TEXT NOT NULL,"
    "stage TEXT NOT NULL, project_id TEXT NOT NULL,"
    "timeline_id TEXT NOT NULL, snapshot_id TEXT NOT NULL "
    "UNIQUE "
    "REFERENCES project_snapshots(snapshot_id),"
    "base_revision TEXT NOT NULL, result_revision TEXT NOT NULL,"
    "receipt_json TEXT NOT NULL "
    "CHECK(json_valid(receipt_json)));"
    "CREATE INDEX run_steps_by_run ON project_run_steps"
    "(run_id,stage,step_id);"
    "CREATE TRIGGER run_steps_no_replace BEFORE INSERT ON "
    "project_run_steps WHEN EXISTS(SELECT 1 FROM "
    "project_run_steps "
    "WHERE step_id=NEW.step_id) BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_update BEFORE UPDATE ON "
    "project_run_steps BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "CREATE TRIGGER run_steps_no_delete BEFORE DELETE ON "
    "project_run_steps BEGIN SELECT "
    "RAISE(ABORT,'run steps are immutable'); END;"
    "PRAGMA user_version=2;";

}  // namespace archive_fixtures
