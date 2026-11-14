#include "EditLog.h"

#include "CanonicalHash.h"
#include "FormatVersion.h"
#include "Json.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

class LogReader {
public:
    explicit LogReader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    std::string Object() { return Balanced('{', '}', "operation object"); }

    // MOTIF1 -- an entry's motifs, read whole and handed to their codec.
    std::string Array() { return Balanced('[', ']', "motif array"); }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start || position_ - start > 9) {
            throw std::runtime_error("expected a version number at byte " +
                                     std::to_string(start));
        }
        return std::strtoll(input_.substr(start, position_ - start).c_str(),
                            nullptr, 10);
    }

    std::string Balanced(char open, char close, const char* what) {
        Skip();
        if (position_ >= input_.size() || input_[position_] != open) {
            throw std::runtime_error("expected " + std::string(what) +
                                     " at byte " + std::to_string(position_));
        }
        const size_t start = position_;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    inString = false;
                continue;
            }
            if (character == '"')
                inString = true;
            else if (character == open)
                ++depth;
            else if (character == close && --depth == 0)
                return input_.substr(start, position_ - start);
        }
        throw std::runtime_error("unterminated " + std::string(what));
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing edit log JSON");
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }
    const std::string& input_;
    size_t position_ = 0;
};

void WriteEntries(std::ostringstream& output,
                  const std::vector<Entry>& entries) {
    output << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index) output << ',';
        output << "{\"op\":" << SerializeOperation(entries[index].op)
               << ",\"inverse\":" << SerializeOperation(entries[index].inverse);
        // MOTIF1 -- only an entry somebody gave a reason spells one: every
        // other entry keeps the bytes it always had.
        if (!entries[index].motifs.empty()) {
            output << ",\"motifs\":"
                   << SerializeEntryMotifs(entries[index].motifs);
        }
        output << '}';
    }
    output << ']';
}

std::vector<Entry> ReadEntries(LogReader& reader, bool motifs) {
    std::vector<Entry> entries;
    reader.Expect("[");
    if (reader.Consume("]")) return entries;
    while (true) {
        reader.Expect("{\"op\":");
        const std::string operationJson = reader.Object();
        reader.Expect(",\"inverse\":");
        const std::string inverseJson = reader.Object();
        // MOTIF1 -- a version 1 journal has no motif to read.
        std::string motifsJson;
        if (motifs && reader.Consume(",\"motifs\":")) {
            motifsJson = reader.Array();
        }
        reader.Expect("}");
        Entry entry;
        EditError error = EditError::None;
        std::string message;
        if (!DeserializeOperation(operationJson, entry.op, error, message))
            throw std::runtime_error("invalid logged operation: " + message);
        if (!DeserializeOperation(inverseJson, entry.inverse, error, message))
            throw std::runtime_error("invalid logged inverse: " + message);
        if (!motifsJson.empty()) {
            if (!DeserializeEntryMotifs(motifsJson, entry.motifs, message) ||
                !ValidateEntryMotifs(entry.motifs, EntryMemberCount(entry.op),
                                     message)) {
                throw std::runtime_error("invalid logged motifs: " + message);
            }
            if (entry.motifs.empty()) {
                throw std::runtime_error(
                    "invalid logged motifs: an empty list is never written");
            }
        }
        entries.push_back(std::move(entry));
        if (reader.Consume("]")) return entries;
        reader.Expect(",");
    }
}

void WriteProjectEntries(std::ostringstream& output,
                         const std::vector<ProjectEntry>& entries) {
    output << '[';
    for (size_t index = 0; index < entries.size(); ++index) {
        if (index) output << ',';
        output << "{\"op\":" << SerializeProjectOperation(entries[index].op)
               << ",\"inverse\":"
               << SerializeProjectOperation(entries[index].inverse);
        // MOTIF6 -- as on a timeline's journal: only an entry somebody gave
        // a reason spells one.
        if (!entries[index].motifs.empty()) {
            output << ",\"motifs\":"
                   << SerializeEntryMotifs(entries[index].motifs);
        }
        output << '}';
    }
    output << ']';
}

std::vector<ProjectEntry> ReadProjectEntries(LogReader& reader, bool motifs,
                                             size_t& compacted) {
    std::vector<ProjectEntry> entries;
    reader.Expect("[");
    if (reader.Consume("]")) return entries;
    while (true) {
        reader.Expect("{\"op\":");
        const std::string operationJson = reader.Object();
        reader.Expect(",\"inverse\":");
        const std::string inverseJson = reader.Object();
        // MOTIF6 -- a version 1 project journal has no motif to read.
        std::string motifsJson;
        if (motifs && reader.Consume(",\"motifs\":")) {
            motifsJson = reader.Array();
        }
        reader.Expect("}");
        ProjectEntry entry{AddProjectTimelineOperation{},
                           AddProjectTimelineOperation{}};
        EditError error = EditError::None;
        std::string message;
        if (!DeserializeProjectOperation(operationJson, entry.op, error,
                                         message))
            throw std::runtime_error("invalid logged project operation: " +
                                     message);
        if (!DeserializeProjectOperation(inverseJson, entry.inverse, error,
                                         message))
            throw std::runtime_error("invalid logged project inverse: " +
                                     message);
        if (!motifsJson.empty()) {
            if (!DeserializeEntryMotifs(motifsJson, entry.motifs, message) ||
                !ValidateEntryMotifs(entry.motifs, 1, message)) {
                throw std::runtime_error("invalid logged project motifs: " +
                                         message);
            }
            if (entry.motifs.empty()) {
                throw std::runtime_error(
                    "invalid logged project motifs: an empty list is never "
                    "written");
            }
        }
        // S6 -- SAVING_ROADMAP.md. An entry written before S6 carries two
        // whole projects; reading it is the one moment it can shrink without
        // anyone losing an undo, and the next save then writes the delta.
        // DEC24 -- counted, so that opening a project can write it back.
        if (CompactLegacyProjectEntry(entry.op, entry.inverse)) ++compacted;
        entries.push_back(std::move(entry));
        if (reader.Consume("]")) return entries;
        reader.Expect(",");
    }
}

}  // namespace

namespace {

// The anchor of a mark: the text of the entry on top of the history when
// the intervention began, or of nothing at all for an empty history.
std::string HistoryAnchor(const std::vector<Entry>& applied, size_t depth) {
    return CanonicalHash(
        depth == 0 ? std::string() : SerializeOperation(applied[depth - 1].op));
}

// DEC36 -- docs/roadmap/GENERATIF-2026-10.md. A media's provenance is
// erased by undo only (decided by the author on 23 October 2026). The mark that a
// picture was generated must not be one edit away from gone: an edit that
// would clear it, or replace it with another, is refused. Undo and redo
// replay entries the journal already holds, and an intervention's fold
// replays them on a copy, so none of them comes through here -- only a new
// edit does, which is exactly the one this guards.
//
// `known` follows the provenances the members of a batch set before the
// one being read, so that a batch which sets one and then clears it is
// refused as the two edits would be.
bool KeepsProvenance(const Document& document, const Operation& operation,
                     std::map<Ulid, MediaGeneration>& known,
                     std::string& message) {
    if (const auto* batch = std::get_if<BatchOperation>(&operation)) {
        for (const Operation& member : batch->operations) {
            if (!KeepsProvenance(document, member, known, message)) {
                return false;
            }
        }
        return true;
    }
    const auto currentOf = [&](const Ulid& mediaId) {
        if (const auto found = known.find(mediaId); found != known.end()) {
            return found->second;
        }
        const LibraryMedia* media = document.FindLibraryMedia(mediaId);
        return media != nullptr ? media->generation : MediaGeneration{};
    };
    const auto refuse = [&](const Ulid& mediaId,
                            const MediaGeneration& current) {
        message = "media '" + mediaId +
                  "' carries the provenance of a "
                  "generation (" +
                  current.service + ", " + current.model + ", " +
                  current.created_at +
                  "): an edit never clears or replaces it, only undo removes "
                  "it";
        return false;
    };
    // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. An ingest has an
    // exact inverse that takes its media out of the library, and with them a
    // provenance set since: applied as a new edit, it would clear the mark by
    // another road, and the file could come back unmarked by a second
    // ingest. Undo replays it without coming through here.
    if (const auto* removal =
            std::get_if<RemoveIngestedMediaOperation>(&operation)) {
        for (const Ulid& mediaId : removal->media_ids) {
            const MediaGeneration current = currentOf(mediaId);
            if (!current.service.empty()) return refuse(mediaId, current);
        }
        return true;
    }
    const auto* set = std::get_if<SetMediaGenerationOperation>(&operation);
    if (set == nullptr) {
        return true;
    }
    const MediaGeneration current = currentOf(set->media_id);
    if (!current.service.empty() && !(set->generation == current)) {
        return refuse(set->media_id, current);
    }
    known[set->media_id] = set->generation;
    return true;
}

}  // namespace

InterventionMark MarkIntervention(const Ulid& timelineId, const EditLog& log) {
    return {timelineId, log.AppliedCount(),
            HistoryAnchor(log.AppliedEntries(), log.AppliedCount())};
}

std::string FormatInterventionMark(const InterventionMark& mark) {
    return mark.timeline_id + ':' + std::to_string(mark.history_depth) + ':' +
           mark.history_anchor;
}

bool ParseInterventionMark(const std::string& token, InterventionMark& mark,
                           std::string& message) {
    const size_t first = token.find(':');
    const size_t second =
        first == std::string::npos ? first : token.find(':', first + 1);
    const auto invalid = [&] {
        message = "'" + token +
                  "' is not an intervention token; pass the `intervention` "
                  "begin_intervention returned";
        return false;
    };
    if (second == std::string::npos || first == 0 ||
        token.find(':', second + 1) != std::string::npos) {
        return invalid();
    }
    const std::string depth = token.substr(first + 1, second - first - 1);
    const std::string anchor = token.substr(second + 1);
    if (depth.empty() || depth.size() > 18 || anchor.size() != 16 ||
        !std::all_of(depth.begin(), depth.end(),
                     [](unsigned char c) { return std::isdigit(c) != 0; }) ||
        !std::all_of(anchor.begin(), anchor.end(),
                     [](unsigned char c) { return std::isxdigit(c) != 0; })) {
        return invalid();
    }
    mark.timeline_id = token.substr(0, first);
    mark.history_depth =
        static_cast<size_t>(std::strtoull(depth.c_str(), nullptr, 10));
    mark.history_anchor = anchor;
    std::transform(mark.history_anchor.begin(), mark.history_anchor.end(),
                   mark.history_anchor.begin(), [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    return true;
}

std::string InterventionBeganJson(const InterventionMark& mark,
                                  const Document& document) {
    return "{\"ok\":true,\"intervention\":\"" +
           mcp_json::EscapeJsonString(FormatInterventionMark(mark)) +
           "\",\"timeline_id\":\"" +
           mcp_json::EscapeJsonString(mark.timeline_id) +
           "\",\"history_depth\":" + std::to_string(mark.history_depth) +
           ",\"doc_hash\":\"" + CanonicalHash(document.SaveToString()) + "\"}";
}

std::string InterventionEndedJson(size_t grouped, const EditLog& log,
                                  const Document& document) {
    return "{\"ok\":true,\"grouped\":" + std::to_string(grouped) +
           ",\"history_depth\":" + std::to_string(log.AppliedCount()) +
           ",\"doc_hash\":\"" + CanonicalHash(document.SaveToString()) + "\"}";
}

bool EditLog::Apply(Document& document, Operation operation, EditError& error,
                    std::string& message, std::vector<EntryMotif> motifs) {
    if (!ValidateEntryMotifs(motifs, EntryMemberCount(operation), message)) {
        error = EditError::InvalidOperation;
        return false;
    }
    std::map<Ulid, MediaGeneration> provenances;
    if (!KeepsProvenance(document, operation, provenances, message)) {
        error = EditError::InvalidOperation;
        return false;
    }
    Operation inverse = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, inverse, error, message))
        return false;
    applied_.push_back(
        {std::move(operation), std::move(inverse), std::move(motifs)});
    undone_.clear();
    return true;
}

bool EditLog::Undo(Document& document, EditError& error, std::string& message) {
    if (applied_.empty()) {
        error = EditError::EmptyUndo;
        message = "undo stack is empty";
        return false;
    }
    Operation inverse = applied_.back().inverse;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, inverse, ignored, error, message))
        return false;
    undone_.push_back(std::move(applied_.back()));
    applied_.pop_back();
    return true;
}

bool EditLog::Redo(Document& document, EditError& error, std::string& message) {
    if (undone_.empty()) {
        error = EditError::EmptyRedo;
        message = "redo stack is empty";
        return false;
    }
    Operation operation = undone_.back().op;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(document, operation, ignored, error, message))
        return false;
    applied_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

bool EditLog::GroupSince(const Document& document, const InterventionMark& mark,
                         const std::string& label, size_t& grouped,
                         EditError& error, std::string& message,
                         const std::optional<EditMotif>& motif) {
    grouped = 0;
    if (motif && !ValidateEditMotif(*motif, message)) {
        error = EditError::InvalidOperation;
        return false;
    }
    if (label.empty()) {
        error = EditError::InvalidOperation;
        message = "an intervention needs a label";
        return false;
    }
    if (applied_.size() < mark.history_depth) {
        error = EditError::InvalidOperation;
        message =
            "the history was undone below the start of the intervention; it "
            "is left as it was";
        return false;
    }
    if (HistoryAnchor(applied_, mark.history_depth) != mark.history_anchor) {
        error = EditError::InvalidOperation;
        message =
            "the history under the intervention was rewritten since it "
            "began; it is left as it was";
        return false;
    }
    const size_t entries = applied_.size() - mark.history_depth;
    if (entries == 0) {
        // Nothing to name.
        error = EditError::None;
        message.clear();
        return true;
    }
    // PRD40 -- ROADMAP.md. One entry is folded too, into a batch of one under
    // the label: an intervention of a single step used to keep its bare
    // operation, and the history read back by describe lost what it was for.
    // A batch is the journal's one way to carry a label, and one of a single
    // member undoes in one step like the operation it holds.

    // One step's worth of members, in the order they were applied, and the
    // inverses that undo them, last first. A batch already in the history
    // lends its members rather than nesting.
    BatchOperation redo{label, {}};
    BatchOperation undo{label, {}};
    // MOTIF1 -- each entry's motifs follow the members it lends, moved by
    // the members folded before it.
    std::vector<EntryMotif> folded;
    for (size_t index = mark.history_depth; index < applied_.size(); ++index) {
        const Entry& entry = applied_[index];
        for (EntryMotif span : entry.motifs) {
            span.first += redo.operations.size();
            folded.push_back(std::move(span));
        }
        if (const auto* batch = std::get_if<BatchOperation>(&entry.op)) {
            redo.operations.insert(redo.operations.end(),
                                   batch->operations.begin(),
                                   batch->operations.end());
        } else {
            redo.operations.push_back(entry.op);
        }
    }
    for (size_t index = applied_.size(); index > mark.history_depth; --index) {
        const Entry& entry = applied_[index - 1];
        if (const auto* batch = std::get_if<BatchOperation>(&entry.inverse)) {
            undo.operations.insert(undo.operations.end(),
                                   batch->operations.begin(),
                                   batch->operations.end());
        } else {
            undo.operations.push_back(entry.inverse);
        }
    }
    if (redo.operations.size() > kMaxBatchOperations) {
        error = EditError::InvalidOperation;
        message =
            "the intervention holds " + std::to_string(redo.operations.size()) +
            " operations; at most " + std::to_string(kMaxBatchOperations) +
            " make one undo step, so the history is left as it was";
        return false;
    }

    // The promise, checked on a copy before it is made: undoing the fold
    // and redoing it land back on the bytes the history leads to now.
    const std::string current = document.SaveToString();
    Document replay = document;
    Operation inverse = undo;
    Operation forward = redo;
    Operation ignored = RemoveClipOperation{};
    if (!ApplyOperation(replay, inverse, ignored, error, message)) {
        message = "the intervention cannot be undone in one step: " + message +
                  "; the history is left as it was";
        return false;
    }
    if (!ApplyOperation(replay, forward, ignored, error, message)) {
        message = "the intervention cannot be redone in one step: " + message +
                  "; the history is left as it was";
        return false;
    }
    if (replay.SaveToString() != current) {
        error = EditError::InvalidOperation;
        message =
            "undoing and redoing the intervention in one step does not lead "
            "back here; the history is left as it was";
        return false;
    }
    Operation foldedOperation = std::move(redo);
    std::vector<EntryMotif> motifs =
        EntryMotifsFor(foldedOperation, folded, motif);
    applied_.resize(mark.history_depth);
    applied_.push_back(
        {std::move(foldedOperation), std::move(undo), std::move(motifs)});
    grouped = entries;
    error = EditError::None;
    message.clear();
    return true;
}

size_t EditLog::AppliedCount() const { return applied_.size(); }
size_t EditLog::UndoneCount() const { return undone_.size(); }
const std::vector<Entry>& EditLog::AppliedEntries() const { return applied_; }
const std::vector<Entry>& EditLog::UndoneEntries() const { return undone_; }

namespace {

std::set<Ulid> ClipIds(const std::vector<ExactTrackState>& tracks) {
    std::set<Ulid> ids;
    for (const ExactTrackState& track : tracks) {
        for (const DocumentClip& clip : track.clips) {
            ids.insert(clip.id);
        }
    }
    return ids;
}

HistoryEntry SummarizeEntry(const Entry& entry, size_t rank, bool undone) {
    HistoryEntry summary;
    summary.rank = rank;
    summary.undone = undone;
    // Named from the format's table, not from a serialization: an entry
    // that carries whole tracks would cost its full text to be named.
    summary.type = OperationTypeName(entry.op);
    for (const EntryMotif& span : entry.motifs) {
        const std::string reason = MotifReasonName(span.motif.reason);
        if (std::find(summary.reasons.begin(), summary.reasons.end(), reason) ==
            summary.reasons.end()) {
            summary.reasons.push_back(reason);
        }
    }
    if (const auto* batch = std::get_if<BatchOperation>(&entry.op)) {
        summary.label = batch->label;
        summary.operations = batch->operations.size();
        return summary;
    }
    // The entry's two states, after and before: the operation's tracks and
    // its inverse's. A journal written before the tracks were kept has
    // neither, and was a gap closed.
    const auto* after = std::get_if<DeleteGapOperation>(&entry.op);
    const auto* before = std::get_if<DeleteGapOperation>(&entry.inverse);
    summary.counts_clips = after != nullptr;
    if (after != nullptr && before != nullptr &&
        !after->exact_track_result.empty() &&
        !before->exact_track_result.empty()) {
        const std::set<Ulid> afterIds = ClipIds(after->exact_track_result);
        const std::set<Ulid> beforeIds = ClipIds(before->exact_track_result);
        for (const Ulid& id : afterIds) {
            if (beforeIds.count(id) == 0) {
                ++summary.clips_added;
            }
        }
        for (const Ulid& id : beforeIds) {
            if (afterIds.count(id) == 0) {
                ++summary.clips_removed;
            }
        }
    }
    return summary;
}

}  // namespace

TimelineHistory SummarizeHistory(const Ulid& timelineId, const EditLog& log,
                                 size_t limit) {
    TimelineHistory history;
    history.timeline_id = timelineId;
    const std::vector<Entry>& applied = log.AppliedEntries();
    const std::vector<Entry>& undone = log.UndoneEntries();
    history.applied = applied.size();
    history.total = applied.size() + undone.size();
    const size_t first = history.total > limit ? history.total - limit : 0;
    for (size_t index = first; index < history.total; ++index) {
        if (index < applied.size()) {
            history.entries.push_back(
                SummarizeEntry(applied[index], index + 1, false));
            continue;
        }
        // The undone entries are a stack whose top is the next Redo: it
        // comes right after the last entry in effect.
        const Entry& entry =
            undone[undone.size() - 1 - (index - applied.size())];
        history.entries.push_back(SummarizeEntry(entry, index + 1, true));
    }
    return history;
}

std::string TimelineHistoryJson(const TimelineHistory& history) {
    using mcp_json::Value;
    const auto count = [](size_t value) {
        return Value::MakeInt(static_cast<int64_t>(value));
    };
    Value entries = Value::MakeArray();
    for (const HistoryEntry& entry : history.entries) {
        Value item = Value::MakeObject();
        item.Set("rank", count(entry.rank));
        item.Set("type", Value::MakeString(entry.type));
        if (!entry.label.empty()) {
            item.Set("label", Value::MakeString(entry.label));
        }
        item.Set("operations", count(entry.operations));
        if (entry.counts_clips) {
            item.Set("clips_added", count(entry.clips_added));
            item.Set("clips_removed", count(entry.clips_removed));
        }
        if (entry.undone) {
            item.Set("undone", Value::MakeBool(true));
        }
        if (!entry.reasons.empty()) {
            Value reasons = Value::MakeArray();
            for (const std::string& reason : entry.reasons) {
                reasons.Push(Value::MakeString(reason));
            }
            item.Set("motifs", std::move(reasons));
        }
        entries.Push(std::move(item));
    }
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(history.timeline_id));
    root.Set("total", count(history.total));
    root.Set("applied", count(history.applied));
    root.Set("entries", std::move(entries));
    return root.Dump();
}

bool ReadTimelineChanges(const Document& current, const EditLog& log,
                         const std::string& sinceHash, TimelineChanges& changes,
                         std::string& message) {
    changes = {};
    message.clear();
    changes.timeline_id = current.sequence.id;
    changes.since = sinceHash;
    changes.doc_hash = CanonicalHash(current.SaveToString());
    // No state asked about: the current one is the answer.
    if (sinceHash.empty()) changes.since = changes.doc_hash;
    if (changes.doc_hash == changes.since) {
        changes.found = true;
        return true;
    }
    const TimelineHistory history =
        SummarizeHistory(current.sequence.id, log, kChangesSearchLimit * 2);
    const auto entryAt = [&](size_t rank) -> const HistoryEntry* {
        for (const HistoryEntry& entry : history.entries)
            if (entry.rank == rank) return &entry;
        return nullptr;
    };
    const auto settle = [&](const Document& state, bool undone, size_t first,
                            size_t last) {
        changes.found = true;
        changes.undone = undone;
        // The two states differ, whatever a delta can say of it: a delta
        // names clips, tracks and the identities created or removed, and
        // is silent on a marker renamed or a style changed, which the
        // entries still tell.
        changes.changed = true;
        ComputeDocumentDelta(state, current, changes.delta);
        for (size_t rank = first; rank <= last; ++rank)
            if (const HistoryEntry* entry = entryAt(rank))
                changes.entries.push_back(*entry);
    };

    // Backwards: the state was left by an entry still in effect.
    {
        Document state = current;
        EditLog copy = log;
        const size_t applied = copy.AppliedCount();
        for (size_t steps = 1; steps <= applied && steps <= kChangesSearchLimit;
             ++steps) {
            EditError error = EditError::None;
            if (!copy.Undo(state, error, message)) {
                message =
                    "the journal cannot be undone to look for the "
                    "state: " +
                    message;
                return false;
            }
            if (CanonicalHash(state.SaveToString()) == sinceHash) {
                settle(state, false, applied - steps + 1, applied);
                return true;
            }
        }
    }
    // Forwards: the entries after the state were undone.
    {
        Document state = current;
        EditLog copy = log;
        const size_t applied = copy.AppliedCount();
        const size_t undone = copy.UndoneCount();
        for (size_t steps = 1; steps <= undone && steps <= kChangesSearchLimit;
             ++steps) {
            EditError error = EditError::None;
            if (!copy.Redo(state, error, message)) {
                message =
                    "the journal cannot be redone to look for the "
                    "state: " +
                    message;
                return false;
            }
            if (CanonicalHash(state.SaveToString()) == sinceHash) {
                settle(state, true, applied + 1, applied + steps);
                return true;
            }
        }
    }
    message.clear();
    return true;
}

std::string TimelineChangesJson(const TimelineChanges& changes) {
    using mcp_json::Value;
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(changes.timeline_id));
    root.Set("since", Value::MakeString(changes.since));
    root.Set("doc_hash", Value::MakeString(changes.doc_hash));
    root.Set("found", Value::MakeBool(changes.found));
    if (!changes.found) {
        root.Set("note",
                 Value::MakeString(
                     "the journal no longer leads to that state (it was "
                     "undone and other edits made, or it is further than " +
                     std::to_string(kChangesSearchLimit) +
                     " entries away): read the timeline again with "
                     "describe"));
        return root.Dump();
    }
    root.Set("changed", Value::MakeBool(changes.changed));
    root.Set("undone", Value::MakeBool(changes.undone));
    Value entries = Value::MakeArray();
    for (const HistoryEntry& entry : changes.entries) {
        Value item = Value::MakeObject();
        item.Set("rank", Value::MakeInt(static_cast<int64_t>(entry.rank)));
        item.Set("type", Value::MakeString(entry.type));
        if (!entry.label.empty())
            item.Set("label", Value::MakeString(entry.label));
        item.Set("operations",
                 Value::MakeInt(static_cast<int64_t>(entry.operations)));
        if (!entry.reasons.empty()) {
            Value reasons = Value::MakeArray();
            for (const std::string& reason : entry.reasons)
                reasons.Push(Value::MakeString(reason));
            item.Set("motifs", std::move(reasons));
        }
        entries.Push(std::move(item));
    }
    root.Set("entries", std::move(entries));
    if (changes.changed) {
        Value delta;
        std::string parseError;
        if (Value::Parse(SerializeDocumentDelta(changes.delta), delta,
                         parseError))
            root.Set("delta", std::move(delta));
    }
    return root.Dump();
}

bool EditLog::Save(const std::string& path, EditError& error,
                   std::string& message) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = EditError::IoError;
        message = "unable to create edit log '" + path + "'";
        return false;
    }
    output << Serialize();
    if (!output) {
        error = EditError::IoError;
        message = "unable to write edit log '" + path + "'";
        return false;
    }
    error = EditError::None;
    message.clear();
    return true;
}

bool EditLog::Load(const std::string& path, EditLog& output, EditError& error,
                   std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = EditError::IoError;
        message = "unable to open edit log '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = EditError::IoError;
        message = "unable to read edit log '" + path + "'";
        return false;
    }
    return Deserialize(contents.str(), output, error, message);
}

std::string EditLog::Serialize() const {
    const auto hasMotifs = [](const std::vector<Entry>& entries) {
        return std::any_of(
            entries.begin(), entries.end(),
            [](const Entry& entry) { return !entry.motifs.empty(); });
    };
    const int version = hasMotifs(applied_) || hasMotifs(undone_)
                            ? kEditLogVersionWithMotifs
                            : kEditLogVersion;
    std::ostringstream output;
    output << "{\"version\":" << version << ",\"applied\":";
    WriteEntries(output, applied_);
    output << ",\"undone\":";
    WriteEntries(output, undone_);
    output << "}\n";
    return output.str();
}

bool EditLog::Deserialize(const std::string& json, EditLog& output,
                          EditError& error, std::string& message) {
    try {
        LogReader reader(json);
        reader.Expect("{\"version\":");
        const int64_t version = reader.Integer();
        if (version > kEditLogVersionWithMotifs) {
            throw std::runtime_error(NewerFormatRefusal(
                "edit log", version, kEditLogVersionWithMotifs));
        }
        if (version < kEditLogVersion) {
            throw std::runtime_error("unsupported edit log version " +
                                     std::to_string(version));
        }
        const bool motifs = version >= kEditLogVersionWithMotifs;
        reader.Expect(",\"applied\":");
        EditLog parsed;
        parsed.applied_ = ReadEntries(reader, motifs);
        reader.Expect(",\"undone\":");
        parsed.undone_ = ReadEntries(reader, motifs);
        reader.Expect("}");
        reader.Finish();
        output = std::move(parsed);
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}

bool ProjectEditLog::Apply(Project& project, ProjectOperation operation,
                           EditError& error, std::string& message,
                           std::vector<EntryMotif> motifs) {
    if (!ValidateEntryMotifs(motifs, 1, message)) {
        error = EditError::InvalidOperation;
        return false;
    }
    ProjectOperation inverse = AddProjectTimelineOperation{};
    if (!ApplyProjectOperation(project, operation, inverse, error, message))
        return false;
    applied_.push_back(
        {std::move(operation), std::move(inverse), std::move(motifs)});
    undone_.clear();
    return true;
}

bool ProjectEditLog::ApplyBatch(Project& project,
                                std::vector<ProjectOperation> operations,
                                EditError& error, std::string& message,
                                std::vector<EntryMotif> motifs) {
    if (operations.empty()) {
        error = EditError::InvalidOperation;
        message = "project operation batch is empty";
        return false;
    }
    if (!ValidateEntryMotifs(motifs, 1, message)) {
        error = EditError::InvalidOperation;
        return false;
    }

    // B10 -- ROADMAP.md. Apply every intention to a private candidate first:
    // a refusal at any position leaves both the project and its history
    // untouched. The log entry itself uses the ordinary exact-state replay
    // path, so the whole ordered batch is one undo/redo step without adding a
    // parallel mutation mechanism.
    Project candidate = project;
    for (ProjectOperation& operation : operations) {
        ProjectOperation ignoredInverse = AddProjectTimelineOperation{};
        if (!ApplyProjectOperation(candidate, operation, ignoredInverse, error,
                                   message))
            return false;
    }

    // S6 -- the batch is recorded as the change it makes, like any entry;
    // whole states remain only for a change a delta cannot express.
    ProjectDelta delta;
    std::string deltaError;
    const bool expressible =
        ComputeProjectDelta(project, candidate, delta, deltaError);
    ProjectOperation redo = SetActiveProjectTimelineOperation{
        candidate.active_timeline_id,
        expressible
            ? ExactProjectState{std::string(), delta}
            : ExactProjectState{candidate.SaveToString(), std::nullopt}};
    ProjectOperation undo = SetActiveProjectTimelineOperation{
        project.active_timeline_id,
        expressible
            ? ExactProjectState{std::string(), ReverseProjectDelta(delta)}
            : ExactProjectState{project.SaveToString(), std::nullopt}};
    project = std::move(candidate);
    applied_.push_back({std::move(redo), std::move(undo), std::move(motifs)});
    undone_.clear();
    error = EditError::None;
    message.clear();
    return true;
}

bool ProjectEditLog::Undo(Project& project, EditError& error,
                          std::string& message) {
    if (applied_.empty()) {
        error = EditError::EmptyUndo;
        message = "project undo stack is empty";
        return false;
    }
    ProjectOperation inverse = applied_.back().inverse;
    ProjectOperation ignored = AddProjectTimelineOperation{};
    if (!ApplyProjectOperation(project, inverse, ignored, error, message))
        return false;
    undone_.push_back(std::move(applied_.back()));
    applied_.pop_back();
    return true;
}

bool ProjectEditLog::Redo(Project& project, EditError& error,
                          std::string& message) {
    if (undone_.empty()) {
        error = EditError::EmptyRedo;
        message = "project redo stack is empty";
        return false;
    }
    ProjectOperation operation = undone_.back().op;
    ProjectOperation ignored = AddProjectTimelineOperation{};
    if (!ApplyProjectOperation(project, operation, ignored, error, message))
        return false;
    applied_.push_back(std::move(undone_.back()));
    undone_.pop_back();
    return true;
}

size_t ProjectEditLog::AppliedCount() const { return applied_.size(); }
size_t ProjectEditLog::CompactedOnRead() const { return compacted_on_read_; }
size_t ProjectEditLog::UndoneCount() const { return undone_.size(); }
const std::vector<ProjectEntry>& ProjectEditLog::AppliedEntries() const {
    return applied_;
}
const std::vector<ProjectEntry>& ProjectEditLog::UndoneEntries() const {
    return undone_;
}

bool ProjectEditLog::Save(const std::string& path, EditError& error,
                          std::string& message) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error = EditError::IoError;
        message = "unable to create project edit log '" + path + "'";
        return false;
    }
    output << Serialize();
    if (!output) {
        error = EditError::IoError;
        message = "unable to write project edit log '" + path + "'";
        return false;
    }
    error = EditError::None;
    message.clear();
    return true;
}

bool ProjectEditLog::Load(const std::string& path, ProjectEditLog& output,
                          EditError& error, std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = EditError::IoError;
        message = "unable to open project edit log '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = EditError::IoError;
        message = "unable to read project edit log '" + path + "'";
        return false;
    }
    return Deserialize(contents.str(), output, error, message);
}

std::string ProjectEditLog::Serialize() const {
    const auto hasMotifs = [](const std::vector<ProjectEntry>& entries) {
        return std::any_of(
            entries.begin(), entries.end(),
            [](const ProjectEntry& entry) { return !entry.motifs.empty(); });
    };
    const int version = hasMotifs(applied_) || hasMotifs(undone_)
                            ? kProjectEditLogVersionWithMotifs
                            : kProjectEditLogVersion;
    std::ostringstream output;
    output << "{\"version\":" << version
           << ",\"kind\":\"project\",\"applied\":";
    WriteProjectEntries(output, applied_);
    output << ",\"undone\":";
    WriteProjectEntries(output, undone_);
    output << "}\n";
    return output.str();
}

bool ProjectEditLog::Deserialize(const std::string& json,
                                 ProjectEditLog& output, EditError& error,
                                 std::string& message) {
    try {
        LogReader reader(json);
        reader.Expect("{\"version\":");
        const int64_t version = reader.Integer();
        if (version > kProjectEditLogVersionWithMotifs) {
            throw std::runtime_error(NewerFormatRefusal(
                "project edit log", version, kProjectEditLogVersionWithMotifs));
        }
        if (version < kProjectEditLogVersion) {
            throw std::runtime_error("unsupported project edit log version " +
                                     std::to_string(version));
        }
        const bool motifs = version >= kProjectEditLogVersionWithMotifs;
        reader.Expect(",\"kind\":\"project\",\"applied\":");
        ProjectEditLog parsed;
        parsed.applied_ =
            ReadProjectEntries(reader, motifs, parsed.compacted_on_read_);
        reader.Expect(",\"undone\":");
        parsed.undone_ =
            ReadProjectEntries(reader, motifs, parsed.compacted_on_read_);
        reader.Expect("}");
        reader.Finish();
        output = std::move(parsed);
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}
