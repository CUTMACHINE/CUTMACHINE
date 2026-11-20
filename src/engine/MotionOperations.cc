#include "MotionOperations.h"

#include "MotionDocument.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <utility>
#include <variant>

namespace {

using mcp_json::Value;

bool Refuse(EditError code, const std::string& text, EditError& error,
            std::string& message) {
    error = code;
    message = text;
    return false;
}

MotionTreePath Then(MotionTreePath path, const std::string& key) {
    path.push_back({key, 0, false});
    return path;
}

DocumentComposition* Composition(Document& candidate, const Ulid& id,
                                 EditError& error, std::string& message) {
    DocumentComposition* found = candidate.FindComposition(id);
    if (found == nullptr) {
        Refuse(EditError::UnknownComposition,
               "unknown composition_id '" + id + "'", error, message);
    }
    return found;
}

bool Node(const DocumentComposition& composition, const Ulid& uid,
          MotionNodeLocation& location, EditError& error,
          std::string& message) {
    if (FindMotionNode(composition.definition, uid, location)) {
        return true;
    }
    return Refuse(
        EditError::UnknownMotionNode,
        "composition '" + composition.id + "' has no node '" + uid + "'", error,
        message);
}

// The composition's new state is judged whole -- the motion parser for what
// it says, the document for its identities -- before anything is kept.
bool Judge(const Document& candidate, const DocumentComposition& composition,
           EditError& error, std::string& message) {
    MotionError motion;
    if (!ValidateDocumentComposition(composition, motion)) {
        MotionRefusal(motion, error, message);
        return false;
    }
    std::string validation;
    if (candidate.Validate(validation)) {
        return true;
    }
    // REMOTION-PARITY-2026-09 REM2 -- what the composition names in the
    // project, refused by name (ValidateCompositionReferences writes the
    // motion code first); a cycle stays InvalidMotion, its code in the
    // message, like every other refusal of the format.
    const auto starts = [&](const char* code) {
        return validation.rfind(code, 0) == 0;
    };
    if (starts("UnknownMedia")) {
        return Refuse(EditError::UnknownMedia, validation, error, message);
    }
    if (starts("UnknownComposition")) {
        return Refuse(EditError::UnknownComposition, validation, error,
                      message);
    }
    if (starts("UnknownProp")) {
        return Refuse(EditError::UnknownProp, validation, error, message);
    }
    if (starts("PropOutOfRange")) {
        return Refuse(EditError::PropOutOfRange, validation, error, message);
    }
    if (starts("PropTypeMismatch")) {
        return Refuse(EditError::PropTypeMismatch, validation, error, message);
    }
    if (starts("CompositionCycle")) {
        return Refuse(EditError::InvalidMotion, validation, error, message);
    }
    return Refuse(validation.find("duplicate ID") != std::string::npos
                      ? EditError::DuplicateId
                      : EditError::ValidationFailed,
                  validation, error, message);
}

// Every edit inside a composition keeps the whole previous state as its
// inverse (Operations.h says why).
bool Replace(Document& candidate, DocumentComposition& target, Value definition,
             Operation& inverse, EditError& error, std::string& message) {
    const DocumentComposition before = target;
    target.definition = std::move(definition);
    if (!Judge(candidate, target, error, message)) {
        return false;
    }
    inverse = UpdateMotionCompositionOperation{before};
    return true;
}

bool ApplyAdd(Document& candidate, AddMotionCompositionOperation& operation,
              Operation& inverse, EditError& error, std::string& message) {
    DocumentComposition& composition = operation.composition;
    if (composition.id.empty()) {
        composition.id = GenerateUlid();
    }
    if (!IsValidUlid(composition.id) ||
        candidate.FindComposition(composition.id) != nullptr) {
        return Refuse(EditError::DuplicateId,
                      "composition id is invalid or already exists: '" +
                          composition.id + "'",
                      error, message);
    }
    if (!composition.definition.IsObject()) {
        return Refuse(EditError::InvalidMotion,
                      "InvalidComposition at definition: a definition is a "
                      "JSON object",
                      error, message);
    }
    composition.definition = WithDefinitionIdentities(composition.definition);
    auto& list = candidate.compositions;
    if (operation.insertion_index < -1 ||
        (operation.insertion_index >= 0 &&
         static_cast<uint64_t>(operation.insertion_index) > list.size())) {
        return Refuse(EditError::InvalidOperation,
                      "composition insertion_index is outside the list", error,
                      message);
    }
    if (operation.insertion_index < 0) {
        operation.insertion_index = static_cast<int64_t>(list.size());
    }
    list.insert(list.begin() + operation.insertion_index, composition);
    if (!Judge(candidate, list[static_cast<size_t>(operation.insertion_index)],
               error, message)) {
        return false;
    }
    inverse = RemoveMotionCompositionOperation{composition.id};
    return true;
}

bool ApplyRemove(Document& candidate,
                 RemoveMotionCompositionOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    auto& list = candidate.compositions;
    const auto found = std::find_if(
        list.begin(), list.end(), [&](const DocumentComposition& item) {
            return item.id == operation.composition_id;
        });
    if (found == list.end()) {
        return Refuse(
            EditError::UnknownComposition,
            "unknown composition_id '" + operation.composition_id + "'", error,
            message);
    }
    // REM2 -- a composition another one shows through an instance stays,
    // as one a clip shows does (M4): the instance goes first.
    for (const DocumentComposition& other : list) {
        std::vector<Ulid> media;
        std::vector<Ulid> shown;
        MotionDefinitionReferences(other.definition, media, shown);
        if (other.id != operation.composition_id &&
            std::find(shown.begin(), shown.end(), operation.composition_id) !=
                shown.end()) {
            return Refuse(EditError::ValidationFailed,
                          "composition '" + operation.composition_id +
                              "' is shown by an instance in composition '" +
                              other.id + "': remove that instance first",
                          error, message);
        }
    }
    const int64_t index = std::distance(list.begin(), found);
    const DocumentComposition removed = *found;
    list.erase(found);
    std::string validation;
    if (!candidate.Validate(validation)) {
        return Refuse(EditError::ValidationFailed, validation, error, message);
    }
    inverse = AddMotionCompositionOperation{removed, index};
    return true;
}

bool ApplyUpdate(Document& candidate,
                 UpdateMotionCompositionOperation& operation,
                 Operation& inverse, EditError& error, std::string& message) {
    DocumentComposition* target =
        Composition(candidate, operation.composition.id, error, message);
    if (target == nullptr) {
        return false;
    }
    if (!operation.composition.definition.IsObject()) {
        return Refuse(EditError::InvalidMotion,
                      "InvalidComposition at definition: a definition is a "
                      "JSON object",
                      error, message);
    }
    operation.composition.definition =
        WithDefinitionIdentities(operation.composition.definition);
    const DocumentComposition before = *target;
    *target = operation.composition;
    if (!Judge(candidate, *target, error, message)) {
        return false;
    }
    inverse = UpdateMotionCompositionOperation{before};
    return true;
}

// Node types that hold children (MOTION_FORMAT.md). A repeat holds one
// template, which is replaced rather than joined.
bool HoldsChildren(const Value& node) {
    const Value* type = node.Find("type");
    if (type == nullptr || !type->IsString()) {
        return false;
    }
    const std::string& name = type->AsString();
    return name == "fill" || name == "box" || name == "sequence" ||
           name == "series";
}

bool ApplySetNode(Document& candidate, SetMotionNodeOperation& operation,
                  Operation& inverse, EditError& error, std::string& message) {
    DocumentComposition* target =
        Composition(candidate, operation.composition_id, error, message);
    if (target == nullptr) {
        return false;
    }
    if (!operation.node.IsObject()) {
        return Refuse(EditError::InvalidMotion,
                      "InvalidValue at node: a node is a JSON object", error,
                      message);
    }
    operation.node = WithMotionIdentities(operation.node);
    const Value* uid = operation.node.Find("uid");
    const Ulid nodeUid =
        uid != nullptr && uid->IsString() ? uid->AsString() : std::string();
    MotionNodeLocation existing;
    if (!nodeUid.empty() &&
        FindMotionNode(target->definition, nodeUid, existing)) {
        if (!operation.parent_uid.empty() || !operation.before_uid.empty()) {
            return Refuse(EditError::InvalidOperation,
                          "node '" + nodeUid +
                              "' is already in the composition: it is "
                              "replaced where it stands, without parent_uid "
                              "or before_uid",
                          error, message);
        }
        return Replace(
            candidate, *target,
            MotionTreeWith(target->definition, existing.path, operation.node),
            inverse, error, message);
    }
    if (operation.parent_uid.empty()) {
        return Refuse(EditError::InvalidOperation,
                      "a new node needs parent_uid: the node it joins", error,
                      message);
    }
    MotionNodeLocation parent;
    if (!Node(*target, operation.parent_uid, parent, error, message)) {
        return false;
    }
    const Value* parentNode = MotionTreeAt(target->definition, parent.path);
    if (parentNode == nullptr || !HoldsChildren(*parentNode)) {
        return Refuse(EditError::InvalidOperation,
                      "node '" + operation.parent_uid +
                          "' holds no children: only fill, box, sequence and "
                          "series do (a repeat's template is replaced)",
                      error, message);
    }
    size_t before = SIZE_MAX;
    if (!operation.before_uid.empty()) {
        MotionNodeLocation sibling;
        if (!Node(*target, operation.before_uid, sibling, error, message)) {
            return false;
        }
        if (sibling.is_template || sibling.is_root ||
            sibling.path.size() != parent.path.size() + 2 ||
            !std::equal(
                parent.path.begin(), parent.path.end(), sibling.path.begin(),
                [](const MotionTreeStep& left, const MotionTreeStep& right) {
                    return left.is_index == right.is_index &&
                           left.key == right.key && left.index == right.index;
                })) {
            return Refuse(EditError::UnknownMotionNode,
                          "before_uid '" + operation.before_uid +
                              "' is not a child of '" + operation.parent_uid +
                              "'",
                          error, message);
        }
        before = sibling.path.back().index;
    }
    return Replace(
        candidate, *target,
        MotionTreeInsert(target->definition, Then(parent.path, "children"),
                         before, operation.node),
        inverse, error, message);
}

bool ApplyRemoveNode(Document& candidate, RemoveMotionNodeOperation& operation,
                     Operation& inverse, EditError& error,
                     std::string& message) {
    DocumentComposition* target =
        Composition(candidate, operation.composition_id, error, message);
    if (target == nullptr) {
        return false;
    }
    MotionNodeLocation location;
    if (!Node(*target, operation.node_uid, location, error, message)) {
        return false;
    }
    if (location.is_root || location.is_template) {
        return Refuse(
            EditError::InvalidOperation,
            std::string(location.is_root ? "the root" : "a repeat's template") +
                " is replaced, not removed: pose another node "
                "with its uid",
            error, message);
    }
    return Replace(candidate, *target,
                   MotionTreeErase(target->definition, location.path), inverse,
                   error, message);
}

// Sets or removes `entry` in the object `key` of a node: `style` or
// `animate`. An object left empty goes with its last entry.
bool SetEntry(Document& candidate, const Ulid& compositionId, const Ulid& uid,
              const std::string& key, const std::string& entry,
              const std::optional<Value>& value, Operation& inverse,
              EditError& error, std::string& message) {
    DocumentComposition* target =
        Composition(candidate, compositionId, error, message);
    if (target == nullptr) {
        return false;
    }
    if (entry.empty()) {
        return Refuse(EditError::InvalidOperation,
                      "the property to set is empty", error, message);
    }
    MotionNodeLocation location;
    if (!Node(*target, uid, location, error, message)) {
        return false;
    }
    const Value* node = MotionTreeAt(target->definition, location.path);
    const Value* object = node != nullptr ? node->Find(key) : nullptr;
    const MotionTreePath objectPath = Then(location.path, key);
    if (!value) {
        // B3 -- ROADMAP.md: removing what is not there is refused, not a
        // success that changed nothing.
        if (object == nullptr || !object->IsObject() ||
            object->Find(entry) == nullptr) {
            return Refuse(EditError::InvalidOperation,
                          "node '" + uid + "' has no " + key + " '" + entry +
                              "' to remove",
                          error, message);
        }
        const bool last = object->AsObject().size() == 1;
        return Replace(
            candidate, *target,
            last ? MotionTreeWith(target->definition, objectPath, std::nullopt)
                 : MotionTreeWith(target->definition, Then(objectPath, entry),
                                  std::nullopt),
            inverse, error, message);
    }
    if (value->IsNull()) {
        return Refuse(EditError::InvalidOperation,
                      "a " + key +
                          " value cannot be null: omit it to remove '" + entry +
                          "'",
                      error, message);
    }
    if (object == nullptr) {
        Value created = Value::MakeObject();
        created.Set(entry, *value);
        return Replace(candidate, *target,
                       MotionTreeWith(target->definition, objectPath, created),
                       inverse, error, message);
    }
    return Replace(
        candidate, *target,
        MotionTreeWith(target->definition, Then(objectPath, entry), value),
        inverse, error, message);
}

bool ApplySetStyle(Document& candidate, SetMotionStyleOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    return SetEntry(candidate, operation.composition_id, operation.node_uid,
                    "style", operation.property, operation.value, inverse,
                    error, message);
}

bool ApplySetAnimation(Document& candidate,
                       SetMotionAnimationOperation& operation,
                       Operation& inverse, EditError& error,
                       std::string& message) {
    // The keyframes are named here, on first application, and the operation
    // keeps their names for redo.
    if (operation.animation) {
        operation.animation = WithAnimationIdentities(*operation.animation);
    }
    return SetEntry(candidate, operation.composition_id, operation.node_uid,
                    "animate", operation.property, operation.animation, inverse,
                    error, message);
}

bool ApplySetKeyframe(Document& candidate,
                      SetMotionKeyframeOperation& operation, Operation& inverse,
                      EditError& error, std::string& message) {
    DocumentComposition* target =
        Composition(candidate, operation.composition_id, error, message);
    if (target == nullptr) {
        return false;
    }
    MotionKeyframeLocation location;
    if (!FindMotionKeyframe(target->definition, operation.keyframe_uid,
                            location)) {
        return Refuse(EditError::UnknownKeyframe,
                      "composition '" + target->id + "' has no keyframe '" +
                          operation.keyframe_uid + "'",
                      error, message);
    }
    if (operation.value.IsNull()) {
        return Refuse(EditError::InvalidOperation,
                      "a keyframe value cannot be null", error, message);
    }
    MotionTreePath frame = Then(location.interpolate, "frames");
    frame.push_back({std::string(), location.index, true});
    MotionTreePath value = Then(location.interpolate, "values");
    value.push_back({std::string(), location.index, true});
    const Value moved = MotionTreeWith(target->definition, frame,
                                       Value::MakeInt(operation.frame));
    return Replace(candidate, *target,
                   MotionTreeWith(moved, value, operation.value), inverse,
                   error, message);
}

bool ApplySetProps(Document& candidate, SetMotionPropsOperation& operation,
                   Operation& inverse, EditError& error, std::string& message) {
    DocumentComposition* target =
        Composition(candidate, operation.composition_id, error, message);
    if (target == nullptr) {
        return false;
    }
    if (!operation.props.IsObject() || operation.props.AsObject().empty()) {
        return Refuse(EditError::InvalidOperation,
                      "props is a non-empty object of prop values", error,
                      message);
    }
    const Value* declared = target->definition.Find("props");
    Value definition = target->definition;
    for (const auto& [name, value] : operation.props.AsObject()) {
        if (declared == nullptr || declared->Find(name) == nullptr) {
            std::string text = "UnknownProp at props.";
            text += name;
            text += ": the composition declares no prop '";
            text += name;
            text += "'";
            return Refuse(EditError::UnknownProp, text, error, message);
        }
        const MotionTreePath path = {
            {"props", 0, false}, {name, 0, false}, {"default", 0, false}};
        definition = value.IsNull()
                         ? (MotionTreeAt(definition, path) != nullptr
                                ? MotionTreeWith(definition, path, std::nullopt)
                                : definition)
                         : MotionTreeWith(definition, path, value);
    }
    return Replace(candidate, *target, std::move(definition), inverse, error,
                   message);
}

}  // namespace

void MotionRefusal(const MotionError& motion, EditError& error,
                   std::string& message) {
    if (motion.code == "UnknownProp") {
        error = EditError::UnknownProp;
    } else if (motion.code == "PropOutOfRange") {
        error = EditError::PropOutOfRange;
    } else if (motion.code == "PropTypeMismatch") {
        error = EditError::PropTypeMismatch;
    } else {
        error = EditError::InvalidMotion;
    }
    message = motion.code + (motion.path.empty() ? "" : " at " + motion.path) +
              ": " + motion.message;
}

bool IsMotionOperation(const Operation& operation) {
    return std::visit(
        [](const auto& value) {
            return kIsMotionOperation<std::decay_t<decltype(value)>>;
        },
        operation);
}

bool ApplyMotionOperation(Document& candidate, Operation& operation,
                          Operation& inverse, EditError& error,
                          std::string& message) {
    return std::visit(
        [&](auto& value) -> bool {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, AddMotionCompositionOperation>) {
                return ApplyAdd(candidate, value, inverse, error, message);
            } else if constexpr (std::is_same_v<
                                     T, RemoveMotionCompositionOperation>) {
                return ApplyRemove(candidate, value, inverse, error, message);
            } else if constexpr (std::is_same_v<
                                     T, UpdateMotionCompositionOperation>) {
                return ApplyUpdate(candidate, value, inverse, error, message);
            } else if constexpr (std::is_same_v<T, SetMotionNodeOperation>) {
                return ApplySetNode(candidate, value, inverse, error, message);
            } else if constexpr (std::is_same_v<T, RemoveMotionNodeOperation>) {
                return ApplyRemoveNode(candidate, value, inverse, error,
                                       message);
            } else if constexpr (std::is_same_v<T, SetMotionStyleOperation>) {
                return ApplySetStyle(candidate, value, inverse, error, message);
            } else if constexpr (std::is_same_v<T,
                                                SetMotionAnimationOperation>) {
                return ApplySetAnimation(candidate, value, inverse, error,
                                         message);
            } else if constexpr (std::is_same_v<T,
                                                SetMotionKeyframeOperation>) {
                return ApplySetKeyframe(candidate, value, inverse, error,
                                        message);
            } else if constexpr (std::is_same_v<T, SetMotionPropsOperation>) {
                return ApplySetProps(candidate, value, inverse, error, message);
            } else {
                return Refuse(EditError::InvalidOperation,
                              "not a motion operation", error, message);
            }
        },
        operation);
}
