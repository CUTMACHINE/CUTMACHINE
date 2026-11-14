#include "RunStepStorage.h"

#include "CanonicalHash.h"
#include "Cli.h"
#include "Json.h"
#include "ProjectStorage.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>

namespace {

namespace fs = std::filesystem;
using mcp_json::Value;

bool Refuse(const std::string& code, const std::string& detail,
            std::string& errorName, std::string& message) {
    errorName = code;
    message = detail;
    return false;
}

bool ReadReceipt(const std::string& projectPath, const Ulid& stepId,
                 RunStepReceipt& receipt, bool& found, std::string& errorName,
                 std::string& message) {
    if (!IsValidUlid(stepId)) {
        return Refuse("ValidationFailed", "step_id must be a full ULID",
                      errorName, message);
    }
    const fs::path path = fs::path(projectPath).parent_path() /
                          (".cutmachine-run-step-" + stepId + ".json");
    std::error_code error;
    const auto status = fs::symlink_status(path, error);
    if (status.type() == fs::file_type::not_found &&
        (!error || error == std::errc::no_such_file_or_directory)) {
        found = false;
        return true;
    }
    if (error || !fs::is_regular_file(status) ||
        fs::file_size(path, error) > kMaxRunStepBytes || error) {
        return Refuse("IoError", "run receipt must be a bounded regular file",
                      errorName, message);
    }
    std::ifstream input(path, std::ios::binary);
    std::ostringstream bytes;
    bytes << input.rdbuf();
    if (!input || !ParseRunStepReceipt(bytes.str(), receipt, message) ||
        receipt.step_id != stepId) {
        return Refuse("ValidationFailed", "invalid run receipt: " + message,
                      errorName, message);
    }
    found = true;
    return true;
}

bool ReadLogs(const std::string& path, const Ulid& timelineId, EditLog& log,
              ProjectEditLog& projectLog, std::string& errorName,
              std::string& message) {
    EditError error = EditError::None;
    if (!EditLog::Load(TimelineEditLogPathForProject(path, timelineId), log,
                       error, message) ||
        !ProjectEditLog::Load(ProjectEditLogPathForProject(path), projectLog,
                              error, message)) {
        errorName = "IoError";
        return false;
    }
    return true;
}

std::string Answer(const RunStepReceipt& receipt, bool replayed, bool matches,
                   const Project& project) {
    Value result = Value::MakeObject();
    result.Set("ok", Value::MakeBool(true));
    result.Set("found", Value::MakeBool(true));
    result.Set("replayed", Value::MakeBool(replayed));
    result.Set("matches_current", Value::MakeBool(matches));
    result.Set("revision",
               Value::MakeString(CanonicalHash(project.SaveToString())));
    result.Set("receipt_json",
               Value::MakeString(SerializeRunStepReceipt(receipt)));
    result.Set("run_id", Value::MakeString(receipt.run_id));
    result.Set("step_id", Value::MakeString(receipt.step_id));
    result.Set("snapshot_id", Value::MakeString(receipt.snapshot_id));
    // OR2 -- the window's journal must observe this step's own boundary,
    // even when its preceding agent call carried no leading revision.
    result.Set("timeline_id", Value::MakeString(receipt.timeline_id));
    result.Set("base_revision", Value::MakeString(receipt.base_revision));
    result.Set("result_revision", Value::MakeString(receipt.result_revision));
    RunStepRequest request;
    EditPlan plan;
    Value preview;
    std::string ignored;
    if (ParseRunStepRequest(receipt.request_json, request, ignored) &&
        !request.plan_json.empty() &&
        ParseEditPlan(request.plan_json, plan, ignored) &&
        Value::Parse(plan.preview_json, preview, ignored)) {
        result.Set("label", Value::MakeString(plan.label));
        if (const auto* delta = preview.Find("delta")) {
            result.Set("delta", *delta);
        }
    }
    return result.Dump();
}

}  // namespace

bool CommitStoredRunStep(const std::string& projectPath,
                         const RunStepRequest& request, Project& committed,
                         EditLog& timelineLog, std::string& resultJson,
                         std::string& errorName, std::string& message,
                         const Project* liveProject, const EditLog* liveLog,
                         const ProjectEditLog* liveProjectLog) {
    errorName.clear();
    message.clear();
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    RunStepRequest checked;
    if (!ParseRunStepRequest(SerializeRunStepRequest(request), checked,
                             message)) {
        errorName = "ValidationFailed";
        return false;
    }
    ProjectSessionLock lock;
    Project project;
    if (!AcquireStoredProjectLock(projectPath, lock, errorName, message) ||
        !LoadStoredProject(projectPath, project, message, false)) {
        if (errorName.empty()) {
            errorName = "IoError";
        }
        return false;
    }
    if (liveProject != nullptr &&
        liveProject->SaveToString() != project.SaveToString()) {
        return Refuse("StaleDocument",
                      "the live project differs from its "
                      "saved state; save it before committing a run step",
                      errorName, message);
    }
    if (project.FindTimeline(checked.timeline_id) == nullptr) {
        return Refuse("UnknownSequence", "the run timeline does not exist",
                      errorName, message);
    }
    EditLog log;
    ProjectEditLog projectLog;
    if (!ReadLogs(projectPath, checked.timeline_id, log, projectLog, errorName,
                  message)) {
        return false;
    }
    if ((liveLog != nullptr && liveLog->Serialize() != log.Serialize()) ||
        (liveProjectLog != nullptr &&
         liveProjectLog->Serialize() != projectLog.Serialize())) {
        return Refuse("StaleDocument",
                      "the live journals differ from their "
                      "saved state",
                      errorName, message);
    }
    RunStepReceipt receipt;
    bool found = false;
    if (!ReadReceipt(projectPath, checked.step_id, receipt, found, errorName,
                     message)) {
        return false;
    }
    if (found) {
        if (receipt.request_json != SerializeRunStepRequest(checked)) {
            return Refuse("RunStepIdReused",
                          "step_id already names another "
                          "request",
                          errorName, message);
        }
        if (!RunStepMatchesState(receipt, project, log, projectLog)) {
            return Refuse("RunStateChanged",
                          "the project or its journals "
                          "changed after this step; do not replay its edit",
                          errorName, message);
        }
        committed = std::move(project);
        timelineLog = std::move(log);
        resultJson = Answer(receipt, true, true, committed);
        return true;
    }
    const Project before = project;
    if (CanonicalHash(projectLog.Serialize()) !=
        checked.base_project_history_hash) {
        return Refuse("RunStateChanged",
                      "the project journal changed before "
                      "this step",
                      errorName, message);
    }
    receipt.base_history_hash = CanonicalHash(log.Serialize());
    if (!ApplyRunStep(project, log, checked, errorName, message)) {
        return false;
    }
    receipt.run_id = checked.run_id;
    receipt.step_id = checked.step_id;
    receipt.stage = checked.stage;
    receipt.project_id = project.id;
    receipt.timeline_id = checked.timeline_id;
    receipt.snapshot_id = GenerateUlid();
    receipt.base_revision = checked.base_revision;
    receipt.result_revision = CanonicalHash(project.SaveToString());
    receipt.timeline_history_json = log.Serialize();
    receipt.project_history_json = projectLog.Serialize();
    receipt.request_json = SerializeRunStepRequest(checked);
    if (!CommitStoredProjectAndLogs(projectPath, project,
                                    {{checked.timeline_id, log}}, projectLog,
                                    message, &before, true, &receipt)) {
        errorName = "IoError";
        return false;
    }
    committed = std::move(project);
    timelineLog = std::move(log);
    resultJson = Answer(receipt, false, true, committed);
    return true;
}

bool ReadStoredRunStep(const std::string& projectPath, const Ulid& stepId,
                       std::string& resultJson, std::string& errorName,
                       std::string& message) {
    errorName.clear();
    message.clear();
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    ProjectSessionLock lock;
    Project project;
    if (!IsValidUlid(stepId)) {
        return Refuse("ValidationFailed", "step_id must be a full ULID",
                      errorName, message);
    }
    if (!AcquireStoredProjectLock(projectPath, lock, errorName, message) ||
        !LoadStoredProject(projectPath, project, message, false)) {
        if (errorName.empty()) {
            errorName = "IoError";
        }
        return false;
    }
    RunStepReceipt receipt;
    bool found = false;
    if (!ReadReceipt(projectPath, stepId, receipt, found, errorName, message)) {
        return false;
    }
    if (!found) {
        Value result = Value::MakeObject();
        result.Set("ok", Value::MakeBool(true));
        result.Set("found", Value::MakeBool(false));
        result.Set("revision",
                   Value::MakeString(CanonicalHash(project.SaveToString())));
        Value histories = Value::MakeObject();
        ProjectEditLog projectLog;
        for (const auto& timeline : project.timelines) {
            EditLog log;
            if (!ReadLogs(projectPath, timeline.id, log, projectLog, errorName,
                          message)) {
                return false;
            }
            histories.Set(timeline.id,
                          Value::MakeString(CanonicalHash(log.Serialize())));
        }
        result.Set("history_hashes", std::move(histories));
        result.Set("project_history_hash",
                   Value::MakeString(CanonicalHash(projectLog.Serialize())));
        resultJson = result.Dump();
        return true;
    }
    // FI16 -- docs/roadmap/FIABILITE-2026-10.md. A receipt outlives its
    // timeline: the timeline's journal went with it, and reading that
    // journal turned a kept receipt into an IoError. The step is read, no
    // longer matching the project.
    if (project.FindTimeline(receipt.timeline_id) == nullptr) {
        resultJson = Answer(receipt, false, false, project);
        return true;
    }
    EditLog log;
    ProjectEditLog projectLog;
    if (!ReadLogs(projectPath, receipt.timeline_id, log, projectLog, errorName,
                  message)) {
        return false;
    }
    resultJson =
        Answer(receipt, false,
               RunStepMatchesState(receipt, project, log, projectLog), project);
    return true;
}
