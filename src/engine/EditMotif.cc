#include "EditMotif.h"

#include "EditLog.h"
#include "Project.h"

#include <algorithm>
#include <map>
#include <tuple>
#include <utility>
#include <variant>

// MOTIF1, MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. The names of the
// vocabulary and the journal codec live in OperationFormat.cc, beside the
// tables that describe them; this file holds what is not a spelling.

namespace {

using mcp_json::Value;

size_t CodePoints(const std::string& text) {
    size_t count = 0;
    for (const char character : text) {
        // Every byte that does not continue a UTF-8 sequence starts one.
        if ((static_cast<unsigned char>(character) & 0xC0) != 0x80) ++count;
    }
    return count;
}

Value TimeJson(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

Value IdsJson(const std::vector<Ulid>& ids) {
    Value array = Value::MakeArray();
    for (const Ulid& id : ids) array.Push(Value::MakeString(id));
    return array;
}

// What one member of an entry touched, as far as the operation says it
// without replaying the history: the clips it names and, for a cut in the
// words, the source ranges it took out.
Value TouchedJson(const Operation& operation) {
    Value touched = Value::MakeObject();
    touched.Set("type", Value::MakeString(OperationTypeName(operation)));
    if (const auto* words = std::get_if<RemoveWordsOperation>(&operation)) {
        touched.Set("clip_id", Value::MakeString(words->clip_id));
        if (!words->linked_clip_ids.empty()) {
            touched.Set("linked_clip_ids", IdsJson(words->linked_clip_ids));
        }
        Value ranges = Value::MakeArray();
        for (const WordRemovalRange& range : words->ranges) {
            Value item = Value::MakeObject();
            item.Set("source_start", TimeJson(range.source_start));
            item.Set("source_end", TimeJson(range.source_end));
            ranges.Push(std::move(item));
        }
        touched.Set("source_ranges", std::move(ranges));
    } else if (const auto* air =
                   std::get_if<TrimBoundaryAirOperation>(&operation)) {
        Value trims = Value::MakeArray();
        for (const BoundaryAirTrim& trim : air->trims) {
            Value item = Value::MakeObject();
            item.Set("clip_id", Value::MakeString(trim.clip_id));
            item.Set("edge",
                     Value::MakeString(trim.edge == TrimEdge::Head ? "head"
                                                                   : "tail"));
            item.Set("delta", TimeJson(trim.delta));
            trims.Push(std::move(item));
        }
        touched.Set("trims", std::move(trims));
    }
    return touched;
}

}  // namespace

bool ValidateEditMotif(const EditMotif& motif, std::string& message) {
    if (!motif.params.IsNull() && !motif.params.IsObject()) {
        message = "a motif's params must be a JSON object";
        return false;
    }
    if (!motif.params.IsNull() &&
        motif.params.Dump().size() > kMaxMotifParamsBytes) {
        message = "a motif's params hold at most " +
                  std::to_string(kMaxMotifParamsBytes) + " bytes";
        return false;
    }
    if (CodePoints(motif.note) > kMaxMotifNoteCodePoints) {
        message = "a motif's note is a line: at most " +
                  std::to_string(kMaxMotifNoteCodePoints) + " characters";
        return false;
    }
    if (motif.reason == MotifReason::Other && motif.note.empty()) {
        message = "a motif whose reason is 'other' says what in its note";
        return false;
    }
    if (motif.reason == MotifReason::Unstated && motif.note.empty() &&
        motif.rule.empty() && motif.marker_id.empty()) {
        message = "a motif says why: a reason, a note, a rule or a marker";
        return false;
    }
    message.clear();
    return true;
}

bool ParseMotifArgument(const Value& value, const std::string& path,
                        EditMotif& motif, std::string& message) {
    if (!value.IsObject()) {
        message = "'" + path + "' must be a JSON object";
        return false;
    }
    EditMotif parsed;
    for (const auto& [key, field] : value.AsObject()) {
        const std::string fieldPath = path + "." + key;
        if (key == "reason") {
            if (!field.IsString() ||
                !ParseMotifReason(field.AsString(), parsed.reason) ||
                parsed.reason == MotifReason::Unstated) {
                std::string names;
                for (const std::string& name : MotifReasonNames()) {
                    names += names.empty() ? name : ", " + name;
                }
                message = "'" + fieldPath + "' must be one of: " + names;
                return false;
            }
        } else if (key == "author") {
            if (!field.IsString() ||
                !ParseMotifAuthor(field.AsString(), parsed.author)) {
                message =
                    "'" + fieldPath + "' must be agent, person or procedure";
                return false;
            }
        } else if (key == "note" || key == "marker_id") {
            if (!field.IsString()) {
                message = "'" + fieldPath + "' must be a string";
                return false;
            }
            (key == "note" ? parsed.note : parsed.marker_id) = field.AsString();
        } else if (key == "params") {
            if (!field.IsObject()) {
                message = "'" + fieldPath + "' must be a JSON object";
                return false;
            }
            parsed.params = field;
        } else if (key == "rule") {
            message = "'" + fieldPath +
                      "' is set by the tool whose rule decided the edit, never "
                      "by its caller";
            return false;
        } else {
            message = "'" + path + "' has unknown key '" + key +
                      "' (reason, author, note, marker_id, params)";
            return false;
        }
    }
    if (parsed.reason == MotifReason::Unstated && parsed.note.empty()) {
        message = "'" + path + "' says why: give a reason or a note";
        return false;
    }
    if (!ValidateEditMotif(parsed, message)) {
        message = "'" + path + "': " + message;
        return false;
    }
    motif = std::move(parsed);
    return true;
}

Value EditMotifJson(const EditMotif& motif) {
    Value json = Value::MakeObject();
    if (motif.reason != MotifReason::Unstated) {
        json.Set("reason", Value::MakeString(MotifReasonName(motif.reason)));
    }
    json.Set("author", Value::MakeString(MotifAuthorName(motif.author)));
    if (!motif.rule.empty()) json.Set("rule", Value::MakeString(motif.rule));
    if (!motif.params.IsNull()) json.Set("params", motif.params);
    if (!motif.note.empty()) json.Set("note", Value::MakeString(motif.note));
    if (!motif.marker_id.empty()) {
        json.Set("marker_id", Value::MakeString(motif.marker_id));
    }
    return json;
}

size_t EntryMemberCount(const Operation& operation) {
    if (const auto* batch = std::get_if<BatchOperation>(&operation)) {
        return batch->operations.size();
    }
    return 1;
}

std::vector<EntryMotif> EntryMotifsFor(const Operation& operation,
                                       const std::vector<EntryMotif>& members,
                                       const std::optional<EditMotif>& motif) {
    std::vector<EntryMotif> spans = members;
    std::sort(spans.begin(), spans.end(),
              [](const EntryMotif& left, const EntryMotif& right) {
                  return left.first < right.first;
              });
    if (!motif) return spans;
    // The call's motif over every run of members no span covers.
    const uint64_t total = EntryMemberCount(operation);
    std::vector<EntryMotif> filled;
    uint64_t next = 0;
    const auto fill = [&](uint64_t end) {
        if (end > next) filled.push_back({next, end - next, *motif});
    };
    for (const EntryMotif& span : spans) {
        fill(span.first);
        filled.push_back(span);
        next = std::max(next, span.first + span.count);
    }
    fill(total);
    return filled;
}

bool ValidateEntryMotifs(const std::vector<EntryMotif>& motifs, size_t members,
                         std::string& message) {
    uint64_t next = 0;
    for (const EntryMotif& span : motifs) {
        if (span.count == 0 || span.first < next || span.first > members ||
            span.count > members - span.first) {
            message =
                "motif spans must be non-empty, sorted, disjoint and "
                "inside the entry's " +
                std::to_string(members) + " member(s)";
            return false;
        }
        if (!ValidateEditMotif(span.motif, message)) return false;
        next = span.first + span.count;
    }
    message.clear();
    return true;
}

namespace {

// MOTIF4, MOTIF6 -- the groups list_motifs answers, the same for a
// timeline's journal and the project's. A group is one reason, one rule and
// one set of parameters: "the pauses tightened at 400 ms" apart from "the
// pauses tightened at 250".
class MotifGroups {
public:
    MotifGroups(const std::optional<MotifReason>& reason,
                const std::string& rule)
        : reason_(reason), rule_(rule) {}

    bool Matches(const EditMotif& motif) const {
        return (!reason_ || motif.reason == *reason_) &&
               (rule_.empty() || motif.rule == rule_);
    }

    // `item` already holds what is the entry's own (rank, type, label,
    // first); the motif's author, note and marker follow it, then
    // `touched`.
    void Add(const EditMotif& motif, Value item, uint64_t operations,
             Value touched) {
        const Key key{MotifReasonName(motif.reason), motif.rule,
                      motif.params.Dump()};
        auto found = groups_.find(key);
        if (found == groups_.end()) {
            order_.push_back(key);
            found = groups_.emplace(key, Group{}).first;
            found->second.params = motif.params;
        }
        item.Set("operations",
                 Value::MakeInt(static_cast<int64_t>(operations)));
        item.Set("author", Value::MakeString(MotifAuthorName(motif.author)));
        if (!motif.note.empty()) {
            item.Set("note", Value::MakeString(motif.note));
        }
        if (!motif.marker_id.empty()) {
            item.Set("marker_id", Value::MakeString(motif.marker_id));
        }
        item.Set("touched", std::move(touched));
        found->second.entries.Push(std::move(item));
        found->second.operations += static_cast<int64_t>(operations);
    }

    Value Json() {
        Value groupsJson = Value::MakeArray();
        for (const Key& key : order_) {
            Group& group = groups_.at(key);
            Value item = Value::MakeObject();
            item.Set("reason", Value::MakeString(std::get<0>(key)));
            if (!std::get<1>(key).empty()) {
                item.Set("rule", Value::MakeString(std::get<1>(key)));
            }
            if (!group.params.IsNull()) item.Set("params", group.params);
            item.Set("operations", Value::MakeInt(group.operations));
            item.Set("entries", std::move(group.entries));
            groupsJson.Push(std::move(item));
        }
        return groupsJson;
    }

private:
    using Key = std::tuple<std::string, std::string, std::string>;
    struct Group {
        Value entries = Value::MakeArray();
        int64_t operations = 0;
        Value params;
    };
    std::optional<MotifReason> reason_;
    std::string rule_;
    std::vector<Key> order_;
    std::map<Key, Group> groups_;
};

}  // namespace

std::string ListMotifsJson(const Ulid& timelineId, const EditLog& log,
                           const std::optional<MotifReason>& reason,
                           const std::string& rule) {
    MotifGroups groups(reason, rule);
    int64_t unexplained = 0;
    const std::vector<Entry>& applied = log.AppliedEntries();
    for (size_t index = 0; index < applied.size(); ++index) {
        const Entry& entry = applied[index];
        const auto* batch = std::get_if<BatchOperation>(&entry.op);
        const auto member = [&](uint64_t at) -> const Operation& {
            return batch != nullptr ? batch->operations[at] : entry.op;
        };
        uint64_t covered = 0;
        for (const EntryMotif& span : entry.motifs) {
            covered += span.count;
            const EditMotif& motif = span.motif;
            if (!groups.Matches(motif)) continue;
            Value item = Value::MakeObject();
            item.Set("rank", Value::MakeInt(static_cast<int64_t>(index + 1)));
            item.Set("type", Value::MakeString(OperationTypeName(entry.op)));
            if (batch != nullptr) {
                item.Set("label", Value::MakeString(batch->label));
                item.Set("first",
                         Value::MakeInt(static_cast<int64_t>(span.first)));
            }
            Value touched = Value::MakeArray();
            for (uint64_t at = span.first; at < span.first + span.count; ++at) {
                touched.Push(TouchedJson(member(at)));
            }
            groups.Add(motif, std::move(item), span.count, std::move(touched));
        }
        unexplained +=
            static_cast<int64_t>(EntryMemberCount(entry.op) - covered);
    }
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(timelineId));
    root.Set("applied", Value::MakeInt(static_cast<int64_t>(applied.size())));
    root.Set("groups", groups.Json());
    // What the journal holds without a reason: the montage's untold part.
    root.Set("operations_without_motif", Value::MakeInt(unexplained));
    return root.Dump();
}

std::vector<EntryMotif> ProjectEntryMotifsFor(
    const std::optional<EditMotif>& motif) {
    if (!motif) return {};
    return {{0, 1, *motif}};
}

namespace {

// A timeline a project entry names, told by its name in the project as it
// is now: the reader of a choice wants "V2 rather than V1", not two IDs.
Value TimelineReference(const Project& project, const Ulid& id) {
    Value value = Value::MakeObject();
    value.Set("timeline_id", Value::MakeString(id));
    if (const DocumentSequence* timeline = project.FindTimeline(id)) {
        value.Set("name", Value::MakeString(timeline->name));
    } else {
        value.Set("removed", Value::MakeBool(true));
    }
    return value;
}

// What one project entry touched, from the operation alone: the timelines
// it names. A B10 batch is journaled as the change it made, under the type
// of its last member's activation, and says no more than that.
Value ProjectTouchedJson(const Project& project,
                         const ProjectOperation& operation) {
    Value touched = Value::MakeObject();
    touched.Set("type", Value::MakeString(ProjectOperationTypeName(operation)));
    const auto timeline = [&](const char* key, const Ulid& id) {
        touched.Set(key, TimelineReference(project, id));
    };
    if (const auto* add =
            std::get_if<AddProjectTimelineOperation>(&operation)) {
        timeline("timeline", add->timeline_id);
    } else if (const auto* clone =
                   std::get_if<CloneProjectTimelineOperation>(&operation)) {
        timeline("source", clone->source_timeline_id);
        timeline("timeline", clone->timeline_id);
    } else if (const auto* variants =
                   std::get_if<CreateProjectTimelineVariantsOperation>(
                       &operation)) {
        Value list = Value::MakeArray();
        for (const ProjectTimelineVariant& variant : variants->variants) {
            Value item = Value::MakeObject();
            item.Set("source", TimelineReference(
                                   project, variant.clone.source_timeline_id));
            item.Set("timeline",
                     TimelineReference(project, variant.clone.timeline_id));
            list.Push(std::move(item));
        }
        touched.Set("variants", std::move(list));
    } else if (const auto* segments =
                   std::get_if<CreateProjectTimelineFromSegmentsOperation>(
                       &operation)) {
        timeline("timeline", segments->timeline_id);
        touched.Set(
            "segments",
            Value::MakeInt(static_cast<int64_t>(segments->segments.size())));
    } else if (const auto* removed =
                   std::get_if<RemoveProjectTimelineOperation>(&operation)) {
        timeline("timeline", removed->timeline_id);
    } else if (const auto* rename =
                   std::get_if<RenameProjectItemOperation>(&operation)) {
        touched.Set("item_id", Value::MakeString(rename->item_id));
        touched.Set("name", Value::MakeString(rename->name));
    } else if (const auto* active =
                   std::get_if<SetActiveProjectTimelineOperation>(&operation)) {
        timeline("timeline", active->timeline_id);
    } else if (const auto* choice =
                   std::get_if<ChooseProjectTimelineOperation>(&operation)) {
        timeline("timeline", choice->timeline_id);
        Value alternatives = Value::MakeArray();
        for (const Ulid& id : choice->alternative_timeline_ids) {
            alternatives.Push(TimelineReference(project, id));
        }
        touched.Set("alternatives", std::move(alternatives));
        if (choice->make_active) {
            touched.Set("make_active", Value::MakeBool(true));
        }
    }
    return touched;
}

}  // namespace

std::string ListProjectMotifsJson(const Project& project,
                                  const ProjectEditLog& log,
                                  const std::optional<MotifReason>& reason,
                                  const std::string& rule) {
    MotifGroups groups(reason, rule);
    int64_t unexplained = 0;
    const std::vector<ProjectEntry>& applied = log.AppliedEntries();
    for (size_t index = 0; index < applied.size(); ++index) {
        const ProjectEntry& entry = applied[index];
        if (entry.motifs.empty()) {
            ++unexplained;
            continue;
        }
        for (const EntryMotif& span : entry.motifs) {
            if (!groups.Matches(span.motif)) continue;
            Value touched = ProjectTouchedJson(project, entry.op);
            Value item = Value::MakeObject();
            item.Set("rank", Value::MakeInt(static_cast<int64_t>(index + 1)));
            item.Set("type", *touched.Find("type"));
            Value members = Value::MakeArray();
            members.Push(std::move(touched));
            groups.Add(span.motif, std::move(item), span.count,
                       std::move(members));
        }
    }
    Value root = Value::MakeObject();
    root.Set("journal", Value::MakeString("project"));
    root.Set("applied", Value::MakeInt(static_cast<int64_t>(applied.size())));
    root.Set("groups", groups.Json());
    root.Set("operations_without_motif", Value::MakeInt(unexplained));
    return root.Dump();
}
