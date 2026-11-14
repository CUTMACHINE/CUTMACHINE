#include "MotionComposition.h"

#include "Ulid.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <locale>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

bool Fail(MotionError& error, std::string code, std::string path,
          std::string message) {
    error = {std::move(code), std::move(path), std::move(message)};
    return false;
}

// A composition is data a model may write: a repeat bound to a list prop
// must not be able to ask for a million boxes.
constexpr int64_t kMaxRepeatCopies = 1000;

const char* const kTimeScaleMessage =
    "timeScale must be a positive number (a multiplier) or a percentage "
    "such as \"50%\", at most 1000";

bool CheckKeys(const Value& object, const std::vector<std::string>& allowed,
               const std::string& path, MotionError& error) {
    if (!object.IsObject()) {
        return Fail(error, "InvalidValue", path, "expected a JSON object");
    }
    for (const auto& entry : object.AsObject()) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            return Fail(error, "UnknownField", path + "." + entry.first,
                        "unknown field '" + entry.first + "'");
        }
    }
    return true;
}

bool OneOf(const std::string& text, const std::vector<std::string>& options) {
    return std::find(options.begin(), options.end(), text) != options.end();
}

std::string Join(const std::vector<std::string>& options) {
    std::string joined;
    for (const std::string& option : options) {
        if (!joined.empty()) {
            joined += ", ";
        }
        joined += option;
    }
    return joined;
}

enum class StyleKind {
    Keyword,
    Length,
    LengthOrAuto,
    LengthShorthand,
    LengthOrAutoShorthand,
    Number,
    UnitInterval,
    Pixels,
    FontWeight,
    LineHeight,
    Color,
    String,
    // M1b: a colour, "none" or "currentColor" (SVG fill and stroke).
    Paint,
    // M1b: clip-path, "none" or polygon(...).
    Polygon,
    // M1b: -webkit-text-stroke, "none" or "<length> <color>".
    TextStroke,
    // REM2: a sound's volume, a linear multiplier from 0 to 4 (+12 dB).
    Volume,
    // REM3: "none" or a linear-gradient() / radial-gradient().
    Gradient,
    // REM3: "none" or a list of blur(), drop-shadow(), glow(), displace().
    Filter,
    // REM4: CSS perspective, "none" or a positive distance in pixels.
    Distance,
    // MM2: CSS font-variant-numeric, "normal" or the figures a font shows
    // (lining-nums, oldstyle-nums) and how wide (tabular-nums,
    // proportional-nums).
    NumericVariant,
    // MA7: SVG stroke-dasharray, "none" or lengths in pixels
    // (ParseMotionDashArray).
    DashArray,
};

struct StyleRule {
    const char* name;
    StyleKind kind;
    std::vector<std::string> keywords;
    bool animatable;
};

const std::vector<StyleRule>& StyleRules() {
    static const std::vector<StyleRule> rules = {
        {"flexDirection",
         StyleKind::Keyword,
         {"row", "row-reverse", "column", "column-reverse"},
         false},
        {"justifyContent",
         StyleKind::Keyword,
         {"flex-start", "center", "flex-end", "space-between", "space-around",
          "space-evenly"},
         false},
        {"alignItems",
         StyleKind::Keyword,
         {"flex-start", "center", "flex-end", "stretch", "baseline"},
         false},
        {"alignSelf",
         StyleKind::Keyword,
         {"auto", "flex-start", "center", "flex-end", "stretch", "baseline"},
         false},
        {"alignContent",
         StyleKind::Keyword,
         {"flex-start", "center", "flex-end", "stretch", "space-between",
          "space-around", "space-evenly"},
         false},
        {"flexWrap",
         StyleKind::Keyword,
         {"nowrap", "wrap", "wrap-reverse"},
         false},
        {"position", StyleKind::Keyword, {"relative", "absolute"}, false},
        {"flexGrow", StyleKind::Number, {}, true},
        {"flexShrink", StyleKind::Number, {}, true},
        {"flexBasis", StyleKind::LengthOrAuto, {}, false},
        {"width", StyleKind::LengthOrAuto, {}, true},
        {"height", StyleKind::LengthOrAuto, {}, true},
        {"minWidth", StyleKind::Length, {}, true},
        {"minHeight", StyleKind::Length, {}, true},
        {"maxWidth", StyleKind::Length, {}, true},
        {"maxHeight", StyleKind::Length, {}, true},
        {"top", StyleKind::Length, {}, true},
        {"right", StyleKind::Length, {}, true},
        {"bottom", StyleKind::Length, {}, true},
        {"left", StyleKind::Length, {}, true},
        {"inset", StyleKind::LengthShorthand, {}, false},
        {"gap", StyleKind::Length, {}, true},
        {"rowGap", StyleKind::Length, {}, true},
        {"columnGap", StyleKind::Length, {}, true},
        {"padding", StyleKind::LengthShorthand, {}, false},
        {"paddingTop", StyleKind::Length, {}, true},
        {"paddingRight", StyleKind::Length, {}, true},
        {"paddingBottom", StyleKind::Length, {}, true},
        {"paddingLeft", StyleKind::Length, {}, true},
        {"margin", StyleKind::LengthOrAutoShorthand, {}, false},
        {"marginTop", StyleKind::LengthOrAuto, {}, true},
        {"marginRight", StyleKind::LengthOrAuto, {}, true},
        {"marginBottom", StyleKind::LengthOrAuto, {}, true},
        {"marginLeft", StyleKind::LengthOrAuto, {}, true},
        {"backgroundColor", StyleKind::Color, {}, true},
        {"borderRadius", StyleKind::Pixels, {}, true},
        {"opacity", StyleKind::UnitInterval, {}, true},
        {"translateX", StyleKind::Pixels, {}, true},
        {"translateY", StyleKind::Pixels, {}, true},
        {"scale", StyleKind::Number, {}, true},
        {"scaleX", StyleKind::Number, {}, true},
        {"scaleY", StyleKind::Number, {}, true},
        {"rotate", StyleKind::Number, {}, true},
        {"color", StyleKind::Color, {}, true},
        {"fontFamily", StyleKind::String, {}, false},
        {"fontWeight", StyleKind::FontWeight, {}, false},
        {"fontSize", StyleKind::Pixels, {}, true},
        {"lineHeight", StyleKind::LineHeight, {}, true},
        {"letterSpacing", StyleKind::Pixels, {}, true},
        {"textTransform",
         StyleKind::Keyword,
         {"none", "uppercase", "lowercase", "capitalize"},
         false},
        {"textAlign", StyleKind::Keyword, {"left", "center", "right"}, false},
        {"objectFit",
         StyleKind::Keyword,
         {"contain", "cover", "fill", "none"},
         false},
        // M1b -- text outline, word spacing, clip-path and SVG painting.
        {"textStroke", StyleKind::TextStroke, {}, false},
        {"wordSpacing", StyleKind::Pixels, {}, true},
        {"clipPath", StyleKind::Polygon, {}, true},
        {"fill", StyleKind::Paint, {}, true},
        {"stroke", StyleKind::Paint, {}, true},
        {"strokeWidth", StyleKind::Pixels, {}, true},
        {"strokeLinecap",
         StyleKind::Keyword,
         {"butt", "round", "square"},
         false},
        {"strokeLinejoin",
         StyleKind::Keyword,
         {"miter", "round", "bevel"},
         false},
        {"fillRule", StyleKind::Keyword, {"nonzero", "evenodd"}, false},
        {"trimStart", StyleKind::UnitInterval, {}, true},
        {"trimEnd", StyleKind::UnitInterval, {}, true},
        // REM2 -- CSS overflow: a box that hides what overflows it clips its
        // descendants to its border box, rounded by its borderRadius. And
        // the volume of a video or an audio node, animatable for fades.
        {"overflow", StyleKind::Keyword, {"visible", "hidden"}, false},
        {"volume", StyleKind::Volume, {}, true},
        // REMOTION-PARITY-2026-09 REM3 -- paint and effects: a gradient
        // under the box's content, a gradient that masks it, a stack of
        // raster effects, and the italic a typography needs.
        {"backgroundImage", StyleKind::Gradient, {}, true},
        {"maskImage", StyleKind::Gradient, {}, true},
        {"filter", StyleKind::Filter, {}, true},
        {"fontStyle", StyleKind::Keyword, {"normal", "italic"}, false},
        // MOTION-MARQUE-2026-10 MM2 -- the figures a font shows: a face
        // whose default figures are old-style reads « 1 point » as « I
        // point ». Inherited, as CSS inherits it.
        {"fontVariantNumeric", StyleKind::NumericVariant, {}, false},
        // REMOTION-PARITY-2026-09 REM4 -- 2.5D: a node turns about its own
        // horizontal and vertical axes, and a parent's perspective gives
        // its children depth, as in CSS.
        {"rotateX", StyleKind::Number, {}, true},
        {"rotateY", StyleKind::Number, {}, true},
        {"perspective", StyleKind::Distance, {}, true},
        // MOTION-DESIGN-2026-09 -- CSS mix-blend-mode: the node and its
        // descendants composited as one group onto what lies under it.
        {"mixBlendMode",
         StyleKind::Keyword,
         {"normal", "multiply", "screen", "overlay", "darken", "lighten",
          "color-dodge", "color-burn", "hard-light", "soft-light", "difference",
          "exclusion", "plus-lighter",
          // Track mattes, in an isolated group: keep what lies under the
          // node inside it (mask-in), outside it (mask-out), or paint the
          // node only over what lies under it (atop).
          "mask-in", "mask-out", "atop"},
         false},
        // CSS isolation: the node and its descendants composited as one
        // group, so that a matte or a blend inside it stops at its edge.
        {"isolation", StyleKind::Keyword, {"auto", "isolate"}, false},
        // SVG dashes on a path's stroke: a list of lengths, and where the
        // pattern starts (animatable: marching ants).
        {"strokeDasharray", StyleKind::DashArray, {}, false},
        {"strokeDashoffset", StyleKind::Pixels, {}, true},
    };
    return rules;
}

const StyleRule* FindStyleRule(const std::string& name) {
    for (const StyleRule& rule : StyleRules()) {
        if (name == rule.name) {
            return &rule;
        }
    }
    return nullptr;
}

// MOTION-BIBLIOTHEQUE-2026-10 MB6 -- the CSS a model reaches for that this
// format spells otherwise or does not have. Naming the property is not
// enough: the refusal says what to write instead, so the next attempt is
// the right one rather than another guess.
std::string UnknownStyleMessage(const std::string& name) {
    // background-color, as CSS writes it, for backgroundColor.
    std::string camel;
    bool upper = false;
    for (const char c : name) {
        if (c == '-') {
            upper = true;
            continue;
        }
        camel += upper && c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c;
        upper = false;
    }
    std::string message = "'" + name + "' is not a supported CSS property name";
    if (camel != name && FindStyleRule(camel) != nullptr) {
        return message + ": style names are camelCase, write '" + camel + "'";
    }
    struct Hint {
        const char* name;
        const char* instead;
    };
    static constexpr Hint kHints[] = {
        {"transformOrigin",
         "scale and rotate turn around the centre of the box. To grow from "
         "an edge, animate width or height, or a clipPath inset(), or "
         "compensate with translateX and translateY"},
        {"transform",
         "transformations are properties of their own: translateX, "
         "translateY, scale, scaleX, scaleY, rotate, rotateX, rotateY"},
        {"translate", "write translateX and translateY"},
        {"whiteSpace",
         "a text wraps at the width of its box. To keep a line whole, put "
         "it in a box wider than the line, with flexShrink: 0"},
        {"textWrap",
         "a text wraps at the width of its box. To keep a line whole, put "
         "it in a box wider than the line, with flexShrink: 0"},
        {"boxShadow",
         "write filter: \"drop-shadow(x y blur colour)\", or glow(radius "
         "colour strength)"},
        {"textShadow",
         "write filter: \"drop-shadow(x y blur colour)\", or glow(radius "
         "colour strength)"},
        {"display",
         "every box is a flex container. Hide a node with its `visible` "
         "field, which frees its place, or with opacity, which keeps it"},
        {"visibility",
         "hide a node with its `visible` field, which frees its place, or "
         "with opacity, which keeps it"},
        {"zIndex", "nodes paint in the order of `children`: reorder them"},
        {"transition",
         "motion is written in the node's `animate`, as interpolate or "
         "spring"},
        {"animation",
         "motion is written in the node's `animate`, as interpolate or "
         "spring"},
        {"border",
         "there are no borders. Nest two boxes, the outer one's padding "
         "being the border, or draw a path with a stroke"},
        {"borderWidth",
         "there are no borders. Nest two boxes, the outer one's padding "
         "being the border, or draw a path with a stroke"},
        {"borderColor",
         "there are no borders. Nest two boxes, the outer one's padding "
         "being the border, or draw a path with a stroke"},
        {"outline",
         "there are no outlines. Nest two boxes, or draw a path with a "
         "stroke"},
        {"background",
         "write backgroundColor for a colour, backgroundImage for a "
         "gradient"},
        {"textDecoration",
         "there is no text decoration. Draw the line as a box or a path "
         "under the text"},
    };
    for (const Hint& hint : kHints) {
        if (camel == hint.name) {
            return message + ": " + hint.instead;
        }
    }
    return message;
}

std::vector<std::string> SplitSpaces(const std::string& text) {
    std::istringstream stream(text);
    std::vector<std::string> parts;
    std::string part;
    while (stream >> part) {
        parts.push_back(part);
    }
    return parts;
}

bool ParseDecimal(const std::string& text, double& number) {
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    stream >> number;
    return !stream.fail() && stream.eof() && std::isfinite(number);
}

bool ParseLengthText(const std::string& text, bool allow_auto,
                     MotionLength& length) {
    if (text == "auto") {
        if (!allow_auto) {
            return false;
        }
        length = {MotionLengthUnit::Auto, 0.0};
        return true;
    }
    double number = 0.0;
    if (text.size() > 1 && text.back() == '%') {
        if (!ParseDecimal(text.substr(0, text.size() - 1), number)) {
            return false;
        }
        length = {MotionLengthUnit::Percent, number};
        return true;
    }
    if (text.size() > 2 && text.compare(text.size() - 2, 2, "px") == 0) {
        if (!ParseDecimal(text.substr(0, text.size() - 2), number)) {
            return false;
        }
        length = {MotionLengthUnit::Px, number};
        return true;
    }
    if (text == "0") {
        length = {MotionLengthUnit::Px, 0.0};
        return true;
    }
    return false;
}

std::string SyntheticKeyframeId(size_t index) {
    static const char kCrockford[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string id(26, '0');
    for (size_t position = 25; index > 0 && position > 21; --position) {
        id[position] = kCrockford[index % 32];
        index /= 32;
    }
    return id;
}

bool ParseEasing(const std::string& text, KeyframeInterpolation& interpolation,
                 KeyframeEasing& easing) {
    if (text == "linear") {
        interpolation = KeyframeInterpolation::Linear;
        return true;
    }
    if (text == "step-end") {
        interpolation = KeyframeInterpolation::Hold;
        return true;
    }
    interpolation = KeyframeInterpolation::CubicBezier;
    // The CSS keyword curves, with their specified control points.
    if (text == "ease") {
        easing = {0.25, 0.1, 0.25, 1.0};
        return true;
    }
    if (text == "ease-in") {
        easing = {0.42, 0.0, 1.0, 1.0};
        return true;
    }
    if (text == "ease-out") {
        easing = {0.0, 0.0, 0.58, 1.0};
        return true;
    }
    if (text == "ease-in-out") {
        easing = {0.42, 0.0, 0.58, 1.0};
        return true;
    }
    const std::string prefix = "cubic-bezier(";
    if (text.compare(0, prefix.size(), prefix) != 0 || text.back() != ')') {
        return false;
    }
    std::string inner =
        text.substr(prefix.size(), text.size() - prefix.size() - 1);
    std::replace(inner.begin(), inner.end(), ',', ' ');
    const std::vector<std::string> parts = SplitSpaces(inner);
    if (parts.size() != 4) {
        return false;
    }
    double points[4] = {};
    for (size_t index = 0; index < 4; ++index) {
        if (!ParseDecimal(parts[index], points[index])) {
            return false;
        }
    }
    easing = {points[0], points[1], points[2], points[3]};
    return easing.x1 >= 0.0 && easing.x1 <= 1.0 && easing.x2 >= 0.0 &&
           easing.x2 <= 1.0;
}

// REM3 -- two gradients mix stop by stop only when they have the same
// kind, the same form (an angle, or the same corner) and as many stops.
bool SameGradientShape(const MotionGradient& a, const MotionGradient& b) {
    return a.kind == b.kind && a.stops.size() == b.stops.size() &&
           a.corner_x == b.corner_x && a.corner_y == b.corner_y &&
           a.circle == b.circle && a.extent == b.extent;
}

// REM3 -- two filter lists mix effect by effect, as CSS interpolates two
// filter lists of the same functions.
bool SameFilterList(const std::vector<MotionFilter>& a,
                    const std::vector<MotionFilter>& b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t index = 0; index < a.size(); ++index) {
        if (a[index].kind != b[index].kind ||
            ((a[index].kind == MotionFilterKind::Displace ||
              a[index].kind == MotionFilterKind::Grain ||
              a[index].kind == MotionFilterKind::Slice) &&
             a[index].seed != b[index].seed)) {
            return false;
        }
        // mesh() moves each vertex along a straight line: the same grid,
        // the same tessellation.
        if (a[index].kind == MotionFilterKind::Mesh &&
            (a[index].mesh_rows != b[index].mesh_rows ||
             a[index].mesh_columns != b[index].mesh_columns ||
             a[index].mesh_quality != b[index].mesh_quality)) {
            return false;
        }
    }
    return true;
}

class Parser {
public:
    Parser(MotionComposition& composition, MotionError& error)
        : composition_(composition), error_(error) {}

    bool Composition(const Value& root) {
        if (!CheckKeys(root,
                       {"width", "height", "fps", "duration", "props", "tokens",
                        "formats", "variants", "motionBlur", "grain", "root"},
                       "composition", error_)) {
            return false;
        }
        if (!PositiveInt32(root, "width", composition_.width) ||
            !PositiveInt32(root, "height", composition_.height) || !Fps(root) ||
            !Duration(root) || !Props(root) || !Tokens(root) ||
            !Formats(root) || !Variants(root) || !MotionBlur(root) ||
            !Grain(root)) {
            return false;
        }
        const Value* tree = root.Find("root");
        if (tree == nullptr) {
            return Fail(error_, "InvalidComposition", "composition.root",
                        "a composition needs a root node");
        }
        return Node(*tree, "root", composition_.root);
    }

private:
    bool PositiveInt32(const Value& object, const std::string& key,
                       int32_t& output,
                       const std::string& where = "composition") {
        const Value* field = object.Find(key);
        int64_t number = 0;
        if (field == nullptr || !field->IsNumber() || !field->AsInt64(number) ||
            number <= 0 || number > 16384) {
            return Fail(error_, "InvalidValue", where + "." + key,
                        key + " must be an integer between 1 and 16384");
        }
        output = static_cast<int32_t>(number);
        return true;
    }

    bool Fps(const Value& root) {
        const Value* fps = root.Find("fps");
        int64_t numerator = 0;
        int64_t denominator = 1;
        bool valid = false;
        if (fps != nullptr && fps->IsNumber() && fps->AsInt64(numerator)) {
            valid = true;
        } else if (fps != nullptr && fps->IsString()) {
            const std::string& text = fps->AsString();
            const size_t slash = text.find('/');
            char* end = nullptr;
            if (slash != std::string::npos) {
                numerator = std::strtoll(text.c_str(), &end, 10);
                valid = end == text.c_str() + slash;
                denominator = std::strtoll(text.c_str() + slash + 1, &end, 10);
                valid = valid && *end == '\0';
            }
        }
        if (!valid || numerator <= 0 || denominator <= 0 ||
            numerator > 1000000 || denominator > 1000000) {
            return Fail(error_, "InvalidValue", "composition.fps",
                        "fps must be a positive integer or \"numerator/"
                        "denominator\"");
        }
        composition_.fps_numerator = static_cast<int32_t>(numerator);
        composition_.fps_denominator = static_cast<int32_t>(denominator);
        return true;
    }

    // MOTION-DESIGN-2026-09 -- {"samples": 2..64, "shutterAngle": (0, 360]}.
    bool MotionBlur(const Value& root) {
        const Value* blur = root.Find("motionBlur");
        if (blur == nullptr) {
            return true;
        }
        if (!CheckKeys(*blur, {"samples", "shutterAngle"},
                       "composition.motionBlur", error_)) {
            return false;
        }
        const Value* samples = blur->Find("samples");
        int64_t count = 0;
        if (samples == nullptr || !samples->IsNumber() ||
            !samples->AsInt64(count) || count < 2 || count > 64) {
            return Fail(error_, "InvalidValue",
                        "composition.motionBlur.samples",
                        "samples must be an integer between 2 and 64");
        }
        double angle = 180.0;
        if (const Value* shutter = blur->Find("shutterAngle")) {
            if (!MotionNumber(*shutter, angle) || !(angle > 0.0) ||
                angle > 360.0) {
                return Fail(error_, "InvalidValue",
                            "composition.motionBlur.shutterAngle",
                            "shutterAngle must be a number in (0, 360]");
            }
        }
        composition_.motion_blur_samples = static_cast<int32_t>(count);
        composition_.motion_blur_angle = angle;
        return true;
    }

    // MOTION-DESIGN-2026-09 -- {"amount": 0..1, "size": 1..64, "seed": n}.
    bool Grain(const Value& root) {
        const Value* grain = root.Find("grain");
        if (grain == nullptr) {
            return true;
        }
        if (!CheckKeys(*grain, {"amount", "size", "seed"}, "composition.grain",
                       error_)) {
            return false;
        }
        double amount = 0.0;
        double size = 1.0;
        int64_t seed = 0;
        const Value* amountValue = grain->Find("amount");
        const Value* sizeValue = grain->Find("size");
        const Value* seedValue = grain->Find("seed");
        if (amountValue == nullptr || !MotionNumber(*amountValue, amount) ||
            amount < 0.0 || amount > 1.0 ||
            (sizeValue != nullptr &&
             (!MotionNumber(*sizeValue, size) || size < 1.0 || size > 64.0)) ||
            (seedValue != nullptr && (!seedValue->AsInt64(seed) || seed < 0))) {
            return Fail(error_, "InvalidValue", "composition.grain",
                        "grain is {amount: 0 to 1, size: 1 to 64 pixels, "
                        "seed: an integer >= 0}");
        }
        composition_.grain_amount = amount;
        composition_.grain_size = size;
        composition_.grain_seed = seed;
        return true;
    }

    bool Duration(const Value& root) {
        const Value* duration = root.Find("duration");
        if (duration == nullptr || !duration->IsNumber() ||
            !duration->AsInt64(composition_.duration) ||
            composition_.duration <= 0) {
            return Fail(error_, "InvalidValue", "composition.duration",
                        "duration must be a positive number of frames");
        }
        return true;
    }

    bool Props(const Value& root) {
        const Value* props = root.Find("props");
        if (props == nullptr) {
            return true;
        }
        if (!props->IsObject()) {
            return Fail(error_, "InvalidValue", "composition.props",
                        "props must be an object");
        }
        for (const auto& entry : props->AsObject()) {
            const std::string path = "props." + entry.first;
            if (!CheckKeys(entry.second,
                           {"type", "default", "min", "max", "values", "unit",
                            "of", "props"},
                           path, error_)) {
                return false;
            }
            // M3 -- "position.x" reads a component of a prop: a name that
            // holds a dot could never be bound.
            if (entry.first.empty() ||
                entry.first.find('.') != std::string::npos) {
                return Fail(error_, "InvalidValue", path,
                            "a prop name is non-empty and holds no '.'");
            }
            MotionPropDefinition definition;
            definition.name = entry.first;
            const Value* type = entry.second.Find("type");
            const std::string typeName =
                type != nullptr && type->IsString() ? type->AsString() : "";
            if (typeName == "text") {
                definition.type = MotionPropType::Text;
                definition.default_value = Value::MakeString("");
            } else if (typeName == "number") {
                definition.type = MotionPropType::Number;
                definition.default_value = Value::MakeInt(0);
            } else if (typeName == "color") {
                definition.type = MotionPropType::Color;
                definition.default_value = Value::MakeString("#000000");
            } else if (typeName == "bool") {
                definition.type = MotionPropType::Bool;
                definition.default_value = Value::MakeBool(false);
            } else if (typeName == "enum") {
                definition.type = MotionPropType::Enum;
            } else if (typeName == "list") {
                definition.type = MotionPropType::List;
                definition.default_value = Value::MakeArray();
            } else if (typeName == "point") {
                // M3 -- the value a JavaScript template would read as
                // props.position.x and props.position.y.
                definition.type = MotionPropType::Point;
                definition.default_value = Value::MakeObject();
                definition.default_value.Set("x", Value::MakeInt(0));
                definition.default_value.Set("y", Value::MakeInt(0));
            } else if (typeName == "size") {
                definition.type = MotionPropType::Size;
                definition.default_value = Value::MakeObject();
                definition.default_value.Set("width", Value::MakeInt(0));
                definition.default_value.Set("height", Value::MakeInt(0));
            } else if (typeName == "group") {
                definition.type = MotionPropType::Group;
            } else {
                return Fail(error_, "InvalidValue", path + ".type",
                            "type must be text, number, color, bool, enum, "
                            "list, point, size or group");
            }
            if (definition.type == MotionPropType::Group) {
                if (!GroupMembers(entry.second, path, definition)) {
                    return false;
                }
                composition_.props.push_back(std::move(definition));
                continue;
            }
            if (entry.second.Find("props") != nullptr) {
                return Fail(error_, "InvalidValue", path + ".props",
                            "props applies to a group prop");
            }
            const Value* of = entry.second.Find("of");
            if (definition.type == MotionPropType::List) {
                const std::string element =
                    of != nullptr && of->IsString() ? of->AsString() : "";
                if (element == "text") {
                    definition.element_type = MotionPropType::Text;
                } else if (element == "number") {
                    definition.element_type = MotionPropType::Number;
                } else if (element == "color") {
                    definition.element_type = MotionPropType::Color;
                } else {
                    return Fail(error_, "InvalidValue", path + ".of",
                                "a list prop needs of: text, number or color");
                }
            } else if (of != nullptr) {
                return Fail(error_, "InvalidValue", path + ".of",
                            "of applies to a list prop");
            }
            const bool spatial = definition.type == MotionPropType::Point ||
                                 definition.type == MotionPropType::Size;
            if (const Value* unit = entry.second.Find("unit")) {
                if ((definition.type != MotionPropType::Number && !spatial) ||
                    !unit->IsString() ||
                    !OneOf(unit->AsString(), {"px", "%"})) {
                    return Fail(error_, "InvalidValue", path + ".unit",
                                "unit applies to a number, point or size prop "
                                "and is px or %");
                }
                definition.unit = unit->AsString();
            }
            const bool numeric =
                definition.type == MotionPropType::Number || spatial ||
                (definition.type == MotionPropType::List &&
                 definition.element_type == MotionPropType::Number);
            for (const char* bound : {"min", "max"}) {
                const Value* limit = entry.second.Find(bound);
                if (limit == nullptr) {
                    continue;
                }
                double number = 0.0;
                if (!numeric || !MotionNumber(*limit, number)) {
                    return Fail(error_, "InvalidValue", path + "." + bound,
                                std::string(bound) +
                                    " applies to a number, a point, a size or "
                                    "a list of numbers and must be a number");
                }
                (std::string(bound) == "min" ? definition.minimum
                                             : definition.maximum) = number;
            }
            const Value* values = entry.second.Find("values");
            if (definition.type == MotionPropType::Enum) {
                std::set<std::string> seen;
                if (values == nullptr || !values->IsArray() ||
                    values->AsArray().empty()) {
                    return Fail(error_, "InvalidValue", path + ".values",
                                "an enum prop needs a non-empty values array");
                }
                for (const Value& option : values->AsArray()) {
                    if (!option.IsString() ||
                        !seen.insert(option.AsString()).second) {
                        return Fail(error_, "InvalidValue", path + ".values",
                                    "enum values must be distinct strings");
                    }
                    definition.values.push_back(option.AsString());
                }
                definition.default_value =
                    Value::MakeString(definition.values.front());
            } else if (values != nullptr) {
                return Fail(error_, "InvalidValue", path + ".values",
                            "values applies to an enum prop");
            }
            const Value* fallback = entry.second.Find("default");
            if (fallback != nullptr) {
                definition.default_value = *fallback;
            }
            composition_.props.push_back(std::move(definition));
            if (!ValidateMotionPropValue(
                    composition_.props.back(),
                    composition_.props.back().default_value, path + ".default",
                    error_)) {
                return false;
            }
        }
        return GroupsResolve();
    }

    // M3 -- a group names the props it gathers; it has no value of its own.
    bool GroupMembers(const Value& raw, const std::string& path,
                      MotionPropDefinition& definition) {
        if (raw.Find("default") != nullptr) {
            return Fail(error_, "PropTypeMismatch", path + ".default",
                        "a group prop has no value: set the props it gathers");
        }
        for (const char* key : {"min", "max", "values", "unit", "of"}) {
            if (raw.Find(key) != nullptr) {
                return Fail(error_, "InvalidValue", path + "." + key,
                            "a group prop has no value: it only gathers the "
                            "props it names");
            }
        }
        const Value* members = raw.Find("props");
        if (members == nullptr || !members->IsArray() ||
            members->AsArray().empty()) {
            return Fail(error_, "InvalidValue", path + ".props",
                        "a group prop needs a non-empty props array");
        }
        for (const Value& member : members->AsArray()) {
            if (!member.IsString() ||
                OneOf(member.AsString(), definition.members)) {
                return Fail(error_, "InvalidValue", path + ".props",
                            "a group names distinct props");
            }
            definition.members.push_back(member.AsString());
        }
        return true;
    }

    // Every member of a group is a declared prop that is not itself a group,
    // and belongs to that group only.
    bool GroupsResolve() {
        std::set<std::string> grouped;
        for (const MotionPropDefinition& group : composition_.props) {
            if (group.type != MotionPropType::Group) {
                continue;
            }
            for (const std::string& member : group.members) {
                const MotionPropDefinition* found = FindProp(member);
                const std::string path = "props." + group.name + ".props";
                if (found == nullptr) {
                    return Fail(error_, "UnknownProp", path,
                                "group '" + group.name + "' names no prop '" +
                                    member + "'");
                }
                if (found->type == MotionPropType::Group) {
                    return Fail(error_, "InvalidValue", path,
                                "a group cannot gather another group");
                }
                if (!grouped.insert(member).second) {
                    return Fail(error_, "InvalidValue", path,
                                "prop '" + member +
                                    "' already belongs to another group");
                }
            }
        }
        return true;
    }

    bool Tokens(const Value& root) {
        const Value* tokens = root.Find("tokens");
        if (tokens == nullptr) {
            return true;
        }
        if (!tokens->IsObject()) {
            return Fail(error_, "InvalidValue", "composition.tokens",
                        "tokens must be an object of tables");
        }
        for (const auto& table : tokens->AsObject()) {
            if (!table.second.IsObject()) {
                return Fail(error_, "InvalidValue", "tokens." + table.first,
                            "a token table must be an object");
            }
            MotionTokenTable parsed{table.first, {}};
            for (const auto& entry : table.second.AsObject()) {
                if (!entry.second.IsString() && !entry.second.IsNumber() &&
                    !entry.second.IsBool()) {
                    return Fail(error_, "InvalidValue",
                                "tokens." + table.first + "." + entry.first,
                                "token values must be strings, numbers or "
                                "booleans");
                }
                parsed.entries.push_back(entry);
            }
            composition_.tokens.push_back(std::move(parsed));
        }
        return true;
    }

    // M6 -- the prop values a format or a version sets, each checked against
    // its declared prop as a caller's override is: the three named refusals
    // of M3, at the value's own path.
    bool PropSet(const Value* raw, const std::string& path, Value& output) {
        output = Value::MakeObject();
        if (raw == nullptr) {
            return true;
        }
        if (!raw->IsObject()) {
            return Fail(error_, "InvalidValue", path,
                        "props must be an object of prop values");
        }
        for (const auto& [name, value] : raw->AsObject()) {
            std::string at = path;
            at += ".";
            at += name;
            const MotionPropDefinition* definition = FindProp(name);
            if (definition == nullptr) {
                return Fail(error_, "UnknownProp", at,
                            "the composition declares no prop '" + name + "'");
            }
            if (!ValidateMotionPropValue(*definition, value, at, error_)) {
                return false;
            }
            output.Set(name, value);
        }
        return true;
    }

    bool SetName(const std::string& name, const std::string& path) {
        if (IsMotionSetName(name)) {
            return true;
        }
        return Fail(error_, "InvalidValue", path,
                    "a name is 1 to 64 ASCII letters, digits, '-' or '_': it "
                    "becomes part of the rendered file names");
    }

    // M6 -- the sizes a composition also renders at. A size alone rarely
    // makes a card: the reference title card of M2 is the same intent at
    // twice the type size in 16:9, so a format carries the prop values that
    // adapt it (a type size, a gap), and flexbox and percentages do the rest.
    bool Formats(const Value& root) {
        const Value* formats = root.Find("formats");
        if (formats == nullptr) {
            return true;
        }
        if (!formats->IsObject()) {
            return Fail(error_, "InvalidValue", "composition.formats",
                        "formats must be an object of named sizes");
        }
        for (const auto& [name, raw] : formats->AsObject()) {
            const std::string path = "formats." + name;
            MotionFormat format;
            format.name = name;
            if (!SetName(name, path) ||
                !CheckKeys(raw, {"width", "height", "props"}, path, error_) ||
                !PositiveInt32(raw, "width", format.width, path) ||
                !PositiveInt32(raw, "height", format.height, path) ||
                !PropSet(raw.Find("props"), path + ".props", format.props)) {
                return false;
            }
            composition_.formats.push_back(std::move(format));
        }
        return true;
    }

    // M6 -- the versions of a composition: named sets of prop values. A prop
    // a format adapts cannot also vary by version: its value would then
    // depend on an order of precedence nobody wrote down. Refusing it now
    // leaves that order free to decide later without changing any stored
    // composition's rendering.
    bool Variants(const Value& root) {
        const Value* variants = root.Find("variants");
        if (variants == nullptr) {
            return true;
        }
        if (!variants->IsObject()) {
            return Fail(error_, "InvalidValue", "composition.variants",
                        "variants must be an object of named prop sets");
        }
        for (const auto& [name, raw] : variants->AsObject()) {
            const std::string path = "variants." + name;
            MotionVariant variant;
            variant.name = name;
            if (!SetName(name, path) ||
                !CheckKeys(raw, {"props"}, path, error_) ||
                !PropSet(raw.Find("props"), path + ".props", variant.props)) {
                return false;
            }
            for (const auto& entry : variant.props.AsObject()) {
                for (const MotionFormat& format : composition_.formats) {
                    if (format.props.Find(entry.first) != nullptr) {
                        return Fail(
                            error_, "InvalidComposition",
                            path + ".props." + entry.first,
                            "prop '" + entry.first + "' is set by format '" +
                                format.name +
                                "' too: a prop is adapted by the formats or "
                                "varied by the versions, not both");
                    }
                }
            }
            composition_.variants.push_back(std::move(variant));
        }
        return true;
    }

    const MotionPropDefinition* FindProp(const std::string& name) const {
        for (const MotionPropDefinition& prop : composition_.props) {
            if (prop.name == name) {
                return &prop;
            }
        }
        return nullptr;
    }

    // Parses a literal or a binding. Bound style values are validated when
    // evaluated, because only then is the prop's value known.
    bool Binding(const Value& raw, const std::string& path, MotionValue& out) {
        if (!raw.IsObject()) {
            out.kind = MotionBindingKind::Literal;
            out.literal = raw;
            return true;
        }
        if (const Value* timecode = raw.Find("timecode")) {
            // MOTION-AFTER-2026-09 -- {"timecode": "composition" | "local",
            // "offset": frames}.
            const Value* offset = raw.Find("offset");
            if (!CheckKeys(raw, {"timecode", "offset"}, path, error_)) {
                return false;
            }
            if (!timecode->IsString() ||
                !OneOf(timecode->AsString(), {"composition", "local"})) {
                return Fail(error_, "InvalidValue", path + ".timecode",
                            "timecode is \"composition\" or \"local\"");
            }
            // MA7 -- at most 2^53 either way: what After's expression, in
            // JavaScript's doubles, holds exactly, and far enough from the
            // ends of int64 that adding a frame never overflows.
            constexpr int64_t kFarthestOffset = int64_t{1} << 53;
            if (offset != nullptr && (!offset->AsInt64(out.timecode_offset) ||
                                      out.timecode_offset < -kFarthestOffset ||
                                      out.timecode_offset > kFarthestOffset)) {
                return Fail(error_, "InvalidValue", path + ".offset",
                            "offset is a whole number of frames, at most "
                            "2^53 either way");
            }
            out.kind = MotionBindingKind::Timecode;
            out.timecode_scope = timecode->AsString();
            return true;
        }
        if (!CheckKeys(raw, {"prop", "map", "notEmpty", "item", "index"}, path,
                       error_)) {
            return false;
        }
        const Value* item = raw.Find("item");
        const Value* index = raw.Find("index");
        if (item != nullptr || index != nullptr) {
            return RepeatBinding(raw, item, index, path, out);
        }
        const Value* prop = raw.Find("prop");
        if (prop == nullptr || !prop->IsString()) {
            return Fail(error_, "InvalidValue", path,
                        "a binding needs {\"prop\": name}, {\"item\": true} "
                        "or {\"index\": true}");
        }
        // M3 -- "position.x": a component of a point or size prop, spelled as
        // JavaScript reads one.
        const std::string& spelled = prop->AsString();
        const size_t dot = spelled.find('.');
        const std::string name = spelled.substr(0, dot);
        const MotionPropDefinition* definition = FindProp(name);
        if (definition == nullptr) {
            return Fail(error_, "UnknownProp", path + ".prop",
                        "no prop named '" + name + "'");
        }
        out.prop = name;
        if (!Component(*definition, spelled, dot, path, out)) {
            return false;
        }
        const Value* map = raw.Find("map");
        const Value* notEmpty = raw.Find("notEmpty");
        if (map != nullptr && notEmpty != nullptr) {
            return Fail(error_, "InvalidValue", path,
                        "map and notEmpty cannot be combined");
        }
        if (notEmpty != nullptr) {
            if (!notEmpty->IsBool() || !notEmpty->AsBool() ||
                definition->type != MotionPropType::Text) {
                return Fail(error_, "InvalidValue", path + ".notEmpty",
                            "notEmpty must be true on a text prop");
            }
            out.kind = MotionBindingKind::PropNotEmpty;
            return true;
        }
        if (map == nullptr) {
            out.kind = MotionBindingKind::Prop;
            return true;
        }
        out.kind = MotionBindingKind::PropMap;
        return Map(*map, path, out);
    }

    // A point or a size binds by component; nothing else has one, and a
    // group has no value to bind at all.
    bool Component(const MotionPropDefinition& definition,
                   const std::string& spelled, size_t dot,
                   const std::string& path, MotionValue& out) {
        if (definition.type == MotionPropType::Group) {
            return Fail(error_, "PropTypeMismatch", path + ".prop",
                        "'" + definition.name +
                            "' is a group: it has no value, bind one of the "
                            "props it gathers");
        }
        const std::vector<std::string> components =
            definition.type == MotionPropType::Point
                ? std::vector<std::string>{"x", "y"}
            : definition.type == MotionPropType::Size
                ? std::vector<std::string>{"width", "height"}
                : std::vector<std::string>{};
        if (dot == std::string::npos) {
            if (!components.empty()) {
                return Fail(error_, "PropTypeMismatch", path + ".prop",
                            "'" + definition.name + "' is a " +
                                (components.front() == "x" ? "point" : "size") +
                                ": bind one component, '" + definition.name +
                                "." + components.front() + "' or '" +
                                definition.name + "." + components.back() +
                                "'");
            }
            return true;
        }
        const std::string component = spelled.substr(dot + 1);
        if (!OneOf(component, components)) {
            return Fail(error_, "PropTypeMismatch", path + ".prop",
                        components.empty()
                            ? "'" + definition.name +
                                  "' has no components: only a point (x, y) "
                                  "or a size (width, height) does"
                            : "'" + definition.name + "' has no component '" +
                                  component + "': " + Join(components));
        }
        out.component = component;
        return true;
    }

    bool Map(const Value& map, const std::string& path, MotionValue& out) {
        if (map.IsString()) {
            const auto table = std::find_if(
                composition_.tokens.begin(), composition_.tokens.end(),
                [&](const MotionTokenTable& candidate) {
                    return candidate.name == map.AsString();
                });
            if (table == composition_.tokens.end()) {
                return Fail(error_, "UnknownTokenTable", path + ".map",
                            "no token table named '" + map.AsString() + "'");
            }
            out.table = map.AsString();
            return true;
        }
        if (!map.IsObject()) {
            return Fail(error_, "InvalidValue", path + ".map",
                        "map must name a token table or be an object");
        }
        out.inline_map = map.AsObject();
        return true;
    }

    // M1b -- the element or the index of the innermost enclosing repeat.
    bool RepeatBinding(const Value& raw, const Value* item, const Value* index,
                       const std::string& path, MotionValue& out) {
        if (repeat_depth_ == 0) {
            return Fail(error_, "InvalidValue", path,
                        "item and index bindings only exist inside a repeat "
                        "template");
        }
        const Value* flag = item != nullptr ? item : index;
        if ((item != nullptr && index != nullptr) ||
            raw.Find("prop") != nullptr || raw.Find("notEmpty") != nullptr ||
            !flag->IsBool() || !flag->AsBool()) {
            return Fail(error_, "InvalidValue", path,
                        "write {\"item\": true} (optionally with map) or "
                        "{\"index\": true}");
        }
        const Value* map = raw.Find("map");
        if (index != nullptr) {
            if (map != nullptr) {
                return Fail(error_, "InvalidValue", path + ".map",
                            "map applies to item, not to index");
            }
            out.kind = MotionBindingKind::Index;
            return true;
        }
        if (map == nullptr) {
            out.kind = MotionBindingKind::Item;
            return true;
        }
        out.kind = MotionBindingKind::ItemMap;
        return Map(*map, path, out);
    }

    bool Animation(const std::string& property, const Value& raw,
                   const std::string& path, MotionAnimation& out) {
        if (property != "d" && !IsAnimatableMotionProperty(property)) {
            return Fail(error_, "InvalidAnimation", path,
                        "'" + property +
                            "' is not an animatable property (numbers, "
                            "colours and clip-path polygons)");
        }
        if (!raw.IsObject() || raw.AsObject().size() != 1) {
            return Fail(error_, "InvalidAnimation", path,
                        "an animation is exactly one of interpolate or spring");
        }
        out.value_kind = MotionPropertyValueKind(property);
        const Value* interpolate = raw.Find("interpolate");
        const Value* spring = raw.Find("spring");
        if (interpolate != nullptr) {
            return Interpolate(*interpolate, path + ".interpolate", out);
        }
        if (spring != nullptr) {
            if (out.value_kind != MotionValueKind::Number) {
                return Fail(error_, "InvalidAnimation", path + ".spring",
                            "a spring animates a number; interpolate '" +
                                property + "' instead");
            }
            return Spring(*spring, path + ".spring", out);
        }
        return Fail(error_, "InvalidAnimation", path,
                    "an animation is exactly one of interpolate or spring");
    }

    // MB11 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. What a prop
    // binding of a time or of an animated value gives with the prop's
    // default, and every value it can give: the entries of its map, the
    // default alone without one. The evaluator reads the same keys
    // (Evaluator::Lookup).
    bool BoundDefault(const Value& raw, const std::string& path,
                      MotionValue& binding, Value& resolved,
                      std::vector<Value>& possible) {
        if (!Binding(raw, path, binding)) {
            return false;
        }
        if (binding.kind != MotionBindingKind::Prop &&
            binding.kind != MotionBindingKind::PropMap) {
            return Fail(error_, "InvalidValue", path,
                        "a time or an animated value binds a prop: "
                        "{\"prop\": name}, or through a map");
        }
        const MotionPropDefinition* definition = FindProp(binding.prop);
        const Value* key = &definition->default_value;
        if (!binding.component.empty()) {
            key = definition->default_value.Find(binding.component);
        }
        if (key == nullptr) {
            return Fail(error_, "InvalidValue", path,
                        "the prop's default has no value to bind");
        }
        possible.clear();
        if (binding.kind == MotionBindingKind::Prop) {
            resolved = *key;
            possible.push_back(resolved);
            return true;
        }
        const std::vector<std::pair<std::string, Value>>* entries =
            &binding.inline_map;
        if (!binding.table.empty()) {
            for (const MotionTokenTable& table : composition_.tokens) {
                if (table.name == binding.table) {
                    entries = &table.entries;
                }
            }
        }
        const std::string name = key->IsString() ? key->AsString()
                                 : key->IsBool()
                                     ? (key->AsBool() ? "true" : "false")
                                     : key->Dump();
        bool found = false;
        for (const auto& entry : *entries) {
            possible.push_back(entry.second);
            if (entry.first == name) {
                resolved = entry.second;
                found = true;
            }
        }
        if (!found) {
            return Fail(
                error_, "UnknownToken", path + ".map",
                "the map has no entry for the prop's default '" + name + "'");
        }
        return true;
    }

    bool Interpolate(const Value& raw, const std::string& path,
                     MotionAnimation& out) {
        if (!CheckKeys(raw,
                       {"frames", "values", "uids", "easing", "extrapolateLeft",
                        "extrapolateRight"},
                       path, error_)) {
            return false;
        }
        out.kind = MotionAnimationKind::Interpolate;
        const Value* frames = raw.Find("frames");
        const Value* values = raw.Find("values");
        // REMOTION-PARITY-2026-09 REM5 -- one key is enough: it holds its
        // value, as the first key set on a property does in a motion tool,
        // until a second one gives it somewhere to go.
        if (frames == nullptr || values == nullptr || !frames->IsArray() ||
            !values->IsArray() || frames->AsArray().empty() ||
            frames->AsArray().size() != values->AsArray().size()) {
            return Fail(error_, "InvalidAnimation", path,
                        "frames and values must be arrays of the same length, "
                        "at least one");
        }
        // M3 -- the keyframes' persistent identities, in keyframe order.
        std::vector<std::string> keyframeIds;
        if (const Value* uids = raw.Find("uids")) {
            if (!uids->IsArray() ||
                uids->AsArray().size() != frames->AsArray().size()) {
                return Fail(error_, "InvalidAnimation", path + ".uids",
                            "uids names every keyframe: an array as long as "
                            "frames");
            }
            for (size_t index = 0; index < uids->AsArray().size(); ++index) {
                std::string uid;
                if (!Identity(uids->AsArray()[index],
                              path + ".uids[" + std::to_string(index) + "]",
                              uid)) {
                    return false;
                }
                keyframeIds.push_back(std::move(uid));
            }
        }
        // MOTION-BIBLIOTHEQUE-2026-10 MB5 -- `easing` names one curve for
        // every segment, or lists one per segment: an entrance and an exit
        // seldom share theirs, and After and Lottie give a curve per key.
        const size_t keyCount = frames->AsArray().size();
        std::vector<KeyframeInterpolation> interpolations(
            keyCount, KeyframeInterpolation::Linear);
        std::vector<KeyframeEasing> easings(keyCount);
        // MW3 -- a GSAP curve name is read here, for an interpolation only.
        std::vector<MotionNamedCurve> named(keyCount);
        bool anyNamed = false;
        const auto curve =
            [&](const std::string& text, KeyframeInterpolation& interpolation,
                KeyframeEasing& easing, MotionNamedCurve& namedCurve) {
                if (ParseEasing(text, interpolation, easing)) {
                    return true;
                }
                if (!ParseMotionNamedCurve(text, namedCurve, easing)) {
                    return false;
                }
                interpolation = KeyframeInterpolation::CubicBezier;
                anyNamed = true;
                return true;
            };
        const char* const curves =
            "linear, ease, ease-in, ease-out, ease-in-out, step-end, "
            "cubic-bezier(x1, y1, x2, y2) with x in [0, 1], or a GSAP curve: "
            "power1 to power4, sine, expo, circ or back(overshoot), with .in, "
            ".out or .inOut";
        const Value* easingName = raw.Find("easing");
        if (easingName != nullptr && easingName->IsArray()) {
            const std::vector<Value>& listed = easingName->AsArray();
            if (listed.size() + 1 != keyCount) {
                return Fail(error_, "InvalidAnimation", path + ".easing",
                            "an easing list holds one curve per segment: " +
                                std::to_string(keyCount - 1) + " for " +
                                std::to_string(keyCount) + " frames, not " +
                                std::to_string(listed.size()) +
                                " (or one name for every segment)");
            }
            for (size_t index = 0; index < listed.size(); ++index) {
                if (!listed[index].IsString() ||
                    !curve(listed[index].AsString(), interpolations[index],
                           easings[index], named[index])) {
                    return Fail(
                        error_, "InvalidAnimation",
                        path + ".easing[" + std::to_string(index) + "]",
                        std::string("each curve of an easing list is ") +
                            curves);
                }
            }
            // The last key leaves no segment: it carries the curve arriving
            // at it, so a list repeating one curve is the track its name
            // alone gives.
            if (keyCount > 1) {
                interpolations[keyCount - 1] = interpolations[keyCount - 2];
                easings[keyCount - 1] = easings[keyCount - 2];
                named[keyCount - 1] = named[keyCount - 2];
            }
        } else if (easingName != nullptr) {
            KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
            KeyframeEasing easing;
            MotionNamedCurve namedCurve;
            if (!easingName->IsString() ||
                !curve(easingName->AsString(), interpolation, easing,
                       namedCurve)) {
                return Fail(error_, "InvalidAnimation", path + ".easing",
                            std::string("easing must be ") + curves +
                                ", or a list of one per segment");
            }
            std::fill(interpolations.begin(), interpolations.end(),
                      interpolation);
            std::fill(easings.begin(), easings.end(), easing);
            std::fill(named.begin(), named.end(), namedCurve);
        }
        if (anyNamed) {
            out.named_curves = std::move(named);
        }
        for (const char* side : {"extrapolateLeft", "extrapolateRight"}) {
            const Value* mode = raw.Find(side);
            if (mode == nullptr) {
                continue;
            }
            if (!mode->IsString() ||
                !OneOf(mode->AsString(), {"clamp", "extend"})) {
                return Fail(error_, "InvalidAnimation", path + "." + side,
                            std::string(side) + " must be clamp or extend");
            }
            (std::string(side) == "extrapolateLeft" ? out.extrapolate_left
                                                    : out.extrapolate_right) =
                mode->AsString() == "extend" ? KeyframeExtrapolation::Extend
                                             : KeyframeExtrapolation::Clamp;
        }
        int64_t previousNumerator = 0;
        int64_t previousDenominator = 1;
        // MA7 -- each key is checked against the one before it: the
        // conditions are transitive, so every key matches the first.
        size_t vertices = 0;
        MotionClipKind previous_shape = MotionClipKind::None;
        MotionGradient previous_gradient;
        std::vector<MotionFilter> previous_filters;
        MotionPath previous_path;
        for (size_t index = 0; index < frames->AsArray().size(); ++index) {
            // MOTION-WEB-2026-10 MW1 -- a key may sit between two frames,
            // written as an exact fraction ("21/2"), never as a decimal.
            int64_t frameNumerator = 0;
            int64_t frameDenominator = 1;
            double value = 0.0;
            if (!ParseMotionKeyFrame(frames->AsArray()[index], frameNumerator,
                                     frameDenominator) ||
                (index > 0 &&
                 static_cast<__int128>(frameNumerator) * previousDenominator <=
                     static_cast<__int128>(previousNumerator) *
                         frameDenominator)) {
                return Fail(error_, "InvalidAnimation", path + ".frames",
                            "frames must be strictly increasing integers, "
                            "or exact fractions written \"n/d\" (\"21/2\")");
            }
            RationalTime keyTime;
            if (!MotionFrameTime(composition_, frameNumerator, frameDenominator,
                                 keyTime)) {
                return Fail(error_, "InvalidAnimation", path + ".frames",
                            "frame " + frames->AsArray()[index].Dump() +
                                " cannot be an exact time at this frame "
                                "rate");
            }
            const std::string valuePath =
                path + ".values[" + std::to_string(index) + "]";
            // MB11 -- a value bound to a prop: checked here with the prop's
            // default and with every value its map can give, kept for the
            // evaluator; what follows reads the default's.
            const Value* written = &values->AsArray()[index];
            Value boundDefault;
            std::vector<Value> possible;
            if (written->IsObject()) {
                if (out.value_kind != MotionValueKind::Number &&
                    out.value_kind != MotionValueKind::Color &&
                    out.value_kind != MotionValueKind::Polygon) {
                    return Fail(error_, "InvalidAnimation", valuePath,
                                "a number, a colour or a clip-path shape "
                                "binds a prop; a gradient, a filter or a "
                                "path is written");
                }
                MotionValue binding;
                if (!BoundDefault(*written, valuePath, binding, boundDefault,
                                  possible)) {
                    return false;
                }
                out.bound_values.emplace_back(index, std::move(binding));
                written = &boundDefault;
            }
            const Value& raw = *written;
            for (const Value& other : possible) {
                double number = 0.0;
                MotionColor colour;
                MotionClipShape shape;
                MotionClipShape own;
                const bool fits =
                    out.value_kind == MotionValueKind::Number
                        ? MotionNumber(other, number)
                    : out.value_kind == MotionValueKind::Color
                        ? other.IsString() &&
                              ParseMotionColor(other.AsString(), colour)
                        : other.IsString() && raw.IsString() &&
                              ParseMotionClipShape(other.AsString(), shape) &&
                              ParseMotionClipShape(raw.AsString(), own) &&
                              shape.kind == own.kind &&
                              shape.polygon.size() == own.polygon.size();
                if (!fits) {
                    return Fail(
                        error_, "InvalidAnimation", valuePath,
                        "every value the bound prop can give is a " +
                            std::string(
                                out.value_kind == MotionValueKind::Number
                                    ? "number"
                                : out.value_kind == MotionValueKind::Color
                                    ? "colour"
                                    : "shape of the same kind and "
                                      "vertex count") +
                            ": '" + other.Dump() + "' is not");
                }
            }
            MotionColor color;
            MotionPolygon polygon;
            switch (out.value_kind) {
                case MotionValueKind::Number:
                    if (!MotionNumber(raw, value)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "this property interpolates numbers");
                    }
                    break;
                case MotionValueKind::Color:
                    if (!raw.IsString() ||
                        !ParseMotionColor(raw.AsString(), color)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "this property interpolates colours "
                                    "(#rrggbb, rgba(), ...)");
                    }
                    out.values.push_back(raw);
                    value = static_cast<double>(index);
                    break;
                case MotionValueKind::Polygon: {
                    MotionClipShape shape;
                    if (!raw.IsString() ||
                        !ParseMotionClipShape(raw.AsString(), shape)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "clipPath interpolates polygon(), "
                                    "circle(), ellipse() or inset() values");
                    }
                    polygon = shape.polygon;
                    // CSS interpolates two polygons vertex by vertex only
                    // when they have as many vertices, and two shapes only
                    // when they are the same shape (REM3); anything else
                    // would be an invented morph.
                    if (index > 0 && shape.kind != previous_shape) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "every shape of an interpolation is the "
                                    "same kind (polygon, circle, ellipse or "
                                    "inset): change shape between two "
                                    "sequences instead");
                    }
                    if (index > 0 && shape.kind == MotionClipKind::Polygon &&
                        polygon.size() != vertices) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "every polygon of an interpolation needs "
                                    "the same number of vertices (" +
                                        std::to_string(vertices) + " then " +
                                        std::to_string(polygon.size()) + ")");
                    }
                    previous_shape = shape.kind;
                    vertices = polygon.size();
                    out.values.push_back(raw);
                    value = static_cast<double>(index);
                    break;
                }
                case MotionValueKind::Gradient: {
                    MotionGradient gradient;
                    if (!raw.IsString() ||
                        !ParseMotionGradient(raw.AsString(), gradient)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "this property interpolates gradients");
                    }
                    if (index > 0 &&
                        !SameGradientShape(previous_gradient, gradient)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "every gradient of an interpolation has "
                                    "the same kind, form and number of stops: "
                                    "stops are mixed one by one");
                    }
                    previous_gradient = gradient;
                    out.values.push_back(raw);
                    value = static_cast<double>(index);
                    break;
                }
                case MotionValueKind::Filter: {
                    std::vector<MotionFilter> filters;
                    if (!raw.IsString() ||
                        !ParseMotionFilters(raw.AsString(), filters)) {
                        return Fail(
                            error_, "InvalidAnimation", valuePath,
                            "filter interpolates lists of blur(), "
                            "drop-shadow(), glow(), displace(), grain(), "
                            "goo(), slice() and mesh()");
                    }
                    if (index > 0 &&
                        !SameFilterList(previous_filters, filters)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "every filter list of an interpolation "
                                    "holds the same effects in the same order: "
                                    "their parameters are mixed one by one");
                    }
                    previous_filters = filters;
                    out.values.push_back(raw);
                    value = static_cast<double>(index);
                    break;
                }
                case MotionValueKind::Path: {
                    // MOTION-DESIGN-2026-09 -- morphs of one structure.
                    MotionPath parsed;
                    std::string message;
                    if (!raw.IsString() ||
                        !ParseMotionSvgPath(raw.AsString(), parsed, message)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "d interpolates SVG path data: " + message);
                    }
                    if (index > 0 &&
                        !MotionPathsMorphable(previous_path, parsed)) {
                        return Fail(error_, "InvalidAnimation", valuePath,
                                    "every path of a morph has the same "
                                    "structure: the same moves and closes, "
                                    "as many segments between them");
                    }
                    previous_path = std::move(parsed);
                    out.values.push_back(raw);
                    value = static_cast<double>(index);
                    break;
                }
            }
            previousNumerator = frameNumerator;
            previousDenominator = frameDenominator;
            const KeyframeResult added = out.track.Add(
                {keyframeIds.empty() ? SyntheticKeyframeId(index)
                                     : keyframeIds[index],
                 keyTime, value, interpolations[index], easings[index]});
            if (!added) {
                return Fail(error_, "InvalidAnimation", path,
                            std::string("keyframe refused: ") +
                                KeyframeErrorName(added.error));
            }
        }
        return true;
    }

    bool Spring(const Value& raw, const std::string& path,
                MotionAnimation& out) {
        if (!CheckKeys(raw,
                       {"from", "to", "delay", "mass", "damping", "stiffness",
                        "overshootClamping"},
                       path, error_)) {
            return false;
        }
        out.kind = MotionAnimationKind::Spring;
        const std::vector<std::pair<const char*, double*>> numbers = {
            {"from", &out.spring.from},
            {"to", &out.spring.to},
            {"mass", &out.spring.mass},
            {"damping", &out.spring.damping},
            {"stiffness", &out.spring.stiffness}};
        for (const auto& field : numbers) {
            const Value* value = raw.Find(field.first);
            if (value != nullptr && !MotionNumber(*value, *field.second)) {
                return Fail(error_, "InvalidAnimation",
                            path + "." + field.first,
                            std::string(field.first) + " must be a number");
            }
        }
        const Value* delay = raw.Find("delay");
        if (delay != nullptr && (!delay->AsInt64(out.delay) || out.delay < 0)) {
            return Fail(error_, "InvalidAnimation", path + ".delay",
                        "delay must be a non-negative number of frames");
        }
        const Value* clamping = raw.Find("overshootClamping");
        if (clamping != nullptr) {
            if (!clamping->IsBool()) {
                return Fail(error_, "InvalidAnimation",
                            path + ".overshootClamping",
                            "overshootClamping must be a boolean");
            }
            out.spring.overshoot_clamping = clamping->AsBool();
        }
        std::string reason;
        if (!ValidateMotionSpring(out.spring, reason)) {
            return Fail(error_, "InvalidAnimation", path, reason);
        }
        return true;
    }

    // `series_child`: a sequence inside a series starts where the previous
    // one ends, moved by its offset; it has no from of its own.
    bool Node(const Value& raw, const std::string& path, MotionNode& node,
              bool series_child = false) {
        const Value* type = raw.IsObject() ? raw.Find("type") : nullptr;
        const std::string typeName =
            type != nullptr && type->IsString() ? type->AsString() : "";
        std::vector<std::string> allowed = {
            "type", "uid", "id", "style", "animate", "visible", "decor"};
        if (typeName == "fill") {
            node.type = MotionNodeType::Fill;
            allowed.push_back("children");
        } else if (typeName == "box") {
            node.type = MotionNodeType::Box;
            allowed.push_back("children");
        } else if (typeName == "text") {
            node.type = MotionNodeType::Text;
            allowed.insert(allowed.end(), {"content", "units", "ranges",
                                           "stagger", "highlights"});
        } else if (typeName == "image") {
            node.type = MotionNodeType::Image;
            allowed.push_back("src");
        } else if (typeName == "sequence") {
            node.type = MotionNodeType::Sequence;
            allowed.insert(allowed.end(), {"children", "from", "duration",
                                           "offset", "timeScale"});
        } else if (typeName == "path") {
            node.type = MotionNodeType::Path;
            allowed.insert(allowed.end(), {"d", "points", "curve", "closed",
                                           "viewBox", "marker"});
        } else if (typeName == "repeat") {
            // A repeat has no box of its own: its copies take its place
            // among its siblings, so it carries neither style nor id.
            node.type = MotionNodeType::Repeat;
            allowed = {"type", "uid",      "visible", "count",
                       "each", "template", "cycle",   "stagger"};
        } else if (typeName == "series") {
            node.type = MotionNodeType::Series;
            allowed.push_back("children");
        } else if (typeName == "video") {
            // REM2 -- pictures and, unless muted, sound of a media.
            node.type = MotionNodeType::Video;
            allowed.insert(allowed.end(), {"src", "media", "startFrom", "endAt",
                                           "playbackRate", "muted"});
        } else if (typeName == "audio") {
            // REM2 -- a sound alone: no box, no picture, nothing to lay out.
            node.type = MotionNodeType::Audio;
            allowed.insert(allowed.end(), {"src", "media", "startFrom", "endAt",
                                           "playbackRate"});
        } else if (typeName == "composition") {
            // REM2 -- another composition of the project, by ULID.
            node.type = MotionNodeType::Composition;
            allowed.insert(allowed.end(), {"composition", "props"});
        } else if (typeName == "transition") {
            // REM3 -- between two sequences of a series, and nowhere else;
            // it has no box: presentation and timing only.
            if (!series_child) {
                return Fail(error_, "InvalidValue", path + ".type",
                            "a transition joins two sequences of a series");
            }
            node.type = MotionNodeType::Transition;
            allowed = {"type",         "uid",       "id",
                       "presentation", "direction", "timing"};
        } else {
            return Fail(error_, "InvalidValue", path + ".type",
                        "type must be fill, box, text, image, video, audio, "
                        "composition, sequence, path, repeat or series");
        }
        if (!CheckKeys(raw, allowed, path, error_)) {
            return false;
        }
        if (node.type == MotionNodeType::Repeat && path == "root") {
            return Fail(error_, "InvalidComposition", path + ".type",
                        "the root cannot be a repeat: put it in a fill or a "
                        "box");
        }

        if (const Value* uid = raw.Find("uid")) {
            if (!Identity(*uid, path + ".uid", node.uid)) {
                return false;
            }
        }
        if (const Value* id = raw.Find("id")) {
            if (!id->IsString() || id->AsString().empty()) {
                return Fail(error_, "InvalidValue", path + ".id",
                            "id must be a non-empty string");
            }
            if (!ids_.insert(id->AsString()).second) {
                return Fail(error_, "DuplicateNodeId", path + ".id",
                            "node id '" + id->AsString() + "' is used twice");
            }
            node.id = id->AsString();
        }
        if (const Value* style = raw.Find("style")) {
            if (!style->IsObject()) {
                return Fail(error_, "InvalidValue", path + ".style",
                            "style must be an object");
            }
            for (const auto& entry : style->AsObject()) {
                const std::string entryPath = path + ".style." + entry.first;
                if (FindStyleRule(entry.first) == nullptr) {
                    return Fail(error_, "UnknownStyleProperty", entryPath,
                                UnknownStyleMessage(entry.first));
                }
                MotionValue value;
                if (!Binding(entry.second, entryPath, value)) {
                    return false;
                }
                if (value.kind == MotionBindingKind::Literal &&
                    !ValidateMotionStyleValue(entry.first, value.literal,
                                              entryPath, error_)) {
                    return false;
                }
                node.style.emplace_back(entry.first, std::move(value));
            }
        }
        if (const Value* animate = raw.Find("animate")) {
            if (!animate->IsObject()) {
                return Fail(error_, "InvalidValue", path + ".animate",
                            "animate must be an object");
            }
            for (const auto& entry : animate->AsObject()) {
                const std::string entryPath = path + ".animate." + entry.first;
                // MOTION-DESIGN-2026-09 -- a path's d morphs.
                const bool morph =
                    entry.first == "d" && node.type == MotionNodeType::Path;
                if (!morph && FindStyleRule(entry.first) == nullptr) {
                    return Fail(error_, "UnknownStyleProperty", entryPath,
                                UnknownStyleMessage(entry.first));
                }
                MotionAnimation animation;
                if (!Animation(entry.first, entry.second, entryPath,
                               animation)) {
                    return false;
                }
                node.animate.emplace_back(entry.first, std::move(animation));
            }
        }
        if (const Value* visible = raw.Find("visible")) {
            MotionValue value;
            if (!Binding(*visible, path + ".visible", value)) {
                return false;
            }
            if (value.kind == MotionBindingKind::Literal &&
                !value.literal.IsBool()) {
                return Fail(error_, "InvalidValue", path + ".visible",
                            "visible must be a boolean or a binding");
            }
            node.visible = std::move(value);
        }
        // M5b -- decor is a literal: what the check leaves out is read in
        // the file, not resolved from a prop.
        if (const Value* decor = raw.Find("decor")) {
            if (!decor->IsBool()) {
                return Fail(error_, "InvalidValue", path + ".decor",
                            "decor must be a boolean: true for a node the "
                            "safe-area check does not judge");
            }
            node.decor = decor->AsBool();
        }
        if (node.type == MotionNodeType::Text) {
            const Value* content = raw.Find("content");
            MotionValue value;
            if (content == nullptr ||
                !Binding(*content, path + ".content", value)) {
                return content == nullptr
                           ? Fail(error_, "InvalidValue", path + ".content",
                                  "a text node needs content")
                           : false;
            }
            if (value.kind == MotionBindingKind::Literal &&
                !value.literal.IsString()) {
                return Fail(error_, "InvalidValue", path + ".content",
                            "content must be a string or a binding");
            }
            node.content = std::move(value);
        }
        if (node.type == MotionNodeType::Image) {
            const Value* src = raw.Find("src");
            MotionValue value;
            if (src == nullptr || !Binding(*src, path + ".src", value)) {
                return src == nullptr
                           ? Fail(error_, "InvalidValue", path + ".src",
                                  "an image node needs src")
                           : false;
            }
            node.src = std::move(value);
        }
        if (node.type == MotionNodeType::Sequence &&
            !SequenceFields(raw, path, node, series_child)) {
            return false;
        }
        if (node.type == MotionNodeType::Text && !TextFields(raw, path, node)) {
            return false;
        }
        if (node.type == MotionNodeType::Path && !PathFields(raw, path, node)) {
            return false;
        }
        if (node.type == MotionNodeType::Repeat &&
            !RepeatFields(raw, path, node)) {
            return false;
        }
        if ((node.type == MotionNodeType::Video ||
             node.type == MotionNodeType::Audio) &&
            !MediaFields(raw, path, node)) {
            return false;
        }
        if (node.type == MotionNodeType::Composition &&
            !InstanceFields(raw, path, node)) {
            return false;
        }
        if (node.type == MotionNodeType::Transition &&
            !TransitionFields(raw, path, node)) {
            return false;
        }
        if (!VolumePlacement(path, node)) {
            return false;
        }
        if (const Value* children = raw.Find("children")) {
            if (!children->IsArray()) {
                return Fail(error_, "InvalidValue", path + ".children",
                            "children must be an array");
            }
            for (size_t index = 0; index < children->AsArray().size();
                 ++index) {
                const Value& rawChild = children->AsArray()[index];
                const std::string childPath =
                    path + ".children[" + std::to_string(index) + "]";
                const Value* childType =
                    rawChild.IsObject() ? rawChild.Find("type") : nullptr;
                if (node.type == MotionNodeType::Series &&
                    (childType == nullptr || !childType->IsString() ||
                     (childType->AsString() != "sequence" &&
                      childType->AsString() != "transition"))) {
                    return Fail(error_, "InvalidValue", childPath + ".type",
                                "a series holds sequence nodes, and "
                                "transitions between them");
                }
                MotionNode child;
                if (!Node(rawChild, childPath, child,
                          node.type == MotionNodeType::Series)) {
                    return false;
                }
                node.children.push_back(std::move(child));
            }
        }
        if (node.type == MotionNodeType::Series) {
            return SeriesStarts(path, node);
        }
        return true;
    }

    bool SequenceFields(const Value& raw, const std::string& path,
                        MotionNode& node, bool series_child) {
        const Value* from = raw.Find("from");
        const Value* offset = raw.Find("offset");
        if (series_child && from != nullptr) {
            return Fail(error_, "InvalidValue", path + ".from",
                        "inside a series a sequence starts where the previous "
                        "one ends; move it with offset");
        }
        if (!series_child && offset != nullptr) {
            return Fail(error_, "InvalidValue", path + ".offset",
                        "offset applies to a sequence inside a series; use "
                        "from elsewhere");
        }
        if (from != nullptr && (!from->AsInt64(node.from) || node.from < 0)) {
            return Fail(error_, "InvalidValue", path + ".from",
                        "from must be a non-negative number of frames");
        }
        if (offset != nullptr && !offset->AsInt64(node.offset)) {
            return Fail(error_, "InvalidValue", path + ".offset",
                        "offset must be a whole number of frames");
        }
        if (const Value* duration = raw.Find("duration")) {
            int64_t frames = 0;
            if (!duration->AsInt64(frames) || frames <= 0) {
                return Fail(error_, "InvalidValue", path + ".duration",
                            "duration must be a positive number of frames");
            }
            node.duration = frames;
        }
        if (const Value* scale = raw.Find("timeScale")) {
            MotionValue value;
            if (!Binding(*scale, path + ".timeScale", value)) {
                return false;
            }
            int64_t numerator = 0;
            int64_t denominator = 1;
            if (value.kind == MotionBindingKind::Literal &&
                !ParseMotionTimeScale(value.literal, numerator, denominator)) {
                return Fail(error_, "InvalidValue", path + ".timeScale",
                            kTimeScaleMessage);
            }
            node.time_scale = std::move(value);
        }
        return true;
    }

    // Remotion's Series: each sequence starts where the previous one ends;
    // an offset moves it and every sequence after it. REM3 -- a transition
    // between two sequences makes them overlap by its duration: the next
    // starts that much earlier, so the series is that much shorter.
    bool SeriesStarts(const std::string& path, MotionNode& node) {
        int64_t start = 0;
        for (size_t index = 0; index < node.children.size(); ++index) {
            MotionNode& child = node.children[index];
            const std::string childPath =
                path + ".children[" + std::to_string(index) + "]";
            if (child.type == MotionNodeType::Transition) {
                const bool between =
                    index > 0 && index + 1 < node.children.size() &&
                    node.children[index - 1].type == MotionNodeType::Sequence &&
                    node.children[index + 1].type == MotionNodeType::Sequence;
                if (!between) {
                    return Fail(error_, "InvalidValue", childPath,
                                "a transition stands between two sequences "
                                "of a series");
                }
                const MotionNode& before = node.children[index - 1];
                const MotionNode& after = node.children[index + 1];
                // TransitionFields gives every transition its duration.
                const int64_t overlap = child.duration.value_or(0);
                if (!before.duration.has_value() ||
                    *before.duration < overlap ||
                    (after.duration.has_value() && *after.duration < overlap)) {
                    return Fail(error_, "InvalidValue",
                                childPath + ".timing.duration",
                                "a transition lasts at most as long as each "
                                "sequence it joins");
                }
                // Where the overlap starts, in the series' frames.
                child.from = start - overlap;
                start -= overlap;
                continue;
            }
            start += child.offset;
            if (start < 0) {
                return Fail(error_, "InvalidValue", childPath + ".offset",
                            "this offset starts the sequence before the "
                            "series itself");
            }
            child.from = start;
            if (!child.duration.has_value()) {
                if (index + 1 < node.children.size()) {
                    return Fail(error_, "InvalidValue", childPath + ".duration",
                                "every sequence of a series but the last needs "
                                "a duration");
                }
                continue;
            }
            start += *child.duration;
        }
        return true;
    }

    bool Stagger(const Value& raw, const std::string& path,
                 MotionStagger& out) {
        if (!CheckKeys(raw, {"each", "order", "seed"}, path, error_)) {
            return false;
        }
        out.present = true;
        const Value* each = raw.Find("each");
        if (each == nullptr || !each->AsInt64(out.each) || out.each < 0 ||
            out.each > 100000) {
            return Fail(error_, "InvalidValue", path + ".each",
                        "each must be a non-negative number of frames");
        }
        const Value* order = raw.Find("order");
        const std::string orderName = order == nullptr    ? "forward"
                                      : order->IsString() ? order->AsString()
                                                          : "";
        if (orderName == "forward") {
            out.order = MotionStaggerOrder::Forward;
        } else if (orderName == "reverse") {
            out.order = MotionStaggerOrder::Reverse;
        } else if (orderName == "random") {
            out.order = MotionStaggerOrder::Random;
        } else {
            return Fail(error_, "InvalidValue", path + ".order",
                        "order must be forward, reverse or random");
        }
        const Value* seed = raw.Find("seed");
        if (out.order != MotionStaggerOrder::Random) {
            if (seed != nullptr) {
                return Fail(error_, "InvalidValue", path + ".seed",
                            "seed applies to the random order");
            }
            return true;
        }
        int64_t number = 0;
        if (seed == nullptr || !seed->AsInt64(number) || number < 0) {
            return Fail(error_, "InvalidValue", path + ".seed",
                        "a random order needs a seed, a non-negative integer: "
                        "there is no unseeded randomness");
        }
        out.seed = static_cast<uint64_t>(number);
        return true;
    }

    static int UnitRank(MotionTextUnit unit) { return static_cast<int>(unit); }

    static bool ParseUnit(const Value* raw, MotionTextUnit& unit) {
        const std::string name =
            raw != nullptr && raw->IsString() ? raw->AsString() : "";
        if (name == "line") {
            unit = MotionTextUnit::Line;
        } else if (name == "word") {
            unit = MotionTextUnit::Word;
        } else if (name == "char") {
            unit = MotionTextUnit::Char;
        } else {
            return false;
        }
        return true;
    }

    // A non-negative integer literal, or a binding checked when evaluated.
    bool IndexValue(const Value& raw, const std::string& path,
                    MotionValue& out) {
        if (!Binding(raw, path, out)) {
            return false;
        }
        int64_t number = 0;
        if (out.kind == MotionBindingKind::Literal &&
            (!out.literal.AsInt64(number) || number < 0)) {
            return Fail(error_, "InvalidValue", path,
                        "must be a non-negative integer or a binding");
        }
        return true;
    }

    // MB3 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. `highlights`: a
    // list of strokes, each over a range of units (MotionTextHighlight).
    // Literal numbers for the band's geometry, which no prop needs to move;
    // the colour and the bounds bind like a range's, and the progress is a
    // number, a binding or an animation of one.
    bool Highlights(const Value& raw, const std::string& path,
                    MotionNode& node) {
        if (!raw.IsArray()) {
            return Fail(error_, "InvalidValue", path,
                        "highlights must be an array");
        }
        for (size_t index = 0; index < raw.AsArray().size(); ++index) {
            const Value& entry = raw.AsArray()[index];
            const std::string itemPath =
                path + "[" + std::to_string(index) + "]";
            if (!CheckKeys(entry,
                           {"unit", "start", "count", "color", "progress",
                            "top", "bottom", "bleed"},
                           itemPath, error_)) {
                return false;
            }
            MotionTextHighlight highlight;
            if (!ParseUnit(entry.Find("unit"), highlight.unit)) {
                return Fail(error_, "InvalidValue", itemPath + ".unit",
                            "unit must be line, word or char");
            }
            const Value* start = entry.Find("start");
            if (start == nullptr) {
                return Fail(error_, "InvalidValue", itemPath + ".start",
                            "a highlight needs a start (zero-based)");
            }
            if (!IndexValue(*start, itemPath + ".start", highlight.start)) {
                return false;
            }
            if (const Value* count = entry.Find("count")) {
                MotionValue value;
                if (!IndexValue(*count, itemPath + ".count", value)) {
                    return false;
                }
                highlight.count = std::move(value);
            }
            const Value* color = entry.Find("color");
            if (color == nullptr) {
                return Fail(error_, "InvalidValue", itemPath + ".color",
                            "a highlight needs a color");
            }
            if (!Binding(*color, itemPath + ".color", highlight.color)) {
                return false;
            }
            if (highlight.color.kind == MotionBindingKind::Literal &&
                !ValidateMotionStyleValue("color", highlight.color.literal,
                                          itemPath + ".color", error_)) {
                return false;
            }
            if (const Value* progress = entry.Find("progress")) {
                const std::string progressPath = itemPath + ".progress";
                if (progress->IsObject() &&
                    (progress->Find("interpolate") != nullptr ||
                     progress->Find("spring") != nullptr)) {
                    // Read as the number it animates: opacity is one, with
                    // the same curves, bounds and refusals.
                    MotionAnimation animation;
                    if (!Animation("opacity", *progress, progressPath,
                                   animation)) {
                        return false;
                    }
                    highlight.progress_animation = std::move(animation);
                } else {
                    MotionValue value;
                    if (!Binding(*progress, progressPath, value)) {
                        return false;
                    }
                    double number = 0.0;
                    if (value.kind == MotionBindingKind::Literal &&
                        (!MotionNumber(value.literal, number) || number < 0.0 ||
                         number > 1.0)) {
                        return Fail(error_, "InvalidValue", progressPath,
                                    "progress is a number from 0 to 1, a "
                                    "binding, or an interpolate or spring");
                    }
                    highlight.progress = std::move(value);
                }
            }
            const auto fraction = [&](const char* key, double& out) {
                const Value* value = entry.Find(key);
                if (value == nullptr) {
                    return true;
                }
                double number = 0.0;
                if (!MotionNumber(*value, number) || number < 0.0 ||
                    number > 1.0) {
                    return Fail(error_, "InvalidValue", itemPath + "." + key,
                                std::string(key) +
                                    " is a fraction of the line box's "
                                    "height, from 0 to 1");
                }
                out = number;
                return true;
            };
            if (!fraction("top", highlight.top) ||
                !fraction("bottom", highlight.bottom)) {
                return false;
            }
            if (!(highlight.top < highlight.bottom)) {
                return Fail(error_, "InvalidValue", itemPath + ".bottom",
                            "the band's bottom must be below its top");
            }
            if (const Value* bleed = entry.Find("bleed")) {
                double number = 0.0;
                if (!MotionNumber(*bleed, number) || number < 0.0 ||
                    number > 1000.0) {
                    return Fail(error_, "InvalidValue", itemPath + ".bleed",
                                "bleed is a number of pixels from 0 to "
                                "1000, past each end of a line's band");
                }
                highlight.bleed = number;
            }
            node.highlights.push_back(std::move(highlight));
        }
        return true;
    }

    bool TextFields(const Value& raw, const std::string& path,
                    MotionNode& node) {
        if (const Value* units = raw.Find("units")) {
            if (!ParseUnit(units, node.units)) {
                return Fail(error_, "InvalidValue", path + ".units",
                            "units must be line, word or char");
            }
        }
        if (const Value* stagger = raw.Find("stagger")) {
            if (node.units == MotionTextUnit::None) {
                return Fail(error_, "InvalidValue", path + ".stagger",
                            "stagger cascades text units: set units to line, "
                            "word or char");
            }
            if (!Stagger(*stagger, path + ".stagger", node.stagger)) {
                return false;
            }
        }
        if (const Value* ranges = raw.Find("ranges")) {
            if (!ranges->IsArray()) {
                return Fail(error_, "InvalidValue", path + ".ranges",
                            "ranges must be an array");
            }
            for (size_t index = 0; index < ranges->AsArray().size(); ++index) {
                const Value& entry = ranges->AsArray()[index];
                const std::string rangePath =
                    path + ".ranges[" + std::to_string(index) + "]";
                if (!CheckKeys(entry, {"unit", "start", "count", "style"},
                               rangePath, error_)) {
                    return false;
                }
                MotionTextRange range;
                if (!ParseUnit(entry.Find("unit"), range.unit)) {
                    return Fail(error_, "InvalidValue", rangePath + ".unit",
                                "unit must be line, word or char");
                }
                if (node.units != MotionTextUnit::None &&
                    UnitRank(range.unit) > UnitRank(node.units)) {
                    return Fail(
                        error_, "InvalidValue", rangePath + ".unit",
                        "a range cannot be finer than the text's units: "
                        "cut the text into the finer unit");
                }
                const Value* start = entry.Find("start");
                if (start == nullptr) {
                    return Fail(error_, "InvalidValue", rangePath + ".start",
                                "a range needs a start (zero-based)");
                }
                if (!IndexValue(*start, rangePath + ".start", range.start)) {
                    return false;
                }
                if (const Value* count = entry.Find("count")) {
                    MotionValue value;
                    if (!IndexValue(*count, rangePath + ".count", value)) {
                        return false;
                    }
                    range.count = std::move(value);
                }
                const Value* style = entry.Find("style");
                if (style == nullptr || !style->IsObject()) {
                    return Fail(error_, "InvalidValue", rangePath + ".style",
                                "a range needs a style object");
                }
                for (const auto& property : style->AsObject()) {
                    const std::string propertyPath =
                        rangePath + ".style." + property.first;
                    if (!OneOf(property.first,
                               {"color", "backgroundColor", "opacity"})) {
                        return Fail(error_, "InvalidValue", propertyPath,
                                    "a range styles color, backgroundColor or "
                                    "opacity: units are cut after layout, so "
                                    "nothing that changes layout");
                    }
                    MotionValue value;
                    if (!Binding(property.second, propertyPath, value)) {
                        return false;
                    }
                    if (value.kind == MotionBindingKind::Literal &&
                        !ValidateMotionStyleValue(property.first, value.literal,
                                                  propertyPath, error_)) {
                        return false;
                    }
                    range.style.emplace_back(property.first, std::move(value));
                }
                node.ranges.push_back(std::move(range));
            }
        }
        if (const Value* highlights = raw.Find("highlights")) {
            if (!Highlights(*highlights, path + ".highlights", node)) {
                return false;
            }
        }
        if (node.units != MotionTextUnit::None) {
            for (const auto& entry : node.animate) {
                if (!IsMotionUnitProperty(entry.first)) {
                    return Fail(
                        error_, "InvalidAnimation",
                        path + ".animate." + entry.first,
                        "with units, animate applies to each unit: opacity, "
                        "translateX, translateY, scale, scaleX, scaleY, "
                        "rotate, color or backgroundColor. Animate an "
                        "enclosing box for anything else: what changes "
                        "layout, or a filter over the whole text");
                }
            }
        }
        return true;
    }

    bool PathFields(const Value& raw, const std::string& path,
                    MotionNode& node) {
        if (raw.Find("marker") != nullptr) {
            return Fail(error_, "UnsupportedField", path + ".marker",
                        "markers (arrow heads) are not drawn yet");
        }
        const Value* data = raw.Find("d");
        const Value* points = raw.Find("points");
        if ((data == nullptr) == (points == nullptr)) {
            return Fail(error_, "InvalidValue", path,
                        "a path needs exactly one of d (SVG path data) or "
                        "points");
        }
        if (points == nullptr &&
            (raw.Find("curve") != nullptr || raw.Find("closed") != nullptr)) {
            return Fail(error_, "InvalidValue", path,
                        "curve and closed apply to points");
        }
        if (data != nullptr) {
            MotionValue value;
            if (!Binding(*data, path + ".d", value)) {
                return false;
            }
            if (value.kind == MotionBindingKind::Literal) {
                std::string message;
                if (!value.literal.IsString() ||
                    !ParseMotionSvgPath(value.literal.AsString(), node.path,
                                        message)) {
                    return Fail(error_, "InvalidPath", path + ".d",
                                value.literal.IsString()
                                    ? message
                                    : "d must be SVG path data");
                }
                node.path_parsed = true;
            }
            node.path_data = std::move(value);
        } else {
            if (!points->IsArray() || points->AsArray().size() < 2) {
                return Fail(error_, "InvalidValue", path + ".points",
                            "points must be an array of at least two [x, y]");
            }
            std::vector<std::pair<double, double>> parsed;
            for (const Value& point : points->AsArray()) {
                double x = 0.0;
                double y = 0.0;
                if (!point.IsArray() || point.AsArray().size() != 2 ||
                    !MotionNumber(point.AsArray()[0], x) ||
                    !MotionNumber(point.AsArray()[1], y)) {
                    return Fail(error_, "InvalidValue", path + ".points",
                                "each point is [x, y] in pixels");
                }
                parsed.emplace_back(x, y);
            }
            MotionPathCurve curve = MotionPathCurve::Linear;
            if (const Value* name = raw.Find("curve")) {
                if (!name->IsString() ||
                    !OneOf(name->AsString(), {"linear", "catmull-rom"})) {
                    return Fail(error_, "InvalidValue", path + ".curve",
                                "curve must be linear or catmull-rom");
                }
                curve = name->AsString() == "linear"
                            ? MotionPathCurve::Linear
                            : MotionPathCurve::CatmullRom;
            }
            bool closed = false;
            if (const Value* flag = raw.Find("closed")) {
                if (!flag->IsBool()) {
                    return Fail(error_, "InvalidValue", path + ".closed",
                                "closed must be a boolean");
                }
                closed = flag->AsBool();
            }
            node.path = MotionPathFromPoints(parsed, curve, closed);
            node.path_parsed = true;
        }
        if (const Value* box = raw.Find("viewBox")) {
            std::string text = box->IsString() ? box->AsString() : "";
            std::replace(text.begin(), text.end(), ',', ' ');
            const std::vector<std::string> parts = SplitSpaces(text);
            double numbers[4] = {};
            bool valid = parts.size() == 4;
            for (size_t index = 0; valid && index < 4; ++index) {
                valid = ParseDecimal(parts[index], numbers[index]);
            }
            if (!valid || numbers[2] <= 0.0 || numbers[3] <= 0.0) {
                return Fail(error_, "InvalidValue", path + ".viewBox",
                            "viewBox is \"min-x min-y width height\" with a "
                            "positive width and height");
            }
            node.view_box =
                MotionViewBox{numbers[0], numbers[1], numbers[2], numbers[3]};
        }
        return true;
    }

    bool RepeatFields(const Value& raw, const std::string& path,
                      MotionNode& node) {
        const Value* count = raw.Find("count");
        const Value* each = raw.Find("each");
        if ((count == nullptr) == (each == nullptr)) {
            return Fail(error_, "InvalidValue", path,
                        "a repeat needs exactly one of count or each");
        }
        if (count != nullptr) {
            MotionValue value;
            if (!Binding(*count, path + ".count", value)) {
                return false;
            }
            int64_t number = 0;
            if (value.kind == MotionBindingKind::Literal &&
                (!value.literal.AsInt64(number) || number < 0 ||
                 number > kMaxRepeatCopies)) {
                return Fail(error_, "InvalidValue", path + ".count",
                            "count must be an integer from 0 to " +
                                std::to_string(kMaxRepeatCopies));
            }
            node.count = std::move(value);
        } else {
            MotionValue value;
            if (!Binding(*each, path + ".each", value)) {
                return false;
            }
            if (value.kind == MotionBindingKind::Literal) {
                bool valid = value.literal.IsArray() &&
                             value.literal.AsArray().size() <=
                                 static_cast<size_t>(kMaxRepeatCopies);
                for (size_t index = 0;
                     valid && index < value.literal.AsArray().size(); ++index) {
                    const Value& item = value.literal.AsArray()[index];
                    valid = item.IsString() || item.IsNumber() || item.IsBool();
                }
                if (!valid) {
                    return Fail(error_, "InvalidValue", path + ".each",
                                "each must be an array of strings, numbers or "
                                "booleans, or a list prop");
                }
            } else if (value.kind == MotionBindingKind::Prop &&
                       FindProp(value.prop)->type != MotionPropType::List) {
                return Fail(error_, "InvalidValue", path + ".each",
                            "each binds a list prop");
            }
            node.each = std::move(value);
        }
        const Value* templ = raw.Find("template");
        if (templ == nullptr) {
            return Fail(error_, "InvalidValue", path + ".template",
                        "a repeat needs a template node");
        }
        ++repeat_depth_;
        MotionNode child;
        bool ok = Node(*templ, path + ".template", child);
        if (ok) {
            node.children.push_back(std::move(child));
        }
        if (const Value* cycle = raw.Find("cycle"); ok && cycle != nullptr) {
            if (!cycle->IsObject()) {
                ok = Fail(error_, "InvalidValue", path + ".cycle",
                          "cycle must map style properties to arrays");
            }
            for (size_t index = 0; ok && index < cycle->AsObject().size();
                 ++index) {
                const auto& entry = cycle->AsObject()[index];
                const std::string entryPath = path + ".cycle." + entry.first;
                if (FindStyleRule(entry.first) == nullptr) {
                    ok = Fail(error_, "UnknownStyleProperty", entryPath,
                              UnknownStyleMessage(entry.first));
                    break;
                }
                if (!entry.second.IsArray() || entry.second.AsArray().empty()) {
                    ok = Fail(error_, "InvalidValue", entryPath,
                              "a cycle is a non-empty array of values");
                    break;
                }
                std::vector<MotionValue> values;
                for (size_t item = 0;
                     ok && item < entry.second.AsArray().size(); ++item) {
                    const std::string itemPath =
                        entryPath + "[" + std::to_string(item) + "]";
                    MotionValue value;
                    ok = Binding(entry.second.AsArray()[item], itemPath,
                                 value) &&
                         (value.kind != MotionBindingKind::Literal ||
                          ValidateMotionStyleValue(entry.first, value.literal,
                                                   itemPath, error_));
                    values.push_back(std::move(value));
                }
                node.cycle.emplace_back(entry.first, std::move(values));
            }
        }
        --repeat_depth_;
        if (!ok) {
            return false;
        }
        if (const Value* stagger = raw.Find("stagger")) {
            return Stagger(*stagger, path + ".stagger", node.stagger);
        }
        return true;
    }

    // REM2 -- where a video's or a sound's media is, and which part of it
    // plays. Exactly one of media (a project media, by ULID) or src (a file,
    // resolved as an image's is); startFrom and endAt are frames of this
    // composition's cadence, as Remotion counts them, so that a media at
    // another cadence is trimmed by the time the author sees.
    bool MediaFields(const Value& raw, const std::string& path,
                     MotionNode& node) {
        const Value* media = raw.Find("media");
        const Value* src = raw.Find("src");
        if ((media == nullptr) == (src == nullptr)) {
            return Fail(error_, "InvalidValue", path,
                        std::string("a ") + MotionNodeTypeName(node.type) +
                            " needs exactly one of media (a project media, "
                            "by ULID) or src (a file)");
        }
        MotionValue value;
        const std::string field = media != nullptr ? ".media" : ".src";
        if (!Binding(media != nullptr ? *media : *src, path + field, value)) {
            return false;
        }
        if (value.kind == MotionBindingKind::Literal) {
            if (!value.literal.IsString() || value.literal.AsString().empty()) {
                return Fail(error_, "InvalidValue", path + field,
                            field.substr(1) +
                                " must be a non-empty string or "
                                "a binding");
            }
            if (media != nullptr && !IsValidUlid(value.literal.AsString())) {
                return Fail(error_, "InvalidValue", path + ".media",
                            "media names a project media by its ULID");
            }
        }
        (media != nullptr ? node.media : node.src) = std::move(value);
        // MB11 -- startFrom and endAt bind a number prop, as a piece that
        // plays the next shot of a timeline from its in-point needs; the
        // fields then hold what the prop's default gives.
        const auto frames = [&](const Value& written, const char* key,
                                std::optional<MotionValue>& binding,
                                int64_t& value) {
            const Value* literal = &written;
            Value resolved;
            std::vector<Value> possible;
            if (written.IsObject()) {
                MotionValue bound;
                if (!BoundDefault(written, path + "." + key, bound, resolved,
                                  possible)) {
                    return false;
                }
                for (const Value& other : possible) {
                    int64_t frame = 0;
                    if (!other.AsInt64(frame) || frame < 0) {
                        return Fail(error_, "InvalidValue", path + "." + key,
                                    std::string(key) +
                                        " binds a prop whose values are "
                                        "whole numbers of frames, not " +
                                        other.Dump());
                    }
                }
                binding = std::move(bound);
                literal = &resolved;
            }
            if (!literal->AsInt64(value) || value < 0) {
                return Fail(error_, "InvalidValue", path + "." + key,
                            std::string(key) +
                                " must be a non-negative number of frames, "
                                "or a binding");
            }
            return true;
        };
        if (const Value* start = raw.Find("startFrom")) {
            if (!frames(*start, "startFrom", node.start_from_binding,
                        node.start_from)) {
                return false;
            }
        }
        if (const Value* end = raw.Find("endAt")) {
            int64_t value = 0;
            if (!frames(*end, "endAt", node.end_at_binding, value)) {
                return false;
            }
            if (value <= node.start_from) {
                return Fail(error_, "InvalidValue", path + ".endAt",
                            "endAt must be a number of frames after startFrom");
            }
            node.end_at = value;
        }
        if (const Value* rate = raw.Find("playbackRate")) {
            MotionValue speed;
            if (!Binding(*rate, path + ".playbackRate", speed)) {
                return false;
            }
            int64_t numerator = 0;
            int64_t denominator = 1;
            if (speed.kind == MotionBindingKind::Literal &&
                !ParseMotionTimeScale(speed.literal, numerator, denominator)) {
                return Fail(error_, "InvalidValue", path + ".playbackRate",
                            "playbackRate must be a positive number (a "
                            "multiplier) or a percentage such as \"50%\", at "
                            "most 1000");
            }
            node.playback_rate = std::move(speed);
        }
        if (const Value* muted = raw.Find("muted")) {
            if (!muted->IsBool()) {
                return Fail(error_, "InvalidValue", path + ".muted",
                            "muted must be a boolean");
            }
            node.muted = muted->AsBool();
        }
        if (node.type == MotionNodeType::Audio) {
            // A sound has no box: nothing else of a style applies to it.
            for (const auto& entry : node.style) {
                if (entry.first != "volume") {
                    return Fail(error_, "InvalidValue",
                                path + ".style." + entry.first,
                                "an audio node has no box: its style holds "
                                "volume only");
                }
            }
            for (const auto& entry : node.animate) {
                if (entry.first != "volume") {
                    return Fail(error_, "InvalidAnimation",
                                path + ".animate." + entry.first,
                                "an audio node has no box: it animates volume "
                                "only");
                }
            }
        }
        return true;
    }

    // REM2 -- an instance names a composition of the project and gives it
    // prop values. Whether that composition exists, and whether the values
    // fit its props, is the document's to say (Document::Validate) and the
    // evaluator's for a bound value: this composition alone cannot know.
    bool InstanceFields(const Value& raw, const std::string& path,
                        MotionNode& node) {
        const Value* target = raw.Find("composition");
        if (target == nullptr || !target->IsString() ||
            !IsValidUlid(target->AsString())) {
            return Fail(error_, "InvalidValue", path + ".composition",
                        "an instance names a composition of the project by "
                        "its ULID");
        }
        node.instance_of = target->AsString();
        if (const Value* props = raw.Find("props")) {
            if (!props->IsObject()) {
                return Fail(error_, "InvalidValue", path + ".props",
                            "props must be an object of prop values");
            }
            for (const auto& entry : props->AsObject()) {
                MotionValue value;
                if (!Binding(entry.second, path + ".props." + entry.first,
                             value)) {
                    return false;
                }
                node.instance_props.emplace_back(entry.first, std::move(value));
            }
        }
        return true;
    }

    // REM3 -- a transition's presentation and timing, kept apart as
    // Remotion keeps them: {"presentation": "slide", "direction":
    // "from-left", "timing": {"duration": 15, "easing": "ease-in-out"}}, or
    // a spring in place of the easing.
    bool TransitionFields(const Value& raw, const std::string& path,
                          MotionNode& node) {
        const Value* presentation = raw.Find("presentation");
        const std::string name =
            presentation != nullptr && presentation->IsString()
                ? presentation->AsString()
                : "";
        if (name == "fade") {
            node.presentation = MotionPresentation::Fade;
        } else if (name == "slide") {
            node.presentation = MotionPresentation::Slide;
        } else if (name == "wipe") {
            node.presentation = MotionPresentation::Wipe;
        } else if (name == "iris") {
            node.presentation = MotionPresentation::Iris;
        } else {
            return Fail(error_, "InvalidValue", path + ".presentation",
                        "presentation must be fade, slide, wipe or iris");
        }
        if (const Value* direction = raw.Find("direction")) {
            const std::string side =
                direction->IsString() ? direction->AsString() : "";
            if (node.presentation != MotionPresentation::Slide &&
                node.presentation != MotionPresentation::Wipe) {
                return Fail(error_, "InvalidValue", path + ".direction",
                            "a direction belongs to a slide or a wipe");
            }
            if (side == "from-left") {
                node.direction = MotionDirection::FromLeft;
            } else if (side == "from-right") {
                node.direction = MotionDirection::FromRight;
            } else if (side == "from-top") {
                node.direction = MotionDirection::FromTop;
            } else if (side == "from-bottom") {
                node.direction = MotionDirection::FromBottom;
            } else {
                return Fail(error_, "InvalidValue", path + ".direction",
                            "direction must be from-left, from-right, "
                            "from-top or from-bottom");
            }
        }
        const Value* timing = raw.Find("timing");
        if (timing == nullptr ||
            !CheckKeys(*timing, {"duration", "easing", "spring"},
                       path + ".timing", error_)) {
            return timing == nullptr
                       ? Fail(error_, "InvalidValue", path + ".timing",
                              "a transition needs a timing with a duration")
                       : false;
        }
        const Value* duration = timing->Find("duration");
        int64_t frames = 0;
        if (duration == nullptr || !duration->AsInt64(frames) || frames <= 0) {
            return Fail(error_, "InvalidValue", path + ".timing.duration",
                        "a transition lasts a positive number of frames");
        }
        node.duration = frames;
        const Value* easing = timing->Find("easing");
        const Value* spring = timing->Find("spring");
        if (easing != nullptr && spring != nullptr) {
            return Fail(error_, "InvalidValue", path + ".timing",
                        "a timing is an easing or a spring, not both");
        }
        if (easing != nullptr &&
            (!easing->IsString() ||
             !ParseEasing(easing->AsString(), node.timing_interpolation,
                          node.timing_easing))) {
            return Fail(error_, "InvalidValue", path + ".timing.easing",
                        "easing must be linear, ease, ease-in, ease-out, "
                        "ease-in-out, step-end or cubic-bezier(x1, y1, x2, "
                        "y2)");
        }
        if (spring != nullptr) {
            MotionAnimation animation;
            if (!Spring(*spring, path + ".timing.spring", animation)) {
                return false;
            }
            if (spring->Find("from") != nullptr ||
                spring->Find("to") != nullptr ||
                spring->Find("delay") != nullptr) {
                return Fail(error_, "InvalidValue", path + ".timing.spring",
                            "a transition's spring runs from 0 to 1 over its "
                            "duration: no from, to or delay");
            }
            node.timing_spring = true;
            node.timing_spring_config = animation.spring;
            node.timing_spring_config.from = 0.0;
            node.timing_spring_config.to = 1.0;
        }
        return true;
    }

    // REM2 -- volume is a sound's: nothing else plays one.
    bool VolumePlacement(const std::string& path, const MotionNode& node) {
        if (node.type == MotionNodeType::Video ||
            node.type == MotionNodeType::Audio) {
            return true;
        }
        for (const auto& entry : node.style) {
            if (entry.first == "volume") {
                return Fail(error_, "InvalidValue", path + ".style.volume",
                            "volume applies to a video or an audio node");
            }
        }
        for (const auto& entry : node.animate) {
            if (entry.first == "volume") {
                return Fail(error_, "InvalidAnimation",
                            path + ".animate.volume",
                            "volume applies to a video or an audio node");
            }
        }
        return true;
    }

    // M3 -- a persistent identity: a ULID, used once in the composition,
    // nodes and keyframes alike.
    bool Identity(const Value& raw, const std::string& path, std::string& out) {
        if (!raw.IsString() || !IsValidUlid(raw.AsString())) {
            return Fail(error_, "InvalidValue", path,
                        "a uid is a ULID (26 characters of Crockford base 32)");
        }
        if (!uids_.insert(raw.AsString()).second) {
            return Fail(error_, "DuplicateUid", path,
                        "uid '" + raw.AsString() + "' is used twice");
        }
        out = raw.AsString();
        return true;
    }

    MotionComposition& composition_;
    MotionError& error_;
    std::set<std::string> ids_;
    std::set<std::string> uids_;
    int repeat_depth_ = 0;
};

}  // namespace

const char* MotionNodeTypeName(MotionNodeType type) {
    switch (type) {
        case MotionNodeType::Fill:
            return "fill";
        case MotionNodeType::Box:
            return "box";
        case MotionNodeType::Text:
            return "text";
        case MotionNodeType::Image:
            return "image";
        case MotionNodeType::Sequence:
            return "sequence";
        case MotionNodeType::Path:
            return "path";
        case MotionNodeType::Repeat:
            return "repeat";
        case MotionNodeType::Series:
            return "series";
        case MotionNodeType::Video:
            return "video";
        case MotionNodeType::Audio:
            return "audio";
        case MotionNodeType::Composition:
            return "composition";
        case MotionNodeType::Transition:
            return "transition";
    }
    return "unknown";
}

bool ValidateMotionPropValue(const MotionPropDefinition& prop,
                             const Value& value, const std::string& path,
                             MotionError& error) {
    MotionColor color;
    double number = 0.0;
    switch (prop.type) {
        case MotionPropType::Text:
            if (value.IsString()) {
                return true;
            }
            break;
        case MotionPropType::Bool:
            if (value.IsBool()) {
                return true;
            }
            break;
        case MotionPropType::Color:
            if (value.IsString() && ParseMotionColor(value.AsString(), color)) {
                return true;
            }
            break;
        case MotionPropType::Enum:
            if (value.IsString() && OneOf(value.AsString(), prop.values)) {
                return true;
            }
            return Fail(error, "PropOutOfRange", path,
                        "value must be one of: " + Join(prop.values));
        case MotionPropType::Number:
            if (!MotionNumber(value, number)) {
                break;
            }
            // Refused, never clamped: a bound that silently moved the value
            // would show a template that does not match what was asked.
            if ((prop.minimum && number < *prop.minimum) ||
                (prop.maximum && number > *prop.maximum)) {
                return Fail(error, "PropOutOfRange", path,
                            "value is outside the prop's bounds");
            }
            return true;
        case MotionPropType::List: {
            if (!value.IsArray()) {
                break;
            }
            if (value.AsArray().size() >
                static_cast<size_t>(kMaxRepeatCopies)) {
                return Fail(error, "PropOutOfRange", path,
                            "a list holds at most " +
                                std::to_string(kMaxRepeatCopies) + " items");
            }
            MotionPropDefinition element = prop;
            element.type = prop.element_type;
            for (size_t index = 0; index < value.AsArray().size(); ++index) {
                if (!ValidateMotionPropValue(
                        element, value.AsArray()[index],
                        path + "[" + std::to_string(index) + "]", error)) {
                    return false;
                }
            }
            return true;
        }
        case MotionPropType::Point:
        case MotionPropType::Size: {
            // M3 -- an object of exactly its two components, each a number
            // within the prop's bounds; a size is never negative.
            const bool point = prop.type == MotionPropType::Point;
            const std::vector<std::string> components =
                point ? std::vector<std::string>{"x", "y"}
                      : std::vector<std::string>{"width", "height"};
            if (!value.IsObject() || value.AsObject().size() != 2) {
                break;
            }
            MotionPropDefinition component = prop;
            component.type = MotionPropType::Number;
            for (const std::string& name : components) {
                const Value* part = value.Find(name);
                double number = 0.0;
                if (part == nullptr || !MotionNumber(*part, number)) {
                    std::string shape = point ? "a point" : "a size";
                    shape += " is {\"";
                    shape += components[0];
                    shape += "\": number, \"";
                    shape += components[1];
                    shape += "\": number}";
                    return Fail(error, "PropTypeMismatch", path, shape);
                }
                std::string partPath = path;
                partPath += ".";
                partPath += name;
                if (!point && number < 0.0) {
                    return Fail(error, "PropOutOfRange", partPath,
                                "a size is never negative");
                }
                if (!ValidateMotionPropValue(component, *part, partPath,
                                             error)) {
                    return false;
                }
            }
            return true;
        }
        case MotionPropType::Group:
            return Fail(error, "PropTypeMismatch", path,
                        "'" + prop.name +
                            "' is a group: it has no value, set the props it "
                            "gathers");
    }
    return Fail(error, "PropTypeMismatch", path,
                "value does not match the type of prop '" + prop.name + "'");
}

bool MotionNumber(const Value& value, double& number) {
    if (!value.IsNumber()) {
        return false;
    }
    return ParseDecimal(value.Dump(), number);
}

bool ParseMotionColor(const std::string& text, MotionColor& color) {
    if (text == "transparent") {
        color = {0.0, 0.0, 0.0, 0.0};
        return true;
    }
    if (text == "white") {
        color = {1.0, 1.0, 1.0, 1.0};
        return true;
    }
    if (text == "black") {
        color = {0.0, 0.0, 0.0, 1.0};
        return true;
    }
    const auto hex = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    if (!text.empty() && text[0] == '#') {
        std::vector<int> digits;
        for (size_t index = 1; index < text.size(); ++index) {
            const int digit = hex(text[index]);
            if (digit < 0) {
                return false;
            }
            digits.push_back(digit);
        }
        if (digits.size() == 3) {
            color = {digits[0] * 17 / 255.0, digits[1] * 17 / 255.0,
                     digits[2] * 17 / 255.0, 1.0};
            return true;
        }
        if (digits.size() == 6 || digits.size() == 8) {
            const auto channel = [&](size_t offset) {
                return (digits[offset] * 16 + digits[offset + 1]) / 255.0;
            };
            color = {channel(0), channel(2), channel(4),
                     digits.size() == 8 ? channel(6) : 1.0};
            return true;
        }
        return false;
    }
    for (const char* function : {"rgba(", "rgb("}) {
        const std::string prefix = function;
        if (text.compare(0, prefix.size(), prefix) != 0 || text.back() != ')') {
            continue;
        }
        std::string inner =
            text.substr(prefix.size(), text.size() - prefix.size() - 1);
        std::replace(inner.begin(), inner.end(), ',', ' ');
        const std::vector<std::string> parts = SplitSpaces(inner);
        const size_t expected = prefix == "rgba(" ? 4 : 3;
        if (parts.size() != expected) {
            return false;
        }
        double channels[4] = {0.0, 0.0, 0.0, 1.0};
        for (size_t index = 0; index < expected; ++index) {
            if (!ParseDecimal(parts[index], channels[index])) {
                return false;
            }
        }
        for (size_t index = 0; index < 3; ++index) {
            if (channels[index] < 0.0 || channels[index] > 255.0) {
                return false;
            }
        }
        if (channels[3] < 0.0 || channels[3] > 1.0) {
            return false;
        }
        color = {channels[0] / 255.0, channels[1] / 255.0, channels[2] / 255.0,
                 channels[3]};
        return true;
    }
    return false;
}

bool ParseMotionLength(const Value& value, bool allow_auto,
                       MotionLength& length) {
    double number = 0.0;
    if (MotionNumber(value, number)) {
        length = {MotionLengthUnit::Px, number};
        return true;
    }
    return value.IsString() &&
           ParseLengthText(value.AsString(), allow_auto, length);
}

bool IsAnimatableMotionProperty(const std::string& property) {
    const StyleRule* rule = FindStyleRule(property);
    return rule != nullptr && rule->animatable;
}

bool ValidateMotionStyleValue(const std::string& property, const Value& value,
                              const std::string& path, MotionError& error) {
    const StyleRule* rule = FindStyleRule(property);
    if (rule == nullptr) {
        return Fail(error, "UnknownStyleProperty", path,
                    UnknownStyleMessage(property));
    }
    MotionLength length;
    MotionColor color;
    double number = 0.0;
    switch (rule->kind) {
        case StyleKind::Keyword:
            if (value.IsString() && OneOf(value.AsString(), rule->keywords)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be one of: " + Join(rule->keywords));
        case StyleKind::Length:
        case StyleKind::LengthOrAuto:
            if (ParseMotionLength(value, rule->kind == StyleKind::LengthOrAuto,
                                  length)) {
                return true;
            }
            return Fail(
                error, "InvalidValue", path,
                property +
                    " must be pixels (number or \"12px\") or a "
                    "percentage (\"45%\")" +
                    (rule->kind == StyleKind::LengthOrAuto ? " or \"auto\""
                                                           : ""));
        case StyleKind::LengthShorthand:
        case StyleKind::LengthOrAutoShorthand: {
            if (MotionNumber(value, number)) {
                return true;
            }
            if (value.IsString()) {
                const std::vector<std::string> parts =
                    SplitSpaces(value.AsString());
                bool valid = !parts.empty() && parts.size() <= 4;
                for (const std::string& part : parts) {
                    MotionLength ignored;
                    valid = valid &&
                            ParseLengthText(
                                part,
                                rule->kind == StyleKind::LengthOrAutoShorthand,
                                ignored);
                }
                if (valid) {
                    return true;
                }
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be a number or one to four CSS "
                            "lengths such as \"4px 10px\"");
        }
        case StyleKind::Number:
            if (MotionNumber(value, number)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be a number");
        case StyleKind::UnitInterval:
            if (MotionNumber(value, number) && number >= 0.0 && number <= 1.0) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be a number between 0 and 1");
        case StyleKind::Pixels:
            if (MotionNumber(value, number) ||
                (value.IsString() &&
                 ParseLengthText(value.AsString(), false, length) &&
                 length.unit == MotionLengthUnit::Px)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be pixels (number or \"12px\")");
        case StyleKind::FontWeight:
            if (value.IsString() &&
                OneOf(value.AsString(), {"normal", "bold"})) {
                return true;
            }
            if (MotionNumber(value, number) && number >= 1.0 &&
                number <= 1000.0) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        "fontWeight must be normal, bold or 1 to 1000");
        case StyleKind::LineHeight:
            // CSS and React: a bare number multiplies the font size; pixels
            // must say so.
            if (MotionNumber(value, number) && number > 0.0) {
                return true;
            }
            if (value.IsString() &&
                ParseLengthText(value.AsString(), false, length) &&
                length.unit == MotionLengthUnit::Px && length.value > 0.0) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        "lineHeight must be a multiplier (number) or pixels "
                        "(\"72px\")");
        case StyleKind::Color:
            if (value.IsString() && ParseMotionColor(value.AsString(), color)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be #rgb, #rrggbb, #rrggbbaa, rgb(), "
                            "rgba(), transparent, white or black");
        case StyleKind::String:
            if (value.IsString() && !value.AsString().empty()) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be a non-empty string");
        case StyleKind::DashArray: {
            std::vector<double> lengths;
            if (value.IsString() &&
                ParseMotionDashArray(value.AsString(), lengths)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be none or lengths in pixels, none "
                            "negative, separated by spaces or commas "
                            "(\"10 5\", \"10px, 5px\")");
        }
        case StyleKind::Paint:
            if (value.IsString() &&
                (value.AsString() == "none" ||
                 value.AsString() == "currentColor" ||
                 ParseMotionColor(value.AsString(), color))) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property + " must be a colour, none or currentColor");
        case StyleKind::Polygon: {
            MotionClipShape shape;
            if (value.IsString() &&
                (value.AsString() == "none" ||
                 ParseMotionClipShape(value.AsString(), shape))) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be none, polygon(x y, x y, x y, ...), "
                            "circle(r at x y), ellipse(rx ry at x y) or "
                            "inset(top right bottom left round r), with "
                            "lengths in px or %");
        }
        case StyleKind::Gradient: {
            MotionGradient gradient;
            if (value.IsString() &&
                (value.AsString() == "none" ||
                 ParseMotionGradient(value.AsString(), gradient))) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be none, linear-gradient(<angle> | to "
                            "<side>, <colour> [<n>%], ...) or radial-gradient("
                            "[circle | ellipse] [<extent>] [at <x> <y>], "
                            "<colour> [<n>%], ...)");
        }
        case StyleKind::Filter: {
            std::vector<MotionFilter> filters;
            if (value.IsString() &&
                (value.AsString() == "none" ||
                 ParseMotionFilters(value.AsString(), filters))) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be none or a list of blur(<σ>px), "
                            "drop-shadow(<dx>px <dy>px <σ>px <colour>), "
                            "glow(<σ>px <colour> <strength>), grain(<amount> "
                            "<size>px <seed> <phase>), displace("
                            "<amplitude>px <wavelength>px <seed> <phase>), "
                            "goo(<σ>px), slice(<amplitude>px <band>px <seed> "
                            "<phase>) and mesh(<rows> <columns> [<quality>], "
                            "<x> <y>, ...) with (rows + 1) x (columns + 1) "
                            "vertices");
        }
        case StyleKind::TextStroke: {
            if (value.IsString() && value.AsString() == "none") {
                return true;
            }
            const std::vector<std::string> parts =
                value.IsString() ? SplitSpaces(value.AsString())
                                 : std::vector<std::string>();
            if (parts.size() == 2 && ParseLengthText(parts[0], false, length) &&
                length.unit == MotionLengthUnit::Px && length.value >= 0.0 &&
                ParseMotionColor(parts[1], color)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be none or \"<width>px <colour>\", as "
                            "-webkit-text-stroke");
        }
        case StyleKind::NumericVariant: {
            std::string figures;
            std::string spacing;
            if (value.IsString() &&
                ParseMotionNumericVariant(value.AsString(), figures, spacing)) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be normal, or lining-nums or "
                            "oldstyle-nums and/or tabular-nums or "
                            "proportional-nums, separated by a space");
        }
        case StyleKind::Distance: {
            if (value.IsString() && value.AsString() == "none") {
                return true;
            }
            bool distance = MotionNumber(value, number);
            if (!distance && value.IsString() &&
                ParseLengthText(value.AsString(), false, length) &&
                length.unit == MotionLengthUnit::Px) {
                number = length.value;
                distance = true;
            }
            if (distance && number > 0.0) {
                return true;
            }
            return Fail(
                error, "InvalidValue", path,
                property + " must be none or a positive distance in pixels");
        }
        case StyleKind::Volume:
            if (MotionNumber(value, number) && number >= 0.0 && number <= 4.0) {
                return true;
            }
            return Fail(error, "InvalidValue", path,
                        property +
                            " must be a number from 0 to 4 (a linear gain: 1 "
                            "plays the media as recorded, 4 is +12 dB)");
    }
    return false;
}

MotionValueKind MotionPropertyValueKind(const std::string& property) {
    if (property == "d") {
        return MotionValueKind::Path;
    }
    const StyleRule* rule = FindStyleRule(property);
    if (rule != nullptr &&
        (rule->kind == StyleKind::Color || rule->kind == StyleKind::Paint)) {
        return MotionValueKind::Color;
    }
    if (rule != nullptr && rule->kind == StyleKind::Polygon) {
        return MotionValueKind::Polygon;
    }
    if (rule != nullptr && rule->kind == StyleKind::Gradient) {
        return MotionValueKind::Gradient;
    }
    if (rule != nullptr && rule->kind == StyleKind::Filter) {
        return MotionValueKind::Filter;
    }
    return MotionValueKind::Number;
}

bool IsMotionUnitProperty(const std::string& property) {
    return OneOf(property,
                 {"opacity", "translateX", "translateY", "scale", "scaleX",
                  "scaleY", "rotate", "color", "backgroundColor"});
}

bool ParseMotionPolygon(const std::string& text, MotionPolygon& polygon) {
    const std::string prefix = "polygon(";
    if (text.compare(0, prefix.size(), prefix) != 0 || text.back() != ')') {
        return false;
    }
    const std::string inner =
        text.substr(prefix.size(), text.size() - prefix.size() - 1);
    MotionPolygon parsed;
    std::istringstream stream(inner);
    std::string vertex;
    while (std::getline(stream, vertex, ',')) {
        const std::vector<std::string> parts = SplitSpaces(vertex);
        MotionLength x;
        MotionLength y;
        if (parts.size() != 2 || !ParseLengthText(parts[0], false, x) ||
            !ParseLengthText(parts[1], false, y)) {
            return false;
        }
        parsed.emplace_back(x, y);
    }
    if (parsed.size() < 3) {
        return false;
    }
    polygon = std::move(parsed);
    return true;
}

namespace {

// REM3 -- `text` cut at the commas (or spaces) that are not inside
// parentheses: "rgba(0, 0, 0, 0.5) 40%" stays one piece.
std::vector<std::string> SplitTopLevel(const std::string& text,
                                       char separator) {
    std::vector<std::string> parts;
    std::string current;
    int depth = 0;
    for (const char character : text) {
        if (character == '(') {
            ++depth;
        } else if (character == ')') {
            --depth;
        }
        const bool split =
            depth == 0 &&
            ((separator == ' '
                  ? std::isspace(static_cast<unsigned char>(character))
                  : static_cast<int>(character == separator)) != 0);
        if (split) {
            if (separator != ' ' || !current.empty()) {
                parts.push_back(current);
            }
            current.clear();
            continue;
        }
        current.push_back(character);
    }
    if (separator != ' ' || !current.empty()) {
        parts.push_back(current);
    }
    for (std::string& part : parts) {
        const size_t first = part.find_first_not_of(" \t\n");
        const size_t last = part.find_last_not_of(" \t\n");
        part = first == std::string::npos
                   ? std::string()
                   : part.substr(first, last - first + 1);
    }
    return parts;
}

// "name(inner)" -> name and inner, the parentheses balanced.
bool FunctionCall(const std::string& text, std::string& name,
                  std::string& inner) {
    const size_t open = text.find('(');
    if (open == std::string::npos || open == 0 || text.back() != ')') {
        return false;
    }
    name = text.substr(0, open);
    inner = text.substr(open + 1, text.size() - open - 2);
    int depth = 0;
    for (const char character : inner) {
        depth += character == '(' ? 1 : (character == ')' ? -1 : 0);
        if (depth < 0) {
            return false;
        }
    }
    return depth == 0;
}

bool PixelLength(const std::string& text, double& pixels) {
    MotionLength length;
    if (!ParseLengthText(text, false, length) ||
        length.unit != MotionLengthUnit::Px) {
        return false;
    }
    pixels = length.value;
    return true;
}

// MA7 -- the seed of displace(), slice() or grain(): a whole number from 0
// to INT64_MAX, written in full. strtoll holds a larger one at INT64_MAX,
// and two seeds written drew one pattern.
bool ParseSeed(const std::string& text, int64_t& seed) {
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0' || errno == ERANGE || value < 0) {
        return false;
    }
    seed = value;
    return true;
}

// A position keyword or a length, for "at <x> <y>".
bool PositionLength(const std::string& text, bool horizontal,
                    MotionLength& length) {
    if (text == "center") {
        length = {MotionLengthUnit::Percent, 50.0};
        return true;
    }
    if (text == (horizontal ? "left" : "top")) {
        length = {MotionLengthUnit::Percent, 0.0};
        return true;
    }
    if (text == (horizontal ? "right" : "bottom")) {
        length = {MotionLengthUnit::Percent, 100.0};
        return true;
    }
    return ParseLengthText(text, false, length);
}

bool Radius(const std::string& text, MotionLength& length,
            MotionRadiusKeyword& keyword) {
    if (text == "closest-side") {
        keyword = MotionRadiusKeyword::ClosestSide;
        return true;
    }
    if (text == "farthest-side") {
        keyword = MotionRadiusKeyword::FarthestSide;
        return true;
    }
    keyword = MotionRadiusKeyword::None;
    return ParseLengthText(text, false, length) && length.value >= 0.0;
}

// "... at <x> <y>" at the end of a shape's or gradient's arguments.
bool AtPosition(std::vector<std::string>& tokens, MotionLength& x,
                MotionLength& y) {
    const auto at = std::find(tokens.begin(), tokens.end(), "at");
    if (at == tokens.end()) {
        return true;
    }
    if (tokens.end() - at != 3 || !PositionLength(*(at + 1), true, x) ||
        !PositionLength(*(at + 2), false, y)) {
        return false;
    }
    tokens.erase(at, tokens.end());
    return true;
}

}  // namespace

bool ParseMotionNumericVariant(const std::string& text, std::string& figures,
                               std::string& spacing) {
    figures.clear();
    spacing.clear();
    const std::vector<std::string> words = SplitSpaces(text);
    if (words.size() == 1 && words[0] == "normal") {
        return true;
    }
    if (words.empty()) {
        return false;
    }
    for (const std::string& word : words) {
        std::string& slot = word == "lining-nums" || word == "oldstyle-nums"
                                ? figures
                                : spacing;
        if (word != "lining-nums" && word != "oldstyle-nums" &&
            word != "tabular-nums" && word != "proportional-nums") {
            return false;
        }
        if (!slot.empty()) {
            return false;
        }
        slot = word.substr(0, word.size() - std::string("-nums").size());
    }
    return true;
}

bool ParseMotionClipShape(const std::string& text, MotionClipShape& shape) {
    MotionClipShape parsed;
    std::string name;
    std::string inner;
    if (!FunctionCall(text, name, inner)) {
        return false;
    }
    if (name == "polygon") {
        parsed.kind = MotionClipKind::Polygon;
        if (!ParseMotionPolygon(text, parsed.polygon)) {
            return false;
        }
        shape = std::move(parsed);
        return true;
    }
    std::vector<std::string> tokens = SplitTopLevel(inner, ' ');
    if (name == "circle" || name == "ellipse") {
        parsed.kind =
            name == "circle" ? MotionClipKind::Circle : MotionClipKind::Ellipse;
        if (!AtPosition(tokens, parsed.center_x, parsed.center_y)) {
            return false;
        }
        const size_t radii = name == "circle" ? 1 : 2;
        if (!tokens.empty() && tokens.size() != radii) {
            return false;
        }
        if (!tokens.empty() &&
            !Radius(tokens[0], parsed.radius_x, parsed.keyword_x)) {
            return false;
        }
        if (tokens.size() == 2 &&
            !Radius(tokens[1], parsed.radius_y, parsed.keyword_y)) {
            return false;
        }
        shape = std::move(parsed);
        return true;
    }
    if (name == "inset") {
        parsed.kind = MotionClipKind::Inset;
        const auto round = std::find(tokens.begin(), tokens.end(), "round");
        if (round != tokens.end()) {
            if (tokens.end() - round != 2 ||
                !ParseLengthText(*(round + 1), false, parsed.round) ||
                parsed.round.value < 0.0) {
                return false;
            }
            tokens.erase(round, tokens.end());
        }
        if (tokens.empty() || tokens.size() > 4) {
            return false;
        }
        MotionLength lengths[4];
        for (size_t index = 0; index < tokens.size(); ++index) {
            if (!ParseLengthText(tokens[index], false, lengths[index])) {
                return false;
            }
        }
        parsed.inset[0] = lengths[0];
        parsed.inset[1] = tokens.size() > 1 ? lengths[1] : lengths[0];
        parsed.inset[2] = tokens.size() > 2 ? lengths[2] : lengths[0];
        parsed.inset[3] = tokens.size() > 3 ? lengths[3] : parsed.inset[1];
        shape = std::move(parsed);
        return true;
    }
    return false;
}

bool ParseMotionGradient(const std::string& text, MotionGradient& gradient) {
    MotionGradient parsed;
    std::string name;
    std::string inner;
    if (!FunctionCall(text, name, inner)) {
        return false;
    }
    if (name != "linear-gradient" && name != "radial-gradient") {
        return false;
    }
    parsed.kind = name == "linear-gradient" ? MotionGradientKind::Linear
                                            : MotionGradientKind::Radial;
    std::vector<std::string> arguments = SplitTopLevel(inner, ',');
    if (arguments.empty()) {
        return false;
    }
    std::vector<std::string> head = SplitTopLevel(arguments.front(), ' ');
    MotionColor probe;
    const bool firstIsStop =
        !head.empty() && ParseMotionColor(head.front(), probe);
    if (!firstIsStop) {
        if (parsed.kind == MotionGradientKind::Linear) {
            if (head.size() == 1 && head[0].size() > 3 &&
                head[0].compare(head[0].size() - 3, 3, "deg") == 0) {
                if (!ParseDecimal(head[0].substr(0, head[0].size() - 3),
                                  parsed.angle)) {
                    return false;
                }
            } else if (head.size() >= 2 && head.size() <= 3 &&
                       head[0] == "to") {
                for (size_t index = 1; index < head.size(); ++index) {
                    const std::string& side = head[index];
                    if (side == "left" && parsed.corner_x == 0) {
                        parsed.corner_x = -1;
                    } else if (side == "right" && parsed.corner_x == 0) {
                        parsed.corner_x = 1;
                    } else if (side == "top" && parsed.corner_y == 0) {
                        parsed.corner_y = -1;
                    } else if (side == "bottom" && parsed.corner_y == 0) {
                        parsed.corner_y = 1;
                    } else {
                        return false;
                    }
                }
                // A side is an angle; a corner depends on the box.
                if (parsed.corner_x == 0 || parsed.corner_y == 0) {
                    parsed.angle = parsed.corner_y == -1  ? 0.0
                                   : parsed.corner_x == 1 ? 90.0
                                   : parsed.corner_y == 1 ? 180.0
                                                          : 270.0;
                    parsed.corner_x = 0;
                    parsed.corner_y = 0;
                }
            } else {
                return false;
            }
        } else {
            if (!AtPosition(head, parsed.center_x, parsed.center_y)) {
                return false;
            }
            for (const std::string& token : head) {
                if (token == "circle") {
                    parsed.circle = true;
                } else if (token == "ellipse") {
                    parsed.circle = false;
                } else if (OneOf(token, {"closest-side", "closest-corner",
                                         "farthest-side", "farthest-corner"})) {
                    parsed.extent = token;
                } else {
                    return false;
                }
            }
        }
        arguments.erase(arguments.begin());
    }
    if (arguments.size() < 2) {
        return false;
    }
    for (const std::string& argument : arguments) {
        std::vector<std::string> parts = SplitTopLevel(argument, ' ');
        MotionGradientStop stop;
        if (parts.empty() || parts.size() > 2 ||
            !ParseMotionColor(parts[0], stop.color)) {
            return false;
        }
        if (parts.size() == 2) {
            MotionLength position;
            if (!ParseLengthText(parts[1], false, position) ||
                position.unit != MotionLengthUnit::Percent) {
                return false;
            }
            stop.has_position = true;
            stop.position = position.value / 100.0;
        }
        parsed.stops.push_back(stop);
    }
    gradient = std::move(parsed);
    return true;
}

std::vector<double> MotionGradientPositions(const MotionGradient& gradient) {
    const size_t count = gradient.stops.size();
    std::vector<double> positions(count, 0.0);
    std::vector<bool> known(count, false);
    for (size_t index = 0; index < count; ++index) {
        known[index] = gradient.stops[index].has_position;
        positions[index] = gradient.stops[index].position;
    }
    if (count == 0) {
        return positions;
    }
    if (!known.front()) {
        positions.front() = 0.0;
        known.front() = true;
    }
    if (!known.back()) {
        positions.back() = 1.0;
        known.back() = true;
    }
    // Each position at least the one before it, then the unknown ones spread
    // evenly between their positioned neighbours.
    double highest = positions.front();
    for (size_t index = 0; index < count; ++index) {
        if (known[index]) {
            positions[index] = std::max(positions[index], highest);
            highest = positions[index];
        }
    }
    size_t start = 0;
    for (size_t index = 1; index < count; ++index) {
        if (!known[index]) {
            continue;
        }
        for (size_t between = start + 1; between < index; ++between) {
            positions[between] =
                positions[start] + (positions[index] - positions[start]) *
                                       static_cast<double>(between - start) /
                                       static_cast<double>(index - start);
        }
        start = index;
    }
    return positions;
}

bool ParseMotionFilters(const std::string& text,
                        std::vector<MotionFilter>& filters) {
    std::vector<MotionFilter> parsed;
    const std::vector<std::string> calls = SplitTopLevel(text, ' ');
    if (calls.empty()) {
        return false;
    }
    for (const std::string& call : calls) {
        std::string name;
        std::string inner;
        if (!FunctionCall(call, name, inner)) {
            return false;
        }
        const std::vector<std::string> tokens = SplitTopLevel(inner, ' ');
        MotionFilter filter;
        if (name == "blur") {
            filter.kind = MotionFilterKind::Blur;
            if (tokens.size() != 1 || !PixelLength(tokens[0], filter.sigma)) {
                return false;
            }
        } else if (name == "drop-shadow") {
            filter.kind = MotionFilterKind::DropShadow;
            filter.color = {0.0, 0.0, 0.0, 1.0};
            std::vector<double> lengths;
            bool colored = false;
            for (const std::string& token : tokens) {
                double pixels = 0.0;
                if (PixelLength(token, pixels)) {
                    lengths.push_back(pixels);
                } else if (!colored && ParseMotionColor(token, filter.color)) {
                    colored = true;
                } else {
                    return false;
                }
            }
            if (lengths.size() < 2 || lengths.size() > 3) {
                return false;
            }
            filter.offset_x = lengths[0];
            filter.offset_y = lengths[1];
            filter.sigma = lengths.size() == 3 ? lengths[2] : 0.0;
        } else if (name == "glow") {
            filter.kind = MotionFilterKind::Glow;
            if (tokens.size() < 2 || tokens.size() > 3 ||
                !PixelLength(tokens[0], filter.sigma) ||
                !ParseMotionColor(tokens[1], filter.color) ||
                (tokens.size() == 3 &&
                 !ParseDecimal(tokens[2], filter.strength)) ||
                filter.strength < 0.0 || filter.strength > 16.0) {
                return false;
            }
        } else if (name == "displace" || name == "slice") {
            filter.kind = name == "slice" ? MotionFilterKind::Slice
                                          : MotionFilterKind::Displace;
            if (tokens.size() < 3 || tokens.size() > 4 ||
                !PixelLength(tokens[0], filter.amplitude) ||
                !PixelLength(tokens[1], filter.wavelength) ||
                !ParseSeed(tokens[2], filter.seed) ||
                (tokens.size() == 4 &&
                 !ParseDecimal(tokens[3], filter.phase)) ||
                filter.amplitude < 0.0 || filter.amplitude > 500.0 ||
                filter.wavelength <= 0.0) {
                return false;
            }
        } else if (name == "goo") {
            filter.kind = MotionFilterKind::Goo;
            if (tokens.size() != 1 || !PixelLength(tokens[0], filter.sigma)) {
                return false;
            }
        } else if (name == "mesh") {
            // MOTION-DESIGN-2026-09 -- the grid, then one "x y" per vertex.
            filter.kind = MotionFilterKind::Mesh;
            const std::vector<std::string> parts = SplitTopLevel(inner, ',');
            const std::vector<std::string> grid =
                parts.empty() ? std::vector<std::string>()
                              : SplitTopLevel(parts[0], ' ');
            const auto count = [](const std::string& text, int32_t low,
                                  int32_t high, int32_t& out) {
                char* end = nullptr;
                const long long value = std::strtoll(text.c_str(), &end, 10);
                if (end == text.c_str() || *end != '\0' || value < low ||
                    value > high) {
                    return false;
                }
                out = static_cast<int32_t>(value);
                return true;
            };
            if (grid.size() < 2 || grid.size() > 3 ||
                !count(grid[0], 1, 32, filter.mesh_rows) ||
                !count(grid[1], 1, 32, filter.mesh_columns) ||
                (grid.size() == 3 &&
                 !count(grid[2], 1, 10, filter.mesh_quality))) {
                return false;
            }
            const size_t vertices =
                static_cast<size_t>(filter.mesh_rows + 1) *
                static_cast<size_t>(filter.mesh_columns + 1);
            if (parts.size() != vertices + 1) {
                return false;
            }
            for (size_t index = 1; index < parts.size(); ++index) {
                const std::vector<std::string> xy =
                    SplitTopLevel(parts[index], ' ');
                MotionLength x;
                MotionLength y;
                if (xy.size() != 2 || !ParseLengthText(xy[0], false, x) ||
                    !ParseLengthText(xy[1], false, y)) {
                    return false;
                }
                filter.mesh_points.emplace_back(x, y);
            }
        } else if (name == "grain") {
            // MOTION-DESIGN-2026-09 -- amount, then size, seed and phase.
            filter.kind = MotionFilterKind::Grain;
            filter.wavelength = 1.0;
            if (tokens.empty() || tokens.size() > 4 ||
                !ParseDecimal(tokens[0], filter.amplitude) ||
                filter.amplitude < 0.0 || filter.amplitude > 1.0 ||
                (tokens.size() >= 2 &&
                 (!PixelLength(tokens[1], filter.wavelength) ||
                  filter.wavelength < 1.0 || filter.wavelength > 64.0))) {
                return false;
            }
            if (tokens.size() >= 3 && !ParseSeed(tokens[2], filter.seed)) {
                return false;
            }
            if (tokens.size() == 4 && !ParseDecimal(tokens[3], filter.phase)) {
                return false;
            }
        } else {
            return false;
        }
        // A deviation past 250 pixels blurs a frame into one colour, and
        // costs as much as it shows nothing.
        if (filter.sigma < 0.0 || filter.sigma > 250.0) {
            return false;
        }
        parsed.push_back(filter);
    }
    filters = std::move(parsed);
    return true;
}

bool ParseMotionDashArray(const std::string& text,
                          std::vector<double>& lengths) {
    if (text == "none") {
        lengths.clear();
        return true;
    }
    std::vector<double> parsed;
    size_t at = 0;
    const auto blank = [&] {
        while (at < text.size() &&
               std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
    };
    blank();
    while (at < text.size()) {
        const size_t start = at;
        while (at < text.size() && text[at] != ',' &&
               std::isspace(static_cast<unsigned char>(text[at])) == 0) {
            ++at;
        }
        std::string item = text.substr(start, at - start);
        if (item.size() > 2 && item.compare(item.size() - 2, 2, "px") == 0) {
            item.resize(item.size() - 2);
        }
        double length = 0.0;
        if (!ParseDecimal(item, length) || length < 0.0) {
            return false;
        }
        parsed.push_back(length);
        blank();
        // One comma between two lengths, never at the end.
        if (at < text.size() && text[at] == ',') {
            ++at;
            blank();
            if (at >= text.size()) {
                return false;
            }
        }
    }
    if (parsed.empty()) {
        return false;
    }
    lengths = std::move(parsed);
    return true;
}

void MotionMeshRest(const MotionFilter& filter, int32_t row, int32_t column,
                    double& x, double& y) {
    x = filter.mesh_box_x +
        filter.mesh_box_width * column / std::max(1, filter.mesh_columns);
    y = filter.mesh_box_y +
        filter.mesh_box_height * row / std::max(1, filter.mesh_rows);
}

void ResolveMotionMeshes(std::vector<MotionFilter>& filters, double box_x,
                         double box_y, double box_width, double box_height) {
    const auto resolve = [](const MotionLength& length, double reference) {
        return length.unit == MotionLengthUnit::Percent
                   ? length.value / 100.0 * reference
                   : length.value;
    };
    for (MotionFilter& filter : filters) {
        if (filter.kind != MotionFilterKind::Mesh) {
            continue;
        }
        filter.mesh_box_x = box_x;
        filter.mesh_box_y = box_y;
        filter.mesh_box_width = box_width;
        filter.mesh_box_height = box_height;
        filter.mesh_resolved.clear();
        for (const auto& [x, y] : filter.mesh_points) {
            filter.mesh_resolved.emplace_back(box_x + resolve(x, box_width),
                                              box_y + resolve(y, box_height));
        }
    }
}

bool MotionDecimalRational(const std::string& text, int64_t& numerator,
                           int64_t& denominator) {
    size_t position = 0;
    bool negative = false;
    if (position < text.size() &&
        (text[position] == '-' || text[position] == '+')) {
        negative = text[position] == '-';
        ++position;
    }
    __int128 digits = 0;
    int64_t scale = 0;
    size_t count = 0;
    bool fraction = false;
    for (; position < text.size(); ++position) {
        const char c = text[position];
        if (c == '.' && !fraction) {
            fraction = true;
            continue;
        }
        if (c < '0' || c > '9') {
            break;
        }
        if (++count > 18) {
            return false;
        }
        digits = digits * 10 + (c - '0');
        scale -= fraction ? 1 : 0;
    }
    if (count == 0) {
        return false;
    }
    if (position < text.size() &&
        (text[position] == 'e' || text[position] == 'E')) {
        ++position;
        bool negativeExponent = false;
        if (position < text.size() &&
            (text[position] == '-' || text[position] == '+')) {
            negativeExponent = text[position] == '-';
            ++position;
        }
        int64_t exponent = 0;
        size_t exponentDigits = 0;
        for (; position < text.size() && text[position] >= '0' &&
               text[position] <= '9';
             ++position) {
            exponent = exponent * 10 + (text[position] - '0');
            if (++exponentDigits > 3) {
                return false;
            }
        }
        if (exponentDigits == 0) {
            return false;
        }
        scale += negativeExponent ? -exponent : exponent;
    }
    if (position != text.size() || scale < -18 || scale > 18) {
        return false;
    }
    __int128 top = digits;
    __int128 bottom = 1;
    for (int64_t step = 0; step < (scale < 0 ? -scale : scale); ++step) {
        (scale < 0 ? bottom : top) *= 10;
    }
    const __int128 limit = std::numeric_limits<int64_t>::max();
    if (top > limit || bottom > limit) {
        return false;
    }
    __int128 a = top;
    __int128 b = bottom;
    while (b != 0) {
        const __int128 r = a % b;
        a = b;
        b = r;
    }
    if (a > 1) {
        top /= a;
        bottom /= a;
    }
    numerator = static_cast<int64_t>(negative ? -top : top);
    denominator = static_cast<int64_t>(bottom);
    return true;
}

bool ParseMotionTimeScale(const Value& value, int64_t& numerator,
                          int64_t& denominator) {
    bool percent = false;
    std::string text;
    if (value.IsNumber()) {
        text = value.Dump();
    } else if (value.IsString() && value.AsString().size() > 1 &&
               value.AsString().back() == '%') {
        text = value.AsString().substr(0, value.AsString().size() - 1);
        percent = true;
    } else {
        return false;
    }
    int64_t top = 0;
    int64_t bottom = 1;
    if (!MotionDecimalRational(text, top, bottom) || top <= 0) {
        return false;
    }
    if (percent) {
        if (bottom > std::numeric_limits<int64_t>::max() / 100) {
            return false;
        }
        bottom *= 100;
        int64_t a = top;
        int64_t b = bottom;
        while (b != 0) {
            const int64_t r = a % b;
            a = b;
            b = r;
        }
        top /= a;
        bottom /= a;
    }
    // At most 1000: beyond that a frame skips seconds of animation.
    if (static_cast<__int128>(top) > static_cast<__int128>(bottom) * 1000) {
        return false;
    }
    numerator = top;
    denominator = bottom;
    return true;
}

RationalTime MotionFrameTime(const MotionComposition& composition,
                             int64_t frame) {
    return {frame * composition.fps_denominator, composition.fps_numerator};
}

namespace {

// Digits without sign or leading zero, at most 18 of them.
bool CanonicalDigits(const std::string& text, int64_t& out) {
    if (text.empty() || text.size() > 18 || text[0] == '0') {
        return false;
    }
    int64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        value = value * 10 + (c - '0');
    }
    out = value;
    return true;
}

}  // namespace

bool ParseMotionKeyFrame(const Value& value, int64_t& numerator,
                         int64_t& denominator) {
    if (value.IsNumber()) {
        if (!value.AsInt64(numerator)) {
            return false;
        }
        denominator = 1;
        return true;
    }
    if (!value.IsString()) {
        return false;
    }
    const std::string& text = value.AsString();
    const size_t slash = text.find('/');
    if (slash == std::string::npos) {
        return false;
    }
    const bool negative = !text.empty() && text[0] == '-';
    int64_t top = 0;
    int64_t bottom = 0;
    if (!CanonicalDigits(
            text.substr(negative ? 1 : 0, slash - (negative ? 1 : 0)), top) ||
        !CanonicalDigits(text.substr(slash + 1), bottom) || bottom < 2) {
        return false;
    }
    int64_t a = top;
    int64_t b = bottom;
    while (b != 0) {
        const int64_t r = a % b;
        a = b;
        b = r;
    }
    if (a != 1) {
        return false;
    }
    numerator = negative ? -top : top;
    denominator = bottom;
    return true;
}

bool MotionFrameTime(const MotionComposition& composition, int64_t numerator,
                     int64_t denominator, RationalTime& time) {
    if (denominator <= 0) {
        return false;
    }
    const __int128 value =
        static_cast<__int128>(numerator) * composition.fps_denominator;
    const __int128 rate =
        static_cast<__int128>(denominator) * composition.fps_numerator;
    if (value > std::numeric_limits<int64_t>::max() ||
        value < std::numeric_limits<int64_t>::min() ||
        rate > std::numeric_limits<int32_t>::max()) {
        return false;
    }
    time = {static_cast<int64_t>(value), static_cast<int32_t>(rate)};
    return true;
}

double EvaluateMotionNamedCurve(const MotionNamedCurve& curve, double p) {
    // Each family is written as GSAP 3.14 writes it (gsap-core.js), so the
    // same progress gives the same double up to the last bits of cos and pow.
    const auto easeIn = [&](double q) -> double {
        switch (curve.family) {
            case MotionNamedFamily::Power: {
                double value = 1.0;
                for (int index = 0; index < curve.exponent; ++index) {
                    value *= q;
                }
                return value;
            }
            case MotionNamedFamily::Sine:
                return q == 1.0 ? 1.0 : -std::cos(q * (M_PI / 2.0)) + 1.0;
            case MotionNamedFamily::Expo:
                // GSAP blends in q^6 (1 - q) so the curve lands exactly on 1.
                return std::pow(2.0, 10.0 * (q - 1.0)) * q +
                       q * q * q * q * q * q * (1.0 - q);
            case MotionNamedFamily::Circ:
                return -(std::sqrt(1.0 - q * q) - 1.0);
            case MotionNamedFamily::Back:
            case MotionNamedFamily::None:
                break;
        }
        return q;
    };
    if (curve.family == MotionNamedFamily::Back) {
        // GSAP defines back by its exit.
        const double s = curve.overshoot;
        const auto easeOut = [s](double q) {
            if (q == 0.0) {
                return 0.0;
            }
            q -= 1.0;
            return q * q * ((s + 1.0) * q + s) + 1.0;
        };
        switch (curve.direction) {
            case MotionNamedDirection::Out:
                return easeOut(p);
            case MotionNamedDirection::In:
                return 1.0 - easeOut(1.0 - p);
            case MotionNamedDirection::InOut:
                return p < 0.5 ? (1.0 - easeOut(1.0 - p * 2.0)) / 2.0
                               : 0.5 + easeOut((p - 0.5) * 2.0) / 2.0;
        }
    }
    if (curve.family == MotionNamedFamily::Power) {
        // Power names its exit and its in-out explicitly in GSAP.
        const auto raise = [&](double q) { return easeIn(q); };
        switch (curve.direction) {
            case MotionNamedDirection::In:
                return raise(p);
            case MotionNamedDirection::Out:
                return 1.0 - raise(1.0 - p);
            case MotionNamedDirection::InOut:
                return p < 0.5 ? raise(p * 2.0) / 2.0
                               : 1.0 - raise((1.0 - p) * 2.0) / 2.0;
        }
    }
    switch (curve.direction) {
        case MotionNamedDirection::In:
            return easeIn(p);
        case MotionNamedDirection::Out:
            return 1.0 - easeIn(1.0 - p);
        case MotionNamedDirection::InOut:
            return p < 0.5 ? easeIn(p * 2.0) / 2.0
                           : 1.0 - easeIn((1.0 - p) * 2.0) / 2.0;
    }
    return p;
}

namespace {

struct NamedApproximation {
    const char* key;
    KeyframeEasing easing;
    bool exact;
    double error;
};

// The closest cubic-bezier to each curve (largest gap over 401 samples of
// GSAP 3.14 itself; tools/motion-web-measure). power1 and power2 in and out
// are cubic polynomials, so their Bézier is the curve.
constexpr double kThird = 1.0 / 3.0;
const NamedApproximation kNamedApproximations[] = {
    {"power.in.2", {kThird, 0.0, 2.0 * kThird, kThird}, true, 0.0},
    {"power.out.2", {kThird, 2.0 * kThird, 2.0 * kThird, 1.0}, true, 0.0},
    {"power.inOut.2", {0.4859, 0.0426, 0.5117, 0.9545}, false, 0.0034},
    {"power.in.3", {kThird, 0.0, 2.0 * kThird, 0.0}, true, 0.0},
    {"power.out.3", {kThird, 1.0, 2.0 * kThird, 1.0}, true, 0.0},
    {"power.inOut.3", {0.643, -0.0202, 0.3572, 1.0206}, false, 0.0054},
    {"power.in.4", {0.4555, 0.0004, 0.7363, -0.0472}, false, 0.0006},
    {"power.out.4", {0.268, 1.0662, 0.5581, 0.994}, false, 0.0005},
    {"power.inOut.4", {0.7099, -0.0848, 0.2847, 1.0811}, false, 0.0075},
    {"power.in.5", {0.5512, 0.0006, 0.7805, -0.0774}, false, 0.0015},
    {"power.out.5", {0.2407, 1.2787, 0.6651, 0.945}, false, 0.0069},
    {"power.inOut.5", {0.7736, -0.1236, 0.2261, 1.1233}, false, 0.0088},
    {"sine.in", {0.3579, 0.001, 0.6638, 0.4709}, false, 0.0002},
    {"sine.out", {0.4355, 0.6945, 0.6892, 0.9918}, false, 0.0009},
    {"sine.inOut", {0.3628, -0.0016, 0.6372, 1.0016}, false, 0.0001},
    {"expo.in", {0.6146, 0.0039, 0.8435, -0.0701}, false, 0.0011},
    {"expo.out", {0.1577, 1.0934, 0.4065, 0.9899}, false, 0.0007},
    {"expo.inOut", {0.838, -0.1285, 0.162, 1.1285}, false, 0.0099},
    {"circ.in", {0.5683, 0.0025, 1.0, 0.4614}, false, 0.0002},
    {"circ.out", {0.0047, 0.6779, 0.616, 1.0237}, false, 0.0035},
    {"circ.inOut", {0.8337, -0.0247, 0.1662, 1.0246}, false, 0.0407},
    // back.inOut at the default overshoot; another overshoot scales it.
    {"back.inOut", {0.7205, -0.334, 0.2795, 1.3338}, false, 0.0144},
};

}  // namespace

bool ParseMotionNamedCurve(const std::string& text, MotionNamedCurve& curve,
                           KeyframeEasing& approximation) {
    std::string body = text;
    double overshoot = 1.70158;
    bool configured = false;
    if (const size_t open = body.find('(');
        open != std::string::npos && body.back() == ')') {
        int64_t numerator = 0;
        int64_t denominator = 1;
        const std::string argument =
            body.substr(open + 1, body.size() - open - 2);
        if (!MotionDecimalRational(argument, numerator, denominator) ||
            numerator < 0 || numerator > 10 * denominator) {
            return false;
        }
        overshoot =
            static_cast<double>(numerator) / static_cast<double>(denominator);
        body = body.substr(0, open);
        configured = true;
    }
    const size_t dot = body.find('.');
    const std::string family = body.substr(0, dot);
    const std::string direction =
        dot == std::string::npos ? "out" : body.substr(dot + 1);
    MotionNamedCurve parsed;
    parsed.name = text;
    if (direction == "in") {
        parsed.direction = MotionNamedDirection::In;
    } else if (direction == "out") {
        parsed.direction = MotionNamedDirection::Out;
    } else if (direction == "inOut") {
        parsed.direction = MotionNamedDirection::InOut;
    } else {
        return false;
    }
    // GSAP's aliases: Power1 is Quad (squared), Power4 Quint and Strong.
    const std::pair<const char*, int> powers[] = {
        {"power1", 2}, {"quad", 2},   {"power2", 3},
        {"cubic", 3},  {"power3", 4}, {"quart", 4},
        {"power4", 5}, {"quint", 5},  {"strong", 5}};
    for (const auto& [name, exponent] : powers) {
        if (family == name) {
            parsed.family = MotionNamedFamily::Power;
            parsed.exponent = exponent;
        }
    }
    if (family == "sine") {
        parsed.family = MotionNamedFamily::Sine;
    } else if (family == "expo") {
        parsed.family = MotionNamedFamily::Expo;
    } else if (family == "circ") {
        parsed.family = MotionNamedFamily::Circ;
    } else if (family == "back") {
        parsed.family = MotionNamedFamily::Back;
        parsed.overshoot = overshoot;
    }
    if (parsed.family == MotionNamedFamily::None ||
        (configured && parsed.family != MotionNamedFamily::Back)) {
        return false;
    }
    if (parsed.family == MotionNamedFamily::Back &&
        parsed.direction != MotionNamedDirection::InOut) {
        // back.in(s) is s-weighted cubic p^2((s + 1)p - s): with its x
        // controls at thirds, a cubic-bezier is exactly that polynomial.
        const double s = parsed.overshoot;
        approximation =
            parsed.direction == MotionNamedDirection::In
                ? KeyframeEasing{kThird, 0.0, 2.0 * kThird, -s / 3.0}
                : KeyframeEasing{kThird, 1.0 + s / 3.0, 2.0 * kThird, 1.0};
        parsed.exact_bezier = true;
        curve = parsed;
        return true;
    }
    const std::string key =
        (parsed.family == MotionNamedFamily::Power ? std::string("power")
                                                   : family) +
        "." + direction +
        (parsed.family == MotionNamedFamily::Power
             ? "." + std::to_string(parsed.exponent)
             : std::string());
    for (const NamedApproximation& entry : kNamedApproximations) {
        if (key == entry.key) {
            approximation = entry.easing;
            parsed.exact_bezier = entry.exact;
            parsed.bezier_error = entry.error;
        }
    }
    if (parsed.family == MotionNamedFamily::Back) {
        // Scaled from the default overshoot: an approximation, said so.
        const double scale = parsed.overshoot / 1.70158;
        approximation.y1 *= scale;
        approximation.y2 = 1.0 + (approximation.y2 - 1.0) * scale;
    }
    curve = parsed;
    return true;
}

KeyframeEvaluation EvaluateMotionTrack(const MotionAnimation& animation,
                                       RationalTime time) {
    KeyframeEvaluation evaluation = animation.track.Evaluate(
        time, animation.extrapolate_left, animation.extrapolate_right);
    if (!evaluation || animation.named_curves.empty()) {
        return evaluation;
    }
    const std::vector<Keyframe>& keys = animation.track.keyframes();
    for (size_t index = 0;
         index + 1 < keys.size() && index < animation.named_curves.size();
         ++index) {
        const MotionNamedCurve& curve = animation.named_curves[index];
        if (curve.family == MotionNamedFamily::None ||
            keys[index].interpolation != KeyframeInterpolation::CubicBezier ||
            time.compare(keys[index].time) <= 0 ||
            time.compare(keys[index + 1].time) >= 0) {
            continue;
        }
        // Progress through the segment. Not by RationalTime::sub: two keys
        // between frames ("63750/817", "74129/850") have rates whose common
        // multiple overflows int32_t, and sub throws.
        const auto seconds = [](const RationalTime& at) {
            return static_cast<long double>(at.value) / at.rate;
        };
        const long double start = seconds(keys[index].time);
        const long double progress =
            (seconds(time) - start) / (seconds(keys[index + 1].time) - start);
        const double eased =
            EvaluateMotionNamedCurve(curve, static_cast<double>(progress));
        evaluation.value = keys[index].value +
                           (keys[index + 1].value - keys[index].value) * eased;
        break;
    }
    return evaluation;
}

bool ParseMotionComposition(const std::string& json,
                            MotionComposition& composition,
                            MotionError& error) {
    Value root;
    std::string message;
    if (!Value::Parse(json, root, message)) {
        return Fail(error, "InvalidJson", "composition", message);
    }
    return ParseMotionComposition(root, composition, error);
}

bool ParseMotionComposition(const Value& root, MotionComposition& composition,
                            MotionError& error) {
    MotionComposition parsed;
    Parser parser(parsed, error);
    if (!parser.Composition(root)) {
        return false;
    }
    composition = std::move(parsed);
    return true;
}

bool IsMotionSetName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [](char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-' ||
               character == '_';
    });
}

namespace {

template <typename Set>
const Set* FindNamed(const std::vector<Set>& sets, const std::string& name,
                     const char* code, const char* kind, const char* plural,
                     MotionError& error) {
    for (const Set& set : sets) {
        if (set.name == name) {
            return &set;
        }
    }
    std::string declared;
    for (const Set& set : sets) {
        declared += (declared.empty() ? "" : ", ") + set.name;
    }
    Fail(error, code, kind,
         "the composition declares no " + std::string(kind) + " '" + name +
             "'" +
             (declared.empty() ? std::string(" (it declares no ") + plural + ")"
                               : " (declared: " + declared + ")"));
    return nullptr;
}

}  // namespace

bool ResolveMotionVariant(const MotionComposition& composition,
                          const std::string& variant, const std::string& format,
                          const Value& overrides, MotionComposition& resolved,
                          Value& props, MotionError& error) {
    const MotionVariant* version = nullptr;
    const MotionFormat* size = nullptr;
    if (!variant.empty()) {
        version = FindNamed(composition.variants, variant, "UnknownVariant",
                            "variant", "variants", error);
        if (version == nullptr) {
            return false;
        }
    }
    if (!format.empty()) {
        size = FindNamed(composition.formats, format, "UnknownFormat", "format",
                         "formats", error);
        if (size == nullptr) {
            return false;
        }
    }
    if (!overrides.IsNull() && !overrides.IsObject()) {
        return Fail(error, "InvalidValue", "props",
                    "props must be an object of overrides");
    }
    Value merged = Value::MakeObject();
    for (const Value* source :
         {size != nullptr ? &size->props : nullptr,
          version != nullptr ? &version->props : nullptr, &overrides}) {
        if (source == nullptr || !source->IsObject()) {
            continue;
        }
        for (const auto& [name, value] : source->AsObject()) {
            merged.Set(name, value);
        }
    }
    resolved = composition;
    if (size != nullptr) {
        resolved.width = size->width;
        resolved.height = size->height;
    }
    props = std::move(merged);
    return true;
}
