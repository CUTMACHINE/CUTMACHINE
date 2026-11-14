#include "ProjectDelta.h"

#include "AggregateMembers.h"
#include "DocumentJson.h"
#include "Json.h"

#include <algorithm>
#include <initializer_list>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace {

using mcp_json::Value;

// --- Canonical text, used both to compare and to serialize -----------------

std::string ItemJson(const DocumentSequence& value) {
    return document_json::SequenceJson(value);
}
std::string ItemJson(const LibraryMedia& value) {
    return document_json::LibraryMediaJson(value);
}
std::string ItemJson(const DocumentBin& value) {
    return document_json::BinJson(value);
}
std::string ItemJson(const DocumentSource& value) {
    return document_json::SourceJson(value);
}

// Everything a timeline holds except its header, so that a rename and an
// edit of its content are told apart.
std::string ContentJson(DocumentSequence sequence) {
    sequence.id.clear();
    sequence.name.clear();
    sequence.width = 0;
    sequence.height = 0;
    sequence.frame_rate = {0, 1};
    return document_json::SequenceJson(sequence);
}

std::string QuotedJson(const std::string& value) {
    return "\"" + mcp_json::EscapeJsonString(value) + "\"";
}

// FORMAT-2026-09 F6 -- the records a delta holds are written and read by
// the document's own tables (DocumentFormat.h); only the shape of the delta
// around them is spelled here.
std::string MetadataJson(const ProjectBinMetadata& metadata) {
    return document_json::BinMetadataJson(metadata);
}

std::string RateJson(const MediaRate& rate) {
    return document_json::MediaRateJson(rate);
}

// FORMAT-2026-09 -- the delta's own structs are listed by hand four times:
// ComputeProjectDelta, ReverseProjectDelta, ApplyProjectDelta and the
// serializer pair below. A member one of them forgets is lost without an
// error -- from the journal, or from every undo -- so a new one is a
// compile error here until all four handle it.
static_assert(AggregateMemberCount<ProjectDelta>::value == 11,
              "ProjectDelta gained a member: compute, reverse, apply, "
              "serialize and deserialize it");
static_assert(AggregateMemberCount<TimelineDelta>::value == 6,
              "TimelineDelta gained a member: compute, reverse, apply, "
              "serialize and deserialize it");
static_assert(AggregateMemberCount<OrderedDelta<DocumentBin>>::value == 3,
              "OrderedDelta gained a member");
static_assert(AggregateMemberCount<DeltaItem<DocumentBin>>::value == 2,
              "DeltaItem gained a member");
static_assert(AggregateMemberCount<DeltaChange<DocumentBin>>::value == 2,
              "DeltaChange gained a member");
static_assert(AggregateMemberCount<KeyedDelta<Ulid>>::value == 3,
              "KeyedDelta gained a member");

// --- Compute ----------------------------------------------------------------

template <typename T>
bool DiffOrdered(const std::vector<T>& from, const std::vector<T>& to,
                 const std::string& what, std::vector<DeltaItem<T>>& removed,
                 std::vector<DeltaItem<T>>& added,
                 std::vector<std::pair<const T*, const T*>>& common,
                 std::string& error) {
    std::map<Ulid, size_t> fromIndex;
    std::map<Ulid, size_t> toIndex;
    for (size_t index = 0; index < from.size(); ++index)
        if (!fromIndex.emplace(from[index].id, index).second) {
            error = what + " contain a duplicate ID '" + from[index].id + "'";
            return false;
        }
    for (size_t index = 0; index < to.size(); ++index)
        if (!toIndex.emplace(to[index].id, index).second) {
            error = what + " contain a duplicate ID '" + to[index].id + "'";
            return false;
        }
    std::vector<Ulid> commonOrder;
    for (size_t index = 0; index < from.size(); ++index) {
        if (toIndex.count(from[index].id))
            commonOrder.push_back(from[index].id);
        else
            removed.push_back({index, from[index]});
    }
    size_t cursor = 0;
    for (size_t index = 0; index < to.size(); ++index) {
        const auto found = fromIndex.find(to[index].id);
        if (found == fromIndex.end()) {
            added.push_back({index, to[index]});
            continue;
        }
        if (commonOrder[cursor++] != to[index].id) {
            error = what + " were reordered";
            return false;
        }
        common.emplace_back(&from[found->second], &to[index]);
    }
    return true;
}

template <typename T>
bool DiffItems(const std::vector<T>& from, const std::vector<T>& to,
               const std::string& what, OrderedDelta<T>& delta,
               std::string& error) {
    std::vector<std::pair<const T*, const T*>> common;
    if (!DiffOrdered(from, to, what, delta.removed, delta.added, common, error))
        return false;
    for (const auto& pair : common)
        if (ItemJson(*pair.first) != ItemJson(*pair.second))
            delta.changed.push_back({*pair.first, *pair.second});
    return true;
}

bool DiffTimelines(const std::vector<DocumentSequence>& from,
                   const std::vector<DocumentSequence>& to, ProjectDelta& delta,
                   std::string& error) {
    std::vector<std::pair<const DocumentSequence*, const DocumentSequence*>>
        common;
    if (!DiffOrdered(from, to, "timelines", delta.removed_timelines,
                     delta.added_timelines, common, error))
        return false;
    for (const auto& pair : common) {
        const DocumentSequence& before = *pair.first;
        const DocumentSequence& after = *pair.second;
        TimelineDelta change;
        change.id = before.id;
        if (before.name != after.name)
            change.name = DeltaChange<std::string>{before.name, after.name};
        if (before.width != after.width)
            change.width = DeltaChange<int32_t>{before.width, after.width};
        if (before.height != after.height)
            change.height = DeltaChange<int32_t>{before.height, after.height};
        if (before.frame_rate.num != after.frame_rate.num ||
            before.frame_rate.den != after.frame_rate.den)
            change.frame_rate =
                DeltaChange<MediaRate>{before.frame_rate, after.frame_rate};
        if (ContentJson(before) != ContentJson(after))
            change.content = DeltaChange<DocumentSequence>{before, after};
        if (change.name || change.width || change.height || change.frame_rate ||
            change.content)
            delta.changed_timelines.push_back(std::move(change));
    }
    return true;
}

template <typename T, typename Text>
void DiffKeyed(const std::map<Ulid, T>& from, const std::map<Ulid, T>& to,
               Text text, std::vector<KeyedDelta<T>>& delta) {
    std::set<Ulid> keys;
    for (const auto& entry : from) keys.insert(entry.first);
    for (const auto& entry : to) keys.insert(entry.first);
    for (const Ulid& key : keys) {
        const auto before = from.find(key);
        const auto after = to.find(key);
        const bool hadKey = before != from.end();
        const bool hasKey = after != to.end();
        if (hadKey && hasKey && text(before->second) == text(after->second))
            continue;
        KeyedDelta<T> entry;
        entry.key = key;
        if (hadKey) entry.from = before->second;
        if (hasKey) entry.to = after->second;
        delta.push_back(std::move(entry));
    }
}

// --- Reverse ----------------------------------------------------------------

template <typename T>
std::optional<DeltaChange<T>> Swapped(
    const std::optional<DeltaChange<T>>& change) {
    if (!change) return std::nullopt;
    return DeltaChange<T>{change->to, change->from};
}

template <typename T>
OrderedDelta<T> Reversed(const OrderedDelta<T>& delta) {
    OrderedDelta<T> reversed;
    reversed.removed = delta.added;
    reversed.added = delta.removed;
    for (const DeltaChange<T>& change : delta.changed)
        reversed.changed.push_back({change.to, change.from});
    return reversed;
}

template <typename T>
std::vector<KeyedDelta<T>> Reversed(const std::vector<KeyedDelta<T>>& delta) {
    std::vector<KeyedDelta<T>> reversed;
    for (const KeyedDelta<T>& entry : delta)
        reversed.push_back({entry.key, entry.to, entry.from});
    return reversed;
}

// --- Apply ------------------------------------------------------------------

template <typename T>
typename std::vector<T>::iterator FindById(std::vector<T>& items,
                                           const Ulid& id) {
    return std::find_if(items.begin(), items.end(),
                        [&](const T& item) { return item.id == id; });
}

template <typename T>
bool ApplyRemovals(std::vector<T>& items,
                   const std::vector<DeltaItem<T>>& removed,
                   const std::string& what, std::string& error) {
    for (const DeltaItem<T>& entry : removed) {
        const auto found = FindById(items, entry.item.id);
        if (found == items.end()) {
            error =
                what + " '" + entry.item.id + "' is no longer in the project";
            return false;
        }
        items.erase(found);
    }
    return true;
}

template <typename T>
bool ApplyAdditions(std::vector<T>& items,
                    const std::vector<DeltaItem<T>>& added,
                    const std::string& what, std::string& error) {
    // Ascending target positions rebuild the resulting order exactly: every
    // item before a position is already in place when that position is
    // filled.
    for (const DeltaItem<T>& entry : added) {
        if (FindById(items, entry.item.id) != items.end()) {
            error = what + " '" + entry.item.id + "' is already in the project";
            return false;
        }
        if (entry.index > items.size()) {
            error = what + " '" + entry.item.id +
                    "' cannot be placed at position " +
                    std::to_string(entry.index);
            return false;
        }
        items.insert(items.begin() + static_cast<std::ptrdiff_t>(entry.index),
                     entry.item);
    }
    return true;
}

template <typename T>
bool ApplyChanges(std::vector<T>& items,
                  const std::vector<DeltaChange<T>>& changed,
                  const std::string& what, std::string& error) {
    for (const DeltaChange<T>& change : changed) {
        const auto found = FindById(items, change.to.id);
        if (found == items.end()) {
            error =
                what + " '" + change.to.id + "' is no longer in the project";
            return false;
        }
        *found = change.to;
    }
    return true;
}

template <typename T>
bool ApplyOrdered(std::vector<T>& items, const OrderedDelta<T>& delta,
                  const std::string& what, std::string& error) {
    return ApplyRemovals(items, delta.removed, what, error) &&
           ApplyChanges(items, delta.changed, what, error) &&
           ApplyAdditions(items, delta.added, what, error);
}

bool ApplyTimelineChanges(std::vector<DocumentSequence>& timelines,
                          const std::vector<TimelineDelta>& changes,
                          std::string& error) {
    for (const TimelineDelta& change : changes) {
        const auto found = FindById(timelines, change.id);
        if (found == timelines.end()) {
            error = "timeline '" + change.id + "' is no longer in the project";
            return false;
        }
        if (change.content) {
            // Take everything from the recorded content and keep the header
            // this timeline has now: a field DocumentSequence gains later is
            // content by default, and is carried rather than dropped.
            DocumentSequence replacement = change.content->to;
            replacement.id = found->id;
            replacement.name = found->name;
            replacement.width = found->width;
            replacement.height = found->height;
            replacement.frame_rate = found->frame_rate;
            *found = std::move(replacement);
        }
        if (change.name) found->name = change.name->to;
        if (change.width) found->width = change.width->to;
        if (change.height) found->height = change.height->to;
        if (change.frame_rate) found->frame_rate = change.frame_rate->to;
    }
    return true;
}

template <typename T>
void ApplyKeyed(std::map<Ulid, T>& map,
                const std::vector<KeyedDelta<T>>& delta) {
    for (const KeyedDelta<T>& entry : delta) {
        if (entry.to)
            map[entry.key] = *entry.to;
        else
            map.erase(entry.key);
    }
}

// --- Serialize --------------------------------------------------------------

template <typename T, typename Write>
void WriteChange(std::ostringstream& output,
                 const std::optional<DeltaChange<T>>& change, Write write) {
    if (!change) {
        output << "null";
        return;
    }
    output << "{\"from\":" << write(change->from)
           << ",\"to\":" << write(change->to) << '}';
}

template <typename T>
void WriteOrdered(std::ostringstream& output,
                  const std::vector<DeltaItem<T>>& removed,
                  const std::vector<DeltaItem<T>>& added) {
    const auto items = [&](const std::vector<DeltaItem<T>>& entries) {
        output << '[';
        for (size_t index = 0; index < entries.size(); ++index) {
            if (index) output << ',';
            output << "{\"index\":" << entries[index].index
                   << ",\"item\":" << ItemJson(entries[index].item) << '}';
        }
        output << ']';
    };
    output << "{\"removed\":";
    items(removed);
    output << ",\"added\":";
    items(added);
}

template <typename T>
void WriteItems(std::ostringstream& output, const OrderedDelta<T>& delta) {
    WriteOrdered(output, delta.removed, delta.added);
    output << ",\"changed\":[";
    for (size_t index = 0; index < delta.changed.size(); ++index) {
        if (index) output << ',';
        output << "{\"from\":" << ItemJson(delta.changed[index].from)
               << ",\"to\":" << ItemJson(delta.changed[index].to) << '}';
    }
    output << "]}";
}

template <typename T, typename Text>
void WriteKeyed(std::ostringstream& output,
                const std::vector<KeyedDelta<T>>& delta, Text text) {
    output << '[';
    for (size_t index = 0; index < delta.size(); ++index) {
        const KeyedDelta<T>& entry = delta[index];
        if (index) output << ',';
        output << "{\"key\":" << QuotedJson(entry.key)
               << ",\"from\":" << (entry.from ? text(*entry.from) : "null")
               << ",\"to\":" << (entry.to ? text(*entry.to) : "null") << '}';
    }
    output << ']';
}

// --- Deserialize ------------------------------------------------------------

const Value& Field(const Value& object, const char* key,
                   const std::string& context) {
    const Value* value = object.Find(key);
    if (!value) throw std::runtime_error(context + " has no '" + key + "'");
    return *value;
}

void ExpectKeys(const Value& object, std::initializer_list<const char*> keys,
                const std::string& context) {
    if (!object.IsObject() || object.AsObject().size() != keys.size())
        throw std::runtime_error(context + " has unexpected or missing fields");
    for (const char* key : keys) Field(object, key, context);
}

int64_t Integer(const Value& value, const std::string& context) {
    int64_t output = 0;
    if (!value.AsInt64(output))
        throw std::runtime_error(context + " must be an integer");
    return output;
}

int32_t Integer32(const Value& value, const std::string& context) {
    const int64_t output = Integer(value, context);
    if (output < std::numeric_limits<int32_t>::min() ||
        output > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(context + " is outside int32_t range");
    return static_cast<int32_t>(output);
}

std::string String(const Value& value, const std::string& context) {
    if (!value.IsString())
        throw std::runtime_error(context + " must be a string");
    return value.AsString();
}

const std::vector<Value>& Array(const Value& value,
                                const std::string& context) {
    if (!value.IsArray())
        throw std::runtime_error(context + " must be an array");
    return value.AsArray();
}

Document Parts(const document_json::DocumentParts& parts,
               const std::string& context) {
    Document document;
    std::string error;
    if (!document_json::ParseDocumentParts(parts, document, error))
        throw std::runtime_error(context + " is invalid: " + error);
    return document;
}

template <typename T>
T ReadItem(const Value& value, const std::string& context);

template <>
DocumentSequence ReadItem<DocumentSequence>(const Value& value,
                                            const std::string& context) {
    document_json::DocumentParts parts;
    parts.sequence = value.Dump();
    return Parts(parts, context).sequence;
}

template <>
LibraryMedia ReadItem<LibraryMedia>(const Value& value,
                                    const std::string& context) {
    document_json::DocumentParts parts;
    parts.library.push_back(value.Dump());
    return Parts(parts, context).library.at(0);
}

template <>
DocumentBin ReadItem<DocumentBin>(const Value& value,
                                  const std::string& context) {
    document_json::DocumentParts parts;
    parts.bins.push_back(value.Dump());
    return Parts(parts, context).bins.at(0);
}

template <>
DocumentSource ReadItem<DocumentSource>(const Value& value,
                                        const std::string& context) {
    document_json::DocumentParts parts;
    parts.sources.push_back(value.Dump());
    return Parts(parts, context).sources.at(0);
}

ColorManagementSettings ReadColorManagement(const Value& value,
                                            const std::string& context) {
    document_json::DocumentParts parts;
    parts.color_management = value.Dump();
    return Parts(parts, context).color_management;
}

MediaRate ReadRate(const Value& value, const std::string& context) {
    MediaRate rate;
    std::string error;
    if (!document_json::ParseMediaRate(value.Dump(), context, rate, error))
        throw std::runtime_error(error);
    return rate;
}

ProjectBinMetadata ReadMetadata(const Value& value,
                                const std::string& context) {
    ProjectBinMetadata metadata;
    std::string error;
    if (!document_json::ParseBinMetadata(value.Dump(), context, metadata,
                                         error))
        throw std::runtime_error(error);
    return metadata;
}

template <typename T, typename Read>
std::optional<DeltaChange<T>> ReadChange(const Value& value, Read read,
                                         const std::string& context) {
    if (value.IsNull()) return std::nullopt;
    ExpectKeys(value, {"from", "to"}, context);
    return DeltaChange<T>{
        read(Field(value, "from", context), context + ".from"),
        read(Field(value, "to", context), context + ".to")};
}

template <typename T>
std::vector<DeltaItem<T>> ReadIndexedItems(const Value& value,
                                           const std::string& context) {
    std::vector<DeltaItem<T>> items;
    const std::vector<Value>& array = Array(value, context);
    for (size_t index = 0; index < array.size(); ++index) {
        const std::string itemContext =
            context + "[" + std::to_string(index) + "]";
        ExpectKeys(array[index], {"index", "item"}, itemContext);
        const int64_t position =
            Integer(Field(array[index], "index", itemContext), itemContext);
        if (position < 0 ||
            (!items.empty() &&
             static_cast<uint64_t>(position) <= items.back().index))
            throw std::runtime_error(itemContext +
                                     ".index must be ascending and positive");
        items.push_back({static_cast<size_t>(position),
                         ReadItem<T>(Field(array[index], "item", itemContext),
                                     itemContext + ".item")});
    }
    return items;
}

template <typename T>
OrderedDelta<T> ReadItems(const Value& value, const std::string& context) {
    ExpectKeys(value, {"removed", "added", "changed"}, context);
    OrderedDelta<T> delta;
    delta.removed = ReadIndexedItems<T>(Field(value, "removed", context),
                                        context + ".removed");
    delta.added =
        ReadIndexedItems<T>(Field(value, "added", context), context + ".added");
    const std::vector<Value>& changed =
        Array(Field(value, "changed", context), context + ".changed");
    for (size_t index = 0; index < changed.size(); ++index) {
        const std::string itemContext =
            context + ".changed[" + std::to_string(index) + "]";
        ExpectKeys(changed[index], {"from", "to"}, itemContext);
        DeltaChange<T> change{
            ReadItem<T>(Field(changed[index], "from", itemContext),
                        itemContext + ".from"),
            ReadItem<T>(Field(changed[index], "to", itemContext),
                        itemContext + ".to")};
        if (change.from.id != change.to.id)
            throw std::runtime_error(itemContext + " changes an item's ID");
        delta.changed.push_back(std::move(change));
    }
    return delta;
}

template <typename T, typename Read>
std::vector<KeyedDelta<T>> ReadKeyed(const Value& value, Read read,
                                     const std::string& context) {
    std::vector<KeyedDelta<T>> delta;
    const std::vector<Value>& array = Array(value, context);
    for (size_t index = 0; index < array.size(); ++index) {
        const std::string itemContext =
            context + "[" + std::to_string(index) + "]";
        ExpectKeys(array[index], {"key", "from", "to"}, itemContext);
        KeyedDelta<T> entry;
        entry.key = String(Field(array[index], "key", itemContext),
                           itemContext + ".key");
        const Value& from = Field(array[index], "from", itemContext);
        const Value& to = Field(array[index], "to", itemContext);
        if (!from.IsNull()) entry.from = read(from, itemContext + ".from");
        if (!to.IsNull()) entry.to = read(to, itemContext + ".to");
        delta.push_back(std::move(entry));
    }
    return delta;
}

}  // namespace

bool ComputeProjectDelta(const Project& from, const Project& to,
                         ProjectDelta& output, std::string& error) {
    if (from.id != to.id) {
        error = "project states belong to different projects";
        return false;
    }
    // MOTION-2026-09 M3 -- compositions are edited by timeline operations,
    // whose journals carry them; no project operation changes one. Should
    // one ever do, the delta refuses and the entry keeps whole states, which
    // do carry them, instead of replaying into a project that lost the edit.
    const auto compositionsJson = [](const Project& project) {
        std::string json;
        for (const DocumentComposition& composition : project.compositions) {
            json += composition.id;
            json += composition.name;
            json += composition.definition.Dump();
            json += '\n';
        }
        return json;
    };
    if (compositionsJson(from) != compositionsJson(to)) {
        error = "compositions changed: a project delta does not carry them";
        return false;
    }
    ProjectDelta delta;
    if (from.name != to.name)
        delta.name = DeltaChange<std::string>{from.name, to.name};
    if (from.active_timeline_id != to.active_timeline_id)
        delta.active_timeline_id =
            DeltaChange<Ulid>{from.active_timeline_id, to.active_timeline_id};
    if (document_json::ColorManagementJson(from.settings.color_management) !=
        document_json::ColorManagementJson(to.settings.color_management))
        delta.color_management = DeltaChange<ColorManagementSettings>{
            from.settings.color_management, to.settings.color_management};
    if (!DiffTimelines(from.timelines, to.timelines, delta, error) ||
        !DiffItems(from.rushes, to.rushes, "rushes", delta.rushes, error) ||
        !DiffItems(from.bins, to.bins, "bins", delta.bins, error) ||
        !DiffItems(from.sources, to.sources, "sources", delta.sources, error))
        return false;
    DiffKeyed(from.bin_metadata, to.bin_metadata, MetadataJson,
              delta.bin_metadata);
    DiffKeyed(
        from.timeline_bin_ids, to.timeline_bin_ids,
        [](const Ulid& value) { return value; }, delta.timeline_bin_ids);
    output = std::move(delta);
    error.clear();
    return true;
}

ProjectDelta ReverseProjectDelta(const ProjectDelta& delta) {
    ProjectDelta reversed;
    reversed.name = Swapped(delta.name);
    reversed.active_timeline_id = Swapped(delta.active_timeline_id);
    reversed.color_management = Swapped(delta.color_management);
    reversed.removed_timelines = delta.added_timelines;
    reversed.added_timelines = delta.removed_timelines;
    for (const TimelineDelta& change : delta.changed_timelines) {
        TimelineDelta item;
        item.id = change.id;
        item.name = Swapped(change.name);
        item.width = Swapped(change.width);
        item.height = Swapped(change.height);
        item.frame_rate = Swapped(change.frame_rate);
        item.content = Swapped(change.content);
        reversed.changed_timelines.push_back(std::move(item));
    }
    reversed.rushes = Reversed(delta.rushes);
    reversed.bins = Reversed(delta.bins);
    reversed.sources = Reversed(delta.sources);
    reversed.bin_metadata = Reversed(delta.bin_metadata);
    reversed.timeline_bin_ids = Reversed(delta.timeline_bin_ids);
    return reversed;
}

bool ApplyProjectDelta(Project& project, const ProjectDelta& delta,
                       std::string& error) {
    if (delta.name) project.name = delta.name->to;
    if (delta.active_timeline_id)
        project.active_timeline_id = delta.active_timeline_id->to;
    if (delta.color_management)
        project.settings.color_management = delta.color_management->to;
    if (!ApplyRemovals(project.timelines, delta.removed_timelines, "timeline",
                       error) ||
        !ApplyTimelineChanges(project.timelines, delta.changed_timelines,
                              error) ||
        !ApplyAdditions(project.timelines, delta.added_timelines, "timeline",
                        error) ||
        !ApplyOrdered(project.rushes, delta.rushes, "rush", error) ||
        !ApplyOrdered(project.bins, delta.bins, "bin", error) ||
        !ApplyOrdered(project.sources, delta.sources, "source", error))
        return false;
    ApplyKeyed(project.bin_metadata, delta.bin_metadata);
    ApplyKeyed(project.timeline_bin_ids, delta.timeline_bin_ids);
    error.clear();
    return true;
}

std::string SerializeProjectDelta(const ProjectDelta& delta) {
    std::ostringstream output;
    output << "{\"name\":";
    WriteChange(output, delta.name, QuotedJson);
    output << ",\"active_timeline_id\":";
    WriteChange(output, delta.active_timeline_id, QuotedJson);
    output << ",\"color_management\":";
    WriteChange(output, delta.color_management,
                document_json::ColorManagementJson);
    output << ",\"timelines\":";
    WriteOrdered(output, delta.removed_timelines, delta.added_timelines);
    output << ",\"changed\":[";
    for (size_t index = 0; index < delta.changed_timelines.size(); ++index) {
        const TimelineDelta& change = delta.changed_timelines[index];
        if (index) output << ',';
        output << "{\"id\":" << QuotedJson(change.id) << ",\"name\":";
        WriteChange(output, change.name, QuotedJson);
        output << ",\"width\":";
        WriteChange(output, change.width,
                    [](int32_t value) { return std::to_string(value); });
        output << ",\"height\":";
        WriteChange(output, change.height,
                    [](int32_t value) { return std::to_string(value); });
        output << ",\"frame_rate\":";
        WriteChange(output, change.frame_rate, RateJson);
        output << ",\"content\":";
        WriteChange(output, change.content, [](const DocumentSequence& value) {
            return ItemJson(value);
        });
        output << '}';
    }
    output << "]},\"rushes\":";
    WriteItems(output, delta.rushes);
    output << ",\"bins\":";
    WriteItems(output, delta.bins);
    output << ",\"sources\":";
    WriteItems(output, delta.sources);
    output << ",\"bin_metadata\":";
    WriteKeyed(output, delta.bin_metadata, MetadataJson);
    output << ",\"timeline_bin_ids\":";
    WriteKeyed(output, delta.timeline_bin_ids, QuotedJson);
    output << '}';
    return output.str();
}

bool DeserializeProjectDelta(const std::string& json, ProjectDelta& output,
                             std::string& error) {
    try {
        Value root;
        if (!Value::Parse(json, root, error))
            throw std::runtime_error("project delta: " + error);
        const std::string context = "project delta";
        ExpectKeys(
            root,
            {"name", "active_timeline_id", "color_management", "timelines",
             "rushes", "bins", "sources", "bin_metadata", "timeline_bin_ids"},
            context);
        ProjectDelta delta;
        delta.name = ReadChange<std::string>(Field(root, "name", context),
                                             String, context + ".name");
        delta.active_timeline_id =
            ReadChange<Ulid>(Field(root, "active_timeline_id", context), String,
                             context + ".active_timeline_id");
        delta.color_management = ReadChange<ColorManagementSettings>(
            Field(root, "color_management", context), ReadColorManagement,
            context + ".color_management");

        const std::string timelinesContext = context + ".timelines";
        const Value& timelines = Field(root, "timelines", context);
        ExpectKeys(timelines, {"removed", "added", "changed"},
                   timelinesContext);
        delta.removed_timelines = ReadIndexedItems<DocumentSequence>(
            Field(timelines, "removed", timelinesContext),
            timelinesContext + ".removed");
        delta.added_timelines = ReadIndexedItems<DocumentSequence>(
            Field(timelines, "added", timelinesContext),
            timelinesContext + ".added");
        const std::vector<Value>& changed =
            Array(Field(timelines, "changed", timelinesContext),
                  timelinesContext + ".changed");
        for (size_t index = 0; index < changed.size(); ++index) {
            const std::string itemContext =
                timelinesContext + ".changed[" + std::to_string(index) + "]";
            const Value& item = changed[index];
            ExpectKeys(
                item,
                {"id", "name", "width", "height", "frame_rate", "content"},
                itemContext);
            TimelineDelta change;
            change.id = String(Field(item, "id", itemContext), itemContext);
            change.name =
                ReadChange<std::string>(Field(item, "name", itemContext),
                                        String, itemContext + ".name");
            change.width =
                ReadChange<int32_t>(Field(item, "width", itemContext),
                                    Integer32, itemContext + ".width");
            change.height =
                ReadChange<int32_t>(Field(item, "height", itemContext),
                                    Integer32, itemContext + ".height");
            change.frame_rate =
                ReadChange<MediaRate>(Field(item, "frame_rate", itemContext),
                                      ReadRate, itemContext + ".frame_rate");
            change.content = ReadChange<DocumentSequence>(
                Field(item, "content", itemContext), ReadItem<DocumentSequence>,
                itemContext + ".content");
            delta.changed_timelines.push_back(std::move(change));
        }

        delta.rushes = ReadItems<LibraryMedia>(Field(root, "rushes", context),
                                               context + ".rushes");
        delta.bins = ReadItems<DocumentBin>(Field(root, "bins", context),
                                            context + ".bins");
        delta.sources = ReadItems<DocumentSource>(
            Field(root, "sources", context), context + ".sources");
        delta.bin_metadata = ReadKeyed<ProjectBinMetadata>(
            Field(root, "bin_metadata", context), ReadMetadata,
            context + ".bin_metadata");
        delta.timeline_bin_ids =
            ReadKeyed<Ulid>(Field(root, "timeline_bin_ids", context), String,
                            context + ".timeline_bin_ids");
        output = std::move(delta);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}
