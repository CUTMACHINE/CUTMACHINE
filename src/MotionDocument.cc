#include "MotionDocument.h"

#include <algorithm>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

MotionTreeStep Key(std::string key) { return {std::move(key), 0, false}; }

MotionTreeStep Index(size_t index) { return {std::string(), index, true}; }

MotionTreePath Then(MotionTreePath path, MotionTreeStep step) {
    path.push_back(std::move(step));
    return path;
}

// Rebuilds `node` with `leaf` applied at `path[depth]`: `leaf` receives the
// current value, or null when the key is absent, and returns the new one,
// or nothing to drop the key.
Value Rebuild(const Value& node, const MotionTreePath& path, size_t depth,
              const std::function<std::optional<Value>(const Value*)>& leaf) {
    if (depth == path.size()) {
        std::optional<Value> replaced = leaf(&node);
        return replaced ? *replaced : Value();
    }
    const MotionTreeStep& step = path[depth];
    const bool last = depth + 1 == path.size();
    if (step.is_index) {
        Value copy = Value::MakeArray();
        if (!node.IsArray()) {
            return node;
        }
        for (size_t index = 0; index < node.AsArray().size(); ++index) {
            const Value& element = node.AsArray()[index];
            if (index != step.index) {
                copy.Push(element);
            } else if (!last) {
                copy.Push(Rebuild(element, path, depth + 1, leaf));
            } else if (std::optional<Value> replaced = leaf(&element)) {
                copy.Push(std::move(*replaced));
            }
        }
        return copy;
    }
    if (!node.IsObject()) {
        return node;
    }
    Value copy = Value::MakeObject();
    bool found = false;
    for (const auto& [key, child] : node.AsObject()) {
        if (key != step.key) {
            copy.Set(key, child);
            continue;
        }
        found = true;
        if (!last) {
            copy.Set(key, Rebuild(child, path, depth + 1, leaf));
        } else if (std::optional<Value> replaced = leaf(&child)) {
            copy.Set(key, std::move(*replaced));
        }
    }
    if (!found && last) {
        if (std::optional<Value> added = leaf(nullptr)) {
            copy.Set(step.key, std::move(*added));
        }
    }
    return copy;
}

// Visits every node below and including `node`, children in order, then a
// repeat's template.
void WalkNodes(
    const Value& node, const MotionTreePath& path, const MotionTreePath& parent,
    bool isRoot, bool isTemplate,
    const std::function<bool(const Value&, const MotionNodeLocation&)>& visit) {
    if (!node.IsObject()) {
        return;
    }
    MotionNodeLocation location{path, parent, isRoot, isTemplate};
    if (!visit(node, location)) {
        return;
    }
    if (const Value* children = node.Find("children");
        children != nullptr && children->IsArray()) {
        for (size_t index = 0; index < children->AsArray().size(); ++index) {
            WalkNodes(children->AsArray()[index],
                      Then(Then(path, Key("children")), Index(index)), path,
                      false, false, visit);
        }
    }
    if (const Value* templ = node.Find("template");
        templ != nullptr && templ->IsObject()) {
        WalkNodes(*templ, Then(path, Key("template")), path, false, true,
                  visit);
    }
}

void WalkDefinition(
    const Value& definition,
    const std::function<bool(const Value&, const MotionNodeLocation&)>& visit) {
    if (const Value* root = definition.Find("root")) {
        WalkNodes(*root, {Key("root")}, {}, true, false, visit);
    }
}

// Every interpolation of one node, with its path.
void WalkInterpolations(
    const Value& node, const MotionTreePath& path,
    const std::function<void(const Value&, const MotionTreePath&)>& visit) {
    const Value* animate = node.Find("animate");
    if (animate == nullptr || !animate->IsObject()) {
        return;
    }
    for (const auto& [property, animation] : animate->AsObject()) {
        const Value* interpolate = animation.Find("interpolate");
        if (interpolate != nullptr && interpolate->IsObject()) {
            visit(*interpolate,
                  Then(Then(Then(path, Key("animate")), Key(property)),
                       Key("interpolate")));
        }
    }
}

// `object` rebuilt with `key` set to `value` right after `after` when it has
// no such key yet (at the end when it has no `after` either), so that a
// generated identity reads where a person would look for it.
Value WithKeyAfter(const Value& object, const std::string& after,
                   const std::string& key, Value value) {
    if (object.Find(key) != nullptr || object.Find(after) == nullptr) {
        Value copy = object;
        if (copy.Find(key) == nullptr) {
            copy.Set(key, std::move(value));
        }
        return copy;
    }
    Value copy = Value::MakeObject();
    for (const auto& [name, child] : object.AsObject()) {
        copy.Set(name, child);
        if (name == after) {
            copy.Set(key, value);
        }
    }
    return copy;
}

Value WithKeyframeIdentities(const Value& interpolate) {
    if (!interpolate.IsObject() || interpolate.Find("uids") != nullptr) {
        return interpolate;
    }
    const Value* frames = interpolate.Find("frames");
    if (frames == nullptr || !frames->IsArray()) {
        return interpolate;
    }
    Value uids = Value::MakeArray();
    for (size_t index = 0; index < frames->AsArray().size(); ++index) {
        uids.Push(Value::MakeString(GenerateUlid()));
    }
    return WithKeyAfter(interpolate, "values", "uids", std::move(uids));
}

bool IsString(const Value* value) {
    return value != nullptr && value->IsString();
}

}  // namespace

const Value* MotionTreeAt(const Value& root, const MotionTreePath& path) {
    const Value* cursor = &root;
    for (const MotionTreeStep& step : path) {
        if (step.is_index) {
            if (!cursor->IsArray() || step.index >= cursor->AsArray().size()) {
                return nullptr;
            }
            cursor = &cursor->AsArray()[step.index];
        } else {
            cursor = cursor->Find(step.key);
            if (cursor == nullptr) {
                return nullptr;
            }
        }
    }
    return cursor;
}

Value MotionTreeWith(const Value& root, const MotionTreePath& path,
                     const std::optional<Value>& replacement) {
    return Rebuild(root, path, 0, [&](const Value*) { return replacement; });
}

Value MotionTreeInsert(const Value& root, const MotionTreePath& arrayPath,
                       size_t before, const Value& value) {
    return Rebuild(root, arrayPath, 0,
                   [&](const Value* array) -> std::optional<Value> {
                       Value copy = Value::MakeArray();
                       const size_t size = array != nullptr && array->IsArray()
                                               ? array->AsArray().size()
                                               : 0;
                       for (size_t index = 0; index < size; ++index) {
                           if (index == before) {
                               copy.Push(value);
                           }
                           copy.Push(array->AsArray()[index]);
                       }
                       if (before >= size) {
                           copy.Push(value);
                       }
                       return copy;
                   });
}

Value MotionTreeErase(const Value& root, const MotionTreePath& path) {
    return Rebuild(root, path, 0,
                   [](const Value*) { return std::optional<Value>(); });
}

bool FindMotionNode(const Value& definition, const Ulid& uid,
                    MotionNodeLocation& location) {
    bool found = false;
    WalkDefinition(
        definition, [&](const Value& node, const MotionNodeLocation& at) {
            if (found) {
                return false;
            }
            const Value* candidate = node.Find("uid");
            if (IsString(candidate) && candidate->AsString() == uid) {
                location = at;
                found = true;
                return false;
            }
            return true;
        });
    return found;
}

bool FindMotionNodeByName(const Value& definition, const std::string& name,
                          MotionNodeLocation& location) {
    bool found = false;
    WalkDefinition(
        definition, [&](const Value& node, const MotionNodeLocation& at) {
            if (found) {
                return false;
            }
            const Value* candidate = node.Find("id");
            if (IsString(candidate) && candidate->AsString() == name) {
                location = at;
                found = true;
                return false;
            }
            return true;
        });
    return found;
}

bool FindMotionKeyframe(const Value& definition, const Ulid& uid,
                        MotionKeyframeLocation& location) {
    bool found = false;
    WalkDefinition(definition, [&](const Value& node,
                                   const MotionNodeLocation& at) {
        WalkInterpolations(
            node, at.path,
            [&](const Value& interpolate, const MotionTreePath& path) {
                const Value* uids = interpolate.Find("uids");
                if (found || uids == nullptr || !uids->IsArray()) {
                    return;
                }
                for (size_t index = 0; index < uids->AsArray().size();
                     ++index) {
                    const Value& candidate = uids->AsArray()[index];
                    if (candidate.IsString() && candidate.AsString() == uid) {
                        location = {path, index};
                        found = true;
                        return;
                    }
                }
            });
        return !found;
    });
    return found;
}

Value WithAnimationIdentities(const Value& animation) {
    const Value* interpolate =
        animation.IsObject() ? animation.Find("interpolate") : nullptr;
    if (interpolate == nullptr) {
        return animation;
    }
    Value identified = animation;
    identified.Set("interpolate", WithKeyframeIdentities(*interpolate));
    return identified;
}

Value WithMotionIdentities(const Value& node) {
    if (!node.IsObject()) {
        return node;
    }
    Value copy = node.Find("uid") == nullptr
                     ? WithKeyAfter(node, "type", "uid",
                                    Value::MakeString(GenerateUlid()))
                     : node;
    Value rebuilt = Value::MakeObject();
    for (const auto& [key, child] : copy.AsObject()) {
        if (key == "children" && child.IsArray()) {
            Value children = Value::MakeArray();
            for (const Value& element : child.AsArray()) {
                children.Push(WithMotionIdentities(element));
            }
            rebuilt.Set(key, std::move(children));
        } else if (key == "template") {
            rebuilt.Set(key, WithMotionIdentities(child));
        } else if (key == "animate" && child.IsObject()) {
            Value animate = Value::MakeObject();
            for (const auto& [property, animation] : child.AsObject()) {
                animate.Set(property, WithAnimationIdentities(animation));
            }
            rebuilt.Set(key, std::move(animate));
        } else {
            rebuilt.Set(key, child);
        }
    }
    return rebuilt;
}

Value WithDefinitionIdentities(const Value& definition) {
    const Value* root = definition.Find("root");
    if (root == nullptr) {
        return definition;
    }
    return MotionTreeWith(definition, {Key("root")},
                          WithMotionIdentities(*root));
}

namespace {

Value WithoutKey(const Value& object, const std::string& removed) {
    Value copy = Value::MakeObject();
    for (const auto& [key, child] : object.AsObject()) {
        if (key != removed) {
            copy.Set(key, child);
        }
    }
    return copy;
}

// Walks what WithMotionIdentities walks, so that one undoes the other.
Value WithoutMotionIdentities(const Value& node) {
    if (!node.IsObject()) {
        return node;
    }
    Value rebuilt = Value::MakeObject();
    for (const auto& [key, child] : node.AsObject()) {
        if (key == "uid") {
            continue;
        }
        if (key == "children" && child.IsArray()) {
            Value children = Value::MakeArray();
            for (const Value& element : child.AsArray()) {
                children.Push(WithoutMotionIdentities(element));
            }
            rebuilt.Set(key, std::move(children));
        } else if (key == "template") {
            rebuilt.Set(key, WithoutMotionIdentities(child));
        } else if (key == "animate" && child.IsObject()) {
            Value animate = Value::MakeObject();
            for (const auto& [property, animation] : child.AsObject()) {
                const Value* interpolate = animation.IsObject()
                                               ? animation.Find("interpolate")
                                               : nullptr;
                if (interpolate == nullptr || !interpolate->IsObject()) {
                    animate.Set(property, animation);
                    continue;
                }
                Value anonymous = animation;
                anonymous.Set("interpolate", WithoutKey(*interpolate, "uids"));
                animate.Set(property, std::move(anonymous));
            }
            rebuilt.Set(key, std::move(animate));
        } else {
            rebuilt.Set(key, child);
        }
    }
    return rebuilt;
}

}  // namespace

Value WithoutDefinitionIdentities(const Value& definition) {
    const Value* root = definition.Find("root");
    if (root == nullptr) {
        return definition;
    }
    return MotionTreeWith(definition, {Key("root")},
                          WithoutMotionIdentities(*root));
}

void MotionDefinitionIdentities(const Value& definition,
                                std::vector<Ulid>& nodeUids,
                                std::vector<Ulid>& keyframeUids) {
    WalkDefinition(
        definition, [&](const Value& node, const MotionNodeLocation& at) {
            const Value* uid = node.Find("uid");
            if (IsString(uid)) {
                nodeUids.push_back(uid->AsString());
            }
            WalkInterpolations(
                node, at.path,
                [&](const Value& interpolate, const MotionTreePath&) {
                    const Value* uids = interpolate.Find("uids");
                    if (uids == nullptr || !uids->IsArray()) {
                        return;
                    }
                    for (const Value& id : uids->AsArray()) {
                        if (id.IsString()) {
                            keyframeUids.push_back(id.AsString());
                        }
                    }
                });
            return true;
        });
}

std::vector<Ulid> CompositionIdentities(
    const DocumentComposition& composition) {
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(composition.definition, nodes, keyframes);
    std::vector<Ulid> identities{composition.id};
    identities.insert(identities.end(), nodes.begin(), nodes.end());
    identities.insert(identities.end(), keyframes.begin(), keyframes.end());
    return identities;
}

bool ParseDocumentComposition(const DocumentComposition& composition,
                              MotionComposition& parsed, MotionError& error) {
    return ParseMotionComposition(composition.definition, parsed, error);
}

bool ValidateDocumentComposition(const DocumentComposition& composition,
                                 MotionError& error) {
    const auto fail = [&](const std::string& path, const std::string& message) {
        error = {"InvalidComposition", path, message};
        return false;
    };
    if (composition.name.empty() || composition.name.size() > 256) {
        return fail("name", "a composition has a name of 1 to 256 bytes");
    }
    for (const unsigned char byte : composition.name) {
        if (byte < 0x20 || byte == 0x7f) {
            return fail("name",
                        "a composition name holds no control character");
        }
    }
    if (!composition.definition.IsObject()) {
        return fail("definition", "a definition is a JSON object");
    }
    // Identities first: a node the document cannot address is refused even
    // when the rest would evaluate.
    std::string missing;
    WalkDefinition(composition.definition, [&](const Value& node,
                                               const MotionNodeLocation& at) {
        if (!missing.empty()) {
            return false;
        }
        std::string where;
        for (const MotionTreeStep& step : at.path) {
            where += step.is_index ? "[" + std::to_string(step.index) + "]"
                                   : (where.empty() ? "" : ".") + step.key;
        }
        if (node.Find("uid") == nullptr) {
            missing = where + ".uid";
            return false;
        }
        WalkInterpolations(
            node, at.path,
            [&](const Value& interpolate, const MotionTreePath& path) {
                if (!missing.empty() || interpolate.Find("uids") != nullptr) {
                    return;
                }
                missing = where + ".animate." + path[path.size() - 2].key +
                          ".interpolate.uids";
            });
        return missing.empty();
    });
    if (!missing.empty()) {
        return fail(missing,
                    "every node of a stored composition has a uid, and every "
                    "interpolation the uids of its keyframes");
    }
    MotionComposition parsed;
    return ParseDocumentComposition(composition, parsed, error);
}

std::string MotionCompositionJson(const DocumentComposition& composition) {
    Value root = Value::MakeObject();
    root.Set("id", Value::MakeString(composition.id));
    root.Set("name", Value::MakeString(composition.name));
    root.Set("definition", composition.definition);
    return root.Dump();
}

bool MotionDefinitionWithSetEntry(const Value& definition,
                                  const std::string& section,
                                  const std::string& name,
                                  const std::optional<Value>& value,
                                  Value& output) {
    const Value* existing = definition.Find(section);
    const bool present = existing != nullptr && existing->IsObject() &&
                         existing->Find(name) != nullptr;
    if (!value) {
        if (!present) {
            return false;
        }
        const bool last = existing->AsObject().size() == 1;
        output = last ? MotionTreeWith(definition, {{section, 0, false}},
                                       std::nullopt)
                      : MotionTreeWith(definition,
                                       {{section, 0, false}, {name, 0, false}},
                                       std::nullopt);
        return true;
    }
    if (existing != nullptr) {
        output = MotionTreeWith(definition,
                                {{section, 0, false}, {name, 0, false}}, value);
        return true;
    }
    // A new section reads before the tree, beside the props it sets.
    Value entries = Value::MakeObject();
    entries.Set(name, *value);
    output = Value::MakeObject();
    bool placed = false;
    for (const auto& [key, member] : definition.AsObject()) {
        if (key == "root" && !placed) {
            output.Set(section, entries);
            placed = true;
        }
        output.Set(key, member);
    }
    if (!placed) {
        output.Set(section, std::move(entries));
    }
    return true;
}

std::string MotionCompositionListJson(
    const std::vector<DocumentComposition>& compositions) {
    Value list = Value::MakeArray();
    for (const DocumentComposition& composition : compositions) {
        Value entry = Value::MakeObject();
        entry.Set("id", Value::MakeString(composition.id));
        entry.Set("name", Value::MakeString(composition.name));
        for (const char* key : {"width", "height", "fps", "duration"}) {
            if (const Value* value = composition.definition.Find(key)) {
                entry.Set(key, *value);
            }
        }
        Value props = Value::MakeArray();
        if (const Value* declared = composition.definition.Find("props");
            declared != nullptr && declared->IsObject()) {
            for (const auto& [name, definition] : declared->AsObject()) {
                Value prop = Value::MakeObject();
                prop.Set("name", Value::MakeString(name));
                if (const Value* type = definition.Find("type")) {
                    prop.Set("type", *type);
                }
                props.Push(std::move(prop));
            }
        }
        entry.Set("props", std::move(props));
        // M6 -- what a batch render can name: versions, and formats with
        // their sizes.
        Value variants = Value::MakeArray();
        if (const Value* declared = composition.definition.Find("variants");
            declared != nullptr && declared->IsObject()) {
            for (const auto& item : declared->AsObject()) {
                variants.Push(Value::MakeString(item.first));
            }
        }
        entry.Set("variants", std::move(variants));
        Value formats = Value::MakeArray();
        if (const Value* declared = composition.definition.Find("formats");
            declared != nullptr && declared->IsObject()) {
            for (const auto& [name, definition] : declared->AsObject()) {
                Value format = Value::MakeObject();
                format.Set("name", Value::MakeString(name));
                for (const char* key : {"width", "height"}) {
                    if (const Value* value = definition.Find(key)) {
                        format.Set(key, *value);
                    }
                }
                formats.Push(std::move(format));
            }
        }
        entry.Set("formats", std::move(formats));
        list.Push(std::move(entry));
    }
    Value root = Value::MakeObject();
    root.Set("compositions", std::move(list));
    return root.Dump();
}

void MotionDefinitionReferences(const Value& definition,
                                std::vector<Ulid>& media,
                                std::vector<Ulid>& compositions) {
    WalkDefinition(
        definition, [&](const Value& node, const MotionNodeLocation&) {
            const Value* type = node.Find("type");
            if (!IsString(type)) {
                return true;
            }
            if (type->AsString() == "video" || type->AsString() == "audio") {
                const Value* id = node.Find("media");
                if (IsString(id)) {
                    media.push_back(id->AsString());
                }
            } else if (type->AsString() == "composition") {
                const Value* id = node.Find("composition");
                if (IsString(id)) {
                    compositions.push_back(id->AsString());
                }
            }
            return true;
        });
}

std::vector<const DocumentComposition*> CompositionDependencies(
    const std::vector<DocumentComposition>& compositions, const Ulid& id) {
    std::vector<const DocumentComposition*> reached;
    std::set<Ulid> seen{id};
    std::vector<Ulid> pending{id};
    while (!pending.empty()) {
        const Ulid current = pending.back();
        pending.pop_back();
        const auto found =
            std::find_if(compositions.begin(), compositions.end(),
                         [&](const DocumentComposition& entry) {
                             return entry.id == current;
                         });
        if (found == compositions.end()) {
            continue;
        }
        if (current != id) {
            reached.push_back(&*found);
        }
        std::vector<Ulid> media;
        std::vector<Ulid> shown;
        MotionDefinitionReferences(found->definition, media, shown);
        for (auto next = shown.rbegin(); next != shown.rend(); ++next) {
            if (seen.insert(*next).second) {
                pending.push_back(*next);
            }
        }
    }
    return reached;
}

bool BuildMotionLibrary(const std::vector<DocumentComposition>& compositions,
                        const Ulid& id, MotionLibrary& library,
                        MotionError& error) {
    library.compositions.clear();
    for (const DocumentComposition* dependency :
         CompositionDependencies(compositions, id)) {
        MotionComposition parsed;
        if (!ParseDocumentComposition(*dependency, parsed, error)) {
            error.message = "composition '" + dependency->id +
                            "', shown by an instance: " + error.message;
            return false;
        }
        library.compositions.emplace(dependency->id, std::move(parsed));
    }
    return true;
}

std::map<std::string, std::string> MotionMediaPaths(const Document& document) {
    std::map<std::string, std::string> paths;
    for (const DocumentSource& source : document.sources) {
        paths[source.id] = source.path;
    }
    for (const LibraryMedia& media : document.library) {
        paths[media.id] = media.path;
    }
    return paths;
}

bool ValidateCompositionReferences(
    const std::vector<DocumentComposition>& compositions,
    const std::set<Ulid>& mediaIds, MotionError& error) {
    std::map<Ulid, const DocumentComposition*> byId;
    for (const DocumentComposition& composition : compositions) {
        byId.emplace(composition.id, &composition);
    }
    std::map<Ulid, std::optional<MotionComposition>> parsed;
    const auto parse = [&](const Ulid& id) -> const MotionComposition* {
        auto found = parsed.find(id);
        if (found == parsed.end()) {
            std::optional<MotionComposition> composition;
            const auto stored = byId.find(id);
            MotionComposition candidate;
            MotionError ignored;
            if (stored != byId.end() &&
                ParseDocumentComposition(*stored->second, candidate, ignored)) {
                composition = std::move(candidate);
            }
            found = parsed.emplace(id, std::move(composition)).first;
        }
        return found->second ? &*found->second : nullptr;
    };
    for (const DocumentComposition& composition : compositions) {
        bool ok = true;
        WalkDefinition(composition.definition, [&](const Value& node,
                                                   const MotionNodeLocation&) {
            const Value* type = node.Find("type");
            const Value* uid = node.Find("uid");
            const std::string where =
                "node '" + (IsString(uid) ? uid->AsString() : std::string()) +
                "'";
            if (!ok || !IsString(type)) {
                return false;
            }
            if (type->AsString() == "video" || type->AsString() == "audio") {
                const Value* id = node.Find("media");
                if (IsString(id) && mediaIds.count(id->AsString()) == 0) {
                    error = {
                        "UnknownMedia", where + ".media",
                        "the project has no media '" + id->AsString() + "'"};
                    ok = false;
                }
                return ok;
            }
            if (type->AsString() != "composition") {
                return true;
            }
            const Value* id = node.Find("composition");
            if (!IsString(id)) {
                return true;  // the parser refuses it, with its path
            }
            if (byId.count(id->AsString()) == 0) {
                error = {
                    "UnknownComposition", where + ".composition",
                    "the project has no composition '" + id->AsString() + "'"};
                ok = false;
                return false;
            }
            const MotionComposition* target = parse(id->AsString());
            const Value* props = node.Find("props");
            if (target == nullptr || props == nullptr || !props->IsObject()) {
                return true;
            }
            for (const auto& entry : props->AsObject()) {
                const std::string& name = entry.first;
                const Value& value = entry.second;
                const std::string at = where + ".props." + name;
                const auto definition =
                    std::find_if(target->props.begin(), target->props.end(),
                                 [&](const MotionPropDefinition& prop) {
                                     return prop.name == name;
                                 });
                if (definition == target->props.end()) {
                    error = {"UnknownProp", at,
                             "composition '" + id->AsString() +
                                 "' declares no prop '" + name + "'"};
                    ok = false;
                    return false;
                }
                // An object is a binding, checked when a frame resolves it.
                if (!value.IsObject() &&
                    !ValidateMotionPropValue(*definition, value, at, error)) {
                    ok = false;
                    return false;
                }
            }
            return true;
        });
        if (!ok) {
            error.message =
                "composition '" + composition.id + "': " + error.message;
            return false;
        }
    }
    // No composition shows itself: a depth-first walk of the instance graph.
    std::map<Ulid, int> state;  // 1 on the path, 2 done
    std::function<bool(const Ulid&, std::vector<Ulid>&)> visit =
        [&](const Ulid& id, std::vector<Ulid>& trail) {
            state[id] = 1;
            trail.push_back(id);
            std::vector<Ulid> media;
            std::vector<Ulid> shown;
            MotionDefinitionReferences(byId.at(id)->definition, media, shown);
            for (const Ulid& next : shown) {
                if (byId.count(next) == 0) {
                    continue;
                }
                if (state[next] == 1) {
                    std::string cycle;
                    bool inCycle = false;
                    for (const Ulid& step : trail) {
                        inCycle = inCycle || step == next;
                        if (inCycle) {
                            cycle += step + " > ";
                        }
                    }
                    error = {"CompositionCycle", "composition '" + next + "'",
                             "compositions show one another in a cycle: " +
                                 cycle + next};
                    return false;
                }
                if (state[next] == 0 && !visit(next, trail)) {
                    return false;
                }
            }
            trail.pop_back();
            state[id] = 2;
            return true;
        };
    for (const DocumentComposition& composition : compositions) {
        std::vector<Ulid> trail;
        if (state[composition.id] == 0 && !visit(composition.id, trail)) {
            return false;
        }
    }
    return true;
}

bool MotionReachFor(const std::vector<DocumentComposition>& compositions,
                    const std::vector<DocumentSource>& sources,
                    const std::vector<LibraryMedia>& library, const Ulid& id,
                    MotionReach& reach, MotionError& error) {
    auto parsed = std::make_shared<MotionLibrary>();
    if (!BuildMotionLibrary(compositions, id, *parsed, error)) {
        return false;
    }
    reach.composition_id = id;
    reach.library = std::move(parsed);
    reach.media_paths.clear();
    for (const DocumentSource& source : sources) {
        reach.media_paths[source.id] = source.path;
    }
    for (const LibraryMedia& media : library) {
        reach.media_paths[media.id] = media.path;
    }
    return true;
}

bool MotionReachFor(const Document& document, const Ulid& id,
                    MotionReach& reach, MotionError& error) {
    return MotionReachFor(document.compositions, document.sources,
                          document.library, id, reach, error);
}

std::string MotionReachKey(const Document& document, const Ulid& id) {
    std::string key;
    std::vector<const DocumentComposition*> reached =
        CompositionDependencies(document.compositions, id);
    std::set<Ulid> media;
    bool bound = false;
    const DocumentComposition* self = document.FindComposition(id);
    if (self != nullptr) {
        reached.insert(reached.begin(), self);
    }
    for (const DocumentComposition* composition : reached) {
        if (composition != self) {
            key +=
                composition->id + '\n' + composition->definition.Dump() + '\n';
        }
        WalkDefinition(composition->definition, [&](const Value& node,
                                                    const MotionNodeLocation&) {
            const Value* type = node.Find("type");
            if (IsString(type) &&
                (type->AsString() == "video" || type->AsString() == "audio")) {
                const Value* ref = node.Find("media");
                if (IsString(ref)) {
                    media.insert(ref->AsString());
                } else if (ref != nullptr) {
                    bound = true;
                }
            }
            return true;
        });
    }
    const std::map<std::string, std::string> paths = MotionMediaPaths(document);
    for (const auto& entry : paths) {
        if (bound || media.count(entry.first) != 0) {
            key += entry.first + '=' + entry.second + '\n';
        }
    }
    return key;
}
