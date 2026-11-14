#pragma once

// PL2 -- ROADMAP.md (PLAN-2026-09). A plan of edits prepared on a copy of
// one timeline, then applied or discarded.
//
// CUTMACHINE lets an agent edit directly: each operation is applied,
// journalled and saved as it comes (IA-2026-09). A plan is the other
// way of working, kept beside that one and not instead of it: the agent
// prepares an intervention, and whoever drives -- a person, or the MCP client
// itself -- decides whether it is applied.
//
// What holds whichever surface prepared a plan:
//  - preparing writes nothing. The steps run through the ordinary
//    ApplyOperation on a private copy (EditPlanDraft): no project byte, no
//    journal entry, no active timeline changes before the plan is applied;
//  - what a plan does is measured, never stated: its summary is the
//    DocumentDelta from the base to the result, and its durations, computed
//    by the engine;
//  - what is applied is what was previewed. The members are kept as they
//    were applied on the copy, generated identities and exact states
//    included, and FinishEditPlan replays them on the base before handing
//    the plan out: the preview and the application create the same IDs;
//  - a plan is refused as stale unless its timeline is still, byte for byte,
//    the one it was prepared on (CheckEditPlanApplicable).
//
// Pure: no filesystem, no FFmpeg, no AppKit. Surfaces keep plans in memory
// (EditPlanStore) or, for the CLI, in a file the caller names
// (SerializeEditPlan); a plan never enters the project or its history until
// it is applied, and then as one BatchOperation entry (PL1).

#include "Document.h"
#include "EditLog.h"
#include "Json.h"
#include "Operations.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <cstddef>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// The revision of one timeline: the `doc_hash` every edit result already
// reports, CanonicalHash of the canonical document text. A plan records the
// revision it was prepared on and is refused at any other.
//
// The grain is the timeline's document, not the project: two plans prepared
// on two timelines stay applicable one after the other, which a revision of
// the whole project would forbid. The document still holds the library and
// the bins, so an ingest makes a plan stale -- refused rather than guessed.
std::string TimelineRevision(const Document& document);

// The refusal of a plan (or, in time, of any call) prepared against a
// revision that is no longer current. The B2 envelope's `result` then names
// both revisions, so the caller knows what to observe again.
inline constexpr char kStaleRevisionError[] = "StaleRevision";
// Refusal codes of the plan surfaces, stable like EditErrorName's.
inline constexpr char kUnknownPlanError[] = "UnknownPlan";
inline constexpr char kEditPlanLimitError[] = "EditPlanLimit";

// A label is the history's name for the plan once applied (PL1): one line a
// person can read, not a document.
inline constexpr size_t kMaxEditPlanLabelBytes = 1024;

struct EditPlan {
    Ulid plan_id;
    Ulid timeline_id;
    std::string label;
    // TimelineRevision of the document the plan was prepared on, and of the
    // one applying it leads to.
    std::string base_doc_hash;
    std::string result_doc_hash;
    // The members as applied on the copy, in order. Applied as one journal
    // entry, so one Undo reverts the whole plan.
    BatchOperation batch;
    // MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. Why each step's members were
    // made, as its step's motif or its rule gave it, placed on the batch's
    // members: the journal entry the plan becomes carries them.
    std::vector<EntryMotif> motifs;
    // JSON object the engine measured while preparing: identity, revisions,
    // durations, DocumentDelta and the steps' own reports. Kept as prepared,
    // so applying answers with the very summary the plan was reviewed on.
    std::string preview_json;
};

// The copy a plan's steps run on. Apply is the ordinary ApplyOperation,
// through an EditLog so that each member is kept exactly as applied.
class EditPlanDraft {
public:
    explicit EditPlanDraft(Document base);

    bool Apply(Operation operation, EditError& error, std::string& message,
               std::vector<EntryMotif> motifs = {});

    const Document& Base() const { return base_; }
    const Document& Current() const { return current_; }
    size_t OperationCount() const { return log_.AppliedCount(); }
    const std::vector<Entry>& Members() const { return log_.AppliedEntries(); }

private:
    Document base_;
    Document current_;
    EditLog log_;
};

// Turns a draft into a plan: a fresh plan_id, the batch of its members (a
// batch among them lends its members, never nests), both revisions and the
// preview. `steps` is the array of reports the surface gathered, one per
// step, stored in the preview as given.
//
// Refused when the label is empty or longer than kMaxEditPlanLabelBytes,
// when nothing was applied, when the plan holds more than
// kMaxBatchOperations members (one undo step, PL1), or when replaying the
// batch on the base does not land on the draft's bytes exactly.
bool FinishEditPlan(const EditPlanDraft& draft, const std::string& label,
                    const mcp_json::Value& steps, EditPlan& plan,
                    std::string& errorName, std::string& message);

// Whether `plan` may be applied to `current`: the same timeline, at the
// revision the plan was prepared on, and a batch that still leads to the
// revision its preview promised (which a plan read back from a file has to
// prove again). On a stale refusal `refusalJson` is the object the B2
// envelope carries as `result`: plan_id, timeline_id, base_doc_hash and the
// current doc_hash.
bool CheckEditPlanApplicable(const EditPlan& plan, const Document& current,
                             std::string& refusalJson, std::string& errorName,
                             std::string& message);

// The engine's application, for a caller holding the document and its
// journal: CheckEditPlanApplicable, then one EditLog::Apply of the batch,
// with the plan's motifs.
bool ApplyEditPlan(Document& document, EditLog& log, const EditPlan& plan,
                   std::string& errorName, std::string& message);

// The preview with `ok` and `status` ("prepared", "applied", "discarded")
// set, as every surface answers it.
std::string EditPlanAnswer(const EditPlan& plan, const std::string& status);

// PL3 -- the CLI keeps a plan in a file the caller names, because each of
// its invocations is a session of its own. Versioned JSON: the members are
// the journal's own BatchOperation text (OperationFormat.h), the rest
// identifies and summarises it. Parse checks the shape; whether the plan may
// still be applied is CheckEditPlanApplicable's question.
std::string SerializeEditPlan(const EditPlan& plan);
bool ParseEditPlan(const std::string& json, EditPlan& plan,
                   std::string& message);

// PL3 -- ROADMAP.md. The plans one session keeps between prepare and apply
// or discard: the MCP server's, or the chat panel's. Memory only -- a plan
// is local to the session that prepared it and dies with it; only the
// montage it produces, once applied, enters the project and its history.
//
// Bounded, and full means refused, never an older plan silently dropped: a
// person may be reading the preview of the plan an eviction would take.
// Plans on the same timeline may coexist -- alternatives to choose from --
// and applying one leaves the others stale rather than removing them, since
// an Undo can make them applicable again. Thread-safe: the panel's worker
// and its buttons reach the same store.
//
// PRD17 -- ROADMAP.md. The count is bounded per timeline: a series -- N
// agents editing N timelines through one server, and real projects hold 8
// to 26 timelines -- shares one store, and eight plans for the whole server
// had the ninth agent refused for plans that were not its own. The bytes
// stay bounded for the whole store.
class EditPlanStore {
public:
    // Per timeline.
    static constexpr size_t kMaxPlans = 8;
    static constexpr size_t kMaxBytes = size_t{64} << 20;

    bool Add(EditPlan plan, std::string& errorName, std::string& message);
    bool Find(const Ulid& planId, EditPlan& plan) const;
    bool Remove(const Ulid& planId);
    std::vector<Ulid> PlanIds() const;
    // PRD17 -- the plans kept for one timeline, in id order.
    std::vector<Ulid> PlanIdsForTimeline(const Ulid& timelineId) const;

private:
    mutable std::mutex mutex_;
    std::map<Ulid, std::pair<EditPlan, size_t>> plans_;
    size_t bytes_ = 0;
};
