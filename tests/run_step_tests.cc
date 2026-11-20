// OR2 -- ROADMAP.md. An exact plan remains reversible when the graph commits
// it as a durable stage; refusals leave both project and journal untouched.

#include "CanonicalHash.h"
#include "RunStep.h"

#include <exception>
#include <iostream>
#include <string>

#include "TestSupport.h"

namespace {}  // namespace

int Run() {
    Project project("Run boundary");
    const Ulid timeline = project.active_timeline_id;
    const std::string original = project.SaveToString();
    EditLog log;
    ProjectEditLog projectLog;
    std::string message, code;
    EditError error = EditError::None;
    EditPlanDraft draft(project.MakeDocument(timeline));
    DocumentMarker marker;
    marker.name = "Choix éditorial";
    marker.time = {0, 25};
    Check(draft.Apply(AddMarkerOperation{marker}, error, message), message);
    EditPlan plan;
    Check(FinishEditPlan(draft, "Selection", mcp_json::Value::MakeArray(), plan,
                         code, message),
          message);
    RunStepRequest request;
    request.run_id = "01K30000000000000000000001";
    request.step_id = "01K30000000000000000000002";
    request.stage = "selection";
    request.timeline_id = timeline;
    request.base_revision = CanonicalHash(original);
    request.base_history_hash = CanonicalHash(log.Serialize());
    request.base_project_history_hash = CanonicalHash(projectLog.Serialize());
    request.plan_json = SerializeEditPlan(plan);
    RunStepRequest decoded;
    const std::string bytes = SerializeRunStepRequest(request);
    Check(ParseRunStepRequest(bytes, decoded, message) &&
              SerializeRunStepRequest(decoded) == bytes,
          "canonical run request round trip");
    Check(!ParseRunStepRequest(
              bytes.substr(0, bytes.size() - 1) + ",\"surprise\":\"value\"}",
              decoded, message),
          "unknown request fields refused");
    Check(ApplyRunStep(project, log, request, code, message), message);
    Check(log.AppliedCount() == 1 &&
              project.MakeDocument(timeline).SaveToString() ==
                  draft.Current().SaveToString(),
          "one history entry with the exact prepared identities");
    const std::string committed = project.SaveToString();
    Check(!ApplyRunStep(project, log, request, code, message) &&
              code == "StaleRevision" && project.SaveToString() == committed &&
              log.AppliedCount() == 1,
          "stale application has no effects");
    Document document = project.MakeDocument(timeline);
    Check(log.Undo(document, error, message) &&
              project.CommitDocument(timeline, document, message) &&
              project.SaveToString() == original,
          "undo restores the complete canonical project byte for byte");
    Check(!ApplyRunStep(project, log, request, code, message) &&
              code == "RunStateChanged" && project.SaveToString() == original,
          "same project with a changed undo cursor is refused");

    // Pin the added package record independently of random fixture IDs.
    request.timeline_id = "01K30000000000000000000003";
    request.base_revision = "0000000000000000";
    request.base_history_hash = "1111111111111111";
    request.base_project_history_hash = "2222222222222222";
    request.plan_json.clear();
    Check(
        SerializeRunStepRequest(request) ==
            R"({"run_id":"01K30000000000000000000001","step_id":"01K30000000000000000000002","stage":"selection","timeline_id":"01K30000000000000000000003","base_revision":"0000000000000000","base_history_hash":"1111111111111111","base_project_history_hash":"2222222222222222","plan_json":"","observations_json":"{}"})",
        "canonical request field order and bytes pinned");
    request.observations_json = "[]";
    Check(!ValidateRunStepRequest(request, message),
          "observations must be an object");
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
