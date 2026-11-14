#include "MotionHumanize.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cmath>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace {

using mcp_json::Value;

std::string TypeOf(const Value& node) {
    const Value* type = node.Find("type");
    return type != nullptr && type->IsString() ? type->AsString() : "";
}

const Value* Children(const Value& node) {
    const Value* children = node.Find("children");
    return children != nullptr && children->IsArray() ? children : nullptr;
}

int64_t CountNodes(const Value& node) {
    int64_t count = 1;
    if (const Value* children = Children(node)) {
        for (const Value& child : children->AsArray()) {
            count += CountNodes(child);
        }
    }
    return count;
}

std::string ChildPath(const std::string& path, size_t index) {
    return path + ".children[" + std::to_string(index) + "]";
}

// True when every key of `object` (absent counts as empty) is in `allowed`.
bool KeysWithin(const Value* object,
                std::initializer_list<const char*> allowed) {
    if (object == nullptr) {
        return true;
    }
    if (!object->IsObject()) {
        return false;
    }
    for (const auto& entry : object->AsObject()) {
        if (std::none_of(allowed.begin(), allowed.end(),
                         [&](const char* key) { return entry.first == key; })) {
            return false;
        }
    }
    return true;
}

bool NumberAt(const Value* object, const char* key, double& number) {
    const Value* value = object != nullptr ? object->Find(key) : nullptr;
    return value != nullptr && MotionNumber(*value, number);
}

std::string StringAt(const Value* object, const char* key) {
    const Value* value = object != nullptr ? object->Find(key) : nullptr;
    return value != nullptr && value->IsString() ? value->AsString() : "";
}

// The easing of an interpolation as it is written, a name or (MB5) a list
// of one curve per segment: two animations share their curves only when
// this is the same.
std::string EasingText(const Value* interpolate) {
    const Value* easing =
        interpolate != nullptr ? interpolate->Find("easing") : nullptr;
    return easing != nullptr ? easing->Dump() : std::string();
}

// A number spelled as short as it reads back to the same double: the
// spacing of a timecode must put each digit exactly where its column was.
Value Exact(double value) {
    char buffer[64];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return Value::MakeRawNumber(std::string(buffer, written.ptr));
}

bool Empty(const Value* object) {
    return object == nullptr ||
           (object->IsObject() && object->AsObject().empty());
}

// `object` without `keys`.
Value Without(const Value& object, std::initializer_list<const char*> keys) {
    Value out = Value::MakeObject();
    for (const auto& entry : object.AsObject()) {
        if (std::none_of(keys.begin(), keys.end(),
                         [&](const char* key) { return entry.first == key; })) {
            out.Set(entry.first, entry.second);
        }
    }
    return out;
}

Value WithChildren(const Value& node, std::vector<Value> children) {
    Value list = Value::MakeArray();
    for (Value& child : children) {
        list.Push(std::move(child));
    }
    Value out = node;
    out.Set("children", std::move(list));
    return out;
}

// One UTF-8 character, not a space.
bool OneCharacter(const std::string& text) {
    if (text.empty() || text == " ") {
        return false;
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    const size_t length = lead < 0x80   ? 1
                          : lead < 0xE0 ? 2
                          : lead < 0xF0 ? 3
                                        : 4;
    return text.size() == length;
}

// Where a node's local time sits in the composition, when that is known:
// sequences shift it by `from`; a series, a repeat or an instance places
// its children by rules the pass does not reproduce.
struct Place {
    bool located = true;
    int64_t start = 0;
    int64_t length = 0;
};

Place Inside(const Value& node, const Place& outer) {
    Place place = outer;
    const std::string type = TypeOf(node);
    if (type == "sequence") {
        int64_t from = 0;
        int64_t duration = 0;
        const Value* fromValue = node.Find("from");
        const Value* durationValue = node.Find("duration");
        if ((fromValue != nullptr && !fromValue->AsInt64(from)) ||
            (durationValue != nullptr && !durationValue->AsInt64(duration))) {
            place.located = false;
            return place;
        }
        place.start = outer.start + from;
        place.length = durationValue != nullptr
                           ? duration
                           : std::max<int64_t>(0, outer.length - from);
    } else if (type == "series" || type == "repeat" || type == "composition") {
        place.located = false;
    }
    return place;
}

// The frames of a composition evaluated once each, for the style a text
// resolves to and where it lands.
class Frames {
public:
    Frames(const Value& definition, MotionMeasurer& measurer,
           const MotionEvaluationContext& context)
        : definition_(definition), measurer_(measurer), context_(context) {}

    bool Prepare(MotionError& error) {
        return ParseMotionComposition(definition_, composition_, error);
    }

    const MotionComposition& composition() const { return composition_; }

    const MotionEvaluatedNode* At(int64_t frame, const std::string& path,
                                  MotionError& error) {
        frame = std::clamp<int64_t>(
            frame, 0, std::max<int64_t>(0, composition_.duration - 1));
        auto& slot = frames_[frame];
        if (!slot) {
            slot = std::make_unique<Indexed>();
            if (!EvaluateMotionFrame(composition_, Value(), frame, measurer_,
                                     context_, slot->frame, error)) {
                slot.reset();
                return nullptr;
            }
            Index(slot->frame.root, *slot);
        }
        const auto found = slot->by_path.find(path);
        return found == slot->by_path.end() ? nullptr : found->second;
    }

private:
    struct Indexed {
        MotionFrame frame;
        std::map<std::string, const MotionEvaluatedNode*> by_path;
    };

    static void Index(const MotionEvaluatedNode& node, Indexed& indexed) {
        indexed.by_path.emplace(node.path, &node);
        for (const MotionEvaluatedNode& child : node.children) {
            Index(child, indexed);
        }
    }

    const Value& definition_;
    MotionMeasurer& measurer_;
    const MotionEvaluationContext& context_;
    MotionComposition composition_;
    std::map<int64_t, std::unique_ptr<Indexed>> frames_;
};

// The advance of one character in `style`, spacing aside.
bool Advance(MotionMeasurer& measurer, MotionTextStyle style,
             const std::string& character, double& advance) {
    style.letter_spacing = 0.0;
    MotionTextLayout layout;
    MotionError ignored;
    if (!measurer.LayoutText(character, style, -1.0, layout, ignored) ||
        layout.lines.size() != 1 || layout.lines[0].clusters.size() != 1) {
        return false;
    }
    advance = layout.lines[0].clusters[0].width;
    return true;
}

// --- timecode --------------------------------------------------------------

struct Column {
    // A static character, or a strip of the ten digits.
    std::string character;
    const Value* keys = nullptr;
    // The path of one of its texts, for the style it resolves to.
    std::string text_path;
};

// The digit a strip shows at local frame `frame`: its translateY held at
// the last key at or before it, a whole number of rows up.
bool DigitAt(const Value& keys, int64_t frame, double height, char& digit) {
    const Value* frames = keys.Find("frames");
    const Value* values = keys.Find("values");
    const std::vector<Value>& at = frames->AsArray();
    size_t index = 0;
    for (size_t k = 0; k < at.size(); ++k) {
        int64_t key = 0;
        if (!at[k].AsInt64(key)) {
            return false;
        }
        if (key <= frame) {
            index = k;
        }
    }
    double value = 0.0;
    if (!MotionNumber(values->AsArray()[index], value)) {
        return false;
    }
    const double rows = -value / height;
    if (rows != std::floor(rows) || rows < 0.0 || rows > 9.0 ||
        -rows * height != value) {
        return false;
    }
    digit = static_cast<char>('0' + static_cast<int>(rows));
    return true;
}

bool ReadTimecode(const std::string& text, int64_t nominal, int64_t& frames) {
    if (text.size() != 11 || text[2] != ':' || text[5] != ':' ||
        text[8] != ':') {
        return false;
    }
    int64_t parts[4] = {0, 0, 0, 0};
    for (int part = 0; part < 4; ++part) {
        const size_t at = static_cast<size_t>(part) * 3;
        const char high = text[at];
        const char low = text[at + 1];
        if (high < '0' || high > '9' || low < '0' || low > '9') {
            return false;
        }
        parts[part] = (high - '0') * 10 + (low - '0');
    }
    if (parts[3] >= nominal) {
        return false;
    }
    frames =
        ((parts[0] * 3600 + parts[1] * 60 + parts[2]) * nominal) + parts[3];
    return true;
}

// The columns of a row of digit strips, or false when `node` is not one.
bool TimecodeColumns(const Value& node, const std::string& path,
                     std::vector<Column>& columns, double& width,
                     double& height, std::string& textStyle) {
    const Value* children = Children(node);
    if (TypeOf(node) != "box" || children == nullptr ||
        children->AsArray().size() < 2 || !Empty(node.Find("animate")) ||
        !KeysWithin(node.Find("style"),
                    {"position", "left", "top", "right", "bottom", "width",
                     "height", "flexShrink", "alignSelf", "marginTop",
                     "marginRight", "marginBottom", "marginLeft", "margin"})) {
        return false;
    }
    bool strips = false;
    const auto sameText = [&](const Value& text) {
        const std::string style =
            text.Find("style") != nullptr ? text.Find("style")->Dump() : "{}";
        if (textStyle.empty()) {
            textStyle = style;
        }
        return TypeOf(text) == "text" && style == textStyle &&
               Empty(text.Find("animate")) && text.Find("units") == nullptr &&
               text.Find("id") == nullptr;
    };
    for (size_t index = 0; index < children->AsArray().size(); ++index) {
        const Value& column = children->AsArray()[index];
        const Value* style = column.Find("style");
        double w = 0.0;
        double h = 0.0;
        const Value* inner = Children(column);
        if (TypeOf(column) != "box" || !Empty(column.Find("animate")) ||
            column.Find("id") != nullptr || !NumberAt(style, "width", w) ||
            !NumberAt(style, "height", h) || h <= 0.0 || inner == nullptr ||
            inner->AsArray().size() != 1 ||
            !KeysWithin(style, {"width", "height", "flexShrink", "overflow",
                                "flexDirection"})) {
            return false;
        }
        if (index == 0) {
            width = w;
            height = h;
        } else if (w != width || h != height) {
            return false;
        }
        const Value& only = inner->AsArray()[0];
        Column read;
        if (TypeOf(only) == "text") {
            const Value* content = only.Find("content");
            if (!sameText(only) || content == nullptr || !content->IsString() ||
                !OneCharacter(content->AsString())) {
                return false;
            }
            read.character = content->AsString();
            read.text_path = ChildPath(ChildPath(path, index), 0);
            columns.push_back(read);
            continue;
        }
        // A strip: a column of the ten digits in a window one digit tall.
        const Value* stripStyle = only.Find("style");
        const Value* digits = Children(only);
        const Value* animate = only.Find("animate");
        const Value* translate =
            animate != nullptr ? animate->Find("translateY") : nullptr;
        const Value* keys =
            translate != nullptr ? translate->Find("interpolate") : nullptr;
        double stripHeight = 0.0;
        if (StringAt(style, "overflow") != "hidden" || TypeOf(only) != "box" ||
            only.Find("id") != nullptr || digits == nullptr ||
            digits->AsArray().size() != 10 || keys == nullptr ||
            animate->AsObject().size() != 1 ||
            !KeysWithin(translate, {"interpolate"}) ||
            !KeysWithin(keys, {"frames", "values", "easing", "uids"}) ||
            StringAt(keys, "easing") != "step-end" ||
            keys->Find("frames") == nullptr ||
            !keys->Find("frames")->IsArray() ||
            keys->Find("values") == nullptr ||
            !keys->Find("values")->IsArray() ||
            keys->Find("frames")->AsArray().empty() ||
            keys->Find("frames")->AsArray().size() !=
                keys->Find("values")->AsArray().size() ||
            !KeysWithin(stripStyle, {"flexDirection", "flexShrink", "height",
                                     "alignSelf"}) ||
            StringAt(stripStyle, "flexDirection") != "column" ||
            (NumberAt(stripStyle, "height", stripHeight) &&
             stripHeight != 10.0 * height)) {
            return false;
        }
        for (size_t digit = 0; digit < 10; ++digit) {
            const Value& row = digits->AsArray()[digit];
            const Value* rowChildren = Children(row);
            double rowHeight = 0.0;
            if (TypeOf(row) != "box" || !Empty(row.Find("animate")) ||
                !KeysWithin(row.Find("style"), {"height", "flexShrink"}) ||
                !NumberAt(row.Find("style"), "height", rowHeight) ||
                rowHeight != height || rowChildren == nullptr ||
                rowChildren->AsArray().size() != 1 ||
                !sameText(rowChildren->AsArray()[0])) {
                return false;
            }
            const Value* content = rowChildren->AsArray()[0].Find("content");
            if (content == nullptr || !content->IsString() ||
                content->AsString() != std::string(1, char('0' + digit))) {
                return false;
            }
        }
        read.keys = keys;
        read.text_path =
            ChildPath(ChildPath(ChildPath(ChildPath(path, index), 0), 0), 0);
        columns.push_back(read);
        strips = true;
    }
    return strips;
}

// --- the pass --------------------------------------------------------------

class Humanizer {
public:
    Humanizer(MotionMeasurer& measurer, const MotionEvaluationContext& context,
              MotionHumanizeReport& report, MotionError& error)
        : measurer_(measurer),
          context_(context),
          report_(report),
          error_(error) {}

    bool Run(const Value& definition, Value& output) {
        const Value* root = definition.Find("root");
        if (root == nullptr) {
            error_ = {"InvalidValue", "root", "a composition has a root"};
            return false;
        }
        report_.nodes_before = CountNodes(*root);
        // Timecodes and wrappers first, on the definition as written; then
        // the letters, on what that leaves, evaluated anew.
        Frames written(definition, measurer_, context_);
        if (!written.Prepare(error_)) {
            return false;
        }
        Place top;
        top.length = written.composition().duration;
        fps_numerator_ = written.composition().fps_numerator;
        fps_denominator_ = written.composition().fps_denominator;
        Value first = definition;
        Value rewritten;
        if (!Structure(*root, "root", top, written, rewritten)) {
            return false;
        }
        first.Set("root", rewritten);

        Frames folded(first, measurer_, context_);
        if (!folded.Prepare(error_)) {
            return false;
        }
        Value lettered;
        if (!Letters(rewritten, "root", top, folded, lettered)) {
            return false;
        }
        output = first;
        output.Set("root", lettered);
        report_.nodes_after = CountNodes(lettered);
        return true;
    }

private:
    // Timecode rows, then boxes that only place one text, bottom up.
    bool Structure(const Value& node, const std::string& path,
                   const Place& place, Frames& frames, Value& out) {
        const Place inside = Inside(node, place);
        if (inside.located) {
            Value row;
            bool folded = false;
            if (!Timecode(node, path, inside, frames, row, folded)) {
                return false;
            }
            if (folded) {
                out = row;
                return true;
            }
        }
        const Value* children = Children(node);
        if (children == nullptr) {
            out = node;
            return true;
        }
        std::vector<Value> rewritten;
        for (size_t index = 0; index < children->AsArray().size(); ++index) {
            Value child;
            if (!Structure(children->AsArray()[index], ChildPath(path, index),
                           inside, frames, child)) {
                return false;
            }
            rewritten.push_back(Unwrapped(child));
        }
        out = WithChildren(node, std::move(rewritten));
        return true;
    }

    bool Timecode(const Value& node, const std::string& path,
                  const Place& place, Frames& frames, Value& out,
                  bool& folded) {
        std::vector<Column> columns;
        double width = 0.0;
        double height = 0.0;
        std::string textStyle;
        if (!TimecodeColumns(node, path, columns, width, height, textStyle)) {
            return true;
        }
        const auto note = [&](const std::string& why) {
            report_.notes.push_back(path + ": " + why);
            return true;
        };
        // The style its texts resolve to, where the row is first shown.
        const MotionEvaluatedNode* text =
            frames.At(place.start, columns.front().text_path, error_);
        if (text == nullptr) {
            return error_.code.empty()
                       ? note("rangée de chiffres jamais montrée")
                       : false;
        }
        // One advance for every character it shows: the columns are that
        // advance plus a spacing, which the text then carries.
        double advance = -1.0;
        std::vector<std::string> shown;
        for (const Column& column : columns) {
            if (column.keys == nullptr) {
                shown.push_back(column.character);
            }
        }
        for (char digit = '0'; digit <= '9'; ++digit) {
            shown.emplace_back(1, digit);
        }
        for (const std::string& character : shown) {
            double measured = 0.0;
            if (!Advance(measurer_, text->text_style, character, measured) ||
                (advance >= 0.0 && measured != advance)) {
                return note("rangée de chiffres d'une police non monospace");
            }
            advance = measured;
        }
        // What the row reads at each frame it is shown, against the
        // timecode read at its first.
        const auto read = [&](int64_t frame, std::string& textRead) {
            textRead.clear();
            for (const Column& column : columns) {
                if (column.keys == nullptr) {
                    textRead += column.character;
                    continue;
                }
                char digit = 0;
                if (!DigitAt(*column.keys, frame, height, digit)) {
                    return false;
                }
                textRead.push_back(digit);
            }
            return true;
        };
        const int64_t nominal =
            (fps_numerator_ + fps_denominator_ - 1) / fps_denominator_;
        std::string atFirst;
        int64_t shownFirst = 0;
        if (!read(0, atFirst) || !ReadTimecode(atFirst, nominal, shownFirst)) {
            return note("rangée de chiffres qui ne lit pas un timecode");
        }
        std::string scope = "local";
        int64_t base = 0;
        if (place.located && shownFirst >= place.start) {
            scope = "composition";
            base = place.start;
        }
        const int64_t offset = shownFirst - base;
        const int64_t length =
            place.length > 0 ? place.length
                             : frames.composition().duration - place.start;
        for (int64_t frame = 0; frame < length; ++frame) {
            std::string atFrame;
            if (!read(frame, atFrame) ||
                atFrame != MotionTimecodeText(base + frame + offset,
                                              fps_numerator_,
                                              fps_denominator_)) {
                return note(
                    "rangée de chiffres qui quitte le timecode à "
                    "l'image " +
                    std::to_string(frame));
            }
        }
        Value binding = Value::MakeObject();
        binding.Set("timecode", Value::MakeString(scope));
        if (offset != 0) {
            binding.Set("offset", Value::MakeInt(offset));
        }
        Value style;
        std::string ignored;
        Value::Parse(textStyle, style, ignored);
        if (!style.IsObject()) {
            style = Value::MakeObject();
        }
        const double spacing = width - advance;
        if (spacing != 0.0) {
            style.Set("letterSpacing", Exact(spacing));
        }
        Value timecode = Value::MakeObject();
        timecode.Set("type", Value::MakeString("text"));
        timecode.Set("style", style);
        timecode.Set("content", binding);
        out = WithChildren(node, {timecode});
        folded = true;
        ++report_.timecodes;
        return true;
    }

    // A box that only places one text, folded into it.
    Value Unwrapped(const Value& box) {
        const Value* children = Children(box);
        const Value* style = box.Find("style");
        if (TypeOf(box) != "box" || children == nullptr ||
            children->AsArray().size() != 1 ||
            !KeysWithin(
                &box, {"type", "style", "children", "animate", "uid", "id"}) ||
            StringAt(style, "position") != "absolute" ||
            !KeysWithin(style, {"position", "left", "top"})) {
            return box;
        }
        const Value& text = children->AsArray()[0];
        const Value* textStyle = text.Find("style");
        const bool boxAnimated = !Empty(box.Find("animate"));
        if (TypeOf(text) != "text" ||
            !KeysWithin(&text, {"type", "style", "content", "animate", "uid",
                                "id", "units", "stagger", "ranges"}) ||
            (box.Find("id") != nullptr && text.Find("id") != nullptr) ||
            (boxAnimated &&
             (!Empty(text.Find("animate")) || text.Find("units") != nullptr))) {
            return box;
        }
        if (textStyle != nullptr) {
            static const char* kPlaced[] = {
                "position",    "left",         "top",        "right",
                "bottom",      "inset",        "margin",     "marginTop",
                "marginRight", "marginBottom", "marginLeft", "alignSelf",
                "flexGrow",    "flexShrink",   "flexBasis",  "width",
                "height"};
            for (const char* key : kPlaced) {
                if (textStyle->Find(key) != nullptr) {
                    return box;
                }
            }
        }
        Value folded = text;
        Value placed = textStyle != nullptr && textStyle->IsObject()
                           ? *textStyle
                           : Value::MakeObject();
        for (const auto& entry : style->AsObject()) {
            placed.Set(entry.first, entry.second);
        }
        folded.Set("style", placed);
        if (boxAnimated) {
            folded.Set("animate", *box.Find("animate"));
        }
        if (const Value* id = box.Find("id")) {
            folded.Set("id", *id);
        }
        ++report_.wrappers;
        return folded;
    }

    // --- letters -------------------------------------------------------------

    struct Letter {
        const Value* node = nullptr;
        std::string character;
        double left = 0.0;
        double top = 0.0;
        std::string rest;  // its style without `left`, spelled
        const Value* animate = nullptr;
    };

    static bool AsLetter(const Value& node, Letter& letter) {
        const Value* style = node.Find("style");
        const Value* content = node.Find("content");
        if (TypeOf(node) != "text" ||
            !KeysWithin(&node,
                        {"type", "style", "content", "animate", "uid"}) ||
            content == nullptr || !content->IsString() ||
            !OneCharacter(content->AsString()) ||
            StringAt(style, "position") != "absolute" ||
            !NumberAt(style, "left", letter.left) ||
            !NumberAt(style, "top", letter.top)) {
            return false;
        }
        letter.node = &node;
        letter.character = content->AsString();
        letter.rest = Without(*style, {"left"}).Dump();
        letter.animate =
            Empty(node.Find("animate")) ? nullptr : node.Find("animate");
        return true;
    }

    // How much later `b`'s animations run than `a`'s, when they are the
    // same otherwise: the same unit properties, interpolated through the
    // same values and curve.
    static bool Shift(const Value* a, const Value* b, int64_t& shift) {
        if ((a == nullptr) != (b == nullptr)) {
            return false;
        }
        shift = 0;
        if (a == nullptr) {
            return true;
        }
        if (a->AsObject().size() != b->AsObject().size()) {
            return false;
        }
        bool first = true;
        for (const auto& entry : a->AsObject()) {
            const Value* other = b->Find(entry.first);
            const Value* keysA = entry.second.Find("interpolate");
            const Value* keysB =
                other != nullptr ? other->Find("interpolate") : nullptr;
            if (!IsMotionUnitProperty(entry.first) || keysA == nullptr ||
                keysB == nullptr ||
                !KeysWithin(&entry.second, {"interpolate"}) ||
                !KeysWithin(other, {"interpolate"}) ||
                !KeysWithin(keysA, {"frames", "values", "easing", "uids"}) ||
                !KeysWithin(keysB, {"frames", "values", "easing", "uids"}) ||
                keysA->Find("frames") == nullptr ||
                keysB->Find("frames") == nullptr ||
                keysA->Find("values") == nullptr ||
                keysB->Find("values") == nullptr ||
                keysA->Find("values")->Dump() !=
                    keysB->Find("values")->Dump() ||
                EasingText(keysA) != EasingText(keysB)) {
                return false;
            }
            const std::vector<Value>& fa = keysA->Find("frames")->AsArray();
            const std::vector<Value>& fb = keysB->Find("frames")->AsArray();
            if (fa.size() != fb.size() || fa.empty()) {
                return false;
            }
            for (size_t k = 0; k < fa.size(); ++k) {
                int64_t x = 0;
                int64_t y = 0;
                if (!fa[k].AsInt64(x) || !fb[k].AsInt64(y)) {
                    return false;
                }
                if (first) {
                    shift = y - x;
                    first = false;
                } else if (y - x != shift) {
                    return false;
                }
            }
        }
        return true;
    }

    bool Letters(const Value& node, const std::string& path, const Place& place,
                 Frames& frames, Value& out) {
        const Place inside = Inside(node, place);
        const Value* children = Children(node);
        if (children == nullptr) {
            out = node;
            return true;
        }
        std::vector<Value> kept;
        const std::vector<Value>& list = children->AsArray();
        size_t index = 0;
        int64_t apart = 0;
        double worst = 0.0;
        while (index < list.size()) {
            size_t used = 0;
            Value word;
            if (inside.located &&
                !Word(list, index, path, inside, frames, word, used, worst)) {
                return false;
            }
            if (used >= 2) {
                kept.push_back(std::move(word));
                index += used;
                continue;
            }
            Letter alone;
            if (worst > 0.0 && AsLetter(list[index], alone)) {
                ++apart;
            }
            Value child;
            if (!Letters(list[index], ChildPath(path, index), inside, frames,
                         child)) {
                return false;
            }
            kept.push_back(std::move(child));
            ++index;
        }
        if (apart > 0) {
            char distance[32];
            std::snprintf(distance, sizeof(distance), "%.3f", worst);
            report_.notes.push_back(
                path + ": " + std::to_string(apart) +
                " lettre(s) placée(s) par d'autres chasses que celles du "
                "moteur (jusqu'à " +
                distance + " px), laissée(s) à part");
        }
        out = WithChildren(node, std::move(kept));
        return true;
    }

    // The longest run of letters from `start` that makes one text, and that
    // text; `used` stays below 2 when there is none.
    bool Word(const std::vector<Value>& list, size_t start,
              const std::string& path, const Place& place, Frames& frames,
              Value& word, size_t& used, double& worst) {
        used = 0;
        std::vector<Letter> letters;
        int64_t each = 0;
        for (size_t index = start; index < list.size(); ++index) {
            Letter letter;
            if (!AsLetter(list[index], letter)) {
                break;
            }
            int64_t shift = 0;
            if (!letters.empty() &&
                (letter.rest != letters.front().rest ||
                 letter.top != letters.front().top ||
                 !Shift(letters.front().animate, letter.animate, shift))) {
                break;
            }
            if (letters.size() == 1) {
                each = shift;
            } else if (letters.size() > 1 &&
                       shift != each * static_cast<int64_t>(letters.size())) {
                break;
            }
            letters.push_back(letter);
        }
        if (letters.size() < 2) {
            return true;
        }
        // Where the engine lays the word out: each letter must start where
        // the text puts it, a space allowed between two.
        const MotionEvaluatedNode* first =
            frames.At(place.start, ChildPath(path, start), error_);
        if (first == nullptr) {
            return error_.code.empty();
        }
        std::string content = letters.front().character;
        size_t fused = 1;
        for (; fused < letters.size(); ++fused) {
            const Letter& letter = letters[fused];
            const double wanted = letter.left - letters.front().left;
            bool placed = false;
            double nearest = -1.0;
            for (const char* between : {"", " "}) {
                const std::string candidate =
                    content + between + letter.character;
                MotionTextLayout layout;
                MotionError ignored;
                if (!measurer_.LayoutText(candidate, first->text_style, -1.0,
                                          layout, ignored) ||
                    layout.lines.size() != 1 ||
                    layout.lines[0].clusters.empty()) {
                    continue;
                }
                const MotionTextLine& line = layout.lines[0];
                const double x = line.x + line.clusters.back().x;
                if (std::fabs(x - wanted) <= 1e-6) {
                    content = candidate;
                    placed = true;
                    break;
                }
                if (nearest < 0.0 || std::fabs(x - wanted) < nearest) {
                    nearest = std::fabs(x - wanted);
                }
            }
            if (!placed) {
                worst = std::max(worst, nearest);
                break;
            }
        }
        if (fused < 2) {
            return true;
        }
        letters.resize(fused);
        // The delay per rank: forward from the first letter, or reverse,
        // the last one leading.
        const bool reverse = each < 0;
        const Letter& base = reverse ? letters.back() : letters.front();
        word = Value::MakeObject();
        word.Set("type", Value::MakeString("text"));
        if (const Value* uid = letters.front().node->Find("uid")) {
            word.Set("uid", *uid);
        }
        word.Set("style", *letters.front().node->Find("style"));
        word.Set("content", Value::MakeString(content));
        if (base.animate != nullptr) {
            word.Set("units", Value::MakeString("char"));
            if (each != 0) {
                Value stagger = Value::MakeObject();
                stagger.Set("each", Value::MakeInt(reverse ? -each : each));
                if (reverse) {
                    stagger.Set("order", Value::MakeString("reverse"));
                }
                word.Set("stagger", stagger);
            }
            word.Set("animate", *base.animate);
        }
        used = fused;
        ++report_.words;
        report_.letters += static_cast<int64_t>(fused);
        return true;
    }

    MotionMeasurer& measurer_;
    const MotionEvaluationContext& context_;
    MotionHumanizeReport& report_;
    MotionError& error_;
    int64_t fps_numerator_ = 25;
    int64_t fps_denominator_ = 1;
};

}  // namespace

bool HumanizeMotionDefinition(const Value& definition, MotionMeasurer& measurer,
                              const MotionEvaluationContext& context,
                              Value& output, MotionHumanizeReport& report,
                              MotionError& error) {
    report = MotionHumanizeReport{};
    Humanizer humanizer(measurer, context, report, error);
    return humanizer.Run(definition, output);
}

void MotionHumanizeReportFields(const MotionHumanizeReport& report,
                                Value& result) {
    result.Set("nodes_before", Value::MakeInt(report.nodes_before));
    result.Set("nodes_after", Value::MakeInt(report.nodes_after));
    result.Set("timecodes", Value::MakeInt(report.timecodes));
    result.Set("wrappers", Value::MakeInt(report.wrappers));
    result.Set("words", Value::MakeInt(report.words));
    result.Set("letters", Value::MakeInt(report.letters));
    Value notes = Value::MakeArray();
    for (const std::string& note : report.notes) {
        notes.Push(Value::MakeString(note));
    }
    result.Set("notes", std::move(notes));
}

bool CompareMotionRenders(const MotionFrameRequest& base,
                          const MotionComposition& before,
                          const MotionComposition& after,
                          std::vector<int64_t>& differing, MotionError& error) {
    differing.clear();
    const MotionFrameRenderer render = CurrentMotionFrameRenderer();
    if (render == nullptr) {
        error = {"UnsupportedOperation", "",
                 std::string("this process has no motion renderer (Core "
                             "Text and Core Graphics): ") +
                     kMotionRendererHolder};
        return false;
    }
    if (before.duration != after.duration || before.width != after.width ||
        before.height != after.height ||
        before.fps_numerator != after.fps_numerator ||
        before.fps_denominator != after.fps_denominator) {
        error = {"InvalidValue", "",
                 "the two compositions differ in size, rate or length"};
        return false;
    }
    const int64_t count = before.duration;
    std::vector<char> differs(static_cast<size_t>(std::max<int64_t>(0, count)),
                              0);
    std::atomic<int64_t> next{0};
    std::atomic_bool failed{false};
    std::mutex lock;
    MotionError firstError;
    const auto work = [&]() {
        for (;;) {
            const int64_t frame = next.fetch_add(1);
            if (frame >= count || failed.load()) {
                return;
            }
            MotionFrameImage images[2];
            const MotionComposition* sides[2] = {&before, &after};
            for (int side = 0; side < 2; ++side) {
                MotionFrameRequest request = base;
                request.composition = sides[side];
                request.frame = frame;
                request.raster = true;
                MotionError renderError;
                if (!render(request, images[side], renderError)) {
                    const std::lock_guard<std::mutex> guard(lock);
                    if (!failed.exchange(true)) {
                        firstError = renderError;
                    }
                    return;
                }
            }
            differs[static_cast<size_t>(frame)] =
                images[0].png != images[1].png ? 1 : 0;
        }
    };
    const unsigned workers =
        std::max(1U, std::min(8U, std::thread::hardware_concurrency()));
    std::vector<std::thread> threads;
    for (unsigned index = 0; index < workers; ++index) {
        threads.emplace_back(work);
    }
    for (std::thread& thread : threads) {
        thread.join();
    }
    if (failed.load()) {
        error = firstError;
        return false;
    }
    for (int64_t frame = 0; frame < count; ++frame) {
        if (differs[static_cast<size_t>(frame)] != 0) {
            differing.push_back(frame);
        }
    }
    return true;
}
