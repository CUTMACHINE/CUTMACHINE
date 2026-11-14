// DB3, OR2 -- ROADMAP.md. A lost reply and a missing SQL service cannot
// duplicate an edit; the package remains the durable replay boundary.

#include "CanonicalHash.h"
#include "Cli.h"
#include "McpLiveBackend.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "ProjectArchive.h"
#include "ProjectStorage.h"
#include "RunStepStorage.h"
#include "archive_schema_fixtures.h"
#include "sqlite3.h"

#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
namespace fs = std::filesystem;
using mcp_json::Value;
int failures = 0;
void Check(bool condition, const std::string& reason) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << reason << '\n';
    }
}
void Write(const fs::path& path, const std::string& bytes) {
    std::ofstream output(path, std::ios::binary);
    output << bytes;
}
std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}
Value Json(const std::string& bytes) {
    Value value;
    std::string error;
    Check(Value::Parse(bytes, value, error), error);
    return value;
}
std::string Hex(const std::string& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (unsigned char byte : bytes) {
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}
}  // namespace

int Run() {
    const fs::path root =
        fs::temp_directory_path() / ("cutmachine-runs-" + GenerateUlid());
    fs::create_directories(root);
    setenv("CUTMACHINE_PROJECT_ARCHIVE", "off", 1);
    Project project("Durable montage");
    std::string path, detail, error, result;
    Check(
        CreatePortableProject((root / "film").string(), project, path, detail),
        detail);
    const std::string original = Read(path);
    EditLog log;
    ProjectEditLog projectLog;
    EditError editError = EditError::None;
    EditPlanDraft draft(project.MakeActiveDocument());
    DocumentMarker marker;
    marker.name = "Selected";
    marker.time = {0, 25};
    Check(draft.Apply(AddMarkerOperation{marker}, editError, detail), detail);
    EditPlan plan;
    Check(FinishEditPlan(draft, "Selection", Value::MakeArray(), plan, error,
                         detail),
          detail);
    RunStepRequest request;
    request.run_id = GenerateUlid();
    request.step_id = GenerateUlid();
    request.stage = "selection";
    request.timeline_id = project.active_timeline_id;
    request.base_revision = CanonicalHash(original);
    request.base_history_hash = CanonicalHash(log.Serialize());
    request.base_project_history_hash = CanonicalHash(projectLog.Serialize());
    request.plan_json = SerializeEditPlan(plan);

    // A v1 archive must retain its old bytes while the next capture migrates
    // (DEC44: through schema 2 to schema 3, in one go).
    const std::string database = (root / "projects.sqlite").string();
    const std::string firstCapture = GenerateUlid();
    const std::string captured = project.SaveToString();
    sqlite3* raw = nullptr;
    Check(sqlite3_open(database.c_str(), &raw) == SQLITE_OK,
          "open SQL fixture");
    sqlite3_stmt* insert = nullptr;
    Check(sqlite3_exec(raw, archive_fixtures::kSchemaOne, nullptr, nullptr,
                       nullptr) == SQLITE_OK &&
              sqlite3_prepare_v2(raw,
                                 "INSERT INTO project_snapshots(snapshot_id,"
                                 "project_id,revision_hash,run_id,step,"
                                 "document_json) VALUES(?,?,?,'','',?)",
                                 -1, &insert, nullptr) == SQLITE_OK,
          "create v1 fixture");
    sqlite3_bind_text(insert, 1, firstCapture.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 2, project.id.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 3, CanonicalHash(captured).c_str(), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(insert, 4, captured.c_str(), -1, SQLITE_TRANSIENT);
    Check(sqlite3_step(insert) == SQLITE_DONE, "v1 fixture holds a capture");
    sqlite3_finalize(insert);
    sqlite3_close(raw);
    const fs::path unavailable = root / "unavailable.sqlite";
    fs::create_directory(unavailable);
    setenv("CUTMACHINE_PROJECT_ARCHIVE", unavailable.c_str(), 1);
    Project committed;
    Check(CommitStoredRunStep(path, request, committed, log, result, error,
                              detail),
          detail);
    const std::string edited = Read(path);
    const Value first = Json(result);
    Check(
        first.Find("timeline_id") != nullptr &&
            first.Find("timeline_id")->AsString() == request.timeline_id &&
            first.Find("base_revision") != nullptr &&
            first.Find("base_revision")->AsString() == request.base_revision &&
            first.Find("result_revision") != nullptr &&
            first.Find("result_revision")->AsString() ==
                CanonicalHash(edited) &&
            first.Find("label") != nullptr &&
            first.Find("label")->AsString() == plan.label &&
            first.Find("delta") != nullptr,
        "the saved step exposes its own before state and reviewed effect");
    const std::string snapshot = first.Find("snapshot_id")->AsString();
    const fs::path receiptPath =
        root / "film" / (".cutmachine-run-step-" + request.step_id + ".json");
    const fs::path pending =
        root / "film" / (".cutmachine-archive-" + snapshot + ".json");
    const fs::path context =
        root / "film" / (".cutmachine-archive-" + snapshot + ".context.json");
    Check(edited != original && log.AppliedCount() == 1 &&
              fs::exists(receiptPath) && fs::exists(pending) &&
              fs::exists(context),
          "edit, journal, receipt and SQL outbox published together");
    RunStepReceipt receipt;
    Check(ParseRunStepReceipt(Read(receiptPath), receipt, detail) &&
              SerializeRunStepReceipt(receipt) == Read(receiptPath),
          "receipt canonical round trip");
    // Recreate both registry and backend, as happens on MCP server restart.
    McpProjectBackend backend(path);
    McpToolRegistry registry;
    Value args = Value::MakeObject();
    args.Set("request_json",
             Value::MakeString(SerializeRunStepRequest(request)));
    const auto replay = registry.Call(backend, "commit_run_step", args);
    Check(replay.ok && Json(replay.result_json).Find("replayed")->AsBool() &&
              Read(path) == edited,
          "new server recognizes committed edit without replay");
    std::string output;
    Check(RunStepCommand({path, request.step_id}, false, output) == 0 &&
              Json(output).Find("matches_current")->AsBool(),
          "CLI reads saved receipt");
    Document liveDocument = committed.MakeActiveDocument();
    EditLog liveLog = log;
    McpLiveBackend live(liveDocument, liveLog);
    live.SetRunStepMatchCallback([&](const RunStepReceipt& saved) {
        Project observed = committed;
        std::string message;
        if (!observed.CommitActiveDocument(liveDocument, message)) {
            return false;
        }
        return RunStepMatchesState(saved, observed, liveLog, projectLog);
    });
    std::string liveAnswer = output;
    Check(live.ValidateRunStepState(liveAnswer, detail) &&
              Json(liveAnswer).Find("matches_current")->AsBool(),
          "live state matches saved receipt");
    DocumentMarker unsavedMarker;
    unsavedMarker.name = "Unsaved intervention";
    unsavedMarker.time = {0, 25};
    Check(liveLog.Apply(liveDocument, AddMarkerOperation{unsavedMarker},
                        editError, detail) &&
              liveLog.Undo(liveDocument, editError, detail),
          detail);
    liveAnswer = output;
    Check(live.ValidateRunStepState(liveAnswer, detail) &&
              !Json(liveAnswer).Find("matches_current")->AsBool(),
          "unsaved live journal divergence also stops reconciliation");
    RunStepRequest changed = request;
    changed.observations_json = R"({"note":"different"})";
    Check(!CommitStoredRunStep(path, changed, committed, log, result, error,
                               detail) &&
              error == "RunStepIdReused" && Read(path) == edited,
          "reused step ID with another request refused");

    const std::string validContext = Read(context);
    fs::remove(context);
    fs::create_symlink(root / "missing", context);
    setenv("CUTMACHINE_PROJECT_ARCHIVE", database.c_str(), 1);
    Check(
        StoredProjectArchiveStatus(path, true).pending == 1 &&
            fs::exists(pending),
        "dangling run context symlink cannot silently become an ordinary save");
    fs::remove(context);
    Write(context, validContext);
    Check(StoredProjectArchiveStatus(path, true).pending == 0 &&
              !fs::exists(context),
          "SQL delivery catches up after repair");
    Value rows;
    Check(
        project_archive::Query(
            database,
            "SELECT s.document_json,r.receipt_json FROM project_snapshots s "
            "JOIN project_run_steps r USING(snapshot_id) WHERE "
            "r.stage='selection'",
            10, rows, error, detail) &&
            rows.Find("rows")->AsArray().size() == 1 &&
            rows.Find("rows")->AsArray()[0].AsArray()[0].AsString() == edited &&
            rows.Find("rows")->AsArray()[0].AsArray()[1].AsString() ==
                Read(receiptPath),
        "v1 migrates and joins exact project with run, stage, plan and "
        "journals");
    Check(
        project_archive::Query(database,
                               "SELECT document_json FROM project_snapshots "
                               "WHERE snapshot_id='" +
                                   firstCapture + "'",
                               1, rows, error, detail) &&
            rows.Find("rows")->AsArray().size() == 1 &&
            rows.Find("rows")->AsArray()[0].AsArray()[0].AsString() == captured,
        "the v1 capture reads back with its old bytes: " + detail);
    Check(sqlite3_open(database.c_str(), &raw) == SQLITE_OK,
          "open migrated archive");
    Check(sqlite3_exec(raw, "DELETE FROM project_run_steps", nullptr, nullptr,
                       nullptr) != SQLITE_OK &&
              sqlite3_exec(raw,
                           "INSERT OR REPLACE INTO project_run_steps SELECT * "
                           "FROM project_run_steps",
                           nullptr, nullptr, nullptr) != SQLITE_OK,
          "run records immutable to direct SQL clients");
    sqlite3_close(raw);

    // Undo returns to the prior project, then an extra undone edit changes
    // only the journal. The original step still must never be replayed.
    Document document = committed.MakeActiveDocument();
    DocumentMarker extra;
    extra.name = "Human intervention";
    extra.time = {0, 25};
    Check(log.Apply(document, AddMarkerOperation{extra}, editError, detail) &&
              log.Undo(document, editError, detail) &&
              committed.CommitActiveDocument(document, detail) &&
              committed.SaveToString() == edited,
          "changed history with same project fixture");
    Check(
        CommitStoredProjectAndLogs(
            path, committed, {{request.timeline_id, log}}, projectLog, detail),
        detail);
    Check(ReadStoredRunStep(path, request.step_id, result, error, detail) &&
              !Json(result).Find("matches_current")->AsBool(),
          "journal divergence exposed");
    Check(!CommitStoredRunStep(path, request, committed, log, result, error,
                               detail) &&
              error == "RunStateChanged" && Read(path) == edited,
          "same project with a different journal stops durable replay");

    RunStepRequest pendingRequest = request;
    pendingRequest.step_id = GenerateUlid();
    pendingRequest.base_revision = CanonicalHash(edited);
    pendingRequest.plan_json.clear();
    Check(!CommitStoredRunStep(path, pendingRequest, committed, log, result,
                               error, detail) &&
              error == "RunStateChanged" && Read(path) == edited,
          "journal changed before first commit is also refused");
    pendingRequest.base_history_hash = CanonicalHash(log.Serialize());
    Project unsaved = committed;
    Check(
        projectLog.Apply(
            unsaved, AddProjectTimelineOperation{"Unsaved", 320, 180, {25, 1}},
            editError, detail),
        detail);
    Check(!CommitStoredRunStep(path, pendingRequest, committed, log, result,
                               error, detail, &unsaved) &&
              error == "StaleDocument" && Read(path) == edited,
          "unsaved live state is never overwritten");
    projectLog = ProjectEditLog{};
    const fs::path held = root / "held-timelines";
    fs::rename(root / "film/Timelines", held);
    Write(root / "film/Timelines", "blocked save destination");
    Check(!CommitStoredRunStep(path, pendingRequest, committed, log, result,
                               error, detail) &&
              Read(path) == edited &&
              !fs::exists(
                  root / "film" /
                  (".cutmachine-run-step-" + pendingRequest.step_id + ".json")),
          "failed package save publishes no receipt and leaves montage intact");
    fs::remove(root / "film/Timelines");
    fs::rename(held, root / "film/Timelines");

    // Interrupted publication restores the old file and removes every new
    // run artifact before archive delivery, using the native crash protocol.
    setenv("CUTMACHINE_PROJECT_ARCHIVE", "off", 1);
    const std::string nonce = ".cutmachine-" + GenerateUlid();
    const std::string interrupted =
        ".cutmachine-run-step-" + GenerateUlid() + ".json";
    const std::string interruptedPending =
        ".cutmachine-archive-" + GenerateUlid() + ".json";
    const std::string interruptedContext =
        interruptedPending.substr(0, interruptedPending.size() - 5) +
        ".context.json";
    Write(path + nonce + ".bak", edited);
    Write(path, "interrupted project");
    Write(root / "film" / interrupted, Read(receiptPath));
    Write(root / "film" / interruptedPending, original);
    Write(root / "film" / interruptedContext, validContext);
    Write(root / "film/.cutmachine-transaction",
          "CUTMACHINE_TRANSACTION_V1\n" + nonce + "\n1 " +
              Hex("project.cutmachine.json") + "\n0 " + Hex(interrupted) +
              "\n0 " + Hex(interruptedPending) + "\n0 " +
              Hex(interruptedContext) + "\n");
    Project recovered;
    Check(LoadStoredProject(path, recovered, detail) && Read(path) == edited &&
              !fs::exists(root / "film" / interrupted) &&
              !fs::exists(root / "film" / interruptedPending) &&
              !fs::exists(root / "film" / interruptedContext),
          "rollback restores project and removes partial receipt and SQL "
          "context");

    // FI16 -- docs/roadmap/FIABILITE-2026-10.md. A receipt outlives its
    // timeline: once the timeline is removed, with its journal, the step is
    // still read -- found, no longer matching -- and its file is untouched.
    {
        Project lifetime("Run step lifetime");
        ProjectEditLog construction;
        Check(construction.Apply(
                  lifetime,
                  AddProjectTimelineOperation{"Alternate", 1920, 1080, {25, 1}},
                  editError, detail),
              detail);
        std::string lifetimePath;
        Check(CreatePortableProject((root / "lifetime").string(), lifetime,
                                    lifetimePath, detail),
              detail);
        EditLog lifetimeLog;
        ProjectEditLog lifetimeProjectLog;
        RunStepRequest step;
        step.run_id = GenerateUlid();
        step.step_id = GenerateUlid();
        step.stage = "selection";
        step.timeline_id = lifetime.active_timeline_id;
        step.base_revision = CanonicalHash(lifetime.SaveToString());
        step.base_history_hash = CanonicalHash(lifetimeLog.Serialize());
        step.base_project_history_hash =
            CanonicalHash(lifetimeProjectLog.Serialize());
        Project stepped;
        Check(CommitStoredRunStep(lifetimePath, step, stepped, lifetimeLog,
                                  result, error, detail),
              error + ": " + detail);
        const fs::path stepReceipt =
            root / "lifetime" /
            (".cutmachine-run-step-" + step.step_id + ".json");
        const std::string kept = Read(stepReceipt);
        Check(lifetimeProjectLog.Apply(
                  stepped, RemoveProjectTimelineOperation{step.timeline_id},
                  editError, detail) &&
                  CommitStoredProjectAndLogs(lifetimePath, stepped, {},
                                             lifetimeProjectLog, detail),
              "the run's timeline is removed: " + detail);
        const bool read = ReadStoredRunStep(lifetimePath, step.step_id, result,
                                            error, detail);
        const Value answer = read ? Json(result) : Value::MakeObject();
        Check(read && answer.Find("found") != nullptr &&
                  answer.Find("found")->AsBool() &&
                  !answer.Find("matches_current")->AsBool() &&
                  answer.Find("timeline_id")->AsString() == step.timeline_id &&
                  Read(stepReceipt) == kept,
              "a receipt whose timeline was removed is read, no longer "
              "matching: " +
                  error + " " + detail);
    }
    fs::remove_all(root);
    return failures == 0 ? 0 : 1;
}

int main() {
    try {
        return Run();
    } catch (const std::exception& error) {
        std::cerr << "FAIL: unexpected exception: " << error.what() << '\n';
        return 1;
    } catch (...) {
        std::cerr << "FAIL: unexpected exception\n";
        return 1;
    }
}
