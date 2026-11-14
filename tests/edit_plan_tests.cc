// PL2 -- ROADMAP.md (PLAN-2026-09). Edit plans in the engine: prepared on a
// copy without touching the document, applied as previewed -- same bytes,
// same generated identities, one undo step -- and refused when the timeline
// moved. Portable: no ProjectStorage, no FFmpeg, no AppKit.

#include "Document.h"
#include "EditLog.h"
#include "EditPlan.h"
#include "Json.h"
#include "Operations.h"
#include "Ulid.h"

#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const Ulid kSource = "01K30000000000000000000001";
const Ulid kVideo = "01K30000000000000000000002";
const Ulid kFirst = "01K30000000000000000000003";
const Ulid kSecond = "01K30000000000000000000004";
const Ulid kThird = "01K30000000000000000000005";

Document Fixture() {
    Document document;
    document.sequence.id = "01K3S000000000000000000001";
    document.sources = {{kSource, "folder/A.MP4", {25, 1}, {1000, 25}}};
    document.sequence.tracks = {
        {kVideo,
         "video",
         0,
         {{kFirst, kSource, {100, 25}, {10, 25}, {0, 25}},
          {kSecond, kSource, {200, 25}, {10, 25}, {10, 25}},
          {kThird, kSource, {300, 25}, {10, 25}, {20, 25}}}}};
    return document;
}

mcp_json::Value Parse(const std::string& json) {
    mcp_json::Value value;
    std::string error;
    mcp_json::Value::Parse(json, value, error);
    return value;
}

std::string Text(const mcp_json::Value& object, const char* key) {
    const mcp_json::Value* field = object.Find(key);
    return field != nullptr && field->IsString() ? field->AsString()
                                                 : std::string();
}

// A draft that creates two identities (an insert, a split) and removes a
// clip, so that what the application creates can be compared with what the
// preview announced.
bool DraftThreeSteps(EditPlanDraft& draft) {
    EditError error = EditError::None;
    std::string message;
    const bool applied =
        draft.Apply(
            InsertClipOperation{kVideo, kSource, {500, 25}, {5, 25}, {30, 25}},
            error, message) &&
        draft.Apply(SplitClipOperation{kSecond, {15, 25}, {}, {}}, error,
                    message) &&
        draft.Apply(RemoveClipOperation{kFirst, {}, {}}, error, message);
    Check(applied, "the three steps apply on the copy: " + message);
    return applied;
}

int Run() {
    const Document base = Fixture();
    const std::string baseBytes = base.SaveToString();

    // ---- preparation touches nothing and measures what it did ----
    EditPlanDraft draft(base);
    DraftThreeSteps(draft);
    Check(draft.Base().SaveToString() == baseBytes,
          "the draft's base stays the document it copied");
    mcp_json::Value steps = mcp_json::Value::MakeArray();
    steps.Push(Parse(R"({"step":1,"tool":"insert_clip"})"));
    EditPlan plan;
    std::string errorName;
    std::string message;
    Check(FinishEditPlan(draft, "Resserrer le début", steps, plan, errorName,
                         message),
          "a draft becomes a plan: " + errorName + " " + message);
    Check(plan.timeline_id == base.sequence.id && !plan.plan_id.empty() &&
              plan.batch.label == "Resserrer le début" &&
              plan.batch.operations.size() == 3,
          "the plan names its timeline and keeps its three members");
    Check(plan.base_doc_hash == TimelineRevision(base) &&
              plan.result_doc_hash == TimelineRevision(draft.Current()),
          "the plan records the revision it starts from and the one it "
          "leads to");
    const auto* insert =
        std::get_if<InsertClipOperation>(&plan.batch.operations[0]);
    const auto* split =
        std::get_if<SplitClipOperation>(&plan.batch.operations[1]);
    Check(insert != nullptr && !insert->clip_id.empty() && split != nullptr &&
              !split->right_clip_id.empty(),
          "the members are kept as applied, generated IDs included");

    const mcp_json::Value preview = Parse(plan.preview_json);
    const mcp_json::Value* delta = preview.Find("delta");
    std::vector<std::string> created;
    if (const mcp_json::Value* clips =
            delta != nullptr ? delta->Find("clips") : nullptr) {
        for (const mcp_json::Value& clip : clips->AsArray()) {
            const mcp_json::Value* flag = clip.Find("created");
            if (flag != nullptr && flag->IsBool() && flag->AsBool()) {
                created.push_back(Text(clip, "clip_id"));
            }
        }
    }
    Check(
        created.size() == 2 &&
            (created[0] == insert->clip_id || created[1] == insert->clip_id) &&
            (created[0] == split->right_clip_id ||
             created[1] == split->right_clip_id),
        "the engine's delta announces the two clips the plan creates: " +
            plan.preview_json);
    const mcp_json::Value* before = preview.Find("duration_before");
    const mcp_json::Value* after = preview.Find("duration_after");
    int64_t beforeValue = 0;
    int64_t afterValue = 0;
    Check(
        before != nullptr && after != nullptr &&
            before->Find("value")->AsInt64(beforeValue) &&
            after->Find("value")->AsInt64(afterValue) && beforeValue == 30 &&
            afterValue == 25,
        "durations before and after are measured, exact: " + plan.preview_json);
    Check(preview.Find("steps") != nullptr &&
              preview.Find("steps")->AsArray().size() == 1 &&
              Text(preview, "label") == "Resserrer le début",
          "the preview keeps the surface's step reports as given");

    // ---- applied as previewed, one undo step ----
    Document live = base;
    EditLog log;
    Check(ApplyEditPlan(live, log, plan, errorName, message),
          "a fresh plan applies: " + errorName + " " + message);
    Check(live.SaveToString() == draft.Current().SaveToString(),
          "the application lands on the preview's bytes, IDs included");
    Check(live.FindClip(insert->clip_id) != nullptr &&
              live.FindClip(split->right_clip_id) != nullptr &&
              live.FindClip(kFirst) == nullptr,
          "the identities created in the preview are the applied ones");
    Check(log.AppliedCount() == 1 && std::holds_alternative<BatchOperation>(
                                         log.AppliedEntries().back().op),
          "the plan is one journal entry, a batch");
    EditError error = EditError::None;
    Check(log.Undo(live, error, message) && live.SaveToString() == baseBytes,
          "one undo restores the original bytes: " + message);
    Check(log.Redo(live, error, message) &&
              TimelineRevision(live) == plan.result_doc_hash,
          "one redo replays the plan: " + message);

    // I9 -- the journal read back undoes the plan like the one in memory.
    EditLog reread;
    Check(EditLog::Deserialize(log.Serialize(), reread, error, message),
          "the journal holding a plan reads back: " + message);
    Document fromDisk = live;
    Check(reread.Undo(fromDisk, error, message) &&
              fromDisk.SaveToString() == baseBytes,
          "undo from the journal read back restores the original bytes: " +
              message);

    // ---- stale ----
    Document moved = base;
    EditLog movedLog;
    Check(movedLog.Apply(moved,
                         TrimClipOperation{kThird, TrimEdge::Tail, {-2, 25}},
                         error, message),
          "an edit elsewhere on the timeline: " + message);
    const std::string movedBytes = moved.SaveToString();
    std::string refusal;
    Check(!CheckEditPlanApplicable(plan, moved, refusal, errorName, message) &&
              errorName == kStaleRevisionError,
          "a plan prepared before that edit is stale: " + errorName);
    const mcp_json::Value refused = Parse(refusal);
    Check(Text(refused, "base_doc_hash") == plan.base_doc_hash &&
              Text(refused, "doc_hash") == TimelineRevision(moved) &&
              Text(refused, "plan_id") == plan.plan_id,
          "the refusal names both revisions: " + refusal);
    Check(!ApplyEditPlan(moved, movedLog, plan, errorName, message) &&
              errorName == kStaleRevisionError &&
              moved.SaveToString() == movedBytes &&
              movedLog.AppliedCount() == 1,
          "a stale plan applies nothing and writes no journal entry");
    Document other = base;
    other.sequence.id = "01K3S000000000000000000002";
    Check(!CheckEditPlanApplicable(plan, other, refusal, errorName, message) &&
              errorName == "UnknownSequence",
          "a plan applies to its own timeline only");

    // A timeline returned to its base state -- here by undo -- makes the
    // plan applicable again: freshness is a state, not a clock.
    Check(movedLog.Undo(moved, error, message) &&
              CheckEditPlanApplicable(plan, moved, refusal, errorName, message),
          "the plan applies again once the timeline is back at its base");

    // ---- the CLI's file ----
    const std::string file = SerializeEditPlan(plan);
    EditPlan read;
    Check(ParseEditPlan(file, read, message),
          "a plan file reads back: " + message);
    Check(SerializeEditPlan(read) == file &&
              SerializeOperation(read.batch) == SerializeOperation(plan.batch),
          "the plan file round-trips byte for byte");
    Document fromFile = base;
    EditLog fileLog;
    Check(ApplyEditPlan(fromFile, fileLog, read, errorName, message) &&
              fromFile.SaveToString() == draft.Current().SaveToString(),
          "a plan read back applies to the same bytes: " + message);
    const auto refusedFile = [&](const std::string& text,
                                 const std::string& label) {
        EditPlan ignored;
        std::string why;
        Check(!ParseEditPlan(text, ignored, why) && !why.empty(),
              label + " is refused: " + why);
    };
    refusedFile("[]", "a file that is not an object");
    std::string wrongVersion = file;
    wrongVersion.replace(wrongVersion.find("\"version\":1"), 11,
                         "\"version\":2");
    refusedFile(wrongVersion, "a later file version");
    std::string wrongLabel = file;
    wrongLabel.replace(wrongLabel.find("\"label\":\"") + 9, 1, "X");
    refusedFile(wrongLabel, "a label that does not match its batch");
    std::string badHash = file;
    badHash.replace(badHash.find("\"base_doc_hash\":\"") + 17, 16,
                    "not-a-revision!!");
    refusedFile(badHash, "a malformed revision");

    // A file whose promise no longer holds -- its result revision edited --
    // is refused before anything is applied.
    EditPlan forged = read;
    forged.result_doc_hash = plan.base_doc_hash;
    Document untouched = base;
    EditLog untouchedLog;
    Check(!ApplyEditPlan(untouched, untouchedLog, forged, errorName, message) &&
              errorName == "InvalidOperation" &&
              untouched.SaveToString() == baseBytes &&
              untouchedLog.AppliedCount() == 0,
          "a plan that does not lead where its preview says is refused: " +
              message);

    // ---- what a draft cannot become ----
    EditPlan refusedPlan;
    const EditPlanDraft empty(base);
    Check(!FinishEditPlan(empty, "Rien", steps, refusedPlan, errorName,
                          message) &&
              errorName == "ValidationFailed",
          "a plan that changes nothing is refused");
    Check(
        !FinishEditPlan(draft, "", steps, refusedPlan, errorName, message) &&
            !FinishEditPlan(draft, std::string(kMaxEditPlanLabelBytes + 1, 'x'),
                            steps, refusedPlan, errorName, message),
        "a plan needs a label a person can read");
    EditPlanDraft tooLong(base);
    for (size_t index = 0; index <= kMaxBatchOperations; ++index) {
        tooLong.Apply(
            TrimClipOperation{
                kThird, TrimEdge::Tail, {index % 2 == 0 ? -1 : 1, 25}},
            error, message);
    }
    Check(tooLong.OperationCount() == kMaxBatchOperations + 1 &&
              !FinishEditPlan(tooLong, "Trop", steps, refusedPlan, errorName,
                              message) &&
              errorName == "InvalidOperation",
          "a plan is one undo step, so at most 256 operations");

    // ---- the session's store ----
    EditPlanStore store;
    std::vector<Ulid> kept;
    for (size_t index = 0; index < EditPlanStore::kMaxPlans; ++index) {
        EditPlanDraft each(base);
        DraftThreeSteps(each);
        EditPlan made;
        FinishEditPlan(each, "Variante " + std::to_string(index), steps, made,
                       errorName, message);
        kept.push_back(made.plan_id);
        Check(store.Add(made, errorName, message),
              "a session keeps up to eight plans: " + message);
    }
    EditPlan ninth;
    EditPlanDraft ninthDraft(base);
    DraftThreeSteps(ninthDraft);
    FinishEditPlan(ninthDraft, "Neuvième", steps, ninth, errorName, message);
    Check(!store.Add(ninth, errorName, message) &&
              errorName == kEditPlanLimitError &&
              message.find(kept.front()) != std::string::npos &&
              store.PlanIds().size() == EditPlanStore::kMaxPlans,
          "a ninth plan is refused, naming those kept, and none is evicted");
    // PRD17 -- ROADMAP.md. The bound is the timeline's: the agents of a
    // series share one store, and another timeline's plan still has room.
    Document otherBase = base;
    otherBase.sequence.id = "01K36000000000000000000099";
    EditPlanDraft otherDraft(otherBase);
    DraftThreeSteps(otherDraft);
    EditPlan otherPlan;
    FinishEditPlan(otherDraft, "Autre timeline", steps, otherPlan, errorName,
                   message);
    Check(
        store.Add(otherPlan, errorName, message) &&
            store.PlanIdsForTimeline(otherBase.sequence.id).size() == 1 &&
            store.PlanIdsForTimeline(base.sequence.id).size() ==
                EditPlanStore::kMaxPlans,
        "a full timeline leaves room for another timeline's plans: " + message);
    store.Remove(otherPlan.plan_id);
    EditPlan found;
    Check(store.Find(kept.front(), found) && found.plan_id == kept.front() &&
              store.Remove(kept.front()) && !store.Find(kept.front(), found) &&
              !store.Remove(kept.front()),
          "a plan is found, then removed once");
    Check(store.Add(ninth, errorName, message),
          "a slot freed takes the next plan: " + message);
    Check(!store.Add(ninth, errorName, message) && errorName == "DuplicateId",
          "the same plan is not kept twice");

    if (failures != 0) {
        std::cerr << failures << " edit plan check(s) failed\n";
        return 1;
    }
    std::cout << "edit plan tests passed\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return Run();
    } catch (...) {
        std::cerr << "FAIL: an edit plan check threw\n";
        return 1;
    }
}
