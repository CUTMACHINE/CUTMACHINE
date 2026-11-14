#include "EditPlan.h"

#include "CanonicalHash.h"
#include "DocumentDelta.h"
#include "Timeline.h"

#include <algorithm>
#include <cctype>
#include <exception>
#include <utility>
#include <variant>

namespace {

using mcp_json::Value;

inline constexpr char kPlanFileFormat[] = "cutmachine.edit_plan";
inline constexpr int64_t kPlanFileVersion = 1;

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

RationalTime DurationOf(const Document& document) {
    try {
        return Timeline(document).Duration();
    } catch (const std::exception&) {
        return RationalTime{0, 1};
    }
}

bool IsRevision(const std::string& text) {
    return text.size() == 16 &&
           std::all_of(text.begin(), text.end(), [](unsigned char c) {
               return std::isdigit(c) != 0 || (c >= 'a' && c <= 'f');
           });
}

const std::string* StringField(const Value& object, const char* key) {
    const Value* field = object.Find(key);
    return field != nullptr && field->IsString() ? &field->AsString() : nullptr;
}

}  // namespace

std::string TimelineRevision(const Document& document) {
    return CanonicalHash(document.SaveToString());
}

EditPlanDraft::EditPlanDraft(Document base)
    : base_(std::move(base)), current_(base_) {}

bool EditPlanDraft::Apply(Operation operation, EditError& error,
                          std::string& message,
                          std::vector<EntryMotif> motifs) {
    return log_.Apply(current_, std::move(operation), error, message,
                      std::move(motifs));
}

bool FinishEditPlan(const EditPlanDraft& draft, const std::string& label,
                    const Value& steps, EditPlan& plan, std::string& errorName,
                    std::string& message) {
    const auto refuse = [&](std::string text) {
        errorName = "ValidationFailed";
        message = std::move(text);
        return false;
    };
    if (label.empty() || label.size() > kMaxEditPlanLabelBytes) {
        return refuse("an edit plan needs a label of 1 to " +
                      std::to_string(kMaxEditPlanLabelBytes) + " bytes");
    }
    BatchOperation batch{label, {}};
    std::vector<EntryMotif> motifs;
    for (const Entry& entry : draft.Members()) {
        // MOTIF2 -- each step's motifs, moved by the members before it.
        for (EntryMotif span : entry.motifs) {
            span.first += batch.operations.size();
            motifs.push_back(std::move(span));
        }
        if (const auto* nested = std::get_if<BatchOperation>(&entry.op)) {
            batch.operations.insert(batch.operations.end(),
                                    nested->operations.begin(),
                                    nested->operations.end());
        } else {
            batch.operations.push_back(entry.op);
        }
    }
    if (batch.operations.empty()) {
        return refuse("the plan changes nothing: no step applied an edit");
    }
    if (batch.operations.size() > kMaxBatchOperations) {
        errorName = "InvalidOperation";
        message = "the plan holds " + std::to_string(batch.operations.size()) +
                  " operations; at most " +
                  std::to_string(kMaxBatchOperations) + " make one undo step";
        return false;
    }

    // The promise the preview makes, kept before the plan is handed out:
    // the batch, replayed on the base, lands on the bytes the steps reached,
    // identities they generated included.
    const std::string current = draft.Current().SaveToString();
    Document replay = draft.Base();
    Operation forward = batch;
    Operation ignored = RemoveClipOperation{};
    EditError error = EditError::None;
    if (!ApplyOperation(replay, forward, ignored, error, message)) {
        errorName = EditErrorName(error);
        message = "the plan does not replay on its own base: " + message;
        return false;
    }
    if (replay.SaveToString() != current) {
        errorName = "InvalidOperation";
        message =
            "the plan does not replay to what its steps produced; it is not "
            "kept";
        return false;
    }

    plan = EditPlan{};
    plan.plan_id = GenerateUlid();
    plan.timeline_id = draft.Base().sequence.id;
    plan.label = label;
    plan.base_doc_hash = TimelineRevision(draft.Base());
    plan.result_doc_hash = CanonicalHash(current);
    plan.batch = std::move(batch);
    plan.motifs = std::move(motifs);

    DocumentDelta delta;
    ComputeDocumentDelta(draft.Base(), draft.Current(), delta);
    Value deltaJson;
    std::string parseError;
    if (!Value::Parse(SerializeDocumentDelta(delta), deltaJson, parseError)) {
        errorName = "InvalidOperation";
        message = "unable to summarise the plan: " + parseError;
        return false;
    }
    Value preview = Value::MakeObject();
    preview.Set("plan_id", Value::MakeString(plan.plan_id));
    preview.Set("timeline_id", Value::MakeString(plan.timeline_id));
    preview.Set("timeline_name", Value::MakeString(draft.Base().sequence.name));
    preview.Set("label", Value::MakeString(plan.label));
    preview.Set(
        "operation_count",
        Value::MakeInt(static_cast<int64_t>(plan.batch.operations.size())));
    preview.Set("base_doc_hash", Value::MakeString(plan.base_doc_hash));
    preview.Set("result_doc_hash", Value::MakeString(plan.result_doc_hash));
    preview.Set("duration_before", TimeValue(DurationOf(draft.Base())));
    preview.Set("duration_after", TimeValue(delta.duration));
    preview.Set("delta", std::move(deltaJson));
    preview.Set("steps", steps.IsArray() ? steps : Value::MakeArray());
    plan.preview_json = preview.Dump();
    errorName.clear();
    message.clear();
    return true;
}

bool CheckEditPlanApplicable(const EditPlan& plan, const Document& current,
                             std::string& refusalJson, std::string& errorName,
                             std::string& message) {
    refusalJson.clear();
    if (current.sequence.id != plan.timeline_id) {
        errorName = "UnknownSequence";
        message = "plan '" + plan.plan_id + "' edits timeline '" +
                  plan.timeline_id + "', not '" + current.sequence.id + "'";
        return false;
    }
    const std::string revision = TimelineRevision(current);
    if (revision != plan.base_doc_hash) {
        Value refusal = Value::MakeObject();
        refusal.Set("plan_id", Value::MakeString(plan.plan_id));
        refusal.Set("timeline_id", Value::MakeString(plan.timeline_id));
        refusal.Set("base_doc_hash", Value::MakeString(plan.base_doc_hash));
        refusal.Set("doc_hash", Value::MakeString(revision));
        refusalJson = refusal.Dump();
        errorName = kStaleRevisionError;
        message = "the timeline changed since plan '" + plan.plan_id +
                  "' was prepared (doc_hash " + plan.base_doc_hash + " then, " +
                  revision +
                  " now); nothing was applied: prepare it again on the "
                  "current timeline";
        return false;
    }
    Document replay = current;
    Operation forward = plan.batch;
    Operation ignored = RemoveClipOperation{};
    EditError error = EditError::None;
    if (!ApplyOperation(replay, forward, ignored, error, message)) {
        errorName = EditErrorName(error);
        message = "plan '" + plan.plan_id + "' no longer applies: " + message;
        return false;
    }
    if (TimelineRevision(replay) != plan.result_doc_hash) {
        errorName = "InvalidOperation";
        message = "plan '" + plan.plan_id +
                  "' does not lead to the timeline its preview describes; "
                  "nothing was applied";
        return false;
    }
    errorName.clear();
    message.clear();
    return true;
}

bool ApplyEditPlan(Document& document, EditLog& log, const EditPlan& plan,
                   std::string& errorName, std::string& message) {
    std::string refusal;
    if (!CheckEditPlanApplicable(plan, document, refusal, errorName, message)) {
        return false;
    }
    EditError error = EditError::None;
    if (!log.Apply(document, plan.batch, error, message, plan.motifs)) {
        errorName = EditErrorName(error);
        return false;
    }
    return true;
}

std::string EditPlanAnswer(const EditPlan& plan, const std::string& status) {
    Value preview;
    std::string parseError;
    if (!Value::Parse(plan.preview_json, preview, parseError) ||
        !preview.IsObject()) {
        preview = Value::MakeObject();
        preview.Set("plan_id", Value::MakeString(plan.plan_id));
    }
    Value answer = Value::MakeObject();
    answer.Set("ok", Value::MakeBool(true));
    answer.Set("status", Value::MakeString(status));
    for (const auto& [key, value] : preview.AsObject()) {
        answer.Set(key, value);
    }
    return answer.Dump();
}

std::string SerializeEditPlan(const EditPlan& plan) {
    // The members stay the journal's own text rather than a re-dump of it:
    // the operation reader is positional, and this is the one spelling it
    // is guaranteed to read back.
    return "{\"format\":\"" + std::string(kPlanFileFormat) +
           "\",\"version\":" + std::to_string(kPlanFileVersion) +
           ",\"plan_id\":\"" + mcp_json::EscapeJsonString(plan.plan_id) +
           "\",\"timeline_id\":\"" +
           mcp_json::EscapeJsonString(plan.timeline_id) + "\",\"label\":\"" +
           mcp_json::EscapeJsonString(plan.label) + "\",\"base_doc_hash\":\"" +
           plan.base_doc_hash + "\",\"result_doc_hash\":\"" +
           plan.result_doc_hash +
           "\",\"operation\":" + SerializeOperation(plan.batch) +
           // MOTIF2 -- only a plan whose steps said why; a build before
           // MOTIF2 reads the file and leaves them out.
           (plan.motifs.empty()
                ? std::string()
                : ",\"motifs\":" + SerializeEntryMotifs(plan.motifs)) +
           ",\"preview\":" + plan.preview_json + "}\n";
}

bool ParseEditPlan(const std::string& json, EditPlan& plan,
                   std::string& message) {
    Value root;
    std::string parseError;
    if (!Value::Parse(json, root, parseError) || !root.IsObject()) {
        message = "not an edit plan file: " +
                  (parseError.empty() ? "not a JSON object" : parseError);
        return false;
    }
    const std::string* format = StringField(root, "format");
    const Value* version = root.Find("version");
    int64_t versionNumber = 0;
    if (format == nullptr || *format != kPlanFileFormat || version == nullptr ||
        !version->AsInt64(versionNumber)) {
        message = "not an edit plan file: `format` must be \"" +
                  std::string(kPlanFileFormat) + "\" with a `version`";
        return false;
    }
    if (versionNumber != kPlanFileVersion) {
        message = "edit plan file version " + std::to_string(versionNumber) +
                  " is not readable by this build (version " +
                  std::to_string(kPlanFileVersion) + ")";
        return false;
    }
    const std::string* planId = StringField(root, "plan_id");
    const std::string* timelineId = StringField(root, "timeline_id");
    const std::string* label = StringField(root, "label");
    const std::string* baseHash = StringField(root, "base_doc_hash");
    const std::string* resultHash = StringField(root, "result_doc_hash");
    const Value* operation = root.Find("operation");
    const Value* preview = root.Find("preview");
    if (planId == nullptr || planId->empty() || timelineId == nullptr ||
        timelineId->empty() || label == nullptr || label->empty() ||
        baseHash == nullptr || !IsRevision(*baseHash) ||
        resultHash == nullptr || !IsRevision(*resultHash) ||
        operation == nullptr || !operation->IsObject() || preview == nullptr ||
        !preview->IsObject()) {
        message =
            "edit plan file is incomplete: it needs plan_id, timeline_id, "
            "label, base_doc_hash, result_doc_hash, operation and preview";
        return false;
    }
    Operation decoded = RemoveClipOperation{};
    EditError error = EditError::None;
    if (!DeserializeOperation(operation->Dump(), decoded, error, message)) {
        message = "edit plan operation: " + message;
        return false;
    }
    auto* batch = std::get_if<BatchOperation>(&decoded);
    if (batch == nullptr || batch->label != *label) {
        message =
            "edit plan operation must be the Batch the plan was prepared "
            "as, under the plan's label";
        return false;
    }
    plan = EditPlan{};
    plan.plan_id = *planId;
    plan.timeline_id = *timelineId;
    plan.label = *label;
    plan.base_doc_hash = *baseHash;
    plan.result_doc_hash = *resultHash;
    std::vector<EntryMotif> motifs;
    if (const Value* given = root.Find("motifs")) {
        if (!given->IsArray() ||
            !DeserializeEntryMotifs(given->Dump(), motifs, message) ||
            !ValidateEntryMotifs(motifs, batch->operations.size(), message)) {
            message = "edit plan motifs: " +
                      (message.empty() ? "must be an array" : message);
            return false;
        }
    }
    plan.batch = std::move(*batch);
    plan.motifs = std::move(motifs);
    plan.preview_json = preview->Dump();
    return true;
}

bool EditPlanStore::Add(EditPlan plan, std::string& errorName,
                        std::string& message) {
    const size_t bytes = plan.preview_json.size() + plan.label.size() +
                         SerializeOperation(plan.batch).size();
    const std::lock_guard<std::mutex> lock(mutex_);
    if (plans_.count(plan.plan_id) != 0) {
        errorName = "DuplicateId";
        message = "edit plan '" + plan.plan_id + "' is already open";
        return false;
    }
    // PRD17 -- the count is the plan's own timeline's.
    std::string open;
    size_t same = 0;
    for (const auto& entry : plans_) {
        if (entry.second.first.timeline_id != plan.timeline_id) {
            continue;
        }
        ++same;
        if (!open.empty()) {
            open += ", ";
        }
        open += entry.first;
    }
    if (same >= kMaxPlans || bytes_ + bytes > kMaxBytes) {
        errorName = kEditPlanLimitError;
        message =
            same >= kMaxPlans
                ? "this session already keeps " + std::to_string(same) +
                      " edit plans for timeline " + plan.timeline_id +
                      " (at most " + std::to_string(kMaxPlans) +
                      " per timeline); apply or discard one first: " + open
                : "this session's edit plans already hold " +
                      std::to_string(bytes_ >> 20) + " MiB (at most " +
                      std::to_string(kMaxBytes >> 20) +
                      " MiB for all timelines); apply or discard one "
                      "first: " +
                      (open.empty() ? std::string("none open for this "
                                                  "timeline")
                                    : open);
        return false;
    }
    bytes_ += bytes;
    const Ulid id = plan.plan_id;
    plans_.emplace(id, std::make_pair(std::move(plan), bytes));
    return true;
}

bool EditPlanStore::Find(const Ulid& planId, EditPlan& plan) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = plans_.find(planId);
    if (found == plans_.end()) {
        return false;
    }
    plan = found->second.first;
    return true;
}

bool EditPlanStore::Remove(const Ulid& planId) {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = plans_.find(planId);
    if (found == plans_.end()) {
        return false;
    }
    bytes_ -= found->second.second;
    plans_.erase(found);
    return true;
}

std::vector<Ulid> EditPlanStore::PlanIds() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Ulid> ids;
    ids.reserve(plans_.size());
    for (const auto& entry : plans_) {
        ids.push_back(entry.first);
    }
    return ids;
}

std::vector<Ulid> EditPlanStore::PlanIdsForTimeline(
    const Ulid& timelineId) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Ulid> ids;
    for (const auto& entry : plans_) {
        if (entry.second.first.timeline_id == timelineId) {
            ids.push_back(entry.first);
        }
    }
    return ids;
}
