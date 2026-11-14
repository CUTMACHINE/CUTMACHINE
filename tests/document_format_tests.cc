// FORMAT-2026-09 -- ROADMAP.md, F6. The canonical format of documents,
// project files and project deltas, pinned.
//
// A project file is read back by every later build, and a project journal
// replays the deltas it recorded. The round trips elsewhere prove that a
// reader accepts what its writer emits; a change that moved both together
// would pass them and still leave saved projects unreadable, or read them
// differently.
//
// The fixtures were produced by the hand-written codec of Document.cc and
// ProjectDelta.cc, before anything replaced it:
//
//   goldens.jsonl    documents, project files and deltas -- fully populated,
//                    random, default and hand-built -- as that codec wrote
//                    them. Each must read back and write out to the same
//                    bytes. DEC44 -- and the manifests of the project
//                    archive (ArchiveManifest.h), as their table writes
//                    them since it was made.
//   mutations.tsv    for the first golden of each kind, what that codec did
//                    with every key removed, every value given another JSON
//                    type, and every integer pushed out of int32_t or below
//                    zero: the text it then wrote, as the difference from
//                    the golden, or the error it gave. This pins the
//                    tolerance of the reader for files written before a
//                    field existed, and the messages a person gets for a
//                    damaged one.
//   legacy.jsonl     inputs no current writer produces -- older document and
//                    project versions, unknown keys, forbidden numbers --
//                    each spelled as a golden and the edits that make it,
//                    with what that codec made of it.
//
// A failure prints the line to paste when a change is intended.

#include "ArchiveManifest.h"
#include "Document.h"
#include "DocumentFormat.h"
#include "DocumentJson.h"
#include "Json.h"
#include "Project.h"
#include "ProjectDelta.h"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <type_traits>
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

const std::string kFixtures = DOCUMENT_FORMAT_FIXTURES_DIR;

std::vector<std::string> Lines(const std::string& name) {
    std::ifstream input(kFixtures + "/" + name);
    if (!input) throw std::runtime_error("missing fixture " + name);
    std::vector<std::string> lines;
    for (std::string line; std::getline(input, line);) {
        if (!line.empty()) lines.push_back(line);
    }
    return lines;
}

Value ParseJson(const std::string& text, const std::string& what) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error))
        throw std::runtime_error(what + " is not JSON: " + error);
    return value;
}

std::string StringField(const Value& object, const char* key) {
    const Value* found = object.Find(key);
    if (!found || !found->IsString())
        throw std::runtime_error(std::string("fixture has no string '") + key +
                                 "'");
    return found->AsString();
}

// Reads `text` as a `kind` and writes it back. Documents are read without
// whole-document validation, which is a separate contract: goldens hold
// random values no editor would produce. A project has no such reader, so
// its goldens are valid projects.
bool Rewrite(const std::string& kind, const std::string& text,
             std::string& output, std::string& error) {
    if (kind == "document") {
        Document document;
        if (!Document::LoadFromString(text, document, error, false))
            return false;
        output = document.SaveToString();
        return true;
    }
    if (kind == "project") {
        Project project;
        if (!Project::LoadFromString(text, project, error)) return false;
        output = project.SaveToString();
        return true;
    }
    if (kind == "delta") {
        ProjectDelta delta;
        if (!DeserializeProjectDelta(text, delta, error)) return false;
        output = SerializeProjectDelta(delta);
        return true;
    }
    // DEC44 -- docs/roadmap/DONNEES-2026-10.md.
    if (kind == "archive_manifest") {
        ArchiveManifest manifest;
        if (!ParseArchiveManifest(text, manifest, error)) return false;
        output = SerializeArchiveManifest(manifest);
        return true;
    }
    throw std::runtime_error("unknown fixture kind '" + kind + "'");
}

struct Golden {
    std::string kind;
    std::string text;
};

std::vector<Golden> Goldens() {
    std::vector<Golden> goldens;
    for (const std::string& line : Lines("goldens.jsonl")) {
        const Value value = ParseJson(line, "goldens.jsonl line");
        goldens.push_back(
            {StringField(value, "kind"), StringField(value, "text")});
    }
    return goldens;
}

// ---------------------------------------------------------------------
// Paths and mutations.
// ---------------------------------------------------------------------

struct Step {
    std::string key;  // empty: first element of an array
};

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

// Every key of `value`, and of the first element of every array. A part a
// project file or a delta carries is read by the document's own rules,
// which the document golden already pins in full: below `parts`, only its
// own keys are visited, to pin the context its errors name.
void Paths(const Value& value, const std::set<std::string>& parts, int budget,
           std::vector<Step>& prefix, std::vector<std::vector<Step>>& out) {
    if (budget == 0) return;
    if (value.IsObject()) {
        for (const auto& [key, child] : value.AsObject()) {
            prefix.push_back({key});
            out.push_back(prefix);
            const int next = budget > 0         ? budget - 1
                             : parts.count(key) ? 1
                                                : -1;
            Paths(child, parts, next, prefix, out);
            prefix.pop_back();
        }
    } else if (value.IsArray() && !value.AsArray().empty()) {
        prefix.push_back({""});
        Paths(value.AsArray()[0], parts, budget, prefix, out);
        prefix.pop_back();
    }
}

// `change` returns the replacement of the value at `path`, or nothing to
// remove its key.
template <typename Change>
Value Mutate(const Value& value, const std::vector<Step>& path, size_t depth,
             Change change) {
    if (value.IsObject()) {
        Value copy = Value::MakeObject();
        for (const auto& [key, child] : value.AsObject()) {
            if (key != path[depth].key) {
                copy.Set(key, child);
            } else if (depth + 1 < path.size()) {
                copy.Set(key, Mutate(child, path, depth + 1, change));
            } else if (auto replacement = change(child)) {
                copy.Set(key, *replacement);
            }
        }
        return copy;
    }
    if (value.IsArray()) {
        Value copy = Value::MakeArray();
        for (size_t index = 0; index < value.AsArray().size(); ++index) {
            copy.Push(index == 0
                          ? Mutate(value.AsArray()[0], path, depth + 1, change)
                          : value.AsArray()[index]);
        }
        return copy;
    }
    return value;
}

std::optional<Value> Retyped(const Value& value) {
    if (value.IsString()) return Value::MakeInt(0);
    if (value.IsObject()) return Value::MakeArray();
    if (value.IsArray()) return Value::MakeObject();
    return Value::MakeString("0");
}

// What a reader made of a text, as the difference from what it started
// from: `-path` for a key it dropped, `path=value` for one it changed or
// added.
void Diff(const Value& before, const Value& after, const std::string& path,
          std::vector<std::string>& out) {
    const auto child = [&path](const std::string& key) {
        return path.empty() ? key : path + "." + key;
    };
    if (before.IsObject() && after.IsObject()) {
        for (const auto& [key, value] : before.AsObject()) {
            if (const Value* found = after.Find(key))
                Diff(value, *found, child(key), out);
            else
                out.push_back("-" + child(key));
        }
        for (const auto& [key, value] : after.AsObject()) {
            if (!before.Find(key))
                out.push_back(child(key) + "=" + value.Dump());
        }
        return;
    }
    if (before.IsArray() && after.IsArray() &&
        before.AsArray().size() == after.AsArray().size()) {
        for (size_t index = 0; index < before.AsArray().size(); ++index) {
            Diff(before.AsArray()[index], after.AsArray()[index],
                 path + "[" + std::to_string(index) + "]", out);
        }
        return;
    }
    if (before.Dump() != after.Dump()) out.push_back(path + "=" + after.Dump());
}

std::string Outcome(const std::string& kind, const Value& golden,
                    const std::string& input) {
    std::string rewritten;
    std::string error;
    if (!Rewrite(kind, input, rewritten, error)) return "refused\t" + error;
    std::vector<std::string> changes;
    Diff(golden, ParseJson(rewritten, "rewritten " + kind), "", changes);
    std::string joined;
    for (const std::string& change : changes)
        joined += (joined.empty() ? "" : " ") + change;
    return "accepted\t" + joined;
}

// Every mutation of the first golden of each kind, as mutations.tsv lines.
std::vector<std::string> MutationLines() {
    static const std::map<std::string, std::set<std::string>> parts = {
        {"document", {}},
        {"project",
         {"color_management", "library", "bins", "sources", "timelines"}},
        {"delta", {"item", "from", "to", "content"}},
        {"archive_manifest", {}},
    };
    std::vector<std::string> lines;
    std::set<std::string> seen;
    for (const Golden& golden : Goldens()) {
        if (!seen.insert(golden.kind).second) continue;
        const Value tree = ParseJson(golden.text, golden.kind + " golden");
        std::vector<std::vector<Step>> paths;
        std::vector<Step> prefix;
        Paths(tree, parts.at(golden.kind), -1, prefix, paths);
        for (const auto& path : paths) {
            const std::string label = Label(path);
            const auto line = [&](const std::string& mutation,
                                  const Value& mutated) {
                lines.push_back(golden.kind + "\t" + mutation + "\t" + label +
                                "\t" +
                                Outcome(golden.kind, tree, mutated.Dump()));
            };
            line("remove", Mutate(tree, path, 0, [](const Value&) {
                     return std::optional<Value>();
                 }));
            line("retype", Mutate(tree, path, 0, Retyped));
            Value leaf = tree;
            bool integer = false;
            Mutate(tree, path, 0, [&](const Value& value) {
                int64_t number = 0;
                integer = value.AsInt64(number);
                return std::optional<Value>(value);
            });
            if (!integer) continue;
            line("int32+1", Mutate(tree, path, 0, [](const Value&) {
                     return std::optional<Value>(
                         Value::MakeInt(INT64_C(2147483648)));
                 }));
            line("negative", Mutate(tree, path, 0, [](const Value&) {
                     return std::optional<Value>(Value::MakeInt(-1));
                 }));
        }
    }
    return lines;
}

// ---------------------------------------------------------------------
// Legacy inputs: a golden and the edits that turn it into the input.
//
//   ["set", path, value]         give `path` this value, adding its key
//   ["remove", path]             drop the key at `path`
//   ["duplicate", path]          append a copy of the array element `path`
//   ["reverse", path]            reverse the key order of an object
//   ["compact"]                  the golden written without whitespace
//   ["as_version_2", edits]      a version-3 project as version 2 wrote it,
//                                `edits` applied to its second timeline's
//                                document only
//   ["replace", old, new]        text edits, after every tree edit
//   ["text", input]              an input no golden resembles
//
// A path is dot-separated; a number is an array index and `*` every
// element. An empty path is the root.
// ---------------------------------------------------------------------

std::vector<std::string> Segments(const std::string& path) {
    std::vector<std::string> segments;
    if (path.empty()) return segments;
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        segments.push_back(path.substr(start, dot - start));
        if (dot == std::string::npos) return segments;
        start = dot + 1;
    }
}

bool IsIndex(const std::string& segment) {
    return !segment.empty() &&
           segment.find_first_not_of("0123456789") == std::string::npos;
}

// Rebuilds `node` with `leaf` applied at `segments`: `leaf` receives the
// current value, or null when the key is absent, and returns the new one,
// or nothing to drop the key.
template <typename Leaf>
Value EditAt(const Value& node, const std::vector<std::string>& segments,
             size_t depth, Leaf leaf) {
    if (depth == segments.size()) {
        auto replaced = leaf(&node);
        if (!replaced) throw std::runtime_error("cannot remove the root");
        return *replaced;
    }
    const std::string& segment = segments[depth];
    const bool last = depth + 1 == segments.size();
    if (node.IsArray()) {
        Value copy = Value::MakeArray();
        for (size_t index = 0; index < node.AsArray().size(); ++index) {
            const Value& element = node.AsArray()[index];
            if (segment != "*" && segment != std::to_string(index)) {
                copy.Push(element);
            } else if (!last) {
                copy.Push(EditAt(element, segments, depth + 1, leaf));
            } else if (auto replaced = leaf(&element)) {
                copy.Push(*replaced);
            }
        }
        return copy;
    }
    if (!node.IsObject())
        throw std::runtime_error("edit path '" + segment +
                                 "' crosses a scalar");
    Value copy = Value::MakeObject();
    bool found = false;
    for (const auto& [key, child] : node.AsObject()) {
        if (key != segment) {
            copy.Set(key, child);
        } else {
            found = true;
            if (!last) {
                copy.Set(key, EditAt(child, segments, depth + 1, leaf));
            } else if (auto replaced = leaf(&child)) {
                copy.Set(key, *replaced);
            }
        }
    }
    if (!found && last) {
        if (auto added = leaf(nullptr)) copy.Set(segment, *added);
    } else if (!found) {
        throw std::runtime_error("edit path has no key '" + segment + "'");
    }
    return copy;
}

Value ApplyTreeEdits(Value tree, const std::vector<Value>& edits);

// S7 -- SAVING_ROADMAP.md. Version 2 embedded one whole document per
// timeline, as a string, each with its own copy of the shared parts.
Value AsVersionTwo(const Value& project, const std::vector<Value>& edits) {
    const Value* timelines = project.Find("timelines");
    if (!timelines || !timelines->IsArray())
        throw std::runtime_error("as_version_2 needs a version-3 project");
    Value snapshots = Value::MakeArray();
    for (size_t index = 0; index < timelines->AsArray().size(); ++index) {
        Value document = Value::MakeObject();
        document.Set("version", Value::MakeInt(7));
        document.Set("sequence", timelines->AsArray()[index]);
        for (const char* key :
             {"color_management", "library", "bins", "sources"})
            document.Set(key, *project.Find(key));
        if (index == 1) document = ApplyTreeEdits(document, edits);
        snapshots.Push(Value::MakeString(document.Dump()));
    }
    Value versionTwo = Value::MakeObject();
    versionTwo.Set("project_format", *project.Find("project_format"));
    versionTwo.Set("project_version", Value::MakeInt(2));
    for (const char* key : {"id", "name", "active_timeline_id"})
        versionTwo.Set(key, *project.Find(key));
    versionTwo.Set("timeline_snapshots", snapshots);
    for (const char* key : {"bin_metadata", "timeline_bin_ids"})
        versionTwo.Set(key, *project.Find(key));
    return versionTwo;
}

Value ApplyTreeEdits(Value tree, const std::vector<Value>& edits) {
    for (const Value& edit : edits) {
        const auto& args = edit.AsArray();
        const std::string op = args.at(0).AsString();
        if (op == "set") {
            const Value value = args.at(2);
            tree = EditAt(
                tree, Segments(args.at(1).AsString()), 0,
                [&value](const Value*) { return std::optional<Value>(value); });
        } else if (op == "remove") {
            tree = EditAt(tree, Segments(args.at(1).AsString()), 0,
                          [](const Value* existing) {
                              if (!existing)
                                  throw std::runtime_error("nothing to remove");
                              return std::optional<Value>();
                          });
        } else if (op == "duplicate") {
            auto segments = Segments(args.at(1).AsString());
            const size_t index = std::stoul(segments.back());
            segments.pop_back();
            tree = EditAt(tree, segments, 0, [index](const Value* array) {
                Value copy = *array;
                copy.Push(array->AsArray().at(index));
                return std::optional<Value>(copy);
            });
        } else if (op == "reverse") {
            tree = EditAt(tree, Segments(args.at(1).AsString()), 0,
                          [](const Value* object) {
                              Value copy = Value::MakeObject();
                              const auto& entries = object->AsObject();
                              for (auto entry = entries.rbegin();
                                   entry != entries.rend(); ++entry)
                                  copy.Set(entry->first, entry->second);
                              return std::optional<Value>(copy);
                          });
        } else if (op == "compact") {
            continue;
        } else if (op == "as_version_2") {
            tree = AsVersionTwo(tree, args.size() > 1 ? args.at(1).AsArray()
                                                      : std::vector<Value>());
        } else {
            throw std::runtime_error("unknown tree edit '" + op + "'");
        }
    }
    return tree;
}

struct LegacyCase {
    std::string kind;
    std::string note;
    std::string input;
    Value golden;
    Value line;
};

LegacyCase ReadLegacyCase(const std::string& text,
                          const std::vector<Golden>& goldens) {
    LegacyCase legacy;
    legacy.line = ParseJson(text, "legacy.jsonl line");
    legacy.note = StringField(legacy.line, "note");
    int64_t index = 0;
    const Value* golden = legacy.line.Find("golden");
    if (!golden || !golden->AsInt64(index) || index < 0 ||
        static_cast<size_t>(index) >= goldens.size())
        throw std::runtime_error(legacy.note + ": no such golden");
    legacy.kind = goldens[index].kind;
    legacy.golden = ParseJson(goldens[index].text, "golden");
    std::vector<Value> treeEdits;
    std::vector<Value> textEdits;
    for (const Value& edit : legacy.line.Find("edits")->AsArray()) {
        const std::string op = edit.AsArray().at(0).AsString();
        (op == "replace" || op == "text" ? textEdits : treeEdits)
            .push_back(edit);
    }
    legacy.input = treeEdits.empty()
                       ? goldens[index].text
                       : ApplyTreeEdits(legacy.golden, treeEdits).Dump();
    for (const Value& edit : textEdits) {
        const auto& args = edit.AsArray();
        if (args.at(0).AsString() == "text") {
            legacy.input = args.at(1).AsString();
            continue;
        }
        const std::string& from = args.at(1).AsString();
        const size_t at = legacy.input.find(from);
        if (at == std::string::npos)
            throw std::runtime_error(legacy.note + ": no '" + from + "'");
        legacy.input.replace(at, from.size(), args.at(2).AsString());
    }
    return legacy;
}

// A legacy.jsonl line with its outcome recomputed.
std::string LegacyLine(const LegacyCase& legacy) {
    const std::string outcome =
        Outcome(legacy.kind, legacy.golden, legacy.input);
    const size_t tab = outcome.find('\t');
    Value line = Value::MakeObject();
    for (const char* key : {"note", "golden", "edits"})
        line.Set(key, *legacy.line.Find(key));
    line.Set(outcome.substr(0, tab) == "accepted" ? "reads_as" : "refused",
             Value::MakeString(outcome.substr(tab + 1)));
    return line.Dump();
}

// ---------------------------------------------------------------------
// A value of every shape a table describes, so that a round trip through
// the file can be compared with what went in. Filling from the tables
// rather than by hand is what makes a newly added member covered the day
// it is added.
// ---------------------------------------------------------------------

namespace generator {

using namespace document_format;

struct Rng {
    std::mt19937_64 engine;
    explicit Rng(uint64_t seed) : engine(seed) {}
    int64_t Pick(int64_t low, int64_t high) {
        return std::uniform_int_distribution<int64_t>(low, high)(engine);
    }
    bool Coin() { return Pick(0, 1) == 1; }
};

// Populated leaves every flag set and every list non-empty, so that a
// member the codec forgets shows up as a difference.
enum class Mode { Populated, Random };

struct Context {
    Rng rng;
    Mode mode;
    int counter = 0;
};

void Fill(Context& context, std::string& value) {
    static const char* pieces[] = {"A",  "Z",  "0", "9",    " ",  "é",
                                   "\"", "\\", "/", "\n",   "\t", "\x01",
                                   "_",  "-",  "x", "\x7f", "🙂", "#"};
    const int64_t length = context.mode == Mode::Populated
                               ? context.rng.Pick(1, 5)
                               : context.rng.Pick(0, 5);
    value.clear();
    for (int64_t index = 0; index < length; ++index)
        value += pieces[context.rng.Pick(0, 17)];
    if (context.mode == Mode::Populated)
        value += std::to_string(++context.counter);
}

void Fill(Context& context, bool& value) {
    value = context.mode == Mode::Populated ? true : context.rng.Coin();
}

void Fill(Context& context, int32_t& value) {
    value = static_cast<int32_t>(context.rng.Pick(INT32_MIN, INT32_MAX));
    if (context.mode == Mode::Populated && value == 0) value = 7;
}

void Fill(Context& context, int64_t& value) {
    value = context.rng.Pick(INT64_MIN, INT64_MAX);
    if (context.mode == Mode::Populated && value == 0) value = 11;
}

void Fill(Context& context, uint32_t& value) {
    value = static_cast<uint32_t>(context.rng.Pick(0, UINT32_MAX));
}

// The parser holds integers as int64_t: the file cannot carry more.
void Fill(Context& context, uint64_t& value) {
    value = static_cast<uint64_t>(context.rng.Pick(0, INT64_MAX));
}

void Fill(Context& context, RationalTime& value) {
    value.value = context.rng.Pick(INT64_MIN, INT64_MAX);
    value.rate = static_cast<int32_t>(context.rng.Pick(INT32_MIN, INT32_MAX));
}

// MOTION-2026-09 M3 -- a composition's definition is a motion tree the codec
// carries as it is: decimals, exponents, key order and nesting come back as
// they were written.
void Fill(Context& context, Value& value) {
    std::string text;
    Fill(context, text);
    Value tree = Value::MakeObject();
    tree.Set("zeta", Value::MakeString(text));
    tree.Set("alpha",
             Value::MakeRawNumber(std::to_string(context.rng.Pick(-999, 999)) +
                                  ".125"));
    Value list = Value::MakeArray();
    list.Push(Value::MakeRawNumber("6.02e23"));
    list.Push(Value::MakeBool(context.rng.Coin()));
    list.Push(Value::MakeNull());
    list.Push(Value::MakeInt(context.rng.Pick(INT64_MIN, INT64_MAX)));
    Value nested = Value::MakeObject();
    nested.Set("k", Value::MakeRawNumber("-0.5"));
    list.Push(std::move(nested));
    tree.Set("list", std::move(list));
    value = context.mode == Mode::Random && context.rng.Coin()
                ? Value::MakeString(text)
                : std::move(tree);
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void Fill(Context& context, Enum& value);
template <typename T>
void Fill(Context& context, std::vector<T>& values);
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

// The couplings the tables express: a member the file only spells when
// another one says so cannot be compared unless the value respects them.
// Written out here rather than derived from the tables, so that a wrong
// condition in a table shows up as a lost member instead of agreeing with
// itself.
void Settle(DocumentClip& clip) {
    if (clip.sync_anchor_clip_id.empty()) clip.sync_reference_delta = {0, 1};
}

void Settle(LibraryMedia& media) {
    if (!media.metadata_complete) {
        LibraryMedia unprobed;
        unprobed.id = media.id;
        unprobed.path = media.path;
        unprobed.filename = media.filename;
        unprobed.rate = media.rate;
        unprobed.duration = media.duration;
        unprobed.bin_id = media.bin_id;
        unprobed.proxy_path = media.proxy_path;
        unprobed.metadata_complete = false;
        media = unprobed;
    }
    if (!media.has_audio) {
        media.audio_rate = 0;
        media.audio_channels = 0;
        media.audio_level_measured = false;
    }
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

}  // namespace generator

// Structural equality, member by member through the tables. Deliberately
// not RationalTime::operator==, which compares rationals: {0, 25} is not
// {0, 1} on disk.
bool SameValue(const std::string& left, const std::string& right) {
    return left == right;
}
bool SameValue(bool left, bool right) { return left == right; }
bool SameValue(int32_t left, int32_t right) { return left == right; }
bool SameValue(int64_t left, int64_t right) { return left == right; }
bool SameValue(uint32_t left, uint32_t right) { return left == right; }
bool SameValue(uint64_t left, uint64_t right) { return left == right; }
bool SameValue(const RationalTime& left, const RationalTime& right) {
    return left.value == right.value && left.rate == right.rate;
}
bool SameValue(const Value& left, const Value& right) {
    return left.Dump() == right.Dump();
}
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
bool SameValue(Enum left, Enum right) {
    return left == right;
}
template <typename T>
bool SameValue(const std::vector<T>& left, const std::vector<T>& right);
template <typename V>
bool SameValue(const std::map<std::string, V>& left,
               const std::map<std::string, V>& right);
template <typename T,
          std::enable_if_t<document_format::IsTabled<T>::value, int> = 0>
bool SameValue(const T& left, const T& right);

template <typename T>
bool SameValue(const std::vector<T>& left, const std::vector<T>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index)
        if (!SameValue(left[index], right[index])) return false;
    return true;
}

template <typename V>
bool SameValue(const std::map<std::string, V>& left,
               const std::map<std::string, V>& right) {
    if (left.size() != right.size()) return false;
    auto other = right.begin();
    for (auto entry = left.begin(); entry != left.end(); ++entry, ++other)
        if (entry->first != other->first ||
            !SameValue(entry->second, other->second))
            return false;
    return true;
}

template <typename T,
          std::enable_if_t<document_format::IsTabled<T>::value, int>>
bool SameValue(const T& left, const T& right) {
    bool same = true;
    document_format::ForEachField<T>([&](const auto& field) {
        same = same && SameValue(left.*field.member, right.*field.member);
    });
    return same;
}

bool SameDocument(const Document& left, const Document& right) {
    return left.version == right.version &&
           SameValue(left.sequence, right.sequence) &&
           SameValue(left.color_management, right.color_management) &&
           SameValue(left.library, right.library) &&
           SameValue(left.bins, right.bins) &&
           SameValue(left.sources, right.sources) &&
           SameValue(left.compositions, right.compositions);
}

}  // namespace

int main(int argc, char** argv) {
    // What the codec now does with every mutation, for a reviewed
    // replacement of mutations.tsv after an intended change.
    if (argc == 2 && std::string(argv[1]) == "--print-mutations") {
        for (const std::string& line : MutationLines())
            std::cout << line << '\n';
        return 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--print-legacy") {
        const auto goldens = Goldens();
        for (const std::string& line : Lines("legacy.jsonl"))
            std::cout << LegacyLine(ReadLegacyCase(line, goldens)) << '\n';
        return 0;
    }

    Test("every golden reads back and writes out to the same bytes", [] {
        const auto goldens = Goldens();
        std::map<std::string, int> kinds;
        for (const Golden& golden : goldens) {
            ++kinds[golden.kind];
            std::string rewritten;
            std::string error;
            if (!Rewrite(golden.kind, golden.text, rewritten, error)) {
                Check(false, "a " + golden.kind + " golden no longer reads: " +
                                 error + "\n" + golden.text);
            } else {
                Check(rewritten == golden.text,
                      "a " + golden.kind + " golden rewrote differently:\n" +
                          golden.text + "\n---\n" + rewritten);
            }
        }
        Check(kinds["document"] >= 10 && kinds["project"] >= 3 &&
                  kinds["delta"] >= 8 && kinds["archive_manifest"] >= 3,
              "goldens.jsonl looks truncated");
    });

    Test("every mutation of a golden reads as recorded", [] {
        std::map<std::string, std::string> recorded;
        for (const std::string& line : Lines("mutations.tsv")) {
            size_t tab = line.find('\t');
            for (int column = 1; column < 3 && tab != std::string::npos;
                 ++column)
                tab = line.find('\t', tab + 1);
            Check(tab != std::string::npos,
                  "malformed mutations.tsv line: " + line);
            if (tab == std::string::npos) continue;
            recorded[line.substr(0, tab)] = line;
        }
        std::set<std::string> current;
        for (const std::string& line : MutationLines()) {
            size_t tab = line.find('\t');
            for (int column = 1; column < 3; ++column)
                tab = line.find('\t', tab + 1);
            const std::string key = line.substr(0, tab);
            current.insert(key);
            const auto found = recorded.find(key);
            if (found == recorded.end()) {
                Check(false,
                      "a mutation nothing records; if intended, add "
                      "to mutations.tsv:\n" +
                          line);
            } else {
                Check(found->second == line,
                      "a mutation now reads differently; if intended, "
                      "replace in mutations.tsv:\n  was " +
                          found->second + "\n  now " + line);
            }
        }
        for (const auto& [key, line] : recorded)
            Check(current.count(key) != 0,
                  "a recorded mutation no longer applies: " + line);
    });

    Test("every legacy input reads as recorded", [] {
        const auto goldens = Goldens();
        const auto lines = Lines("legacy.jsonl");
        Check(lines.size() >= 60, "legacy.jsonl looks truncated");
        for (const std::string& line : lines) {
            const LegacyCase legacy = ReadLegacyCase(line, goldens);
            const std::string now = LegacyLine(legacy);
            Check(now == line, legacy.note +
                                   " now reads differently; if intended, "
                                   "replace in legacy.jsonl:\n  was " +
                                   line + "\n  now " + now);
        }
    });

    Test("every document reads back as the value that was written", [] {
        // The byte fixtures pin the format; this pins its meaning. A table
        // whose condition drops a member in some state, or reads one into
        // the wrong place, would still round-trip its own bytes.
        int checked = 0;
        for (uint64_t seed = 1; seed <= 200; ++seed) {
            generator::Context context{generator::Rng(seed),
                                       seed <= 20 ? generator::Mode::Populated
                                                  : generator::Mode::Random};
            Document document;
            generator::Fill(context, document.sequence);
            generator::Fill(context, document.color_management);
            generator::Fill(context, document.library);
            generator::Fill(context, document.bins);
            generator::Fill(context, document.sources);
            generator::Fill(context, document.compositions);
            const std::string json = document.SaveToString();
            Document parsed;
            std::string error;
            if (!Document::LoadFromString(json, parsed, error, false)) {
                Check(false,
                      "a document does not read back: " + error + "\n" + json);
                continue;
            }
            Check(SameDocument(document, parsed),
                  "a document changed on the way through the file:\n" + json +
                      "\n---\n" + parsed.SaveToString());
            ++checked;
        }
        Check(checked == 200, "too few documents read back");
    });

    Test("bin metadata reads back from a project file and a delta", [] {
        Project project("P");
        project.id = "01K60000000000000000000001";
        project.timelines.front().id = "01K60000000000000000000002";
        project.active_timeline_id = project.timelines.front().id;
        for (uint64_t seed = 1; seed <= 100; ++seed) {
            generator::Context context{generator::Rng(seed),
                                       seed <= 10 ? generator::Mode::Populated
                                                  : generator::Mode::Random};
            ProjectBinMetadata metadata;
            generator::Fill(context, metadata);
            // What a project may hold (Project::Validate).
            metadata.rating %= 6;
            metadata.display_name = metadata.display_name.substr(0, 128);
            project.bin_metadata[project.active_timeline_id] = metadata;

            Project parsed;
            std::string error;
            Check(Project::LoadFromString(project.SaveToString(), parsed,
                                          error) &&
                      parsed.bin_metadata.size() == 1 &&
                      SameValue(parsed.bin_metadata.begin()->second, metadata),
                  "bin metadata changed in a project file: " + error + "\n" +
                      project.SaveToString());

            ProjectBinMetadata record;
            Check(document_json::ParseBinMetadata(
                      document_json::BinMetadataJson(metadata), "record",
                      record, error) &&
                      SameValue(record, metadata),
                  "bin metadata changed in a delta: " + error);

            MediaRate rate;
            generator::Fill(context, rate);
            MediaRate parsedRate;
            Check(document_json::ParseMediaRate(
                      document_json::MediaRateJson(rate), "rate", parsedRate,
                      error) &&
                      SameValue(parsedRate, rate),
                  "a frame rate changed in a delta: " + error);
        }
    });

    // ITW4 -- the decoration of a caption style is written only when set,
    // so undecorated styles keep their bytes; a decorated one is spelled so.
    Test("a readable example: a decorated caption style", [] {
        Document document;
        document.sequence.id = "01K40000000000000000000001";
        document.sequence.name = "ITW";
        CaptionStyle plain;
        plain.id = "01K40000000000000000000005";
        CaptionStyle legible = plain;
        legible.id = "01K40000000000000000000006";
        legible.outline_width = 4;
        legible.background_color = "#000000@0.5";
        legible.vertical_position = {3, 4};
        document.sequence.caption_styles = {plain, legible};
        const std::string text = document.SaveToString();
        Check(text.find(
                  "\"caption_styles\":[\n"
                  "    {\"id\":\"01K40000000000000000000005\","
                  "\"font_family\":\"system\",\"font_size\":48,"
                  "\"color\":\"#ffffff\",\"position\":\"bottom\"},\n"
                  "    {\"id\":\"01K40000000000000000000006\","
                  "\"font_family\":\"system\",\"font_size\":48,"
                  "\"color\":\"#ffffff\",\"position\":\"bottom\","
                  "\"outline_width\":4,\"background_color\":\"#000000@0.5\","
                  "\"vertical_position\":{\"num\":3,\"den\":4}}\n"
                  "  ]") != std::string::npos,
              "caption styles are spelled with their decoration only when "
              "set:\n" +
                  text);
        Document reread;
        std::string error;
        Check(
            Document::LoadFromString(text, reread, error) &&
                reread.sequence.caption_styles.size() == 2 &&
                reread.sequence.caption_styles[0].outline_width == 0 &&
                reread.sequence.caption_styles[1].outline_width == 4 &&
                reread.sequence.caption_styles[1].outline_color == "#000000" &&
                reread.sequence.caption_styles[1].vertical_position.num == 3,
            "and read back with the defaults they omit: " + error);
    });

    Test("a readable example: one clip in a document", [] {
        Document document;
        document.sequence.id = "01K40000000000000000000001";
        document.sequence.name = "ITW";
        DocumentTrack track;
        track.id = "01K40000000000000000000002";
        track.kind = "video";
        DocumentClip clip;
        clip.id = "01K40000000000000000000003";
        clip.source_id = "01K40000000000000000000004";
        clip.source_in = {50, 25};
        clip.duration = {100, 25};
        clip.timeline_in = {0, 25};
        clip.include_audio = false;
        track.clips.push_back(clip);
        document.sequence.tracks.push_back(track);
        DocumentSource source;
        source.id = clip.source_id;
        source.path = "rushes/C0001.MP4";
        source.rate = {25, 1};
        source.duration = {750, 25};
        document.sources.push_back(source);
        const std::string expected =
            "{\n"
            "  \"version\": 7,\n"
            "  \"sequence\":{\"id\":\"01K40000000000000000000001\","
            "\"name\":\"ITW\",\"width\":1920,\"height\":1080,"
            "\"frame_rate\":{\"num\":25,\"den\":1},\"markers\":[  ],"
            "\"transitions\":[  ],\"tracks\":[\n"
            "    {\"id\":\"01K40000000000000000000002\",\"kind\":\"video\","
            "\"index\":0,\"locked\":false,\"sync_lock\":true,"
            "\"visible\":true,\"muted\":false,\"solo\":false,\"clips\":[\n"
            "      {\"id\":\"01K40000000000000000000003\","
            "\"source_id\":\"01K40000000000000000000004\","
            "\"source_in\":{\"value\":50,\"rate\":25},"
            "\"duration\":{\"value\":100,\"rate\":25},"
            "\"timeline_in\":{\"value\":0,\"rate\":25},"
            "\"include_audio\":false,\"effects\":[],"
            "\"opacity\":{\"num\":1,\"den\":1},"
            "\"audio_gain_db\":{\"num\":0,\"den\":1},"
            "\"audio_fade_in\":{\"value\":0,\"rate\":1},"
            "\"audio_fade_out\":{\"value\":0,\"rate\":1}}\n"
            "    ]}\n"
            "  ],\"caption_styles\":[  ],\"multicam_groups\":[  ]},\n"
            "  \"color_management\":{\"enabled\":false,"
            "\"input_gamut\":\"rec709\",\"input_transfer\":\"rec709\","
            "\"input_ycbcr_matrix\":\"auto\",\"input_range\":\"auto\","
            "\"working_gamut\":\"acescct\",\"output_gamut\":\"rec709\","
            "\"output_transfer\":\"rec709\"},\n"
            "  \"library\": [  ],\n"
            "  \"bins\": [  ],\n"
            "  \"sources\": [\n"
            "    {\"id\":\"01K40000000000000000000004\","
            "\"path\":\"rushes/C0001.MP4\",\"rate\":{\"num\":25,\"den\":1},"
            "\"duration\":{\"value\":750,\"rate\":25}}\n"
            "  ]\n"
            "}\n";
        Check(document.SaveToString() == expected,
              "the document was written as\n" + document.SaveToString());

        // R3 -- docs/roadmap/REEL-2026-09.md. The same clip slowed to 1/2:
        // one key more, at the end of the clip, and the version it needs.
        // Everything else is byte for byte what it was.
        document.sequence.tracks[0].clips[0].speed = {1, 2};
        std::string slowed = expected;
        const std::string tail = "\"audio_fade_out\":{\"value\":0,\"rate\":1}}";
        slowed.replace(slowed.find(tail), tail.size(),
                       "\"audio_fade_out\":{\"value\":0,\"rate\":1},"
                       "\"speed\":{\"num\":1,\"den\":2}}");
        slowed.replace(slowed.find("\"version\": 7"),
                       std::string("\"version\": 7").size(), "\"version\": 10");
        Check(document.SaveToString() == slowed,
              "the slowed document was written as\n" + document.SaveToString());
        Document reread;
        std::string error;
        Check(Document::LoadFromString(document.SaveToString(), reread, error,
                                       false) &&
                  reread.sequence.tracks.at(0).clips.at(0).speed.num == 1 &&
                  reread.sequence.tracks.at(0).clips.at(0).speed.den == 2,
              "and read back with its speed: " + error);
        // The version follows what the file spells, not what the fraction
        // means: a clip written 2/2 carries a key an older reader would
        // drop, so it needs version 10 too. set_clip_speed reduces what it
        // is given, so that spelling only ever arrives by hand.
        document.sequence.tracks[0].clips[0].speed = {2, 2};
        Check(document.SaveToString().find("\"version\": 10") !=
                  std::string::npos,
              "a speed spelled 2/2 still declares version 10: " +
                  document.SaveToString());
        document.sequence.tracks[0].clips[0].speed = {1, 1};
        Check(document.SaveToString() == expected,
              "and 1/1 is written by leaving the key out: " +
                  document.SaveToString());
    });

    if (failures) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
