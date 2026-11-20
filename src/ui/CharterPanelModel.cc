// CH5 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel's fields, the edit
// a typed value becomes, and the panel's undo over versions
// (CharterPanelModel.h). Every field reads the charter file as it is
// written, so that what the panel shows is what the file says, at the level
// that says it.

#include "CharterPanelModel.h"

#include "CharterCanvas.h"
#include "Document.h"
#include "MediaPanelModel.h"
#include "MotionComposition.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <locale>
#include <numeric>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace ui::charter {

namespace {

using mcp_json::Value;
using media_panel::CaptionHeightPercent;
using media_panel::kCaptionHeightPercentMax;
using media_panel::kCaptionHeightPercentMin;
using media_panel::kCaptionTabMaximumFontSize;
using media_panel::kCaptionTabMinimumFontSize;

constexpr char kSectionCharter[] = "Charte";
constexpr char kSectionColors[] = "Couleurs";
constexpr char kSectionFonts[] = "Polices";
constexpr char kSectionCaptions[] = "Sous-titres";
constexpr char kSectionPiecePrefix[] = "Pièce · ";

bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' ||
           c == '\v';
}

std::string Trimmed(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsSpace(text[begin])) {
        ++begin;
    }
    while (end > begin && IsSpace(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

// A number as a person types it, in either decimal mark ("12,5" as
// "12.5"), read whatever the process locale.
bool ReadNumber(const std::string& typed, double& number) {
    std::string text = Trimmed(typed);
    std::replace(text.begin(), text.end(), ',', '.');
    if (text.empty()) {
        return false;
    }
    std::istringstream stream(text);
    stream.imbue(std::locale::classic());
    double value = 0.0;
    stream >> value;
    if (stream.fail() || !stream.eof() || !std::isfinite(value)) {
        return false;
    }
    number = value;
    return true;
}

bool IsHex(const std::string& text) {
    return text.size() == 7 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// "#rrggbb", or "#rrggbb@a" with an opacity a from 0 to 1: what a
// charter's captions take (ParseMotionKit).
bool IsCaptionColor(const std::string& text) {
    const size_t at = text.find('@');
    if (!IsHex(text.substr(0, at))) {
        return false;
    }
    if (at == std::string::npos) {
        return true;
    }
    const std::string alpha = text.substr(at + 1);
    if (alpha.empty() || alpha.size() > 8 ||
        !std::all_of(alpha.begin(), alpha.end(), [](unsigned char c) {
            return std::isdigit(c) != 0 || c == '.';
        })) {
        return false;
    }
    double opacity = 0.0;
    return ReadNumber(alpha, opacity) && opacity >= 0.0 && opacity <= 1.0;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

bool EndsWith(const std::string& text, const std::string& suffix) {
    return text.size() >= suffix.size() &&
           text.compare(text.size() - suffix.size(), suffix.size(), suffix) ==
               0;
}

std::string FirstSegment(const std::string& path) {
    return path.substr(0, path.find('.'));
}

bool IsRole(const std::string& name) {
    return std::any_of(
        MotionKitRoles().begin(), MotionKitRoles().end(),
        [&name](const MotionKitRole& role) { return name == role.name; });
}

std::string NumberText(double number) {
    return MotionKitNumber(number, 0.0).Dump();
}

// A charter's vertical_position, {num, den}, as the whole percentage the
// field shows (CaptionHeightPercent); empty for the preset (num 0) or a
// value that is not a fraction.
std::string PercentText(const Value& fraction) {
    int64_t num = 0;
    int64_t den = 0;
    const Value* numValue = fraction.Find("num");
    const Value* denValue = fraction.Find("den");
    if (numValue == nullptr || denValue == nullptr || !numValue->AsInt64(num) ||
        !denValue->AsInt64(den) || den <= 0 || num <= 0) {
        return {};
    }
    CaptionStyle style;
    style.vertical_position = {static_cast<int32_t>(num),
                               static_cast<int32_t>(den)};
    return std::to_string(CaptionHeightPercent(style));
}

// A value of the file as a field shows it: a text as written, a number as
// MotionKitNumber spells it, a boolean as "true" or "false", a list one
// item per line.
std::string Spelled(const Value& value) {
    switch (value.type()) {
        case mcp_json::Type::String:
            return value.AsString();
        case mcp_json::Type::Number: {
            double number = 0.0;
            return MotionNumber(value, number) ? NumberText(number)
                                               : value.Dump();
        }
        case mcp_json::Type::Bool:
            return value.AsBool() ? "true" : "false";
        case mcp_json::Type::Array: {
            std::string lines;
            for (const Value& item : value.AsArray()) {
                lines += (lines.empty() ? "" : "\n") + Spelled(item);
            }
            return lines;
        }
        case mcp_json::Type::Object:
            return PercentText(value);
        case mcp_json::Type::Null:
            break;
    }
    return {};
}

// "marge_gauche" as a label: "Marge gauche", and " (%)" for a percentage.
std::string LabelOf(const std::string& name, const std::string& unit) {
    std::string label = name;
    std::replace(label.begin(), label.end(), '_', ' ');
    if (!label.empty() && label[0] >= 'a' && label[0] <= 'z') {
        label[0] = static_cast<char>(label[0] - 'a' + 'A');
    }
    if (unit == "%") {
        label += " (%)";
    } else if (unit == "px") {
        label += " (px)";
    }
    return label;
}

// The text props an image node's `src` is bound to, under `node` (the
// logo's `logo`): a file the charter names, where every other text prop of
// a piece is the film's content.
void CollectImageSources(const Value& node, std::vector<std::string>& names) {
    if (!node.IsObject()) {
        return;
    }
    const Value* type = node.Find("type");
    const Value* src = node.Find("src");
    if (type != nullptr && type->IsString() && type->AsString() == "image" &&
        src != nullptr && src->IsObject() && src->AsObject().size() == 1) {
        const Value* prop = src->Find("prop");
        if (prop != nullptr && prop->IsString()) {
            names.push_back(prop->AsString());
        }
    }
    if (const Value* children = node.Find("children");
        children != nullptr && children->IsArray()) {
        for (const Value& child : children->AsArray()) {
            CollectImageSources(child, names);
        }
    }
}

std::optional<double> NumberOf(const Value* value) {
    double number = 0.0;
    if (value == nullptr || !value->IsNumber() ||
        !MotionNumber(*value, number)) {
        return std::nullopt;
    }
    return number;
}

std::optional<std::pair<int64_t, int64_t>> SizeOf(const Value& entry) {
    int64_t width = 0;
    int64_t height = 0;
    const Value* w = entry.Find("width");
    const Value* h = entry.Find("height");
    if (w == nullptr || h == nullptr || !w->AsInt64(width) ||
        !h->AsInt64(height)) {
        return std::nullopt;
    }
    return std::make_pair(width, height);
}

// The charter's caption format of `frame`'s size, and its name; null when
// it declares none of that size.
const Value* CaptionFormatOf(const Value& kitFile, const PreviewFrame& frame,
                             std::string& name) {
    name.clear();
    const Value* formats = FindMotionKitPath(kitFile, "captions.formats");
    if (formats == nullptr || !formats->IsObject()) {
        return nullptr;
    }
    for (const auto& [formatName, entry] : formats->AsObject()) {
        const auto size = SizeOf(entry);
        if (size && size->first == frame.width &&
            size->second == frame.height) {
            name = formatName;
            return &entry;
        }
    }
    return nullptr;
}

Field TopField(const Value& kitFile, const std::string& path,
               const std::string& label, const std::string& section,
               FieldKind kind) {
    Field field;
    field.path = path;
    field.label = label;
    field.section = section;
    field.kind = kind;
    if (const Value* value = FindMotionKitPath(kitFile, path)) {
        field.value = Spelled(*value);
        field.origin = kOriginCharter;
    }
    return field;
}

// One caption setting the panel offers: the charter's key, its label, its
// kind and bounds, and the value a new caption style starts from.
struct CaptionSetting {
    const char* key;
    const char* label;
    FieldKind kind;
    std::optional<double> min;
    std::optional<double> max;
    std::string start;
};

std::vector<CaptionSetting> CaptionSettings() {
    const CaptionStyle start;
    return {
        {"font_family", "Police", FieldKind::Text, {}, {}, start.font_family},
        {"font_size", "Taille (px)", FieldKind::Integer,
         kCaptionTabMinimumFontSize, kCaptionTabMaximumFontSize,
         std::to_string(start.font_size)},
        {"font_weight", "Graisse", FieldKind::Integer,
         kCaptionStyleMinFontWeight, kCaptionStyleMaxFontWeight,
         std::to_string(start.font_weight)},
        {"color", "Couleur", FieldKind::Color, {}, {}, start.color},
        {"position", "Position", FieldKind::Enum, {}, {}, start.position},
        {"outline_width", "Contour (px)", FieldKind::Integer, 0,
         kCaptionStyleMaxOutlineWidth, std::to_string(start.outline_width)},
        {"outline_color",
         "Couleur du contour",
         FieldKind::Color,
         {},
         {},
         start.outline_color},
        {"shadow_offset", "Ombre (px)", FieldKind::Integer, 0,
         kCaptionStyleMaxShadowOffset, std::to_string(start.shadow_offset)},
        {"shadow_color",
         "Couleur de l’ombre",
         FieldKind::Color,
         {},
         {},
         start.shadow_color},
        {"background_color",
         "Fond",
         FieldKind::Color,
         {},
         {},
         start.background_color},
        {"background_padding", "Marge du fond (px)", FieldKind::Integer, 0,
         kCaptionStyleMaxBackgroundPadding,
         std::to_string(start.background_padding)},
        {"vertical_position", "Hauteur (%)", FieldKind::Integer,
         kCaptionHeightPercentMin, kCaptionHeightPercentMax, std::string()},
    };
}

void AddCaptionFields(const Value& kitFile, const PreviewFrame& frame,
                      const std::string& formatOrigin,
                      std::vector<Field>& fields) {
    const Value* captions = kitFile.Find("captions");
    if (captions != nullptr && !captions->IsObject()) {
        captions = nullptr;
    }
    std::string formatName;
    const Value* sized = CaptionFormatOf(kitFile, frame, formatName);
    for (const CaptionSetting& setting : CaptionSettings()) {
        Field field;
        field.label = setting.label;
        field.section = kSectionCaptions;
        field.kind = setting.kind;
        field.min = setting.min;
        field.max = setting.max;
        if (setting.kind == FieldKind::Integer) {
            field.step = 1.0;
        }
        if (setting.kind == FieldKind::Enum) {
            field.options = {"bottom", "center", "top"};
        }
        const Value* inFormat =
            sized != nullptr ? sized->Find(setting.key) : nullptr;
        const Value* inBase =
            captions != nullptr ? captions->Find(setting.key) : nullptr;
        if (inFormat != nullptr) {
            field.path = MotionKitCaptionPath(formatName, setting.key);
            field.value = Spelled(*inFormat);
            field.origin = formatOrigin;
        } else {
            field.path = MotionKitCaptionPath("", setting.key);
            if (inBase != nullptr) {
                field.value = Spelled(*inBase);
                field.origin = kOriginCharter;
            } else if (captions != nullptr) {
                field.value = setting.start;
                field.origin = kOriginDefault;
            }
        }
        fields.push_back(std::move(field));
    }
}

void AddPieceFields(const Value& kitFile, const MotionTemplateInfo& piece,
                    const PreviewFrame& frame, const std::string& formatOrigin,
                    std::vector<Field>& fields) {
    const Value* declared = piece.definition.Find("props");
    if (declared == nullptr || !declared->IsObject()) {
        return;
    }
    const Value* own = FindMotionKitPath(kitFile, "pieces." + piece.name);
    const std::string format =
        PieceFormatFor(piece.definition, frame.width, frame.height);
    const Value* ownFormat = own != nullptr && !format.empty()
                                 ? FindMotionKitPath(*own, "formats." + format)
                                 : nullptr;
    const Value* pieceFormat =
        format.empty() ? nullptr
                       : FindMotionKitPath(piece.definition,
                                           "formats." + format + ".props");
    std::vector<std::string> imageSources;
    if (const Value* root = piece.definition.Find("root")) {
        CollectImageSources(*root, imageSources);
    }
    for (const auto& [name, prop] : declared->AsObject()) {
        if (name == "formats") {
            continue;
        }
        const Value* typeValue = prop.Find("type");
        const std::string type = typeValue != nullptr && typeValue->IsString()
                                     ? typeValue->AsString()
                                     : std::string();
        FieldKind kind = FieldKind::Text;
        if (type == "color") {
            kind = FieldKind::Color;
        } else if (type == "number") {
            kind = FieldKind::Number;
        } else if (type == "enum") {
            kind = FieldKind::Enum;
        } else if (type == "bool") {
            kind = FieldKind::Bool;
        } else if (type != "text") {
            continue;  // list, point, size, group: no value a charter gives
        } else if (!IsRole(name) &&
                   std::find(imageSources.begin(), imageSources.end(), name) ==
                       imageSources.end()) {
            // CH6 -- a name, a function, a title: the words of one film,
            // typed where the piece is placed, never a value of the brand.
            // A text prop is the charter's only as a shared role (police)
            // or as the file an image shows (the logo's).
            continue;
        }
        const Value* kitFormatValue =
            ownFormat != nullptr ? ownFormat->Find(name) : nullptr;
        const Value* kitValue = own != nullptr ? own->Find(name) : nullptr;
        if (IsRole(name) && kitFormatValue == nullptr && kitValue == nullptr) {
            continue;
        }
        const Value* pieceFormatValue =
            pieceFormat != nullptr ? pieceFormat->Find(name) : nullptr;
        const bool perFormat = !format.empty() && (kitFormatValue != nullptr ||
                                                   pieceFormatValue != nullptr);
        const Value* unitValue = prop.Find("unit");
        const std::string unit = unitValue != nullptr && unitValue->IsString()
                                     ? unitValue->AsString()
                                     : std::string();
        Field field;
        field.path =
            MotionKitPiecePath(piece.name, perFormat ? format : "", name);
        field.label = LabelOf(name, unit);
        field.section = kSectionPiecePrefix + piece.name;
        field.kind = kind;
        if (kind == FieldKind::Number) {
            field.min = NumberOf(prop.Find("min"));
            field.max = NumberOf(prop.Find("max"));
            field.step = unit == "%" ? kPercentStep : 0.0;
        }
        if (const Value* values = prop.Find("values");
            kind == FieldKind::Enum && values != nullptr && values->IsArray()) {
            for (const Value& option : values->AsArray()) {
                if (option.IsString()) {
                    field.options.push_back(option.AsString());
                }
            }
        }
        if (kitFormatValue != nullptr) {
            field.value = Spelled(*kitFormatValue);
            field.origin = formatOrigin;
        } else if (kitValue != nullptr) {
            field.value = Spelled(*kitValue);
            field.origin = kOriginCharter;
        } else {
            const Value* shown = pieceFormatValue != nullptr
                                     ? pieceFormatValue
                                     : prop.Find("default");
            field.value = shown != nullptr ? Spelled(*shown) : std::string();
            field.origin = kOriginPiece;
        }
        fields.push_back(std::move(field));
    }
}

bool Refuse(std::string& messageFr, std::string message) {
    messageFr = std::move(message);
    return false;
}

// Whether `text` is a colour of the path `path`, and if not, why, in
// French.
bool ColorFits(const std::string& path, const std::string& text,
               std::string& messageFr) {
    if (StartsWith(path, "palette")) {
        return IsHex(text) ||
               Refuse(messageFr,
                      "Une couleur de la palette s’écrit #rrggbb, sans "
                      "opacité.");
    }
    if (StartsWith(path, "captions.")) {
        return IsCaptionColor(text) ||
               Refuse(messageFr,
                      "Une couleur de sous-titre s’écrit #rrggbb, ou "
                      "#rrggbb@a avec une opacité a de 0 à 1.");
    }
    MotionColor color;
    return ParseMotionColor(text, color) ||
           Refuse(messageFr,
                  "Couleur illisible : #rrggbb, #rgb, rgb(), rgba(), white "
                  "ou black.");
}

}  // namespace

std::vector<Field> KitFields(const mcp_json::Value& kitFile,
                             const std::vector<MotionTemplateInfo>& pieces,
                             const PreviewFrame& frame) {
    std::vector<Field> fields;
    if (!kitFile.IsObject()) {
        return fields;
    }
    const std::string formatOrigin =
        std::string(kOriginCharterFormatPrefix) + frame.label;
    fields.push_back(TopField(kitFile, "description", "Description",
                              kSectionCharter, FieldKind::Text));
    fields.push_back(TopField(kitFile, "principle", "Principe graphique",
                              kSectionCharter, FieldKind::LongText));
    fields.push_back(TopField(kitFile, "sources", "Sources", kSectionCharter,
                              FieldKind::List));
    fields.push_back(TopField(kitFile, "props.accent", "Couleur d’accent",
                              kSectionColors, FieldKind::Color));
    fields.push_back(TopField(kitFile, "props.texte", "Couleur du texte",
                              kSectionColors, FieldKind::Color));
    fields.push_back(TopField(kitFile, "props.fond", "Couleur de fond",
                              kSectionColors, FieldKind::Color));
    fields.push_back(TopField(kitFile, "palette", "Palette", kSectionColors,
                              FieldKind::List));
    fields.push_back(TopField(kitFile, "props.police", "Police", kSectionFonts,
                              FieldKind::Text));
    fields.push_back(TopField(kitFile, "fonts", "Polices permises",
                              kSectionFonts, FieldKind::List));
    AddCaptionFields(kitFile, frame, formatOrigin, fields);
    const Value* withdrawn = kitFile.Find("withdrawn");
    for (const MotionTemplateInfo& piece : pieces) {
        if (withdrawn != nullptr && withdrawn->IsObject() &&
            withdrawn->Find(piece.name) != nullptr) {
            continue;
        }
        AddPieceFields(kitFile, piece, frame, formatOrigin, fields);
    }
    return fields;
}

bool FieldEdit(const Field& field, const std::string& typedText,
               MotionKitEdit& edit, std::string& messageFr) {
    edit = MotionKitEdit{};
    edit.path = field.path;
    messageFr.clear();
    const std::string text = Trimmed(typedText);
    if (text.empty()) {
        return true;  // removed: the level below gives the value again
    }
    switch (field.kind) {
        case FieldKind::Number:
        case FieldKind::Integer: {
            double number = 0.0;
            if (!ReadNumber(text, number)) {
                return Refuse(messageFr,
                              field.kind == FieldKind::Integer
                                  ? "Un nombre entier est attendu."
                                  : "Un nombre est attendu, par exemple 12,5.");
            }
            const double step =
                field.kind == FieldKind::Integer ? 1.0 : field.step;
            const auto bounded = [&field](double value) {
                if (field.min) {
                    value = std::max(value, *field.min);
                }
                if (field.max) {
                    value = std::min(value, *field.max);
                }
                return value;
            };
            Value spelled = MotionKitNumber(bounded(number), step);
            double rounded = 0.0;
            if (!MotionNumber(spelled, rounded)) {
                rounded = bounded(number);
            }
            if (bounded(rounded) != rounded) {
                // A bound off the step: the bound itself.
                rounded = bounded(rounded);
                spelled = MotionKitNumber(rounded, 0.0);
            }
            if (field.kind == FieldKind::Integer) {
                const auto whole = static_cast<int64_t>(std::llround(rounded));
                if (EndsWith(field.path, "vertical_position")) {
                    const int64_t divisor = std::gcd(whole, int64_t{100});
                    Value fraction = Value::MakeObject();
                    fraction.Set("num", Value::MakeInt(whole / divisor));
                    fraction.Set("den", Value::MakeInt(100 / divisor));
                    edit.value = std::move(fraction);
                } else {
                    edit.value = Value::MakeInt(whole);
                }
            } else {
                edit.value = std::move(spelled);
            }
            return true;
        }
        case FieldKind::Color:
            if (!ColorFits(field.path, text, messageFr)) {
                return false;
            }
            edit.value = Value::MakeString(text);
            return true;
        case FieldKind::Enum: {
            if (std::find(field.options.begin(), field.options.end(), text) ==
                field.options.end()) {
                std::string options;
                for (const std::string& option : field.options) {
                    options += (options.empty() ? "" : ", ") + option;
                }
                return Refuse(messageFr,
                              "Valeurs possibles : " + options + ".");
            }
            edit.value = Value::MakeString(text);
            return true;
        }
        case FieldKind::Bool:
            if (text != "true" && text != "false") {
                return Refuse(messageFr, "Attendu : true ou false.");
            }
            edit.value = Value::MakeBool(text == "true");
            return true;
        case FieldKind::List: {
            Value list = Value::MakeArray();
            std::istringstream lines(typedText);
            std::string line;
            size_t number = 0;
            while (std::getline(lines, line)) {
                ++number;
                const std::string item = Trimmed(line);
                if (item.empty()) {
                    continue;
                }
                if (StartsWith(field.path, "palette") && !IsHex(item)) {
                    return Refuse(messageFr,
                                  "Ligne " + std::to_string(number) +
                                      " : une couleur de la palette s’écrit "
                                      "#rrggbb, sans opacité.");
                }
                list.Push(Value::MakeString(item));
            }
            edit.value = std::move(list);
            return true;
        }
        case FieldKind::LongText:
            edit.value = Value::MakeString(text);
            return true;
        case FieldKind::Text:
            if (text.find('\n') != std::string::npos) {
                return Refuse(messageFr, "Une seule ligne est attendue.");
            }
            edit.value = Value::MakeString(text);
            return true;
    }
    return Refuse(messageFr, "Ce champ ne prend pas de valeur.");
}

std::vector<MotionKitEdit> WithBrandFormat(const mcp_json::Value& kitFile,
                                           std::vector<MotionKitEdit> edits) {
    const Value* format = kitFile.Find("format");
    if (format != nullptr && format->IsString() &&
        format->AsString() == kBrandKitFormat) {
        return edits;
    }
    const auto charterOnly = [](const MotionKitEdit& edit) {
        const std::string first = FirstSegment(edit.path);
        return first == "palette" || first == "fonts" || first == "sources" ||
               first == "captions" || first == "pieces" || first == "withdrawn";
    };
    const auto setsFormat = [](const MotionKitEdit& edit) {
        return edit.path == "format";
    };
    if (std::any_of(edits.begin(), edits.end(), charterOnly) &&
        std::none_of(edits.begin(), edits.end(), setsFormat)) {
        edits.insert(
            edits.begin(),
            MotionKitEdit{"format", Value::MakeString(kBrandKitFormat), false});
    }
    return edits;
}

mcp_json::Value NewKitFile(const std::string& description) {
    Value props = Value::MakeObject();
    props.Set("police", Value::MakeString("Helvetica Neue"));
    Value file = Value::MakeObject();
    file.Set("format", Value::MakeString(kBrandKitFormat));
    file.Set("description", Value::MakeString(description));
    file.Set("props", std::move(props));
    return file;
}

std::vector<MotionKitEdit> CaptionFormatEdits(
    const mcp_json::Value& kitFile, const std::vector<PreviewFrame>& frames) {
    std::vector<MotionKitEdit> edits;
    const Value* captions = kitFile.Find("captions");
    if (captions == nullptr || !captions->IsObject()) {
        return edits;
    }
    const Value* formats = captions->Find("formats");
    for (const PreviewFrame& frame : frames) {
        bool sized = false;
        bool named = false;
        if (formats != nullptr && formats->IsObject()) {
            for (const auto& [name, entry] : formats->AsObject()) {
                const auto size = SizeOf(entry);
                sized = sized || (size && size->first == frame.width &&
                                  size->second == frame.height);
                named = named || name == frame.name;
            }
        }
        if (sized) {
            continue;
        }
        // The usual name taken by a format of another size: the size
        // itself tells this one apart.
        const std::string name = named ? frame.name + "_" +
                                             std::to_string(frame.width) + "x" +
                                             std::to_string(frame.height)
                                       : frame.name;
        Value entry = Value::MakeObject();
        entry.Set("width", Value::MakeInt(frame.width));
        entry.Set("height", Value::MakeInt(frame.height));
        edits.push_back({"captions.formats." + name, std::move(entry), false});
    }
    return edits;
}

std::string KitFileDivergedMessage(const std::string& name,
                                   const std::string& issue) {
    std::string message = "Le fichier kits/" + name + ".json ne se lit plus";
    if (!issue.empty()) {
        message += " : " + issue;
    }
    return message +
           ". Corrige-le ou déplace-le ; le panneau n’écrit rien tant qu’il "
           "diverge.";
}

std::string KitWriteRefusalMessage(const std::string& name,
                                   const MotionKitError& error) {
    const std::string reason =
        error.message.empty() ? "" : " (" + error.message + ")";
    if (error.code == kMotionKitStaleKit) {
        return "La charte « " + name +
               " » a changé ailleurs entre-temps : le panneau la relit, rien "
               "n’est écrit. Refais le geste sur ce qu’elle est devenue.";
    }
    if (error.code == kMotionKitFileDiverged) {
        return KitFileDivergedMessage(name, error.message);
    }
    if (error.code == kMotionKitInvalidKit) {
        return "La charte refuserait cette valeur" + reason +
               " : rien n’est écrit.";
    }
    if (error.code == kMotionKitInvalidEdit) {
        return "Cette modification n’a pas de place dans une charte" + reason +
               " : rien n’est écrit.";
    }
    if (error.code == kMotionKitUnknownKit) {
        return "La charte « " + name + " » n’existe plus" + reason + ".";
    }
    if (error.code == kMotionKitIoError) {
        return "La bibliothèque n’a pas pu être écrite" + reason + ".";
    }
    return "Écriture refusée" + reason + ".";
}

void RecordSave(History& history, int64_t fromVersion, int64_t toVersion) {
    if (fromVersion == toVersion) {
        return;
    }
    history.undo.push_back({fromVersion, toVersion});
    history.redo.clear();
}

std::optional<int64_t> UndoTarget(const History& history,
                                  int64_t latestVersion) {
    if (history.undo.empty() ||
        history.undo.back().to_version != latestVersion) {
        return std::nullopt;
    }
    return history.undo.back().from_version;
}

std::optional<int64_t> RedoTarget(const History& history,
                                  int64_t latestVersion) {
    if (history.redo.empty() ||
        history.redo.back().from_version != latestVersion) {
        return std::nullopt;
    }
    return history.redo.back().to_version;
}

void RecordUndo(History& history, int64_t writtenVersion) {
    if (history.undo.empty()) {
        return;
    }
    const HistoryStep step = history.undo.back();
    history.undo.pop_back();
    history.redo.push_back({writtenVersion, step.to_version});
    if (!history.undo.empty() &&
        history.undo.back().to_version == step.from_version) {
        history.undo.back().to_version = writtenVersion;
    }
}

void RecordRedo(History& history, int64_t writtenVersion) {
    if (history.redo.empty()) {
        return;
    }
    const HistoryStep step = history.redo.back();
    history.redo.pop_back();
    history.undo.push_back({step.from_version, writtenVersion});
    if (!history.redo.empty() &&
        history.redo.back().from_version == step.to_version) {
        history.redo.back().from_version = writtenVersion;
    }
}

}  // namespace ui::charter
