#include "McpMotionTools.h"

#include "AfterBridge.h"
#include "Document.h"
#include "FontCatalog.h"
#include "FrameCapture.h"
#include "McpMotionClipTools.h"
#include "MotionAfterExport.h"
#include "MotionAfterImport.h"
#include "MotionAfterVerify.h"
#include "MotionCanvas.h"
#include "MotionDocument.h"
#include "MotionFrameRenderer.h"
#include "MotionHumanize.h"
#include "MotionLibraryBase.h"
#include "MotionTemplates.h"
#include "MotionVideo.h"
#include "Operations.h"
#include "Project.h"
#include "Ulid.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <utility>
#include <vector>

namespace {

using mcp_json::Value;

bool Refuse(std::string& errorName, std::string& message,
            const std::string& name, std::string text) {
    errorName = name;
    message = std::move(text);
    return false;
}

bool Invalid(std::string& errorName, std::string& message, std::string text) {
    return Refuse(errorName, message, "ValidationFailed", std::move(text));
}

std::string Quoted(const std::string& text) {
    return "\"" + mcp_json::EscapeJsonString(text) + "\"";
}

// Schemas, as the rest of the catalog spells them (McpTools.cc).
std::string Described(const std::string& type, const std::string& text) {
    return "{\"type\":\"" + type + "\",\"description\":" + Quoted(text) + "}";
}

std::string Schema(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const std::vector<std::string>& required, const std::string& description) {
    std::string json =
        "{\"type\":\"object\",\"description\":" + Quoted(description) +
        ",\"properties\":{";
    for (size_t index = 0; index < properties.size(); ++index) {
        if (index != 0) {
            json += ",";
        }
        json +=
            Quoted(properties[index].first) + ":" + properties[index].second;
    }
    json += "}";
    // PRD15 -- an empty `required` is left out, as McpTools.cc does.
    for (size_t index = 0; index < required.size(); ++index) {
        json += index != 0 ? "," : ",\"required\":[";
        json += Quoted(required[index]);
    }
    if (!required.empty()) {
        json += "]";
    }
    return json + ",\"additionalProperties\":false}";
}

bool Known(const Value& args, const std::vector<std::string>& allowed,
           const std::string& path, bool timelineScoped, std::string& message) {
    for (const auto& entry : args.AsObject()) {
        if (timelineScoped && entry.first == "timeline_id") {
            continue;
        }
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            message =
                "'" + path + "' has unknown argument '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool ReadText(const Value& args, const std::string& key,
              const std::string& path, bool required, std::string& out,
              std::string& message) {
    const Value* value = args.Find(key);
    if (value == nullptr) {
        if (!required) {
            return true;
        }
        message = "'" + path + "." + key + "' is required";
        return false;
    }
    if (!value->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    out = value->AsString();
    return true;
}

bool ReadFlag(const Value& args, const std::string& key,
              const std::string& path, bool fallback, bool& out,
              std::string& message) {
    const Value* value = args.Find(key);
    if (value == nullptr) {
        out = fallback;
        return true;
    }
    if (!value->IsBool()) {
        message = "'" + path + "." + key + "' must be a boolean";
        return false;
    }
    out = value->AsBool();
    return true;
}

// A composition by full id or by an unambiguous prefix of one, like every
// ID the catalog takes.
bool ResolveComposition(const Document& document, const std::string& input,
                        const DocumentComposition*& composition,
                        std::string& errorName, std::string& message) {
    const DocumentComposition* found = nullptr;
    size_t matches = 0;
    for (const DocumentComposition& candidate : document.compositions) {
        if (candidate.id == input) {
            composition = &candidate;
            return true;
        }
        if (!input.empty() &&
            candidate.id.compare(0, input.size(), input) == 0) {
            found = &candidate;
            ++matches;
        }
    }
    if (matches == 1) {
        composition = found;
        return true;
    }
    return Refuse(errorName, message,
                  matches > 1 ? "ValidationFailed" : "UnknownComposition",
                  matches > 1 ? "composition_id '" + input +
                                    "' matches several compositions"
                              : "unknown composition_id '" + input + "'");
}

// A node of `composition` by uid, by the author's `id` for it, or by an
// unambiguous uid prefix: an agent names a node the way it reads it.
bool ResolveNode(const DocumentComposition& composition,
                 const std::string& input, const std::string& path, Ulid& uid,
                 std::string& errorName, std::string& message) {
    MotionNodeLocation location;
    if (FindMotionNode(composition.definition, input, location)) {
        uid = input;
        return true;
    }
    if (FindMotionNodeByName(composition.definition, input, location)) {
        const Value* found =
            MotionTreeAt(composition.definition, location.path)->Find("uid");
        if (found != nullptr && found->IsString()) {
            uid = found->AsString();
            return true;
        }
    }
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(composition.definition, nodes, keyframes);
    std::vector<Ulid> matches;
    for (const Ulid& candidate : nodes) {
        if (!input.empty() && candidate.compare(0, input.size(), input) == 0) {
            matches.push_back(candidate);
        }
    }
    if (matches.size() == 1) {
        uid = matches.front();
        return true;
    }
    return Refuse(
        errorName, message,
        matches.empty() ? "UnknownMotionNode" : "ValidationFailed",
        "'" + path + "': " +
            (matches.empty() ? "composition '" + composition.id +
                                   "' has no node '" + input + "'"
                             : "'" + input + "' matches several nodes"));
}

bool ReadNodeReference(const DocumentComposition& composition,
                       const Value& edit, const std::string& key,
                       const std::string& path, bool required, Ulid& uid,
                       std::string& errorName, std::string& message) {
    std::string reference;
    if (!ReadText(edit, key, path, required, reference, message)) {
        return Invalid(errorName, message, message);
    }
    if (reference.empty()) {
        return !required || Invalid(errorName, message,
                                    "'" + path + "." + key + "' is empty");
    }
    return ResolveNode(composition, reference, path + "." + key, uid, errorName,
                       message);
}

// MOTION-DESIGN-2026-09 -- a definition read from a JSON file instead of
// the call: a motion design film is compositions of a hundred kilobytes,
// written by a script, and an agent that pastes them into every call pays
// for them twice. Absolute paths only.
bool ReadDefinitionFile(const Value& args, const std::string& path,
                        std::optional<Value>& definition,
                        std::string& message) {
    const Value* file = args.Find("definition_file");
    if (file == nullptr) {
        return true;
    }
    if (!file->IsString() || file->AsString().empty() ||
        file->AsString().front() != '/') {
        message = "'" + path + ".definition_file' must be an absolute path";
        return false;
    }
    std::ifstream input(file->AsString(), std::ios::binary);
    const std::string text{std::istreambuf_iterator<char>(input),
                           std::istreambuf_iterator<char>()};
    Value parsed;
    std::string error;
    if (!input.good() && text.empty()) {
        message = "cannot read '" + file->AsString() + "'";
        return false;
    }
    if (!Value::Parse(text, parsed, error) || !parsed.IsObject()) {
        message =
            "'" + file->AsString() + "' is not a composition object: " + error;
        return false;
    }
    definition = std::move(parsed);
    return true;
}

// MOTION-DESIGN-2026-09 -- what a call made from a file answers: the
// composition and the revision, not the thousand identities it created.
void CompactResult(const Ulid& id, std::string& resultJson) {
    Value result;
    std::string error;
    if (!Value::Parse(resultJson, result, error) || !result.IsObject()) {
        return;
    }
    Value compact = Value::MakeObject();
    for (const char* key : {"revision", "ok", "doc_hash"}) {
        if (const Value* value = result.Find(key)) {
            compact.Set(key, *value);
        }
    }
    compact.Set("composition_id", Value::MakeString(id));
    if (const Value* delta = result.Find("delta")) {
        if (const Value* nodes = delta->Find("created_motion_node_ids")) {
            compact.Set(
                "created_nodes",
                Value::MakeInt(static_cast<int64_t>(nodes->AsArray().size())));
        }
    }
    resultJson = compact.Dump();
}

// One entry of edit_motion_composition's `edits`, as the operation it asks
// for, resolved against the composition as the edits before it left it.
bool BuildEdit(const DocumentComposition& composition, const Value& edit,
               const std::string& path, Operation& operation,
               std::string& errorName, std::string& message) {
    if (!edit.IsObject()) {
        return Invalid(errorName, message, "'" + path + "' must be an object");
    }
    std::string op;
    if (!ReadText(edit, "op", path, true, op, message)) {
        return Invalid(errorName, message, message);
    }
    const std::string where = path + " (" + op + ")";
    if (op == "replace") {
        if (!Known(edit, {"op", "name", "definition", "definition_file"}, where,
                   false, message)) {
            return Invalid(errorName, message, message);
        }
        DocumentComposition replaced = composition;
        if (!ReadText(edit, "name", where, false, replaced.name, message)) {
            return Invalid(errorName, message, message);
        }
        if (const Value* definition = edit.Find("definition")) {
            replaced.definition = *definition;
        }
        std::optional<Value> fromFile;
        if (!ReadDefinitionFile(edit, where, fromFile, message)) {
            return Invalid(errorName, message, message);
        }
        if (fromFile) {
            replaced.definition = std::move(*fromFile);
        }
        operation = UpdateMotionCompositionOperation{replaced};
        return true;
    }
    if (op == "set_node") {
        if (!Known(edit, {"op", "node", "parent", "before"}, where, false,
                   message)) {
            return Invalid(errorName, message, message);
        }
        SetMotionNodeOperation set;
        set.composition_id = composition.id;
        const Value* node = edit.Find("node");
        if (node == nullptr || !node->IsObject()) {
            return Invalid(errorName, message,
                           "'" + where + ".node' must be a node object");
        }
        set.node = *node;
        if (!ReadNodeReference(composition, edit, "parent", where, false,
                               set.parent_uid, errorName, message) ||
            !ReadNodeReference(composition, edit, "before", where, false,
                               set.before_uid, errorName, message)) {
            return false;
        }
        operation = std::move(set);
        return true;
    }
    if (op == "remove_node") {
        if (!Known(edit, {"op", "node"}, where, false, message)) {
            return Invalid(errorName, message, message);
        }
        RemoveMotionNodeOperation remove;
        remove.composition_id = composition.id;
        if (!ReadNodeReference(composition, edit, "node", where, true,
                               remove.node_uid, errorName, message)) {
            return false;
        }
        operation = std::move(remove);
        return true;
    }
    if (op == "set_style" || op == "set_animation") {
        const bool style = op == "set_style";
        const std::string valueKey = style ? "value" : "animation";
        if (!Known(edit, {"op", "node", "property", valueKey}, where, false,
                   message)) {
            return Invalid(errorName, message, message);
        }
        Ulid node;
        std::string property;
        if (!ReadNodeReference(composition, edit, "node", where, true, node,
                               errorName, message)) {
            return false;
        }
        if (!ReadText(edit, "property", where, true, property, message)) {
            return Invalid(errorName, message, message);
        }
        std::optional<Value> value;
        if (const Value* given = edit.Find(valueKey);
            given != nullptr && !given->IsNull()) {
            value = *given;
        }
        if (style) {
            operation =
                SetMotionStyleOperation{composition.id, node, property, value};
        } else {
            operation = SetMotionAnimationOperation{composition.id, node,
                                                    property, value};
        }
        return true;
    }
    if (op == "set_keyframe") {
        if (!Known(edit, {"op", "keyframe", "frame", "value"}, where, false,
                   message)) {
            return Invalid(errorName, message, message);
        }
        SetMotionKeyframeOperation set;
        set.composition_id = composition.id;
        const Value* frame = edit.Find("frame");
        const Value* value = edit.Find("value");
        if (!ReadText(edit, "keyframe", where, true, set.keyframe_uid,
                      message) ||
            frame == nullptr || !frame->AsInt64(set.frame) ||
            value == nullptr) {
            return Invalid(errorName, message,
                           "'" + where +
                               "' takes keyframe (its uid), frame (an "
                               "integer) and value");
        }
        set.value = *value;
        operation = std::move(set);
        return true;
    }
    if (op == "key" || op == "unkey" || op == "ease") {
        // REMOTION-PARITY-2026-09 REM5 -- keys by frame, as the visual
        // editor sets them (MotionCanvas.h): a key at the frame shown is
        // moved or added, removed, or every segment's easing changed -- or,
        // MOTION-BIBLIOTHEQUE-2026-10 MB5, the one segment leaving the key
        // at `frame`. Each is the animation rewritten, one set_animation.
        const std::vector<std::string> allowed =
            op == "key" ? std::vector<std::string>{"op", "node", "property",
                                                   "frame", "value"}
            : op == "unkey"
                ? std::vector<std::string>{"op", "node", "property", "frame"}
                : std::vector<std::string>{"op", "node", "property", "easing",
                                           "frame"};
        if (!Known(edit, allowed, where, false, message)) {
            return Invalid(errorName, message, message);
        }
        Ulid node;
        std::string property;
        if (!ReadNodeReference(composition, edit, "node", where, true, node,
                               errorName, message)) {
            return false;
        }
        if (!ReadText(edit, "property", where, true, property, message)) {
            return Invalid(errorName, message, message);
        }
        MotionNodeLocation location;
        FindMotionNode(composition.definition, node, location);
        const Value* stored =
            MotionTreeAt(composition.definition, location.path);
        const Value* animate =
            stored != nullptr ? stored->Find("animate") : nullptr;
        const Value* animation =
            animate != nullptr ? animate->Find(property) : nullptr;
        std::string error;
        std::optional<Value> updated;
        if (op == "ease") {
            std::string easing;
            Value eased;
            if (!ReadText(edit, "easing", where, true, easing, message)) {
                return Invalid(errorName, message, message);
            }
            std::optional<int64_t> segment;
            if (const Value* at = edit.Find("frame");
                at != nullptr && !at->IsNull()) {
                int64_t frame = 0;
                if (!at->AsInt64(frame)) {
                    return Invalid(errorName, message,
                                   "'" + where +
                                       ".frame' must be the integer frame of "
                                       "the key the segment leaves");
                }
                segment = frame;
            }
            if (animation == nullptr ||
                !MotionAnimationWithEasing(*animation, easing, eased, error,
                                           segment)) {
                return Refuse(
                    errorName, message, "InvalidOperation",
                    "'" + where + "': " +
                        (animation == nullptr ? property + " is not animated"
                                              : error));
            }
            updated = std::move(eased);
        } else {
            int64_t frame = 0;
            const Value* at = edit.Find("frame");
            if (at == nullptr || !at->AsInt64(frame)) {
                return Invalid(errorName, message,
                               "'" + where +
                                   ".frame' must be an integer "
                                   "frame of the composition");
            }
            if (op == "key") {
                const Value* value = edit.Find("value");
                Value keyed;
                if (value == nullptr) {
                    return Invalid(errorName, message,
                                   "'" + where + "' needs the key's value");
                }
                if (!MotionAnimationWithKey(animation, frame, *value, keyed,
                                            error)) {
                    return Refuse(errorName, message, "InvalidOperation",
                                  "'" + where + "': " + error);
                }
                updated = std::move(keyed);
            } else if (animation == nullptr ||
                       !MotionAnimationWithoutKey(*animation, frame, updated,
                                                  error)) {
                return Refuse(
                    errorName, message, "InvalidOperation",
                    "'" + where + "': " +
                        (animation == nullptr ? property + " is not animated"
                                              : error));
            }
        }
        operation = SetMotionAnimationOperation{composition.id, node, property,
                                                updated};
        return true;
    }
    if (op == "set_props") {
        if (!Known(edit, {"op", "props"}, where, false, message)) {
            return Invalid(errorName, message, message);
        }
        const Value* props = edit.Find("props");
        if (props == nullptr || !props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + where + ".props' must be an object");
        }
        operation = SetMotionPropsOperation{composition.id, *props};
        return true;
    }
    if (op == "set_variant" || op == "set_format") {
        // M6 -- a version or a format rewrites the definition, and the
        // composition is replaced whole: the parser judges the new one,
        // with the prop refusals of M3, and the journal needs no new
        // operation.
        const bool variant = op == "set_variant";
        const std::string key = variant ? "variant" : "format";
        const std::vector<std::string> allowed =
            variant ? std::vector<std::string>{"op", "variant", "props"}
                    : std::vector<std::string>{"op", "format", "width",
                                               "height", "props"};
        std::string name;
        if (!Known(edit, allowed, where, false, message) ||
            !ReadText(edit, key, where, true, name, message)) {
            return Invalid(errorName, message, message);
        }
        const Value* props = edit.Find("props");
        if (props != nullptr && props->IsNull()) {
            props = nullptr;
        }
        if (props != nullptr && !props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + where + ".props' must be an object");
        }
        std::optional<Value> entry;
        if (variant) {
            if (props != nullptr) {
                entry = Value::MakeObject();
                entry->Set("props", *props);
            }
        } else {
            const Value* width = edit.Find("width");
            const Value* height = edit.Find("height");
            if ((width == nullptr) != (height == nullptr) ||
                (width == nullptr && props != nullptr)) {
                return Invalid(errorName, message,
                               "'" + where +
                                   "' takes width and height (and props) to "
                                   "set a format, or neither to remove it");
            }
            if (width != nullptr) {
                entry = Value::MakeObject();
                entry->Set("width", *width);
                entry->Set("height", *height);
                if (props != nullptr) {
                    entry->Set("props", *props);
                }
            }
        }
        DocumentComposition replaced = composition;
        if (!MotionDefinitionWithSetEntry(composition.definition,
                                          variant ? "variants" : "formats",
                                          name, entry, replaced.definition)) {
            return Refuse(errorName, message,
                          variant ? "UnknownVariant" : "UnknownFormat",
                          "'" + where + "': composition '" + composition.id +
                              "' has no " + key + " '" + name + "' to remove");
        }
        operation = UpdateMotionCompositionOperation{replaced};
        return true;
    }
    return Invalid(errorName, message,
                   "'" + path +
                       ".op' must be replace, set_node, remove_node, "
                       "set_style, set_animation, set_keyframe, set_props, "
                       "set_variant or set_format");
}

bool Snapshot(McpBackend& backend, Document& document, std::string& errorName,
              std::string& message) {
    if (backend.SnapshotDocument(document, message)) {
        return true;
    }
    errorName = "IoError";
    return false;
}

bool DispatchList(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    if (!Known(args, {}, "list_motion_compositions", false, message)) {
        return Invalid(errorName, message, message);
    }
    Document document;
    if (!Snapshot(backend, document, errorName, message)) {
        return false;
    }
    resultJson = MotionCompositionListJson(document.compositions);
    return true;
}

bool DispatchDescribe(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    const std::string path = "describe_motion_composition";
    std::string id;
    if (!Known(args, {"composition_id"}, path, false, message) ||
        !ReadText(args, "composition_id", path, true, id, message)) {
        return Invalid(errorName, message, message);
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    resultJson = MotionCompositionJson(*composition);
    return true;
}

// The applied result, and the composition as it is now stored: an agent
// that added one reads the uids it was given without another call.
bool WithComposition(McpBackend& backend, const Ulid& id,
                     std::string& resultJson, std::string& errorName,
                     std::string& message) {
    Document document;
    if (!Snapshot(backend, document, errorName, message)) {
        return false;
    }
    const DocumentComposition* composition = document.FindComposition(id);
    Value result;
    std::string error;
    if (composition == nullptr || !Value::Parse(resultJson, result, error) ||
        !result.IsObject()) {
        return true;
    }
    Value stored;
    Value::Parse(MotionCompositionJson(*composition), stored, error);
    result.Set("composition", std::move(stored));
    resultJson = result.Dump();
    return true;
}

bool DispatchAdd(McpBackend& backend, const IdResolver&, const Value& args,
                 std::string& resultJson, std::string& errorName,
                 std::string& message) {
    const std::string path = "add_motion_composition";
    AddMotionCompositionOperation op;
    if (!Known(args, {"name", "definition", "definition_file"}, path, true,
               message) ||
        !ReadText(args, "name", path, true, op.composition.name, message)) {
        return Invalid(errorName, message, message);
    }
    std::optional<Value> fromFile;
    if (!ReadDefinitionFile(args, path, fromFile, message)) {
        return Invalid(errorName, message, message);
    }
    const Value* definition = fromFile ? &*fromFile : args.Find("definition");
    if (definition == nullptr || !definition->IsObject()) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".definition' (or definition_file) must be a "
                           "composition object");
    }
    op.composition.definition = *definition;
    const Ulid id = op.composition.id;
    if (!backend.ApplyOperation(op, resultJson, errorName, message)) {
        return false;
    }
    if (fromFile) {
        CompactResult(id, resultJson);
        return true;
    }
    return WithComposition(backend, id, resultJson, errorName, message);
}

// M5 -- docs/roadmap/MOTION-2026-09.md. The templates the binary ships
// (assets/motion-templates): each one's props with their types, bounds and
// defaults, and the sizes it has a layout for.
//
// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. And the pieces of this
// machine's library, after them: one catalog, two shelves. A piece of the
// library that cannot be read is reported beside the catalog, never dropped
// in silence and never a reason to hide the others.
//
// MB10 -- the library is the user's personal base now, its folder an inbox
// read into it first (MotionLibraryBase.h).
struct MotionShelves {
    std::vector<MotionTemplateInfo> pieces;
    // How many of `pieces`, from the front, the binary ships.
    size_t shipped = 0;
    MotionLibraryLocation location;
    std::vector<MotionLibraryIssue> issues;
    // MB10 -- the inbox's files this read put in the base.
    std::vector<MotionLibraryIssue> imported;
    // MB4 -- the charters of this machine's library.
    std::vector<MotionKitInfo> kits;
};

bool ReadShelves(MotionShelves& shelves, std::string& error) {
    if (!MotionTemplateCatalog(shelves.pieces, error)) {
        return false;
    }
    shelves.shipped = shelves.pieces.size();
    shelves.location = CurrentMotionLibraryLocation();
    MotionLibraryContents library;
    ReadMotionLibraryBase(shelves.location, shelves.pieces, library);
    std::move(library.pieces.begin(), library.pieces.end(),
              std::back_inserter(shelves.pieces));
    shelves.kits = std::move(library.kits);
    shelves.issues = std::move(library.issues);
    shelves.imported = std::move(library.imported);
    return true;
}

Value IssuesJson(const std::vector<MotionLibraryIssue>& list) {
    Value issues = Value::MakeArray();
    for (const MotionLibraryIssue& issue : list) {
        Value entry = Value::MakeObject();
        entry.Set("path", Value::MakeString(issue.path));
        entry.Set("message", Value::MakeString(issue.message));
        issues.Push(std::move(entry));
    }
    return issues;
}

// What a piece says about itself beyond its description: read when
// choosing one, and again when adapting it.
void SetPieceNotes(const MotionTemplateInfo& info, Value& entry) {
    if (!info.use.empty()) {
        entry.Set("use", Value::MakeString(info.use));
    }
    if (!info.avoid.empty()) {
        entry.Set("avoid", Value::MakeString(info.avoid));
    }
    if (!info.invariants.empty()) {
        Value invariants = Value::MakeArray();
        for (const std::string& invariant : info.invariants) {
            invariants.Push(Value::MakeString(invariant));
        }
        entry.Set("invariants", std::move(invariants));
    }
    if (!info.origin.empty()) {
        entry.Set("origin", Value::MakeString(info.origin));
    }
}

Value FormatsOf(const Value& definition) {
    Value formats = Value::MakeArray();
    if (const Value* declared = definition.Find("formats");
        declared != nullptr && declared->IsObject()) {
        for (const auto& [name, size] : declared->AsObject()) {
            Value format = Value::MakeObject();
            format.Set("name", Value::MakeString(name));
            for (const char* key : {"width", "height"}) {
                if (const Value* value = size.Find(key)) {
                    format.Set(key, *value);
                }
            }
            formats.Push(std::move(format));
        }
    }
    return formats;
}

bool DispatchListTemplates(McpBackend&, const IdResolver&, const Value& args,
                           std::string& resultJson, std::string& errorName,
                           std::string& message) {
    const std::string path = "list_motion_templates";
    std::string query;
    if (!Known(args, {"query"}, path, false, message) ||
        !ReadText(args, "query", path, false, query, message)) {
        return Invalid(errorName, message, message);
    }
    MotionShelves shelves;
    std::string error;
    if (!ReadShelves(shelves, error)) {
        return Refuse(errorName, message, "InvalidMotion", error);
    }
    Value list = Value::MakeArray();
    for (size_t index = 0; index < shelves.pieces.size(); ++index) {
        const MotionTemplateInfo& info = shelves.pieces[index];
        // DEC11 -- docs/roadmap/MOTION-WEB-2026-10.md. A search of the
        // shelf keeps the pieces that answer it, in shelf order: shipped,
        // then library. Kits are few, and always listed.
        if (!query.empty() && !MotionPieceMatches(info, query)) {
            continue;
        }
        Value entry = Value::MakeObject();
        entry.Set("name", Value::MakeString(info.name));
        entry.Set(
            "source",
            Value::MakeString(index < shelves.shipped ? "shipped" : "library"));
        if (info.version > 0) {
            entry.Set("version", Value::MakeInt(info.version));
        }
        entry.Set("description", Value::MakeString(info.description));
        SetPieceNotes(info, entry);
        for (const char* key : {"fps", "duration", "props"}) {
            if (const Value* value = info.definition.Find(key)) {
                entry.Set(key, *value);
            }
        }
        entry.Set("formats", FormatsOf(info.definition));
        list.Push(std::move(entry));
    }
    Value result = Value::MakeObject();
    if (!query.empty()) {
        result.Set("query", Value::MakeString(query));
        result.Set("total",
                   Value::MakeInt(static_cast<int64_t>(shelves.pieces.size())));
    }
    result.Set("templates", std::move(list));
    // MB4 -- the charters a piece can be added with (add_motion_template
    // kit), each with the values it gives the props pieces share.
    Value kits = Value::MakeArray();
    for (const MotionKitInfo& kit : shelves.kits) {
        Value entry = Value::MakeObject();
        entry.Set("name", Value::MakeString(kit.name));
        if (kit.version > 0) {
            entry.Set("version", Value::MakeInt(kit.version));
        }
        entry.Set("description", Value::MakeString(kit.description));
        if (!kit.principle.empty()) {
            entry.Set("principle", Value::MakeString(kit.principle));
        }
        entry.Set("props", kit.props);
        // DEC6 -- a brand's charter: what else it covers, and where its
        // values come from.
        entry.Set("format", Value::MakeString(kit.format));
        if (kit.format == kBrandKitFormat) {
            const auto texts = [](const std::vector<std::string>& list) {
                Value array = Value::MakeArray();
                for (const std::string& item : list) {
                    array.Push(Value::MakeString(item));
                }
                return array;
            };
            entry.Set("colors", texts(MotionKitColors(kit)));
            entry.Set("fonts", texts(MotionKitFonts(kit)));
            if (!kit.captions.IsNull()) {
                entry.Set("captions", kit.captions);
            }
            if (!kit.sources.empty()) {
                entry.Set("sources", texts(kit.sources));
            }
            // M5b -- what the charter gives one piece's own props.
            if (kit.pieces.IsObject()) {
                entry.Set("pieces", kit.pieces);
            }
            // M5b -- and the pieces it does not offer, with the reason.
            if (kit.withdrawn.IsObject()) {
                entry.Set("withdrawn", kit.withdrawn);
            }
        }
        kits.Push(std::move(entry));
    }
    result.Set("kits", std::move(kits));
    Value library = Value::MakeObject();
    library.Set("base", Value::MakeString(shelves.location.base));
    library.Set("directory", Value::MakeString(shelves.location.inbox));
    if (!shelves.imported.empty()) {
        library.Set("imported", IssuesJson(shelves.imported));
    }
    if (!shelves.issues.empty()) {
        library.Set("issues", IssuesJson(shelves.issues));
    }
    result.Set("library", std::move(library));
    resultJson = result.Dump();
    return true;
}

// A template becomes a composition of the project at the size of the
// timeline it is added from: the format of that exact size, its values as
// the props' defaults. From there it is a composition like any other, which
// edit_motion_composition changes and insert_motion_clip or
// place_motion_clip poses.
bool DispatchAddTemplate(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string path = "add_motion_template";
    std::string templateName;
    AddMotionCompositionOperation op;
    bool keepFormats = false;
    std::string kitName;
    if (!Known(args, {"template", "name", "keep_formats", "kit"}, path, true,
               message) ||
        !ReadText(args, "template", path, true, templateName, message) ||
        !ReadText(args, "name", path, false, op.composition.name, message) ||
        !ReadFlag(args, "keep_formats", path, false, keepFormats, message) ||
        !ReadText(args, "kit", path, false, kitName, message)) {
        return Invalid(errorName, message, message);
    }
    MotionShelves shelves;
    std::string error;
    if (!ReadShelves(shelves, error)) {
        return Refuse(errorName, message, "InvalidMotion", error);
    }
    const std::vector<MotionTemplateInfo>& templates = shelves.pieces;
    const auto found = std::find_if(templates.begin(), templates.end(),
                                    [&](const MotionTemplateInfo& info) {
                                        return info.name == templateName;
                                    });
    if (found == templates.end()) {
        std::string known;
        for (const MotionTemplateInfo& info : templates) {
            known += (known.empty() ? "" : ", ") + info.name;
        }
        // A file of that name that is not a piece is the likelier reason
        // than a typo: say what is wrong with it.
        for (const MotionLibraryIssue& issue : shelves.issues) {
            if (std::filesystem::path(issue.path).stem() == templateName) {
                known +=
                    "; " + issue.path + " is not a piece: " + issue.message;
            }
        }
        return Refuse(
            errorName, message, "UnknownTemplate",
            "unknown template '" + templateName + "'; known: " + known);
    }
    // MB4 -- the charter first, on the piece itself: its values become the
    // defaults and the values of every format, then the timeline's format
    // is chosen as it would be without a kit.
    Value piece = found->definition;
    const MotionKitInfo* kit = nullptr;
    MotionKitApplication application;
    if (!kitName.empty()) {
        const auto named = std::find_if(
            shelves.kits.begin(), shelves.kits.end(),
            [&](const MotionKitInfo& info) { return info.name == kitName; });
        if (named == shelves.kits.end()) {
            std::string known;
            for (const MotionKitInfo& info : shelves.kits) {
                known += (known.empty() ? "" : ", ") + info.name;
            }
            for (const MotionLibraryIssue& issue : shelves.issues) {
                if (std::filesystem::path(issue.path).stem() == kitName) {
                    known +=
                        "; " + issue.path + " is not a kit: " + issue.message;
                }
            }
            return Refuse(errorName, message, "UnknownKit",
                          "unknown kit '" + kitName + "' in " +
                              shelves.location.base +
                              "; known: " + (known.empty() ? "none" : known));
        }
        kit = &*named;
        // M5b -- a piece the charter withdraws has its own code: the agent
        // adds it without the charter if the film really wants it.
        const bool withdrawn = !MotionKitWithdrawal(*kit, found->name).empty();
        if (!ApplyMotionKit(*kit, found->name, piece, application, error)) {
            return Refuse(errorName, message,
                          withdrawn ? "WithdrawnByKit" : "KitMismatch", error);
        }
    }
    Document document;
    if (!Snapshot(backend, document, errorName, message)) {
        return false;
    }
    std::string format;
    if (!InstantiateMotionTemplate(
            piece, document.sequence.width, document.sequence.height,
            op.composition.definition, format, error, keepFormats)) {
        return Refuse(errorName, message, "UnknownFormat", error);
    }
    if (op.composition.name.empty()) {
        op.composition.name = found->name;
    }
    const Ulid id = op.composition.id;
    const Value kept = FormatsOf(op.composition.definition);
    if (!backend.ApplyOperation(op, resultJson, errorName, message) ||
        !WithComposition(backend, id, resultJson, errorName, message)) {
        return false;
    }
    Value result;
    if (Value::Parse(resultJson, result, error) && result.IsObject()) {
        result.Set("composition_id", Value::MakeString(id));
        result.Set("template", Value::MakeString(found->name));
        result.Set("format", Value::MakeString(format));
        if (keepFormats) {
            result.Set("formats", kept);
        }
        // MB1 -- what must still hold once the piece is adapted, in front
        // of whoever is about to adapt it.
        SetPieceNotes(*found, result);
        // MB4 -- what the charter set, what it named that the piece does
        // not have, and the principle no value carries.
        if (kit != nullptr) {
            Value given = Value::MakeObject();
            given.Set("name", Value::MakeString(kit->name));
            Value applied = Value::MakeArray();
            for (const std::string& role : application.applied) {
                applied.Push(Value::MakeString(role));
            }
            given.Set("applied", std::move(applied));
            if (!application.not_in_piece.empty()) {
                Value absent = Value::MakeArray();
                for (const std::string& role : application.not_in_piece) {
                    absent.Push(Value::MakeString(role));
                }
                given.Set("not_in_piece", std::move(absent));
            }
            if (!kit->principle.empty()) {
                given.Set("principle", Value::MakeString(kit->principle));
            }
            result.Set("kit", std::move(given));
        }
        resultJson = result.Dump();
    }
    return true;
}

bool DispatchRemove(McpBackend& backend, const IdResolver&, const Value& args,
                    std::string& resultJson, std::string& errorName,
                    std::string& message) {
    const std::string path = "remove_motion_composition";
    std::string id;
    if (!Known(args, {"composition_id"}, path, true, message) ||
        !ReadText(args, "composition_id", path, true, id, message)) {
        return Invalid(errorName, message, message);
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    return backend.ApplyOperation(
        RemoveMotionCompositionOperation{composition->id}, resultJson,
        errorName, message);
}

bool DispatchEdit(McpBackend& backend, const IdResolver&, const Value& args,
                  std::string& resultJson, std::string& errorName,
                  std::string& message) {
    const std::string path = "edit_motion_composition";
    std::string id;
    if (!Known(args, {"composition_id", "edits"}, path, true, message) ||
        !ReadText(args, "composition_id", path, true, id, message)) {
        return Invalid(errorName, message, message);
    }
    const Value* edits = args.Find("edits");
    if (edits == nullptr || !edits->IsArray() || edits->AsArray().empty() ||
        edits->AsArray().size() > kMaxBatchOperations) {
        return Invalid(errorName, message,
                       "'" + path + ".edits' must hold 1 to " +
                           std::to_string(kMaxBatchOperations) + " edits");
    }
    Document scratch;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, scratch, errorName, message) ||
        !ResolveComposition(scratch, id, composition, errorName, message)) {
        return false;
    }
    const Ulid compositionId = composition->id;
    // Each edit is resolved against the composition the edits before it
    // leave, on a copy: a node posed by one edit can be named by the next.
    // What reaches the backend is the operations as that dry run enriched
    // them, identities included, so applying them again gives the same
    // result.
    std::vector<Operation> operations;
    for (size_t index = 0; index < edits->AsArray().size(); ++index) {
        const std::string editPath =
            path + ".edits[" + std::to_string(index) + "]";
        const DocumentComposition* current =
            scratch.FindComposition(compositionId);
        Operation operation;
        if (!BuildEdit(*current, edits->AsArray()[index], editPath, operation,
                       errorName, message)) {
            return false;
        }
        Operation inverse;
        EditError error = EditError::None;
        std::string refusal;
        if (!ApplyOperation(scratch, operation, inverse, error, refusal)) {
            std::string text = editPath;
            text += ": ";
            text += refusal;
            return Refuse(errorName, message, EditErrorName(error), text);
        }
        operations.push_back(std::move(operation));
    }
    // MOTION-DESIGN-2026-09 -- a replacement read from a file answers
    // compactly (CompactResult).
    bool fromFile = false;
    for (const Value& edit : edits->AsArray()) {
        fromFile = fromFile || edit.Find("definition_file") != nullptr;
    }
    const bool applied =
        operations.size() == 1
            ? backend.ApplyOperation(std::move(operations.front()), resultJson,
                                     errorName, message)
            // PL1 -- several edits are one intervention, one undo step.
            : backend.ApplyOperation(
                  BatchOperation{path, std::move(operations)}, resultJson,
                  errorName, message);
    if (applied && fromFile) {
        CompactResult(compositionId, resultJson);
    }
    return applied;
}

bool DispatchRenderClip(McpBackend& backend, const IdResolver& resolver,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    const std::string path = "render_motion_frame";
    for (const char* other :
         {"composition_id", "frame", "props", "variant", "format"}) {
        if (args.Find(other) != nullptr) {
            return Invalid(errorName, message,
                           "'" + path + "." + other +
                               "' does not go with clip_id: the clip names "
                               "its composition, its props and, with time, "
                               "its frame");
        }
    }
    const Value* clip = args.Find("clip_id");
    const Value* time = args.Find("time");
    Ulid clipId;
    if (!clip->IsString() ||
        !resolver.Resolve(path + ".clip_id", clip->AsString(), clipId,
                          message)) {
        return Invalid(
            errorName, message,
            message.empty() ? "'" + path + ".clip_id' is a string" : message);
    }
    int64_t value = 0;
    int64_t rate = 0;
    if (time == nullptr || !time->IsObject() ||
        time->Find("value") == nullptr || time->Find("rate") == nullptr ||
        !time->Find("value")->AsInt64(value) ||
        !time->Find("rate")->AsInt64(rate) || rate <= 0 || rate > INT32_MAX ||
        time->AsObject().size() != 2) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".time' is required with clip_id: {value, rate}, "
                           "a time of the timeline");
    }
    std::vector<std::string> fonts;
    if (const Value* directories = args.Find("font_dirs")) {
        if (!directories->IsArray()) {
            return Invalid(errorName, message,
                           "'" + path + ".font_dirs' must be an array");
        }
        for (const Value& directory : directories->AsArray()) {
            if (!directory.IsString()) {
                return Invalid(
                    errorName, message,
                    "'" + path + ".font_dirs' holds directory paths");
            }
            fonts.push_back(directory.AsString());
        }
    }
    return RenderMotionClipFrame(backend, clipId,
                                 {value, static_cast<int32_t>(rate)}, fonts,
                                 resultJson, errorName, message);
}

bool DispatchRender(McpBackend& backend, const IdResolver& resolver,
                    const Value& args, std::string& resultJson,
                    std::string& errorName, std::string& message) {
    const std::string path = "render_motion_frame";
    std::string id;
    MotionFrameRequest request;
    if (!Known(args,
               {"composition_id", "frame", "variant", "format", "props",
                "font_dirs", "clip_id", "time"},
               path, false, message)) {
        return Invalid(errorName, message, message);
    }
    // MOTION-2026-09 M4 -- or what a motion clip shows at a time of the
    // timeline: its composition, its props, the frame that time falls on.
    if (args.Find("clip_id") != nullptr) {
        return DispatchRenderClip(backend, resolver, args, resultJson,
                                  errorName, message);
    }
    // PRD15 -- ROADMAP.md. One of two arguments, which the schema cannot
    // require of every client: said as such rather than as a "required"
    // composition_id the schema does not list.
    if (args.Find("composition_id") == nullptr) {
        return Invalid(errorName, message,
                       "'render_motion_frame' takes composition_id, or "
                       "clip_id with time for what a motion clip shows");
    }
    if (!ReadText(args, "composition_id", path, true, id, message) ||
        !ReadText(args, "variant", path, false, request.variant, message) ||
        !ReadText(args, "format", path, false, request.format, message)) {
        return Invalid(errorName, message, message);
    }
    const Value* frame = args.Find("frame");
    if (frame == nullptr || !frame->AsInt64(request.frame)) {
        return Invalid(errorName, message,
                       "'" + path + ".frame' must be an integer frame");
    }
    if (const Value* props = args.Find("props")) {
        if (!props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        request.props = *props;
    }
    if (const Value* fonts = args.Find("font_dirs")) {
        if (!fonts->IsArray()) {
            return Invalid(errorName, message,
                           "'" + path + ".font_dirs' must be an array");
        }
        for (const Value& directory : fonts->AsArray()) {
            if (!directory.IsString()) {
                return Invalid(
                    errorName, message,
                    "'" + path + ".font_dirs' holds directory paths");
            }
            request.font_directories.push_back(directory.AsString());
        }
    }
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("this process has no motion renderer "
                                  "(Core Text and Core Graphics): ") +
                          kMotionRendererHolder);
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    MotionComposition parsed;
    MotionError error;
    if (!ParseDocumentComposition(*composition, parsed, error)) {
        return Refuse(errorName, message, error.code,
                      error.path + ": " + error.message);
    }
    request.composition = &parsed;
    request.media_directory = backend.MotionMediaDirectory();
    MotionFrameImage image;
    if (!MotionReachFor(document, composition->id, request.reach, error) ||
        !render(request, image, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    // The picture, and as text the geometry of that same evaluation: every
    // box, line, word and path, each with the uid of its document node.
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey, Value::MakeString(EncodeBase64(image.png)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/png"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(image.geometry));
    resultJson = envelope.Dump();
    return true;
}

bool ReadTexts(const Value& args, const std::string& key,
               const std::string& path, std::vector<std::string>& out,
               std::string& message) {
    const Value* value = args.Find(key);
    if (value == nullptr) {
        return true;
    }
    const bool strings =
        value->IsArray() &&
        std::all_of(value->AsArray().begin(), value->AsArray().end(),
                    [](const Value& item) { return item.IsString(); });
    if (!strings) {
        message = "'";
        message += path;
        message += ".";
        message += key;
        message += "' must be an array of strings";
        return false;
    }
    for (const Value& item : value->AsArray()) {
        out.push_back(item.AsString());
    }
    return true;
}

// AX-2026-09 AX6 -- docs/roadmap/AX-2026-09.md. The families a composition
// or a caption style can name (FontCatalog.h), so that an agent stops
// reading ~/Library/Fonts.
bool DispatchListFonts(McpBackend&, const IdResolver&, const Value& args,
                       std::string& resultJson, std::string& errorName,
                       std::string& message) {
    const std::string path = "list_fonts";
    std::string query;
    std::vector<std::string> fonts;
    if (!Known(args, {"query", "font_dirs"}, path, false, message) ||
        !ReadText(args, "query", path, false, query, message) ||
        !ReadTexts(args, "font_dirs", path, fonts, message)) {
        return Invalid(errorName, message, message);
    }
    FontCatalog catalog;
    MotionError error;
    if (!ListFonts(fonts, query, catalog, error)) {
        return Refuse(errorName, message, error.code, error.message);
    }
    resultJson = FontCatalogJson(catalog).Dump();
    return true;
}

// M6 -- a composition rendered to files, in versions and formats. The
// renderer is the executable's (MotionVideo.h), as render_motion_frame's is.
bool DispatchVideo(McpBackend& backend, const IdResolver&, const Value& args,
                   std::string& resultJson, std::string& errorName,
                   std::string& message) {
    const std::string path = "render_motion_video";
    std::string id;
    MotionVideoRequest request;
    MotionVideoOptions& options = request.options;
    if (!Known(args,
               {"composition_id", "output", "variants", "formats", "alpha",
                "overwrite", "preview", "font_dirs", "loudness", "true_peak"},
               path, false, message) ||
        !ReadText(args, "composition_id", path, true, id, message) ||
        !ReadText(args, "output", path, true, options.output, message) ||
        !ReadTexts(args, "variants", path, options.variants, message) ||
        !ReadTexts(args, "formats", path, options.formats, message) ||
        !ReadTexts(args, "font_dirs", path, request.font_directories,
                   message) ||
        !ReadFlag(args, "alpha", path, false, options.alpha, message) ||
        !ReadFlag(args, "overwrite", path, false, options.overwrite, message) ||
        !ReadFlag(args, "preview", path, true, options.preview, message)) {
        return Invalid(errorName, message, message);
    }
    // MM1 -- a delivery loudness and its ceiling, checked by the planner.
    for (const char* key : {"loudness", "true_peak"}) {
        const Value* field = args.Find(key);
        double number = 0.0;
        if (field == nullptr) {
            continue;
        }
        if (!MotionNumber(*field, number)) {
            return Invalid(errorName, message,
                           "'" + path + "." + key + "' must be a number");
        }
        if (std::string(key) == "loudness") {
            options.loudness = number;
        } else {
            options.true_peak = number;
        }
    }
    const MotionVideoRenderer render = CurrentMotionVideoRenderer();
    if (render == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("this process has no motion renderer "
                                  "(Core Text and Core Graphics): ") +
                          kMotionRendererHolder);
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    MotionComposition parsed;
    MotionError error;
    if (!ParseDocumentComposition(*composition, parsed, error)) {
        return Refuse(errorName, message, error.code,
                      error.path + ": " + error.message);
    }
    request.composition = &parsed;
    request.media_directory = backend.MotionMediaDirectory();
    // PRD16 -- ROADMAP.md. On a background task, each file is followed and
    // can be stopped (MotionVideo.h).
    request.runner = backend.BackgroundTaskRunner();
    if (!MotionReachFor(document, composition->id, request.reach, error)) {
        return Refuse(errorName, message, error.code,
                      error.path + ": " + error.message);
    }
    std::string report;
    if (!render(request, report, error)) {
        // Files completed before a failure are listed beside the refusal.
        resultJson = report;
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    resultJson = report;
    return true;
}

// A path the caller names for a file this tool writes: absolute, with that
// extension, in a directory that exists, and not there yet -- like the
// bridge's, which never overwrites.
bool NewFilePath(const std::string& path, const std::string& key,
                 const std::string& extension, std::string& errorName,
                 std::string& message) {
    const std::filesystem::path file(path);
    std::error_code error;
    if (!file.is_absolute() || file.extension() != extension) {
        return Invalid(errorName, message,
                       "'export_motion_after." + key + "' must be an " +
                           "absolute " + extension + " path");
    }
    if (!std::filesystem::is_directory(file.parent_path(), error)) {
        return Refuse(
            errorName, message, "IoError",
            "directory does not exist: '" + file.parent_path().string() + "'");
    }
    if (std::filesystem::exists(file, error)) {
        return Refuse(errorName, message,
                      extension == ".aep" ? "ProjectExists" : "IoError",
                      "refusing to overwrite existing file '" + path + "'");
    }
    return true;
}

// MA1 -- docs/roadmap/MOTION-AFTER-2026-09.md. A composition as an After
// Effects project a motion designer can retouch: the engine describes
// (MotionAfterExport.h), the bridge's build_motion_project builds and saves.
bool DispatchExportAfter(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string path = "export_motion_after";
    std::string id;
    std::string projectPath;
    std::string timelineId;
    std::string descriptionPath;
    bool build = true;
    MotionAfterRequest request;
    if (!Known(args,
               {"composition_id", "project_path", "timeline_id", "name",
                "props", "font_dirs", "description_path", "build", "boil"},
               path, false, message) ||
        !ReadText(args, "composition_id", path, true, id, message) ||
        !ReadText(args, "project_path", path, true, projectPath, message) ||
        !ReadText(args, "timeline_id", path, false, timelineId, message) ||
        !ReadText(args, "name", path, false, request.options.name, message) ||
        !ReadText(args, "description_path", path, false, descriptionPath,
                  message) ||
        !ReadTexts(args, "font_dirs", path, request.font_directories,
                   message) ||
        !ReadTexts(args, "boil", path, request.options.boil, message) ||
        !ReadFlag(args, "build", path, true, build, message)) {
        return Invalid(errorName, message, message);
    }
    if (const Value* props = args.Find("props")) {
        if (!props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        request.props = *props;
    }
    if (!build && descriptionPath.empty()) {
        return Invalid(errorName, message,
                       "without build, description_path is where the "
                       "description goes");
    }
    if (!NewFilePath(projectPath, "project_path", ".aep", errorName, message) ||
        (!descriptionPath.empty() &&
         !NewFilePath(descriptionPath, "description_path", ".json", errorName,
                      message))) {
        return false;
    }
    const MotionAfterDescriber describe = CurrentMotionAfterDescriber();
    if (describe == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      "this process cannot measure text (Core Text): use the "
                      "cutmachine executable");
    }

    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    MotionComposition parsed;
    MotionError error;
    if (!ParseDocumentComposition(*composition, parsed, error) ||
        !MotionReachFor(document, composition->id, request.reach, error)) {
        return Refuse(errorName, message, error.code,
                      error.path + ": " + error.message);
    }
    request.composition = &parsed;
    request.options.media_directory = backend.MotionMediaDirectory();
    request.options.aep_path = projectPath;
    if (request.options.name.empty() || args.Find("name") == nullptr) {
        request.options.name = composition->name;
    }
    if (!timelineId.empty()) {
        // The sounds of that timeline go under the pictures.
        Project project;
        if (!backend.SnapshotProject(project, message)) {
            return Refuse(errorName, message, "UnsupportedOperation", message);
        }
        const DocumentSequence* timeline = nullptr;
        size_t matches = 0;
        for (const DocumentSequence& candidate : project.timelines) {
            if (candidate.id == timelineId) {
                timeline = &candidate;
                matches = 1;
                break;
            }
            if (candidate.id.compare(0, timelineId.size(), timelineId) == 0) {
                timeline = &candidate;
                ++matches;
            }
        }
        if (matches != 1) {
            return Refuse(errorName, message,
                          matches > 1 ? "ValidationFailed" : "UnknownTimeline",
                          matches > 1
                              ? "timeline_id '" + timelineId +
                                    "' matches several timelines"
                              : "unknown timeline_id '" + timelineId + "'");
        }
        if (!MotionAfterTimelineAudio(project.MakeDocument(timeline->id),
                                      request.options.media_directory,
                                      request.options.audio, error)) {
            return Refuse(errorName, message, error.code,
                          error.path + ": " + error.message);
        }
    }

    Value description;
    MotionAfterExportReport report;
    if (!describe(request, description, report, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    // Without a path of its own the description is a private step of the
    // build, removed once After has read it.
    const bool kept = !descriptionPath.empty();
    const std::string written =
        kept ? descriptionPath
             : (std::filesystem::temp_directory_path() /
                ("cutmachine-motion-after-" + GenerateUlid() + ".json"))
                   .string();
    {
        std::ofstream file(written, std::ios::binary | std::ios::trunc);
        file << description.Dump();
        if (!file) {
            return Refuse(errorName, message, "IoError",
                          "cannot write '" + written + "'");
        }
    }
    Value result = Value::MakeObject();
    result.Set("project_path", Value::MakeString(projectPath));
    result.Set("built", Value::MakeBool(build));
    MotionAfterReportFields(report, result);
    if (kept) {
        result.Set("description_path", Value::MakeString(written));
    }
    if (build) {
        AfterBridgeRequest bridge;
        bridge.method = AfterBridgeMethod::BuildMotionProject;
        bridge.arguments.Set("description_path", Value::MakeString(written));
        bridge.arguments.Set("project_path", Value::MakeString(projectPath));
        std::string responseJson;
        const bool ok =
            backend.RunAfterRequest(SerializeAfterBridgeRequest(bridge),
                                    responseJson, errorName, message);
        if (!kept) {
            std::error_code ignored;
            std::filesystem::remove(written, ignored);
        }
        if (!ok) {
            return false;
        }
        Value after;
        std::string parseError;
        if (Value::Parse(responseJson, after, parseError)) {
            result.Set("after", std::move(after));
        }
    }
    resultJson = result.Dump();
    return true;
}

// MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md (DEC43). A composition
// of the project open in After, read whole by the bridge (read_comp),
// translated by the engine (MotionAfterImport.h) and added to the project
// as add_motion_composition adds one: one undo step, the same operation.
// What does not translate is listed in `notes`.
bool DispatchImportAfter(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string path = "import_after_comp";
    std::string compId;
    std::string name;
    std::string readingPath;
    MotionAfterImportRequest request;
    if (!Known(args, {"comp_id", "name", "fonts", "font_dirs", "reading_path"},
               path, true, message) ||
        !ReadText(args, "comp_id", path, true, compId, message) ||
        !ReadText(args, "name", path, false, name, message) ||
        !ReadText(args, "reading_path", path, false, readingPath, message) ||
        !ReadTexts(args, "font_dirs", path, request.font_directories,
                   message)) {
        return Invalid(errorName, message, message);
    }
    if (const Value* fonts = args.Find("fonts")) {
        if (!fonts->IsObject()) {
            return Invalid(errorName, message,
                           "'" + path + ".fonts' must be an object");
        }
        for (const auto& [from, to] : fonts->AsObject()) {
            if (from.empty() || !to.IsString() || to.AsString().empty()) {
                return Invalid(errorName, message,
                               "'" + path +
                                   ".fonts' maps a PostScript name read in "
                                   "After to a non-empty PostScript name");
            }
            request.options.fonts[from] = to.AsString();
        }
    }
    if (!readingPath.empty()) {
        const std::filesystem::path file(readingPath);
        std::error_code error;
        if (!file.is_absolute() || file.extension() != ".json") {
            return Invalid(
                errorName, message,
                "'" + path + ".reading_path' must be an absolute .json path");
        }
        if (std::filesystem::exists(file, error)) {
            return Refuse(
                errorName, message, "IoError",
                "refusing to overwrite existing file '" + readingPath + "'");
        }
    }
    const MotionAfterImporter import = CurrentMotionAfterImporter();
    if (import == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      "this process cannot measure text (Core Text): use the "
                      "cutmachine executable");
    }
    AfterBridgeRequest bridge;
    bridge.method = AfterBridgeMethod::ReadComp;
    bridge.arguments.Set("comp_id", Value::MakeString(compId));
    std::string responseJson;
    if (!backend.RunAfterRequest(SerializeAfterBridgeRequest(bridge),
                                 responseJson, errorName, message)) {
        return false;
    }
    Value reading;
    std::string parseError;
    if (!Value::Parse(responseJson, reading, parseError)) {
        return Refuse(errorName, message, "AfterExecutionFailed",
                      "unreadable read_comp answer: " + parseError);
    }
    if (!readingPath.empty()) {
        std::ofstream file(readingPath, std::ios::binary | std::ios::trunc);
        file << responseJson;
        if (!file) {
            return Refuse(errorName, message, "IoError",
                          "cannot write '" + readingPath + "'");
        }
    }
    request.comp = &reading;
    Value definition;
    MotionAfterImportReport report;
    MotionError error;
    if (!import(request, definition, report, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    AddMotionCompositionOperation op;
    op.composition.name = name.empty() ? report.comp_name : name;
    op.composition.definition = std::move(definition);
    const Ulid id = op.composition.id;
    if (!backend.ApplyOperation(op, resultJson, errorName, message)) {
        return false;
    }
    Value result;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject()) {
        result = Value::MakeObject();
    }
    result.Set("composition_id", Value::MakeString(id));
    MotionAfterImportReportFields(report, result);
    if (!readingPath.empty()) {
        result.Set("reading_path", Value::MakeString(readingPath));
    }
    resultJson = result.Dump();
    return true;
}

// MA2 -- docs/roadmap/MOTION-AFTER-2026-09.md. The project export_motion_after
// built, checked against the engine: After renders each frame through the
// bridge's render_frame (the method of after_render_frame), the engine
// renders the same frame, and each gap is judged against the exporter's own
// report (MotionAfterVerify.h). Both pictures stay in image_dir.
bool DispatchVerifyAfter(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string path = "verify_after_export";
    std::string id;
    std::string compId;
    std::string directory;
    MotionAfterRequest describing;
    MotionAfterVerifyRequest request;
    if (!Known(args,
               {"composition_id", "comp_id", "image_dir", "frames", "step",
                "tolerance", "props", "font_dirs", "boil"},
               path, false, message) ||
        !ReadText(args, "composition_id", path, true, id, message) ||
        !ReadText(args, "comp_id", path, true, compId, message) ||
        !ReadText(args, "image_dir", path, true, directory, message) ||
        !ReadTexts(args, "font_dirs", path, describing.font_directories,
                   message) ||
        !ReadTexts(args, "boil", path, describing.options.boil, message)) {
        return Invalid(errorName, message, message);
    }
    if (const Value* props = args.Find("props")) {
        if (!props->IsObject()) {
            return Invalid(errorName, message,
                           "'" + path + ".props' must be an object");
        }
        describing.props = *props;
    }
    if (const Value* tolerance = args.Find("tolerance")) {
        double read = -1.0;
        if (!MotionNumber(*tolerance, read) || !(read >= 0.0) || read > 255.0) {
            return Invalid(errorName, message,
                           "'" + path +
                               ".tolerance' must be a number of luma levels, "
                               "0 to 255");
        }
        request.tolerance = read;
    }
    const Value* frames = args.Find("frames");
    const Value* step = args.Find("step");
    int64_t every = 8;
    if (frames != nullptr && step != nullptr) {
        return Invalid(errorName, message,
                       "'" + path + "' takes frames or step, not both");
    }
    if (step != nullptr && (!step->AsInt64(every) || every < 1)) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".step' must be a whole number of frames, "
                           "1 or more");
    }
    if (frames != nullptr) {
        if (!frames->IsArray() || frames->AsArray().empty()) {
            return Invalid(errorName, message,
                           "'" + path +
                               ".frames' must be a non-empty array "
                               "of frames");
        }
        for (const Value& frame : frames->AsArray()) {
            int64_t read = 0;
            if (!frame.AsInt64(read)) {
                return Invalid(errorName, message,
                               "'" + path + ".frames' holds whole frames");
            }
            request.frames.push_back(read);
        }
    }
    const std::filesystem::path folder(directory);
    std::error_code fileError;
    if (!folder.is_absolute()) {
        return Invalid(errorName, message,
                       "'" + path + ".image_dir' must be an absolute path");
    }
    if (std::filesystem::exists(folder, fileError)) {
        return Refuse(errorName, message, "IoError",
                      "refusing to write into an existing '" + directory +
                          "': image_dir is a new directory");
    }
    if (!std::filesystem::is_directory(folder.parent_path(), fileError)) {
        return Refuse(errorName, message, "IoError",
                      "directory does not exist: '" +
                          folder.parent_path().string() + "'");
    }
    const MotionAfterDescriber describe = CurrentMotionAfterDescriber();
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (describe == nullptr || render == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("this process cannot render nor measure "
                                  "text (Core Text): ") +
                          kMotionRendererHolder);
    }

    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    MotionComposition parsed;
    MotionError error;
    if (!ParseDocumentComposition(*composition, parsed, error) ||
        !MotionReachFor(document, composition->id, describing.reach, error)) {
        return Refuse(errorName, message, error.code,
                      error.path + ": " + error.message);
    }
    if (frames == nullptr) {
        for (int64_t frame = 0; frame < parsed.duration; frame += every) {
            request.frames.push_back(frame);
        }
    }
    for (const int64_t frame : request.frames) {
        if (frame < 0 || frame >= parsed.duration) {
            return Invalid(errorName, message,
                           "frame " + std::to_string(frame) +
                               " is outside the composition (0 to " +
                               std::to_string(parsed.duration - 1) + ")");
        }
    }

    // The report of the export as it would be made now: which node each
    // approximation is about.
    describing.composition = &parsed;
    describing.options.media_directory = backend.MotionMediaDirectory();
    describing.options.name = composition->name;
    Value description;
    if (!describe(describing, description, request.report, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    request.engine.composition = &parsed;
    request.engine.props = describing.props;
    request.engine.media_directory = describing.options.media_directory;
    request.engine.font_directories = describing.font_directories;
    request.engine.reach = describing.reach;
    request.directory = directory;
    if (!std::filesystem::create_directory(folder, fileError)) {
        return Refuse(errorName, message, "IoError",
                      "cannot create '" + directory + "'");
    }

    const MotionAfterFrameSource after =
        [&](int64_t frame, const std::string& image, std::string& code,
            std::string& text) {
            AfterBridgeRequest bridge;
            bridge.method = AfterBridgeMethod::RenderFrame;
            bridge.arguments.Set("comp_id", Value::MakeString(compId));
            bridge.arguments.Set("frame", Value::MakeInt(frame));
            bridge.arguments.Set("image_path", Value::MakeString(image));
            std::string response;
            return backend.RunAfterRequest(SerializeAfterBridgeRequest(bridge),
                                           response, code, text);
        };
    std::vector<MotionAfterFrameCheck> checks;
    if (!VerifyMotionAfterFrames(request, render, after, checks, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    Value result = Value::MakeObject();
    result.Set("comp_id", Value::MakeString(compId));
    MotionAfterVerifyFields(request, checks, result);
    resultJson = result.Dump();
    return true;
}

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. A composition written by a
// machine rewritten at a human's level (MotionHumanize.h), proved by
// rendering every frame of both, and applied as one UpdateMotionComposition:
// one undo takes it back. Refused, and nothing applied, when one frame would
// change.
bool DispatchHumanize(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    const std::string path = "humanize_motion_composition";
    std::string id;
    bool apply = true;
    if (!Known(args, {"composition_id", "apply"}, path, true, message) ||
        !ReadText(args, "composition_id", path, true, id, message) ||
        !ReadFlag(args, "apply", path, true, apply, message)) {
        return Invalid(errorName, message, message);
    }
    const MotionHumanizer humanize = CurrentMotionHumanizer();
    if (humanize == nullptr) {
        return Refuse(errorName, message, "UnsupportedOperation",
                      "this process cannot measure text (Core Text): use the "
                      "cutmachine executable");
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    MotionHumanizeRequest request;
    request.definition = &composition->definition;
    request.media_directory = backend.MotionMediaDirectory();
    MotionError error;
    Value humanized;
    MotionHumanizeReport report;
    MotionComposition before;
    MotionComposition after;
    if (!MotionReachFor(document, composition->id, request.reach, error) ||
        !humanize(request, humanized, report, error) ||
        !ParseMotionComposition(composition->definition, before, error) ||
        !ParseMotionComposition(humanized, after, error)) {
        return Refuse(
            errorName, message, error.code,
            (error.path.empty() ? "" : error.path + ": ") + error.message);
    }
    MotionFrameRequest base;
    base.media_directory = request.media_directory;
    base.reach = request.reach;
    std::vector<int64_t> differing;
    if (!CompareMotionRenders(base, before, after, differing, error)) {
        return Refuse(errorName, message, error.code, error.message);
    }
    if (!differing.empty()) {
        std::string frames;
        for (size_t index = 0; index < differing.size() && index < 12;
             ++index) {
            frames +=
                (index > 0 ? ", " : "") + std::to_string(differing[index]);
        }
        return Refuse(errorName, message, "NotEquivalent",
                      std::to_string(differing.size()) +
                          " frame(s) would change, first " + frames +
                          ": nothing applied");
    }
    Value result = Value::MakeObject();
    result.Set("composition_id", Value::MakeString(composition->id));
    MotionHumanizeReportFields(report, result);
    result.Set("frames_compared", Value::MakeInt(before.duration));
    const bool changed = report.timecodes + report.wrappers + report.words > 0;
    if (apply && changed) {
        DocumentComposition updated = *composition;
        updated.definition = humanized;
        std::string applied;
        if (!backend.ApplyOperation(UpdateMotionCompositionOperation{updated},
                                    applied, errorName, message)) {
            return false;
        }
        CompactResult(updated.id, applied);
        Value edit;
        std::string parseError;
        if (Value::Parse(applied, edit, parseError)) {
            result.Set("edit", std::move(edit));
        }
    } else if (changed) {
        result.Set("definition", humanized);
    }
    result.Set("applied", Value::MakeBool(apply && changed));
    resultJson = result.Dump();
    return true;
}

// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The way back to the
// shelf: a composition built or adapted in a project becomes a piece of
// this machine's library, which every project then lists. It writes a file
// outside the project and leaves the document alone, like an export: not
// an operation, nothing to undo.
bool DispatchSaveTemplate(McpBackend& backend, const IdResolver&,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    const std::string path = "save_motion_template";
    std::string id;
    MotionTemplateInfo piece;
    bool overwrite = false;
    if (!Known(args,
               {"composition_id", "template", "description", "use", "avoid",
                "invariants", "origin", "overwrite"},
               path, false, message) ||
        !ReadText(args, "composition_id", path, true, id, message) ||
        !ReadText(args, "template", path, true, piece.name, message) ||
        !ReadText(args, "description", path, true, piece.description,
                  message) ||
        !ReadText(args, "use", path, false, piece.use, message) ||
        !ReadText(args, "avoid", path, false, piece.avoid, message) ||
        !ReadTexts(args, "invariants", path, piece.invariants, message) ||
        !ReadText(args, "origin", path, false, piece.origin, message) ||
        !ReadFlag(args, "overwrite", path, false, overwrite, message)) {
        return Invalid(errorName, message, message);
    }
    if (!IsMotionPieceName(piece.name)) {
        return Invalid(errorName, message,
                       "'" + path +
                           ".template' must be a piece name: "
                           "letters, digits, '-' and '_', 64 at most");
    }
    if (piece.description.empty() ||
        std::any_of(piece.invariants.begin(), piece.invariants.end(),
                    [](const std::string& text) { return text.empty(); })) {
        return Invalid(errorName, message,
                       "'" + path +
                           "' needs a description, and no empty "
                           "invariant: a piece is chosen by what it says of "
                           "itself");
    }
    std::vector<MotionTemplateInfo> shipped;
    std::string error;
    if (!MotionTemplateCatalog(shipped, error)) {
        return Refuse(errorName, message, "InvalidMotion", error);
    }
    if (std::any_of(shipped.begin(), shipped.end(),
                    [&](const MotionTemplateInfo& info) {
                        return info.name == piece.name;
                    })) {
        return Refuse(errorName, message, "TemplateExists",
                      "'" + piece.name +
                          "' is the name of a template CUTMACHINE ships; a "
                          "library piece never replaces one: save under "
                          "another name");
    }
    Document document;
    const DocumentComposition* composition = nullptr;
    if (!Snapshot(backend, document, errorName, message) ||
        !ResolveComposition(document, id, composition, errorName, message)) {
        return false;
    }
    if (!MotionPieceDefinition(composition->definition, piece.definition,
                               error)) {
        return Refuse(errorName, message, "ProjectBound", error);
    }
    // The file is judged before it is written, by the reader that will
    // list it: what the shelf holds can always be added.
    MotionTemplateInfo read;
    if (!ParseMotionPiece(piece.name, DumpMotionPiece(piece), read, error)) {
        return Refuse(errorName, message, "InvalidMotion",
                      "the composition does not make a piece: " + error);
    }
    // MB10 -- the next version of its name in the personal base; the
    // versions before it stay.
    const MotionLibraryLocation location = CurrentMotionLibraryLocation();
    int64_t version = 0;
    bool existed = false;
    if (!SaveMotionPieceVersion(location, piece, overwrite, version, existed,
                                error)) {
        return Refuse(errorName, message,
                      existed && !overwrite ? "TemplateExists" : "IoError",
                      error);
    }
    Value result = Value::MakeObject();
    result.Set("template", Value::MakeString(piece.name));
    result.Set("base", Value::MakeString(location.base));
    result.Set("version", Value::MakeInt(version));
    result.Set("replaced", Value::MakeBool(existed));
    result.Set("formats", FormatsOf(piece.definition));
    resultJson = result.Dump();
    return true;
}

const char kNodeReference[] =
    "A node: its uid, the author's id for it (\\\"titre\\\"), or an "
    "unambiguous uid prefix.";

}  // namespace

bool IsMotionEditingTool(const std::string& name) {
    return name == "add_motion_composition" ||
           name == "remove_motion_composition" ||
           name == "edit_motion_composition" || name == "add_motion_template" ||
           name == "humanize_motion_composition" || name == "import_after_comp";
}

void RegisterMotionTools(const McpToolAdder& add) {
    // PRD15 -- compositions have no alias: an ID or a prefix of one.
    const std::string compositionId =
        Described("string", "Composition to use.");
    add("list_motion_compositions",
        "List the project's motion compositions (title cards, lower thirds, "
        "animated text): id, name, size, frame rate, duration in frames and "
        "their props with types. Compositions belong to the project, not to "
        "a timeline.",
        Schema({}, {}, "list_motion_compositions takes no arguments"),
        DispatchList);

    add("describe_motion_composition",
        "Read one motion composition as it is stored: its definition in the "
        "format of docs/MOTION_FORMAT.md (CSS names for layout, "
        "interpolate/spring as in Remotion), with the uid of every node and "
        "the uids of every interpolation's keyframes.",
        Schema({{"composition_id", compositionId}}, {"composition_id"},
               "describe_motion_composition arguments"),
        DispatchDescribe);

    add("add_motion_composition",
        "Add a motion composition to the project. `definition` follows "
        "docs/MOTION_FORMAT.md: width, height, fps, duration (frames), props "
        "(typed: text, number with min/max, color, bool, enum, list, point {x, "
        "y}, size {width, height}, group), optional tokens, and a root node "
        "tree laid out with flexbox and animated with interpolate or spring. "
        "You choose layout and timing; the engine measures text and computes "
        "every box. Every node and keyframe receives a uid; the result returns "
        "the stored composition. Decimal numbers are kept as written.",
        Schema({{"name", Described("string", "Name shown to the editor.")},
                {"definition",
                 Described("object",
                           "The composition, in docs/MOTION_FORMAT.md.")},
                {"definition_file",
                 Described("string",
                           "Instead of definition: absolute path of a JSON "
                           "file holding it. The answer is then compact: "
                           "composition_id, revision, created_nodes.")}},
               {"name"}, "add_motion_composition arguments"),
        DispatchAdd);

    // M5 -- docs/roadmap/MOTION-2026-09.md; MB1 --
    // docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md for the library.
    add("list_motion_templates",
        "List the motion pieces you can start from: the templates CUTMACHINE "
        "ships (source: shipped) and the pieces of this machine's motion "
        "library (source: library). Each comes with its description, its props "
        "(type, bounds, default), the timeline sizes it has a layout for, and "
        "when its author wrote them: use and avoid (when it fits, when it does "
        "not) and invariants (what must still hold once you adapt it). `kits` "
        "are the brands' charters of the library (brand-kit.v1, or the older "
        "motion-kit.v1 of props alone: description, principle, props among "
        "accent, texte, fond, police), which add_motion_template applies with "
        "`kit`. Files dropped in `library.directory` enter the library when "
        "this is read (`library.imported`, `library.issues`). Burned-in "
        "subtitles are caption tracks (generate_captions), not a template. "
        "`query` narrows the shelf, which can hold hundreds of pieces; kits "
        "are listed either way.",
        Schema({{"query",
                 Described("string",
                           "Words that must all appear in a piece's name, "
                           "description, use, avoid, invariants or origin, "
                           "as substrings, ignoring case and accents. "
                           "Omitted: the whole shelf.")}},
               {}, "list_motion_templates arguments"),
        DispatchListTemplates);

    // AX-2026-09 AX6 -- docs/roadmap/AX-2026-09.md.
    add("list_fonts",
        "List the font families a motion text (font_family) or a caption "
        "style can name on this machine. `families`: those Core Text finds, "
        "which lays out and draws motion and captions, in Record and in the "
        "file; a composition naming another is refused (FontMissing), a "
        "caption is drawn in Helvetica Neue. `captions.missing`: those of "
        "`families` a caption is still not drawn in (rare: a family of italic "
        "faces only); `captions.checked` false gives the reason "
        "it could not tell. Fonts activated by Adobe Fonts are there only "
        "while Creative Cloud runs. Choose a font here rather than in "
        "~/Library/Fonts.",
        Schema({{"query", Described("string",
                                    "Part of a family name, without case: "
                                    "only the families that contain it.")},
                {"font_dirs",
                 "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
                 "\"description\":" +
                     Quoted("Directories of .ttf/.otf files to register "
                            "first, as the render tools take them.") +
                     "}"}},
               {}, "list_fonts arguments"),
        DispatchListFonts);

    add("add_motion_template",
        "Add a piece of list_motion_templates to the project as a motion "
        "composition, at the size of the timeline: the piece's format of "
        "exactly that size is chosen and its values become the props' "
        "defaults, so the text sizes and margins suit the frame. The other "
        "formats are dropped unless keep_formats is true. The project takes a "
        "copy: it does not depend on the library afterwards. `kit` names a "
        "charter of the library (list_motion_templates `kits`): its colours "
        "and font become the values of the props the pieces share (accent, "
        "texte, fond, police) in every format, and its `pieces` section sets "
        "this piece's own props by name; `kit.not_in_piece` says what the "
        "piece has no prop for, `kit.principle` what no value says. A piece "
        "the charter lists under `withdrawn` is refused (WithdrawnByKit) "
        "with its reason. "
        "Returns composition_id, the stored composition and the piece's "
        "invariants: keep them true when you adapt it. Pose it with "
        "place_motion_clip, giving each instance its own props (the name, the "
        "title).",
        Schema({{"template",
                 Described("string",
                           "Piece name, as list_motion_templates gives it: "
                           "a shipped one (nom, carton, logo, fente...) or "
                           "a piece of the library.")},
                {"name", Described("string",
                                   "Name of the composition (default: the "
                                   "piece's).")},
                {"keep_formats",
                 Described("boolean",
                           "Keep every format the piece declares, for a film "
                           "declined in several sizes or a piece meant to go "
                           "back to the shelf; default false (the timeline's "
                           "size only).")},
                {"kit", Described("string",
                                  "A charter of the library, by name "
                                  "(list_motion_templates kits).")}},
               {"template"}, "add_motion_template arguments"),
        DispatchAddTemplate);

    add("save_motion_template",
        "Put a composition of the project on the shelf: save it in this user's "
        "motion library (a personal SQLite base on this machine) as a piece "
        "that list_motion_templates then lists in every project. Save a "
        "mechanism, not a look: neutral texts and colours as props. Node "
        "identities are removed; each project that adds the piece draws its "
        "own. A media a node plays goes through a prop: each project sets its "
        "own. The earlier versions of a piece stay in the base. Writes outside "
        "the project and does not change the project: nothing to undo. Returns "
        "the base, the version saved and the formats the piece declares.",
        Schema({{"composition_id", compositionId},
                {"template",
                 Described("string",
                           "Name of the piece: letters, digits, '-' and "
                           "'_'.")},
                {"description",
                 Described("string",
                           "What the piece is and does on screen, and its "
                           "props: what an agent chooses it by.")},
                {"use", Described("string", "When it fits.")},
                {"avoid", Described("string", "When not to use it.")},
                {"invariants",
                 "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
                 "\"description\":\"What must still hold once the piece is "
                 "adapted, a sentence each: the part of it that is the "
                 "mechanism.\"}"},
                {"origin",
                 Described("string",
                           "Where it comes from: the film it was made for, "
                           "a licence.")},
                {"overwrite",
                 Described("boolean",
                           "Save the next version of a library piece of that "
                           "name; default false.")}},
               {"composition_id", "template", "description"},
               "save_motion_template arguments"),
        DispatchSaveTemplate);

    add("remove_motion_composition",
        "Remove a motion composition from the project.",
        Schema({{"composition_id", compositionId}}, {"composition_id"},
               "remove_motion_composition arguments"),
        DispatchRemove);

    const std::string edit =
        "{\"type\":\"object\",\"description\":" +
        Quoted(
            "One edit, by `op`: replace {name?, definition? or "
            "definition_file?} replaces the whole composition; set_node {node, "
            "parent?, before?} poses a node object -- replaced where it stands "
            "when node.uid names an existing node, else inserted under "
            "`parent` before `before` (last when omitted); remove_node {node}; "
            "set_style {node, property, value?} sets one CSS property, or "
            "removes it when value is omitted or null; set_animation {node, "
            "property, animation?} sets {interpolate: {...}} or {spring: "
            "{...}}, or removes it; set_keyframe {keyframe, frame, value} "
            "moves one keyframe (its uid) to another frame and value; "
            "set_props {props} sets the composition's prop values, checked "
            "against each prop's type and bounds (null removes a value); "
            "set_variant {variant, props?} sets a named version -- the prop "
            "values it gives, checked like set_props -- or removes it when "
            "props is omitted; set_format {format, width?, height?, props?} "
            "sets a named size such as 9x16 or 16x9 with the prop values that "
            "adapt the card to it, or removes it when width and height are "
            "omitted. key {node, property, frame, value} sets the key of an "
            "animated property at a frame of the composition -- moved there if "
            "one is, added otherwise, the first one making the property "
            "animated (one key holds its value); unkey {node, property, frame} "
            "removes it (the last one removes the animation); ease {node, "
            "property, easing, frame?} sets the easing of every segment, or "
            "with `frame` of the one segment leaving the key at that frame (an "
            "entrance and an exit of different curves in one animation). " +
            std::string(kNodeReference)) +
        ",\"properties\":{\"op\":{\"type\":\"string\",\"enum\":[\"replace\","
        "\"set_node\",\"remove_node\",\"set_style\",\"set_animation\","
        "\"set_keyframe\",\"key\",\"unkey\",\"ease\",\"set_props\","
        "\"set_variant\",\"set_format\"]}},"
        "\"required\":[\"op\"]}";
    add("edit_motion_composition",
        "Edit one motion composition with a list of edits, applied in order "
        "and refused whole if one is: posing a node, setting or removing one "
        "CSS style property or one animation, moving a keyframe, setting prop "
        "values, versions or formats, or replacing the composition. An edit "
        "can name a node an earlier edit of the same call posed. Values use "
        "the vocabulary of docs/MOTION_FORMAT.md. The result's delta names the "
        "node and keyframe uids created or removed.",
        Schema({{"composition_id", compositionId},
                {"edits", "{\"type\":\"array\",\"items\":" + edit +
                              ",\"minItems\":1,\"description\":" +
                              Quoted("Edits, applied in order.") + "}"}},
               {"composition_id", "edits"},
               "edit_motion_composition arguments"),
        DispatchEdit);

    add("humanize_motion_composition",
        "Rewrite a motion composition written by a machine at the level a "
        "person would write it, without changing one pixel: a row of digit "
        "strips that reads the timecode becomes one text bound to it; a box "
        "that only places one text folds into it; letters placed one by one "
        "where the engine's own advances put them become one text cut into "
        "characters, their delays a stagger. Letters placed by other metrics "
        "stay apart, and `notes` says how far off they are. Every frame of the "
        "composition is rendered before and after to prove it (tens of seconds "
        "for a long film); with apply false, the rewritten definition is "
        "returned instead.",
        Schema({{"composition_id", compositionId},
                {"apply",
                 Described("boolean", "Apply the rewrite; default true.")}},
               {"composition_id"}, "humanize_motion_composition arguments"),
        DispatchHumanize);

    add("render_motion_frame",
        "Look at one frame of a motion composition. Returns the picture (PNG) "
        "and, as text, the geometry the engine computed for that same frame: "
        "the box of every node with its uid, and for text its lines, words and "
        "characters with their baselines.",
        Schema(
            {{"composition_id", compositionId},
             {"frame",
              Described("integer",
                        "Frame to render, from 0 to the duration "
                        "(exclusive), in the composition's own frames.")},
             {"variant", Described("string",
                                   "A version the composition declares "
                                   "(list_motion_compositions lists them).")},
             {"format",
              Described("string",
                        "A format the composition declares, such as 9x16 or "
                        "16x9.")},
             {"props", Described("object", "Prop values for this frame only.")},
             {"clip_id", Described("string",
                                   "A motion clip of the timeline, instead of "
                                   "composition_id.")},
             {"time", "{\"type\":\"object\",\"description\":" +
                          Quoted("With clip_id: a time of the timeline, exact "
                                 "{value, rate}.") +
                          ",\"properties\":{\"value\":{\"type\":\"integer\"},"
                          "\"rate\":{\"type\":\"integer\",\"minimum\":1}},"
                          "\"required\":[\"value\",\"rate\"],"
                          "\"additionalProperties\":false}"},
             {"font_dirs",
              "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
              "\"description\":" +
                  Quoted("Directories of .ttf/.otf files to register "
                         "before rendering.") +
                  "}"}},
            {}, "render_motion_frame arguments"),
        DispatchRender);

    const std::string names =
        "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
        "\"description\":";
    add("render_motion_video",
        "Render a motion composition to video files on its own, without a "
        "timeline: each version in each format (every declared one by "
        "default), frame 0 to the duration at the composition's frame rate. "
        "`preview` is true by default: every destination is checked and every "
        "frame laid out, and the files that would be written are listed; set "
        "it to false to encode. Encoding is one file at a time; files "
        "completed before a failure are kept and listed.",
        Schema(
            {{"composition_id", compositionId},
             {"output", Described("string",
                                  "Destination pattern, .mov or .mp4, "
                                  "with {variant} and {format}; a server "
                                  "takes a relative one from the project's "
                                  "folder.")},
             {"variants",
              names +
                  Quoted("Versions to render, in order; default: all "
                         "the composition declares.") +
                  "}"},
             {"formats", names +
                             Quoted("Formats to render, in order; default: all "
                                    "the composition declares.") +
                             "}"},
             {"alpha", Described("boolean",
                                 "ProRes 4444 with alpha; default false "
                                 "(H.264 over black).")},
             {"overwrite",
              Described("boolean", "Replace existing files; default false.")},
             {"preview",
              Described("boolean", "Check and list only; default true.")},
             {"loudness",
              Described("number",
                        "Integrated loudness to deliver at, in LUFS (-16 web, "
                        "-23 broadcast), limited under true_peak; default: the "
                        "mix as it sums.")},
             {"true_peak",
              Described("number",
                        "With loudness: the true-peak ceiling, dBTP; "
                        "default -1.")},
             {"font_dirs",
              names +
                  Quoted("Directories of .ttf/.otf files to register "
                         "before rendering.") +
                  "}"}},
            {"composition_id", "output"}, "render_motion_video arguments"),
        DispatchVideo);

    add("export_motion_after",
        "Export a motion composition to After Effects as a project a motion "
        "designer can retouch: precompositions, layers, keyframes and "
        "expressions, never baked frames. Each sequence becomes a numbered, "
        "named precomposition; texts stay text layers in their font; "
        "cascades by unit become text animators driven by sliders. The "
        "engine describes the composition, then After builds and saves the "
        ".aep through the bridge. After must be running with an empty, "
        "untitled project: a project with work in it is refused "
        "(ProjectAlreadyOpen), never closed. `timeline_id` adds the sounds "
        "of that timeline as audio layers. What After cannot show the same "
        "way is listed in `notes` (slice() as Wave Warp, springs). A row "
        "of texts keeps its flexbox push by expressions (sourceRectAtTime). "
        "With build false, only the description is "
        "written, to description_path. After is an output: nothing comes "
        "back into the project.",
        Schema(
            {{"composition_id", compositionId},
             {"project_path",
              Described("string",
                        "Absolute path of the new .aep; never overwritten.")},
             {"timeline_id",
              Described("string",
                        "A timeline whose sound goes under the pictures: its "
                        "audio tracks as they play (mute, solo), on the same "
                        "stretches, gains and fades.")},
             {"name", Described("string",
                                "Name of the main composition in After "
                                "(default: the composition's).")},
             {"props", Described("object", "Prop values for the export.")},
             {"font_dirs",
              names +
                  Quoted("Directories of .ttf/.otf files to register "
                         "first.") +
                  "}"},
             {"description_path",
              Described("string",
                        "Absolute .json path to keep the after-motion.v1 "
                        "description at; never overwritten. Required "
                        "without build.")},
             {"build", Described("boolean",
                                 "Build the project in After; default "
                                 "true.")},
             {"boil",
              names +
                  Quoted("Ids of nodes that boil (a texture jumping every n "
                         "frames): their translateX/translateY held at a "
                         "constant step become a posterizeTime + random "
                         "expression a designer can retune, instead of one "
                         "hold keyframe per jump. Inexact by design, and "
                         "listed in `notes`; a node without such a track is "
                         "refused.") +
                  "}"}},
            {"composition_id", "project_path"},
            "export_motion_after arguments"),
        DispatchExportAfter);

    add("import_after_comp",
        "Import a composition of the project open in After Effects into this "
        "project as a motion composition you can read and edit: one node per "
        "layer, named after it (texts stay texts, in their face, on their "
        "baseline; shape rectangles become boxes; parents become boxes "
        "around their children), keys with their eases as cubic-bezier per "
        "segment. What After computes and the format cannot say (an "
        "expression, a curved path, a track matte made of one rectangle) is "
        "read frame by frame and stops being live; what has no equivalent "
        "(precompositions, footage, masks, text animators, other effects, "
        "blending modes) is left out. Both are listed in `notes`: read them "
        "before using the composition. Nothing is written to the .aep. A "
        "face this machine lacks can be replaced (`fonts`), as After's own "
        "font replacement; each replacement is a note.",
        Schema({{"comp_id", Described("string",
                                      "Host ID of the After composition "
                                      "(after_list_comps).")},
                {"name", Described("string",
                                   "Name in the project (default: the After "
                                   "composition's).")},
                {"fonts",
                 Described("object",
                           "PostScript name read in After -> PostScript name "
                           "to draw instead.")},
                {"font_dirs",
                 names +
                     Quoted("Directories of .ttf/.otf files to register "
                            "first.") +
                     "}"},
                {"reading_path",
                 Described("string",
                           "Absolute .json path to keep what the bridge read "
                           "(after-comp.v1), to replay with "
                           "--import-after-comp; never overwritten.")}},
               {"comp_id"}, "import_after_comp arguments"),
        DispatchImportAfter);

    add("verify_after_export",
        "Check an After Effects project export_motion_after built against "
        "the engine, frame by frame: After renders each frame through the "
        "bridge (the project must be open in After, as the export leaves "
        "it), the engine renders the same frame, and each pair is compared "
        "by mean luma difference (levels 0-255, over black). A frame over "
        "the tolerance is `announced` when a note of the export's report "
        "about a node drawn at that frame explains it (slice() only while "
        "it cuts), `unexplained` otherwise: that is what to look at. Notes "
        "about the whole composition are listed apart and never explain a "
        "frame. Both pictures of every frame stay in image_dir "
        "(moteur-NNNNNN.png, after-NNNNNN.png). Pass the props, font_dirs "
        "and boil the export was given.",
        Schema(
            {{"composition_id", compositionId},
             {"comp_id",
              Described("string",
                        "Host ID of the After composition the export built "
                        "(export_motion_after's after.result.main_comp_id, "
                        "or after_list_comps).")},
             {"image_dir",
              Described("string",
                        "Absolute path of a new directory for the pictures; "
                        "an existing one is refused.")},
             {"frames",
              "{\"type\":\"array\",\"items\":{\"type\":\"integer\"},"
              "\"description\":" +
                  Quoted("Frames to check, each within the composition.") +
                  "}"},
             {"step", Described("integer",
                                "Without frames: every step-th frame from "
                                "0; default 8.")},
             {"tolerance",
              Described("number",
                        "Mean luma difference a frame may have, in levels; "
                        "default 0.5.")},
             {"props", Described("object", "Prop values the export had.")},
             {"font_dirs",
              names +
                  Quoted("Directories of .ttf/.otf files to register "
                         "first.") +
                  "}"},
             {"boil", names + Quoted("The boil the export was given.") + "}"}},
            {"composition_id", "comp_id", "image_dir"},
            "verify_after_export arguments"),
        DispatchVerifyAfter);
}
