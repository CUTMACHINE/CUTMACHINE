// FORMAT-2026-09 -- ROADMAP.md. The journal format of every operation,
// pinned.
//
// Edit logs and project journals keep operations as positional JSON, and a
// project reopened by a newer build replays them as they were written. The
// round-trip checks elsewhere prove that the reader accepts what the writer
// emits; a change that moved both sides together would pass them and still
// leave every existing journal unreadable, or read it differently.
//
// The fixtures were produced by the hand-written codec, before anything
// replaced it:
//
//   goldens.jsonl   operations of every type -- fully populated, default
//                   and random -- as that codec wrote them. Each must read
//                   back and write out to the same bytes.
//   omissions.tsv   for the first golden of each type, every key whose
//                   removal that codec still accepted, and what it then
//                   wrote. This pins the tolerance of the reader for
//                   journals written before a field existed: accepting
//                   less breaks old journals, accepting more is a decision
//                   that must show up in this file.
//
// Map entries (effect params, clone ID maps) appear in omissions.tsv too:
// removing one is a smaller map, not a missing field.
//
// BENCH-2026-09 added, after the fact:
//
//   goldens.jsonl   its last lines, text holding control characters in the
//                   JSON spelling the writer adopted on 18 September 2026.
//   tolerances.jsonl
//                   spellings the reader accepts and the writer never
//                   produces -- raw control characters, from every journal
//                   written before that date, and the escapes other JSON
//                   writers use -- each with the operation it reads as.

#include "EditLog.h"
#include "Json.h"
#include "OperationFormat.h"
#include "Operations.h"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <type_traits>
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

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

using mcp_json::Value;

const std::string kFixtures = OPERATION_FORMAT_FIXTURES_DIR;

std::vector<std::string> Lines(const std::string& name) {
    std::ifstream input(kFixtures + "/" + name);
    if (!input) throw std::runtime_error("missing fixture " + name);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

// Reads `json` as a timeline operation, else as a project operation, and
// writes it back. Returns false when neither reader accepts it.
bool Rewrite(const std::string& json, std::string& output) {
    EditError error = EditError::None;
    std::string message;
    Operation timeline;
    if (DeserializeOperation(json, timeline, error, message)) {
        output = SerializeOperation(timeline);
        return true;
    }
    ProjectOperation project;
    if (DeserializeProjectOperation(json, project, error, message)) {
        output = SerializeProjectOperation(project);
        return true;
    }
    return false;
}

std::string TypeOf(const std::string& json) {
    Value value;
    std::string error;
    if (!Value::Parse(json, value, error) || !value.Find("type"))
        throw std::runtime_error("fixture line is not an operation: " + json);
    return value.Find("type")->AsString();
}

// Default-constructs every alternative of a variant.
template <typename... Alternatives>
std::vector<std::variant<Alternatives...>> EveryAlternative(
    const std::variant<Alternatives...>*) {
    return {std::variant<Alternatives...>(Alternatives{})...};
}

struct Step {
    std::string key;  // empty: first element of an array
};

void Paths(const Value& value, std::vector<Step>& prefix,
           std::vector<std::vector<Step>>& out) {
    if (value.IsObject()) {
        for (const auto& [key, child] : value.AsObject()) {
            if (prefix.empty() && key == "type") continue;
            prefix.push_back({key});
            out.push_back(prefix);
            // A project delta has its own strict format (ProjectDelta.h),
            // and a motion tree is the motion format's, which the codec
            // carries as it is (MOTION-2026-09 M3), a clip's instance props
            // included (M4).
            static const std::set<std::string> kOpaque = {
                "exact_project_delta", "definition", "node",        "value",
                "animation",           "props",      "motion_props"};
            if (kOpaque.count(key) == 0) {
                Paths(child, prefix, out);
            }
            prefix.pop_back();
        }
    } else if (value.IsArray() && !value.AsArray().empty()) {
        prefix.push_back({""});
        Paths(value.AsArray()[0], prefix, out);
        prefix.pop_back();
    }
}

Value Remove(const Value& value, const std::vector<Step>& path, size_t depth) {
    if (value.IsObject()) {
        Value copy = Value::MakeObject();
        for (const auto& [key, child] : value.AsObject()) {
            if (key != path[depth].key) {
                copy.Set(key, child);
            } else if (depth + 1 < path.size()) {
                copy.Set(key, Remove(child, path, depth + 1));
            }
        }
        return copy;
    }
    if (value.IsArray()) {
        Value copy = Value::MakeArray();
        for (size_t index = 0; index < value.AsArray().size(); ++index) {
            copy.Push(index == 0 ? Remove(value.AsArray()[0], path, depth + 1)
                                 : value.AsArray()[index]);
        }
        return copy;
    }
    return value;
}

std::string Label(const std::vector<Step>& path) {
    std::string label;
    for (const Step& step : path) {
        if (step.key.empty()) {
            label += "[0]";
        } else {
            if (!label.empty()) label += '.';
            label += step.key;
        }
    }
    return label;
}

// ---------------------------------------------------------------------
// A value of every shape a table describes, so that a round trip through
// the journal can be compared with what went in. Filling from the tables
// rather than by hand is what makes a newly added operation covered the day
// it is added.
// ---------------------------------------------------------------------

namespace generator {

using namespace operation_format;

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(uint64_t seed) : engine(seed) {}
    int64_t Pick(int64_t low, int64_t high) {
        return std::uniform_int_distribution<int64_t>(low, high)(engine);
    }
    bool Coin() { return Pick(0, 1) == 1; }
};

// Populated leaves every optional present and every list non-empty, so that
// a field the codec forgets shows up as a difference.
enum class Mode { Populated, Random };

struct Context {
    Rng rng;
    Mode mode;
    int counter = 0;
};

void Fill(Context& context, std::string& value) {
    // BENCH-2026-09 -- a line feed and a control character, since the
    // writer now escapes them and a round trip must restore them.
    static const char* pieces[] = {"A", "B", "Z", "0",  "9",  " ",  "é",
                                   "-", "_", "x", "\"", "\\", "\n", "\x01"};
    constexpr int64_t kLastPiece = sizeof(pieces) / sizeof(pieces[0]) - 1;
    const int64_t length = context.mode == Mode::Populated
                               ? context.rng.Pick(1, 5)
                               : context.rng.Pick(0, 5);
    value.clear();
    for (int64_t index = 0; index < length; ++index)
        value += pieces[context.rng.Pick(0, kLastPiece)];
    if (context.mode == Mode::Populated)
        value += std::to_string(++context.counter);
}

void Fill(Context& context, bool& value) {
    value = context.mode == Mode::Populated ? true : context.rng.Coin();
}

void Fill(Context& context, int32_t& value) {
    value = static_cast<int32_t>(context.rng.Pick(-100000, 100000));
    if (context.mode == Mode::Populated && value == 0) value = 7;
}

void Fill(Context& context, int64_t& value) {
    value = context.rng.Pick(-1000000000000LL, 1000000000000LL);
    if (context.mode == Mode::Populated && value == 0) value = 11;
}

void Fill(Context& context, uint32_t& value) {
    value = static_cast<uint32_t>(context.rng.Pick(0, 4000000000LL));
}

void Fill(Context& context, uint64_t& value) {
    value = static_cast<uint64_t>(context.rng.Pick(0, 1000000000000000LL));
}

void Fill(Context& context, RationalTime& value) {
    static const int32_t rates[] = {1, 24, 25, 30, 1001, 30000, 48000};
    value.value = context.rng.Pick(-1000000, 1000000);
    value.rate = rates[context.rng.Pick(0, 6)];
    if (context.mode == Mode::Populated && value.value == 0) value.value = 13;
}

void Fill(Context& context, ExactProjectState& value);
void Fill(Context& context, Operation& value);

// MOTION-2026-09 M3 -- a motion tree is opaque to the codec: any JSON value,
// decimals, exponents and nesting included, comes back as it was written.
// Never null at the top: an optional one reads null as absent.
void Fill(Context& context, mcp_json::Value& value) {
    std::string text;
    Fill(context, text);
    if (context.mode == Mode::Random && context.rng.Coin()) {
        value = context.rng.Coin()
                    ? Value::MakeString(text)
                    : Value::MakeRawNumber(
                          std::to_string(context.rng.Pick(-99, 99)) + ".5");
        return;
    }
    Value tree = Value::MakeObject();
    tree.Set("text", Value::MakeString(text));
    tree.Set("number",
             Value::MakeRawNumber(
                 std::to_string(context.rng.Pick(-1000, 1000)) + ".25"));
    Value list = Value::MakeArray();
    list.Push(Value::MakeRawNumber("-2.5e3"));
    list.Push(Value::MakeBool(context.rng.Coin()));
    list.Push(Value::MakeNull());
    list.Push(Value::MakeInt(context.rng.Pick(0, 1000)));
    tree.Set("list", std::move(list));
    value = std::move(tree);
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void Fill(Context& context, Enum& value);
template <typename T>
void Fill(Context& context, std::vector<T>& values);
template <typename T>
void Fill(Context& context, std::optional<T>& value);
template <typename V>
void Fill(Context& context, std::map<std::string, V>& values);
template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void Fill(Context& context, T& value);

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int>>
void Fill(Context& context, Enum& value) {
    const auto& names = EnumNames<Enum>::kNames;
    const int64_t count = sizeof(names) / sizeof(names[0]);
    value = names[context.rng.Pick(0, count - 1)].first;
}

template <typename T>
void Fill(Context& context, std::vector<T>& values) {
    const int64_t count =
        context.mode == Mode::Populated ? 1 : context.rng.Pick(0, 2);
    values.assign(static_cast<size_t>(count), T{});
    for (T& element : values) Fill(context, element);
}

template <typename T>
void Fill(Context& context, std::optional<T>& value) {
    if (context.mode == Mode::Random && context.rng.Coin()) {
        value.reset();
        return;
    }
    T present{};
    Fill(context, present);
    value = std::move(present);
}

template <typename V>
void Fill(Context& context, std::map<std::string, V>& values) {
    const int64_t count =
        context.mode == Mode::Populated ? 1 : context.rng.Pick(0, 2);
    values.clear();
    for (int64_t index = 0; index < count; ++index) {
        std::string key;
        Fill(context, key);
        key += "#" + std::to_string(index);
        V item{};
        Fill(context, item);
        values.emplace(key, std::move(item));
    }
}

// The couplings the tables express: a member the journal only spells when
// another one says so cannot be compared unless the value respects them.
// A new coupling that lands without a line here fails this test rather than
// going unnoticed.
void Settle(DocumentClip& clip) {
    if (clip.sync_anchor_clip_id.empty()) clip.sync_reference_delta = {0, 1};
}
void Settle(LibraryMedia& media) {
    if (!media.audio_level_measured) media.audio_level = 0;
}
template <typename T>
void Settle(T&) {}

template <typename T, std::enable_if_t<IsTabled<T>::value, int>>
void Fill(Context& context, T& value) {
    ForEachField<T>(
        [&](const auto& field) { Fill(context, value.*field.member); });
    Settle(value);
}

// PL1 -- a batch member: one of the three kinds the ticket names
// (insertion, trim, audio setting), filled from its own table. A batch never
// holds a batch.
void Fill(Context& context, Operation& value) {
    switch (context.rng.Pick(0, 2)) {
        case 0: {
            InsertClipOperation member;
            Fill(context, member);
            value = std::move(member);
            break;
        }
        case 1: {
            TrimClipOperation member;
            Fill(context, member);
            value = std::move(member);
            break;
        }
        default: {
            SetClipAudioOperation member;
            Fill(context, member);
            value = std::move(member);
            break;
        }
    }
}

// Either shape the journal gives this member: a whole project, hex-encoded,
// or the delta it amounts to. Its content is opaque to the codec.
void Fill(Context& context, ExactProjectState& value) {
    value = {};
    if (context.mode == Mode::Random && context.rng.Coin()) {
        ProjectDelta delta;
        std::string before;
        std::string after;
        Fill(context, before);
        Fill(context, after);
        delta.name = DeltaChange<std::string>{before, after};
        value.delta = std::move(delta);
        return;
    }
    value.canonical_json =
        "{\"project\":" + std::to_string(context.rng.Pick(1, 1000000)) + "}";
}

}  // namespace generator

// Typed access to the two journals, so that a round trip can be compared
// with the value it started from.
template <typename Variant, typename T>
struct IsAlternative;
template <typename... Alternatives, typename T>
struct IsAlternative<std::variant<Alternatives...>, T>
    : std::bool_constant<(std::is_same_v<Alternatives, T> || ...)> {};

template <typename T,
          std::enable_if_t<IsAlternative<Operation, T>::value, int> = 0>
std::string Write(const T& value) {
    return SerializeOperation(Operation(value));
}

template <typename T,
          std::enable_if_t<IsAlternative<ProjectOperation, T>::value, int> = 0>
std::string Write(const T& value) {
    return SerializeProjectOperation(ProjectOperation(value));
}

template <typename T,
          std::enable_if_t<IsAlternative<Operation, T>::value, int> = 0>
std::optional<T> Parse(const std::string& json, std::string& message) {
    Operation parsed;
    EditError error = EditError::None;
    if (!DeserializeOperation(json, parsed, error, message))
        return std::nullopt;
    if (const T* held = std::get_if<T>(&parsed)) return *held;
    message = "read back as another operation";
    return std::nullopt;
}

template <typename T,
          std::enable_if_t<IsAlternative<ProjectOperation, T>::value, int> = 0>
std::optional<T> Parse(const std::string& json, std::string& message) {
    ProjectOperation parsed;
    EditError error = EditError::None;
    if (!DeserializeProjectOperation(json, parsed, error, message))
        return std::nullopt;
    if (const T* held = std::get_if<T>(&parsed)) return *held;
    message = "read back as another operation";
    return std::nullopt;
}

const Ulid kTrack = "01K20000000000000000000003";
const Ulid kSource = "01K20000000000000000000001";
const Ulid kClip = "01K20000000000000000000004";
const Ulid kCaptionGroup = "01K20000000000000000000009";

}  // namespace

int main() {
    Test("every golden reads back and writes out to the same bytes", [] {
        const auto goldens = Lines("goldens.jsonl");
        Check(goldens.size() > 300, "goldens.jsonl looks truncated");
        for (const std::string& golden : goldens) {
            std::string rewritten;
            if (!Rewrite(golden, rewritten)) {
                Check(false, "golden no longer reads: " + golden);
            } else {
                Check(rewritten == golden, "golden rewrote differently:\n  " +
                                               golden + "\n  " + rewritten);
            }
        }
    });

    Test("every registered operation type has goldens", [] {
        std::map<std::string, int> counts;
        for (const std::string& golden : Lines("goldens.jsonl"))
            ++counts[TypeOf(golden)];
        // An operation added without goldens is not pinned by anything, so
        // the failure hands over the lines to paste rather than a chore.
        const auto pinned = [&counts](const auto& sample,
                                      const std::string& type) {
            if (counts[type] >= 3) return;
            std::string proposal;
            std::visit(
                [&](const auto& held) {
                    uint64_t seed = 1;
                    for (const generator::Mode mode :
                         {generator::Mode::Populated, generator::Mode::Random,
                          generator::Mode::Random}) {
                        auto value = held;
                        generator::Context context{generator::Rng(++seed),
                                                   mode};
                        generator::Fill(context, value);
                        proposal += "  " + Write(value) + "\n";
                    }
                    proposal += "  " + Write(held) + "\n";
                },
                sample);
            Check(false, "nothing pins the format of " + type +
                             "; add to goldens.jsonl:\n" + proposal);
        };
        for (const Operation& value :
             EveryAlternative(static_cast<const Operation*>(nullptr))) {
            pinned(value, TypeOf(SerializeOperation(value)));
        }
        for (const ProjectOperation& value :
             EveryAlternative(static_cast<const ProjectOperation*>(nullptr))) {
            pinned(value, TypeOf(SerializeProjectOperation(value)));
        }
    });

    // PRD35 -- ROADMAP.md. describe's `history` names an entry without
    // writing it: by the name the journal writes, for every type.
    Test("OperationTypeName is the type the journal writes", [] {
        for (const Operation& value :
             EveryAlternative(static_cast<const Operation*>(nullptr))) {
            const std::string written = TypeOf(SerializeOperation(value));
            Check(OperationTypeName(value) == written,
                  "OperationTypeName says '" + OperationTypeName(value) +
                      "' where the journal writes '" + written + "'");
        }
    });

    Test("the reader tolerates exactly the recorded omissions", [] {
        std::map<std::pair<std::string, std::string>, std::string> recorded;
        for (const std::string& line : Lines("omissions.tsv")) {
            const size_t first = line.find('\t');
            const size_t second = line.find('\t', first + 1);
            Check(first != std::string::npos && second != std::string::npos,
                  "malformed omissions.tsv line: " + line);
            recorded[{line.substr(0, first),
                      line.substr(first + 1, second - first - 1)}] =
                line.substr(second + 1);
        }

        std::set<std::string> seen;
        std::set<std::pair<std::string, std::string>> visited;
        for (const std::string& golden : Lines("goldens.jsonl")) {
            const std::string type = TypeOf(golden);
            if (!seen.insert(type).second) continue;  // first golden only
            Value tree;
            std::string error;
            Check(Value::Parse(golden, tree, error) && tree.Dump() == golden,
                  type + " golden does not survive the JSON tree");
            std::vector<std::vector<Step>> paths;
            std::vector<Step> prefix;
            Paths(tree, prefix, paths);
            for (const auto& path : paths) {
                // Exact-track snapshots share one codec; ClearClip and
                // AddTrack stand for all of them.
                if (type != "ClearClip" && type != "AddTrack" &&
                    path.size() > 1 && path[0].key == "exact_tracks")
                    continue;
                // PL1 -- a batch member is read by its own type's codec,
                // whose omissions are pinned under that type.
                if (type == "Batch" && path.size() > 1 &&
                    path[0].key == "operations")
                    continue;
                const auto key = std::make_pair(type, Label(path));
                const auto found = recorded.find(key);
                std::string rewritten;
                const bool accepted =
                    Rewrite(Remove(tree, path, 0).Dump(), rewritten);
                if (found == recorded.end()) {
                    Check(!accepted,
                          type + " now tolerates a missing " + key.second +
                              "; if that is intended, add to "
                              "omissions.tsv:\n  " +
                              type + "\t" + key.second + "\t" + rewritten);
                } else {
                    visited.insert(key);
                    Check(accepted, type + " no longer tolerates a missing " +
                                        key.second);
                    Check(!accepted || rewritten == found->second,
                          type + " without " + key.second +
                              " now reads as:\n  " + rewritten);
                }
            }
        }
        for (const auto& [key, canonical] : recorded) {
            Check(visited.count(key) != 0,
                  "recorded omission never exercised: " + key.first + " " +
                      key.second);
        }
    });

    Test("a readable example: InsertClip in its field order", [] {
        InsertClipOperation insert;
        insert.track_id = kTrack;
        insert.source_id = kSource;
        insert.source_in = {100, 25};
        insert.duration = {10, 25};
        insert.timeline_in = {0, 25};
        insert.clip_id = kClip;
        Check(SerializeOperation(insert) ==
                  R"({"type":"InsertClip",)"
                  R"("track_id":"01K20000000000000000000003",)"
                  R"("source_id":"01K20000000000000000000001",)"
                  R"("source_in":{"value":100,"rate":25},)"
                  R"("duration":{"value":10,"rate":25},)"
                  R"("timeline_in":{"value":0,"rate":25},)"
                  R"("clip_id":"01K20000000000000000000004",)"
                  R"("exact_timeline":[],"sync_track_ids":[]})",
              "InsertClip wrote " + SerializeOperation(insert));
    });

    Test("text is escaped as JSON requires, and nothing more", [] {
        // BENCH-2026-09 -- quotes and backslashes were the only escapes, so
        // a two-line caption wrote a raw line feed into the journal.
        AddMarkerOperation marker;
        marker.marker.id = kClip;
        marker.marker.name = std::string("plan \"large\" \\ é\n\t\r\b\f") +
                             '\x01' + '\x1f' + '\0' + "/\x7f🙂";
        marker.marker.time = {3, 25};
        const std::string json = SerializeOperation(marker);
        const std::string escaped =
            R"("name":"plan \"large\" \\ é\n\t\r\b\f\u0001\u001f\u0000/)"
            "\x7f🙂\"";
        Check(json.find(escaped) != std::string::npos,
              "marker name was written as " + json);
        Check(std::none_of(json.begin(), json.end(),
                           [](unsigned char c) { return c < 0x20; }),
              "no control character is written raw: " + json);
        std::string rewritten;
        Check(Rewrite(json, rewritten) && rewritten == json,
              "escaped marker name round-trips");
        std::string message;
        const auto parsed = Parse<AddMarkerOperation>(json, message);
        Check(parsed && parsed->marker.name == marker.marker.name,
              "escaped marker name reads back as written: " + message);
    });

    Test("a journal holding a two-line caption is strict JSON", [] {
        // BENCH-2026-09 -- the bench script had to read journals leniently.
        // mcp_json refuses a raw control character in a string, as
        // Python's json.loads does.
        Document document;
        document.sources = {{kSource, "A.MP4", {25, 1}, {1000, 25}}};
        document.sequence.tracks = {
            {kTrack,
             "video",
             0,
             {{kClip, kSource, {0, 25}, {10, 25}, {0, 25}}}},
        };
        CaptionStyle style;
        style.id = kCaptionGroup;
        document.sequence.caption_styles = {style};
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document,
                        SetClipCaptionOperation{kClip, kCaptionGroup,
                                                "Première ligne\nseconde "
                                                "ligne"},
                        error, message),
              "two-line caption applies: " + message);
        const std::string journal = log.Serialize();
        Value tree;
        std::string parseError;
        Check(Value::Parse(journal, tree, parseError),
              "a strict JSON reader accepts the journal: " + parseError +
                  "\n  " + journal);
        const Value* applied = tree.Find("applied");
        const Value* op =
            applied && applied->IsArray() && !applied->AsArray().empty()
                ? applied->AsArray()[0].Find("op")
                : nullptr;
        const Value* text = op ? op->Find("caption_text") : nullptr;
        Check(text && text->IsString() &&
                  text->AsString() == "Première ligne\nseconde ligne",
              "the strict reader sees both lines of the caption");
        EditLog reloaded;
        Check(EditLog::Deserialize(journal, reloaded, error, message) &&
                  reloaded.Serialize() == journal,
              "the journal reads back to the same bytes: " + message);
    });

    Test("the reader tolerates exactly the recorded spellings", [] {
        // Each line holds, as a JSON string, text the writer does not
        // produce -- a raw control character from a journal written before
        // the fix, or an escape another JSON writer uses -- and the
        // operation it must read as, in the writer's own spelling.
        int checked = 0;
        for (const std::string& line : Lines("tolerances.jsonl")) {
            Value entry;
            std::string error;
            Check(Value::Parse(line, entry, error) && entry.IsObject(),
                  "malformed tolerances.jsonl line: " + line);
            const Value* note = entry.Find("note");
            const Value* reads = entry.Find("reads");
            const Value* writes = entry.Find("writes");
            if (!note || !reads || !reads->IsString() || !writes ||
                !writes->IsObject()) {
                Check(false, "tolerances.jsonl line lacks a field: " + line);
                continue;
            }
            std::string rewritten;
            const bool accepted = Rewrite(reads->AsString(), rewritten);
            Check(accepted, note->AsString() + ": no longer read");
            Check(!accepted || rewritten == writes->Dump(),
                  note->AsString() + ": now reads as\n  " + rewritten);
            ++checked;
        }
        Check(checked >= 6, "tolerances.jsonl looks truncated");
    });

    Test("an escape JSON does not define is refused", [] {
        const auto rejects = [](const std::string& text,
                                const std::string& label) {
            std::string rewritten;
            Check(!Rewrite(
                      R"({"type":"RemoveMarker","marker_id":")" + text + "\"}",
                      rewritten),
                  label + " was accepted");
        };
        rejects(R"(\x41)", "an unknown escape");
        rejects(R"(\u00e)", "a truncated Unicode escape");
        rejects(R"(\u00g9)", "a Unicode escape that is not hex");
        rejects(R"(\ud83d)", "a lone high surrogate");
        rejects(R"(\ude42)", "a lone low surrogate");
        rejects(R"(\ud83d\u0041)", "a high surrogate without its low half");
        std::string rewritten;
        Check(
            Rewrite(R"({"type":"RemoveMarker","marker_id":"\u00e9"})",
                    rewritten) &&
                rewritten == "{\"type\":\"RemoveMarker\",\"marker_id\":\"é\"}",
            "a valid Unicode escape still reads: " + rewritten);
    });

    Test("the positional format refuses what it never wrote", [] {
        const auto rejects = [](const std::string& json,
                                const std::string& label) {
            std::string rewritten;
            Check(!Rewrite(json, rewritten), label + " was accepted");
        };
        rejects(R"({"type":"TrimClip","clip_id":"a","edge":"Middle",)"
                R"("delta":{"value":4,"rate":25},"exact_clip":null})",
                "unknown trim edge");
        rejects(R"({"type":"TrimClip","edge":"Head","clip_id":"a",)"
                R"("delta":{"value":4,"rate":25},"exact_clip":null})",
                "fields out of order");
        rejects(R"({"type":"RemoveClip","clip_id":"a",)"
                R"("sync_track_ids":[],"exact_timeline":[],"extra":1})",
                "unknown trailing field");
        rejects(R"({"type":"TrimClip","clip_id":"a","edge":"Head",)"
                R"("delta":{"value":4,"rate":4294967296},"exact_clip":null})",
                "rate outside int32_t");
        rejects(R"({"type":"RemoveClip","clip_id":"a",)"
                R"("sync_track_ids":[],"exact_timeline":[]} {})",
                "trailing JSON");
        rejects(R"({"type":"Teleport","clip_id":"a"})", "unknown type");
    });

    Test("an integer that does not fit its member is refused", [] {
        const auto rejects = [](const std::string& json,
                                const std::string& label) {
            std::string rewritten;
            Check(!Rewrite(json, rewritten), label + " was accepted");
        };
        const std::string metadata =
            R"({"type":"SetProjectBinMetadata","item_id":"a","metadata":)"
            R"({"description":"","rating":RATING,"tags":[],)"
            R"("insert_order":ORDER,"display_name":""},)"
            R"("exact_project_hex":null})";
        const auto withMetadata = [&](const std::string& rating,
                                      const std::string& order) {
            std::string json = metadata;
            json.replace(json.find("RATING"), 6, rating);
            json.replace(json.find("ORDER"), 5, order);
            return json;
        };
        rejects(withMetadata("-1", "3"), "a negative rating");
        rejects(withMetadata("4294967296", "3"), "a rating over uint32_t");
        rejects(withMetadata("4", "99999999999999999999999"),
                "an insert order over uint64_t");
        rejects(R"({"type":"AddMarker","marker":{"id":"a","name":"",)"
                R"("time":{"value":0,"rate":25},"color":"","category":""},)"
                R"("insertion_index":99999999999999999999})",
                "an insertion index over int64_t");

        // The largest order the writer can emit reads back exactly; it used
        // to come back as INT64_MAX.
        const std::string largest = withMetadata("4", "18446744073709551615");
        std::string rewritten;
        Check(Rewrite(largest, rewritten) && rewritten == largest,
              "the largest insert order round-trips: " + rewritten);

        // Media dimensions used to be truncated by the cast.
        for (const std::string& golden : Lines("goldens.jsonl")) {
            if (TypeOf(golden) != "RelinkProjectMedia") continue;
            const size_t width = golden.find(R"("width":)");
            Check(width != std::string::npos, "relink golden has a width");
            const size_t end = golden.find(',', width);
            std::string json = golden;
            json.replace(width, end - width, R"("width":4294967296)");
            rejects(json, "a media width over int32_t");
            break;
        }
    });

    Test("a duplicated key is refused rather than resolved by position", [] {
        std::string rewritten;
        Check(!Rewrite(R"({"type":"SetClipEffects","clip_id":"a","effects":)"
                       R"([{"id":"e","type":"color.exposure","params":)"
                       R"({"k":{"num":1,"den":2},"k":{"num":3,"den":4}}}]})",
                       rewritten),
              "a duplicated effect parameter was accepted");
    });

    Test("every operation reads back as the value that was written", [] {
        // The byte fixtures pin the format; this pins its meaning. A codec
        // that dropped a member, or read one into the wrong place, would
        // still round-trip its own bytes.
        int checked = 0;
        const auto roundTrip = [&checked](auto value, uint64_t seed,
                                          generator::Mode mode) {
            using T = decltype(value);
            generator::Context context{generator::Rng(seed), mode};
            generator::Fill(context, value);
            const std::string json = Write(value);
            std::string message;
            auto parsed = Parse<T>(json, message);
            if (!parsed) {
                Check(false, json + " does not read back: " + message);
                return;
            }
            Check(operation_format::SameValue(value, *parsed),
                  "an operation changed on the way through the journal: " +
                      json + "\n  -> " + Write(*parsed));
            ++checked;
        };
        uint64_t seed = 7;
        const auto everyType = [&](const auto& alternatives) {
            for (const auto& sample : alternatives) {
                std::visit(
                    [&](const auto& value) {
                        for (int round = 0; round < 4; ++round) {
                            roundTrip(value, ++seed,
                                      round == 0 ? generator::Mode::Populated
                                                 : generator::Mode::Random);
                        }
                    },
                    sample);
            }
        };
        everyType(EveryAlternative(static_cast<const Operation*>(nullptr)));
        everyType(
            EveryAlternative(static_cast<const ProjectOperation*>(nullptr)));
        Check(checked >= 244,
              "too few operations exercised: " + std::to_string(checked));
    });

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
