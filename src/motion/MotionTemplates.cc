#include "MotionTemplates.h"

#include "Document.h"
#include "LocalEnv.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "ShotDescriptions.h"

#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace {

namespace fs = std::filesystem;

using mcp_json::Value;

// A piece is a few kilobytes; a file past this is not one, and is not read
// whole to find that out.
constexpr uintmax_t kMaxPieceBytes = uintmax_t{16} * 1024 * 1024;

bool ReadOptionalText(const Value& file, const char* key, std::string& out,
                      std::string& error) {
    const Value* value = file.Find(key);
    if (value == nullptr) {
        return true;
    }
    if (!value->IsString()) {
        error = std::string("'") + key + "' must be a string";
        return false;
    }
    out = value->AsString();
    return true;
}

// A piece is added at the size of a timeline: without a format it could
// be listed and never added. What each format sets is the parser's to
// judge (a prop it does not declare is refused there).
bool DeclaresAFormat(const Value& definition, std::string& error) {
    const Value* formats = definition.Find("formats");
    if (formats == nullptr || !formats->IsObject() ||
        formats->AsObject().empty()) {
        error =
            "the composition declares no format: a piece is added at "
            "the size of a timeline, and needs one format per size it "
            "has a layout for";
        return false;
    }
    return true;
}

// Past this width an array of scalars takes one line per item: sentences
// read better apart, `frames` and `values` together.
constexpr size_t kInlineArrayWidth = 64;

// Scalars on one line, containers one member per line -- except a short
// array of scalars, which stays on its line: a composition is mostly
// `frames` and `values`, and one number per line would bury the tree.
void DumpIndented(const Value& value, size_t depth, std::string& out) {
    const auto indent = [&](size_t level) { out.append(level * 2, ' '); };
    if (value.IsObject()) {
        if (value.AsObject().empty()) {
            out += "{}";
            return;
        }
        out += "{\n";
        bool first = true;
        for (const auto& [key, child] : value.AsObject()) {
            if (!first) {
                out += ",\n";
            }
            first = false;
            indent(depth + 1);
            out += "\"" + mcp_json::EscapeJsonString(key) + "\": ";
            DumpIndented(child, depth + 1, out);
        }
        out += "\n";
        indent(depth);
        out += "}";
        return;
    }
    if (value.IsArray()) {
        const std::vector<Value>& items = value.AsArray();
        const bool flat =
            std::none_of(items.begin(), items.end(), [](const Value& item) {
                return item.IsObject() || item.IsArray();
            });
        if (flat) {
            std::string line = "[";
            for (size_t index = 0; index < items.size(); ++index) {
                line += (index == 0 ? "" : ", ") + items[index].Dump();
            }
            line += "]";
            if (line.size() <= kInlineArrayWidth) {
                out += line;
                return;
            }
        }
        out += "[\n";
        for (size_t index = 0; index < items.size(); ++index) {
            if (index != 0) {
                out += ",\n";
            }
            indent(depth + 1);
            DumpIndented(items[index], depth + 1, out);
        }
        out += "\n";
        indent(depth);
        out += "]";
        return;
    }
    out += value.Dump();
}

// The nodes of a definition that hold something of the project: a media
// written by its ULID, or another composition. A media bound to a prop
// holds nothing: each project gives the prop its own. Named the way their
// author named them.
void CollectProjectBound(const Value& node, std::vector<std::string>& bound) {
    if (!node.IsObject()) {
        return;
    }
    const Value* type = node.Find("type");
    const std::string kind =
        type != nullptr && type->IsString() ? type->AsString() : std::string();
    const Value* played = node.Find("media");
    const bool media = (kind == "video" || kind == "audio") &&
                       played != nullptr && played->IsString();
    if (media || kind == "composition") {
        const Value* id = node.Find("id");
        bound.push_back(id != nullptr && id->IsString()
                            ? "'" + id->AsString() + "' (" + kind + ")"
                            : "a " + kind + " node");
    }
    if (const Value* children = node.Find("children");
        children != nullptr && children->IsArray()) {
        for (const Value& child : children->AsArray()) {
            CollectProjectBound(child, bound);
        }
    }
    if (const Value* repeated = node.Find("template")) {
        CollectProjectBound(*repeated, bound);
    }
}

// The name a size usually goes by, else the size itself.
std::string FormatNameOf(int64_t width, int64_t height) {
    struct Known {
        int64_t width;
        int64_t height;
        const char* name;
    };
    static constexpr Known kKnown[] = {{1080, 1920, "9x16"},
                                       {1920, 1080, "16x9"},
                                       {1080, 1080, "1x1"},
                                       {1080, 1350, "4x5"}};
    for (const Known& known : kKnown) {
        if (known.width == width && known.height == height) {
            return known.name;
        }
    }
    return std::to_string(width) + "x" + std::to_string(height);
}

}  // namespace

bool IsMotionPieceName(const std::string& name) {
    if (name.empty() || name.size() > 64) {
        return false;
    }
    const auto alnum = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    if (!alnum(name.front())) {
        return false;
    }
    return std::all_of(name.begin(), name.end(), [&](char c) {
        return alnum(c) || c == '-' || c == '_';
    });
}

bool MotionPieceMatches(const MotionTemplateInfo& piece,
                        const std::string& query) {
    std::string haystack = piece.name + '\n' + piece.description + '\n' +
                           piece.use + '\n' + piece.avoid + '\n' + piece.origin;
    for (const std::string& invariant : piece.invariants) {
        haystack += '\n' + invariant;
    }
    haystack = FoldShotText(haystack);
    const std::string folded = FoldShotText(query);
    size_t start = 0;
    while (start < folded.size()) {
        const size_t end = folded.find_first_of(" \t\r\n", start);
        const size_t stop = end == std::string::npos ? folded.size() : end;
        if (stop > start && haystack.find(folded.substr(start, stop - start)) ==
                                std::string::npos) {
            return false;
        }
        start = stop + 1;
    }
    return true;
}

bool ParseMotionPiece(const std::string& name, const std::string& json,
                      MotionTemplateInfo& piece, std::string& error) {
    piece = MotionTemplateInfo{};
    piece.name = name;
    Value file;
    std::string parseError;
    if (!Value::Parse(json, file, parseError)) {
        error = "not JSON: " + parseError;
        return false;
    }
    if (!file.IsObject()) {
        error = "a piece is a JSON object";
        return false;
    }
    for (const auto& entry : file.AsObject()) {
        static const char* const kFields[] = {
            "format", "description", "use",        "avoid",
            "origin", "invariants",  "composition"};
        if (std::none_of(
                std::begin(kFields), std::end(kFields),
                [&](const char* field) { return entry.first == field; })) {
            error = "unknown field '" + entry.first + "'";
            return false;
        }
    }
    const Value* format = file.Find("format");
    if (format == nullptr || !format->IsString() ||
        format->AsString() != kMotionPieceFormat) {
        error = std::string("'format' must be \"") + kMotionPieceFormat +
                "\": a bare composition is not a piece (save_motion_template "
                "writes one from a composition of a project)";
        return false;
    }
    if (!ReadOptionalText(file, "description", piece.description, error) ||
        !ReadOptionalText(file, "use", piece.use, error) ||
        !ReadOptionalText(file, "avoid", piece.avoid, error) ||
        !ReadOptionalText(file, "origin", piece.origin, error)) {
        return false;
    }
    if (piece.description.empty()) {
        error = "'description' is required: it is what a piece is chosen by";
        return false;
    }
    if (const Value* invariants = file.Find("invariants")) {
        if (!invariants->IsArray()) {
            error = "'invariants' must be an array of sentences";
            return false;
        }
        for (const Value& invariant : invariants->AsArray()) {
            if (!invariant.IsString() || invariant.AsString().empty()) {
                error = "'invariants' must be an array of sentences";
                return false;
            }
            piece.invariants.push_back(invariant.AsString());
        }
    }
    const Value* composition = file.Find("composition");
    if (composition == nullptr || !composition->IsObject()) {
        error = "'composition' is required";
        return false;
    }
    piece.definition = *composition;
    MotionComposition parsed;
    MotionError motion;
    if (!ParseMotionComposition(piece.definition, parsed, motion)) {
        error = "not a composition: " + motion.code + " at " + motion.path +
                ": " + motion.message;
        return false;
    }
    std::vector<Ulid> nodes;
    std::vector<Ulid> keyframes;
    MotionDefinitionIdentities(piece.definition, nodes, keyframes);
    if (!nodes.empty() || !keyframes.empty()) {
        error =
            "the composition carries identities (uid, uids): a piece is "
            "added to many projects and each draws its own "
            "(save_motion_template removes them)";
        return false;
    }
    return DeclaresAFormat(piece.definition, error);
}

std::string DumpMotionPiece(const MotionTemplateInfo& piece) {
    Value file = Value::MakeObject();
    file.Set("format", Value::MakeString(kMotionPieceFormat));
    file.Set("description", Value::MakeString(piece.description));
    if (!piece.use.empty()) {
        file.Set("use", Value::MakeString(piece.use));
    }
    if (!piece.avoid.empty()) {
        file.Set("avoid", Value::MakeString(piece.avoid));
    }
    if (!piece.invariants.empty()) {
        Value invariants = Value::MakeArray();
        for (const std::string& invariant : piece.invariants) {
            invariants.Push(Value::MakeString(invariant));
        }
        file.Set("invariants", std::move(invariants));
    }
    if (!piece.origin.empty()) {
        file.Set("origin", Value::MakeString(piece.origin));
    }
    file.Set("composition", piece.definition);
    return DumpIndentedMotionJson(file) + "\n";
}

std::string DumpIndentedMotionJson(const Value& value) {
    std::string out;
    DumpIndented(value, 0, out);
    return out;
}

bool MotionTemplateCatalog(std::vector<MotionTemplateInfo>& templates,
                           std::string& error) {
    templates.clear();
    for (const auto& [name, json] : EmbeddedMotionTemplateSources()) {
        MotionTemplateInfo info;
        std::string reason;
        if (!ParseMotionPiece(name, json, info, reason)) {
            error = "motion template '";
            error += name;
            error += "' is not a piece: ";
            error += reason;
            return false;
        }
        templates.push_back(std::move(info));
    }
    return true;
}

void ReadMotionLibrary(const std::string& directory,
                       const std::vector<MotionTemplateInfo>& shipped,
                       std::vector<MotionTemplateInfo>& pieces,
                       std::vector<MotionLibraryIssue>& issues) {
    pieces.clear();
    issues.clear();
    if (directory.empty()) {
        return;
    }
    std::error_code failure;
    std::vector<fs::path> files;
    for (fs::directory_iterator entry(directory, failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const fs::path& path = entry->path();
        const std::string filename = path.filename().string();
        std::error_code ignored;
        if (filename.empty() || filename.front() == '.' ||
            path.extension() != ".json" ||
            !fs::is_regular_file(path, ignored)) {
            continue;
        }
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        const std::string name = path.stem().string();
        const auto issue = [&](std::string message) {
            issues.push_back({path.string(), std::move(message)});
        };
        if (!IsMotionPieceName(name)) {
            issue("'" + name +
                  "' is not a piece name: letters, digits, '-' and '_'");
            continue;
        }
        if (std::any_of(shipped.begin(), shipped.end(),
                        [&](const MotionTemplateInfo& info) {
                            return info.name == name;
                        })) {
            issue("'" + name +
                  "' is the name of a template CUTMACHINE ships: rename the "
                  "file, a library piece never replaces a shipped one");
            continue;
        }
        std::error_code sizeFailure;
        const uintmax_t size = fs::file_size(path, sizeFailure);
        if (sizeFailure || size > kMaxPieceBytes) {
            issue(sizeFailure ? "unreadable: " + sizeFailure.message()
                              : "larger than a piece can be");
            continue;
        }
        std::ifstream file(path, std::ios::binary);
        const std::string json((std::istreambuf_iterator<char>(file)),
                               std::istreambuf_iterator<char>());
        if (!file.good() && !file.eof()) {
            issue("unreadable");
            continue;
        }
        MotionTemplateInfo piece;
        std::string reason;
        if (!ParseMotionPiece(name, json, piece, reason)) {
            issue(reason);
            continue;
        }
        piece.path = path.string();
        pieces.push_back(std::move(piece));
    }
}

const std::vector<MotionKitRole>& MotionKitRoles() {
    static const std::vector<MotionKitRole> kRoles = {
        {"accent", "color", "the color that signs the charter"},
        {"texte", "color", "the color of the text"},
        {"fond", "color", "the color of a full background or a card"},
        {"police", "text", "the font family of the text (fontFamily)"},
    };
    return kRoles;
}

namespace {

// DEC6 -- "#rrggbb", exactly: a palette entry is a colour, not an opacity.
bool IsHexColor(const std::string& text) {
    return text.size() == 7 && text[0] == '#' &&
           std::all_of(text.begin() + 1, text.end(),
                       [](unsigned char c) { return std::isxdigit(c) != 0; });
}

// DEC6 -- a charter's caption colour: "#rrggbb", or "#rrggbb@a" with an
// opacity a in [0, 1]. What FFmpeg's colour syntax reads (the export,
// ParseCaptionColor) and what MotionColorKey reads too, so that a
// charter's own captions always pass its check.
bool IsCharterCaptionColor(const std::string& text) {
    const size_t at = text.find('@');
    if (!IsHexColor(text.substr(0, at))) {
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
    char* end = nullptr;
    const double value = std::strtod(alpha.c_str(), &end);
    return end != nullptr && *end == '\0' && value >= 0.0 && value <= 1.0;
}

bool ReadTextList(const Value& file, const char* key,
                  std::vector<std::string>& out, std::string& error) {
    const Value* list = file.Find(key);
    if (list == nullptr) {
        return true;
    }
    if (!list->IsArray()) {
        error = std::string("'") + key + "' must be an array of strings";
        return false;
    }
    for (size_t index = 0; index < list->AsArray().size(); ++index) {
        const Value& item = list->AsArray()[index];
        if (!item.IsString() || item.AsString().empty()) {
            error = std::string("'") + key + "[" + std::to_string(index) +
                    "]' must be a non-empty string";
            return false;
        }
        out.push_back(item.AsString());
    }
    return true;
}

// DEC6 -- caption settings, as add_caption_style reads them, over `style`.
// `formats` is skipped where it is allowed (the charter's top level); any
// other key the caption style does not have is refused by name.
bool ApplyCharterCaptionSettings(const Value& settings, const std::string& path,
                                 bool formatsAllowed, CaptionStyle& style,
                                 std::vector<std::string>& set,
                                 std::string& error) {
    if (!settings.IsObject()) {
        error = "'" + path + "' must be an object";
        return false;
    }
    for (const auto& [key, value] : settings.AsObject()) {
        const std::string at = path + "." + key;
        if (key == "formats" && formatsAllowed) {
            continue;
        }
        if ((key == "width" || key == "height") && !formatsAllowed) {
            continue;  // A caption format's size, read by the caller.
        }
        std::string* text = key == "font_family"     ? &style.font_family
                            : key == "color"         ? &style.color
                            : key == "position"      ? &style.position
                            : key == "outline_color" ? &style.outline_color
                            : key == "shadow_color"  ? &style.shadow_color
                            : key == "background_color"
                                ? &style.background_color
                                : nullptr;
        int32_t* number = key == "font_size"       ? &style.font_size
                          : key == "outline_width" ? &style.outline_width
                          : key == "shadow_offset" ? &style.shadow_offset
                          : key == "background_padding"
                              ? &style.background_padding
                              : nullptr;
        if (text != nullptr) {
            const bool color = key.find("color") != std::string::npos;
            if (!value.IsString() ||
                (value.AsString().empty() && key != "background_color") ||
                (color && !value.AsString().empty() &&
                 !IsCharterCaptionColor(value.AsString()))) {
                error = "'" + at + "' must be " +
                        (color ? std::string("\"#rrggbb\", or \"#rrggbb@a\" "
                                             "with an opacity a in [0, 1]")
                               : std::string("a non-empty string"));
                return false;
            }
            *text = value.AsString();
        } else if (number != nullptr) {
            int64_t integer = 0;
            if (!value.IsNumber() || !value.AsInt64(integer) || integer < 0 ||
                integer > 4096) {
                error = "'" + at + "' must be a whole number of pixels";
                return false;
            }
            *number = static_cast<int32_t>(integer);
        } else if (key == "font_weight") {
            int64_t weight = 0;
            if (!value.IsNumber() || !value.AsInt64(weight) ||
                weight < kCaptionStyleMinFontWeight ||
                weight > kCaptionStyleMaxFontWeight) {
                error = "'" + at + "' must be a weight from 1 to 1000";
                return false;
            }
            style.font_weight = static_cast<int32_t>(weight);
        } else if (key == "vertical_position") {
            const Value* num = value.IsObject() ? value.Find("num") : nullptr;
            const Value* den = value.IsObject() ? value.Find("den") : nullptr;
            int64_t numerator = 0;
            int64_t denominator = 0;
            if (num == nullptr || den == nullptr ||
                value.AsObject().size() != 2 || !num->AsInt64(numerator) ||
                !den->AsInt64(denominator) || denominator <= 0 ||
                denominator > 1000000 || numerator < 0 ||
                numerator >= denominator) {
                error = "'" + at +
                        "' must be {\"num\",\"den\"}, an exact fraction of "
                        "the frame height inside it";
                return false;
            }
            style.vertical_position =
                numerator == 0
                    ? EffectParamValue{0, 1}
                    : EffectParamValue{static_cast<int32_t>(numerator),
                                       static_cast<int32_t>(denominator)};
        } else {
            error =
                "'" + at +
                "' is not a caption setting; a charter's captions take "
                "font_family, font_size, font_weight, color, position, "
                "outline_width, "
                "outline_color, shadow_offset, shadow_color, "
                "background_color, background_padding, "
                "vertical_position" +
                (formatsAllowed ? std::string(" and formats") : std::string());
            return false;
        }
        if (std::find(set.begin(), set.end(), key) == set.end()) {
            set.push_back(key);
        }
    }
    std::string invalid;
    if (!ValidateCaptionStyle(style, invalid)) {
        error = "'" + path + "' " + invalid;
        return false;
    }
    return true;
}

bool ReadCaptionFormatSize(const Value& format, const std::string& path,
                           int32_t& width, int32_t& height,
                           std::string& error) {
    const Value* w = format.IsObject() ? format.Find("width") : nullptr;
    const Value* h = format.IsObject() ? format.Find("height") : nullptr;
    int64_t widthValue = 0;
    int64_t heightValue = 0;
    if (w == nullptr || h == nullptr || !w->AsInt64(widthValue) ||
        !h->AsInt64(heightValue) || widthValue <= 0 || heightValue <= 0 ||
        widthValue > 16384 || heightValue > 16384) {
        error = "'" + path +
                "' needs its size: 'width' and 'height', whole pixels: a "
                "caption's sizes are pixels of a frame";
        return false;
    }
    width = static_cast<int32_t>(widthValue);
    height = static_cast<int32_t>(heightValue);
    return true;
}

// Every caption colour a charter writes, base and formats, in order.
void CollectCaptionColors(const Value& captions,
                          std::vector<std::string>& colors) {
    const auto collect = [&](const Value& settings) {
        for (const char* key :
             {"color", "outline_color", "shadow_color", "background_color"}) {
            const Value* value = settings.Find(key);
            if (value != nullptr && value->IsString() &&
                !value->AsString().empty()) {
                colors.push_back(
                    value->AsString().substr(0, value->AsString().find('@')));
            }
        }
    };
    if (!captions.IsObject()) {
        return;
    }
    collect(captions);
    if (const Value* formats = captions.Find("formats");
        formats != nullptr && formats->IsObject()) {
        for (const auto& entry : formats->AsObject()) {
            collect(entry.second);
        }
    }
}

void AddUnique(std::vector<std::string>& list, const std::string& item) {
    if (!item.empty() &&
        std::find(list.begin(), list.end(), item) == list.end()) {
        list.push_back(item);
    }
}

}  // namespace

bool ParseMotionKit(const std::string& name, const std::string& json,
                    MotionKitInfo& kit, std::string& error) {
    kit = MotionKitInfo{};
    kit.name = name;
    Value file;
    std::string parseError;
    if (!Value::Parse(json, file, parseError)) {
        error = "not JSON: " + parseError;
        return false;
    }
    if (!file.IsObject()) {
        error = "a kit is a JSON object";
        return false;
    }
    const Value* format = file.Find("format");
    if (format == nullptr || !format->IsString() ||
        (format->AsString() != kMotionKitFormat &&
         format->AsString() != kBrandKitFormat)) {
        error = std::string("'format' must be \"") + kMotionKitFormat +
                "\" or \"" + kBrandKitFormat + "\"";
        return false;
    }
    kit.format = format->AsString();
    const bool brand = kit.format == kBrandKitFormat;
    for (const auto& entry : file.AsObject()) {
        const std::string& key = entry.first;
        const bool common = key == "format" || key == "description" ||
                            key == "principle" || key == "props";
        const bool charter = key == "palette" || key == "fonts" ||
                             key == "captions" || key == "sources" ||
                             key == "pieces" || key == "withdrawn";
        if (common || (brand && charter)) {
            continue;
        }
        error = "unknown field '" + key + "'";
        if (charter) {
            error += std::string(
                         ": a field of a brand's charter, write "
                         "\"format\": \"") +
                     kBrandKitFormat + "\"";
        }
        return false;
    }
    if (!ReadOptionalText(file, "description", kit.description, error) ||
        !ReadOptionalText(file, "principle", kit.principle, error)) {
        return false;
    }
    if (kit.description.empty()) {
        error = "'description' is required: it is what a kit is chosen by";
        return false;
    }
    const Value* props = file.Find("props");
    if ((!brand || props != nullptr) &&
        (props == nullptr || !props->IsObject() || props->AsObject().empty())) {
        error =
            "'props' is required: the values the kit gives the props "
            "pieces share (accent, texte, fond, police)";
        if (brand) {
            error =
                "'props' must be an object that sets some of the props "
                "pieces share (accent, texte, fond, police); leave it out "
                "when the charter sets none";
        }
        return false;
    }
    if (props != nullptr) {
        for (const auto& [role, value] : props->AsObject()) {
            const auto found =
                std::find_if(MotionKitRoles().begin(), MotionKitRoles().end(),
                             [&name = role](const MotionKitRole& known) {
                                 return name == known.name;
                             });
            if (found == MotionKitRoles().end()) {
                std::string roles;
                for (const MotionKitRole& known : MotionKitRoles()) {
                    roles +=
                        (roles.empty() ? "" : ", ") + std::string(known.name);
                }
                error = "'props.";
                error += role;
                error += "' is not a prop pieces share; a kit sets ";
                error += roles;
                return false;
            }
            MotionColor color;
            if (!value.IsString() || value.AsString().empty() ||
                (std::string(found->type) == "color" &&
                 !ParseMotionColor(value.AsString(), color))) {
                error = "'props." + role + "' must be " +
                        (std::string(found->type) == "color"
                             ? "a color the motion format reads (#rrggbb...)"
                             : "a non-empty text");
                return false;
            }
        }
        kit.props = *props;
    } else {
        kit.props = Value::MakeObject();
    }
    if (!brand) {
        return true;
    }
    if (!ReadTextList(file, "palette", kit.palette, error) ||
        !ReadTextList(file, "fonts", kit.fonts, error) ||
        !ReadTextList(file, "sources", kit.sources, error)) {
        return false;
    }
    for (size_t index = 0; index < kit.palette.size(); ++index) {
        if (!IsHexColor(kit.palette[index])) {
            error = "'palette[" + std::to_string(index) +
                    "]' must be \"#rrggbb\": a palette lists colours, the "
                    "opacity is the film's";
            return false;
        }
    }
    if (const Value* captions = file.Find("captions")) {
        CaptionStyle base;
        std::vector<std::string> set;
        if (!ApplyCharterCaptionSettings(*captions, "captions", true, base, set,
                                         error)) {
            return false;
        }
        if (const Value* formats = captions->Find("formats")) {
            if (!formats->IsObject() || formats->AsObject().empty()) {
                error =
                    "'captions.formats' must be an object: by name, a width, "
                    "a height and the settings that differ at that size";
                return false;
            }
            std::vector<std::pair<int32_t, int32_t>> sizes;
            for (const auto& [formatName, entry] : formats->AsObject()) {
                const std::string path = "captions.formats." + formatName;
                int32_t width = 0;
                int32_t height = 0;
                if (!ReadCaptionFormatSize(entry, path, width, height, error)) {
                    return false;
                }
                if (std::find(sizes.begin(), sizes.end(),
                              std::make_pair(width, height)) != sizes.end()) {
                    error = "'" + path + "' repeats the size " +
                            std::to_string(width) + "x" +
                            std::to_string(height) + " of another format";
                    return false;
                }
                sizes.emplace_back(width, height);
                CaptionStyle sized = base;
                std::vector<std::string> sizedSet;
                if (!ApplyCharterCaptionSettings(entry, path, false, sized,
                                                 sizedSet, error)) {
                    return false;
                }
            }
        }
        if (set.empty()) {
            error =
                "'captions' sets nothing: give the caption style's settings, "
                "or leave it out";
            return false;
        }
        kit.captions = *captions;
    }
    // M5b -- by piece, the values of its own props. The piece is not at
    // hand here: a value is checked for shape now, and against the prop it
    // names when the piece is added (ApplyMotionKit). A shared role keeps
    // its type here too, so a colour that does not read is refused on
    // reading the charter, like one of `props`.
    if (const Value* pieces = file.Find("pieces")) {
        if (!pieces->IsObject() || pieces->AsObject().empty()) {
            error =
                "'pieces' must be an object: by piece name, the values the "
                "charter gives that piece's own props";
            return false;
        }
        // One value of a piece, for every format or for one (DEC62): the
        // same shape either way.
        const auto takesValue = [&error](const std::string& at,
                                         const std::string& prop,
                                         const Value& value) {
            if (!(value.IsString() || value.IsNumber() || value.IsBool())) {
                error = "'" + at +
                        "' must be a string, a number or a boolean: the "
                        "value a prop of that piece takes";
                return false;
            }
            if (value.IsString() && value.AsString().empty()) {
                error = "'" + at + "' is empty";
                return false;
            }
            const auto role =
                std::find_if(MotionKitRoles().begin(), MotionKitRoles().end(),
                             [&name = prop](const MotionKitRole& known) {
                                 return name == known.name;
                             });
            MotionColor color;
            if (role != MotionKitRoles().end() &&
                (!value.IsString() ||
                 (std::string(role->type) == "color" &&
                  !ParseMotionColor(value.AsString(), color)))) {
                error = "'" + at + "' must be ";
                error += std::string(role->type) == "color"
                             ? "a color the motion format reads "
                               "(#rrggbb...)"
                             : "a non-empty text";
                error += ", as the shared prop '";
                error += prop;
                error += "' is";
                return false;
            }
            return true;
        };
        for (const auto& [pieceName, values] : pieces->AsObject()) {
            const std::string path = "pieces." + pieceName;
            if (!IsMotionPieceName(pieceName)) {
                error = "'" + path +
                        "' is not a piece name (letters, digits, '-' and "
                        "'_', 64 at most)";
                return false;
            }
            if (!values.IsObject() || values.AsObject().empty()) {
                error = "'" + path +
                        "' must be an object of prop values, at least one";
                return false;
            }
            for (const auto& [prop, value] : values.AsObject()) {
                if (prop != "formats") {
                    if (!takesValue(path + "." + prop, prop, value)) {
                        return false;
                    }
                    continue;
                }
                // DEC62/CH2 -- by format of the piece, the values for that
                // format only. Whether the piece has the format is known
                // when it is added (ApplyMotionKit).
                const std::string formatsPath = path + ".formats";
                if (!value.IsObject() || value.AsObject().empty()) {
                    error = "'" + formatsPath +
                            "' must be an object: by the piece's name for a "
                            "format (\"9x16\"), the values for that format "
                            "only";
                    return false;
                }
                for (const auto& [formatName, formatValues] :
                     value.AsObject()) {
                    const std::string formatPath =
                        formatsPath + "." + formatName;
                    if (!IsMotionPieceName(formatName)) {
                        error = "'" + formatPath +
                                "' is not a format name (letters, digits, "
                                "'-' and '_', 64 at most)";
                        return false;
                    }
                    if (!formatValues.IsObject() ||
                        formatValues.AsObject().empty()) {
                        error = "'" + formatPath +
                                "' must be an object of prop values, at "
                                "least one";
                        return false;
                    }
                    for (const auto& [formatProp, formatValue] :
                         formatValues.AsObject()) {
                        if (!takesValue(formatPath + "." + formatProp,
                                        formatProp, formatValue)) {
                            return false;
                        }
                    }
                }
            }
        }
        kit.pieces = *pieces;
    }
    // M5b -- the pieces the charter does not offer, each with its reason:
    // a verdict of whoever judged them, kept with the brand rather than in
    // the shipped piece, which other brands still use.
    if (const Value* withdrawn = file.Find("withdrawn")) {
        if (!withdrawn->IsObject() || withdrawn->AsObject().empty()) {
            error =
                "'withdrawn' must be an object: by piece name, why the "
                "charter does not offer that piece";
            return false;
        }
        for (const auto& [pieceName, reason] : withdrawn->AsObject()) {
            const std::string path = "withdrawn." + pieceName;
            if (!IsMotionPieceName(pieceName)) {
                error = "'" + path +
                        "' is not a piece name (letters, digits, '-' and "
                        "'_', 64 at most)";
                return false;
            }
            if (!reason.IsString() || reason.AsString().empty()) {
                error = "'" + path +
                        "' must say why, in a non-empty text: who decided "
                        "it, and when";
                return false;
            }
            if (kit.pieces.IsObject() &&
                kit.pieces.Find(pieceName) != nullptr) {
                error = "the piece '" + pieceName +
                        "' is both withdrawn and given values in 'pieces'";
                return false;
            }
        }
        kit.withdrawn = *withdrawn;
    }
    if (kit.props.AsObject().empty() && kit.palette.empty() &&
        kit.fonts.empty() && kit.captions.IsNull() && kit.pieces.IsNull() &&
        kit.withdrawn.IsNull()) {
        error =
            "a charter sets something: props, palette, fonts, captions, "
            "pieces or withdrawn";
        return false;
    }
    return true;
}

std::string MotionColorKey(const std::string& text) {
    MotionColor color;
    if (!ParseMotionColor(text, color) || color.alpha <= 0.0) {
        return std::string();
    }
    const auto channel = [](double value) {
        const long scaled = std::lround(std::clamp(value, 0.0, 1.0) * 255.0);
        static const char kDigits[] = "0123456789abcdef";
        return std::string{kDigits[(scaled >> 4) & 15], kDigits[scaled & 15]};
    };
    return "#" + channel(color.red) + channel(color.green) +
           channel(color.blue);
}

std::vector<std::string> MotionKitColors(const MotionKitInfo& kit) {
    std::vector<std::string> colors;
    if (kit.props.IsObject()) {
        for (const MotionKitRole& role : MotionKitRoles()) {
            const Value* value = kit.props.Find(role.name);
            if (std::string(role.type) == "color" && value != nullptr &&
                value->IsString()) {
                AddUnique(colors, MotionColorKey(value->AsString()));
            }
        }
    }
    for (const std::string& color : kit.palette) {
        AddUnique(colors, MotionColorKey(color));
    }
    std::vector<std::string> captionColors;
    CollectCaptionColors(kit.captions, captionColors);
    for (const std::string& color : captionColors) {
        AddUnique(colors, MotionColorKey(color));
    }
    // M5b -- a piece's own value that reads as a colour is one the charter
    // allows: the piece's prop type is not known here, the file is.
    // DEC62/CH2 -- and a value for one format of the piece, read where the
    // file has its `formats`.
    const auto collectValues = [&colors](const Value& values) {
        for (const auto& entry : values.AsObject()) {
            if (entry.second.IsString()) {
                AddUnique(colors, MotionColorKey(entry.second.AsString()));
            }
        }
    };
    if (kit.pieces.IsObject()) {
        for (const auto& piece : kit.pieces.AsObject()) {
            if (!piece.second.IsObject()) {
                continue;
            }
            for (const auto& entry : piece.second.AsObject()) {
                if (entry.first == "formats" && entry.second.IsObject()) {
                    for (const auto& format : entry.second.AsObject()) {
                        if (format.second.IsObject()) {
                            collectValues(format.second);
                        }
                    }
                } else if (entry.second.IsString()) {
                    AddUnique(colors, MotionColorKey(entry.second.AsString()));
                }
            }
        }
    }
    return colors;
}

std::vector<std::string> MotionKitFonts(const MotionKitInfo& kit) {
    std::vector<std::string> fonts;
    if (kit.props.IsObject()) {
        if (const Value* police = kit.props.Find("police");
            police != nullptr && police->IsString()) {
            AddUnique(fonts, police->AsString());
        }
    }
    for (const std::string& font : kit.fonts) {
        AddUnique(fonts, font);
    }
    if (kit.captions.IsObject()) {
        const auto collect = [&](const Value& settings) {
            const Value* family = settings.Find("font_family");
            if (family != nullptr && family->IsString()) {
                AddUnique(fonts, family->AsString());
            }
        };
        collect(kit.captions);
        if (const Value* formats = kit.captions.Find("formats");
            formats != nullptr && formats->IsObject()) {
            for (const auto& entry : formats->AsObject()) {
                collect(entry.second);
            }
        }
    }
    // M5b -- the `police` a charter gives one piece; DEC62/CH2 -- and the
    // one it gives each format of it.
    const auto collectPolice = [&fonts](const Value& values) {
        const Value* police =
            values.IsObject() ? values.Find("police") : nullptr;
        if (police != nullptr && police->IsString()) {
            AddUnique(fonts, police->AsString());
        }
    };
    if (kit.pieces.IsObject()) {
        for (const auto& piece : kit.pieces.AsObject()) {
            if (!piece.second.IsObject()) {
                continue;
            }
            // In the file's order, as the colours are.
            for (const auto& entry : piece.second.AsObject()) {
                if (entry.first == "police" && entry.second.IsString()) {
                    AddUnique(fonts, entry.second.AsString());
                } else if (entry.first == "formats" &&
                           entry.second.IsObject()) {
                    for (const auto& format : entry.second.AsObject()) {
                        collectPolice(format.second);
                    }
                }
            }
        }
    }
    return fonts;
}

bool MotionKitCaptionStyle(const MotionKitInfo& kit, int32_t width,
                           int32_t height, CaptionStyle& style,
                           std::string& format, std::vector<std::string>& set,
                           std::string& error) {
    format.clear();
    set.clear();
    if (!kit.captions.IsObject()) {
        error = "kit '" + kit.name +
                "' says nothing of captions: its file has no 'captions'";
        return false;
    }
    CaptionStyle built;
    built.id = style.id;
    if (!ApplyCharterCaptionSettings(kit.captions, "captions", true, built, set,
                                     error)) {
        return false;
    }
    if (const Value* formats = kit.captions.Find("formats");
        formats != nullptr && formats->IsObject()) {
        std::string known;
        const Value* chosen = nullptr;
        for (const auto& [name, entry] : formats->AsObject()) {
            int32_t formatWidth = 0;
            int32_t formatHeight = 0;
            if (!ReadCaptionFormatSize(entry, "captions.formats." + name,
                                       formatWidth, formatHeight, error)) {
                return false;
            }
            if (formatWidth == width && formatHeight == height) {
                chosen = &entry;
                format = name;
            }
            known += (known.empty() ? "" : ", ") + std::to_string(formatWidth) +
                     "x" + std::to_string(formatHeight) + " (" + name + ")";
        }
        if (chosen == nullptr) {
            error = "kit '" + kit.name + "' has captions for " + known +
                    ", none for " + std::to_string(width) + "x" +
                    std::to_string(height) +
                    ": its sizes are pixels of those frames";
            return false;
        }
        if (!ApplyCharterCaptionSettings(*chosen, "captions.formats." + format,
                                         false, built, set, error)) {
            return false;
        }
    }
    style = built;
    return true;
}

std::string MotionKitDirectory(const std::string& library) {
    return library.empty() ? std::string()
                           : (fs::path(library) / "kits").string();
}

void ReadMotionKits(const std::string& library,
                    std::vector<MotionKitInfo>& kits,
                    std::vector<MotionLibraryIssue>& issues) {
    kits.clear();
    issues.clear();
    const std::string directory = MotionKitDirectory(library);
    if (directory.empty()) {
        return;
    }
    std::error_code failure;
    std::vector<fs::path> files;
    for (fs::directory_iterator entry(directory, failure), end;
         !failure && entry != end; entry.increment(failure)) {
        const fs::path& path = entry->path();
        const std::string filename = path.filename().string();
        std::error_code ignored;
        if (filename.empty() || filename.front() == '.' ||
            path.extension() != ".json" ||
            !fs::is_regular_file(path, ignored)) {
            continue;
        }
        files.push_back(path);
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& path : files) {
        const std::string name = path.stem().string();
        std::error_code sizeFailure;
        const uintmax_t size = fs::file_size(path, sizeFailure);
        std::string reason;
        MotionKitInfo kit;
        if (!IsMotionPieceName(name)) {
            reason = "'" + name +
                     "' is not a kit name: letters, digits, '-' and '_'";
        } else if (sizeFailure || size > kMaxPieceBytes) {
            reason = sizeFailure ? "unreadable: " + sizeFailure.message()
                                 : "larger than a kit can be";
        } else {
            std::ifstream file(path, std::ios::binary);
            const std::string json((std::istreambuf_iterator<char>(file)),
                                   std::istreambuf_iterator<char>());
            if (!file.good() && !file.eof()) {
                reason = "unreadable";
            } else if (ParseMotionKit(name, json, kit, reason)) {
                kit.path = path.string();
                kits.push_back(std::move(kit));
                continue;
            }
        }
        issues.push_back({path.string(), reason});
    }
}

namespace {

// M5b -- whether `prop`, a prop declaration of a piece, takes `value` as
// written in a charter; `why` says what it takes when it does not. Mirrors
// the prop types of the motion format (MotionComposition.cc) for the
// literals a charter writes: a list, a point or a size are not a brand's
// business.
// A JSON number as a double: its literal, as the parser kept it.
bool NumberOf(const Value& value, double& number) {
    if (!value.IsNumber()) {
        return false;
    }
    const std::string text = value.Dump();
    char* end = nullptr;
    number = std::strtod(text.c_str(), &end);
    return end != nullptr && *end == '\0';
}

bool PropTakes(const Value& prop, const Value& value, std::string& why) {
    const Value* type = prop.Find("type");
    const std::string kind =
        type != nullptr && type->IsString() ? type->AsString() : "";
    if (kind == "color") {
        MotionColor color;
        if (value.IsString() && ParseMotionColor(value.AsString(), color)) {
            return true;
        }
        why = "a color the motion format reads (#rrggbb...)";
        return false;
    }
    if (kind == "text") {
        if (value.IsString() && !value.AsString().empty()) {
            return true;
        }
        why = "a non-empty text";
        return false;
    }
    if (kind == "number") {
        double number = 0.0;
        if (NumberOf(value, number)) {
            const Value* minimum = prop.Find("min");
            const Value* maximum = prop.Find("max");
            double bound = 0.0;
            const bool belowMin = minimum != nullptr &&
                                  NumberOf(*minimum, bound) && number < bound;
            const bool aboveMax = maximum != nullptr &&
                                  NumberOf(*maximum, bound) && number > bound;
            if (!belowMin && !aboveMax) {
                return true;
            }
            why = "a number between " +
                  (minimum != nullptr ? minimum->Dump() : std::string("-")) +
                  " and " +
                  (maximum != nullptr ? maximum->Dump() : std::string("-"));
            return false;
        }
        why = "a number";
        return false;
    }
    if (kind == "bool") {
        if (value.IsBool()) {
            return true;
        }
        why = "true or false";
        return false;
    }
    if (kind == "enum") {
        const Value* values = prop.Find("values");
        std::string known;
        if (values != nullptr && values->IsArray()) {
            for (const Value& item : values->AsArray()) {
                if (value.IsString() && item.IsString() &&
                    item.AsString() == value.AsString()) {
                    return true;
                }
                known += (known.empty() ? "" : ", ") + item.Dump();
            }
        }
        why = "one of " + (known.empty() ? std::string("its values") : known);
        return false;
    }
    why = "a value of a text, number, color, bool or enum prop, and '" + kind +
          "' is none of these";
    return false;
}

}  // namespace

std::string MotionKitWithdrawal(const MotionKitInfo& kit,
                                const std::string& piece) {
    const Value* reason =
        kit.withdrawn.IsObject() ? kit.withdrawn.Find(piece) : nullptr;
    return reason != nullptr && reason->IsString() ? reason->AsString()
                                                   : std::string();
}

bool ApplyMotionKit(const MotionKitInfo& kit, const std::string& piece,
                    Value& definition, MotionKitApplication& application,
                    std::string& error) {
    application = MotionKitApplication{};
    if (const std::string why = MotionKitWithdrawal(kit, piece); !why.empty()) {
        error = "kit '" + kit.name + "' does not offer the piece '";
        error += piece;
        error += "': ";
        error += why;
        return false;
    }
    const Value* declared = definition.Find("props");
    Value props = declared != nullptr && declared->IsObject()
                      ? *declared
                      : Value::MakeObject();
    std::vector<std::pair<std::string, Value>> set;
    for (const auto& [role, value] : kit.props.AsObject()) {
        const Value* prop = props.Find(role);
        if (prop == nullptr) {
            application.not_in_piece.push_back(role);
            continue;
        }
        const auto found =
            std::find_if(MotionKitRoles().begin(), MotionKitRoles().end(),
                         [&name = role](const MotionKitRole& known) {
                             return name == known.name;
                         });
        const Value* type = prop->Find("type");
        if (found == MotionKitRoles().end() || type == nullptr ||
            !type->IsString() || type->AsString() != found->type) {
            error = "kit '";
            error += kit.name;
            error += "' gives '";
            error += role;
            error += "' a ";
            error += found != MotionKitRoles().end() ? found->type : "value";
            error += ", and the piece's prop '";
            error += role;
            error += "' is a ";
            error +=
                type != nullptr && type->IsString() ? type->AsString() : "prop";
            error += ": the piece does not follow the shared names";
            return false;
        }
        Value changed = *prop;
        changed.Set("default", value);
        props.Set(role, std::move(changed));
        set.emplace_back(role, value);
        application.applied.push_back(role);
    }
    // M5b -- the charter's values for this piece's own props, over the
    // roles: the more specific word wins. A prop the piece does not have,
    // or a value it does not take, is refused by name.
    const Value* own = kit.pieces.IsObject() ? kit.pieces.Find(piece) : nullptr;
    if (own != nullptr && own->IsObject()) {
        for (const auto& [name, value] : own->AsObject()) {
            if (name == "formats") {
                continue;  // DEC62: the values for one format, below.
            }
            const Value* prop = props.Find(name);
            const std::string where = "'pieces." + piece + "." + name + "'";
            if (prop == nullptr) {
                error = "kit '" + kit.name + "' sets " + where +
                        ", and the piece '" + piece + "' has no prop '" + name +
                        "': the charter was written for another version of "
                        "it";
                return false;
            }
            std::string why;
            if (!PropTakes(*prop, value, why)) {
                error = "kit '" + kit.name + "' gives " + where + " " +
                        value.Dump() + ", and the prop takes " + why;
                return false;
            }
            Value changed = *prop;
            changed.Set("default", value);
            props.Set(name, std::move(changed));
            set.emplace_back(name, value);
            if (std::find(application.applied.begin(),
                          application.applied.end(),
                          name) == application.applied.end()) {
                application.applied.push_back(name);
            }
        }
    }
    // DEC62/CH2 -- the charter's values for one format of the piece, by the
    // piece's own name for it, each checked against its prop like the
    // values above. They are written last, into that format alone.
    const Value* declaredFormats = definition.Find("formats");
    std::vector<
        std::pair<std::string, std::vector<std::pair<std::string, Value>>>>
        perFormat;
    const Value* ownFormats =
        own != nullptr && own->IsObject() ? own->Find("formats") : nullptr;
    if (ownFormats != nullptr && ownFormats->IsObject()) {
        for (const auto& [formatName, values] : ownFormats->AsObject()) {
            const std::string at =
                "'pieces." + piece + ".formats." + formatName;
            const Value* target =
                declaredFormats != nullptr && declaredFormats->IsObject()
                    ? declaredFormats->Find(formatName)
                    : nullptr;
            if (target == nullptr || !target->IsObject()) {
                std::string known;
                if (declaredFormats != nullptr && declaredFormats->IsObject()) {
                    for (const auto& entry : declaredFormats->AsObject()) {
                        known += (known.empty() ? "" : ", ") + entry.first;
                    }
                }
                error = "kit '" + kit.name + "' sets " + at +
                        "', and the piece '" + piece + "' has no format '" +
                        formatName + "'; it has " +
                        (known.empty() ? std::string("none") : known);
                return false;
            }
            std::vector<std::pair<std::string, Value>> formatSet;
            if (!values.IsObject()) {
                continue;  // ParseMotionKit refuses it; nothing to set.
            }
            for (const auto& [name, value] : values.AsObject()) {
                const Value* prop = props.Find(name);
                const std::string where = at + "." + name + "'";
                if (prop == nullptr) {
                    error = "kit '" + kit.name + "' sets " + where +
                            ", and the piece '" + piece + "' has no prop '" +
                            name +
                            "': the charter was written for another version "
                            "of it";
                    return false;
                }
                std::string why;
                if (!PropTakes(*prop, value, why)) {
                    error = "kit '" + kit.name + "' gives " + where + " " +
                            value.Dump() + ", and the prop takes " + why;
                    return false;
                }
                formatSet.emplace_back(name, value);
                if (std::find(application.applied.begin(),
                              application.applied.end(),
                              name) == application.applied.end()) {
                    application.applied.push_back(name);
                }
            }
            perFormat.emplace_back(formatName, std::move(formatSet));
        }
    }
    if (set.empty() && perFormat.empty()) {
        return true;
    }
    if (!set.empty()) {
        definition.Set("props", std::move(props));
    }
    // A format that sets a prop of the charter takes the kit's value too;
    // then (DEC62) a format the charter names takes its own values, its
    // `props` created when it had none.
    if (const Value* formats = definition.Find("formats");
        formats != nullptr && formats->IsObject()) {
        Value rewritten = Value::MakeObject();
        for (const auto& [name, entry] : formats->AsObject()) {
            Value format = entry;
            const auto mine =
                std::find_if(perFormat.begin(), perFormat.end(),
                             [&formatName = name](const auto& candidate) {
                                 return candidate.first == formatName;
                             });
            const Value* formatProps = entry.Find("props");
            if ((formatProps != nullptr && formatProps->IsObject()) ||
                mine != perFormat.end()) {
                Value values = formatProps != nullptr && formatProps->IsObject()
                                   ? *formatProps
                                   : Value::MakeObject();
                for (const auto& [role, value] : set) {
                    if (values.Find(role) != nullptr) {
                        values.Set(role, value);
                    }
                }
                if (mine != perFormat.end()) {
                    for (const auto& [prop, value] : mine->second) {
                        values.Set(prop, value);
                    }
                }
                format.Set("props", std::move(values));
            }
            rewritten.Set(name, std::move(format));
        }
        definition.Set("formats", std::move(rewritten));
    }
    return true;
}

bool MotionPieceDefinition(const Value& stored, Value& definition,
                           std::string& error) {
    const Value* root = stored.Find("root");
    std::vector<std::string> bound;
    if (root != nullptr) {
        CollectProjectBound(*root, bound);
    }
    if (!bound.empty()) {
        error =
            "the composition holds something of this project, which a "
            "piece cannot carry to another: ";
        for (size_t index = 0; index < bound.size(); ++index) {
            error += (index == 0 ? "" : ", ") + bound[index];
        }
        error +=
            ". Bind the media to a prop, or play a file by `src`; build "
            "the instance in place of another composition";
        return false;
    }
    definition = WithoutDefinitionIdentities(stored);
    int64_t width = 0;
    int64_t height = 0;
    const Value* w = definition.Find("width");
    const Value* h = definition.Find("height");
    if (w == nullptr || h == nullptr || !w->AsInt64(width) ||
        !h->AsInt64(height)) {
        return true;
    }
    const Value* formats = definition.Find("formats");
    std::string name = FormatNameOf(width, height);
    if (formats != nullptr && formats->IsObject()) {
        for (const auto& [existing, entry] : formats->AsObject()) {
            int64_t formatWidth = 0;
            int64_t formatHeight = 0;
            const Value* fw = entry.Find("width");
            const Value* fh = entry.Find("height");
            if (fw != nullptr && fh != nullptr && fw->AsInt64(formatWidth) &&
                fh->AsInt64(formatHeight) && formatWidth == width &&
                formatHeight == height) {
                return true;
            }
        }
        // The usual name already names another size here: say the size.
        if (formats->Find(name) != nullptr) {
            name = std::to_string(width) + "x" + std::to_string(height);
        }
        while (formats->Find(name) != nullptr) {
            name += "-base";
        }
    }
    Value size = Value::MakeObject();
    size.Set("width", *w);
    size.Set("height", *h);
    Value withFormat;
    if (MotionDefinitionWithSetEntry(definition, "formats", name,
                                     std::optional<Value>(std::move(size)),
                                     withFormat)) {
        definition = std::move(withFormat);
    }
    return true;
}

bool WriteMotionPiece(const std::string& directory,
                      const MotionTemplateInfo& piece, bool overwrite,
                      std::string& path, bool& existed, std::string& error) {
    existed = false;
    if (directory.empty()) {
        error =
            "no motion library on this machine: set "
            "CUTMACHINE_MOTION_LIBRARY (" +
            local_env::LocalEnvFilePath() + ")";
        return false;
    }
    if (!IsMotionPieceName(piece.name)) {
        error = "'" + piece.name +
                "' is not a piece name: letters, digits, '-' and '_', 64 at "
                "most";
        return false;
    }
    std::error_code failure;
    fs::create_directories(directory, failure);
    if (failure) {
        error = "unable to create " + directory + ": " + failure.message();
        return false;
    }
    const fs::path target = fs::path(directory) / (piece.name + ".json");
    path = target.string();
    existed = fs::exists(target, failure);
    if (existed && !overwrite) {
        error = "the library already holds '" + piece.name + "' (" + path +
                "): pass overwrite to replace it, or save under another name";
        return false;
    }
    const fs::path temporary =
        fs::path(directory) /
        ("." + piece.name + ".json.tmp-" + std::to_string(::getpid()));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << DumpMotionPiece(piece);
        file.flush();
        if (!file.good()) {
            fs::remove(temporary, failure);
            error = "unable to write " + path;
            return false;
        }
    }
    fs::rename(temporary, target, failure);
    if (failure) {
        const std::string reason = failure.message();
        fs::remove(temporary, failure);
        error = "unable to write " + path + ": " + reason;
        return false;
    }
    return true;
}

bool InstantiateMotionTemplate(const Value& definition, int32_t width,
                               int32_t height, Value& instance,
                               std::string& format, std::string& error,
                               bool keepFormats) {
    instance = Value();
    format.clear();
    const Value* formats = definition.Find("formats");
    if (formats == nullptr || !formats->IsObject() ||
        formats->AsObject().empty()) {
        error = "the template declares no format";
        return false;
    }
    const Value* chosen = nullptr;
    std::string known;
    for (const auto& [name, entry] : formats->AsObject()) {
        int64_t formatWidth = 0;
        int64_t formatHeight = 0;
        const Value* w = entry.Find("width");
        const Value* h = entry.Find("height");
        if (w == nullptr || h == nullptr || !w->AsInt64(formatWidth) ||
            !h->AsInt64(formatHeight)) {
            continue;
        }
        known += (known.empty() ? "" : ", ") + name + " " +
                 std::to_string(formatWidth) + "x" +
                 std::to_string(formatHeight);
        if (formatWidth == width && formatHeight == height) {
            chosen = &entry;
            format = name;
        }
    }
    if (chosen == nullptr) {
        error = "the template has no format for a " + std::to_string(width) +
                "x" + std::to_string(height) + " timeline; it knows " + known +
                ": build a composition of that size with "
                "add_motion_composition instead";
        return false;
    }
    // MB1 -- the other formats, kept on demand. The chosen format's values
    // become the defaults below, so a format that left one of those props
    // to the template's default now has to say that default itself: each
    // format then resolves to the values it resolved to in the piece.
    Value rebased = Value::MakeObject();
    if (keepFormats) {
        MotionComposition original;
        MotionError motion;
        if (!ParseMotionComposition(definition, original, motion)) {
            error = "the template does not parse: " + motion.code + " at " +
                    motion.path + ": " + motion.message;
            return false;
        }
        const Value* chosenProps = chosen->Find("props");
        for (const auto& [name, entry] : formats->AsObject()) {
            Value kept = Value::MakeObject();
            for (const char* key : {"width", "height"}) {
                if (const Value* size = entry.Find(key)) {
                    kept.Set(key, *size);
                }
            }
            if (&entry != chosen) {
                const Value* own = entry.Find("props");
                Value props = own != nullptr && own->IsObject()
                                  ? *own
                                  : Value::MakeObject();
                if (chosenProps != nullptr && chosenProps->IsObject()) {
                    for (const auto& [prop, ignored] :
                         chosenProps->AsObject()) {
                        if (props.Find(prop) != nullptr) {
                            continue;
                        }
                        for (const MotionPropDefinition& declared :
                             original.props) {
                            if (declared.name == prop) {
                                props.Set(prop, declared.default_value);
                            }
                        }
                    }
                }
                if (!props.AsObject().empty()) {
                    kept.Set("props", std::move(props));
                }
            }
            rebased.Set(name, std::move(kept));
        }
    }
    // The instance: the template at the format's size, with the format's
    // values as the props' defaults, and without its formats unless they
    // are kept.
    instance = Value::MakeObject();
    for (const auto& [key, value] : definition.AsObject()) {
        if (key == "formats") {
            if (keepFormats) {
                instance.Set("formats", rebased);
            }
            continue;
        }
        if (key == "width") {
            instance.Set("width", *chosen->Find("width"));
            continue;
        }
        if (key == "height") {
            instance.Set("height", *chosen->Find("height"));
            continue;
        }
        if (key == "props") {
            Value props = value;
            if (const Value* overrides = chosen->Find("props")) {
                for (const auto& [prop, overrideValue] :
                     overrides->AsObject()) {
                    const Value* declared = props.Find(prop);
                    if (declared == nullptr || !declared->IsObject()) {
                        error = "format '" + format + "' sets prop '" + prop +
                                "', which the template does not declare";
                        return false;
                    }
                    Value entry = *declared;
                    entry.Set("default", overrideValue);
                    props.Set(prop, std::move(entry));
                }
            }
            instance.Set("props", std::move(props));
            continue;
        }
        instance.Set(key, value);
    }
    MotionComposition parsed;
    MotionError motion;
    if (!ParseMotionComposition(instance, parsed, motion)) {
        error = "the instance does not parse: " + motion.code + " at " +
                motion.path + ": " + motion.message;
        return false;
    }
    return true;
}
