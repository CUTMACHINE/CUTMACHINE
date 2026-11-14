#include "RunStep.h"

#include "CanonicalHash.h"
#include "Json.h"

#include <algorithm>
#include <array>
#include <string>

namespace {

using mcp_json::Value;

bool Validate(const RunStepRequest& request, std::string& message) {
    constexpr std::array<const char*, 6> kStages = {"analysis", "selection",
                                                    "assembly", "finishing",
                                                    "control",  "delivery"};
    if (!IsValidUlid(request.run_id) || !IsValidUlid(request.step_id) ||
        !IsValidUlid(request.timeline_id) ||
        std::find(kStages.begin(), kStages.end(), request.stage) ==
            kStages.end() ||
        request.base_history_hash.size() != 16 ||
        request.base_history_hash.find_first_not_of("0123456789abcdef") !=
            std::string::npos ||
        request.base_project_history_hash.size() != 16 ||
        request.base_project_history_hash.find_first_not_of(
            "0123456789abcdef") != std::string::npos ||
        request.base_revision.size() != 16 ||
        request.base_revision.find_first_not_of("0123456789abcdef") !=
            std::string::npos) {
        message =
            "run_id, step_id and timeline_id must be full ULIDs; stage "
            "must name one of the six montage stages; base_revision "
            "must be the project's canonical hash";
        return false;
    }
    Value observations;
    if (request.observations_json.size() > size_t{1024} * 1024 ||
        !Value::Parse(request.observations_json, observations, message) ||
        !observations.IsObject()) {
        message = "observations_json must hold a JSON object of at most 1 MiB";
        return false;
    }
    if (!request.plan_json.empty()) {
        EditPlan plan;
        if (!ParseEditPlan(request.plan_json, plan, message) ||
            plan.timeline_id != request.timeline_id) {
            message =
                "plan_json must hold a serialized edit plan for the "
                "step's timeline: " +
                message;
            return false;
        }
    }
    return true;
}

}  // namespace

bool ValidateRunStepRequest(const RunStepRequest& request,
                            std::string& message) {
    return Validate(request, message);
}

bool ApplyRunStep(Project& project, EditLog& log, const RunStepRequest& request,
                  std::string& errorName, std::string& message) {
    if (!Validate(request, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    if (CanonicalHash(project.SaveToString()) != request.base_revision) {
        errorName = "StaleRevision";
        message = "the project changed before this run step; observe it again";
        return false;
    }
    if (CanonicalHash(log.Serialize()) != request.base_history_hash) {
        errorName = "RunStateChanged";
        message = "the timeline journal changed before this run step";
        return false;
    }
    if (project.FindTimeline(request.timeline_id) == nullptr) {
        errorName = "UnknownSequence";
        message = "the run step's timeline does not exist";
        return false;
    }
    if (request.plan_json.empty()) {
        return true;
    }
    EditPlan plan;
    if (!ParseEditPlan(request.plan_json, plan, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    Document document = project.MakeDocument(request.timeline_id);
    EditLog stagedLog = log;
    Project stagedProject = project;
    if (!ApplyEditPlan(document, stagedLog, plan, errorName, message)) {
        return false;
    }
    if (!stagedProject.CommitDocument(request.timeline_id, document, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    project = std::move(stagedProject);
    log = std::move(stagedLog);
    return true;
}

bool RunStepMatchesState(const RunStepReceipt& receipt, const Project& project,
                         const EditLog& log, const ProjectEditLog& projectLog) {
    return project.id == receipt.project_id &&
           project.FindTimeline(receipt.timeline_id) != nullptr &&
           CanonicalHash(project.SaveToString()) == receipt.result_revision &&
           log.Serialize() == receipt.timeline_history_json &&
           projectLog.Serialize() == receipt.project_history_json;
}
