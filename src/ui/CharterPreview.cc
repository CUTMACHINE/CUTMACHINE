// CH5 -- docs/roadmap/CHARTE-2026-11.md. What the Charte panel renders, at
// which size and frame (CharterPreview.h). The piece is dressed exactly as
// add_motion_template dresses it, so that the preview never shows a
// charter the export would apply otherwise.

#include "CharterPreview.h"

#include "MotionComposition.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace ui::charter {

namespace {

using mcp_json::Value;

bool WholeNumber(const Value* value, int64_t& number) {
    return value != nullptr && value->IsNumber() && value->AsInt64(number);
}

// The text props an image node's `src` is bound to, under `node`, in the
// order the tree gives them, each once. A literal `src` is the piece's own
// file, not the charter's, and is left to the renderer.
void CollectImageProps(const Value& node, std::vector<std::string>& names) {
    if (!node.IsObject()) {
        return;
    }
    const Value* type = node.Find("type");
    const Value* src = node.Find("src");
    if (type != nullptr && type->IsString() && type->AsString() == "image" &&
        src != nullptr && src->IsObject() && src->AsObject().size() == 1) {
        const Value* prop = src->Find("prop");
        if (prop != nullptr && prop->IsString() &&
            std::find(names.begin(), names.end(), prop->AsString()) ==
                names.end()) {
            names.push_back(prop->AsString());
        }
    }
    if (const Value* children = node.Find("children");
        children != nullptr && children->IsArray()) {
        for (const Value& child : children->AsArray()) {
            CollectImageProps(child, names);
        }
    }
}

// The value prop `name` takes in format `format` of `definition`: the
// format's own value, else the declared default. Null when neither is set.
const Value* ValueInFormat(const Value& definition, const std::string& format,
                           const std::string& name) {
    if (const Value* formats = definition.Find("formats")) {
        if (const Value* entry = formats->Find(format)) {
            if (const Value* props = entry->Find("props")) {
                if (const Value* value = props->Find(name)) {
                    return value;
                }
            }
        }
    }
    if (const Value* props = definition.Find("props")) {
        if (const Value* prop = props->Find(name)) {
            return prop->Find("default");
        }
    }
    return nullptr;
}

bool IsTextProp(const Value& definition, const std::string& name) {
    const Value* props = definition.Find("props");
    const Value* prop = props != nullptr ? props->Find(name) : nullptr;
    const Value* type = prop != nullptr ? prop->Find("type") : nullptr;
    return type != nullptr && type->IsString() && type->AsString() == "text";
}

}  // namespace

const std::vector<PreviewFrame>& PreviewFrames() {
    static const std::vector<PreviewFrame> kFrames = {
        {1080, 1920, "9x16", "9:16"},
        {1920, 1080, "16x9", "16:9"},
        {1080, 1080, "1x1", "1:1"},
    };
    return kFrames;
}

std::string PieceFormatFor(const mcp_json::Value& definition, int32_t width,
                           int32_t height) {
    const Value* formats = definition.Find("formats");
    if (formats == nullptr || !formats->IsObject()) {
        return {};
    }
    for (const auto& [name, entry] : formats->AsObject()) {
        int64_t w = 0;
        int64_t h = 0;
        if (WholeNumber(entry.Find("width"), w) &&
            WholeNumber(entry.Find("height"), h) && w == width && h == height) {
            return name;
        }
    }
    return {};
}

int64_t RestFrame(const mcp_json::Value& definition) {
    int64_t duration = 0;
    if (!WholeNumber(definition.Find("duration"), duration) || duration <= 0) {
        return 0;
    }
    return duration / 2;
}

bool BuildPieceRequest(
    const MotionKitInfo& kit, const MotionTemplateInfo& piece,
    const PreviewFrame& frame, const std::string& placeholderImagePath,
    const std::function<bool(const std::string&)>& imageExists,
    PiecePreview& out, std::string& error) {
    out = PiecePreview{};
    out.piece = piece.name;
    out.frame = frame;
    out.format = PieceFormatFor(piece.definition, frame.width, frame.height);
    if (out.format.empty()) {
        error = "La pièce « " + piece.name + " » n’a pas de format " +
                frame.label + ".";
        return false;
    }
    out.definition = piece.definition;
    std::string why;
    if (!ApplyMotionKit(kit, piece.name, out.definition, out.application,
                        why)) {
        error = "La charte « " + kit.name +
                " » ne s’applique pas à la pièce « " + piece.name + " » (" +
                why + ").";
        return false;
    }
    MotionError motion;
    if (!ParseMotionComposition(out.definition, out.composition, motion)) {
        error = "La pièce « " + piece.name +
                " » habillée par la charte ne se lit plus (" + motion.message +
                ").";
        return false;
    }
    std::vector<std::string> images;
    if (const Value* root = out.definition.Find("root")) {
        CollectImageProps(*root, images);
    }
    Value props = Value::MakeObject();
    for (const std::string& name : images) {
        if (!IsTextProp(out.definition, name)) {
            continue;
        }
        const Value* value = ValueInFormat(out.definition, out.format, name);
        const std::string path = value != nullptr && value->IsString()
                                     ? value->AsString()
                                     : std::string();
        if (imageExists && imageExists(path)) {
            continue;
        }
        props.Set(name, Value::MakeString(placeholderImagePath));
        out.substituted.emplace_back(name, path);
    }
    if (!out.substituted.empty()) {
        out.props = std::move(props);
    }
    out.rest_frame = RestFrame(out.definition);
    return true;
}

MotionFrameRequest PieceFrameRequest(const PiecePreview& preview,
                                     double scale) {
    MotionFrameRequest request;
    request.composition = &preview.composition;
    request.format = preview.format;
    request.props = preview.props;
    request.frame = preview.rest_frame;
    request.scale = scale;
    return request;
}

mcp_json::Value PieceValues(const PiecePreview& preview) {
    Value values = Value::MakeObject();
    for (const MotionPropDefinition& prop : preview.composition.props) {
        if (prop.type != MotionPropType::Group) {
            values.Set(prop.name, prop.default_value);
        }
    }
    for (const MotionFormat& format : preview.composition.formats) {
        if (format.name == preview.format && format.props.IsObject()) {
            for (const auto& [name, value] : format.props.AsObject()) {
                values.Set(name, value);
            }
        }
    }
    if (preview.props.IsObject()) {
        for (const auto& [name, value] : preview.props.AsObject()) {
            values.Set(name, value);
        }
    }
    return values;
}

}  // namespace ui::charter
