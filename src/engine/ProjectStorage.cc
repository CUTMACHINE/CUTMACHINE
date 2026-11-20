#include "ProjectStorage.h"

#include "CanonicalHash.h"
#include "EditLog.h"
#include "FormatVersion.h"
#include "Json.h"
#include "LookLut.h"
#include "LookLutStorage.h"
#include "MotionDocument.h"
#include "PackageFiles.h"
#include "Project.h"
#include "ProjectArchive.h"
#include "Ulid.h"
#include "WorkCounters.h"

#include <CommonCrypto/CommonDigest.h>

#include <fcntl.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <system_error>

namespace {

namespace fs = std::filesystem;

// PRD20 -- how a missing project's refusal starts, for
// IsMissingProjectMessage.
constexpr char kMissingProjectPrefix[] = "no project at '";
// The package format this build reads and writes ("version" in
// manifest.json), for the refusal of a newer one (FormatVersion.h).
constexpr int64_t kNewestPackageVersion = 2;

// S2 -- SAVING_ROADMAP.md. Renames alone do not make a generation durable;
// file contents and the directory carrying each rename must both land.
bool SynchronizePath(const fs::path& path, bool directory, std::string& error) {
    // Read-only originals remain valid collected media; fsync does not require
    // a writable descriptor on the supported POSIX/macOS filesystems.
    const int descriptor = open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        error = "unable to open '" + path.string() +
                "' for synchronization: " + std::strerror(errno);
        return false;
    }
#ifdef F_FULLFSYNC
    const bool synchronized =
        directory
            ? fsync(descriptor) == 0
            : (fcntl(descriptor, F_FULLFSYNC) == 0 || fsync(descriptor) == 0);
#else
    const bool synchronized = fsync(descriptor) == 0;
#endif
    if (!synchronized) {
        const int savedError = errno;
        close(descriptor);
        error = "unable to synchronize '" + path.string() +
                "': " + std::strerror(savedError);
        return false;
    }
    if (close(descriptor) != 0) {
        error =
            "unable to close '" + path.string() + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

class StagingDirectory {
public:
    explicit StagingDirectory(fs::path path) : path_(std::move(path)) {}
    ~StagingDirectory() {
        if (!committed_) {
            std::error_code ignored;
            fs::remove_all(path_, ignored);
        }
    }
    const fs::path& path() const { return path_; }
    void Commit() { committed_ = true; }

private:
    fs::path path_;
    bool committed_ = false;
};

// Not mcp_json::EscapeJsonString: this one spells a backspace and a form
// feed as \u0008 and \u000c where the shared one writes \b and \f. Both
// are JSON, but the manifest's bytes are part of what a package holds, so
// the spelling it has always written stays.
std::string EscapeJson(const std::string& value) {
    std::ostringstream output;
    for (unsigned char character : value) {
        switch (character) {
            case '\\':
                output << "\\\\";
                break;
            case '"':
                output << "\\\"";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20)
                    output << "\\u" << std::hex << std::setw(4)
                           << std::setfill('0') << static_cast<int>(character)
                           << std::dec;
                else
                    output << static_cast<char>(character);
        }
    }
    return output.str();
}

bool Sha256(const fs::path& path, std::string& digest, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to hash media '" + path.string() + "'";
        return false;
    }
    CC_SHA256_CTX context;
    CC_SHA256_Init(&context);
    std::array<char, 1024 * 1024> buffer{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const std::streamsize count = input.gcount();
        if (count > 0)
            CC_SHA256_Update(&context, buffer.data(),
                             static_cast<CC_LONG>(count));
    }
    if (!input.eof()) {
        error = "unable to read media while hashing '" + path.string() + "'";
        return false;
    }
    std::array<unsigned char, CC_SHA256_DIGEST_LENGTH> bytes{};
    CC_SHA256_Final(bytes.data(), &context);
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned char byte : bytes) output << std::setw(2) << int(byte);
    digest = output.str();
    return true;
}

bool HasParentTraversal(const fs::path& path) {
    for (const fs::path& component : path)
        if (component == "..") return true;
    return false;
}

// FI12 -- docs/roadmap/FIABILITE-2026-10.md. Keep relative bindings editable:
// collect every stored choice (defaults, variants, formats, clips, instances
// and repeats), rather than freezing the picture at one frame. Literals and
// inline map outputs move in place. A prop moves too when nothing but a
// node's src reads it, as the shipped `logo` template's does: its stored
// values -- default, variants, formats and clips -- are rewritten in the
// copy. A shared token, a prop read elsewhere (a text, an instance) or a
// value an instance passes on cannot move without changing another use,
// and stays refused. A default that no clip and no instance relies on is
// collected when it names a file and skipped when it does not: the film
// never shows it (the template's `logo.png`, which every clip replaces).
class MotionAssetCollector {
public:
    MotionAssetCollector(fs::path source, fs::path destination)
        : source_(std::move(source)), destination_(std::move(destination)) {}

    bool Collect(Project& project, std::string& error) {
        for (const auto& stored : project.compositions) {
            Entry& entry = compositions_[stored.id];
            entry.id = stored.id;
            MotionError parseError;
            if (!ParseDocumentComposition(stored, entry.composition,
                                          parseError)) {
                error = "invalid motion composition: " + parseError.message;
                return false;
            }
            std::map<std::string, PropUses> uses;
            CountNodeUses(*stored.definition.Find("root"), uses);
            for (const auto& prop : entry.composition.props) {
                const auto found = uses.find(prop.name);
                if (prop.type == MotionPropType::Text && found != uses.end() &&
                    found->second.src > 0 && found->second.other == 0) {
                    entry.movable.insert(prop.name);
                }
            }
        }
        // A clip or an instance that does not set a prop shows its default.
        for (const auto& timeline : project.timelines) {
            for (const auto& track : timeline.tracks) {
                for (const auto& clip : track.clips) {
                    const auto found = compositions_.find(clip.composition_id);
                    if (found == compositions_.end()) {
                        continue;
                    }
                    for (const auto& prop : found->second.composition.props) {
                        if (clip.motion_props.Find(prop.name) == nullptr) {
                            found->second.shown_defaults.insert(prop.name);
                        }
                    }
                }
            }
        }
        for (const auto& entry : compositions_) {
            MarkInstanceDefaults(entry.second.composition.root);
        }
        for (auto& item : compositions_) {
            Entry& entry = item.second;
            for (const auto& prop : entry.composition.props) {
                entry.props[prop.name].Add(
                    prop.default_value,
                    entry.shown_defaults.count(prop.name) == 0);
                for (const auto& value : prop.values) {
                    entry.props[prop.name].Add(Value::MakeString(value));
                }
            }
            for (const auto& variant : entry.composition.variants) {
                AddProps(entry, variant.props);
            }
            for (const auto& format : entry.composition.formats) {
                AddProps(entry, format.props);
            }
        }
        for (const auto& timeline : project.timelines) {
            for (const auto& track : timeline.tracks) {
                for (const auto& clip : track.clips) {
                    const auto found = compositions_.find(clip.composition_id);
                    if (found != compositions_.end()) {
                        AddProps(found->second, clip.motion_props);
                    }
                }
            }
        }
        // Instance props may pass through several compositions. The validated
        // graph is acyclic, so one pass per composition reaches every leaf.
        for (size_t pass = 0; pass < compositions_.size(); ++pass) {
            for (auto& entry : compositions_) {
                Propagate(entry.second, entry.second.composition.root, {});
            }
        }
        for (auto& stored : project.compositions) {
            const Entry& entry = compositions_.at(stored.id);
            if (!CollectNode(entry, entry.composition.root, {}, error)) {
                error.insert(0, "motion composition '" + stored.name + "': ");
                return false;
            }
            Value definition = stored.definition;
            definition.Set("root", Rewrite(*definition.Find("root")));
            const auto moves = moves_.find(stored.id);
            if (moves != moves_.end()) {
                MoveStoredValues(moves->second, definition);
            }
            stored.definition = std::move(definition);
        }
        for (auto& timeline : project.timelines) {
            for (auto& track : timeline.tracks) {
                for (auto& clip : track.clips) {
                    const auto moves = moves_.find(clip.composition_id);
                    if (moves != moves_.end()) {
                        clip.motion_props =
                            MovedProps(moves->second, clip.motion_props);
                    }
                }
            }
        }
        return true;
    }

    // FI12, undo in a collected copy -- docs/roadmap/FIABILITE-2026-10.md.
    // The journals were copied as they were: undoing then redoing a motion
    // edit in the copy brought back the source's path, and so did a rush.
    // The copy's history is now the source's with the same rewrite applied
    // to every state it holds: History() is that rewrite on any JSON the
    // engine writes -- a project, a document, a journal -- at the places the
    // collection rewrites the current state (rush records, node srcs, the
    // stored values of a moved prop, clip props). DiscoverHistory() first
    // copies what history alone names (a logo replaced since, a node
    // removed since), when it can be read, so that undoing in the copy does
    // not reach outside it. A rush version relinked away is not copied: it
    // is footage the film no longer reads, and its size is not bounded.
    void SetRushPaths(
        std::map<std::pair<std::string, std::string>, std::string> paths) {
        rush_paths_ = std::move(paths);
    }

    // Which composition each motion clip instantiates, for an edit that
    // names only the clip (SetClipMotionProps). A clip keeps its
    // composition for life, so every state agrees.
    void LearnClips(const Project& project) {
        for (const auto& timeline : project.timelines) {
            for (const auto& track : timeline.tracks) {
                for (const auto& clip : track.clips) {
                    if (!clip.composition_id.empty()) {
                        clip_compositions_[clip.id] = clip.composition_id;
                    }
                }
            }
        }
    }
    void LearnClips(const mcp_json::Value& value) {
        if (value.IsArray()) {
            for (const auto& item : value.AsArray()) {
                LearnClips(item);
            }
            return;
        }
        if (!value.IsObject()) {
            return;
        }
        const mcp_json::Value* composition = value.Find("composition_id");
        if (composition != nullptr && composition->IsString() &&
            !composition->AsString().empty()) {
            for (const char* key : {"id", "clip_id"}) {
                const mcp_json::Value* clip = value.Find(key);
                if (clip != nullptr && clip->IsString()) {
                    clip_compositions_[clip->AsString()] =
                        composition->AsString();
                }
            }
        }
        for (const auto& field : value.AsObject()) {
            LearnClips(field.second);
        }
    }

    void DiscoverHistory(const mcp_json::Value& value) { Walk(value, true); }
    mcp_json::Value History(const mcp_json::Value& value) {
        return Walk(value, false);
    }

private:
    using Value = mcp_json::Value;
    struct Choice {
        Value value;
        // A default nothing in the film shows: collected when it names a
        // file, skipped when it does not.
        bool optional = false;
        // Passed on by an instance: the value is stored in the composition
        // that instantiates this one, where moving it would also change
        // what its own binding means.
        bool inherited = false;
    };
    struct Choices {
        std::map<std::string, Choice> values;
        bool dynamic = false;
        void Add(const Value& value, bool optional = false,
                 bool inherited = false) {
            const auto added = values.emplace(
                value.Dump(), Choice{value, optional, inherited});
            if (!added.second) {
                Choice& known = added.first->second;
                known.optional = known.optional && optional;
                known.inherited = known.inherited || inherited;
            }
        }
        void Merge(const Choices& other, bool inherited = false) {
            for (const auto& choice : other.values) {
                Add(choice.second.value, choice.second.optional,
                    choice.second.inherited || inherited);
            }
            dynamic = dynamic || other.dynamic;
        }
    };
    // How a composition's tree reads a prop: as the whole of a node's src,
    // or in any other way.
    struct PropUses {
        size_t src = 0;
        size_t other = 0;
    };
    // For one composition, by prop, a moved value's text and the value the
    // copy stores instead.
    using Moves = std::map<std::string, std::map<std::string, Value>>;
    struct Entry {
        Ulid id;
        MotionComposition composition;
        std::map<std::string, Choices> props;
        // Text props read only as a node's whole src.
        std::set<std::string> movable;
        // Props whose default a clip or an instance shows.
        std::set<std::string> shown_defaults;
    };

    static void AddProps(Entry& entry, const Value& props) {
        for (const auto& prop : props.AsObject()) {
            entry.props[prop.first].Add(prop.second);
        }
    }

    // Every {"prop": name} below `value`, wherever it stands. A binding
    // this collector knows nothing about counts against moving its prop.
    static void CountBindings(const Value& value,
                              std::map<std::string, PropUses>& uses) {
        if (value.IsArray()) {
            for (const auto& item : value.AsArray()) {
                CountBindings(item, uses);
            }
            return;
        }
        const Value* prop = value.Find("prop");
        if (prop != nullptr && prop->IsString()) {
            const std::string& spelled = prop->AsString();
            ++uses[spelled.substr(0, spelled.find('.'))].other;
        }
        for (const auto& field : value.AsObject()) {
            CountBindings(field.second, uses);
        }
    }

    // The definition's tree, as stored: a src that is exactly
    // {"prop": name} is the one use that lets the prop move.
    static void CountNodeUses(const Value& node,
                              std::map<std::string, PropUses>& uses) {
        for (const auto& field : node.AsObject()) {
            const Value* prop = field.second.Find("prop");
            if (field.first == "children") {
                for (const auto& child : field.second.AsArray()) {
                    CountNodeUses(child, uses);
                }
            } else if (field.first == "template") {
                CountNodeUses(field.second, uses);
            } else if (field.first == "src" &&
                       field.second.AsObject().size() == 1 && prop != nullptr &&
                       prop->IsString() &&
                       prop->AsString().find('.') == std::string::npos) {
                ++uses[prop->AsString()].src;
            } else {
                CountBindings(field.second, uses);
            }
        }
    }

    void MarkInstanceDefaults(const MotionNode& node) {
        if (!node.instance_of.empty()) {
            const auto target = compositions_.find(node.instance_of);
            if (target != compositions_.end()) {
                for (const auto& prop : target->second.composition.props) {
                    const bool passed = std::any_of(
                        node.instance_props.begin(), node.instance_props.end(),
                        [&](const auto& given) {
                            return given.first == prop.name;
                        });
                    if (!passed) {
                        target->second.shown_defaults.insert(prop.name);
                    }
                }
            }
        }
        for (const auto& child : node.children) {
            MarkInstanceDefaults(child);
        }
    }

    static Value Moved(const Moves& moves, const std::string& prop,
                       const Value& value) {
        const auto values = moves.find(prop);
        if (values == moves.end()) {
            return value;
        }
        const auto found = values->second.find(value.Dump());
        return found == values->second.end() ? value : found->second;
    }

    static Value MovedProps(const Moves& moves, const Value& props) {
        if (!props.IsObject()) {
            return props;
        }
        Value result = Value::MakeObject();
        for (const auto& prop : props.AsObject()) {
            result.Set(prop.first, Moved(moves, prop.first, prop.second));
        }
        return result;
    }

    // The values the definition itself stores for a moved prop: its
    // default, and its value in each variant and format. Key order and
    // every other byte stay as written.
    static void MoveStoredValues(const Moves& moves, Value& definition) {
        const Value* props = definition.Find("props");
        if (props != nullptr && props->IsObject()) {
            Value declared = Value::MakeObject();
            for (const auto& prop : props->AsObject()) {
                Value declaration = prop.second;
                const Value* fallback = declaration.Find("default");
                if (fallback != nullptr) {
                    Value moved = Moved(moves, prop.first, *fallback);
                    declaration.Set("default", std::move(moved));
                }
                declared.Set(prop.first, std::move(declaration));
            }
            definition.Set("props", std::move(declared));
        }
        for (const char* section : {"variants", "formats"}) {
            const Value* entries = definition.Find(section);
            if (entries == nullptr || !entries->IsObject()) {
                continue;
            }
            Value rewritten = Value::MakeObject();
            for (const auto& entry : entries->AsObject()) {
                Value item = entry.second;
                const Value* values = item.Find("props");
                if (values != nullptr) {
                    Value moved = MovedProps(moves, *values);
                    item.Set("props", std::move(moved));
                }
                rewritten.Set(entry.first, std::move(item));
            }
            definition.Set(section, std::move(rewritten));
        }
    }

    static Choices Resolve(const Entry& entry, const MotionValue& value,
                           const Choices& items) {
        Choices result;
        switch (value.kind) {
            case MotionBindingKind::Literal:
                result.Add(value.literal);
                break;
            case MotionBindingKind::Prop: {
                const auto found = entry.props.find(value.prop);
                if (found == entry.props.end()) {
                    result.dynamic = true;
                    break;
                }
                result.dynamic = found->second.dynamic;
                for (const auto& choice : found->second.values) {
                    const Value* component =
                        value.component.empty()
                            ? &choice.second.value
                            : choice.second.value.Find(value.component);
                    if (component == nullptr) {
                        continue;
                    }
                    Value resolved = *component;
                    for (const auto& prop : entry.composition.props) {
                        if (prop.name == value.prop && !prop.unit.empty() &&
                            resolved.IsNumber()) {
                            resolved =
                                Value::MakeString(resolved.Dump() + prop.unit);
                        }
                    }
                    result.Add(resolved, choice.second.optional,
                               choice.second.inherited);
                }
                break;
            }
            case MotionBindingKind::Item:
                result = items;
                break;
            case MotionBindingKind::PropMap:
            case MotionBindingKind::ItemMap:
                for (const auto& item : MapEntries(entry, value)) {
                    result.Add(item.second);
                }
                break;
            case MotionBindingKind::PropNotEmpty:
                result.Add(Value::MakeBool(false));
                result.Add(Value::MakeBool(true));
                break;
            case MotionBindingKind::Index:
            case MotionBindingKind::Timecode:
                result.dynamic = true;
                break;
        }
        return result;
    }

    static const std::vector<std::pair<std::string, Value>>& MapEntries(
        const Entry& entry, const MotionValue& value) {
        if (!value.table.empty()) {
            for (const auto& table : entry.composition.tokens) {
                if (table.name == value.table) {
                    return table.entries;
                }
            }
        }
        return value.inline_map;
    }

    static Choices ChildItems(const Entry& entry, const MotionNode& node,
                              const Choices& items) {
        if (node.type != MotionNodeType::Repeat) {
            return items;
        }
        Choices children;
        if (node.each) {
            const Choices lists = Resolve(entry, *node.each, items);
            children.dynamic = lists.dynamic;
            for (const auto& list : lists.values) {
                for (const auto& item : list.second.value.AsArray()) {
                    children.Add(item, list.second.optional,
                                 list.second.inherited);
                }
            }
        }
        return children;
    }

    void Propagate(const Entry& entry, const MotionNode& node,
                   const Choices& items) {
        if (!node.instance_of.empty()) {
            const auto target = compositions_.find(node.instance_of);
            if (target != compositions_.end()) {
                for (const auto& prop : node.instance_props) {
                    target->second.props[prop.first].Merge(
                        Resolve(entry, prop.second, items), true);
                }
            }
        }
        const Choices children = ChildItems(entry, node, items);
        for (const auto& child : node.children) {
            Propagate(entry, child, children);
        }
    }

    // Whether `value` names a file the source can read, as Copy resolves it.
    bool NamesFile(const Value& value) const {
        if (!value.IsString() || value.AsString().empty() ||
            value.AsString().find('\0') != std::string::npos) {
            return false;
        }
        const fs::path path(value.AsString());
        std::error_code ec;
        return fs::is_regular_file(path.is_absolute() ? path : source_ / path,
                                   ec) &&
               !ec;
    }

    // An `optional` value that names no file is left as it is spelled: the
    // copy shows what the source shows.
    bool Copy(const Value& value, bool relocatable, bool optional,
              Value& replacement, std::string& error) {
        if (optional && !NamesFile(value)) {
            replacement = value;
            return true;
        }
        if (!value.IsString() || value.AsString().empty() ||
            value.AsString().find('\0') != std::string::npos) {
            error = "src must name a non-empty file path";
            return false;
        }
        const fs::path path(value.AsString());
        fs::path relative = path.lexically_normal();
        std::string first = relative.begin()->string();
        std::transform(first.begin(), first.end(), first.begin(),
                       [](unsigned char byte) {
                           return static_cast<char>(std::tolower(byte));
                       });
        const bool reserved = first == "timelines" || first == "luts" ||
                              first == "manifest.json" ||
                              first.rfind("project.", 0) == 0 ||
                              first.rfind(".cutmachine", 0) == 0;
        const bool relocate =
            path.is_absolute() || HasParentTraversal(path) || reserved;
        if (relocate && !relocatable) {
            error = "src binding names an external or reserved path: '" +
                    path.string() +
                    "'; use a package-relative path or a literal src";
            return false;
        }
        const fs::path original = path.is_absolute() ? path : source_ / path;
        std::error_code ec;
        if (!fs::is_regular_file(original, ec) || ec) {
            error = "motion asset is missing or unreadable: '" +
                    original.string() + "'";
            return false;
        }
        std::string digest;
        if (!Sha256(original, digest, error)) {
            return false;
        }
        if (relocate) {
            relative = fs::path("Media") / "Motion" /
                       (digest + path.extension().string());
        }
        const fs::path copied = destination_ / relative;
        if (fs::exists(copied, ec) && !ec) {
            std::string existing;
            if (!Sha256(copied, existing, error)) {
                return false;
            }
            if (existing != digest) {
                error = "motion asset collides with another package file: " +
                        relative.generic_string();
                return false;
            }
        } else {
            if (!ec) {
                fs::create_directories(copied.parent_path(), ec);
            }
            if (!ec) {
                fs::copy_file(original, copied, ec);
            }
            if (ec) {
                error = "unable to collect motion asset '" + path.string() +
                        "': " + ec.message();
                return false;
            }
            std::string copiedDigest;
            if (!Sha256(copied, copiedDigest, error)) {
                return false;
            }
            if (copiedDigest != digest) {
                error =
                    "motion asset changed during collection: " + path.string();
                return false;
            }
            if (!SynchronizePath(copied, false, error)) {
                return false;
            }
            for (fs::path directory = copied.parent_path();
                 directory != destination_;
                 directory = directory.parent_path()) {
                if (!SynchronizePath(directory, true, error)) {
                    return false;
                }
            }
        }
        // Unrelocated bindings retain their original spelling and meaning.
        replacement =
            relocate ? Value::MakeString(relative.generic_string()) : value;
        if (relocate) {
            relocated_[value.AsString()] = replacement;
        }
        return true;
    }

    bool CollectNode(const Entry& entry, const MotionNode& node,
                     const Choices& items, std::string& error) {
        if (node.src) {
            const MotionValue& src = *node.src;
            Value replacement;
            if (src.kind == MotionBindingKind::Literal) {
                if (!Copy(src.literal, true, false, replacement, error)) {
                    return false;
                }
            } else if (src.kind == MotionBindingKind::PropMap ||
                       src.kind == MotionBindingKind::ItemMap) {
                Value map = Value::MakeObject();
                bool changed = false;
                for (const auto& item : MapEntries(entry, src)) {
                    Value path;
                    if (!Copy(item.second, src.table.empty(), false, path,
                              error)) {
                        return false;
                    }
                    changed = changed || path.Dump() != item.second.Dump();
                    map.Set(item.first, std::move(path));
                }
                if (changed) {
                    replacement = Value::MakeObject();
                    if (src.kind == MotionBindingKind::PropMap) {
                        replacement.Set(
                            "prop",
                            Value::MakeString(src.prop +
                                              (src.component.empty()
                                                   ? ""
                                                   : "." + src.component)));
                    } else {
                        replacement.Set("item", Value::MakeBool(true));
                    }
                    replacement.Set("map", std::move(map));
                }
            } else {
                // A movable prop's own values move like literals, where the
                // definition and the clips store them (MoveStoredValues,
                // MovedProps); a value an instance passes on cannot.
                const bool movable = src.kind == MotionBindingKind::Prop &&
                                     src.component.empty() &&
                                     entry.movable.count(src.prop) != 0;
                const Choices paths = Resolve(entry, src, items);
                if (paths.dynamic) {
                    error = "cannot enumerate the motion src binding";
                    return false;
                }
                for (const auto& path : paths.values) {
                    const Choice& choice = path.second;
                    Value copied;
                    if (!Copy(choice.value, movable && !choice.inherited,
                              choice.optional, copied, error)) {
                        return false;
                    }
                    if (movable && copied.Dump() != path.first) {
                        moves_[entry.id][src.prop].emplace(path.first,
                                                           std::move(copied));
                    }
                }
                replacement = Value::MakeNull();
            }
            if (!replacement.IsNull()) {
                replacements_[node.uid] = replacement;
            }
        }
        const Choices children = ChildItems(entry, node, items);
        for (const auto& child : node.children) {
            if (!CollectNode(entry, child, children, error)) {
                return false;
            }
        }
        return true;
    }

    Value Rewrite(const Value& value) const {
        const Value* uid = value.Find("uid");
        const auto replacement = replacements_.find(
            (uid != nullptr) && uid->IsString() ? uid->AsString() : "");
        Value result = Value::MakeObject();
        for (const auto& item : value.AsObject()) {
            if (item.first == "children") {
                Value children = Value::MakeArray();
                for (const auto& child : item.second.AsArray()) {
                    children.Push(Rewrite(child));
                }
                result.Set(item.first, std::move(children));
            } else if (item.first == "template") {
                result.Set(item.first, Rewrite(item.second));
            } else {
                result.Set(
                    item.first,
                    item.first == "src" && replacement != replacements_.end()
                        ? replacement->second
                        : item.second);
            }
        }
        return result;
    }

    // FI12 -- what history names, copied when it can be read. A failure
    // leaves the value as it is spelled: history is not a reason to refuse
    // a collection that the current state allows.
    void DiscoverPath(const Value& value) {
        if (!value.IsString() || relocated_.count(value.AsString()) != 0 ||
            !attempted_.insert("src:" + value.Dump()).second) {
            return;
        }
        Value ignored;
        std::string error;
        Copy(value, true, true, ignored, error);
    }

    void DiscoverProp(const Ulid& composition, const std::string& prop,
                      const Value& value) {
        const auto entry = compositions_.find(composition);
        if (entry == compositions_.end() ||
            entry->second.movable.count(prop) == 0) {
            return;
        }
        const std::string text = value.Dump();
        auto& moved = moves_[composition][prop];
        if (moved.count(text) != 0 ||
            !attempted_.insert(composition + ':' + prop + ':' + text).second) {
            return;
        }
        Value copied;
        std::string error;
        if (Copy(value, true, true, copied, error) && copied.Dump() != text) {
            moved.emplace(text, std::move(copied));
        }
    }

    // The node tree as Rewrite spells it in the current state: a literal
    // src by its copy, an inline map by its copies under the binding's own
    // name, every other src as written.
    Value HistorySrc(const Value& src, bool discover) {
        if (src.IsString()) {
            if (discover) {
                DiscoverPath(src);
                return src;
            }
            const auto found = relocated_.find(src.AsString());
            return found == relocated_.end() ? src : found->second;
        }
        const Value* map = src.Find("map");
        if (map == nullptr || !map->IsObject() ||
            src.Find("table") != nullptr) {
            return src;
        }
        Value rewritten = Value::MakeObject();
        bool changed = false;
        for (const auto& item : map->AsObject()) {
            Value path = HistorySrc(item.second, discover);
            changed = changed || path.Dump() != item.second.Dump();
            rewritten.Set(item.first, std::move(path));
        }
        if (!changed) {
            return src;
        }
        Value replacement = Value::MakeObject();
        if (const Value* prop = src.Find("prop")) {
            replacement.Set("prop", *prop);
        } else {
            replacement.Set("item", Value::MakeBool(true));
        }
        replacement.Set("map", std::move(rewritten));
        return replacement;
    }

    Value HistoryNode(const Value& node, bool discover) {
        if (!node.IsObject()) {
            return node;
        }
        Value result = Value::MakeObject();
        for (const auto& item : node.AsObject()) {
            if (item.first == "children" && item.second.IsArray()) {
                Value children = Value::MakeArray();
                for (const auto& child : item.second.AsArray()) {
                    children.Push(HistoryNode(child, discover));
                }
                result.Set(item.first, std::move(children));
            } else if (item.first == "template") {
                result.Set(item.first, HistoryNode(item.second, discover));
            } else if (item.first == "src") {
                result.Set(item.first, HistorySrc(item.second, discover));
            } else {
                result.Set(item.first, item.second);
            }
        }
        return result;
    }

    Value HistoryProps(const Ulid& composition, const Value& props,
                       bool discover) {
        if (!props.IsObject()) {
            return props;
        }
        if (discover) {
            for (const auto& prop : props.AsObject()) {
                DiscoverProp(composition, prop.first, prop.second);
            }
            return props;
        }
        const auto moves = moves_.find(composition);
        return moves == moves_.end() ? props : MovedProps(moves->second, props);
    }

    // A composition as stored: its tree, then the values it stores for a
    // moved prop (MoveStoredValues), as Collect rewrites the current one.
    Value HistoryDefinition(const Ulid& composition, const Value& definition,
                            bool discover) {
        Value result = Value::MakeObject();
        for (const auto& item : definition.AsObject()) {
            result.Set(item.first, item.first == "root"
                                       ? HistoryNode(item.second, discover)
                                       : item.second);
        }
        if (discover) {
            if (const Value* props = definition.Find("props");
                props != nullptr && props->IsObject()) {
                for (const auto& prop : props->AsObject()) {
                    if (const Value* fallback = prop.second.Find("default")) {
                        DiscoverProp(composition, prop.first, *fallback);
                    }
                }
            }
            for (const char* section : {"variants", "formats"}) {
                const Value* entries = definition.Find(section);
                if (entries == nullptr || !entries->IsObject()) {
                    continue;
                }
                for (const auto& entry : entries->AsObject()) {
                    if (const Value* values = entry.second.Find("props")) {
                        HistoryProps(composition, *values, true);
                    }
                }
            }
            return result;
        }
        const auto moves = moves_.find(composition);
        if (moves != moves_.end()) {
            MoveStoredValues(moves->second, result);
        }
        return result;
    }

    static std::string HexText(const std::string& bytes) {
        static constexpr char kDigits[] = "0123456789abcdef";
        std::string text;
        text.reserve(bytes.size() * 2);
        for (const unsigned char byte : bytes) {
            text.push_back(kDigits[byte >> 4]);
            text.push_back(kDigits[byte & 0x0f]);
        }
        return text;
    }

    static bool HexBytes(const std::string& text, std::string& bytes) {
        if (text.size() % 2 != 0) {
            return false;
        }
        const auto digit = [](char character) -> int {
            if (character >= '0' && character <= '9') return character - '0';
            if (character >= 'a' && character <= 'f')
                return character - 'a' + 10;
            if (character >= 'A' && character <= 'F')
                return character - 'A' + 10;
            return -1;
        };
        bytes.clear();
        for (size_t index = 0; index < text.size(); index += 2) {
            const int high = digit(text[index]);
            const int low = digit(text[index + 1]);
            if (high < 0 || low < 0) {
                return false;
            }
            bytes.push_back(static_cast<char>(high * 16 + low));
        }
        return true;
    }

    // FI12 -- every place the collection rewrites, wherever a project, a
    // document or a journal holds it: a rush record (its id and the path it
    // had), a composition (`id` beside `definition`), a node posed by
    // SetMotionNode, a clip's props (`composition_id` beside
    // `motion_props`, which InsertMotionClip shares), the props of
    // SetMotionProps and SetClipMotionProps, and the whole project a
    // journal written before S6 stores in hex. Everything else is walked
    // through unchanged, so a batch, a delta or an entry finds what it
    // holds.
    Value Walk(const Value& value, bool discover) {
        if (value.IsArray()) {
            Value result = Value::MakeArray();
            for (const auto& item : value.AsArray()) {
                result.Push(Walk(item, discover));
            }
            return result;
        }
        if (!value.IsObject()) {
            return value;
        }
        const auto text = [&value](const char* key) -> std::string {
            const Value* found = value.Find(key);
            return found != nullptr && found->IsString() ? found->AsString()
                                                         : std::string();
        };
        const Value* id = value.Find("id");
        const Value* path = value.Find("path");
        const auto rush =
            id != nullptr && id->IsString() && path != nullptr &&
                    path->IsString()
                ? rush_paths_.find({id->AsString(), path->AsString()})
                : rush_paths_.end();
        const Value* definition = value.Find("definition");
        const bool composition = id != nullptr && id->IsString() &&
                                 definition != nullptr &&
                                 definition->IsObject();
        const std::string type = text("type");
        Value result = Value::MakeObject();
        for (const auto& item : value.AsObject()) {
            const std::string& key = item.first;
            if (rush != rush_paths_.end() && key == "path") {
                result.Set(key, Value::MakeString(rush->second));
            } else if (rush != rush_paths_.end() && key == "proxy_path") {
                // Cleared, as the collection clears the current one: a
                // derived cache the receiver regenerates.
            } else if (key == "exact_project_hex" && item.second.IsString()) {
                std::string bytes;
                Value project;
                std::string error;
                if (HexBytes(item.second.AsString(), bytes) &&
                    Value::Parse(bytes, project, error)) {
                    result.Set(key, Value::MakeString(HexText(
                                        Walk(project, discover).Dump())));
                } else {
                    result.Set(key, item.second);
                }
            } else if (composition && key == "definition") {
                result.Set(key, HistoryDefinition(id->AsString(), item.second,
                                                  discover));
            } else if (type == "SetMotionNode" && key == "node") {
                result.Set(key, HistoryNode(item.second, discover));
            } else if (key == "motion_props" &&
                       !text("composition_id").empty()) {
                result.Set(key, HistoryProps(text("composition_id"),
                                             item.second, discover));
            } else if (key == "props" && type == "SetMotionProps") {
                result.Set(key, HistoryProps(text("composition_id"),
                                             item.second, discover));
            } else if (key == "props" && type == "SetClipMotionProps") {
                const auto clip = clip_compositions_.find(text("clip_id"));
                result.Set(key, clip == clip_compositions_.end()
                                    ? item.second
                                    : HistoryProps(clip->second, item.second,
                                                   discover));
            } else if (key == "replacements" && type == "RelinkProjectMedia") {
                // What the person asked for, at the source: a journaled
                // relink replays its exact delta, never these.
                result.Set(key, item.second);
            } else {
                result.Set(key, Walk(item.second, discover));
            }
        }
        return result;
    }

    fs::path source_;
    fs::path destination_;
    std::map<Ulid, Entry> compositions_;
    std::map<Ulid, Value> replacements_;
    std::map<Ulid, Moves> moves_;
    // FI12 -- a relocated path's copy, by the path as spelled; a rush's
    // package path, by its id and the path it had; each motion clip's
    // composition; what discovery already tried.
    std::map<std::string, Value> relocated_;
    std::map<std::pair<std::string, std::string>, std::string> rush_paths_;
    std::map<Ulid, Ulid> clip_compositions_;
    std::set<std::string> attempted_;
};

bool LoadTimelineLogs(const std::string& projectPath, const Project& project,
                      std::map<std::string, EditLog>& logs,
                      std::string& error) {
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : project.timelines) {
        EditLog log;
        const fs::path path =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        std::error_code existsError;
        if (fs::exists(path, existsError)) {
            if (!EditLog::Load(path.string(), log, editError, error))
                return false;
        }
        if (existsError) {
            error =
                "unable to inspect timeline history: " + existsError.message();
            return false;
        }
        logs.emplace(timeline.id, std::move(log));
    }
    return true;
}

std::string Hostname() {
    std::array<char, 256> value{};
    if (gethostname(value.data(), value.size() - 1) != 0) return "unknown";
    return value.data();
}

std::string Username() {
    const char* value = std::getenv("USER");
    return value && *value ? value : "unknown";
}

bool ReadLockOwner(const fs::path& directory, std::string& token,
                   std::string& host, int64_t& pid, std::string& owner) {
    std::ifstream input(directory / "owner");
    if (!input) return false;
    std::string user;
    std::getline(input, token);
    std::getline(input, host);
    std::string pidText;
    std::getline(input, pidText);
    std::getline(input, user);
    try {
        pid = std::stoll(pidText);
    } catch (...) {
        return false;
    }
    owner = user + " sur " + host + " (PID " + pidText + ")";
    return !token.empty() && !host.empty() && pid > 0;
}

// SAVING_ROADMAP.md, forced-stop validation (24 September 2026). A lock
// whose metadata cannot be read after this long was abandoned by a process
// killed while writing it: writing four short lines takes milliseconds.
// Thirty seconds leave room for exFAT, which keeps modification times to two
// seconds, and for a slow volume.
constexpr std::chrono::seconds kAbandonedLockAge{30};

bool OlderThan(const fs::path& path, std::chrono::seconds age) {
    std::error_code error;
    const fs::file_time_type written = fs::last_write_time(path, error);
    return !error && fs::file_time_type::clock::now() - written > age;
}

bool WriteLockOwner(const fs::path& directory, const std::string& token) {
    std::ofstream output(directory / "owner", std::ios::trunc);
    if (!output) return false;
    output << token << '\n'
           << Hostname() << '\n'
           << getpid() << '\n'
           << Username() << '\n';
    output.close();
    return static_cast<bool>(output);
}

// Whether the owner a lock names is gone: a live owner on this host keeps
// it; one on another host cannot be checked from here and keeps it too.
bool OwnerIsGone(const std::string& host, int64_t pid) {
    return host == Hostname() && kill(static_cast<pid_t>(pid), 0) != 0 &&
           errno == ESRCH;
}

// The staging directories Acquire writes a lock's metadata into before
// publishing it, left behind by a process killed in between.
void RemoveAbandonedLockStaging(const fs::path& projectPath) {
    const std::string prefix = projectPath.filename().string() + ".lock-";
    std::vector<fs::path> abandoned;
    std::error_code error;
    for (fs::directory_iterator item(projectPath.parent_path(), error), end;
         !error && item != end; item.increment(error)) {
        const std::string name = item->path().filename().string();
        std::error_code typeError;
        if (name.rfind(prefix, 0) != 0 || !item->is_directory(typeError)) {
            continue;
        }
        std::string token;
        std::string host;
        std::string owner;
        int64_t pid = 0;
        const bool readable =
            ReadLockOwner(item->path(), token, host, pid, owner);
        if (readable ? OwnerIsGone(host, pid)
                     : OlderThan(item->path(), kAbandonedLockAge)) {
            abandoned.push_back(item->path());
        }
    }
    for (const fs::path& path : abandoned) {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
}

struct MediaFingerprint {
    uint64_t bytes = 0;
    std::string sha256;
};

// S4 -- SAVING_ROADMAP.md. The fingerprints of a collection manifest, read
// as JSON. They used to be searched as text, up to the first '}' after the
// media's id: a filename holding one ended the search before the
// fingerprint, and the collected original then passed for a rush left
// outside the package, which nothing verifies. Parsed once, and a
// fingerprint that cannot be read refuses the package instead of being
// skipped. (First written on codex/ai-edit-plans, 5 September 2026.)
bool ReadMediaFingerprints(
    const std::string& json,
    std::map<std::string, MediaFingerprint>& fingerprints, std::string& error) {
    mcp_json::Value manifest;
    if (!mcp_json::Value::Parse(json, manifest, error)) {
        error = "invalid collection manifest: " + error;
        return false;
    }
    const auto* format = manifest.Find("format");
    const auto* version = manifest.Find("version");
    int64_t versionNumber = 0;
    if (format == nullptr || !format->IsString() ||
        format->AsString() != "cutmachine-collection" || version == nullptr ||
        !version->AsInt64(versionNumber) ||
        versionNumber != kNewestPackageVersion) {
        // PRD20 -- a newer package says it is newer, as a document does.
        error = versionNumber > kNewestPackageVersion
                    ? NewerFormatRefusal("project package", versionNumber,
                                         kNewestPackageVersion)
                    : "unsupported collection manifest";
        return false;
    }
    const auto* media = manifest.Find("media");
    if (media == nullptr || !media->IsArray()) {
        error = "collection manifest has no media array";
        return false;
    }
    for (const auto& entry : media->AsArray()) {
        const auto* id = entry.Find("id");
        const auto* bytes = entry.Find("bytes");
        const auto* sha = entry.Find("sha256");
        if (id == nullptr || !id->IsString() || !IsValidUlid(id->AsString())) {
            error = "collection manifest has an invalid media id";
            return false;
        }
        error = "collection manifest has an invalid fingerprint for " +
                id->AsString();
        if (bytes == nullptr || !bytes->IsNumber() || sha == nullptr ||
            !sha->IsString())
            return false;
        // A byte count is a whole number that fits: no sign, no fraction,
        // no exponent, read from its own text and never through a double.
        const std::string byteText = bytes->Dump();
        MediaFingerprint fingerprint;
        const auto parsed =
            std::from_chars(byteText.data(), byteText.data() + byteText.size(),
                            fingerprint.bytes);
        if (parsed.ec != std::errc{} ||
            parsed.ptr != byteText.data() + byteText.size())
            return false;
        fingerprint.sha256 = sha->AsString();
        if (fingerprint.sha256.size() != CC_SHA256_DIGEST_LENGTH * 2 ||
            fingerprint.sha256.find_first_not_of("0123456789abcdef") !=
                std::string::npos)
            return false;
        if (!fingerprints.emplace(id->AsString(), std::move(fingerprint))
                 .second)
            return false;
    }
    error.clear();
    return true;
}

bool ReadFile(const fs::path& path, std::string& contents, std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to open '" + path.string() + "'";
        return false;
    }
    std::ostringstream output;
    output << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read '" + path.string() + "'";
        return false;
    }
    contents = output.str();
    return true;
}

bool ExtractManifestArray(const std::string& json, const std::string& key,
                          std::string& array, std::string& error) {
    const size_t keyPosition = json.find("\"" + key + "\"");
    const size_t begin = keyPosition == std::string::npos
                             ? keyPosition
                             : json.find('[', keyPosition);
    if (begin == std::string::npos) {
        error = "collection manifest has no " + key + " array";
        return false;
    }
    bool quoted = false;
    bool escaped = false;
    size_t depth = 0;
    for (size_t index = begin; index < json.size(); ++index) {
        const char character = json[index];
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                quoted = false;
            continue;
        }
        if (character == '"') {
            quoted = true;
        } else if (character == '[') {
            ++depth;
        } else if (character == ']' && --depth == 0) {
            array = json.substr(begin, index - begin + 1);
            return true;
        }
    }
    error = "collection manifest has an invalid " + key + " array";
    return false;
}

std::string BuildManifest(const Project& project,
                          const std::string& mediaArray) {
    std::ostringstream manifest;
    manifest << "{\n  \"format\":\"cutmachine-collection\",\n"
             << "  \"version\":2,\n"
             << "  \"project\":\"project.cutmachine.json\",\n"
             << "  \"timelines\":[";
    for (size_t index = 0; index < project.timelines.size(); ++index) {
        const DocumentSequence& timeline = project.timelines[index];
        manifest << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                 << EscapeJson(timeline.id) << "\",\"path\":\"Timelines/"
                 << EscapeJson(timeline.id) << ".json\"}";
    }
    if (!project.timelines.empty()) manifest << '\n';
    manifest << "  ],\n  \"media\":" << mediaArray << "\n}\n";
    return manifest.str();
}

Document StandaloneTimelineDocument(const Project& project,
                                    const DocumentSequence& timeline) {
    Document document;
    document.sequence = timeline;
    document.color_management = project.settings.color_management;
    return document;
}

// PRD20 -- ROADMAP.md. Why `projectPath` is not a package this build opens,
// once IsPortableProjectV2 has said it is not: nothing is there, the package
// was written by a more recent CUTMACHINE, or it is not a package at all --
// which keeps the sentence every such refusal used to get.
std::string PackageRefusal(const std::string& projectPath) {
    if (IsMissingProjectPath(projectPath)) {
        return MissingProjectMessage(projectPath);
    }
    std::string contents;
    std::string ignored;
    mcp_json::Value manifest;
    if (ReadFile(fs::path(projectPath).parent_path() / "manifest.json",
                 contents, ignored) &&
        mcp_json::Value::Parse(contents, manifest, ignored) &&
        manifest.IsObject()) {
        const auto* format = manifest.Find("format");
        const auto* version = manifest.Find("version");
        int64_t versionNumber = 0;
        if (format != nullptr && format->IsString() &&
            format->AsString() == "cutmachine-collection" &&
            version != nullptr && version->AsInt64(versionNumber) &&
            versionNumber > kNewestPackageVersion) {
            return NewerFormatRefusal("project package", versionNumber,
                                      kNewestPackageVersion);
        }
    }
    return "unsupported project package (CUTMACHINE package v2 required)";
}

// DB2 -- the outbox is itself a canonical Project, so a new document field
// cannot disappear from deferred delivery. Its filename is the retry ULID.
constexpr char kArchivePrefix[] = ".cutmachine-archive-";

std::vector<fs::path> PendingArchives(const fs::path& base,
                                      std::string& error) {
    std::vector<fs::path> paths;
    std::error_code ioError;
    for (fs::directory_iterator it(base, ioError), end; !ioError && it != end;
         it.increment(ioError)) {
        const std::string filename = it->path().filename().string();
        if (filename.rfind(kArchivePrefix, 0) != 0 ||
            it->path().extension() != ".json") {
            continue;
        }
        const std::string id =
            it->path().stem().string().substr(std::strlen(kArchivePrefix));
        if (!IsValidUlid(id)) {
            continue;
        }
        paths.push_back(it->path());
    }
    if (ioError) {
        error =
            "unable to inspect pending project captures: " + ioError.message();
    }
    std::sort(paths.begin(), paths.end());
    return paths;
}

void DeliverProjectArchive(const std::string& projectPath) {
    if (project_archive::DefaultDatabasePath().empty()) {
        return;
    }
    const StoredProjectArchiveReport report =
        StoredProjectArchiveStatus(projectPath, true);
    if (!report.error_name.empty()) {
        // The file commit succeeded: never turn an archive refusal into a
        // failed edit that an agent could apply twice. Pending JSON remains.
        std::fprintf(stderr,
                     "Project saved; SQL archive pending (%llu): %s: %s\n",
                     static_cast<unsigned long long>(report.pending),
                     report.error_name.c_str(), report.message.c_str());
    }
}

}  // namespace

static StoredProjectArchiveReport DeliverPendingArchives(
    const std::string& projectPath, bool retry) {
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    StoredProjectArchiveReport report;
    report.database = project_archive::DefaultDatabasePath();
    report.enabled = !report.database.empty();
    const fs::path base = fs::path(projectPath).parent_path();
    // A read-only CLI command may load a package beside the app. Delivery
    // must not recover or consume an outbox the other process is publishing.
    // Within this process, the I/O mutex serializes us with its lock owner.
    ProjectSessionLock deliveryLock;
    if (retry) {
        std::string token;
        std::string host;
        std::string owner;
        int64_t pid = 0;
        const bool alreadyHeld = ReadLockOwner(fs::path(projectPath + ".lock"),
                                               token, host, pid, owner) &&
                                 host == Hostname() && pid == getpid();
        if (!alreadyHeld &&
            !deliveryLock.Acquire(projectPath, owner, report.message)) {
            report.error_name = owner.empty() ? "IoError" : "ProjectLocked";
            std::string ignored;
            report.pending = PendingArchives(base, ignored).size();
            return report;
        }
    }
    if (retry &&
        !RecoverTextArtifactTransaction(base.string(), report.message)) {
        report.error_name = "IoError";
        return report;
    }
    const auto paths = PendingArchives(base, report.message);
    report.pending = paths.size();
    if (!report.message.empty()) {
        report.error_name = "IoError";
        return report;
    }
    if (!retry || !report.enabled || paths.empty()) {
        return report;
    }
    if (!fs::path(report.database).is_absolute()) {
        report.error_name = "ValidationFailed";
        report.message =
            "CUTMACHINE_PROJECT_ARCHIVE must be an absolute path "
            "or off";
        return report;
    }
    std::error_code ioError;
    fs::create_directories(fs::path(report.database).parent_path(), ioError);
    if (ioError) {
        report.error_name = "ArchiveError";
        report.message =
            "unable to create archive directory: " + ioError.message();
        return report;
    }
    for (const fs::path& path : paths) {
        const auto status = fs::symlink_status(path, ioError);
        if (ioError || !fs::is_regular_file(status)) {
            report.error_name = "IoError";
            report.message =
                "pending capture must be a regular file: " + path.string();
            break;
        }
        const auto bytes = fs::file_size(path, ioError);
        if (ioError || bytes > uintmax_t{64} * 1024 * 1024) {
            report.error_name = "ArchiveLimit";
            report.message = "pending capture exceeds 64 MiB or is unreadable";
            break;
        }
        std::string json;
        Project captured;
        if (!ReadFile(path, json, report.message) ||
            !Project::LoadFromString(json, captured, report.message)) {
            report.error_name = "ValidationFailed";
            break;
        }
        // Do not silently rewrite an outbox produced by an incompatible
        // future serializer, or deliver corrupted noncanonical bytes.
        if (captured.SaveToString() != json) {
            report.error_name = "ValidationFailed";
            report.message = "pending capture is not canonical project JSON";
            break;
        }
        const Ulid id =
            path.stem().string().substr(std::strlen(kArchivePrefix));
        project_archive::CaptureContext context{id, "", "save", ""};
        const fs::path contextPath =
            path.parent_path() /
            (std::string(kArchivePrefix) + id + ".context.json");
        const auto contextStatus = fs::symlink_status(contextPath, ioError);
        const bool hasContext =
            contextStatus.type() != fs::file_type::not_found;
        if (ioError && ioError != std::errc::no_such_file_or_directory) {
            report.error_name = "IoError";
            report.message = "unable to inspect pending run context";
            break;
        }
        ioError.clear();
        if (hasContext) {
            if (ioError || !fs::is_regular_file(contextStatus) ||
                fs::file_size(contextPath, ioError) > kMaxRunStepBytes ||
                ioError ||
                !ReadFile(contextPath, context.run_step_json, report.message)) {
                report.error_name = "IoError";
                report.message =
                    "pending run context must be a bounded "
                    "regular file";
                break;
            }
            RunStepReceipt receipt;
            if (!ParseRunStepReceipt(context.run_step_json, receipt,
                                     report.message) ||
                receipt.snapshot_id != id) {
                report.error_name = "ValidationFailed";
                break;
            }
            context.run_id = receipt.run_id;
            context.step = receipt.stage;
        }
        mcp_json::Value result;
        if (!project_archive::Capture(report.database, captured, context,
                                      result, report.error_name,
                                      report.message)) {
            break;
        }
        fs::remove(path, ioError);
        if (ioError) {
            report.error_name = "IoError";
            report.message =
                "unable to remove delivered capture: " + ioError.message();
            break;
        }
        ++report.archived;
        --report.pending;
        if (hasContext) {
            // OR2 -- persist the JSON removal before removing its context.
            // Without this directory sync, a power cut could retain only
            // the JSON and make a run capture look like an ordinary save.
            if (!SynchronizePath(base, true, report.message)) {
                report.error_name = "IoError";
                break;
            }
            fs::remove(contextPath, ioError);
            if (ioError) {
                report.error_name = "IoError";
                report.message = "unable to remove delivered run context";
                break;
            }
        }
        if (!SynchronizePath(base, true, report.message)) {
            report.error_name = "IoError";
            break;
        }
    }
    return report;
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The day's copy follows a
// delivery that went through, outside the package lock: it reads the
// archive, never the package. At most once a day (BackupMode::kIfDue).
StoredProjectArchiveReport StoredProjectArchiveStatus(
    const std::string& projectPath, bool retry, bool describeBackup) {
    StoredProjectArchiveReport report =
        DeliverPendingArchives(projectPath, retry);
    using mcp_json::Value;
    const std::string directory = project_archive::BackupDirectory();
    if (directory.empty()) {
        if (describeBackup) {
            report.backup = Value::MakeObject();
            report.backup.Set("configured", Value::MakeBool(false));
        }
        return report;
    }
    const std::time_t now = std::time(nullptr);
    project_archive::BackupReport copied;
    const bool attempted = retry && report.enabled &&
                           report.error_name.empty() && report.archived > 0;
    if (attempted && !project_archive::Backup(
                         report.database, directory,
                         project_archive::BackupMode::kIfDue, now, copied)) {
        std::fprintf(stderr, "Project archived; daily copy refused: %s: %s\n",
                     copied.error_name.c_str(), copied.message.c_str());
    }
    if (!describeBackup) {
        return report;
    }
    report.backup = project_archive::BackupStateJson(
        project_archive::InspectBackups(directory, now));
    if (attempted) {
        report.backup.Set("performed", Value::MakeBool(copied.performed));
        if (copied.performed) {
            report.backup.Set("path", Value::MakeString(copied.path));
        }
        if (!copied.removed.empty()) {
            Value removed = Value::MakeArray();
            for (const std::string& name : copied.removed) {
                removed.Push(Value::MakeString(name));
            }
            report.backup.Set("removed", std::move(removed));
        }
        if (!copied.error_name.empty()) {
            report.backup.Set("error", Value::MakeString(copied.error_name));
            report.backup.Set("detail", Value::MakeString(copied.message));
        }
    }
    return report;
}

mcp_json::Value StoredProjectArchiveReportJson(
    const StoredProjectArchiveReport& report) {
    using mcp_json::Value;
    Value result = Value::MakeObject();
    result.Set("ok", Value::MakeBool(true));
    result.Set("database", Value::MakeString(report.database));
    result.Set("enabled", Value::MakeBool(report.enabled));
    result.Set("archived", Value::MakeInt(report.archived));
    result.Set("pending", Value::MakeInt(report.pending));
    result.Set("complete", Value::MakeBool(report.pending == 0 &&
                                           report.error_name.empty()));
    if (!report.error_name.empty()) {
        result.Set("error", Value::MakeString(report.error_name));
        result.Set("detail", Value::MakeString(report.message));
    }
    if (!report.backup.IsNull()) {
        result.Set("backup", report.backup);
    }
    return result;
}

bool ImportStoredProjectsToArchive(const std::string& database,
                                   const std::vector<std::string>& projectPaths,
                                   mcp_json::Value& result,
                                   std::string& errorName,
                                   std::string& message) {
    using mcp_json::Value;
    if (!fs::path(database).is_absolute() || projectPaths.empty() ||
        projectPaths.size() > 100) {
        errorName = "ValidationFailed";
        message =
            "import needs an absolute database path and 1 to 100 projects";
        return false;
    }
    for (const auto& path : projectPaths) {
        if (!fs::path(path).is_absolute()) {
            errorName = "ValidationFailed";
            message = "import project paths must be absolute";
            return false;
        }
    }
    std::error_code directoryError;
    fs::create_directories(fs::path(database).parent_path(), directoryError);
    if (directoryError) {
        errorName = "ArchiveError";
        message =
            "unable to create archive directory: " + directoryError.message();
        return false;
    }
    Value items = Value::MakeArray();
    int64_t imported = 0;
    int64_t refused = 0;
    for (const auto& requested : projectPaths) {
        const fs::path path =
            fs::path(requested).filename() == "project.cutmachine.json"
                ? fs::path(requested)
                : fs::path(requested) / "project.cutmachine.json";
        ProjectSessionLock lock;
        std::string owner;
        std::string detail;
        std::string code;
        Project project;
        Value capture;
        bool ok = lock.Acquire(path.string(), owner, detail);
        if (!ok) {
            code = owner.empty() ? "IoError" : "ProjectLocked";
        } else if (!LoadStoredProject(path.string(), project, detail)) {
            ok = false;
            code = "IoError";
        } else {
            ok = project_archive::CaptureImport(database, project, capture,
                                                code, detail);
        }
        Value item = Value::MakeObject();
        item.Set("project", Value::MakeString(path.string()));
        item.Set("ok", Value::MakeBool(ok));
        if (ok) {
            ++imported;
            item.Set("capture", std::move(capture));
        } else {
            ++refused;
            item.Set("error", Value::MakeString(code));
            item.Set("detail", Value::MakeString(detail));
        }
        items.Push(std::move(item));
    }
    result = Value::MakeObject();
    result.Set("ok", Value::MakeBool(true));
    result.Set("complete", Value::MakeBool(refused == 0));
    result.Set("imported", Value::MakeInt(imported));
    result.Set("refused", Value::MakeInt(refused));
    result.Set("projects", std::move(items));
    errorName.clear();
    message.clear();
    return true;
}

bool IsMissingProjectPath(const std::string& projectPath) {
    const fs::path path(projectPath);
    std::error_code error;
    // A path that cannot be inspected is not known to be empty.
    if (fs::exists(path, error) || error) {
        return false;
    }
    // The project file of a folder that exists: the folder is there and is
    // not a package, which is another refusal than a wrong path.
    return !(path.filename() == "project.cutmachine.json" &&
             fs::is_directory(path.parent_path(), error));
}

std::string MissingProjectMessage(const std::string& projectPath) {
    return kMissingProjectPrefix + projectPath +
           "': nothing exists at this path";
}

bool IsMissingProjectMessage(const std::string& message) {
    return message.find(kMissingProjectPrefix) != std::string::npos;
}

std::recursive_mutex& StoredProjectIoMutex() {
    static std::recursive_mutex mutex;
    return mutex;
}

namespace {

// Moved under StoredProjectIoMutex, after the write, so a reader that read
// it before parsing can only see it move -- never a new value over an old
// package. Atomic because it is read outside the mutex.
std::atomic<uint64_t> gStoredProjectCommits{0};

}  // namespace

uint64_t StoredProjectCommitGeneration() {
    return gStoredProjectCommits.load(std::memory_order_acquire);
}

bool IsPortableProjectV2(const std::string& projectPath) {
    const fs::path package = fs::path(projectPath).parent_path();
    std::string recoveryError;
    if (!RecoverTextArtifactTransaction(package.string(), recoveryError))
        return false;
    const fs::path manifest = package / "manifest.json";
    std::string contents;
    std::string error;
    if (!ReadFile(manifest, contents, error)) {
        return false;
    }
    // PRD39 -- ROADMAP.md. Read as JSON, as the manifest's own reader
    // (ReadMediaFingerprints) reads it: a search for the text
    // `"version":2` refused a manifest written back with a space after the
    // colon. PRD20 -- and version 2, not a version that starts with a 2.
    mcp_json::Value parsed;
    if (!mcp_json::Value::Parse(contents, parsed, error) ||
        !parsed.IsObject()) {
        return false;
    }
    const mcp_json::Value* format = parsed.Find("format");
    const mcp_json::Value* version = parsed.Find("version");
    int64_t versionNumber = 0;
    return format != nullptr && format->IsString() &&
           format->AsString() == "cutmachine-collection" &&
           version != nullptr && version->AsInt64(versionNumber) &&
           versionNumber == kNewestPackageVersion;
}

bool CreatePortableProject(const std::string& destinationDirectory,
                           const Project& project, std::string& projectPath,
                           std::string& error) {
    projectPath.clear();
    if (!project.Validate(error)) return false;
    const fs::path destination = fs::absolute(destinationDirectory);
    std::error_code pathError;
    if (fs::exists(destination, pathError) || pathError) {
        error = pathError
                    ? "unable to inspect destination: " + pathError.message()
                    : "destination already exists";
        return false;
    }
    // AGENT-2026-09 A28 -- a missing parent is created, as a shell's
    // mkdir -p would: on the bench of the 25 September the run's folder did
    // not exist yet and the refusal cost a turn and a mkdir by hand.
    if (!fs::is_directory(destination.parent_path(), pathError)) {
        pathError.clear();
        if (!fs::create_directories(destination.parent_path(), pathError) ||
            pathError) {
            error = "destination parent is not accessible";
            return false;
        }
    }
    const fs::path staging =
        destination.parent_path() / (destination.filename().string() +
                                     ".cutmachine-" + GenerateUlid() + ".tmp");
    StagingDirectory cleanup(staging);
    if (!fs::create_directories(staging / "Timelines", pathError) ||
        pathError) {
        error = "unable to create project package: " + pathError.message();
        return false;
    }
    if (!CommitTextArtifacts({{(staging / "manifest.json").string(),
                               BuildManifest(project, "[]")}},
                             error))
        return false;

    std::map<std::string, EditLog> logs;
    for (const DocumentSequence& timeline : project.timelines)
        logs.emplace(timeline.id, EditLog{});
    const fs::path inner = staging / "project.cutmachine.json";
    if (!CommitStoredProjectAndLogs(inner.string(), project, logs,
                                    ProjectEditLog{}, error, nullptr, false)) {
        return false;
    }
    fs::rename(staging, destination, pathError);
    if (pathError) {
        error = "unable to publish project package: " + pathError.message();
        return false;
    }
    if (!SynchronizePath(destination.parent_path(), true, error)) return false;
    cleanup.Commit();
    projectPath = (destination / "project.cutmachine.json").string();
    DeliverProjectArchive(projectPath);
    error.clear();
    return true;
}

bool LoadStoredProject(const std::string& projectPath, Project& project,
                       std::string& error, bool deliverArchive) {
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    work_counters::Count(work_counters::Work::kPackageLoad);
    // PRD20 -- a wrong path is said to be a wrong path, before anything is
    // looked for beside it.
    if (IsMissingProjectPath(projectPath)) {
        error = MissingProjectMessage(projectPath);
        return false;
    }
    if (!RecoverTextArtifactTransaction(
            fs::path(projectPath).parent_path().string(), error))
        return false;
    if (!IsPortableProjectV2(projectPath)) {
        error = PackageRefusal(projectPath);
        return false;
    }
    Project loaded;
    if (!Project::Load(projectPath, loaded, error)) return false;
    const fs::path timelineDirectory =
        fs::path(projectPath).parent_path() / "Timelines";
    for (DocumentSequence& timeline : loaded.timelines) {
        const fs::path path = timelineDirectory / (timeline.id + ".json");
        std::string json;
        if (!ReadFile(path, json, error)) {
            error =
                "portable timeline is missing: " + timeline.id + ": " + error;
            return false;
        }
        Document document;
        if (!Document::LoadFromString(json, document, error, false)) {
            error = "invalid portable timeline '" + timeline.id + "': " + error;
            return false;
        }
        if (document.sequence.id != timeline.id) {
            error = "portable timeline ID does not match its filename: " +
                    timeline.id;
            return false;
        }
        timeline = std::move(document.sequence);
    }
    if (!loaded.Validate(error)) return false;
    project = std::move(loaded);
    if (deliverArchive) {
        DeliverProjectArchive(projectPath);
    }
    error.clear();
    return true;
}

bool CommitStoredProject(const std::string& projectPath, const Project& project,
                         std::string& error) {
    if (!RecoverTextArtifactTransaction(
            fs::path(projectPath).parent_path().string(), error))
        return false;
    std::map<std::string, EditLog> timelineLogs;
    if (!LoadTimelineLogs(projectPath, project, timelineLogs, error))
        return false;
    ProjectEditLog projectLog;
    const fs::path projectLogPath = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    if (fs::exists(projectLogPath, existsError)) {
        EditError editError = EditError::None;
        if (!ProjectEditLog::Load(projectLogPath.string(), projectLog,
                                  editError, error))
            return false;
    }
    if (existsError) {
        error = "unable to inspect project history: " + existsError.message();
        return false;
    }
    return CommitStoredProjectAndLogs(projectPath, project, timelineLogs,
                                      projectLog, error);
}

bool CommitStoredProjectAndLogs(
    const std::string& projectPath, const Project& project,
    const std::map<std::string, EditLog>& timelineLogs,
    const ProjectEditLog& projectLog, std::string& error,
    const Project* storedBefore, bool deliverArchive,
    const RunStepReceipt* runStep) {
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    if (!RecoverTextArtifactTransaction(
            fs::path(projectPath).parent_path().string(), error))
        return false;
    if (!IsPortableProjectV2(projectPath)) {
        error = PackageRefusal(projectPath);
        return false;
    }
    if (!project.Validate(error)) return false;
    (void)storedBefore;
    std::vector<std::pair<std::string, std::string>> artifacts;
    artifacts.push_back({projectPath, project.SaveToString()});
    const fs::path base = fs::path(projectPath).parent_path();
    std::string receiptJson;
    if (runStep != nullptr) {
        receiptJson = SerializeRunStepReceipt(*runStep);
        RunStepReceipt checked;
        const auto log = timelineLogs.find(runStep->timeline_id);
        if (!ParseRunStepReceipt(receiptJson, checked, error) ||
            log == timelineLogs.end() ||
            !RunStepMatchesState(checked, project, log->second, projectLog)) {
            error =
                "run receipt does not match the committed project and "
                "journals: " +
                error;
            return false;
        }
        const fs::path receiptPath =
            base / (".cutmachine-run-step-" + checked.step_id + ".json");
        std::error_code existsError;
        if (fs::exists(receiptPath, existsError) || existsError) {
            error = "run receipt already exists or cannot be inspected";
            return false;
        }
        artifacts.push_back({receiptPath.string(), receiptJson});
    }
    if (!project_archive::DefaultDatabasePath().empty()) {
        const Ulid snapshotId =
            runStep != nullptr ? runStep->snapshot_id : GenerateUlid();
        artifacts.push_back(
            {(base / (std::string(kArchivePrefix) + snapshotId + ".json"))
                 .string(),
             artifacts.front().second});
        if (runStep != nullptr) {
            artifacts.push_back({(base / (std::string(kArchivePrefix) +
                                          snapshotId + ".context.json"))
                                     .string(),
                                 receiptJson});
        }
    }
    std::string manifestJson;
    if (!ReadFile(base / "manifest.json", manifestJson, error)) return false;
    std::string mediaArray;
    if (!ExtractManifestArray(manifestJson, "media", mediaArray, error))
        return false;
    artifacts.push_back({(base / "manifest.json").string(),
                         BuildManifest(project, mediaArray)});
    const fs::path timelines = base / "Timelines";
    std::error_code directoryError;
    fs::create_directories(timelines, directoryError);
    if (directoryError) {
        error =
            "unable to create timeline directory: " + directoryError.message();
        return false;
    }
    const fs::file_status timelineStatus =
        fs::symlink_status(timelines, directoryError);
    if (directoryError || fs::is_symlink(timelineStatus) ||
        !fs::is_directory(timelineStatus)) {
        error = "timeline directory must be a package-local directory";
        return false;
    }
    for (const DocumentSequence& timeline : project.timelines) {
        artifacts.push_back(
            {(timelines / (timeline.id + ".json")).string(),
             StandaloneTimelineDocument(project, timeline).SaveToString()});
        const auto log = timelineLogs.find(timeline.id);
        const std::string logPath =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        if (log != timelineLogs.end()) {
            artifacts.push_back({logPath, log->second.Serialize()});
        } else {
            // B10 -- ROADMAP.md. An absent map entry means "not loaded", not
            // "replace this history with an empty one". Only a timeline that
            // has no journal yet (normally a newly added timeline) needs a
            // default artifact.
            std::error_code existsError;
            const bool logExists = fs::exists(logPath, existsError);
            if (existsError) {
                error = "unable to inspect timeline history: " +
                        existsError.message();
                return false;
            }
            if (!logExists)
                artifacts.push_back({logPath, EditLog{}.Serialize()});
        }
    }
    artifacts.push_back(
        {ProjectEditLogPathForProject(projectPath), projectLog.Serialize()});
    std::set<std::string> activeTimelineIds;
    for (const DocumentSequence& timeline : project.timelines)
        activeTimelineIds.insert(timeline.id);
    std::vector<std::string> removals;
    for (fs::directory_iterator item(timelines, directoryError), end;
         !directoryError && item != end; item.increment(directoryError)) {
        // A stale timeline is a file this code itself could have written, so
        // it is named <ULID>.json and nothing else. Requiring a valid ULID
        // rather than merely a .json extension keeps the transaction off
        // files it does not own -- notably the AppleDouble "._<name>"
        // sidecars macOS creates for every file on exFAT and FAT volumes,
        // which cannot be renamed on their own and made every save to a
        // project stored on such a drive fail.
        const std::string stem = item->path().stem().string();
        if (item->is_regular_file(directoryError) &&
            item->path().extension() == ".json" && IsValidUlid(stem) &&
            !activeTimelineIds.count(stem))
            removals.push_back(item->path().string());
        directoryError.clear();
    }
    if (directoryError) {
        error =
            "unable to inspect timeline directory: " + directoryError.message();
        return false;
    }
    const std::string logPrefix =
        fs::path(projectPath).filename().string() + ".timeline-";
    constexpr char kLogSuffix[] = ".editlog.json";
    for (fs::directory_iterator item(base, directoryError), end;
         !directoryError && item != end; item.increment(directoryError)) {
        const std::string filename = item->path().filename().string();
        if (item->is_regular_file(directoryError) &&
            filename.rfind(logPrefix, 0) == 0 &&
            filename.size() > logPrefix.size() + std::strlen(kLogSuffix) &&
            filename.compare(filename.size() - std::strlen(kLogSuffix),
                             std::strlen(kLogSuffix), kLogSuffix) == 0) {
            const std::string timelineId = filename.substr(
                logPrefix.size(),
                filename.size() - logPrefix.size() - std::strlen(kLogSuffix));
            if (!activeTimelineIds.count(timelineId))
                removals.push_back(item->path().string());
        }
        directoryError.clear();
    }
    if (directoryError) {
        error =
            "unable to inspect project histories: " + directoryError.message();
        return false;
    }
    // A failed transaction may have renamed part of the generation before it
    // stopped: counted too, so no holder keeps trusting what it parsed.
    const bool committed =
        CommitTextArtifactsAndRemove(artifacts, removals, error);
    gStoredProjectCommits.fetch_add(1, std::memory_order_release);
    if (!committed) {
        return false;
    }
    if (deliverArchive) {
        DeliverProjectArchive(projectPath);
    }
    return true;
}

ProjectSessionLock::ProjectSessionLock() = default;
ProjectSessionLock::~ProjectSessionLock() { Release(); }

bool AcquireStoredProjectLock(const std::string& projectPath,
                              ProjectSessionLock& lock, std::string& errorName,
                              std::string& message) {
    std::string token;
    std::string host;
    std::string owner;
    int64_t pid = 0;
    if (ReadLockOwner(fs::path(projectPath + ".lock"), token, host, pid,
                      owner) &&
        host == Hostname() && pid == getpid()) {
        return true;
    }
    if (lock.Acquire(projectPath, owner, message)) {
        return true;
    }
    errorName = owner.empty() ? "IoError" : "ProjectLocked";
    return false;
}

bool LoadProjectEditLogMigrating(const std::string& projectPath,
                                 ProjectEditLog& log,
                                 ProjectEditLogMigration& migration,
                                 std::string& error) {
    migration = ProjectEditLogMigration();
    log = ProjectEditLog();
    const fs::path logPath = ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    const bool exists = fs::exists(logPath, existsError);
    if (existsError) {
        error = "unable to inspect project history: " + existsError.message();
        return false;
    }
    if (!exists) {
        error.clear();
        return true;
    }
    migration.found = true;
    // Locked before the journal is read, so that no other writer can slip
    // an entry in between the read and the write back (OR2: under the I/O
    // mutex).
    std::lock_guard<std::recursive_mutex> guard(StoredProjectIoMutex());
    ProjectSessionLock lock;
    std::string lockErrorName;
    std::string lockMessage;
    const bool locked =
        AcquireStoredProjectLock(projectPath, lock, lockErrorName, lockMessage);
    // A transaction left half done may hold a newer journal: the editor
    // has recovered it already, an MCP session has not. Only under the lock.
    if (locked && !RecoverTextArtifactTransaction(
                      fs::path(projectPath).parent_path().string(), error)) {
        return false;
    }
    std::error_code sizeError;
    const std::uintmax_t size = fs::file_size(logPath, sizeError);
    migration.bytes_before = sizeError ? 0 : static_cast<uint64_t>(size);
    EditError editError = EditError::None;
    if (!ProjectEditLog::Load(logPath.string(), log, editError, error)) {
        return false;
    }
    migration.compacted = log.CompactedOnRead();
    error.clear();
    if (migration.compacted == 0) {
        return true;
    }
    if (!locked) {
        migration.rewrite_error =
            "the project journal was not migrated, the session lock is not "
            "this process's: " +
            lockMessage;
        return true;
    }
    const std::string compact = log.Serialize();
    std::string commitError;
    if (!CommitTextArtifacts({{logPath.string(), compact}}, commitError)) {
        migration.rewrite_error = commitError;
        return true;
    }
    migration.rewritten = true;
    migration.bytes_after = compact.size();
    return true;
}

bool MigrateProjectEditLog(const std::string& projectPath,
                           ProjectEditLogMigration& migration,
                           std::string& error) {
    migration = ProjectEditLogMigration();
    const fs::path logPath = ProjectEditLogPathForProject(projectPath);
    std::ifstream input(logPath, std::ios::binary);
    if (!input) {
        // No journal, or none this process can read: the migration has
        // nothing to say that the next project operation would not.
        error.clear();
        return true;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    // Only an entry that holds a whole project spells the key with a
    // string; every other one writes `"exact_project_hex":null` or a delta.
    if (contents.str().find("\"exact_project_hex\":\"") == std::string::npos) {
        migration.found = true;
        error.clear();
        return true;
    }
    ProjectEditLog ignored;
    return LoadProjectEditLogMigrating(projectPath, ignored, migration, error);
}

// SAVING_ROADMAP.md, forced-stop validation (24 September 2026). The lock
// is a directory whose `owner` file names its holder. It used to be created
// empty and filled afterwards: a process killed in between -- measured on
// the first run of kills on an exFAT volume, 19 edits in -- left a lock
// nobody could read, and every later opening, by the editor, the CLI or MCP,
// was refused until someone deleted a hidden directory inside the package.
// The metadata is now written into a staging directory of this process and
// published by rename(2), which is atomic and never replaces a lock that has
// an owner: a lock is visible complete, or not at all. rename(2) does replace
// an empty directory, so a lock left empty by an earlier binary heals on the
// next opening; one left with unreadable metadata is abandoned after
// kAbandonedLockAge.
bool ProjectSessionLock::Acquire(const std::string& projectPath,
                                 std::string& owner, std::string& error) {
    Release();
    owner.clear();
    error.clear();
    if (projectPath.empty()) {
        error = "project path must not be empty";
        return false;
    }
    const fs::path directory(projectPath + ".lock");
    RemoveAbandonedLockStaging(fs::path(projectPath));
    for (int attempt = 0; attempt < 2; ++attempt) {
        const Ulid token = GenerateUlid();
        const fs::path staging(projectPath + ".lock-" + token);
        std::error_code createError;
        if (!fs::create_directory(staging, createError)) {
            // PRD20 -- a lock that cannot be made beside a project that is
            // not there is refused for the project, not for the lock.
            error = IsMissingProjectPath(projectPath)
                        ? MissingProjectMessage(projectPath)
                        : "unable to create project lock: " +
                              (createError
                                   ? createError.message()
                                   : std::string("staging directory exists"));
            return false;
        }
        std::error_code ignored;
        if (!WriteLockOwner(staging, token)) {
            fs::remove_all(staging, ignored);
            error = "unable to write project lock metadata";
            return false;
        }
        std::error_code renameError;
        fs::rename(staging, directory, renameError);
        if (!renameError) {
            token_ = token;
            lock_directory_ = directory.string();
            return true;
        }
        fs::remove_all(staging, ignored);
        if (renameError != std::errc::directory_not_empty &&
            renameError != std::errc::file_exists) {
            error = "unable to create project lock: " + renameError.message();
            return false;
        }
        std::string existingToken;
        std::string host;
        int64_t pid = 0;
        const bool readable =
            ReadLockOwner(directory, existingToken, host, pid, owner);
        if (!readable && !OlderThan(directory, kAbandonedLockAge)) {
            error = "project lock exists but its owner metadata is invalid";
            return false;
        }
        if (!readable || OwnerIsGone(host, pid)) {
            std::error_code removeError;
            fs::remove_all(directory, removeError);
            if (removeError) {
                error = "unable to remove stale project lock: " +
                        removeError.message();
                return false;
            }
            owner.clear();
            continue;
        }
        error = "project is already open by " + owner;
        return false;
    }
    error = "unable to acquire project lock";
    return false;
}

void ProjectSessionLock::Release() {
    if (lock_directory_.empty() || token_.empty()) return;
    const fs::path directory(lock_directory_);
    std::string existingToken;
    std::string host;
    std::string owner;
    int64_t pid = 0;
    if (ReadLockOwner(directory, existingToken, host, pid, owner) &&
        existingToken == token_) {
        std::error_code ignored;
        fs::remove_all(directory, ignored);
    }
    lock_directory_.clear();
    token_.clear();
}

bool ProjectSessionLock::Held() const { return !lock_directory_.empty(); }

bool VerifyPortableProject(const std::string& projectPath,
                           CollectionIntegrityReport& report,
                           std::string& error) {
    report = {};
    const fs::path project(projectPath);
    if (!RecoverTextArtifactTransaction(project.parent_path().string(), error))
        return false;
    const fs::path manifestPath = project.parent_path() / "manifest.json";
    std::ifstream input(manifestPath, std::ios::binary);
    if (!input) {
        // "manifest is missing" describes our own bookkeeping, not what the
        // caller did wrong. The two ways to get here are worth telling
        // apart, because they need opposite things from the user.
        std::error_code projectError;
        if (!fs::exists(project, projectError) || projectError) {
            // PRD39 -- ROADMAP.md. The command line's sentence, for the same
            // path (PackageRefusal): the window said "no project file at
            // <path>" where the CLI says "no project at '<path>': nothing
            // exists at this path".
            error = PackageRefusal(projectPath);
        } else {
            error = project.filename().string() + " is not inside a " +
                    ".cutmachine-project package (no manifest.json beside "
                    "it). Single-file projects predate the package format "
                    "and cannot be opened directly; create a new project "
                    "and re-ingest, or point at the project.cutmachine.json "
                    "inside an existing package";
        }
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (!input.good() && !input.eof()) {
        error = "unable to read collection manifest";
        return false;
    }
    std::map<std::string, MediaFingerprint> fingerprints;
    if (!ReadMediaFingerprints(contents.str(), fingerprints, error))
        return false;
    Project loaded;
    if (!LoadStoredProject(projectPath, loaded, error)) return false;
    // FI13 -- the digest stored in each sequence is an integrity check too.
    // A damaged look is reported, not refused: refusing kept the whole
    // project from opening, and with it the re-import that repairs the LUT.
    for (const DocumentSequence& timeline : loaded.timelines) {
        LoadedLookLut look;
        std::string lookError;
        if (!LoadSequenceLookLut(timeline, loaded.settings.color_management,
                                 project.parent_path().string(), look,
                                 lookError)) {
            report.damaged_looks.push_back(timeline.name);
        }
    }
    for (const LibraryMedia& media : loaded.rushes) {
        // A regular project package deliberately leaves externally referenced
        // rushes out of the collection manifest. Only media copied by
        // CollectPortableProject has a fingerprint to verify; treating every
        // external rush as tampered makes all non-collected projects report a
        // false integrity failure on their second launch.
        const auto fingerprint = fingerprints.find(media.id);
        if (fingerprint == fingerprints.end()) continue;
        const fs::path path = project.parent_path() / media.path;
        std::error_code sizeError;
        const uint64_t actualBytes = fs::file_size(path, sizeError);
        if (sizeError) {
            report.missing_media.push_back(media.filename);
            continue;
        }
        if (actualBytes != fingerprint->second.bytes) {
            report.modified_media.push_back(media.filename);
            continue;
        }
        std::string actualSha;
        work_counters::Count(work_counters::Work::kMediaHash);
        if (!Sha256(path, actualSha, error)) return false;
        if (actualSha != fingerprint->second.sha256)
            report.modified_media.push_back(media.filename);
        else
            report.verified_media++;
    }
    error.clear();
    return true;
}

namespace {

// FI12 -- whether `copy` is `original` with the collection's rewrite
// applied, both through one writer so that only their content can differ.
bool IsCollectedForm(const std::string& original, const std::string& copy,
                     MotionAssetCollector& assets) {
    mcp_json::Value source;
    mcp_json::Value collected;
    std::string error;
    return mcp_json::Value::Parse(original, source, error) &&
           mcp_json::Value::Parse(copy, collected, error) &&
           assets.History(source).Dump() == collected.Dump();
}

// FI12 -- replays a journal and its rewrite side by side: from the current
// state back to the first entry, then forward to the last one undone. Each
// state of the copy must be the rewrite of the source's, and each step must
// apply on both: what undo and redo will do in the copy, checked before it
// is published.
template <typename State, typename Log, typename Canonical>
bool ReplaysAsCollected(State source, State copy, Log sourceLog, Log copyLog,
                        Canonical canonical, MotionAssetCollector& assets) {
    const auto same = [&] {
        return IsCollectedForm(canonical(source), canonical(copy), assets);
    };
    const size_t applied = sourceLog.AppliedCount();
    const size_t undone = sourceLog.UndoneCount();
    if (copyLog.AppliedCount() != applied || copyLog.UndoneCount() != undone ||
        !same()) {
        return false;
    }
    EditError error = EditError::None;
    std::string message;
    for (size_t step = 0; step < applied; ++step) {
        if (!sourceLog.Undo(source, error, message) ||
            !copyLog.Undo(copy, error, message) || !same()) {
            return false;
        }
    }
    for (size_t step = 0; step < applied + undone; ++step) {
        if (!sourceLog.Redo(source, error, message) ||
            !copyLog.Redo(copy, error, message) || !same()) {
            return false;
        }
    }
    return true;
}

}  // namespace

bool CollectPortableProject(const std::string& sourceProjectPath,
                            const std::string& destinationDirectory,
                            PortableProjectResult& result, std::string& error) {
    result = {};
    if (sourceProjectPath.empty() || destinationDirectory.empty()) {
        error = "source project and destination must not be empty";
        return false;
    }

    std::error_code pathError;
    const fs::path sourceProject =
        fs::weakly_canonical(sourceProjectPath, pathError);
    if (pathError || !fs::is_regular_file(sourceProject, pathError)) {
        error = "source project is not a readable regular file";
        return false;
    }
    const fs::path destination = fs::absolute(destinationDirectory);
    if (fs::exists(destination, pathError) || pathError) {
        error = pathError
                    ? "unable to inspect destination: " + pathError.message()
                    : "destination already exists";
        return false;
    }
    const fs::path parent = destination.parent_path();
    if (!fs::is_directory(parent, pathError)) {
        pathError.clear();
        if (!fs::create_directories(parent, pathError) || pathError) {
            error = "destination parent is not accessible";
            return false;
        }
    }

    Project collected;
    if (!LoadStoredProject(sourceProject.string(), collected, error))
        return false;
    std::map<std::string, EditLog> timelineLogs;
    if (!LoadTimelineLogs(sourceProject.string(), collected, timelineLogs,
                          error))
        return false;
    ProjectEditLog projectLog;
    const fs::path sourceProjectLog =
        ProjectEditLogPathForProject(sourceProject.string());
    pathError.clear();
    if (fs::exists(sourceProjectLog, pathError)) {
        EditError editError = EditError::None;
        if (!ProjectEditLog::Load(sourceProjectLog.string(), projectLog,
                                  editError, error))
            return false;
    }
    if (pathError) {
        error = "unable to inspect project history: " + pathError.message();
        return false;
    }

    const fs::path staging =
        parent / (destination.filename().string() + ".cutmachine-" +
                  GenerateUlid() + ".tmp");
    StagingDirectory cleanup(staging);
    const fs::path mediaDirectory = staging / "Media";
    if (!fs::create_directories(mediaDirectory, pathError) || pathError) {
        error = "unable to create collection staging directory: " +
                pathError.message();
        return false;
    }

    struct ManifestMedia {
        std::string id;
        std::string path;
        uint64_t bytes = 0;
        std::string sha256;
    };
    std::vector<ManifestMedia> manifest;
    const fs::path sourceBase = sourceProject.parent_path();
    // FI12 -- the state the copied journals were written against, and each
    // rush's package path by the paths its records had.
    const Project storedProject = collected;
    std::map<std::pair<std::string, std::string>, std::string> rushPaths;
    for (LibraryMedia& media : collected.rushes) {
        pathError.clear();
        fs::path original(media.path);
        if (original.is_relative()) original = sourceBase / original;
        original = fs::weakly_canonical(original, pathError);
        if (pathError || !fs::is_regular_file(original, pathError)) {
            error = "original media is missing or unreadable: " + media.path;
            return false;
        }
        std::string filename = original.filename().string();
        if (filename.empty()) filename = "media";
        const fs::path relative =
            fs::path("Media") / (media.id + "-" + filename);
        const fs::path copied = staging / relative;
        fs::copy_file(original, copied, fs::copy_options::none, pathError);
        if (pathError) {
            error = "unable to copy media '" + original.string() +
                    "': " + pathError.message();
            return false;
        }
        if (!SynchronizePath(copied, false, error)) return false;
        const uint64_t bytes = fs::file_size(copied, pathError);
        if (pathError) {
            error = "unable to inspect copied media: " + pathError.message();
            return false;
        }
        std::string digest;
        if (!Sha256(copied, digest, error)) return false;
        rushPaths[{media.id, media.path}] = relative.generic_string();
        media.path = relative.generic_string();
        media.proxy_path.clear();
        if (DocumentSource* source = [&]() -> DocumentSource* {
                for (DocumentSource& candidate : collected.sources)
                    if (candidate.id == media.id) return &candidate;
                return nullptr;
            }()) {
            rushPaths[{source->id, source->path}] = media.path;
            source->path = media.path;
        }
        manifest.push_back({media.id, media.path, bytes, std::move(digest)});
        result.media_count++;
        result.media_bytes += bytes;
    }
    if (!SynchronizePath(mediaDirectory, true, error)) return false;

    MotionAssetCollector assets(sourceBase, staging);
    if (!assets.Collect(collected, error)) {
        return false;
    }

    // FI12, undo in a collected copy -- docs/roadmap/FIABILITE-2026-10.md.
    // Each journal is rewritten like the state, then proven by replaying it
    // beside the source's; one whose rewrite does not replay exactly is
    // copied as it was, as every journal used to be, and the result says
    // which.
    {
        std::map<std::string, mcp_json::Value> timelineTrees;
        std::string parseError;
        for (const auto& [id, log] : timelineLogs) {
            mcp_json::Value tree;
            if (mcp_json::Value::Parse(log.Serialize(), tree, parseError)) {
                timelineTrees.emplace(id, std::move(tree));
            }
        }
        mcp_json::Value projectTree;
        const bool projectParsed = mcp_json::Value::Parse(
            projectLog.Serialize(), projectTree, parseError);
        assets.SetRushPaths(std::move(rushPaths));
        assets.LearnClips(storedProject);
        for (const auto& tree : timelineTrees) assets.LearnClips(tree.second);
        if (projectParsed) assets.LearnClips(projectTree);
        for (const auto& tree : timelineTrees) {
            assets.DiscoverHistory(tree.second);
        }
        if (projectParsed) assets.DiscoverHistory(projectTree);

        EditError editError = EditError::None;
        std::string message;
        for (auto& [id, log] : timelineLogs) {
            if (log.AppliedCount() + log.UndoneCount() == 0) continue;
            const auto tree = timelineTrees.find(id);
            EditLog rewritten;
            const bool proven =
                tree != timelineTrees.end() &&
                EditLog::Deserialize(assets.History(tree->second).Dump(),
                                     rewritten, editError, message) &&
                ReplaysAsCollected(
                    storedProject.MakeDocument(id), collected.MakeDocument(id),
                    log, rewritten,
                    [](const Document& state) { return state.SaveToString(); },
                    assets);
            if (proven) {
                log = std::move(rewritten);
            } else {
                result.verbatim_histories.push_back(id);
            }
        }
        if (projectLog.AppliedCount() + projectLog.UndoneCount() != 0) {
            ProjectEditLog rewritten;
            const bool proven =
                projectParsed &&
                ProjectEditLog::Deserialize(assets.History(projectTree).Dump(),
                                            rewritten, editError, message) &&
                ReplaysAsCollected(
                    storedProject, collected, projectLog, rewritten,
                    [](const Project& state) { return state.SaveToString(); },
                    assets);
            if (proven) {
                projectLog = std::move(rewritten);
            } else {
                result.verbatim_histories.push_back("project");
            }
        }
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. A timeline's look LUT travels with
    // the package, like its media: without it the collected project refuses
    // to render rather than rendering the film ungraded, which is the whole
    // point of storing the digest. Content-addressed, so two timelines that
    // share a look share one file and a second copy is a no-op.
    for (const DocumentSequence& timeline : collected.timelines) {
        if (timeline.look_lut.digest.empty()) continue;
        LoadedLookLut look;
        if (!LoadSequenceLookLut(timeline, collected.settings.color_management,
                                 sourceBase.string(), look, error)) {
            return false;
        }
        const fs::path relative(LookLutPackagePath(timeline.look_lut.digest));
        const fs::path original = sourceBase / relative;
        pathError.clear();
        if (!fs::is_regular_file(original, pathError) || pathError) {
            error = "the look LUT of timeline '" + timeline.name +
                    "' is missing from the project package: " +
                    relative.generic_string();
            return false;
        }
        const fs::path copied = staging / relative;
        pathError.clear();
        if (fs::exists(copied, pathError)) continue;
        if (!fs::create_directories(copied.parent_path(), pathError) &&
            pathError) {
            error = "unable to create the collected LUT directory: " +
                    pathError.message();
            return false;
        }
        pathError.clear();
        fs::copy_file(original, copied, fs::copy_options::none, pathError);
        if (pathError) {
            error = "unable to copy the look LUT '" + original.string() +
                    "': " + pathError.message();
            return false;
        }
        if (!SynchronizePath(copied, false, error)) return false;
        if (!SynchronizePath(copied.parent_path(), true, error)) return false;
    }
    // Verify the copied bytes as well: the source may change while copying.
    for (const DocumentSequence& timeline : collected.timelines) {
        LoadedLookLut look;
        if (!LoadSequenceLookLut(timeline, collected.settings.color_management,
                                 staging.string(), look, error)) {
            return false;
        }
    }

    std::ofstream manifestOutput(staging / "manifest.json",
                                 std::ios::binary | std::ios::trunc);
    if (!manifestOutput) {
        error = "unable to create collection manifest";
        return false;
    }
    manifestOutput
        << "{\n  \"format\":\"cutmachine-collection\",\n"
        << "  \"version\":2,\n  \"project\":\"project.cutmachine.json\",\n"
        << "  \"timelines\":[";
    for (size_t index = 0; index < collected.timelines.size(); ++index) {
        const DocumentSequence& timeline = collected.timelines[index];
        manifestOutput << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                       << EscapeJson(timeline.id) << "\",\"path\":\"Timelines/"
                       << EscapeJson(timeline.id) << ".json\"}";
    }
    if (!collected.timelines.empty()) manifestOutput << '\n';
    manifestOutput << "  ],\n"
                   << "  \"media\":[";
    for (size_t index = 0; index < manifest.size(); ++index) {
        const ManifestMedia& item = manifest[index];
        manifestOutput << (index == 0 ? "\n" : ",\n") << "    {\"id\":\""
                       << EscapeJson(item.id) << "\",\"path\":\""
                       << EscapeJson(item.path) << "\",\"bytes\":" << item.bytes
                       << ",\"sha256\":\"" << item.sha256 << "\"}";
    }
    if (!manifest.empty()) manifestOutput << '\n';
    manifestOutput << "  ]\n}\n";
    manifestOutput.close();
    if (!manifestOutput) {
        error = "unable to write collection manifest";
        return false;
    }

    const fs::path collectedProject = staging / "project.cutmachine.json";
    if (!CommitStoredProjectAndLogs(collectedProject.string(), collected,
                                    timelineLogs, projectLog, error, nullptr,
                                    false)) {
        return false;
    }

    Project verified;
    if (!LoadStoredProject(collectedProject.string(), verified, error, false)) {
        error = "collected project failed validation: " + error;
        return false;
    }
    for (const LibraryMedia& media : verified.rushes) {
        const fs::path relative(media.path);
        pathError.clear();
        if (relative.is_absolute() || HasParentTraversal(relative) ||
            !fs::is_regular_file(staging / relative, pathError) || pathError) {
            error =
                "collected project contains an unsafe or missing media path";
            return false;
        }
    }

    fs::rename(staging, destination, pathError);
    if (pathError) {
        error = "unable to publish collected project: " + pathError.message();
        return false;
    }
    if (!SynchronizePath(parent, true, error)) return false;
    cleanup.Commit();
    result.directory_path = destination.string();
    result.project_path = (destination / "project.cutmachine.json").string();
    DeliverProjectArchive(result.project_path);
    error.clear();
    return true;
}
