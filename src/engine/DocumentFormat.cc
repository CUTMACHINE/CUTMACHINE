// FORMAT-2026-09 -- ROADMAP.md, F6. The document codec: how a document, a
// project file and the parts a project delta carries are written and read.
//
// Document.cc owns what a document *is* -- its validation and lookups; this
// file owns how it is spelled on disk, which is a compatibility contract
// with every project already saved. The field tables of DocumentFormat.h
// decide that spelling, and both directions below are derived from them.
// What remains written here is the frame around the parts: the root of a
// document and of a project file, and the migration of older versions.
//
// tests/document_format_tests.cc pins what this file may not change.

#include "DocumentFormat.h"
#include "CanonicalHash.h"
#include "DocumentJson.h"
#include "FormatVersion.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace document_format {
namespace {

// PRD20 -- ROADMAP.md. The newest versions this build reads, for the
// refusal that says so (FormatVersion.h): the last of Document.h's
// kDocumentVersionWith... constants, and the project file's own version. A
// version added to the lists below moves these with it.
constexpr int32_t kNewestDocumentVersion = kDocumentVersionWithPersonMask;
constexpr int32_t kNewestProjectVersion = 3;

// MOTION-2026-09 M3 -- the root key under which a document or a project
// file keeps its compositions: the one part of either where a number may be
// a decimal (Document.h says why).
const char kMotionRootKey[] = "compositions";

// MOTION-2026-09 M4 -- the key under which a clip keeps the prop values of
// its composition: a motion value too, at whatever depth it sits.
const char kMotionPropsKey[] = "motion_props";

struct JsonValue {
    // Decimal: a number with a fraction or an exponent, read only under
    // kMotionRootKey and kept as its text in `string`.
    enum class Type : uint8_t {
        Null,
        Boolean,
        Number,
        String,
        Array,
        Object,
        Decimal
    };
    Type type = Type::Null;
    bool boolean = false;
    int64_t number = 0;
    std::string string;
    std::vector<JsonValue> array;
    std::map<std::string, JsonValue> object;
    // MOTION-2026-09 M3 -- a value read under kMotionRootKey, as the exact
    // text it was written in: a motion tree is read back from it, with its
    // key order and its decimals, which `object` and `number` cannot hold.
    std::string raw;
};

class JsonParser {
public:
    explicit JsonParser(const std::string& input) : input_(input) {}

    JsonValue Parse() {
        SkipWhitespace();
        JsonValue value = ParseValue();
        SkipWhitespace();
        if (position_ != input_.size()) {
            Fail("unexpected trailing content");
        }
        return value;
    }

private:
    [[noreturn]] void Fail(const std::string& message) const {
        throw std::runtime_error("JSON byte " + std::to_string(position_) +
                                 ": " + message);
    }

    void SkipWhitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    char Take() {
        if (position_ >= input_.size()) {
            Fail("unexpected end of input");
        }
        return input_[position_++];
    }

    bool Consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue ParseValue() {
        const size_t start = position_;
        JsonValue value = ParseBareValue();
        if (motion_) {
            value.raw = input_.substr(start, position_ - start);
        }
        return value;
    }

    JsonValue ParseBareValue() {
        if (position_ >= input_.size()) {
            Fail("expected a value");
        }
        switch (input_[position_]) {
            case '{':
                return ParseObject();
            case '[':
                return ParseArray();
            case '"': {
                JsonValue value;
                value.type = JsonValue::Type::String;
                value.string = ParseString();
                return value;
            }
            case 't':
                return ParseLiteral("true", true);
            case 'f':
                return ParseLiteral("false", false);
            case 'n':
                return ParseNull();
            default:
                if (input_[position_] == '-' ||
                    std::isdigit(
                        static_cast<unsigned char>(input_[position_]))) {
                    return ParseNumber();
                }
                Fail("expected an object, array, string, integer or literal");
        }
    }

    JsonValue ParseObject() {
        Take();
        const bool root = depth_++ == 0;
        JsonValue value;
        value.type = JsonValue::Type::Object;
        SkipWhitespace();
        if (Consume('}')) {
            --depth_;
            return value;
        }
        while (true) {
            if (position_ >= input_.size() || input_[position_] != '"') {
                Fail("expected an object key");
            }
            std::string key = ParseString();
            SkipWhitespace();
            if (!Consume(':')) {
                Fail("expected ':' after object key");
            }
            SkipWhitespace();
            const bool motion = motion_;
            if ((root && key == kMotionRootKey) || key == kMotionPropsKey) {
                motion_ = true;
            }
            JsonValue member = ParseValue();
            motion_ = motion;
            if (!value.object.emplace(key, std::move(member)).second) {
                Fail("duplicate object key '" + key + "'");
            }
            SkipWhitespace();
            if (Consume('}')) {
                --depth_;
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or '}'");
            }
            SkipWhitespace();
        }
    }

    JsonValue ParseArray() {
        Take();
        ++depth_;
        JsonValue value;
        value.type = JsonValue::Type::Array;
        SkipWhitespace();
        if (Consume(']')) {
            --depth_;
            return value;
        }
        while (true) {
            value.array.push_back(ParseValue());
            SkipWhitespace();
            if (Consume(']')) {
                --depth_;
                return value;
            }
            if (!Consume(',')) {
                Fail("expected ',' or ']'");
            }
            SkipWhitespace();
        }
    }

    static void AppendUtf8(uint32_t codePoint, std::string& output) {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    std::string ParseString() {
        Take();
        std::string result;
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(Take());
            if (character == '"') {
                return result;
            }
            if (character < 0x20) {
                Fail("unescaped control character in string");
            }
            if (character != '\\') {
                result.push_back(static_cast<char>(character));
                continue;
            }
            const char escape = Take();
            switch (escape) {
                case '"':
                    result.push_back('"');
                    break;
                case '\\':
                    result.push_back('\\');
                    break;
                case '/':
                    result.push_back('/');
                    break;
                case 'b':
                    result.push_back('\b');
                    break;
                case 'f':
                    result.push_back('\f');
                    break;
                case 'n':
                    result.push_back('\n');
                    break;
                case 'r':
                    result.push_back('\r');
                    break;
                case 't':
                    result.push_back('\t');
                    break;
                case 'u': {
                    uint32_t codePoint = 0;
                    for (int index = 0; index < 4; ++index) {
                        const char hex = Take();
                        codePoint <<= 4;
                        if (hex >= '0' && hex <= '9')
                            codePoint |= hex - '0';
                        else if (hex >= 'a' && hex <= 'f')
                            codePoint |= hex - 'a' + 10;
                        else if (hex >= 'A' && hex <= 'F')
                            codePoint |= hex - 'A' + 10;
                        else
                            Fail("invalid Unicode escape");
                    }
                    if (codePoint >= 0xd800 && codePoint <= 0xdfff) {
                        Fail("UTF-16 surrogate escapes are not supported");
                    }
                    AppendUtf8(codePoint, result);
                    break;
                }
                default:
                    Fail("invalid string escape");
            }
        }
        Fail("unterminated string");
    }

    JsonValue ParseNumber() {
        const size_t start = position_;
        Consume('-');
        if (position_ >= input_.size()) {
            Fail("incomplete integer");
        }
        if (Consume('0')) {
            if (position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("leading zero in integer");
            }
        } else {
            if (!std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                Fail("invalid integer");
            }
            while (
                position_ < input_.size() &&
                std::isdigit(static_cast<unsigned char>(input_[position_]))) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            (input_[position_] == '.' || input_[position_] == 'e' ||
             input_[position_] == 'E')) {
            if (!motion_) {
                Fail(
                    "floating-point JSON numbers are forbidden in this "
                    "document");
            }
            return ParseDecimal(start);
        }
        const std::string text = input_.substr(start, position_ - start);
        errno = 0;
        char* end = nullptr;
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        if (errno == ERANGE || !end || *end != '\0') {
            if (motion_) {
                return Decimal(start);
            }
            Fail("integer is outside int64_t range");
        }
        JsonValue value;
        value.type = JsonValue::Type::Number;
        value.number = static_cast<int64_t>(parsed);
        return value;
    }

    // MOTION-2026-09 M3 -- the fraction and exponent of a motion number,
    // after its integer part, in the JSON grammar.
    JsonValue ParseDecimal(size_t start) {
        const auto digits = [this](const char* what) {
            const size_t first = position_;
            while (position_ < input_.size() &&
                   std::isdigit(
                       static_cast<unsigned char>(input_[position_])) != 0) {
                ++position_;
            }
            if (position_ == first) {
                Fail(what);
            }
        };
        if (Consume('.')) {
            digits("invalid fractional part");
        }
        if (Consume('e') || Consume('E')) {
            if (!Consume('+')) {
                Consume('-');
            }
            digits("invalid exponent");
        }
        return Decimal(start);
    }

    JsonValue Decimal(size_t start) {
        JsonValue value;
        value.type = JsonValue::Type::Decimal;
        value.string = input_.substr(start, position_ - start);
        return value;
    }

    JsonValue ParseLiteral(const char* literal, bool boolean) {
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        JsonValue value;
        value.type = JsonValue::Type::Boolean;
        value.boolean = boolean;
        return value;
    }

    JsonValue ParseNull() {
        const char literal[] = "null";
        for (size_t index = 0; literal[index]; ++index) {
            if (Take() != literal[index]) {
                Fail("invalid literal");
            }
        }
        return {};
    }

    const std::string& input_;
    size_t position_ = 0;
    int depth_ = 0;
    bool motion_ = false;
};

const JsonValue& Require(const JsonValue& object, const std::string& key,
                         JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) {
        throw std::runtime_error(context + " is missing '" + key + "'");
    }
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return found->second;
}

const JsonValue* Optional(const JsonValue& object, const std::string& key,
                          JsonValue::Type type, const std::string& context) {
    if (object.type != JsonValue::Type::Object) {
        throw std::runtime_error(context + " must be an object");
    }
    const auto found = object.object.find(key);
    if (found == object.object.end()) return nullptr;
    if (found->second.type != type) {
        throw std::runtime_error(context + "." + key +
                                 " has the wrong JSON type");
    }
    return &found->second;
}

int32_t Int32(const JsonValue& object, const std::string& key,
              const std::string& context) {
    const int64_t value =
        Require(object, key, JsonValue::Type::Number, context).number;
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(context + "." + key +
                                 " is outside int32_t range");
    }
    return static_cast<int32_t>(value);
}

std::string Escape(const std::string& input) {
    std::ostringstream output;
    for (const unsigned char character : input) {
        switch (character) {
            case '"':
                output << "\\\"";
                break;
            case '\\':
                output << "\\\\";
                break;
            case '\b':
                output << "\\b";
                break;
            case '\f':
                output << "\\f";
                break;
            case '\n':
                output << "\\n";
                break;
            case '\r':
                output << "\\r";
                break;
            case '\t':
                output << "\\t";
                break;
            default:
                if (character < 0x20) {
                    const char digits[] = "0123456789abcdef";
                    output << "\\u00" << digits[character >> 4]
                           << digits[character & 0xf];
                } else {
                    output << static_cast<char>(character);
                }
        }
    }
    return output.str();
}

// ---------------------------------------------------------------------
// Writing. A list written one element per line indents its elements two
// spaces deeper than the object that holds it; `depth` is that object's.
// ---------------------------------------------------------------------

template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void WriteObject(std::ostringstream& output, const T& value, int depth);
template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void WriteFields(std::ostringstream& output, const T& value, int depth,
                 bool comma);

void WriteValue(std::ostringstream& output, const std::string& value, int) {
    output << '"' << Escape(value) << '"';
}

void WriteValue(std::ostringstream& output, bool value, int) {
    output << (value ? "true" : "false");
}

void WriteValue(std::ostringstream& output, int32_t value, int) {
    output << value;
}
void WriteValue(std::ostringstream& output, int64_t value, int) {
    output << value;
}
void WriteValue(std::ostringstream& output, uint32_t value, int) {
    output << value;
}
void WriteValue(std::ostringstream& output, uint64_t value, int) {
    output << value;
}

void WriteValue(std::ostringstream& output, const RationalTime& time, int) {
    output << "{\"value\":" << time.value << ",\"rate\":" << time.rate << "}";
}

// MOTION-2026-09 M3 -- a motion tree, as the JSON it is.
void WriteValue(std::ostringstream& output, const mcp_json::Value& value, int) {
    output << value.Dump();
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void WriteValue(std::ostringstream& output, Enum value, int) {
    static_assert(EnumNamesAreDistinct<Enum>(),
                  "two enumerators share a name in the document");
    for (const auto& [enumerator, name] : EnumNames<Enum>::kNames) {
        if (enumerator != value) continue;
        output << '"' << name << '"';
        return;
    }
    throw std::logic_error("an enumerator has no name in the document");
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void WriteValue(std::ostringstream& output, const T& value, int depth) {
    WriteObject(output, value, depth + 1);
}

// Two spaces per level of nesting.
std::string Indent(int depth) {
    return std::string(static_cast<size_t>(depth) * 2, ' ');
}

void OpenLine(std::ostringstream& output, size_t index, int depth) {
    output << (index == 0 ? "\n" : ",\n") << Indent(depth + 1);
}

void CloseLines(std::ostringstream& output, bool empty, int depth,
                Layout layout, char close) {
    if (layout == Layout::LinesPaddedWhenEmpty) {
        if (!empty) output << '\n';
        output << Indent(depth);
    } else if (!empty) {
        output << '\n' << Indent(depth);
    }
    output << close;
}

template <typename T>
void WriteList(std::ostringstream& output, const std::vector<T>& values,
               int depth, Layout layout) {
    output << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (layout == Layout::Inline) {
            if (index) output << ',';
        } else {
            OpenLine(output, index, depth);
        }
        WriteValue(output, values[index], depth);
    }
    if (layout == Layout::Inline) {
        output << ']';
    } else {
        CloseLines(output, values.empty(), depth, layout, ']');
    }
}

template <typename V>
void WriteList(std::ostringstream& output,
               const std::map<std::string, V>& values, int depth,
               Layout layout) {
    output << '{';
    size_t index = 0;
    for (const auto& [key, value] : values) {
        if (layout == Layout::Inline) {
            if (index) output << ',';
        } else {
            OpenLine(output, index, depth);
        }
        ++index;
        output << '"' << Escape(key) << "\":";
        WriteValue(output, value, depth);
    }
    if (layout == Layout::Inline) {
        output << '}';
    } else {
        CloseLines(output, values.empty(), depth, layout, '}');
    }
}

// The struct as declared, so that OmittedWhenDefault compares against the
// member's in-class default rather than a guess about its type.
template <typename T>
const T& DefaultOf() {
    static const T value{};
    return value;
}

// Only the scalar members a table marks OmittedWhenDefault are compared;
// every other type reads as never default, so it is always written.
template <typename Member>
bool IsDefaultValue(const Member&, const Member&) {
    return false;
}

inline bool IsDefaultValue(const std::string& member,
                           const std::string& fallback) {
    return member == fallback;
}

inline bool IsDefaultValue(int32_t member, int32_t fallback) {
    return member == fallback;
}

// DEC31 -- docs/roadmap/BLANC-HLG-2026-10.md. The first flag a table
// omits (MediaColorInput::graphics): before it, no bool was marked, and
// every bool is still written unless its table marks it.
inline bool IsDefaultValue(bool member, bool fallback) {
    return member == fallback;
}

inline bool IsDefaultValue(const EffectParamValue& member,
                           const EffectParamValue& fallback) {
    return member.num == fallback.num && member.den == fallback.den;
}

// D6 -- docs/roadmap/DIALOGUE-2026-10.md. A time is default spelled as the
// struct declares it, value and rate, as the journal codec compares one
// (OperationFormat.h, SameValue): a zero written 0/25 is a key, and is
// written.
inline bool IsDefaultValue(const RationalTime& member,
                           const RationalTime& fallback) {
    return member.value == fallback.value && member.rate == fallback.rate;
}

// MOTION-2026-09 M4 -- a motion value's default is its absence.
inline bool IsDefaultValue(const mcp_json::Value& member,
                           const mcp_json::Value&) {
    return member.IsNull();
}

// R1 -- docs/roadmap/REEL-2026-09.md. A framing is default when every one
// of its fields is spelled as the struct declares it. Spelling and not
// value, for the reason ClipHasFraming (Document.h) gives: a scale written
// 2/2 is a key an older reader would drop, so it has to be written.
inline bool IsDefaultValue(const ClipFraming& member,
                           const ClipFraming& fallback) {
    const auto same = [](const EffectParamValue& left,
                         const EffectParamValue& right) {
        return left.num == right.num && left.den == right.den;
    };
    return member.mode == fallback.mode && same(member.scale, fallback.scale) &&
           same(member.position_x, fallback.position_x) &&
           same(member.position_y, fallback.position_y) &&
           same(member.crop_left, fallback.crop_left) &&
           same(member.crop_right, fallback.crop_right) &&
           same(member.crop_top, fallback.crop_top) &&
           same(member.crop_bottom, fallback.crop_bottom);
}

// G3 -- docs/roadmap/GRADE-2026-09.md. An input is default spelled as the
// struct declares it, for ClipFraming's reason above.
inline bool IsDefaultValue(const MediaColorInput& member,
                           const MediaColorInput& fallback) {
    return member == fallback;
}

// GN3 -- docs/roadmap/GENERATIF-2026-10.md. A provenance is default spelled
// as the struct declares it: every member empty.
inline bool IsDefaultValue(const MediaGeneration& member,
                           const MediaGeneration& fallback) {
    return member == fallback;
}

// R2 -- docs/roadmap/REEL-2026-09.md. An animation is default when there is
// none. No spelling subtlety here, unlike a framing: the list is either
// empty or it is not, and Document::Validate refuses an entry with fewer
// than two keys, so an empty entry never reaches the file.
inline bool IsDefaultValue(const std::vector<ClipParameterAnimation>& member,
                           const std::vector<ClipParameterAnimation>&) {
    return member.empty();
}

// R3, criterion 4 -- a speed ramp is default when there is none, for the
// animation's reason above.
inline bool IsDefaultValue(const std::vector<ClipKeyframe>& member,
                           const std::vector<ClipKeyframe>&) {
    return member.empty();
}

// PL1 -- docs/roadmap/PLANS-2026-10.md. A media's described shots, and a
// description's subjects and keywords, are default when there are none.
inline bool IsDefaultValue(const std::vector<ShotDescription>& member,
                           const std::vector<ShotDescription>&) {
    return member.empty();
}

inline bool IsDefaultValue(const std::vector<std::string>& member,
                           const std::vector<std::string>&) {
    return member.empty();
}

// PA8 -- docs/roadmap/PARITE-2026-11.md. A mask is default when every one
// of its fields is spelled as the struct declares it, for ClipFraming's
// reason above: a shapeless mask that places something is a key, which
// Document::Validate then refuses, rather than bytes dropped in silence.
inline bool IsDefaultValue(const ClipMask& member, const ClipMask& fallback) {
    const auto same = [](const EffectParamValue& left,
                         const EffectParamValue& right) {
        return left.num == right.num && left.den == right.den;
    };
    return member.shape == fallback.shape &&
           member.inverted == fallback.inverted &&
           same(member.center_x, fallback.center_x) &&
           same(member.center_y, fallback.center_y) &&
           same(member.width, fallback.width) &&
           same(member.height, fallback.height) &&
           same(member.feather, fallback.feather);
}

// PA4 -- docs/roadmap/PARITE-2026-11.md. A carton's word times are default
// when there are none: a clip that is not a timed carton keeps its bytes.
inline bool IsDefaultValue(const std::vector<RationalTime>& member,
                           const std::vector<RationalTime>&) {
    return member.empty();
}

// R6 -- docs/roadmap/REEL-2026-09.md. A look is default when all three of
// its strings are spelled as the struct declares them. Spelling and not
// "has a digest", for the reason HoldsSequenceLut (Document.h) gives: a
// sequence naming a space without a digest carries a key an older reader
// would drop, so it has to be written.
inline bool IsDefaultValue(const SequenceLut& member,
                           const SequenceLut& fallback) {
    return member.digest == fallback.digest && member.name == fallback.name &&
           member.space == fallback.space;
}

template <typename Field, typename T>
bool IsWritten(const Field& field, const T& value) {
    if (field.written_if && !field.written_if(value)) return false;
    if (field.presence && !(value.*field.presence)) return false;
    if (field.omitted_when_default &&
        IsDefaultValue(value.*field.member, DefaultOf<T>().*field.member))
        return false;
    if constexpr (std::is_same_v<typename Field::MemberType, std::string>) {
        if (field.omitted_when_empty && (value.*field.member).empty())
            return false;
    }
    return true;
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int>>
void WriteFields(std::ostringstream& output, const T& value, int depth,
                 bool comma) {
    CheckTable<T>();
    ForEachField<T>([&](const auto& field) {
        using Field = std::decay_t<decltype(field)>;
        if constexpr (Field::kKind == Kind::Carried) {
            return;
        } else {
            if (!IsWritten(field, value)) return;
            if (comma) output << ',';
            comma = true;
            output << '"' << field.key << "\":";
            using Member = typename Field::MemberType;
            if constexpr (IsList<Member>::value) {
                WriteList(output, value.*field.member, depth, field.layout);
            } else {
                WriteValue(output, value.*field.member, depth);
            }
        }
    });
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int>>
void WriteObject(std::ostringstream& output, const T& value, int depth) {
    output << '{';
    WriteFields(output, value, depth, false);
    output << '}';
}

// ---------------------------------------------------------------------
// Reading. `context` names the value being read, as errors spell it.
// ---------------------------------------------------------------------

struct Rules {
    // The version the file declares: a member Since a later one is not read.
    int32_t version = Document().version;
    // A delta's own records: every key required, and no other key.
    bool strict = false;
};

[[noreturn]] void WrongType(const std::string& context) {
    throw std::runtime_error(context + " has the wrong JSON type");
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void ReadObject(const JsonValue& json, T& value, const std::string& context,
                const Rules& rules);

void ReadValue(const JsonValue& json, std::string& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::String) WrongType(context);
    value = json.string;
}

void ReadValue(const JsonValue& json, bool& value, const std::string& context,
               const Rules&) {
    if (json.type != JsonValue::Type::Boolean) WrongType(context);
    value = json.boolean;
}

void ReadValue(const JsonValue& json, int64_t& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::Number) WrongType(context);
    value = json.number;
}

void ReadValue(const JsonValue& json, int32_t& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::Number) WrongType(context);
    if (json.number < std::numeric_limits<int32_t>::min() ||
        json.number > std::numeric_limits<int32_t>::max())
        throw std::runtime_error(context + " is outside int32_t range");
    value = static_cast<int32_t>(json.number);
}

void ReadValue(const JsonValue& json, uint32_t& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::Number) WrongType(context);
    if (json.number < 0)
        throw std::runtime_error(context + " cannot be negative");
    if (json.number > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(context + " is outside uint32_t range");
    value = static_cast<uint32_t>(json.number);
}

// The parser keeps integers as int64_t, so the upper half of uint64_t is
// refused there, before it could be misread.
void ReadValue(const JsonValue& json, uint64_t& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::Number) WrongType(context);
    if (json.number < 0)
        throw std::runtime_error(context + " cannot be negative");
    value = static_cast<uint64_t>(json.number);
}

// MOTION-2026-09 M3 -- read back from the text the parser kept, since the
// document's own tree has neither key order nor decimals.
void ReadValue(const JsonValue& json, mcp_json::Value& value,
               const std::string& context, const Rules&) {
    if (json.raw.empty()) {
        throw std::runtime_error(context +
                                 " is a motion value outside compositions");
    }
    std::string error;
    if (!mcp_json::Value::Parse(json.raw, value, error)) {
        throw std::runtime_error(context + " is not JSON: " + error);
    }
}

void ReadValue(const JsonValue& json, RationalTime& value,
               const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::Object) WrongType(context);
    value = {Require(json, "value", JsonValue::Type::Number, context).number,
             Int32(json, "rate", context)};
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void ReadValue(const JsonValue& json, T& value, const std::string& context,
               const Rules& rules) {
    if (json.type != JsonValue::Type::Object) WrongType(context);
    ReadObject(json, value, context, rules);
}

void ReadElement(const JsonValue& json, std::string& value,
                 const std::string& context, const Rules&) {
    if (json.type != JsonValue::Type::String)
        throw std::runtime_error(context + " must be a string");
    value = json.string;
}

// PA4 -- a list of times (DocumentClip::caption_word_times).
void ReadElement(const JsonValue& json, RationalTime& value,
                 const std::string& context, const Rules& rules) {
    ReadValue(json, value, context, rules);
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
void ReadElement(const JsonValue& json, T& value, const std::string& context,
                 const Rules& rules) {
    ReadObject(json, value, context, rules);
}

template <typename T>
void ReadValue(const JsonValue& json, std::vector<T>& values,
               const std::string& context, const Rules& rules) {
    if (json.type != JsonValue::Type::Array) WrongType(context);
    values.clear();
    values.reserve(json.array.size());
    for (size_t index = 0; index < json.array.size(); ++index) {
        T element{};
        ReadElement(json.array[index], element,
                    context + "[" + std::to_string(index) + "]", rules);
        values.push_back(std::move(element));
    }
}

template <typename V>
void ReadValue(const JsonValue& json, std::map<std::string, V>& values,
               const std::string& context, const Rules& rules) {
    if (json.type != JsonValue::Type::Object) WrongType(context);
    values.clear();
    for (const auto& [key, entry] : json.object) {
        const std::string entryContext = context + "['" + key + "']";
        if (entry.type != JsonValue::Type::Object) WrongType(entryContext);
        V item{};
        ReadObject(entry, item, entryContext, rules);
        values.emplace(key, std::move(item));
    }
}

// An enumerator that names nothing is a property of the object that holds
// it, the way the document has always reported it.
template <typename Enum>
void ReadEnum(const JsonValue& json, Enum& value, const std::string& owner,
              std::string_view key) {
    const std::string context = owner + "." + std::string(key);
    if (json.type != JsonValue::Type::String) WrongType(context);
    for (const auto& [enumerator, name] : EnumNames<Enum>::kNames) {
        if (name != json.string) continue;
        value = enumerator;
        return;
    }
    throw std::runtime_error(owner + " has invalid " + std::string(key));
}

template <typename T>
size_t KeyCount() {
    size_t count = 0;
    ForEachField<T>([&](const auto& field) {
        using Field = std::decay_t<decltype(field)>;
        if (Field::kKind == Kind::Carried) return;
        if (field.written_if || field.presence || field.since ||
            field.omitted_when_default)
            throw std::logic_error(
                "a conditional table cannot be read "
                "strictly");
        ++count;
    });
    return count;
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int>>
void ReadObject(const JsonValue& json, T& value, const std::string& context,
                const Rules& rules) {
    CheckTable<T>();
    if (json.type != JsonValue::Type::Object)
        throw std::runtime_error(context + " must be an object");
    if (rules.strict && json.object.size() != KeyCount<T>())
        throw std::runtime_error(context + " has unexpected or missing fields");
    ForEachField<T>([&](const auto& field) {
        using Field = std::decay_t<decltype(field)>;
        if constexpr (Field::kKind == Kind::Carried) {
            return;
        } else {
            if (field.since > rules.version) return;
            if (field.written_if && !field.written_if(value)) return;
            const std::string key(field.key);
            const auto found = json.object.find(key);
            if (found == json.object.end()) {
                if (field.presence) {
                    value.*field.presence = false;
                    return;
                }
                if (!rules.strict &&
                    (field.added_later || field.omitted_when_empty ||
                     field.omitted_when_default))
                    return;
                throw std::runtime_error(context + " is missing '" + key + "'");
            }
            if (field.presence) value.*field.presence = true;
            using Member = typename Field::MemberType;
            if constexpr (std::is_enum_v<Member>) {
                ReadEnum(found->second, value.*field.member, context,
                         field.key);
            } else {
                ReadValue(found->second, value.*field.member,
                          context + "." + key, rules);
            }
        }
    });
}

template <typename T>
void ReadItems(const JsonValue& json, std::vector<T>& items,
               const std::string& prefix, const Rules& rules) {
    items.clear();
    for (size_t index = 0; index < json.array.size(); ++index) {
        T item{};
        ReadObject(json.array[index], item,
                   prefix + "[" + std::to_string(index) + "]", rules);
        items.push_back(std::move(item));
    }
}

// A root-level list: one part per line, as the document and the project
// file both write theirs.
template <typename T>
void WriteItems(std::ostringstream& output, const std::vector<T>& items) {
    for (size_t index = 0; index < items.size(); ++index) {
        output << (index == 0 ? "\n" : ",\n") << "    ";
        WriteObject(output, items[index], 1);
    }
    if (!items.empty()) output << '\n';
}

template <typename T>
std::string PartJson(const T& value) {
    std::ostringstream output;
    WriteObject(output, value, 1);
    return output.str();
}

JsonValue JsonOfType(JsonValue::Type type) {
    JsonValue value;
    value.type = type;
    return value;
}

JsonValue JsonNumber(int64_t number) {
    JsonValue value = JsonOfType(JsonValue::Type::Number);
    value.number = number;
    return value;
}

// A v3 project part in the shape Document's reader expects: the parts it
// carries, and an empty sequence when it carries none.
JsonValue DocumentPartValue(int64_t documentVersion, JsonValue sequence) {
    JsonValue root = JsonOfType(JsonValue::Type::Object);
    root.object["version"] = JsonNumber(documentVersion);
    if (sequence.type == JsonValue::Type::Null) {
        JsonValue rate = JsonOfType(JsonValue::Type::Object);
        rate.object["num"] = JsonNumber(0);
        rate.object["den"] = JsonNumber(1);
        sequence = JsonOfType(JsonValue::Type::Object);
        sequence.object["id"] = JsonOfType(JsonValue::Type::String);
        sequence.object["name"] = JsonOfType(JsonValue::Type::String);
        sequence.object["width"] = JsonNumber(0);
        sequence.object["height"] = JsonNumber(0);
        sequence.object["frame_rate"] = std::move(rate);
        sequence.object["tracks"] = JsonOfType(JsonValue::Type::Array);
    }
    root.object["sequence"] = std::move(sequence);
    root.object["library"] = JsonOfType(JsonValue::Type::Array);
    root.object["sources"] = JsonOfType(JsonValue::Type::Array);
    return root;
}

bool ParseDocumentValue(const JsonValue& root, Document& output,
                        std::string& error, bool validate) {
    try {
        Document parsed;
        const int32_t storedVersion = Int32(root, "version", "document");
        if (storedVersion != 4 && storedVersion != 5 && storedVersion != 6 &&
            storedVersion != 7 &&
            storedVersion != kDocumentVersionWithCompositions &&
            storedVersion != kDocumentVersionWithMotionClips &&
            storedVersion != kDocumentVersionWithClipSpeed &&
            storedVersion != kDocumentVersionWithClipFraming &&
            storedVersion != kDocumentVersionWithClipAnimation &&
            storedVersion != kDocumentVersionWithColorTransition &&
            storedVersion != kDocumentVersionWithSequenceLut &&
            storedVersion != kDocumentVersionWithWhiteBalance &&
            storedVersion != kDocumentVersionWithMediaColorInput &&
            storedVersion != kDocumentVersionWithHueStages &&
            storedVersion != kDocumentVersionWithSpeedRamp &&
            storedVersion != kDocumentVersionWithShotDescriptions &&
            storedVersion != kDocumentVersionWithGeneratedMedia &&
            storedVersion != kDocumentVersionWithGraphicsInput &&
            storedVersion != kDocumentVersionWithResolvePrimaries &&
            storedVersion != kDocumentVersionWithDisabledClips &&
            storedVersion != kDocumentVersionWithTimelineEffects &&
            storedVersion != kDocumentVersionWithVoiceReference &&
            storedVersion != kDocumentVersionWithClipMask &&
            storedVersion != kDocumentVersionWithPersonMask) {
            // PRD20 -- a version past the newest is not an unreadable
            // file: the build is what is out of date, and the refusal says
            // how far it reads.
            if (storedVersion > kNewestDocumentVersion) {
                throw std::runtime_error(NewerFormatRefusal(
                    "document", storedVersion, kNewestDocumentVersion));
            }
            throw std::runtime_error("unsupported document version " +
                                     std::to_string(storedVersion));
        }
        // A7's neutral settings are unambiguous: zero dB is unity and a zero
        // duration has no envelope. Keep the established v4/v5/v6 migration
        // policy, then always emit the complete v7 representation on save.
        parsed.version = 7;
        Rules rules;
        rules.version = storedVersion;

        if (const JsonValue* color = Optional(
                root, "color_management", JsonValue::Type::Object, "document"))
            ReadObject(*color, parsed.color_management,
                       "document.color_management", rules);
        ReadObject(
            Require(root, "sequence", JsonValue::Type::Object, "document"),
            parsed.sequence, "document.sequence", rules);
        // MOTION-2026-09 M4 -- nor can a version before 9 hold a motion
        // clip: refused rather than read as a clip without a source.
        if (storedVersion < kDocumentVersionWithMotionClips &&
            HoldsMotionClip(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold motion clips: they need version " +
                std::to_string(kDocumentVersionWithMotionClips));
        }
        // R3 -- docs/roadmap/REEL-2026-09.md. Nor can a version before 10
        // hold a clip speed: read by an older binary the key disappears and
        // the shot silently plays at 1/1, so the file is refused instead.
        if (storedVersion < kDocumentVersionWithClipSpeed &&
            HoldsClipSpeed(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a clip speed: it needs version " +
                std::to_string(kDocumentVersionWithClipSpeed));
        }
        // R1 -- docs/roadmap/REEL-2026-09.md. Nor can a version before 11
        // hold a clip framing: read by an older binary the key disappears
        // and the shot is delivered fitted, with the black bars the framing
        // existed to remove, so the file is refused instead.
        if (storedVersion < kDocumentVersionWithClipFraming &&
            HoldsClipFraming(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a clip framing: it needs version " +
                std::to_string(kDocumentVersionWithClipFraming));
        }
        // R2 -- docs/roadmap/REEL-2026-09.md. Nor can a version before 12
        // hold an animated parameter: read by an older binary the key
        // disappears and the zoom is delivered as a still, so the rhythm
        // the edit is built on is gone under no error at all.
        if (storedVersion < kDocumentVersionWithClipAnimation &&
            HoldsClipAnimation(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold an animated clip parameter: it needs version " +
                std::to_string(kDocumentVersionWithClipAnimation));
        }
        // R5 -- docs/roadmap/REEL-2026-09.md. Nor can a version before 13
        // hold a colour transition or one at the edge of a track. A binary
        // that predates R5 refuses both anyway, but on the transition's
        // type or its missing neighbour; the version says the build is the
        // thing that is out of date.
        if (storedVersion < kDocumentVersionWithColorTransition &&
            HoldsColorTransition(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a colour or edge transition: it needs version " +
                std::to_string(kDocumentVersionWithColorTransition));
        }
        // R6 -- docs/roadmap/REEL-2026-09.md. Nor can a version before 14
        // hold a sequence look LUT: read by an older binary the key
        // disappears and the film is delivered ungraded, the look the whole
        // grade was built around gone under no error at all.
        if (storedVersion < kDocumentVersionWithSequenceLut &&
            HoldsSequenceLut(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a sequence look LUT: it needs version " +
                std::to_string(kDocumentVersionWithSequenceLut));
        }
        // R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. Nor can a
        // version before 18 hold a speed ramp: read by an older binary the
        // key disappears and the shot plays at 1/1 over another stretch of
        // its rush, under no error at all.
        if (storedVersion < kDocumentVersionWithSpeedRamp &&
            HoldsSpeedRamp(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a speed ramp: it needs version " +
                std::to_string(kDocumentVersionWithSpeedRamp));
        }
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. Nor can a version before
        // 23 hold a clip blur: read by an older binary the key disappears
        // and the shot is delivered sharp, under no error at all.
        if (storedVersion < kDocumentVersionWithTimelineEffects &&
            HoldsClipBlur(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a clip blur: it needs version " +
                std::to_string(kDocumentVersionWithTimelineEffects));
        }
        // PA8 -- docs/roadmap/PARITE-2026-11.md. Nor can a version before
        // 26 hold a clip mask: read by an older binary the key disappears
        // and the whole picture is delivered, under no error at all.
        if (storedVersion < kDocumentVersionWithClipMask &&
            HoldsClipMask(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a clip mask: it needs version " +
                std::to_string(kDocumentVersionWithClipMask));
        }
        // PA15 -- nor can a version before 27 hold a mask of the people.
        if (storedVersion < kDocumentVersionWithPersonMask &&
            HoldsPersonMask(parsed.sequence)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a 'person' mask: it needs version " +
                std::to_string(kDocumentVersionWithPersonMask));
        }
        ReadItems(Require(root, "library", JsonValue::Type::Array, "document"),
                  parsed.library, "library", rules);
        // PL1 -- docs/roadmap/PLANS-2026-10.md. Nor can a version before 19
        // hold a described shot: read by an older binary the key disappears
        // and the next save loses what a model or a person wrote.
        if (storedVersion < kDocumentVersionWithShotDescriptions &&
            HoldsShotDescriptions(parsed.library)) {
            throw std::runtime_error(
                "document version " + std::to_string(storedVersion) +
                " cannot hold a described shot: it needs version " +
                std::to_string(kDocumentVersionWithShotDescriptions));
        }
        if (const JsonValue* bins =
                Optional(root, "bins", JsonValue::Type::Array, "document"))
            ReadItems(*bins, parsed.bins, "bins", rules);
        ReadItems(Require(root, "sources", JsonValue::Type::Array, "document"),
                  parsed.sources, "sources", rules);
        // MOTION-2026-09 M3 -- a version that predates compositions cannot
        // hold one: refused rather than read as a document without them.
        if (const JsonValue* compositions = Optional(
                root, kMotionRootKey, JsonValue::Type::Array, "document")) {
            if (storedVersion < kDocumentVersionWithCompositions) {
                throw std::runtime_error(
                    "document version " + std::to_string(storedVersion) +
                    " cannot hold compositions: they need version " +
                    std::to_string(kDocumentVersionWithCompositions));
            }
            ReadItems(*compositions, parsed.compositions, kMotionRootKey,
                      rules);
        }

        if (validate && !parsed.Validate(error)) {
            return false;
        }
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

}  // namespace
}  // namespace document_format

using namespace document_format;

// The roots below list their members by hand: a document and a project
// file frame their parts in ways no table describes (spacing, versions,
// the migration of version 2). A member added to either is a compile error
// here until it is written and read there. Project, with its constructor,
// is not an aggregate and cannot be counted.
static_assert(AggregateMemberCount<Document>::value == 7,
              "Document gained a member: write and read it at its root");
static_assert(AggregateMemberCount<ProjectSettings>::value == 1,
              "ProjectSettings gained a member: write and read it in the "
              "project file");

bool Project::LoadFromString(const std::string& json, Project& output,
                             std::string& error) {
    try {
        JsonValue root = JsonParser(json).Parse();
        const JsonValue* format = Optional(root, "project_format",
                                           JsonValue::Type::String, "project");
        if (!format) throw std::runtime_error("missing project format");
        if (format->string != "cutmachine-project")
            throw std::runtime_error("unsupported project format '" +
                                     format->string + "'");
        const int32_t projectVersion =
            Int32(root, "project_version", "project");
        if (projectVersion != 2 && projectVersion != kNewestProjectVersion) {
            // PRD20 -- as for a document: newer is said to be newer.
            if (projectVersion > kNewestProjectVersion) {
                throw std::runtime_error(NewerFormatRefusal(
                    "project", projectVersion, kNewestProjectVersion));
            }
            throw std::runtime_error("unsupported project version");
        }

        Project parsed(
            Require(root, "name", JsonValue::Type::String, "project").string);
        parsed.id =
            Require(root, "id", JsonValue::Type::String, "project").string;
        parsed.active_timeline_id = Require(root, "active_timeline_id",
                                            JsonValue::Type::String, "project")
                                        .string;
        parsed.timelines.clear();

        if (projectVersion == 3) {
            // S7 -- SAVING_ROADMAP.md. The library, bins, sources and color
            // settings once, then each timeline: every part is read by
            // Document's own reader, and Project::Validate below checks the
            // assembled whole exactly as it checks a version-2 project.
            const int32_t documentVersion =
                Int32(root, "document_version", "project");
            JsonValue shared = DocumentPartValue(documentVersion, JsonValue());
            for (const char* key : {"library", "bins", "sources"}) {
                Require(root, key, JsonValue::Type::Array, "project");
                shared.object[key] = std::move(root.object.at(key));
            }
            Require(root, "color_management", JsonValue::Type::Object,
                    "project");
            shared.object["color_management"] =
                std::move(root.object.at("color_management"));
            // MOTION-2026-09 M3 -- a shared part, present when there is one.
            if (Optional(root, kMotionRootKey, JsonValue::Type::Array,
                         "project") != nullptr) {
                shared.object[kMotionRootKey] =
                    std::move(root.object.at(kMotionRootKey));
            }
            Document sharedDocument;
            std::string partError;
            if (!ParseDocumentValue(shared, sharedDocument, partError, false))
                throw std::runtime_error("invalid shared project data: " +
                                         partError);
            parsed.settings.color_management = sharedDocument.color_management;
            parsed.rushes = std::move(sharedDocument.library);
            parsed.bins = std::move(sharedDocument.bins);
            parsed.sources = std::move(sharedDocument.sources);
            parsed.compositions = std::move(sharedDocument.compositions);

            Require(root, "timelines", JsonValue::Type::Array, "project");
            JsonValue& timelines = root.object.at("timelines");
            if (timelines.array.empty())
                throw std::runtime_error("project has no timelines");
            for (size_t index = 0; index < timelines.array.size(); ++index) {
                Document timeline;
                if (!ParseDocumentValue(
                        DocumentPartValue(documentVersion,
                                          std::move(timelines.array[index])),
                        timeline, partError, false))
                    throw std::runtime_error("invalid timeline " +
                                             std::to_string(index) + ": " +
                                             partError);
                parsed.timelines.push_back(std::move(timeline.sequence));
            }
        } else {
            const JsonValue& documents = Require(
                root, "timeline_snapshots", JsonValue::Type::Array, "project");
            if (documents.array.empty())
                throw std::runtime_error("project has no timeline documents");
            std::optional<Document> shared;
            for (size_t index = 0; index < documents.array.size(); ++index) {
                const JsonValue& value = documents.array[index];
                if (value.type != JsonValue::Type::String)
                    throw std::runtime_error("project.timeline_snapshots[" +
                                             std::to_string(index) +
                                             "] has the wrong JSON type");
                Document document;
                std::string documentError;
                if (!Document::LoadFromString(value.string, document,
                                              documentError))
                    throw std::runtime_error("invalid timeline document " +
                                             std::to_string(index) + ": " +
                                             documentError);
                if (!shared) {
                    shared = document;
                    parsed.settings.color_management =
                        document.color_management;
                    parsed.rushes = document.library;
                    parsed.bins = document.bins;
                    parsed.sources = document.sources;
                    parsed.compositions = document.compositions;
                } else {
                    Document candidate = document;
                    candidate.sequence = shared->sequence;
                    if (candidate.SaveToString() != shared->SaveToString())
                        throw std::runtime_error(
                            "timeline documents disagree on shared project "
                            "data");
                }
                parsed.timelines.push_back(std::move(document.sequence));
            }
        }

        parsed.bin_metadata.clear();
        if (const JsonValue* metadata = Optional(
                root, "bin_metadata", JsonValue::Type::Array, "project")) {
            for (size_t index = 0; index < metadata->array.size(); ++index) {
                const JsonValue& item = metadata->array[index];
                const std::string context =
                    "project.bin_metadata[" + std::to_string(index) + "]";
                const Ulid itemId =
                    Require(item, "item_id", JsonValue::Type::String, context)
                        .string;
                ProjectBinMetadata entry;
                ReadObject(item, entry, context, Rules());
                if (!parsed.bin_metadata.emplace(itemId, std::move(entry))
                         .second)
                    throw std::runtime_error("duplicate bin metadata for '" +
                                             itemId + "'");
            }
        }

        parsed.timeline_bin_ids.clear();
        if (const JsonValue* placements = Optional(
                root, "timeline_bin_ids", JsonValue::Type::Array, "project")) {
            for (size_t index = 0; index < placements->array.size(); ++index) {
                const JsonValue& item = placements->array[index];
                const std::string context =
                    "project.timeline_bin_ids[" + std::to_string(index) + "]";
                const Ulid timelineId =
                    Require(item, "timeline_id", JsonValue::Type::String,
                            context)
                        .string;
                const Ulid binId =
                    Require(item, "bin_id", JsonValue::Type::String, context)
                        .string;
                if (!parsed.timeline_bin_ids.emplace(timelineId, binId).second)
                    throw std::runtime_error(
                        "duplicate timeline bin placement for '" + timelineId +
                        "'");
            }
        }

        if (!parsed.Validate(error)) return false;
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

// S7 -- SAVING_ROADMAP.md. Version 2 embedded one whole document per
// timeline, each carrying its own copy of the library: on an interview
// package with 405 rushes and 8 timelines, 1.9 of the file's 2.3 MB were
// seven redundant copies, and every save, autosave and load paid for them.
// Version 3 writes the shared parts once, then the timelines, all as plain
// JSON a person or a script can read without unescaping.
std::string Project::SaveToString() const {
    std::ostringstream output;
    output << "{\n  \"project_format\":\"cutmachine-project\",";
    output << "\n  \"project_version\":3,";
    // MOTION-2026-09 M4 -- version 9 as soon as one timeline holds a
    // motion clip: the timelines are read at the version the file declares.
    const bool motionClips =
        std::any_of(timelines.begin(), timelines.end(), HoldsMotionClip);
    // R3 -- and version 10 as soon as one timeline holds a clip speed.
    const bool clipSpeed =
        std::any_of(timelines.begin(), timelines.end(), HoldsClipSpeed);
    // R1 -- and version 11 as soon as one timeline holds a clip framing.
    const bool clipFraming =
        std::any_of(timelines.begin(), timelines.end(), HoldsClipFraming);
    // R2 -- and version 12 as soon as one timeline animates a parameter.
    const bool clipAnimation =
        std::any_of(timelines.begin(), timelines.end(), HoldsClipAnimation);
    // R5 -- and version 13 as soon as one timeline holds a colour or edge
    // transition.
    const bool colorTransition =
        std::any_of(timelines.begin(), timelines.end(), HoldsColorTransition);
    // R6 -- and version 14 as soon as one timeline carries a look LUT.
    const bool sequenceLut =
        std::any_of(timelines.begin(), timelines.end(), HoldsSequenceLut);
    // G7 -- and version 15 as soon as one timeline holds a white balance or
    // one media an as-shot white balance.
    const bool whiteBalance =
        std::any_of(timelines.begin(), timelines.end(), HoldsTemperature) ||
        HoldsAsShotWhiteBalance(rushes);
    // G3 -- and version 16 as soon as one media declares its colour input.
    // GR6 -- and version 17 as soon as one timeline holds a contrast pivot
    // or a hue stage. R3 -- and version 18 as soon as one holds a speed
    // ramp. PL1 -- and version 19 as soon as one media has a described
    // shot. GN3 -- and version 20 as soon as one media was generated.
    // DEC31 -- and version 21 as soon as one media is declared a graphic.
    // GR16 -- and version 22 as soon as one timeline holds a Resolve node.
    // And version 23 as soon as one timeline holds a disabled clip.
    // EF2, EF3 -- and version 24 as soon as one timeline holds a cross fade
    // or a blurred clip. GN6b -- and version 25 as soon as one media is a
    // voice with its reference words.
    output
        << "\n  \"document_version\":"
        << DocumentFormatVersion(
               !compositions.empty(), motionClips, clipSpeed, clipFraming,
               clipAnimation, colorTransition, sequenceLut, whiteBalance,
               HoldsMediaColorInput(rushes),
               std::any_of(timelines.begin(), timelines.end(), HoldsHueStages),
               std::any_of(timelines.begin(), timelines.end(), HoldsSpeedRamp),
               HoldsShotDescriptions(rushes), HoldsGeneratedMedia(rushes),
               HoldsGraphicsInput(rushes),
               std::any_of(timelines.begin(), timelines.end(),
                           HoldsResolvePrimaries),
               std::any_of(timelines.begin(), timelines.end(),
                           HoldsDisabledClip),
               std::any_of(timelines.begin(), timelines.end(),
                           [](const DocumentSequence& timeline) {
                               return HoldsCrossFade(timeline) ||
                                      HoldsClipBlur(timeline);
                           }),
               HoldsVoiceReference(rushes),
               std::any_of(timelines.begin(), timelines.end(), HoldsClipMask),
               std::any_of(timelines.begin(), timelines.end(), HoldsPersonMask))
        << ",";
    output << "\n  \"id\":\"" << Escape(id) << "\",";
    output << "\n  \"name\":\"" << Escape(name) << "\",";
    output << "\n  \"active_timeline_id\":\"" << Escape(active_timeline_id)
           << "\",";
    output << "\n  \"color_management\":";
    WriteObject(output, settings.color_management, 1);
    output << ",\n  \"library\":[";
    WriteItems(output, rushes);
    output << "  ],\n  \"bins\":[";
    WriteItems(output, bins);
    output << "  ],\n  \"sources\":[";
    WriteItems(output, sources);
    // MOTION-2026-09 M3 -- written only when there is one, so a project
    // without compositions keeps its bytes.
    if (!compositions.empty()) {
        output << "  ],\n  \"" << kMotionRootKey << "\":[";
        WriteItems(output, compositions);
    }
    output << "  ],\n  \"timelines\":[";
    WriteItems(output, timelines);
    output << "  ],\n  \"bin_metadata\":[";
    size_t metadataIndex = 0;
    for (const auto& item : bin_metadata) {
        output << (metadataIndex++ == 0 ? "\n" : ",\n") << "    {\"item_id\":\""
               << Escape(item.first) << '"';
        WriteFields(output, item.second, 1, true);
        output << '}';
    }
    if (!bin_metadata.empty()) output << '\n';
    output << "  ],\n  \"timeline_bin_ids\":[";
    size_t placementIndex = 0;
    for (const auto& item : timeline_bin_ids) {
        output << (placementIndex++ == 0 ? "\n" : ",\n")
               << "    {\"timeline_id\":\"" << Escape(item.first)
               << "\",\"bin_id\":\"" << Escape(item.second) << "\"}";
    }
    if (!timeline_bin_ids.empty()) output << '\n';
    output << "  ]\n}\n";
    return output.str();
}

bool Document::LoadFromString(const std::string& json, Document& output,
                              std::string& error) {
    return LoadFromString(json, output, error, true);
}

bool Document::LoadFromString(const std::string& json, Document& output,
                              std::string& error, bool validate) {
    try {
        return ParseDocumentValue(JsonParser(json).Parse(), output, error,
                                  validate);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

std::string Document::SaveToString() const {
    std::ostringstream output;
    // MOTION-2026-09 M3, M4, R3, R1, R2, R5 and R6 -- the oldest version
    // that holds the document: 7 without a composition, 8 with one, 9 with
    // a motion clip, 10 with a clip speed, 11 with a clip framing, 12 with
    // an animated clip parameter, 13 with a colour or edge transition, 14
    // with a sequence look LUT, 15 with a white balance (G7), 16 with a
    // media's own colour input (G3), 17 with a contrast pivot or a hue
    // stage (GR6), 18 with a speed ramp (R3), 19 with a described shot
    // (PL1), 20 with a generated media (GN3), 21 with a media declared a
    // graphic (DEC31), 22 with a Resolve node (GR16), 23 with a disabled
    // clip, 24 with a cross fade (EF2) or a blurred clip (EF3), 25 with a
    // voice and its reference words (GN6b).
    output
        << "{\n  \"version\": "
        << (compositions.empty() && !HoldsMotionClip(sequence) &&
                    !HoldsClipSpeed(sequence) && !HoldsClipFraming(sequence) &&
                    !HoldsClipAnimation(sequence) &&
                    !HoldsColorTransition(sequence) &&
                    !HoldsSequenceLut(sequence) &&
                    !HoldsTemperature(sequence) &&
                    !HoldsAsShotWhiteBalance(library) &&
                    !HoldsMediaColorInput(library) &&
                    !HoldsHueStages(sequence) && !HoldsSpeedRamp(sequence) &&
                    !HoldsShotDescriptions(library) &&
                    !HoldsGeneratedMedia(library) &&
                    !HoldsGraphicsInput(library) &&
                    !HoldsResolvePrimaries(sequence) &&
                    !HoldsDisabledClip(sequence) && !HoldsCrossFade(sequence) &&
                    !HoldsClipBlur(sequence) && !HoldsVoiceReference(library) &&
                    !HoldsClipMask(sequence) && !HoldsPersonMask(sequence)
                ? version
                : DocumentFormatVersion(
                      !compositions.empty(), HoldsMotionClip(sequence),
                      HoldsClipSpeed(sequence), HoldsClipFraming(sequence),
                      HoldsClipAnimation(sequence),
                      HoldsColorTransition(sequence),
                      HoldsSequenceLut(sequence),
                      HoldsTemperature(sequence) ||
                          HoldsAsShotWhiteBalance(library),
                      HoldsMediaColorInput(library), HoldsHueStages(sequence),
                      HoldsSpeedRamp(sequence), HoldsShotDescriptions(library),
                      HoldsGeneratedMedia(library), HoldsGraphicsInput(library),
                      HoldsResolvePrimaries(sequence),
                      HoldsDisabledClip(sequence),
                      HoldsCrossFade(sequence) || HoldsClipBlur(sequence),
                      HoldsVoiceReference(library), HoldsClipMask(sequence),
                      HoldsPersonMask(sequence)))
        << ",\n  \"sequence\":";
    WriteObject(output, sequence, 1);
    output << ",\n  \"color_management\":";
    WriteObject(output, color_management, 1);
    output << ",\n  \"library\": [";
    WriteItems(output, library);
    output << "  ],\n  \"bins\": [";
    WriteItems(output, bins);
    output << "  ],\n  \"sources\": [";
    WriteItems(output, sources);
    // MOTION-2026-09 M3 -- written only when there is one: a document
    // without compositions keeps version 7 and every byte it had.
    if (!compositions.empty()) {
        output << "  ],\n  \"" << kMotionRootKey << "\": [";
        WriteItems(output, compositions);
    }
    output << "  ]\n}\n";
    return output.str();
}

namespace document_json {

std::string SequenceJson(const DocumentSequence& sequence) {
    return document_format::PartJson(sequence);
}

std::string ColorManagementJson(const ColorManagementSettings& settings) {
    return document_format::PartJson(settings);
}

std::string LibraryMediaJson(const LibraryMedia& media) {
    return document_format::PartJson(media);
}

std::string BinJson(const DocumentBin& bin) {
    return document_format::PartJson(bin);
}

std::string SourceJson(const DocumentSource& source) {
    return document_format::PartJson(source);
}

std::string BinMetadataJson(const ProjectBinMetadata& metadata) {
    return document_format::PartJson(metadata);
}

std::string MediaRateJson(const MediaRate& rate) {
    return document_format::PartJson(rate);
}

bool ParseDocumentParts(const DocumentParts& parts, Document& output,
                        std::string& error) {
    const auto join = [](const std::vector<std::string>& items) {
        std::string joined;
        for (size_t index = 0; index < items.size(); ++index) {
            if (index) joined += ',';
            joined += items[index];
        }
        return joined;
    };
    // The version every part writer above emits: parts carry no version of
    // their own, so they are only ever read as the current one. MOTION-2026-09
    // M4, R3, R1, R2, R5, R6 -- the newest this build writes, since a
    // timeline part may hold a motion clip, a clip speed, a clip framing, an
    // animated parameter, a colour transition or a look LUT. G7, G3 -- and
    // the library part a white balance or a media's colour input. GR6 -- and
    // a timeline part a contrast pivot or a hue stage.
    static const std::string version = std::to_string(kNewestDocumentVersion);
    std::string json = "{\"version\":" + version + ",\"sequence\":";
    json += parts.sequence.empty()
                ? "{\"id\":\"\",\"name\":\"\",\"width\":0,\"height\":0,"
                  "\"frame_rate\":{\"num\":0,\"den\":1},\"tracks\":[]}"
                : parts.sequence;
    if (!parts.color_management.empty())
        json += ",\"color_management\":" + parts.color_management;
    json += ",\"library\":[" + join(parts.library) + "],\"bins\":[" +
            join(parts.bins) + "],\"sources\":[" + join(parts.sources) + "]}";
    return Document::LoadFromString(json, output, error, false);
}

namespace {

// S6 -- SAVING_ROADMAP.md. A delta's own records, read strictly: the
// document tables say what they hold, the delta that every key is there.
template <typename T>
bool ParseStrict(const std::string& json, const std::string& context, T& output,
                 std::string& error) {
    try {
        T parsed{};
        document_format::Rules rules;
        rules.strict = true;
        document_format::ReadObject(document_format::JsonParser(json).Parse(),
                                    parsed, context, rules);
        output = std::move(parsed);
        error.clear();
        return true;
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
}

}  // namespace

bool ParseBinMetadata(const std::string& json, const std::string& context,
                      ProjectBinMetadata& output, std::string& error) {
    return ParseStrict(json, context, output, error);
}

bool ParseMediaRate(const std::string& json, const std::string& context,
                    MediaRate& output, std::string& error) {
    return ParseStrict(json, context, output, error);
}

}  // namespace document_json

// DB3, OR2 -- the run records use the same complete field tables as the
// project. Only strings here: the plan and journals retain their own codecs.
namespace {

using mcp_json::Value;

template <typename T>
std::string SerializeRunRecord(const T& record) {
    document_format::CheckTable<T>();
    Value object = Value::MakeObject();
    document_format::ForEachField<T>([&](const auto& field) {
        object.Set(std::string(field.key),
                   Value::MakeString(record.*field.member));
    });
    return object.Dump();
}

template <typename T>
bool ParseRunRecord(const std::string& json, T& record, std::string& message) {
    document_format::CheckTable<T>();
    Value object;
    if (json.size() > kMaxRunStepBytes ||
        !Value::Parse(json, object, message) || !object.IsObject()) {
        message = "run step must be a JSON object of at most 16 MiB";
        return false;
    }
    T candidate;
    bool ok = true;
    document_format::ForEachField<T>([&](const auto& field) {
        const Value* value = object.Find(std::string(field.key));
        if (value == nullptr && field.added_later) {
            return;
        }
        if (value == nullptr || !value->IsString()) {
            message = "run step field '" + std::string(field.key) +
                      "' must be a string";
            ok = false;
        } else {
            candidate.*field.member = value->AsString();
        }
    });
    for (const auto& [key, value] : object.AsObject()) {
        (void)value;
        bool known = false;
        const std::string fieldKey = key;
        document_format::ForEachField<T>(
            [&](const auto& field) { known = known || fieldKey == field.key; });
        if (!known) {
            message = "unknown run step field '" + key + "'";
            ok = false;
        }
    }
    if (ok) {
        record = std::move(candidate);
    }
    return ok;
}

}  // namespace

bool ParseRunStepRequest(const std::string& json, RunStepRequest& request,
                         std::string& message) {
    return ParseRunRecord(json, request, message) &&
           ValidateRunStepRequest(request, message);
}

std::string SerializeRunStepRequest(const RunStepRequest& request) {
    return SerializeRunRecord(request);
}

bool ParseRunStepReceipt(const std::string& json, RunStepReceipt& receipt,
                         std::string& message) {
    if (!ParseRunRecord(json, receipt, message)) {
        return false;
    }
    RunStepRequest request;
    EditLog timelineLog;
    ProjectEditLog projectLog;
    EditError error = EditError::None;
    if (receipt.format != "cutmachine.run-step.v1" ||
        receipt.result_revision.size() != 16 ||
        receipt.result_revision.find_first_not_of("0123456789abcdef") !=
            std::string::npos ||
        receipt.base_history_hash.size() != 16 ||
        receipt.base_history_hash.find_first_not_of("0123456789abcdef") !=
            std::string::npos ||
        !IsValidUlid(receipt.project_id) || !IsValidUlid(receipt.snapshot_id) ||
        !ParseRunStepRequest(receipt.request_json, request, message) ||
        receipt.run_id != request.run_id ||
        receipt.step_id != request.step_id || receipt.stage != request.stage ||
        receipt.timeline_id != request.timeline_id ||
        receipt.base_revision != request.base_revision ||
        receipt.base_history_hash != request.base_history_hash ||
        CanonicalHash(receipt.project_history_json) !=
            request.base_project_history_hash ||
        !EditLog::Deserialize(receipt.timeline_history_json, timelineLog, error,
                              message) ||
        !ProjectEditLog::Deserialize(receipt.project_history_json, projectLog,
                                     error, message)) {
        message = "invalid or incompatible run step receipt: " + message;
        return false;
    }
    return true;
}

std::string SerializeRunStepReceipt(const RunStepReceipt& receipt) {
    return SerializeRunRecord(receipt);
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The archive manifest, written
// and read by its table: strict, as a delta's own records, then checked for
// what no writer produces.
std::string SerializeArchiveManifest(const ArchiveManifest& manifest) {
    return document_format::PartJson(manifest);
}

bool ParseArchiveManifest(const std::string& json, ArchiveManifest& manifest,
                          std::string& error) {
    ArchiveManifest parsed;
    try {
        document_format::Rules rules;
        rules.strict = true;
        document_format::ReadObject(document_format::JsonParser(json).Parse(),
                                    parsed, "archive manifest", rules);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (parsed.format != kArchiveManifestFormat) {
        error =
            "archive manifest has an unknown format '" + parsed.format + "'";
        return false;
    }
    if (parsed.bytes < 0 || !IsArchiveDigest(parsed.rest)) {
        error = "archive manifest needs a size and the digest of its rest";
        return false;
    }
    int64_t previous = -1;
    for (size_t index = 0; index < parsed.parts.size(); ++index) {
        const ArchivePart& part = parsed.parts[index];
        const std::string context =
            "archive manifest.parts[" + std::to_string(index) + "]";
        if (part.kind != kArchiveTimelinePart &&
            part.kind != kArchiveCompositionPart) {
            error = context + " has an unknown kind '" + part.kind + "'";
            return false;
        }
        if (!IsArchiveDigest(part.digest)) {
            error = context + " has no SHA-256 digest";
            return false;
        }
        if (part.at <= previous || part.at >= parsed.bytes) {
            error = context + " is out of document order";
            return false;
        }
        previous = part.at;
    }
    manifest = std::move(parsed);
    error.clear();
    return true;
}
