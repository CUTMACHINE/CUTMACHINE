#include "ResolveGrade.h"

#include "Json.h"
#include "Ulid.h"

#include <sqlite3.h>
#include <zstd.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>

// DEC15 (GR15) -- docs/roadmap/GRADE-RESOLVE-2026-10.md. Everything below
// was read off the bodies of five copied project databases (PRJH, PRJG,
// PRJE, PRJF and the oldest one on the Mac) on 24 October 2026,
// and checked against the measurement of 20 October. What a field holds is
// written where it is used; a field this reading has not seen is never
// guessed at -- it is named in `unread`.

namespace {

// --- Protobuf, the wire format only ------------------------------------

struct WireField {
    uint32_t number = 0;
    int type = 0;
    uint64_t varint = 0;
    uint32_t fixed32 = 0;
    std::string_view bytes;
};

bool ReadVarint(std::string_view data, size_t& position, uint64_t& value) {
    value = 0;
    for (int shift = 0; shift < 64; shift += 7) {
        if (position >= data.size()) return false;
        const auto byte = static_cast<uint8_t>(data[position++]);
        if (shift == 63 && (byte & 0x7e) != 0) return false;
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if ((byte & 0x80) == 0) return true;
    }
    return false;
}

// Strict: a truncated field, field number 0, a group (wire types 3 and 4,
// which nothing measured carries) or an unknown wire type refuses the whole
// message.
bool ParseMessage(std::string_view data, std::vector<WireField>& fields,
                  std::string& error) {
    fields.clear();
    size_t position = 0;
    while (position < data.size()) {
        uint64_t tag = 0;
        if (!ReadVarint(data, position, tag) || (tag >> 3) == 0 ||
            (tag >> 3) > 0x1fffffff) {
            error = "protobuf illisible (étiquette)";
            return false;
        }
        WireField field;
        field.number = static_cast<uint32_t>(tag >> 3);
        field.type = static_cast<int>(tag & 7);
        if (field.type == 0) {
            if (!ReadVarint(data, position, field.varint)) {
                error = "protobuf illisible (entier)";
                return false;
            }
        } else if (field.type == 1) {
            if (data.size() - position < 8) {
                error = "protobuf illisible (64 bits)";
                return false;
            }
            field.bytes = data.substr(position, 8);
            position += 8;
        } else if (field.type == 2) {
            uint64_t length = 0;
            if (!ReadVarint(data, position, length) ||
                length > data.size() - position) {
                error = "protobuf illisible (longueur)";
                return false;
            }
            field.bytes = data.substr(position, static_cast<size_t>(length));
            position += static_cast<size_t>(length);
        } else if (field.type == 5) {
            if (data.size() - position < 4) {
                error = "protobuf illisible (32 bits)";
                return false;
            }
            const auto* bytes =
                reinterpret_cast<const unsigned char*>(data.data() + position);
            field.fixed32 = static_cast<uint32_t>(bytes[0]) |
                            static_cast<uint32_t>(bytes[1]) << 8 |
                            static_cast<uint32_t>(bytes[2]) << 16 |
                            static_cast<uint32_t>(bytes[3]) << 24;
            position += 4;
        } else {
            error = "protobuf illisible (type de champ " +
                    std::to_string(field.type) + ")";
            return false;
        }
        fields.push_back(field);
    }
    return true;
}

const WireField* First(const std::vector<WireField>& fields, uint32_t number,
                       int type) {
    for (const WireField& field : fields)
        if (field.number == number && field.type == type) return &field;
    return nullptr;
}

std::vector<const WireField*> All(const std::vector<WireField>& fields,
                                  uint32_t number, int type) {
    std::vector<const WireField*> found;
    for (const WireField& field : fields)
        if (field.number == number && field.type == type)
            found.push_back(&field);
    return found;
}

float AsFloat(uint32_t bits) {
    float value = 0.0f;
    static_assert(sizeof(value) == sizeof(bits));
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

std::string Hex(uint64_t value) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%08llX",
                  static_cast<unsigned long long>(value));
    return buffer;
}

// --- zstd ----------------------------------------------------------------

bool Decompress(std::string_view frame, std::string& output,
                std::string& error) {
    const size_t consumed =
        ZSTD_findFrameCompressedSize(frame.data(), frame.size());
    if (ZSTD_isError(consumed)) {
        error = std::string("trame zstd illisible : ") +
                ZSTD_getErrorName(consumed);
        return false;
    }
    if (consumed != frame.size()) {
        error = "octets après la trame zstd";
        return false;
    }
    const unsigned long long size =
        ZSTD_getFrameContentSize(frame.data(), frame.size());
    if (size == ZSTD_CONTENTSIZE_ERROR) {
        error = "trame zstd illisible";
        return false;
    }
    if (size != ZSTD_CONTENTSIZE_UNKNOWN) {
        if (size > kMaxResolveGradeBytes) {
            error = "étalonnage de " + std::to_string(size) +
                    " octets : au-delà de ce que la lecture accepte";
            return false;
        }
        output.assign(static_cast<size_t>(size), '\0');
        const size_t written = ZSTD_decompress(output.data(), output.size(),
                                               frame.data(), frame.size());
        if (ZSTD_isError(written) || written != output.size()) {
            error = std::string("trame zstd illisible : ") +
                    (ZSTD_isError(written) ? ZSTD_getErrorName(written)
                                           : "taille annoncée non tenue");
            return false;
        }
        return true;
    }
    // A frame that does not announce its size: decoded by chunks, under the
    // same ceiling.
    std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(
        ZSTD_createDCtx(), ZSTD_freeDCtx);
    if (!context) {
        error = "décodeur zstd indisponible";
        return false;
    }
    output.clear();
    ZSTD_inBuffer input{frame.data(), frame.size(), 0};
    std::string chunk(ZSTD_DStreamOutSize(), '\0');
    size_t remaining = 1;
    while (input.pos < input.size || remaining != 0) {
        ZSTD_outBuffer out{chunk.data(), chunk.size(), 0};
        remaining = ZSTD_decompressStream(context.get(), &out, &input);
        if (ZSTD_isError(remaining)) {
            error = std::string("trame zstd illisible : ") +
                    ZSTD_getErrorName(remaining);
            return false;
        }
        output.append(chunk.data(), out.pos);
        if (output.size() > kMaxResolveGradeBytes) {
            error = "étalonnage au-delà de ce que la lecture accepte";
            return false;
        }
        if (out.pos == 0 && input.pos == input.size && remaining != 0) {
            error = "trame zstd tronquée";
            return false;
        }
    }
    return true;
}

// --- What the primaries hold ---------------------------------------------

// The settings read, named after Resolve's panel. Identifiers from the
// davinci-resolve-mcp notes. What lift, gamma, gain, offset, saturation,
// the masters and lum_mix do to the picture was measured in Resolve 21.1 on
// 25 October (GR15 -- docs/roadmap/GRADE-RESOLVE-2026-10.md); the values
// stay as Resolve stores them, and the panel's scale for lift, gamma and
// offset is not known.
struct PrimaryName {
    uint32_t id;
    const char* name;
};
constexpr PrimaryName kPrimaries[] = {
    {0x06000004, "hue"},
    {0x06000005, "saturation"},
    {0x06000018, "lift.r"},
    {0x06000019, "lift.g"},
    {0x0600001a, "lift.b"},
    {0x0600001b, "lift.master"},
    {0x0600001d, "gain.r"},
    {0x0600001e, "gain.g"},
    {0x0600001f, "gain.b"},
    {0x06000020, "gain.master"},
    {0x06000022, "gamma.r"},
    {0x06000023, "gamma.g"},
    {0x06000024, "gamma.b"},
    {0x06000025, "gamma.master"},
    {0x0600007d, "offset.r"},
    {0x0600007e, "offset.g"},
    {0x0600007f, "offset.b"},
    {0x8600000b, "lum_mix"},
    {0x860000c0, "pivot"},
    {0x860000c1, "contrast"},
    {0x860000c2, "log.shadow.r"},
    {0x860000c3, "log.shadow.g"},
    {0x860000c4, "log.shadow.b"},
    {0x860000c5, "log.midtone.r"},
    {0x860000c6, "log.midtone.g"},
    {0x860000c7, "log.midtone.b"},
    {0x860000c8, "log.highlight.r"},
    {0x860000c9, "log.highlight.g"},
    {0x860000ca, "log.highlight.b"},
    {0x860000cb, "log.low_range"},
    {0x860000cc, "log.high_range"},
    {0x86000110, "highlights"},
    {0x86000111, "shadows"},
    {0x86000112, "color_boost"},
    {0x86000113, "midtone_detail"},
    {0x86000115, "temperature"},
    {0x86000116, "tint"},
};

const char* PrimaryNameOf(uint32_t id) {
    for (const PrimaryName& entry : kPrimaries)
        if (entry.id == id) return entry.name;
    return nullptr;
}

// What else the primaries carry, by family. `quiet` marks a flag that comes
// with its family's data and says nothing alone: measured, a node whose HSL
// curves were opened and left flat keeps hslCurves.commonFlag and no curve.
struct PrimaryFamily {
    uint32_t first;
    uint32_t last;
    const char* label;
    bool quiet;
};
constexpr PrimaryFamily kPrimaryFamilies[] = {
    {0x86000051, 0x8600005d, "flou ou netteté", false},
    {0x860000a0, 0x860000a0, "LUT", true},
    {0x860000a1, 0x860000a1, "LUT", false},
    {0x860000a7, 0x860000af, "mélangeur RGB", false},
    {0x860000b7, 0x860000bb, "courbes", true},
    {0x860000bc, 0x860000bc, "mélangeur RGB", false},
    {0x860000bd, 0x860000bf, "courbes custom", true},
    {0x860000cf, 0x860000cf, "courbes HSL", true},
    {0x86000103, 0x86000103, "courbes HSL", true},
    {0x86000118, 0x86000118, "courbes HSL", true},
    // Measured: a JSON text whose first key is "ShotMatchParams".
    {0x8600011b, 0x8600011b, "correspondance automatique (Shot Match)", false},
    {0x86000133, 0x86000138, "Color Warper", false},
    {0x86000202, 0x86000202, "courbes HSL", true},
    {0x86000303, 0x86000309, "roues HDR", false},
    {0x86000400, 0x86000405, "courbes HSL", false},
    {0x86000506, 0x86000509, "courbes custom", false},
    {0x86000600, 0x86000605, "Color Slice", false},
};

const PrimaryFamily* PrimaryFamilyOf(uint32_t id) {
    for (const PrimaryFamily& family : kPrimaryFamilies)
        if (id >= family.first && id <= family.last) return &family;
    return nullptr;
}

// The other correctors of a node, by type. Types 2 and 4 were seen holding
// a qualifier and a window on PRJH (GR15); the other labels are the
// format notes'. The type number travels with the label either way.
std::string CorrectorLabel(uint64_t type) {
    std::string label;
    switch (type) {
        case 2:
            label = "qualificateur";
            break;
        case 3:
        case 0x10003:
            label = "adoucissement de fenêtre";
            break;
        case 4:
        case 0x10004:
        case 0x20004:
        case 0x30004:
            label = "fenêtre";
            break;
        case 0x10012:
            label = "fenêtre en dégradé";
            break;
        case 9:
            label = "clé ou Matte Finesse";
            break;
        case 15:
            label = "effets de mouvement";
            break;
        case 18:
            label = "courbes";
            break;
        case 0x10001:
            label = "Magic Mask";
            break;
        case 0x20001:
            label = "Object Mask";
            break;
        default:
            label = "réglage";
            break;
    }
    return label + " (correcteur " + std::to_string(type) + ")";
}

// A setting a corrector holds at rest. Every node carries one corrector of
// each of types 2, 3, 4, 5, 6 and 18 even when untouched: with an
// identifier whose high bit is set, and either a flag at 0 or a 3x3 matrix
// (field 10), the scale between the clip and the frame for whatever that
// corrector may draw -- it changes with the clip's shape, never with the
// grade. Anything else (an identifier below 0x80000000, a flag that is not
// 0, a float) is a setting.
bool AtRest(uint64_t id, std::string_view value) {
    if (id < 0x80000000u) return false;
    std::vector<WireField> fields;
    std::string ignored;
    if (!ParseMessage(value, fields, ignored) || fields.size() != 1)
        return false;
    const WireField& only = fields[0];
    return (only.number == 2 && only.type == 0 && only.varint == 0) ||
           (only.number == 10 && only.type == 2);
}

void AddUnread(ResolveGrade& grade, int64_t node, const std::string& what) {
    for (const ResolveGradeUnread& entry : grade.unread)
        if (entry.node == node && entry.what == what) return;
    grade.unread.push_back({node, what});
}

// One corrector's settings block: field 6, its parameters in 6.2.3, each an
// identifier (1) and a value (2). More than one block is a keyframed
// corrector (6.1 is the frame of each); 6.2.4 holds per-frame data, measured
// on tracked windows.
struct CorrectorBlock {
    bool animated = false;
    size_t keyframes = 0;
    std::vector<std::pair<uint64_t, std::string_view>> parameters;
};

bool ReadCorrectorBlock(const std::vector<WireField>& corrector,
                        CorrectorBlock& block, std::string& error) {
    const std::vector<const WireField*> blocks = All(corrector, 6, 2);
    block = CorrectorBlock{};
    block.keyframes = blocks.size();
    if (blocks.empty()) return true;
    if (blocks.size() > 1) {
        block.animated = true;
        return true;
    }
    std::vector<WireField> fields;
    if (!ParseMessage(blocks[0]->bytes, fields, error)) return false;
    const WireField* parameters = First(fields, 2, 2);
    if (parameters == nullptr) return true;
    std::vector<WireField> inner;
    if (!ParseMessage(parameters->bytes, inner, error)) return false;
    for (const WireField& field : inner)
        if (field.number == 4 && field.type == 2 && !field.bytes.empty())
            block.animated = true;
    for (const WireField* parameter : All(inner, 3, 2)) {
        std::vector<WireField> entry;
        if (!ParseMessage(parameter->bytes, entry, error)) return false;
        const WireField* id = First(entry, 1, 0);
        const WireField* value = First(entry, 2, 2);
        if (id == nullptr) {
            error = "paramètre sans identifiant";
            return false;
        }
        block.parameters.emplace_back(
            id->varint, value != nullptr ? value->bytes : std::string_view());
    }
    return true;
}

// A primary's value: field 1, a float.
bool PrimaryFloat(std::string_view value, float& result) {
    std::vector<WireField> fields;
    std::string ignored;
    if (!ParseMessage(value, fields, ignored) || fields.size() != 1 ||
        fields[0].number != 1 || fields[0].type != 5)
        return false;
    result = AsFloat(fields[0].fixed32);
    // JSON has no infinity: a stored value that is not finite is named.
    return std::isfinite(result);
}

// The text of a LUT path (field 5), when it is one.
std::string PrimaryText(std::string_view value) {
    std::vector<WireField> fields;
    std::string ignored;
    if (!ParseMessage(value, fields, ignored)) return {};
    const WireField* text = First(fields, 5, 2);
    return text != nullptr ? std::string(text->bytes) : std::string();
}

// Field 9 of a node (or of the block outside the nodes): its correctors in
// 9.1, each a type (1), an enabled flag (3) and its settings (6).
bool ReadCorrectors(std::string_view bytes, int64_t node,
                    ResolveGradeNode* settings, ResolveGrade& grade,
                    std::string& error) {
    std::vector<WireField> fields;
    if (!ParseMessage(bytes, fields, error)) return false;
    for (const WireField* entry : All(fields, 1, 2)) {
        std::vector<WireField> corrector;
        if (!ParseMessage(entry->bytes, corrector, error)) return false;
        const WireField* type = First(corrector, 1, 0);
        if (type == nullptr) {
            error = "correcteur sans type";
            return false;
        }
        const WireField* enabled = First(corrector, 3, 0);
        CorrectorBlock block;
        if (!ReadCorrectorBlock(corrector, block, error)) return false;
        const bool primary = type->varint == 1 && settings != nullptr;
        const std::string label =
            primary ? std::string("primaires") : CorrectorLabel(type->varint);
        if (block.animated) {
            AddUnread(
                grade, node,
                label + (block.keyframes > 1
                             ? " : animé, " + std::to_string(block.keyframes) +
                                   " images clés"
                             : std::string(" : animé ou suivi")));
            continue;
        }
        bool holds = false;
        for (const auto& parameter : block.parameters)
            if (primary || !AtRest(parameter.first, parameter.second))
                holds = true;
        if (!holds) continue;
        if (enabled == nullptr || enabled->varint == 0) {
            AddUnread(grade, node, label + " : correcteur désactivé");
            continue;
        }
        if (!primary) {
            AddUnread(grade, node, label);
            continue;
        }
        std::map<std::string, std::string> families;
        for (const auto& parameter : block.parameters) {
            const uint64_t id = parameter.first;
            if (const char* name =
                    id <= 0xffffffffu ? PrimaryNameOf(static_cast<uint32_t>(id))
                                      : nullptr) {
                float value = 0.0f;
                const bool twice = std::any_of(
                    settings->settings.begin(), settings->settings.end(),
                    [&](const ResolveGradeSetting& setting) {
                        return setting.name == name;
                    });
                if (twice) {
                    AddUnread(grade, node,
                              std::string(name) + " (deux primaires)");
                } else if (PrimaryFloat(parameter.second, value)) {
                    settings->settings.push_back({name, value});
                } else {
                    AddUnread(
                        grade, node,
                        std::string(name) + " (valeur d'une autre forme)");
                }
                continue;
            }
            const PrimaryFamily* family =
                id <= 0xffffffffu ? PrimaryFamilyOf(static_cast<uint32_t>(id))
                                  : nullptr;
            if (family == nullptr) {
                AddUnread(grade, node, "paramètre " + Hex(id) + " inconnu");
                continue;
            }
            std::string& detail = families[family->label];
            if (family->quiet) continue;
            detail = "*";
            if (id == 0x860000a1) {
                const std::string path = PrimaryText(parameter.second);
                if (!path.empty()) detail = path;
            }
        }
        for (const auto& family : families) {
            if (family.second.empty()) continue;
            AddUnread(grade, node,
                      family.second == "*"
                          ? family.first
                          : family.first + " « " + family.second + " »");
        }
    }
    return true;
}

// Field 10 of a node: properties, each an identifier (1) and a value (2).
// 0xC0000001 is on every node; a ResolveFX or any OFX plug-in adds
// 0xC0000049, its identifier as text ("com.blackmagicdesign.resolvefx.
// halationplugin"), and 0xC0000087, its parameters.
bool ReadNodeProperties(std::string_view bytes, int64_t node,
                        ResolveGrade& grade, std::string& error) {
    std::vector<WireField> fields;
    if (!ParseMessage(bytes, fields, error)) return false;
    for (const WireField* entry : All(fields, 1, 2)) {
        std::vector<WireField> property;
        if (!ParseMessage(entry->bytes, property, error)) return false;
        const WireField* id = First(property, 1, 0);
        if (id == nullptr || id->varint == 0xC0000001u) continue;
        if (id->varint == 0xC0000049u) {
            const WireField* value = First(property, 2, 2);
            std::string plugin =
                value != nullptr ? PrimaryText(value->bytes) : std::string();
            AddUnread(grade, node,
                      plugin.empty() ? std::string("greffon OFX")
                                     : "greffon OFX « " + plugin + " »");
            continue;
        }
        // The plug-in's context, state and parameters come with its
        // identifier; any other property is named.
        if (id->varint == 0xC000005Eu || id->varint == 0xC0000063u ||
            id->varint == 0xC0000087u || id->varint == 0xC00000D2u)
            continue;
        AddUnread(grade, node, "propriété " + Hex(id->varint) + " du nœud");
    }
    return true;
}

struct NodeRead {
    uint64_t id = 0;
    ResolveGradeNode node;
};

// One node (field 7 of the grade): an internal id (1), the number Resolve
// shows (2), its place on the canvas (4, 5), a label (6), enabled (7), its
// correctors (9), its properties (10), and the nodes of a compound node
// (11). Enabled is read as protobuf reads a flag: absent is off.
bool ReadNode(std::string_view bytes, NodeRead& read, ResolveGrade& grade,
              std::string& error) {
    std::vector<WireField> fields;
    if (!ParseMessage(bytes, fields, error)) return false;
    if (const WireField* id = First(fields, 1, 0)) read.id = id->varint;
    if (const WireField* number = First(fields, 2, 0))
        read.node.number = static_cast<int64_t>(number->varint);
    if (const WireField* label = First(fields, 6, 2))
        read.node.label = std::string(label->bytes);
    const WireField* enabled = First(fields, 7, 0);
    read.node.enabled = enabled != nullptr && enabled->varint != 0;
    const int64_t number = read.node.number;
    for (const WireField& field : fields) {
        if (field.type != 2) continue;
        if (field.number == 9) {
            if (!ReadCorrectors(field.bytes, number, &read.node, grade, error))
                return false;
        } else if (field.number == 10) {
            if (!ReadNodeProperties(field.bytes, number, grade, error))
                return false;
        } else if (field.number == 11) {
            if (!field.bytes.empty()) AddUnread(grade, number, "nœud composé");
        } else if (field.number != 6 && !field.bytes.empty()) {
            AddUnread(grade, number,
                      "champ " + std::to_string(field.number) + " du nœud");
        }
    }
    return true;
}

// The wiring (field 8 of the grade): one edge per link, from a node's
// internal id (1) to another's (3). A serial grade is a single chain
// through every node, and only then is the order of application known.
bool SerialOrder(const std::vector<NodeRead>& nodes,
                 const std::vector<std::pair<uint64_t, uint64_t>>& edges,
                 std::vector<size_t>& order) {
    order.clear();
    if (nodes.size() <= 1) {
        for (size_t index = 0; index < nodes.size(); ++index)
            order.push_back(index);
        return edges.empty();
    }
    if (edges.size() != nodes.size() - 1) return false;
    std::map<uint64_t, size_t> byId;
    for (size_t index = 0; index < nodes.size(); ++index)
        if (!byId.emplace(nodes[index].id, index).second) return false;
    std::map<uint64_t, uint64_t> next;
    std::set<uint64_t> targets;
    for (const auto& edge : edges) {
        if (!byId.count(edge.first) || !byId.count(edge.second) ||
            edge.first == edge.second ||
            !next.emplace(edge.first, edge.second).second ||
            !targets.insert(edge.second).second)
            return false;
    }
    uint64_t head = 0;
    size_t heads = 0;
    for (const NodeRead& node : nodes)
        if (!targets.count(node.id)) {
            head = node.id;
            ++heads;
        }
    if (heads != 1) return false;
    for (uint64_t current = head;;) {
        order.push_back(byId[current]);
        const auto found = next.find(current);
        if (found == next.end()) break;
        current = found->second;
        if (order.size() > nodes.size()) return false;
    }
    return order.size() == nodes.size();
}

}  // namespace

bool DecodeResolveGradeBody(const std::string& body, ResolveGrade& grade,
                            std::string& error) {
    const std::string version = grade.version;
    grade = ResolveGrade{};
    grade.version = version;
    if (body.empty()) {
        error = "corps d'étalonnage vide";
        return false;
    }
    std::string bytes;
    const auto magic = static_cast<uint8_t>(body[0]);
    if (magic == 0x81) {
        if (!Decompress(std::string_view(body).substr(1), bytes, error))
            return false;
    } else if (magic == 0x80) {
        bytes = body.substr(1);
    } else {
        char hex[8];
        std::snprintf(hex, sizeof(hex), "0x%02X", magic);
        error = std::string("en-tête d'étalonnage ") + hex +
                " inconnu (0x80 ou 0x81 attendus)";
        return false;
    }
    std::vector<WireField> root;
    if (!ParseMessage(bytes, root, error)) return false;
    const std::vector<const WireField*> grades = All(root, 1, 2);
    if (grades.size() != 1) {
        error = "corps sans étalonnage unique (champ 1)";
        return false;
    }
    // Field 3 beside the grade holds a block shaped like a node, on every
    // body measured: correctors at rest, except a type 10 on about one
    // grade in twenty, which is named.
    for (const WireField& field : root) {
        if (field.type != 2 || field.number == 1) continue;
        if (field.number == 3) {
            std::vector<WireField> block;
            if (!ParseMessage(field.bytes, block, error)) return false;
            ResolveGrade outside;
            for (const WireField* correctors : All(block, 9, 2))
                if (!ReadCorrectors(correctors->bytes, 0, nullptr, outside,
                                    error))
                    return false;
            for (const ResolveGradeUnread& unread : outside.unread)
                AddUnread(grade, 0, "hors des nœuds : " + unread.what);
        } else if (!field.bytes.empty()) {
            AddUnread(grade, 0,
                      "bloc " + std::to_string(field.number) +
                          " hors de l'étalonnage");
        }
    }
    std::vector<WireField> fields;
    if (!ParseMessage(grades[0]->bytes, fields, error)) return false;
    std::vector<NodeRead> nodes;
    std::vector<std::pair<uint64_t, uint64_t>> edges;
    for (const WireField& field : fields) {
        if (field.type != 2) continue;
        switch (field.number) {
            case 7: {
                NodeRead node;
                if (!ReadNode(field.bytes, node, grade, error)) return false;
                nodes.push_back(std::move(node));
                break;
            }
            case 8: {
                std::vector<WireField> edge;
                if (!ParseMessage(field.bytes, edge, error)) return false;
                const WireField* from = First(edge, 1, 0);
                const WireField* to = First(edge, 3, 0);
                edges.emplace_back(from != nullptr ? from->varint : 0,
                                   to != nullptr ? to->varint : 0);
                break;
            }
            // 3: the clip's and the timeline's sizes; 9 and 10: the links to
            // the grade's input and output.
            case 3:
            case 9:
            case 10:
                break;
            default:
                if (!field.bytes.empty())
                    AddUnread(grade, 0,
                              "bloc " + std::to_string(field.number) +
                                  " de l'étalonnage");
                break;
        }
    }
    std::vector<size_t> order;
    grade.serial = SerialOrder(nodes, edges, order);
    if (!grade.serial) {
        AddUnread(grade, 0,
                  "câblage des " + std::to_string(nodes.size()) +
                      " nœuds : pas une simple chaîne (nœuds parallèles, "
                      "calques ou entrée de clé), l'ordre d'application "
                      "n'est pas lu");
        order.clear();
        for (size_t index = 0; index < nodes.size(); ++index)
            order.push_back(index);
    }
    for (size_t index : order) grade.nodes.push_back(nodes[index].node);
    return true;
}

// --- The database ---------------------------------------------------------

namespace {

using Database = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using Statement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

// The copy, removed whatever the outcome.
struct TemporaryCopy {
    std::filesystem::path directory;
    ~TemporaryCopy() {
        if (directory.empty()) return;
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
};

bool CopyDatabase(const std::filesystem::path& source, TemporaryCopy& copy,
                  std::filesystem::path& copied, std::string& error) {
    std::error_code code;
    if (!std::filesystem::is_regular_file(source, code)) {
        error = "base Resolve introuvable : " + source.string();
        return false;
    }
    copy.directory = std::filesystem::temp_directory_path(code);
    if (code) {
        error = "pas de dossier temporaire : " + code.message();
        return false;
    }
    copy.directory /= "cutmachine-resolve-" + GenerateUlid();
    if (!std::filesystem::create_directory(copy.directory, code)) {
        error = "copie de la base impossible : " + code.message();
        copy.directory.clear();
        return false;
    }
    copied = copy.directory / "Project.db";
    // The journal of a database in WAL mode holds what was written since
    // its last checkpoint: without it the copy would be older than the
    // database.
    for (const char* suffix : {"", "-wal", "-shm"}) {
        const std::filesystem::path from = source.string() + suffix;
        if (*suffix != '\0' && !std::filesystem::exists(from, code)) continue;
        std::filesystem::copy_file(
            from, std::filesystem::path(copied.string() + suffix), code);
        if (code) {
            error = "copie de la base impossible (" + from.filename().string() +
                    ") : " + code.message();
            return false;
        }
    }
    return true;
}

bool Prepare(sqlite3* db, const char* sql, Statement& statement,
             std::string& error) {
    sqlite3_stmt* raw = nullptr;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, nullptr) != SQLITE_OK) {
        error =
            std::string("base Resolve non reconnue : ") + sqlite3_errmsg(db);
        if (raw != nullptr) sqlite3_finalize(raw);
        return false;
    }
    statement.reset(raw);
    return true;
}

std::string Text(sqlite3_stmt* statement, int column) {
    const unsigned char* text = sqlite3_column_text(statement, column);
    return text != nullptr ? reinterpret_cast<const char*>(text)
                           : std::string();
}

std::string Blob(sqlite3_stmt* statement, int column) {
    const void* data = sqlite3_column_blob(statement, column);
    const int size = sqlite3_column_bytes(statement, column);
    return data != nullptr && size > 0
               ? std::string(static_cast<const char*>(data),
                             static_cast<size_t>(size))
               : std::string();
}

// Start and Duration are text columns holding whole frames ("90000").
bool WholeFrames(const std::string& text, int64_t& value) {
    if (text.empty()) return false;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

// The active version of a grade: decoded when it has a body, its
// correction flag kept.
void ReadActiveVersion(const std::string& name, bool correction,
                       const std::string& body, bool& graded,
                       ResolveGrade& grade) {
    grade.version = name;
    if (body.empty()) {
        graded = correction;
        if (correction)
            AddUnread(grade, 0, "version étalonnée sans corps lisible");
        return;
    }
    std::string why;
    if (!DecodeResolveGradeBody(body, grade, why)) {
        grade.nodes.clear();
        AddUnread(grade, 0, "étalonnage illisible : " + why);
        graded = true;
        return;
    }
    graded = correction || !grade.unread.empty();
}

// Timelines named `name`, and their sequence.
constexpr char kTimelines[] = R"SQL(
select tl.Sm2Timeline_id, tl.Sequence
from Sm2Timeline tl
where tl.Name = ?1
order by tl.Sm2Timeline_id
)SQL";

// The timeline's own grade: the version table of its sequence that belongs
// to no item. One per timeline on every database measured.
constexpr char kTimelineGrade[] = R"SQL(
select v.Name, v.HasCorrection, v.Body
from "ListMgt::LmVersionTable" t
left join "ListMgt::LmVersion" v on v."ListMgt::LmVersion_id" = t.pActive
where t.Sm2Sequence_id = ?1 and t.Sm2TiItem_id is null
)SQL";

// Every item of the sequence's video tracks (VideoTrackVec, V1 first), with
// the active version of its local grade, whether its remote version is
// graded, and its group and whether the group is graded.
constexpr char kItems[] = R"SQL(
select j.DbIndex, it.Name, it.Start, it.Duration, it.MediaFilePath,
       v.Name, v.HasCorrection, v.Body,
       (select rv.HasCorrection
          from "ListMgt::LmVersionTable" r
          join "ListMgt::LmVersion" rv on rv."ListMgt::LmVersion_id" = r.pActive
         where r."ListMgt::LmVersionTable_id" = t.pRemoteTable),
       g.Name,
       (select max(gv.HasCorrection)
          from "ListMgt::LmVersionTable" gt
          join "ListMgt::LmVersion" gv
            on gv."ListMgt::LmVersionTable_id" = gt."ListMgt::LmVersionTable_id"
         where gt.Sm2Group_id = t.LinkedGroup)
from Sm2SequenceContainer sc
join Sm2SequenceContainer_Sm2TiTrack j
  on j.DbOwner = sc.Sm2SequenceContainer_id and j.DbPropertyName = 'VideoTrackVec'
join Sm2TiItem it on it.Sm2TiTrack_id = j.DbAssociate
left join "ListMgt::LmVersionTable" t on t.Sm2TiItem_id = it.Sm2TiItem_id
left join "ListMgt::LmVersion" v on v."ListMgt::LmVersion_id" = t.pActive
left join Sm2Group g on g.Sm2Group_id = t.LinkedGroup
where sc.Sm2Sequence_id = ?1
order by j.DbIndex, it.Sm2TiItem_id
)SQL";

}  // namespace

ResolveProjectSettings ParseResolveProjectSettings(const std::string& blob) {
    ResolveProjectSettings settings;
    // The field's name in UTF-16, then a 32-bit type, one byte, its size, a
    // 1, the length of its data, and the data: 0x81 then zstd, or 0x80.
    const std::string name("\0S\0e\0t\0u\0p\0B\0A", 14);
    const size_t at = blob.find(name);
    const auto word = [&](size_t position) {
        uint32_t value = 0;
        for (size_t i = 0; i < 4; ++i)
            value = (value << 8) | static_cast<uint8_t>(blob[position + i]);
        return value;
    };
    if (at == std::string::npos || blob.size() < at + name.size() + 17) {
        settings.unread = "configuration du projet sans réglages (SetupBA)";
        return settings;
    }
    const size_t start = at + name.size() + 17;
    const uint32_t length = word(at + name.size() + 13);
    if (length < 1 || blob.size() - start < length) {
        settings.unread = "réglages du projet tronqués";
        return settings;
    }
    const std::string_view data(blob.data() + start, length);
    std::string message;
    std::string error;
    if (static_cast<uint8_t>(data[0]) == 0x81) {
        if (!Decompress(data.substr(1), message, error)) {
            settings.unread = "réglages du projet illisibles : " + error;
            return settings;
        }
    } else if (static_cast<uint8_t>(data[0]) == 0x80) {
        message.assign(data.substr(1));
    } else {
        settings.unread = "réglages du projet d'un format inconnu";
        return settings;
    }
    std::vector<WireField> fields;
    if (!ParseMessage(message, fields, error)) {
        settings.unread = "réglages du projet illisibles : " + error;
        return settings;
    }
    for (const WireField& field : fields) {
        if (field.number == 209 && field.type == 0)
            settings.color_space_aware = field.varint != 0;
        if (field.number == 7 && field.type == 0) {
            switch (field.varint) {
                case 2:
                    settings.color_science = "DaVinci YRGB Color Managed v1";
                    break;
                case 6:
                    settings.color_science = "ACEScc";
                    break;
                case 8:
                    settings.color_science = "ACEScct";
                    break;
                case 10:
                    settings.color_science = "DaVinci YRGB Color Managed";
                    break;
                default:
                    settings.color_science =
                        "valeur " + std::to_string(field.varint);
            }
        }
        if (field.number == 202 && field.type == 0) {
            switch (field.varint) {
                case 0:
                    settings.working_luminance = "SDR 100";
                    break;
                case 6:
                    settings.working_luminance = "HDR 2000";
                    break;
                case 8:
                    settings.working_luminance = "HDR 4000";
                    break;
                default:
                    settings.working_luminance =
                        "valeur " + std::to_string(field.varint);
            }
        }
        if (field.number == 36 && field.type == 2 && field.bytes.size() >= 8) {
            std::vector<WireField> inner;
            if (!ParseMessage(field.bytes.substr(8), inner, error)) {
                settings.unread = "réglages du contraste illisibles : " + error;
                return settings;
            }
            for (const WireField& setting : inner)
                if (setting.number == 19 && setting.type == 0)
                    settings.contrast_s_curve = setting.varint != 0;
        }
    }
    settings.read = true;
    settings.unread.clear();
    return settings;
}

bool ResolveScienceRendersPrimaries(const std::string& science) {
    return ResolveScienceRendersBalance(science) || science == "ACEScct";
}

bool ResolveScienceRendersBalance(const std::string& science) {
    return science == "DaVinci YRGB" || science == "DaVinci YRGB Color Managed";
}

bool ReadResolveDatabaseTimelines(
    const std::string& databasePath, const std::string& timelineName,
    std::vector<ResolveDatabaseTimeline>& timelines, std::string& error,
    ResolveProjectSettings* settings) {
    timelines.clear();
    TemporaryCopy copy;
    std::filesystem::path copied;
    if (!CopyDatabase(databasePath, copy, copied, error)) return false;

    sqlite3* raw = nullptr;
    const int opened = sqlite3_open_v2(copied.string().c_str(), &raw,
                                       SQLITE_OPEN_READONLY, nullptr);
    Database db(raw, sqlite3_close);
    if (opened != SQLITE_OK) {
        error = std::string("base Resolve illisible : ") +
                (raw != nullptr ? sqlite3_errmsg(raw) : "ouverture impossible");
        return false;
    }

    // GR16 -- the project's settings, from the same copy; a configuration
    // that does not read only leaves them unread.
    if (settings != nullptr) {
        *settings = ResolveProjectSettings{};
        sqlite3_stmt* raw_config = nullptr;
        if (sqlite3_prepare_v2(db.get(), "select FieldsBlob from SM_Config", -1,
                               &raw_config, nullptr) == SQLITE_OK) {
            Statement config(raw_config, sqlite3_finalize);
            if (sqlite3_step(config.get()) == SQLITE_ROW)
                *settings = ParseResolveProjectSettings(Blob(config.get(), 0));
        } else {
            sqlite3_finalize(raw_config);
            settings->unread = "base sans configuration de projet (SM_Config)";
        }
    }

    Statement found(nullptr, sqlite3_finalize);
    if (!Prepare(db.get(), kTimelines, found, error)) return false;
    sqlite3_bind_text(found.get(), 1, timelineName.c_str(), -1,
                      SQLITE_TRANSIENT);
    std::vector<std::string> sequences;
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(found.get())) == SQLITE_ROW) {
        ResolveDatabaseTimeline timeline;
        timeline.name = timelineName;
        sequences.push_back(Text(found.get(), 1));
        timelines.push_back(std::move(timeline));
    }
    if (step != SQLITE_DONE) {
        error =
            std::string("base Resolve illisible : ") + sqlite3_errmsg(db.get());
        return false;
    }

    Statement grades(nullptr, sqlite3_finalize);
    Statement items(nullptr, sqlite3_finalize);
    if (!timelines.empty() &&
        (!Prepare(db.get(), kTimelineGrade, grades, error) ||
         !Prepare(db.get(), kItems, items, error)))
        return false;
    for (size_t index = 0; index < timelines.size(); ++index) {
        ResolveDatabaseTimeline& timeline = timelines[index];
        sqlite3_reset(grades.get());
        sqlite3_bind_text(grades.get(), 1, sequences[index].c_str(), -1,
                          SQLITE_TRANSIENT);
        size_t tables = 0;
        while ((step = sqlite3_step(grades.get())) == SQLITE_ROW) {
            if (tables++ != 0) continue;
            sqlite3_stmt* row = grades.get();
            if (sqlite3_column_type(row, 0) != SQLITE_NULL ||
                sqlite3_column_type(row, 2) != SQLITE_NULL) {
                ReadActiveVersion(Text(row, 0), sqlite3_column_int(row, 1) != 0,
                                  Blob(row, 2), timeline.graded,
                                  timeline.grade);
            }
        }
        if (step != SQLITE_DONE) {
            error = std::string("base Resolve illisible : ") +
                    sqlite3_errmsg(db.get());
            return false;
        }
        if (tables > 1) {
            AddUnread(timeline.grade, 0,
                      std::to_string(tables) +
                          " tables d'étalonnage de timeline : seule la "
                          "première est lue");
            timeline.graded = true;
        }
        // A sequence with no video track in VideoTrackVec simply has no
        // clip: a timeline of that name is then matched by nothing.
        sqlite3_reset(items.get());
        sqlite3_bind_text(items.get(), 1, sequences[index].c_str(), -1,
                          SQLITE_TRANSIENT);
        while ((step = sqlite3_step(items.get())) == SQLITE_ROW) {
            sqlite3_stmt* row = items.get();
            ResolveDatabaseClip clip;
            clip.track = sqlite3_column_int(row, 0) + 1;
            clip.name = Text(row, 1);
            clip.whole_frames = WholeFrames(Text(row, 2), clip.start) &&
                                WholeFrames(Text(row, 3), clip.duration);
            clip.path = Text(row, 4);
            if (sqlite3_column_type(row, 5) != SQLITE_NULL ||
                sqlite3_column_type(row, 7) != SQLITE_NULL) {
                ReadActiveVersion(Text(row, 5), sqlite3_column_int(row, 6) != 0,
                                  Blob(row, 7), clip.graded, clip.grade);
            }
            // Which of its two versions a clip shows is not in what this
            // reading knows of the database: a graded remote version is
            // named, never assumed away.
            if (sqlite3_column_type(row, 8) != SQLITE_NULL &&
                sqlite3_column_int(row, 8) != 0) {
                AddUnread(clip.grade, 0,
                          "version distante (remote) étalonnée elle aussi : "
                          "laquelle des deux le plan montre n'est pas lu");
                clip.graded = true;
            }
            if (sqlite3_column_type(row, 10) != SQLITE_NULL &&
                sqlite3_column_int(row, 10) != 0) {
                const std::string group = Text(row, 9);
                AddUnread(
                    clip.grade, 0,
                    "groupe" +
                        (group.empty() ? std::string() : " « " + group + " »") +
                        " étalonné : l'étalonnage de groupe n'est pas "
                        "lu");
                clip.graded = true;
            }
            timeline.clips.push_back(std::move(clip));
        }
        if (step != SQLITE_DONE) {
            error = std::string("base Resolve illisible : ") +
                    sqlite3_errmsg(db.get());
            return false;
        }
    }
    return true;
}

namespace {

std::string ShortestFloat(float value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    if (result.ec != std::errc{}) return "0";
    return std::string(buffer, result.ptr);
}

}  // namespace

std::string SerializeResolveGrade(const ResolveGrade& grade) {
    std::ostringstream output;
    output << "{\"version\":\"" << mcp_json::EscapeJsonString(grade.version)
           << "\",\"serial\":" << (grade.serial ? "true" : "false")
           << ",\"nodes\":[";
    for (size_t index = 0; index < grade.nodes.size(); ++index) {
        const ResolveGradeNode& node = grade.nodes[index];
        if (index) output << ',';
        output << "{\"node\":" << node.number << ",\"label\":\""
               << mcp_json::EscapeJsonString(node.label)
               << "\",\"enabled\":" << (node.enabled ? "true" : "false")
               << ",\"settings\":{";
        for (size_t setting = 0; setting < node.settings.size(); ++setting) {
            if (setting) output << ',';
            output << '"' << node.settings[setting].name
                   << "\":" << ShortestFloat(node.settings[setting].value);
        }
        output << "}}";
    }
    output << "],\"unread\":[";
    for (size_t index = 0; index < grade.unread.size(); ++index) {
        if (index) output << ',';
        output << "{\"node\":" << grade.unread[index].node << ",\"what\":\""
               << mcp_json::EscapeJsonString(grade.unread[index].what) << "\"}";
    }
    output << "]}";
    return output.str();
}
