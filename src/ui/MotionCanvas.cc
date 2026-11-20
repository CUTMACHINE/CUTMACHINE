#include "MotionCanvas.h"

#include "MotionDocument.h"
#include "Ulid.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>

namespace {

using mcp_json::Value;

bool Fail(std::string& error, std::string message) {
    error = std::move(message);
    return false;
}

// MOTION-WEB-2026-10 MW1 -- where a written key sits against a whole frame:
// -1 before it, 0 on it, 1 after it. A key between two frames ("21/2") is
// never on one; reading it as an integer would have put it at frame 0.
int CompareKeyFrame(const Value& written, int64_t frame) {
    int64_t numerator = 0;
    int64_t denominator = 1;
    if (!ParseMotionKeyFrame(written, numerator, denominator)) {
        return -1;
    }
    const __int128 key = numerator;
    const __int128 whole = static_cast<__int128>(frame) * denominator;
    return key < whole ? -1 : (key > whole ? 1 : 0);
}

// Is (x, y), in the node's own space, inside a clip shape of it?
bool InsideClip(const MotionResolvedClip& clip, double x, double y) {
    switch (clip.kind) {
        case MotionClipKind::None:
            return true;
        case MotionClipKind::Polygon: {
            bool inside = false;
            const auto& points = clip.points;
            for (size_t i = 0, j = points.size() - 1; i < points.size();
                 j = i++) {
                const double xi = points[i].first;
                const double yi = points[i].second;
                const double xj = points[j].first;
                const double yj = points[j].second;
                if ((yi > y) != (yj > y) &&
                    x < (xj - xi) * (y - yi) / (yj - yi) + xi) {
                    inside = !inside;
                }
            }
            return inside;
        }
        case MotionClipKind::Circle:
        case MotionClipKind::Ellipse: {
            if (clip.rx <= 0.0 || clip.ry <= 0.0) {
                return false;
            }
            const double u = (x - clip.cx) / clip.rx;
            const double v = (y - clip.cy) / clip.ry;
            return u * u + v * v <= 1.0;
        }
        case MotionClipKind::Inset:
            return x >= clip.rect.x && x <= clip.rect.x + clip.rect.width &&
                   y >= clip.rect.y && y <= clip.rect.y + clip.rect.height;
    }
    return true;
}

bool InsideBox(const MotionBox& box, double x, double y) {
    return x >= box.x && x <= box.x + box.width && y >= box.y &&
           y <= box.y + box.height;
}

// A node a click can take: one that paints something of its own, or a box
// someone laid out. A full-frame layer that paints nothing (a fill, a
// sequence, a series without a background) lets the click through to what
// it holds.
bool Selectable(const MotionEvaluatedNode& node) {
    switch (node.type) {
        case MotionNodeType::Audio:
        case MotionNodeType::Repeat:
        case MotionNodeType::Transition:
            return false;
        case MotionNodeType::Fill:
        case MotionNodeType::Sequence:
        case MotionNodeType::Series:
            return node.has_background || node.has_gradient;
        default:
            return true;
    }
}

void Quad(const MotionEvaluatedNode& node, const MotionHomography& full,
          std::pair<double, double> quad[4]) {
    const double xs[] = {node.box.x, node.box.x + node.box.width,
                         node.box.x + node.box.width, node.box.x};
    const double ys[] = {node.box.y, node.box.y, node.box.y + node.box.height,
                         node.box.y + node.box.height};
    for (int corner = 0; corner < 4; ++corner) {
        full.Apply(xs[corner], ys[corner], quad[corner].first,
                   quad[corner].second);
    }
}

struct HitSearch {
    double x = 0.0;
    double y = 0.0;
    bool into_instances = true;
    bool found = false;
    MotionHit hit;
};

void Visit(const MotionEvaluatedNode& node, const MotionHomography& parent,
           std::vector<std::string>& instances, HitSearch& search) {
    if (node.opacity <= 0.0) {
        return;
    }
    const MotionHomography full = parent.After(MotionNodeMapping(node));
    double lx = 0.0;
    double ly = 0.0;
    if (!full.Inverse().Apply(search.x, search.y, lx, ly)) {
        return;
    }
    // What a clip hides is hidden from the click, for the node and all it
    // holds.
    if ((node.has_clip && !InsideClip(node.clip_shape, lx, ly)) ||
        !InsideClip(node.transition_clip, lx, ly)) {
        return;
    }
    if (Selectable(node) && InsideBox(node.box, lx, ly)) {
        search.found = true;
        search.hit.uid = node.uid;
        search.hit.path = node.path;
        search.hit.instances = instances;
        Quad(node, full, search.hit.quad);
    }
    if (node.overflow_hidden && !InsideBox(node.box, lx, ly)) {
        return;
    }
    for (const MotionEvaluatedNode& child : node.children) {
        Visit(child, full, instances, search);
    }
    if (search.into_instances && !node.instance_root.empty()) {
        instances.push_back(node.uid);
        Visit(node.instance_root.front(),
              full.After(
                  MotionHomography::FromAffine(MotionInstanceTransform(node))),
              instances, search);
        instances.pop_back();
    }
}

// The first evaluated copy of `uid` in the composition itself, and the
// transform of the space it sits in.
const MotionEvaluatedNode* Find(const MotionEvaluatedNode& node,
                                const MotionHomography& parent,
                                const std::string& uid,
                                MotionHomography& found) {
    if (node.uid == uid) {
        found = parent;
        return &node;
    }
    const MotionHomography full = parent.After(MotionNodeMapping(node));
    for (const MotionEvaluatedNode& child : node.children) {
        if (const MotionEvaluatedNode* hit = Find(child, full, uid, found)) {
            return hit;
        }
    }
    return nullptr;
}

// The `animate` entry of a node of the definition for `property`, or null.
const Value* Animation(const Value& node, const std::string& property) {
    const Value* animate = node.Find("animate");
    return animate != nullptr && animate->IsObject() ? animate->Find(property)
                                                     : nullptr;
}

bool AnimatedBySpring(const Value& node, const std::string& property) {
    const Value* animation = Animation(node, property);
    return animation != nullptr && animation->Find("spring") != nullptr;
}

MotionTreePath Then(MotionTreePath path, const std::string& key) {
    path.push_back({key, 0, false});
    return path;
}

// `root` with `object`'s key `key` at `path` set, the object made when it
// is not there yet.
Value WithEntry(const Value& root, const MotionTreePath& path,
                const std::string& key, const Value& value) {
    const Value* object = MotionTreeAt(root, path);
    if (object == nullptr || !object->IsObject()) {
        Value made = Value::MakeObject();
        made.Set(key, value);
        return MotionTreeWith(root, path, made);
    }
    return MotionTreeWith(root, Then(path, key), value);
}

}  // namespace

bool MotionHitTest(const MotionFrame& frame, double x, double y,
                   bool into_instances, MotionHit& hit) {
    HitSearch search;
    search.x = x;
    search.y = y;
    search.into_instances = into_instances;
    std::vector<std::string> instances;
    Visit(frame.root, MotionHomography{}, instances, search);
    if (search.found) {
        hit = search.hit;
    }
    return search.found;
}

bool MotionNodeQuad(const MotionFrame& frame, const std::string& uid,
                    std::pair<double, double> quad[4]) {
    MotionHomography parent;
    const MotionEvaluatedNode* node =
        Find(frame.root, MotionHomography{}, uid, parent);
    if (node == nullptr) {
        return false;
    }
    Quad(*node, parent.After(MotionNodeMapping(*node)), quad);
    return true;
}

std::vector<std::pair<double, double>> MotionSelectionHandles(
    const std::pair<double, double> quad[4]) {
    std::vector<std::pair<double, double>> handles(quad, quad + 4);
    for (int edge = 0; edge < 4; ++edge) {
        const auto& from = quad[edge];
        const auto& to = quad[(edge + 1) % 4];
        handles.emplace_back((from.first + to.first) / 2.0,
                             (from.second + to.second) / 2.0);
    }
    return handles;
}

Value MotionNumberValue(double value) {
    double rounded = std::round(value * 1000.0) / 1000.0;
    if (rounded == 0.0) {
        rounded = 0.0;
    }
    char buffer[64];
    const auto written =
        std::to_chars(buffer, buffer + sizeof(buffer), rounded);
    return Value::MakeRawNumber(std::string(buffer, written.ptr));
}

namespace {

// MOTION-BIBLIOTHEQUE-2026-10 MB5 -- the curves of an easing written as a
// list, one per segment; null for a name shared by every segment, which
// adding or removing a key leaves as it is.
const std::vector<Value>* SegmentCurves(const Value& interpolate, size_t keys) {
    const Value* easing = interpolate.Find("easing");
    if (easing == nullptr || !easing->IsArray() ||
        easing->AsArray().size() + 1 != keys) {
        return nullptr;
    }
    return &easing->AsArray();
}

Value CurveList(const std::vector<Value>& curves) {
    Value list = Value::MakeArray();
    for (const Value& curve : curves) {
        list.Push(curve);
    }
    return list;
}

// The curve an animation shows as one name: the name itself, the one its
// list repeats, "mixte" when its segments differ.
std::string EasingName(const Value& interpolate) {
    const Value* easing = interpolate.Find("easing");
    if (easing != nullptr && easing->IsString()) {
        return easing->AsString();
    }
    std::string shared;
    if (easing != nullptr && easing->IsArray()) {
        for (const Value& curve : easing->AsArray()) {
            const std::string name =
                curve.IsString() ? curve.AsString() : std::string();
            if (shared.empty()) {
                shared = name;
            } else if (shared != name) {
                return "mixte";
            }
        }
    }
    return shared.empty() ? std::string("linear") : shared;
}

}  // namespace

bool MotionAnimationWithKey(const Value* animation, int64_t frame,
                            const Value& value, Value& output,
                            std::string& error) {
    if (frame < 0) {
        return Fail(error, "a key sits at a frame of 0 or more");
    }
    if (animation == nullptr) {
        Value interpolate = Value::MakeObject();
        Value frames = Value::MakeArray();
        frames.Push(Value::MakeInt(frame));
        Value values = Value::MakeArray();
        values.Push(value);
        Value uids = Value::MakeArray();
        uids.Push(Value::MakeString(GenerateUlid()));
        interpolate.Set("frames", std::move(frames));
        interpolate.Set("values", std::move(values));
        interpolate.Set("uids", std::move(uids));
        output = Value::MakeObject();
        output.Set("interpolate", std::move(interpolate));
        return true;
    }
    const Value* interpolate = animation->Find("interpolate");
    if (interpolate == nullptr || !interpolate->IsObject()) {
        return Fail(error,
                    "this property moves by a spring, which has no keys: "
                    "edit its spring, or replace it by keys");
    }
    const Value* frames = interpolate->Find("frames");
    const Value* values = interpolate->Find("values");
    const Value* uids = interpolate->Find("uids");
    if (frames == nullptr || values == nullptr || !frames->IsArray() ||
        !values->IsArray()) {
        return Fail(error, "the animation has no frames and values");
    }
    Value newFrames = Value::MakeArray();
    Value newValues = Value::MakeArray();
    Value newUids = Value::MakeArray();
    const size_t keys = frames->AsArray().size();
    bool placed = false;
    bool added = true;
    size_t position = keys;
    for (size_t index = 0; index < keys; ++index) {
        const int at = CompareKeyFrame(frames->AsArray()[index], frame);
        if (!placed && at >= 0) {
            placed = true;
            position = index;
            added = at != 0;
            newFrames.Push(Value::MakeInt(frame));
            newValues.Push(value);
            if (uids != nullptr) {
                newUids.Push(at == 0 ? uids->AsArray()[index]
                                     : Value::MakeString(GenerateUlid()));
            }
            if (at == 0) {
                continue;
            }
        }
        newFrames.Push(frames->AsArray()[index]);
        newValues.Push(values->AsArray()[index]);
        if (uids != nullptr) {
            newUids.Push(uids->AsArray()[index]);
        }
    }
    if (!placed) {
        newFrames.Push(Value::MakeInt(frame));
        newValues.Push(value);
        if (uids != nullptr) {
            newUids.Push(Value::MakeString(GenerateUlid()));
        }
    }
    Value updated = *interpolate;
    updated.Set("frames", std::move(newFrames));
    updated.Set("values", std::move(newValues));
    if (uids != nullptr) {
        updated.Set("uids", std::move(newUids));
    }
    // MB5 -- a key added to a list of curves cuts a segment in two halves
    // of the same curve, or lengthens the animation by a segment shaped as
    // its neighbour; the first segment of all is linear.
    if (const std::vector<Value>* listed = SegmentCurves(*interpolate, keys);
        listed != nullptr && added) {
        std::vector<Value> curves = *listed;
        if (curves.empty()) {
            curves.push_back(Value::MakeString("linear"));
        } else {
            const size_t source =
                std::min(position == 0 ? 0 : position - 1, curves.size() - 1);
            const Value copy = curves[source];
            curves.insert(curves.begin() + static_cast<std::ptrdiff_t>(source),
                          copy);
        }
        updated.Set("easing", CurveList(curves));
    }
    output = *animation;
    output.Set("interpolate", std::move(updated));
    return true;
}

bool MotionAnimationWithoutKey(const Value& animation, int64_t frame,
                               std::optional<Value>& output,
                               std::string& error) {
    const Value* interpolate = animation.Find("interpolate");
    const Value* frames =
        interpolate != nullptr ? interpolate->Find("frames") : nullptr;
    if (frames == nullptr || !frames->IsArray()) {
        return Fail(error, "this property has no keys");
    }
    const Value* values = interpolate->Find("values");
    const Value* uids = interpolate->Find("uids");
    Value newFrames = Value::MakeArray();
    Value newValues = Value::MakeArray();
    Value newUids = Value::MakeArray();
    const size_t keys = frames->AsArray().size();
    bool removed = false;
    size_t position = 0;
    for (size_t index = 0; index < keys; ++index) {
        if (CompareKeyFrame(frames->AsArray()[index], frame) == 0) {
            removed = true;
            position = index;
            continue;
        }
        newFrames.Push(frames->AsArray()[index]);
        newValues.Push(values->AsArray()[index]);
        if (uids != nullptr) {
            newUids.Push(uids->AsArray()[index]);
        }
    }
    if (!removed) {
        return Fail(error, "there is no key at frame " + std::to_string(frame));
    }
    if (newFrames.AsArray().empty()) {
        output.reset();
        return true;
    }
    Value updated = *interpolate;
    updated.Set("frames", std::move(newFrames));
    updated.Set("values", std::move(newValues));
    if (uids != nullptr) {
        updated.Set("uids", std::move(newUids));
    }
    // MB5 -- the two segments around a removed key become one, shaped as
    // the one that left the key before it: a key owns the curve it starts.
    if (const std::vector<Value>* listed = SegmentCurves(*interpolate, keys);
        listed != nullptr && !listed->empty()) {
        std::vector<Value> curves = *listed;
        curves.erase(curves.begin() + static_cast<std::ptrdiff_t>(std::min(
                                          position, curves.size() - 1)));
        updated.Set("easing", CurveList(curves));
    }
    Value result = animation;
    result.Set("interpolate", std::move(updated));
    output = std::move(result);
    return true;
}

bool MotionAnimationWithEasing(const Value& animation,
                               const std::string& easing, Value& output,
                               std::string& error,
                               std::optional<int64_t> frame) {
    const Value* interpolate = animation.Find("interpolate");
    if (interpolate == nullptr || !interpolate->IsObject()) {
        return Fail(error, "a spring has no easing: its settings shape it");
    }
    Value updated = *interpolate;
    if (frame) {
        // MB5 -- one segment only: the one leaving the key at `frame`.
        const Value* frames = interpolate->Find("frames");
        if (frames == nullptr || !frames->IsArray()) {
            return Fail(error, "the animation has no frames");
        }
        const size_t keys = frames->AsArray().size();
        size_t position = keys;
        for (size_t index = 0; index < keys; ++index) {
            int64_t at = 0;
            if (frames->AsArray()[index].AsInt64(at) && at == *frame) {
                position = index;
            }
        }
        if (position == keys) {
            return Fail(error,
                        "there is no key at frame " + std::to_string(*frame));
        }
        if (position + 1 == keys) {
            return Fail(error, "the key at frame " + std::to_string(*frame) +
                                   " is the last one: no segment leaves it");
        }
        const std::vector<Value>* listed = SegmentCurves(*interpolate, keys);
        std::vector<Value> curves =
            listed != nullptr
                ? *listed
                : std::vector<Value>(
                      keys - 1, Value::MakeString(EasingName(*interpolate)));
        curves[position] = Value::MakeString(easing);
        // One name when every segment ends up with the same curve: the
        // shortest spelling of the same track.
        const bool same =
            std::all_of(curves.begin(), curves.end(), [&](const Value& curve) {
                return curve.IsString() && curve.AsString() == easing;
            });
        updated.Set("easing",
                    same ? Value::MakeString(easing) : CurveList(curves));
    } else {
        updated.Set("easing", Value::MakeString(easing));
    }
    output = animation;
    output.Set("interpolate", std::move(updated));
    return true;
}

bool MotionDefinitionWithEdits(const Value& definition,
                               const std::vector<MotionPropertyEdit>& edits,
                               Value& output, std::string& error) {
    Value result = definition;
    for (const MotionPropertyEdit& edit : edits) {
        MotionNodeLocation location;
        if (!FindMotionNode(result, edit.uid, location)) {
            return Fail(error,
                        "the composition has no node '" + edit.uid + "'");
        }
        const Value* node = MotionTreeAt(result, location.path);
        const Value* animation = Animation(*node, edit.property);
        if (edit.value.IsNull()) {
            // A field emptied: the style goes, and the property is CSS's
            // default again.
            const MotionTreePath path =
                Then(Then(location.path, "style"), edit.property);
            if (MotionTreeAt(result, path) != nullptr) {
                result = MotionTreeWith(result, path, std::nullopt);
            }
            continue;
        }
        if (animation != nullptr || edit.keyed) {
            Value keyed;
            if (!MotionAnimationWithKey(animation, edit.frame, edit.value,
                                        keyed, error)) {
                error.insert(0, edit.property + ": ");
                return false;
            }
            result = WithEntry(result, Then(location.path, "animate"),
                               edit.property, keyed);
        } else {
            result = WithEntry(result, Then(location.path, "style"),
                               edit.property, edit.value);
        }
    }
    output = std::move(result);
    return true;
}

bool MotionMoveEdits(const MotionFrame& frame, const Value& definition,
                     const std::string& uid, int64_t compositionFrame,
                     double dx, double dy,
                     std::vector<MotionPropertyEdit>& edits,
                     std::string& error) {
    MotionHomography parent;
    const MotionEvaluatedNode* node =
        Find(frame.root, MotionHomography{}, uid, parent);
    MotionNodeLocation location;
    if (node == nullptr || !FindMotionNode(definition, uid, location)) {
        return Fail(error, "the node '" + uid + "' is not shown at this frame");
    }
    const Value* stored = MotionTreeAt(definition, location.path);
    for (const char* property : {"translateX", "translateY"}) {
        if (AnimatedBySpring(*stored, property)) {
            return Fail(error, std::string(property) +
                                   " moves by a spring: edit its spring");
        }
    }
    // Where the centre of the box sits in its parent's space, and where the
    // drag takes it there.
    const double centerX = node->box.x + node->box.width / 2.0;
    const double centerY = node->box.y + node->box.height / 2.0;
    double frameX = 0.0;
    double frameY = 0.0;
    double parentX = 0.0;
    double parentY = 0.0;
    if (!parent.Apply(centerX + node->translate_x, centerY + node->translate_y,
                      frameX, frameY) ||
        !parent.Inverse().Apply(frameX + dx, frameY + dy, parentX, parentY)) {
        return Fail(error, "the node is behind the viewer");
    }
    edits.push_back({uid, "translateX", MotionNumberValue(parentX - centerX),
                     false, compositionFrame});
    edits.push_back({uid, "translateY", MotionNumberValue(parentY - centerY),
                     false, compositionFrame});
    return true;
}

bool MotionResizeEdits(const MotionFrame& frame, const Value& definition,
                       const std::string& uid, int64_t compositionFrame,
                       int handle, double dx, double dy,
                       std::vector<MotionPropertyEdit>& edits,
                       std::string& error) {
    if (handle < 0 || handle > 7) {
        return Fail(error, "a selection has eight handles, 0 to 7");
    }
    MotionHomography parent;
    const MotionEvaluatedNode* node =
        Find(frame.root, MotionHomography{}, uid, parent);
    MotionNodeLocation location;
    if (node == nullptr || !FindMotionNode(definition, uid, location)) {
        return Fail(error, "the node '" + uid + "' is not shown at this frame");
    }
    const Value* stored = MotionTreeAt(definition, location.path);
    for (const char* property : {"width", "height"}) {
        if (AnimatedBySpring(*stored, property)) {
            return Fail(error, std::string(property) +
                                   " moves by a spring: edit its spring");
        }
    }
    std::pair<double, double> quad[4];
    const MotionHomography full = parent.After(MotionNodeMapping(*node));
    Quad(*node, full, quad);
    const auto handles = MotionSelectionHandles(quad);
    const MotionHomography inverse = full.Inverse();
    double fromX = 0.0;
    double fromY = 0.0;
    double toX = 0.0;
    double toY = 0.0;
    const auto& at = handles[static_cast<size_t>(handle)];
    if (!inverse.Apply(at.first, at.second, fromX, fromY) ||
        !inverse.Apply(at.first + dx, at.second + dy, toX, toY)) {
        return Fail(error, "the node is behind the viewer");
    }
    // The drag along the node's own axes.
    const double lx = toX - fromX;
    const double ly = toY - fromY;
    static const int kHorizontal[8] = {-1, 1, 1, -1, 0, 1, 0, -1};
    static const int kVertical[8] = {-1, -1, 1, 1, -1, 0, 1, 0};
    const int horizontal = kHorizontal[handle];
    const int vertical = kVertical[handle];
    if (horizontal != 0) {
        edits.push_back({uid, "width",
                         MotionNumberValue(
                             std::max(1.0, node->box.width + horizontal * lx)),
                         false, compositionFrame});
    }
    if (vertical != 0) {
        edits.push_back(
            {uid, "height",
             MotionNumberValue(std::max(1.0, node->box.height + vertical * ly)),
             false, compositionFrame});
    }
    return true;
}

namespace {

// #rrggbb, and its alpha as #rrggbbaa when it is not opaque.
std::string Hex(const MotionColor& color) {
    static const char kHex[] = "0123456789abcdef";
    const auto byte = [](double channel) {
        return static_cast<int>(
            std::lround(std::clamp(channel, 0.0, 1.0) * 255.0));
    };
    std::string text = "#";
    for (const double channel : {color.red, color.green, color.blue}) {
        text.push_back(kHex[byte(channel) >> 4]);
        text.push_back(kHex[byte(channel) & 15]);
    }
    if (byte(color.alpha) != 255) {
        text.push_back(kHex[byte(color.alpha) >> 4]);
        text.push_back(kHex[byte(color.alpha) & 15]);
    }
    return text;
}

std::string Short(double value) { return MotionNumberValue(value).Dump(); }

// The literal a style holds, as text, or `fallback`.
std::string StyleText(const Value& node, const std::string& property,
                      const std::string& fallback) {
    const Value* style = node.Find("style");
    const Value* value = style != nullptr ? style->Find(property) : nullptr;
    if (value == nullptr || value->IsObject()) {
        return fallback;
    }
    return value->IsString() ? value->AsString() : value->Dump();
}

void Animate(const Value& node, int64_t frame, MotionField& field) {
    field.keyable = true;
    const Value* animation = Animation(node, field.property);
    if (animation == nullptr) {
        return;
    }
    if (const Value* spring = animation->Find("spring")) {
        field.spring = true;
        double number = 0.0;
        if (const Value* from = spring->Find("from");
            from != nullptr && MotionNumber(*from, number)) {
            field.spring_from = number;
        }
        if (const Value* to = spring->Find("to");
            to != nullptr && MotionNumber(*to, number)) {
            field.spring_to = number;
        }
        if (const Value* delay = spring->Find("delay")) {
            delay->AsInt64(field.spring_delay);
        }
        if (const Value* damping = spring->Find("damping");
            damping != nullptr && MotionNumber(*damping, number)) {
            field.spring_damping = number;
        }
        return;
    }
    const Value* interpolate = animation->Find("interpolate");
    if (interpolate == nullptr) {
        return;
    }
    field.animated = true;
    field.easing = EasingName(*interpolate);
    if (const Value* frames = interpolate->Find("frames")) {
        for (const Value& at : frames->AsArray()) {
            int64_t keyed = 0;
            if (at.AsInt64(keyed) && keyed == frame) {
                field.key_here = true;
            }
        }
    }
}

MotionField Number(const Value& node, int64_t frame, const char* property,
                   const char* label, const char* group, double value,
                   bool keyable = true) {
    MotionField field;
    field.property = property;
    field.label = label;
    field.group = group;
    field.kind = MotionFieldKind::Number;
    field.text = Short(value);
    if (keyable) {
        Animate(node, frame, field);
    }
    return field;
}

MotionField Color(const Value& node, int64_t frame, const char* property,
                  const char* label, const char* group,
                  const MotionColor& color) {
    MotionField field;
    field.property = property;
    field.label = label;
    field.group = group;
    field.kind = MotionFieldKind::Color;
    field.text = Hex(color);
    Animate(node, frame, field);
    return field;
}

MotionField Choice(const Value& node, const char* property, const char* label,
                   const char* group, const std::string& fallback,
                   std::vector<std::string> choices) {
    MotionField field;
    field.property = property;
    field.label = label;
    field.group = group;
    field.kind = MotionFieldKind::Choice;
    field.text = StyleText(node, property, fallback);
    field.choices = std::move(choices);
    return field;
}

MotionField NodeChoice(const Value& value, const char* property,
                       const char* label, const char* group,
                       const std::string& fallback,
                       std::vector<std::string> choices) {
    MotionField field;
    field.property = property;
    field.label = label;
    field.group = group;
    field.kind = MotionFieldKind::Choice;
    field.text = value.IsString() ? value.AsString() : fallback;
    field.choices = std::move(choices);
    return field;
}

const std::vector<std::string> kJustify = {"flex-start",   "center",
                                           "flex-end",     "space-between",
                                           "space-around", "space-evenly"};
const std::vector<std::string> kAlign = {"flex-start", "center", "flex-end",
                                         "stretch", "baseline"};
const std::vector<std::string> kEasings = {"linear", "ease", "ease-in",
                                           "ease-out", "ease-in-out"};

bool Layered(MotionNodeType type) {
    return type == MotionNodeType::Fill || type == MotionNodeType::Box ||
           type == MotionNodeType::Sequence || type == MotionNodeType::Series;
}

const std::vector<std::string> kCascadeFields = {
    "units", "stagger.each", "stagger.order", "stagger.seed", "animate.easing"};

// The easing every keyed animation of `node` shares, "mixte" when they
// differ, empty when none is keyed.
std::string SharedEasing(const Value& node) {
    const Value* animate = node.Find("animate");
    std::string shared;
    if (animate == nullptr || !animate->IsObject()) {
        return shared;
    }
    for (const auto& entry : animate->AsObject()) {
        const Value* interpolate = entry.second.Find("interpolate");
        if (interpolate == nullptr) {
            continue;
        }
        const std::string name = EasingName(*interpolate);
        if (shared.empty()) {
            shared = name;
        } else if (shared != name) {
            return "mixte";
        }
    }
    return shared;
}

// MA6 -- MOTION-AFTER-2026-09. A text's cascade as rows: how it is cut,
// the frames between two units, their order (and its seed), and the curve
// every unit animation shares -- what After shows as an animator's
// controls, set here instead of in the JSON.
void AppendCascade(const Value& node, std::vector<MotionField>& fields) {
    const Value* units = node.Find("units");
    fields.push_back(NodeChoice(units != nullptr ? *units : Value(), "units",
                                "Découpe", "Cascade", "none",
                                {"none", "line", "word", "char"}));
    if (units == nullptr) {
        return;
    }
    const Value* stagger = node.Find("stagger");
    const Value* each = stagger != nullptr ? stagger->Find("each") : nullptr;
    const Value* order = stagger != nullptr ? stagger->Find("order") : nullptr;
    const Value* seed = stagger != nullptr ? stagger->Find("seed") : nullptr;
    MotionField delay;
    delay.property = "stagger.each";
    delay.label = "Décalage (images)";
    delay.group = "Cascade";
    delay.text = each != nullptr ? each->Dump() : "0";
    fields.push_back(delay);
    fields.push_back(NodeChoice(order != nullptr ? *order : Value(),
                                "stagger.order", "Ordre", "Cascade", "forward",
                                {"forward", "reverse", "random"}));
    if (order != nullptr && order->IsString() &&
        order->AsString() == "random") {
        MotionField drawn;
        drawn.property = "stagger.seed";
        drawn.label = "Graine";
        drawn.group = "Cascade";
        drawn.text = seed != nullptr ? seed->Dump() : "0";
        fields.push_back(drawn);
    }
    const std::string easing = SharedEasing(node);
    if (!easing.empty()) {
        MotionField curve;
        curve.property = "animate.easing";
        curve.label = "Courbe partagée";
        curve.group = "Cascade";
        curve.kind = MotionFieldKind::Choice;
        curve.text = easing;
        curve.choices = kEasings;
        fields.push_back(curve);
    }
}

// A whole number of frames or a seed, as typed: refused unless it is one.
bool WholeNumber(const std::string& text, int64_t& number) {
    char* end = nullptr;
    const double value = std::strtod(text.c_str(), &end);
    if (end == text.c_str() || *end != '\0' || value < 0.0 ||
        value != std::floor(value) || value > 1e9) {
        return false;
    }
    number = static_cast<int64_t>(value);
    return true;
}

// `definition` with a cascade setting of text `uid` changed. The motion
// parser judges the result when the operation is applied; what it would
// refuse outright -- a stagger without units, a random order without a
// seed, a seed without it -- is kept consistent here.
bool WithCascade(const Value& definition, const MotionTreePath& path,
                 const Value& node, const std::string& field,
                 const std::string& text, Value& output, std::string& error) {
    const MotionTreePath staggerPath = Then(path, "stagger");
    const bool staggered = node.Find("stagger") != nullptr;
    if (field == "units") {
        if (text.empty() || text == "none") {
            // No units, no cascade.
            Value result = definition;
            for (const char* key : {"units", "stagger"}) {
                if (node.Find(key) != nullptr) {
                    result =
                        MotionTreeWith(result, Then(path, key), std::nullopt);
                }
            }
            output = std::move(result);
            return true;
        }
        if (text != "line" && text != "word" && text != "char") {
            return Fail(error, "découpe : line, word ou char");
        }
        output = WithEntry(definition, path, "units", Value::MakeString(text));
        return true;
    }
    if (node.Find("units") == nullptr) {
        return Fail(error, "la cascade demande une découpe d'abord");
    }
    if (field == "animate.easing") {
        if (text.empty() || text == "mixte") {
            return Fail(error, "choisissez une courbe pour toutes les unités");
        }
        const Value* animate = node.Find("animate");
        if (animate == nullptr || !animate->IsObject() ||
            SharedEasing(node).empty()) {
            return Fail(error, "aucune animation par clés à courber");
        }
        Value result = definition;
        for (const auto& entry : animate->AsObject()) {
            if (entry.second.Find("interpolate") == nullptr) {
                continue;
            }
            Value eased;
            if (!MotionAnimationWithEasing(entry.second, text, eased, error)) {
                return false;
            }
            result =
                WithEntry(result, Then(path, "animate"), entry.first, eased);
        }
        output = std::move(result);
        return true;
    }
    Value stagger = staggered ? *node.Find("stagger") : Value::MakeObject();
    if (stagger.Find("each") == nullptr) {
        stagger.Set("each", Value::MakeInt(0));
    }
    if (field == "stagger.each") {
        if (text.empty()) {
            // No delay: every unit at once.
            output = staggered
                         ? MotionTreeWith(definition, staggerPath, std::nullopt)
                         : definition;
            return true;
        }
        int64_t frames = 0;
        if (!WholeNumber(text, frames)) {
            return Fail(error, "décalage : un nombre entier d'images");
        }
        stagger.Set("each", Value::MakeInt(frames));
    } else if (field == "stagger.order") {
        Value kept = Value::MakeObject();
        kept.Set("each", *stagger.Find("each"));
        if (text == "random") {
            kept.Set("order", Value::MakeString("random"));
            const Value* seed = stagger.Find("seed");
            kept.Set("seed", seed != nullptr ? *seed : Value::MakeInt(0));
        } else if (text == "reverse") {
            kept.Set("order", Value::MakeString("reverse"));
        } else if (!text.empty() && text != "forward") {
            return Fail(error, "ordre : forward, reverse ou random");
        }
        stagger = std::move(kept);
    } else if (field == "stagger.seed") {
        int64_t seed = 0;
        if (!WholeNumber(text.empty() ? "0" : text, seed)) {
            return Fail(error, "graine : un entier positif ou nul");
        }
        stagger.Set("seed", Value::MakeInt(seed));
    }
    output = MotionTreeWith(definition, staggerPath, stagger);
    return true;
}

}  // namespace

bool MotionNodeFields(const Value& definition, const MotionFrame& frame,
                      const std::string& uid, int64_t compositionFrame,
                      std::vector<MotionField>& fields, std::string& error) {
    fields.clear();
    MotionNodeLocation location;
    if (!FindMotionNode(definition, uid, location)) {
        return Fail(error, "the composition has no node '" + uid + "'");
    }
    const Value& stored = *MotionTreeAt(definition, location.path);
    const Value* type = stored.Find("type");
    const std::string typeName =
        type != nullptr && type->IsString() ? type->AsString() : "";
    if (typeName == "transition") {
        // Not evaluated: it has no box. Its time and presentation are read
        // as written.
        const Value* timing = stored.Find("timing");
        const Value* presentation = stored.Find("presentation");
        const Value* direction = stored.Find("direction");
        fields.push_back(
            NodeChoice(presentation != nullptr ? *presentation : Value(),
                       "presentation", "Présentation", "Temps", "fade",
                       {"fade", "slide", "wipe", "iris"}));
        fields.push_back(
            NodeChoice(direction != nullptr ? *direction : Value(), "direction",
                       "Direction", "Temps", "from-right",
                       {"from-left", "from-right", "from-top", "from-bottom"}));
        MotionField duration;
        duration.property = "timing.duration";
        duration.label = "Durée";
        duration.group = "Temps";
        duration.text = "0";
        if (timing != nullptr) {
            if (const Value* frames = timing->Find("duration")) {
                duration.text = frames->Dump();
            }
        }
        fields.push_back(duration);
        const Value* easing =
            timing != nullptr ? timing->Find("easing") : nullptr;
        fields.push_back(NodeChoice(easing != nullptr ? *easing : Value(),
                                    "timing.easing", "Courbe", "Temps",
                                    "linear", kEasings));
        return true;
    }
    MotionHomography parent;
    const MotionEvaluatedNode* node =
        Find(frame.root, MotionHomography{}, uid, parent);
    if (node == nullptr) {
        return Fail(error, "the node '" + uid + "' is not shown at this frame");
    }
    const int64_t at = compositionFrame;
    if (node->type == MotionNodeType::Text) {
        MotionField content;
        content.property = "content";
        content.label = "Texte";
        content.group = "Texte";
        content.kind = MotionFieldKind::Text;
        const Value* written = stored.Find("content");
        content.text = node->text;
        content.editable = written != nullptr && written->IsString();
        fields.push_back(content);
        fields.push_back(Number(stored, at, "fontSize", "Taille", "Texte",
                                node->text_style.font_size));
        fields.push_back(Color(stored, at, "color", "Couleur", "Texte",
                               node->text_style.color));
        MotionField family;
        family.property = "fontFamily";
        family.label = "Police";
        family.group = "Texte";
        family.kind = MotionFieldKind::Text;
        family.text = node->text_style.font_family;
        fields.push_back(family);
        fields.push_back(Number(stored, at, "fontWeight", "Graisse", "Texte",
                                node->text_style.font_weight, false));
        fields.push_back(Choice(stored, "fontStyle", "Style", "Texte",
                                node->text_style.italic ? "italic" : "normal",
                                {"normal", "italic"}));
        AppendCascade(stored, fields);
    }
    if (node->type != MotionNodeType::Audio) {
        fields.push_back(Number(stored, at, "translateX", "Position X",
                                "Transformation", node->translate_x));
        fields.push_back(Number(stored, at, "translateY", "Position Y",
                                "Transformation", node->translate_y));
        fields.push_back(Number(stored, at, "rotate", "Rotation",
                                "Transformation", node->rotate));
        fields.push_back(Number(stored, at, "scale", "Échelle",
                                "Transformation", node->scale_x));
        fields.push_back(Number(stored, at, "opacity", "Opacité",
                                "Transformation", node->opacity));
        fields.push_back(Number(stored, at, "rotateX", "Rotation X",
                                "Transformation", node->rotate_x));
        fields.push_back(Number(stored, at, "rotateY", "Rotation Y",
                                "Transformation", node->rotate_y));
        fields.push_back(Number(stored, at, "width", "Largeur",
                                "Transformation", node->box.width));
        fields.push_back(Number(stored, at, "height", "Hauteur",
                                "Transformation", node->box.height));
    }
    if (Layered(node->type) || node->type == MotionNodeType::Image ||
        node->type == MotionNodeType::Video ||
        node->type == MotionNodeType::Composition) {
        MotionColor background = node->background;
        if (!node->has_background) {
            background = {0.0, 0.0, 0.0, 0.0};
        }
        fields.push_back(Color(stored, at, "backgroundColor", "Fond",
                               "Apparence", background));
        fields.push_back(Number(stored, at, "borderRadius", "Arrondi",
                                "Apparence", node->border_radius));
    }
    if (node->type == MotionNodeType::Video ||
        node->type == MotionNodeType::Audio) {
        fields.push_back(
            Number(stored, at, "volume", "Volume", "Son", node->media.volume));
    }
    if (Layered(node->type)) {
        const bool column = node->type != MotionNodeType::Box;
        fields.push_back(Choice(stored, "flexDirection", "Direction",
                                "Mise en page", column ? "column" : "row",
                                {"row", "column"}));
        fields.push_back(Choice(stored, "justifyContent", "Justifier",
                                "Mise en page", "flex-start", kJustify));
        fields.push_back(Choice(stored, "alignItems", "Aligner", "Mise en page",
                                "stretch", kAlign));
        MotionField gap = Number(stored, at, "gap", "Écart", "Mise en page", 0);
        gap.text = StyleText(stored, "gap", "0");
        fields.push_back(gap);
        MotionField padding;
        padding.property = "padding";
        padding.label = "Marge intérieure";
        padding.group = "Mise en page";
        padding.kind = MotionFieldKind::Text;
        padding.text = StyleText(stored, "padding", "0");
        fields.push_back(padding);
    }
    if (node->type == MotionNodeType::Sequence) {
        const Value* duration = stored.Find("duration");
        MotionField field;
        field.property = "duration";
        field.label = "Durée";
        field.group = "Temps";
        field.text = duration != nullptr ? duration->Dump() : std::string();
        fields.push_back(field);
    }
    return true;
}

bool MotionDefinitionWithNodeField(const Value& definition,
                                   const std::string& uid,
                                   const std::string& field,
                                   const std::optional<Value>& value,
                                   Value& output, std::string& error) {
    static const std::vector<std::string> kFields = {
        "content",         "duration",     "from",
        "presentation",    "direction",    "startFrom",
        "timing.duration", "timing.easing"};
    if (std::find(kFields.begin(), kFields.end(), field) == kFields.end()) {
        return Fail(error, "'" + field + "' is not a field of a node");
    }
    MotionNodeLocation location;
    if (!FindMotionNode(definition, uid, location)) {
        return Fail(error, "the composition has no node '" + uid + "'");
    }
    const size_t dot = field.find('.');
    if (dot == std::string::npos) {
        const Value* existing =
            MotionTreeAt(definition, Then(location.path, field));
        if (!value.has_value() && existing == nullptr) {
            output = definition;
            return true;
        }
        output = value.has_value()
                     ? WithEntry(definition, location.path, field, *value)
                     : MotionTreeWith(definition, Then(location.path, field),
                                      std::nullopt);
        return true;
    }
    const std::string outer = field.substr(0, dot);
    const std::string inner = field.substr(dot + 1);
    if (!value.has_value()) {
        const MotionTreePath path = Then(Then(location.path, outer), inner);
        output = MotionTreeAt(definition, path) != nullptr
                     ? MotionTreeWith(definition, path, std::nullopt)
                     : definition;
        return true;
    }
    output = WithEntry(definition, Then(location.path, outer), inner, *value);
    return true;
}

bool MotionDefinitionWithTypedField(const Value& definition,
                                    const std::string& uid,
                                    const MotionField& field,
                                    const std::string& text,
                                    int64_t compositionFrame, Value& output,
                                    std::string& error) {
    MotionNodeLocation location;
    if (!FindMotionNode(definition, uid, location)) {
        return Fail(error, "the composition has no node '" + uid + "'");
    }
    if (std::find(kCascadeFields.begin(), kCascadeFields.end(),
                  field.property) != kCascadeFields.end()) {
        return WithCascade(definition, location.path,
                           *MotionTreeAt(definition, location.path),
                           field.property, text, output, error);
    }
    static const std::vector<std::string> kNodeFields = {
        "content",         "duration",     "from",
        "presentation",    "direction",    "startFrom",
        "timing.duration", "timing.easing"};
    const bool nodeField = std::find(kNodeFields.begin(), kNodeFields.end(),
                                     field.property) != kNodeFields.end();
    Value value = Value::MakeNull();
    if (!text.empty() && field.kind == MotionFieldKind::Number) {
        char* end = nullptr;
        const double number = std::strtod(text.c_str(), &end);
        if (end == text.c_str()) {
            return Fail(error, "un nombre est attendu");
        }
        value = nodeField ? Value::MakeInt(std::llround(number))
                          : MotionNumberValue(number);
    } else if (!text.empty()) {
        value = Value::MakeString(text);
    }
    if (nodeField) {
        return MotionDefinitionWithNodeField(
            definition, uid, field.property,
            text.empty() ? std::nullopt : std::optional<Value>(value), output,
            error);
    }
    return MotionDefinitionWithEdits(
        definition, {{uid, field.property, value, false, compositionFrame}},
        output, error);
}

bool MotionDefinitionWithSpring(const Value& definition, const std::string& uid,
                                const std::string& property, bool spring,
                                double from, double to, int64_t delay,
                                double damping, Value& output,
                                std::string& error) {
    MotionNodeLocation location;
    if (!FindMotionNode(definition, uid, location)) {
        return Fail(error, "the composition has no node '" + uid + "'");
    }
    if (spring) {
        Value settings = Value::MakeObject();
        settings.Set("from", MotionNumberValue(from));
        settings.Set("to", MotionNumberValue(to));
        if (delay > 0) {
            settings.Set("delay", Value::MakeInt(delay));
        }
        settings.Set("damping", MotionNumberValue(damping));
        Value animation = Value::MakeObject();
        animation.Set("spring", std::move(settings));
        output = WithEntry(definition, Then(location.path, "animate"), property,
                           animation);
        return true;
    }
    Value result = definition;
    const MotionTreePath path = Then(Then(location.path, "animate"), property);
    if (MotionTreeAt(result, path) != nullptr) {
        result = MotionTreeWith(result, path, std::nullopt);
    }
    output = WithEntry(result, Then(location.path, "style"), property,
                       MotionNumberValue(from));
    return true;
}

Value MotionNewNode(const std::string& type) {
    Value node = Value::MakeObject();
    node.Set("type", Value::MakeString(type));
    Value style = Value::MakeObject();
    if (type == "text") {
        node.Set("content", Value::MakeString("Texte"));
        style.Set("fontSize", Value::MakeInt(64));
        style.Set("color", Value::MakeString("#ffffff"));
    } else if (type == "box") {
        style.Set("width", Value::MakeInt(320));
        style.Set("height", Value::MakeInt(180));
        style.Set("backgroundColor", Value::MakeString("#4DABA1"));
    } else if (type == "sequence") {
        style.Set("justifyContent", Value::MakeString("center"));
        style.Set("alignItems", Value::MakeString("center"));
    } else if (type == "image" || type == "video") {
        style.Set("width", Value::MakeString("50%"));
        style.Set("objectFit", Value::MakeString("contain"));
    }
    node.Set("style", std::move(style));
    return node;
}

Value MotionNewTransition(const std::string& presentation,
                          const std::string& direction, int64_t duration,
                          const std::string& easing) {
    Value node = Value::MakeObject();
    node.Set("type", Value::MakeString("transition"));
    node.Set("presentation", Value::MakeString(presentation));
    if (!direction.empty() &&
        (presentation == "slide" || presentation == "wipe")) {
        node.Set("direction", Value::MakeString(direction));
    }
    Value timing = Value::MakeObject();
    timing.Set("duration", Value::MakeInt(duration));
    timing.Set("easing", Value::MakeString(easing));
    node.Set("timing", std::move(timing));
    return node;
}

Value MotionNewComposition(int32_t width, int32_t height, int32_t fpsNumerator,
                           int32_t fpsDenominator, int64_t duration,
                           bool scenes) {
    Value composition = Value::MakeObject();
    composition.Set("width", Value::MakeInt(width));
    composition.Set("height", Value::MakeInt(height));
    composition.Set("fps",
                    fpsDenominator == 1
                        ? Value::MakeInt(fpsNumerator)
                        : Value::MakeString(std::to_string(fpsNumerator) + "/" +
                                            std::to_string(fpsDenominator)));
    composition.Set("duration", Value::MakeInt(duration));
    Value root = Value::MakeObject();
    if (scenes) {
        root.Set("type", Value::MakeString("series"));
        Value children = Value::MakeArray();
        children.Push(MotionNewNode("sequence"));
        root.Set("children", std::move(children));
    } else {
        root.Set("type", Value::MakeString("fill"));
        Value style = Value::MakeObject();
        style.Set("justifyContent", Value::MakeString("center"));
        style.Set("alignItems", Value::MakeString("center"));
        root.Set("style", std::move(style));
        root.Set("children", Value::MakeArray());
    }
    composition.Set("root", std::move(root));
    return composition;
}

bool MotionDefinitionWithChild(const Value& definition,
                               const std::string& parentUid,
                               const std::string& afterUid, const Value& node,
                               Value& output, std::string& error) {
    MotionNodeLocation parent;
    if (!FindMotionNode(definition, parentUid, parent)) {
        return Fail(error, "the composition has no node '" + parentUid + "'");
    }
    const MotionTreePath children = Then(parent.path, "children");
    const Value* list = MotionTreeAt(definition, children);
    if (list == nullptr) {
        Value made = Value::MakeArray();
        made.Push(node);
        output = WithEntry(definition, parent.path, "children", made);
        return true;
    }
    if (!list->IsArray()) {
        return Fail(error, "node '" + parentUid + "' holds no children");
    }
    size_t before = list->AsArray().size();
    if (!afterUid.empty()) {
        for (size_t index = 0; index < list->AsArray().size(); ++index) {
            const Value* uid = list->AsArray()[index].Find("uid");
            if (uid != nullptr && uid->IsString() &&
                uid->AsString() == afterUid) {
                before = index + 1;
            }
        }
    }
    output = MotionTreeInsert(definition, children, before, node);
    return true;
}

bool MotionDefinitionWithScene(const Value& definition,
                               const std::string& seriesUid, const Value& scene,
                               int64_t duration, Value& output,
                               std::string& error) {
    MotionNodeLocation series;
    if (!FindMotionNode(definition, seriesUid, series)) {
        return Fail(error, "the composition has no node '" + seriesUid + "'");
    }
    const Value* children =
        MotionTreeAt(definition, Then(series.path, "children"));
    Value result = definition;
    if (children != nullptr && children->IsArray() &&
        !children->AsArray().empty()) {
        const size_t last = children->AsArray().size() - 1;
        const Value& previous = children->AsArray()[last];
        const Value* type = previous.Find("type");
        if (type != nullptr && type->IsString() &&
            type->AsString() == "sequence" &&
            previous.Find("duration") == nullptr) {
            MotionTreePath path = Then(series.path, "children");
            path.push_back({std::string(), last, true});
            result = WithEntry(result, path, "duration",
                               Value::MakeInt(std::max<int64_t>(1, duration)));
        }
    }
    return MotionDefinitionWithChild(result, seriesUid, std::string(), scene,
                                     output, error);
}

void MotionMonitorMapping::ToView(double x, double y, double& viewX,
                                  double& viewY) const {
    viewX = offset_x + x * scale_x;
    viewY = offset_y + y * scale_y;
}

void MotionMonitorMapping::FromView(double viewX, double viewY, double& x,
                                    double& y) const {
    x = scale_x != 0.0 ? (viewX - offset_x) / scale_x : 0.0;
    y = scale_y != 0.0 ? (viewY - offset_y) / scale_y : 0.0;
}

MotionMonitorMapping MotionMonitorMappingFor(const ClipFramingGeometry& framing,
                                             double canvasX, double canvasY,
                                             double canvasScale) {
    // Composition pixel -> sequence pixel: the source region onto its
    // rectangle. Sequence pixel -> view point: the canvas.
    const double sx = framing.source_width > 0
                          ? static_cast<double>(framing.width) /
                                static_cast<double>(framing.source_width)
                          : 1.0;
    const double sy = framing.source_height > 0
                          ? static_cast<double>(framing.height) /
                                static_cast<double>(framing.source_height)
                          : 1.0;
    MotionMonitorMapping mapping;
    mapping.scale_x = sx * canvasScale;
    mapping.scale_y = sy * canvasScale;
    mapping.offset_x = canvasX + (static_cast<double>(framing.x) -
                                  static_cast<double>(framing.source_x) * sx) *
                                     canvasScale;
    mapping.offset_y = canvasY + (static_cast<double>(framing.y) -
                                  static_cast<double>(framing.source_y) * sy) *
                                     canvasScale;
    return mapping;
}

bool MotionDefinitionWithAnimation(const Value& definition,
                                   const std::string& uid,
                                   const std::string& property,
                                   const std::optional<Value>& animation,
                                   Value& output, std::string& error) {
    MotionNodeLocation location;
    if (!FindMotionNode(definition, uid, location)) {
        return Fail(error, "the composition has no node '" + uid + "'");
    }
    if (animation.has_value()) {
        output = WithEntry(definition, Then(location.path, "animate"), property,
                           *animation);
        return true;
    }
    const MotionTreePath path = Then(Then(location.path, "animate"), property);
    output = MotionTreeAt(definition, path) != nullptr
                 ? MotionTreeWith(definition, path, std::nullopt)
                 : definition;
    return true;
}
