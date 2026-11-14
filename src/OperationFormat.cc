// FORMAT-2026-09 -- ROADMAP.md. The journal codec: how an operation is
// written to, and read back from, an edit log or a project journal.
//
// Operations.cc owns what an operation *does* to a document; this file owns
// how it is spelled on disk, which is a compatibility contract with every
// project already saved. Nothing here decides the spelling: the field tables
// of OperationFormat.h do, and both directions below are derived from them,
// so a writer and a reader cannot drift apart or forget a member.
//
// tests/operation_format_tests.cc pins what this file may not change.

#include "OperationFormat.h"

#include "Json.h"

#include <cctype>
#include <cerrno>
#include <cstdint>
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
#include <variant>
#include <vector>

namespace operation_format {

class Reader {
public:
    explicit Reader(const std::string& input) : input_(input) {}

    void Expect(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) {
            throw std::runtime_error("expected '" + text + "' at byte " +
                                     std::to_string(position_));
        }
        position_ += text.size();
    }

    bool Consume(const std::string& text) {
        Skip();
        if (input_.compare(position_, text.size(), text) != 0) return false;
        position_ += text.size();
        return true;
    }

    // BENCH-2026-09 -- every escape JSON defines. The writer used to escape
    // only `"` and `\`, so a two-line caption put a raw line feed in the
    // journal and a strict JSON reader refused the file. It now spells
    // control characters as JSON requires; this reader takes those spellings
    // and the ones other JSON writers use (`\/`, `é`, surrogate pairs),
    // which an `--apply-op` argument built by a script may contain.
    //
    // A raw control character is still read as itself: every journal written
    // before the fix spells them that way, and it has to replay. The next
    // save writes the escaped form. tests/fixtures/operation_format/
    // tolerances.jsonl pins both tolerances.
    std::string String() {
        Skip();
        if (position_ >= input_.size() || input_[position_++] != '"') {
            throw std::runtime_error("expected string at byte " +
                                     std::to_string(position_));
        }
        std::string output;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (character == '"') return output;
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= input_.size()) {
                throw std::runtime_error("unterminated string escape");
            }
            const char escaped = input_[position_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    output.push_back(escaped);
                    break;
                case 'b':
                    output.push_back('\b');
                    break;
                case 'f':
                    output.push_back('\f');
                    break;
                case 'n':
                    output.push_back('\n');
                    break;
                case 'r':
                    output.push_back('\r');
                    break;
                case 't':
                    output.push_back('\t');
                    break;
                case 'u':
                    AppendUtf8(output, CodePoint());
                    break;
                default:
                    throw std::runtime_error("unsupported string escape");
            }
        }
        throw std::runtime_error("unterminated string");
    }

    int64_t Integer() {
        Skip();
        const size_t start = position_;
        if (position_ < input_.size() && input_[position_] == '-') ++position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start ||
            (position_ == start + 1 && input_[start] == '-')) {
            throw std::runtime_error("expected integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        errno = 0;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        if (!end || *end != '\0') throw std::runtime_error("invalid integer");
        // FORMAT-2026-09 -- strtoll saturates on overflow. Reading
        // 99999999999999999999 as INT64_MAX is a value nobody wrote, so it
        // is refused rather than clamped (PHILOSOPHY.md principle 4).
        if (errno == ERANGE)
            throw std::runtime_error("integer '" + text +
                                     "' outside int64_t range");
        return static_cast<int64_t>(value);
    }

    // The unsigned counterpart, for the members the journal stores as
    // uint64_t: strtoll cannot read back what the writer emitted for them.
    uint64_t Unsigned() {
        Skip();
        const size_t start = position_;
        while (position_ < input_.size() &&
               std::isdigit(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
        if (position_ == start) {
            throw std::runtime_error("expected unsigned integer at byte " +
                                     std::to_string(start));
        }
        char* end = nullptr;
        const std::string text = input_.substr(start, position_ - start);
        errno = 0;
        const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
        if (!end || *end != '\0')
            throw std::runtime_error("invalid unsigned integer");
        if (errno == ERANGE)
            throw std::runtime_error("integer '" + text +
                                     "' outside uint64_t range");
        return static_cast<uint64_t>(value);
    }

    // MOTION-2026-09 M3 -- the JSON value at the cursor, whatever its type,
    // read by the general parser: a motion tree keeps its key order and the
    // decimals the positional reader refuses.
    mcp_json::Value Json() {
        Skip();
        const size_t start = position_;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position_ < input_.size()) {
            const char character = input_[position_];
            if (inString) {
                ++position_;
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    inString = false;
                }
                if (!inString && depth == 0) {
                    break;
                }
                continue;
            }
            if (depth == 0 && position_ > start &&
                (character == ',' || character == '}' || character == ']')) {
                break;
            }
            ++position_;
            if (character == '"') {
                inString = true;
            } else if (character == '{' || character == '[') {
                ++depth;
            } else if (character == '}' || character == ']') {
                if (--depth == 0) {
                    break;
                }
            }
        }
        mcp_json::Value value;
        std::string error;
        if (depth != 0 || inString ||
            !mcp_json::Value::Parse(input_.substr(start, position_ - start),
                                    value, error)) {
            throw std::runtime_error("invalid JSON value at byte " +
                                     std::to_string(start) +
                                     (error.empty() ? "" : ": " + error));
        }
        return value;
    }

    // S6 -- SAVING_ROADMAP.md. The balanced JSON object at the cursor,
    // exactly as written, for a field that has its own strict parser
    // (ProjectDelta.h).
    std::string RawObject() {
        Skip();
        if (position_ >= input_.size() || input_[position_] != '{') {
            throw std::runtime_error("expected object at byte " +
                                     std::to_string(position_));
        }
        const size_t start = position_;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        while (position_ < input_.size()) {
            const char character = input_[position_++];
            if (inString) {
                if (escaped)
                    escaped = false;
                else if (character == '\\')
                    escaped = true;
                else if (character == '"')
                    inString = false;
                continue;
            }
            if (character == '"')
                inString = true;
            else if (character == '{')
                ++depth;
            else if (character == '}' && --depth == 0)
                return input_.substr(start, position_ - start);
        }
        throw std::runtime_error("unterminated object");
    }

    void Finish() {
        Skip();
        if (position_ != input_.size())
            throw std::runtime_error("unexpected trailing operation JSON");
    }

    // FORMAT-2026-09 -- matches `,"key":` (or `"key":` for the first member
    // of an object) without building the token: loading a project walks
    // millions of these.
    bool ConsumeKey(bool comma, std::string_view key) {
        Skip();
        size_t at = position_;
        if (comma) {
            if (at >= input_.size() || input_[at] != ',') return false;
            ++at;
        }
        if (at >= input_.size() || input_[at] != '"') return false;
        ++at;
        if (at + key.size() + 1 >= input_.size()) return false;
        if (input_.compare(at, key.size(), key) != 0) return false;
        at += key.size();
        if (input_[at] != '"' || input_[at + 1] != ':') return false;
        position_ = at + 2;
        return true;
    }

    void ExpectKey(bool comma, std::string_view key) {
        if (ConsumeKey(comma, key)) return;
        std::string token = comma ? "," : "";
        token += '"';
        token += key;
        token += "\":";
        throw std::runtime_error("expected '" + token + "' at byte " +
                                 std::to_string(position_));
    }

private:
    void Skip() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_]))) {
            ++position_;
        }
    }

    // The four hex digits after `\u`, the cursor just past the `u`.
    uint32_t Hex4() {
        if (input_.size() - position_ < 4) {
            throw std::runtime_error("truncated Unicode escape");
        }
        uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char digit = input_[position_++];
            value <<= 4;
            if (digit >= '0' && digit <= '9') {
                value |= static_cast<uint32_t>(digit - '0');
            } else if (digit >= 'a' && digit <= 'f') {
                value |= static_cast<uint32_t>(digit - 'a' + 10);
            } else if (digit >= 'A' && digit <= 'F') {
                value |= static_cast<uint32_t>(digit - 'A' + 10);
            } else {
                throw std::runtime_error("invalid Unicode escape");
            }
        }
        return value;
    }

    // A `\u` escape, joined with the low half that must follow a high
    // surrogate. A lone surrogate names no character and is refused.
    uint32_t CodePoint() {
        const uint32_t first = Hex4();
        if (first >= 0xdc00 && first <= 0xdfff) {
            throw std::runtime_error("unpaired surrogate escape");
        }
        if (first < 0xd800 || first > 0xdbff) {
            return first;
        }
        if (input_.compare(position_, 2, "\\u") != 0) {
            throw std::runtime_error("unpaired surrogate escape");
        }
        position_ += 2;
        const uint32_t second = Hex4();
        if (second < 0xdc00 || second > 0xdfff) {
            throw std::runtime_error("unpaired surrogate escape");
        }
        return 0x10000 + ((first - 0xd800) << 10) + (second - 0xdc00);
    }

    static void AppendUtf8(std::string& output, uint32_t codePoint) {
        if (codePoint <= 0x7f) {
            output.push_back(static_cast<char>(codePoint));
        } else if (codePoint <= 0x7ff) {
            output.push_back(static_cast<char>(0xc0 | (codePoint >> 6)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else if (codePoint <= 0xffff) {
            output.push_back(static_cast<char>(0xe0 | (codePoint >> 12)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        } else {
            output.push_back(static_cast<char>(0xf0 | (codePoint >> 18)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 12) & 0x3f)));
            output.push_back(
                static_cast<char>(0x80 | ((codePoint >> 6) & 0x3f)));
            output.push_back(static_cast<char>(0x80 | (codePoint & 0x3f)));
        }
    }

    const std::string& input_;
    size_t position_ = 0;
};

namespace {

// BENCH-2026-09 -- the same spelling as the document format and every MCP
// payload (Json.h): `"` and `\` escaped, control characters as `\n`, `\t`,
// `\r`, `\b`, `\f` or `\u00XX`, everything else, UTF-8 included, as is.
// Before, only `"` and `\` were escaped, and a journal holding a two-line
// caption was not JSON for anyone but this reader.
void WriteString(std::ostringstream& output, const std::string& value) {
    output << '"' << mcp_json::EscapeJsonString(value) << '"';
}

std::string HexEncode(const std::string& input) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string output;
    output.reserve(input.size() * 2);
    for (const unsigned char byte : input) {
        output.push_back(digits[byte >> 4]);
        output.push_back(digits[byte & 0x0f]);
    }
    return output;
}

std::string HexDecode(const std::string& input) {
    if (input.size() % 2 != 0)
        throw std::runtime_error("invalid exact project hex length");
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        throw std::runtime_error("invalid exact project hex digit");
    };
    std::string output;
    output.reserve(input.size() / 2);
    for (size_t index = 0; index < input.size(); index += 2)
        output.push_back(static_cast<char>((digit(input[index]) << 4) |
                                           digit(input[index + 1])));
    return output;
}

int32_t ReadInt32(Reader& reader, const char* what) {
    const int64_t value = reader.Integer();
    if (value < std::numeric_limits<int32_t>::min() ||
        value > std::numeric_limits<int32_t>::max()) {
        throw std::runtime_error(std::string(what) + " outside int32_t range");
    }
    return static_cast<int32_t>(value);
}

uint32_t ReadUint32(Reader& reader, const char* what) {
    const int64_t value = reader.Integer();
    if (value < 0 || value > std::numeric_limits<uint32_t>::max()) {
        throw std::runtime_error(std::string(what) + " outside uint32_t range");
    }
    return static_cast<uint32_t>(value);
}

// ---------------------------------------------------------------------
// Value codecs. Declared before any template body so that the types they
// serve are found from inside the templates that call them.
// ---------------------------------------------------------------------

void WriteValue(std::ostringstream& output, const std::string& value);
void WriteValue(std::ostringstream& output, bool value);
void WriteValue(std::ostringstream& output, int32_t value);
void WriteValue(std::ostringstream& output, int64_t value);
void WriteValue(std::ostringstream& output, uint32_t value);
void WriteValue(std::ostringstream& output, uint64_t value);
void WriteValue(std::ostringstream& output, const RationalTime& value);
void WriteValue(std::ostringstream& output, const mcp_json::Value& value);
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void WriteValue(std::ostringstream& output, Enum value);
template <typename T>
void WriteValue(std::ostringstream& output, const std::vector<T>& values);
template <typename T>
void WriteValue(std::ostringstream& output, const std::optional<T>& value);
template <typename V>
void WriteValue(std::ostringstream& output,
                const std::map<std::string, V>& values);
template <typename T,
          std::enable_if_t<IsTabled<T>::value && !IsTagged<T>::value, int> = 0>
void WriteValue(std::ostringstream& output, const T& value);
template <typename T, std::enable_if_t<IsTagged<T>::value, int> = 0>
void WriteValue(std::ostringstream& output, const T& value);

void ReadValue(Reader& reader, std::string& value);
void ReadValue(Reader& reader, bool& value);
void ReadValue(Reader& reader, int32_t& value);
void ReadValue(Reader& reader, int64_t& value);
void ReadValue(Reader& reader, uint32_t& value);
void ReadValue(Reader& reader, uint64_t& value);
void ReadValue(Reader& reader, RationalTime& value);
void ReadValue(Reader& reader, mcp_json::Value& value);
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
void ReadValue(Reader& reader, Enum& value);
template <typename T>
void ReadValue(Reader& reader, std::vector<T>& values);
template <typename T>
void ReadValue(Reader& reader, std::optional<T>& value);
template <typename V>
void ReadValue(Reader& reader, std::map<std::string, V>& values);
template <typename T,
          std::enable_if_t<IsTabled<T>::value && !IsTagged<T>::value, int> = 0>
void ReadValue(Reader& reader, T& value);
template <typename T, std::enable_if_t<IsTagged<T>::value, int> = 0>
void ReadValue(Reader& reader, T& value);

// The struct as declared, so that OmittedWhenDefault compares against the
// member's in-class default rather than a guess about its type.
template <typename T>
const T& DefaultOf() {
    static const T value{};
    return value;
}

// ---------------------------------------------------------------------
// Fields.
// ---------------------------------------------------------------------

template <typename T>
void WriteFields(std::ostringstream& output, const T& value, bool comma) {
    CheckTable<T>();
    ForEachField<T>([&](const auto& field) {
        using Field = std::decay_t<decltype(field)>;
        if constexpr (Field::kKind == Kind::Carried) {
            return;
        } else if constexpr (Field::kKind == Kind::Custom) {
            field.write(output, value, comma);
        } else {
            using Member = typename Field::MemberType;
            const Member& member = value.*field.member;
            if (field.presence == Presence::OmittedWhenDefault &&
                SameValue(member, DefaultOf<T>().*field.member)) {
                return;
            }
            if (field.presence == Presence::WrittenIf &&
                !field.condition(value)) {
                return;
            }
            if (comma) output << ',';
            comma = true;
            output << '"' << field.key << "\":";
            if constexpr (std::is_same_v<Member, bool>) {
                if (field.encoding == Encoding::BoolAsInteger) {
                    output << (member ? 1 : 0);
                    return;
                }
            }
            WriteValue(output, member);
        }
    });
}

template <typename T>
void ReadFields(Reader& reader, T& value, bool comma) {
    ForEachField<T>([&](const auto& field) {
        using Field = std::decay_t<decltype(field)>;
        if constexpr (Field::kKind == Kind::Carried) {
            return;
        } else if constexpr (Field::kKind == Kind::Custom) {
            field.read(reader, value, comma);
        } else {
            const bool required = field.presence == Presence::Required ||
                                  (field.presence == Presence::WrittenIf &&
                                   field.condition(value));
            if (required) {
                reader.ExpectKey(comma, field.key);
            } else if (!reader.ConsumeKey(comma, field.key)) {
                return;
            }
            comma = true;
            using Member = typename Field::MemberType;
            Member& member = value.*field.member;
            if constexpr (std::is_same_v<Member, bool>) {
                if (field.encoding == Encoding::BoolAsInteger) {
                    member = reader.Integer() != 0;
                    return;
                }
            }
            ReadValue(reader, member);
        }
    });
}

// ---------------------------------------------------------------------
// Values.
// ---------------------------------------------------------------------

void WriteValue(std::ostringstream& output, const std::string& value) {
    WriteString(output, value);
}

void WriteValue(std::ostringstream& output, bool value) {
    output << (value ? "true" : "false");
}

void WriteValue(std::ostringstream& output, int32_t value) { output << value; }
void WriteValue(std::ostringstream& output, int64_t value) { output << value; }
void WriteValue(std::ostringstream& output, uint32_t value) { output << value; }
void WriteValue(std::ostringstream& output, uint64_t value) { output << value; }

void WriteValue(std::ostringstream& output, const RationalTime& value) {
    output << "{\"value\":" << value.value << ",\"rate\":" << value.rate << '}';
}

void WriteValue(std::ostringstream& output, const mcp_json::Value& value) {
    output << value.Dump();
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int>>
void WriteValue(std::ostringstream& output, Enum value) {
    const auto& names = EnumNames<Enum>::kNames;
    const size_t count = sizeof(names) / sizeof(names[0]);
    static_assert(EnumNamesAreDistinct<Enum>(),
                  "two enumerators share a name in the journal");
    for (size_t index = 0; index + 1 < count; ++index) {
        if (names[index].first != value) continue;
        output << '"' << names[index].second << '"';
        return;
    }
    // A value cast in from outside the enumeration falls back to the last
    // name, which is what the hand-written writers did.
    output << '"' << names[count - 1].second << '"';
}

template <typename T>
void WriteValue(std::ostringstream& output, const std::vector<T>& values) {
    output << '[';
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) output << ',';
        WriteValue(output, values[index]);
    }
    output << ']';
}

template <typename T>
void WriteValue(std::ostringstream& output, const std::optional<T>& value) {
    if (!value) {
        output << "null";
        return;
    }
    WriteValue(output, *value);
}

template <typename V>
void WriteValue(std::ostringstream& output,
                const std::map<std::string, V>& values) {
    output << '{';
    bool comma = false;
    for (const auto& entry : values) {
        if (comma) output << ',';
        comma = true;
        WriteString(output, entry.first);
        output << ':';
        WriteValue(output, entry.second);
    }
    output << '}';
}

template <typename T,
          std::enable_if_t<IsTabled<T>::value && !IsTagged<T>::value, int>>
void WriteValue(std::ostringstream& output, const T& value) {
    output << '{';
    WriteFields(output, value, false);
    output << '}';
}

template <typename T, std::enable_if_t<IsTagged<T>::value, int>>
void WriteValue(std::ostringstream& output, const T& value) {
    output << "{\"type\":\"" << FieldTable<T>::kType << '"';
    WriteFields(output, value, true);
    output << '}';
}

void ReadValue(Reader& reader, std::string& value) { value = reader.String(); }

void ReadValue(Reader& reader, bool& value) {
    if (reader.Consume("true")) {
        value = true;
        return;
    }
    reader.Expect("false");
    value = false;
}

void ReadValue(Reader& reader, int32_t& value) {
    value = ReadInt32(reader, "integer");
}

void ReadValue(Reader& reader, int64_t& value) { value = reader.Integer(); }

void ReadValue(Reader& reader, uint32_t& value) {
    value = ReadUint32(reader, "integer");
}

void ReadValue(Reader& reader, uint64_t& value) { value = reader.Unsigned(); }

void ReadValue(Reader& reader, mcp_json::Value& value) {
    value = reader.Json();
}

void ReadValue(Reader& reader, RationalTime& value) {
    reader.Expect("{");
    reader.ExpectKey(false, "value");
    const int64_t amount = reader.Integer();
    reader.ExpectKey(true, "rate");
    const int32_t rate = ReadInt32(reader, "RationalTime rate");
    reader.Expect("}");
    value = {amount, rate};
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int>>
void ReadValue(Reader& reader, Enum& value) {
    const std::string text = reader.String();
    for (const auto& entry : EnumNames<Enum>::kNames) {
        if (entry.second != text) continue;
        value = entry.first;
        return;
    }
    throw std::runtime_error("unknown " + std::string(EnumNames<Enum>::kWhat) +
                             " '" + text + "'");
}

template <typename T>
void ReadValue(Reader& reader, std::vector<T>& values) {
    values.clear();
    reader.Expect("[");
    if (reader.Consume("]")) return;
    while (true) {
        T element{};
        ReadValue(reader, element);
        values.push_back(std::move(element));
        if (reader.Consume("]")) return;
        reader.Expect(",");
    }
}

template <typename T>
void ReadValue(Reader& reader, std::optional<T>& value) {
    if (reader.Consume("null")) {
        value.reset();
        return;
    }
    T present{};
    ReadValue(reader, present);
    value = std::move(present);
}

template <typename V>
void ReadValue(Reader& reader, std::map<std::string, V>& values) {
    values.clear();
    reader.Expect("{");
    if (reader.Consume("}")) return;
    while (true) {
        const std::string key = reader.String();
        reader.Expect(":");
        V item{};
        ReadValue(reader, item);
        if (!values.emplace(key, std::move(item)).second) {
            // Two answers to one question, resolved by position: refused.
            throw std::runtime_error("duplicate key '" + key + "'");
        }
        if (reader.Consume("}")) return;
        reader.Expect(",");
    }
}

template <typename T,
          std::enable_if_t<IsTabled<T>::value && !IsTagged<T>::value, int>>
void ReadValue(Reader& reader, T& value) {
    reader.Expect("{");
    ReadFields(reader, value, false);
    reader.Expect("}");
}

template <typename T, std::enable_if_t<IsTagged<T>::value, int>>
void ReadValue(Reader& reader, T& value) {
    reader.Expect("{\"type\":");
    const std::string type = reader.String();
    if (type != FieldTable<T>::kType) {
        throw std::runtime_error("expected a " +
                                 std::string(FieldTable<T>::kType) +
                                 " operation, found '" + type + "'");
    }
    ReadFields(reader, value, true);
    reader.Expect("}");
}

// ---------------------------------------------------------------------
// Operations.
// ---------------------------------------------------------------------

template <typename T, typename Variant>
bool ReadAlternative(Reader& reader, const std::string& type,
                     Variant& operation) {
    if (type != FieldTable<T>::kType) return false;
    T value;
    ReadFields(reader, value, true);
    reader.Expect("}");
    operation = std::move(value);
    return true;
}

template <typename... Alternatives>
void ReadOperation(Reader& reader, std::variant<Alternatives...>& operation,
                   const char* what) {
    reader.Expect("{\"type\":");
    const std::string type = reader.String();
    if (!(ReadAlternative<Alternatives>(reader, type, operation) || ...)) {
        throw std::runtime_error("unknown " + std::string(what) + " type '" +
                                 type + "'");
    }
}

template <typename Variant>
std::string WriteOperation(const Variant& operation) {
    std::ostringstream output;
    std::visit([&](const auto& value) { WriteValue(output, value); },
               operation);
    return output.str();
}

template <typename Variant>
bool ReadOperationJson(const std::string& json, Variant& operation,
                       const char* what, EditError& error,
                       std::string& message) {
    try {
        Reader reader(json);
        ReadOperation(reader, operation, what);
        reader.Finish();
        error = EditError::None;
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        error = EditError::ParseError;
        message = exception.what();
        return false;
    }
}

static_assert(TypeTagsAreDistinct(static_cast<const Operation*>(nullptr)),
              "two FieldTables share a timeline operation type tag");
static_assert(
    TypeTagsAreDistinct(static_cast<const ProjectOperation*>(nullptr)),
    "two FieldTables share a project operation type tag");

}  // namespace

// ---------------------------------------------------------------------
// Custom codecs: the shapes no presence rule describes.
// ---------------------------------------------------------------------

void WriteAddTrackClips(std::ostringstream& output,
                        const AddTrackOperation& value, bool& comma) {
    if (value.clips.empty()) return;
    if (comma) output << ',';
    comma = true;
    output << "\"exact_tracks\":";
    WriteValue(output,
               std::vector<ExactTrackState>{{value.track_id, value.clips}});
}

void ReadAddTrackClips(Reader& reader, AddTrackOperation& value, bool& comma) {
    if (!reader.ConsumeKey(comma, "exact_tracks")) return;
    comma = true;
    std::vector<ExactTrackState> tracks;
    ReadValue(reader, tracks);
    if (tracks.size() != 1 || tracks.front().track_id != value.track_id) {
        throw std::runtime_error("AddTrack exact state must match track_id");
    }
    value.clips = std::move(tracks.front().clips);
}

void WriteBatchMembers(std::ostringstream& output, const BatchOperation& value,
                       bool& comma) {
    if (comma) {
        output << ',';
    }
    comma = true;
    output << "\"operations\":[";
    for (size_t index = 0; index < value.operations.size(); ++index) {
        if (index != 0u) {
            output << ',';
        }
        std::visit([&](const auto& member) { WriteValue(output, member); },
                   value.operations[index]);
    }
    output << ']';
}

void ReadBatchMembers(Reader& reader, BatchOperation& value, bool& comma) {
    reader.ExpectKey(comma, "operations");
    comma = true;
    value.operations.clear();
    reader.Expect("[");
    if (reader.Consume("]")) {
        return;
    }
    while (true) {
        Operation member = RemoveClipOperation{};
        ReadOperation(reader, member, "batch operation");
        if (std::holds_alternative<BatchOperation>(member)) {
            throw std::runtime_error("a batch operation cannot hold a batch");
        }
        value.operations.push_back(std::move(member));
        if (reader.Consume("]")) {
            return;
        }
        reader.Expect(",");
    }
}

void WriteMeasuredAudioLevel(std::ostringstream& output,
                             const LibraryMedia& value, bool& comma) {
    if (!value.audio_level_measured) return;
    if (comma) output << ',';
    comma = true;
    output << "\"audio_level\":" << value.audio_level;
}

void ReadMeasuredAudioLevel(Reader& reader, LibraryMedia& value, bool& comma) {
    if (!reader.ConsumeKey(comma, "audio_level")) return;
    comma = true;
    value.audio_level_measured = true;
    value.audio_level = reader.Integer();
}

void WriteExactProject(std::ostringstream& output,
                       const std::optional<ExactProjectState>& exact,
                       bool& comma) {
    if (comma) output << ',';
    comma = true;
    if (exact && exact->delta) {
        output << "\"exact_project_delta\":"
               << SerializeProjectDelta(*exact->delta);
        return;
    }
    output << "\"exact_project_hex\":";
    if (!exact) {
        output << "null";
        return;
    }
    WriteString(output, HexEncode(exact->canonical_json));
}

void ReadExactProject(Reader& reader, std::optional<ExactProjectState>& exact,
                      bool& comma) {
    if (reader.ConsumeKey(comma, "exact_project_delta")) {
        comma = true;
        ProjectDelta delta;
        std::string error;
        if (!DeserializeProjectDelta(reader.RawObject(), delta, error)) {
            throw std::runtime_error(error);
        }
        exact = ExactProjectState{"", std::move(delta)};
        return;
    }
    reader.ExpectKey(comma, "exact_project_hex");
    comma = true;
    if (reader.Consume("null")) {
        exact.reset();
        return;
    }
    exact = ExactProjectState{HexDecode(reader.String()), std::nullopt};
}

}  // namespace operation_format

std::string SerializeOperation(const Operation& operation) {
    return operation_format::WriteOperation(operation);
}

std::string OperationTypeName(const Operation& operation) {
    return std::visit(
        [](const auto& value) {
            return std::string(
                operation_format::TypeTagOf<std::decay_t<decltype(value)>>());
        },
        operation);
}

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. list_motifs names a project
// entry as the journal writes it, without writing it.
std::string ProjectOperationTypeName(const ProjectOperation& operation) {
    return std::visit(
        [](const auto& value) {
            return std::string(
                operation_format::TypeTagOf<std::decay_t<decltype(value)>>());
        },
        operation);
}

bool DeserializeOperation(const std::string& json, Operation& operation,
                          EditError& error, std::string& message) {
    return operation_format::ReadOperationJson(json, operation, "operation",
                                               error, message);
}

std::string SerializeProjectOperation(const ProjectOperation& operation) {
    return operation_format::WriteOperation(operation);
}

bool DeserializeProjectOperation(const std::string& json,
                                 ProjectOperation& operation, EditError& error,
                                 std::string& message) {
    return operation_format::ReadOperationJson(
        json, operation, "project operation", error, message);
}

// MOTIF1 -- docs/roadmap/MOTIF-2026-10.md. The motifs of a journal entry,
// and the names of their vocabulary, from the tables of OperationFormat.h.

std::string SerializeEntryMotifs(const std::vector<EntryMotif>& motifs) {
    std::ostringstream output;
    operation_format::WriteValue(output, motifs);
    return output.str();
}

bool DeserializeEntryMotifs(const std::string& json,
                            std::vector<EntryMotif>& motifs,
                            std::string& message) {
    try {
        operation_format::Reader reader(json);
        std::vector<EntryMotif> parsed;
        operation_format::ReadValue(reader, parsed);
        reader.Finish();
        motifs = std::move(parsed);
        message.clear();
        return true;
    } catch (const std::exception& exception) {
        message = exception.what();
        return false;
    }
}

namespace {

template <typename Enum>
const char* EnumName(Enum value) {
    for (const auto& entry : operation_format::EnumNames<Enum>::kNames) {
        if (entry.first == value) return entry.second.data();
    }
    return "";
}

template <typename Enum>
bool ParseEnumName(const std::string& name, Enum& value) {
    for (const auto& entry : operation_format::EnumNames<Enum>::kNames) {
        if (entry.second == name) {
            value = entry.first;
            return true;
        }
    }
    return false;
}

}  // namespace

const char* MotifReasonName(MotifReason reason) { return EnumName(reason); }
const char* MotifAuthorName(MotifAuthor author) { return EnumName(author); }

bool ParseMotifReason(const std::string& name, MotifReason& reason) {
    return ParseEnumName(name, reason);
}

bool ParseMotifAuthor(const std::string& name, MotifAuthor& author) {
    return ParseEnumName(name, author);
}

std::vector<std::string> MotifReasonNames() {
    std::vector<std::string> names;
    for (const auto& entry : operation_format::EnumNames<MotifReason>::kNames) {
        if (entry.first != MotifReason::Unstated) {
            names.emplace_back(entry.second);
        }
    }
    return names;
}
