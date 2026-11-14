#include "McpArguments.h"

#include <utility>
#include <vector>

using mcp_json::Value;

namespace {

// PRD14 -- ROADMAP.md. The one table of the names a rush goes by. A row is a
// pair that means the same argument; a schema that declares one side takes
// the other.
constexpr const char* kRushArgumentNames[][2] = {
    {"media_id", "source_id"},
    {"media_ids", "source_ids"},
};

// ASCII only, and not std::tolower: the spellings are ASCII and the answer
// must not depend on the process locale.
char FoldAsciiCase(char character) {
    return character >= 'A' && character <= 'Z'
               ? static_cast<char>(character - 'A' + 'a')
               : character;
}

bool EqualIgnoringAsciiCase(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) {
        return false;
    }
    for (size_t index = 0; index < left.size(); ++index) {
        if (FoldAsciiCase(left[index]) != FoldAsciiCase(right[index])) {
            return false;
        }
    }
    return true;
}

// The schema's spelling of `given`, or nullptr when it is none of the
// values. An exact match wins, and two values that differ only by case
// leave a value written in a third case unmatched rather than guessed.
const std::string* CanonicalSpelling(const std::vector<std::string>& values,
                                     const std::string& given) {
    for (const std::string& value : values) {
        if (value == given) {
            return &value;
        }
    }
    const std::string* match = nullptr;
    for (const std::string& value : values) {
        if (!EqualIgnoringAsciiCase(value, given)) {
            continue;
        }
        if (match != nullptr) {
            return nullptr;
        }
        match = &value;
    }
    return match;
}

}  // namespace

const char* RushArgumentTwin(const std::string& name) {
    for (const auto& pair : kRushArgumentNames) {
        if (name == pair[0]) {
            return pair[1];
        }
        if (name == pair[1]) {
            return pair[0];
        }
    }
    return nullptr;
}

// What one schema node asks of the value it describes. Only the parts of a
// schema that carry a rule are kept, so a call is walked no deeper than its
// rules reach.
struct McpArgumentRules::Node {
    // The spellings of an enumeration of strings.
    std::vector<std::string> enum_values;
    // For an object: the accepted name of a rush, then the declared one.
    std::vector<std::pair<std::string, std::string>> twins;
    // For an object: the declared properties with a rule beneath them, and
    // those rules, index-aligned.
    std::vector<std::string> property_names;
    std::vector<Node> property_rules;
    // For an array: the rule of each item.
    std::shared_ptr<const Node> items;

    bool Empty() const {
        return enum_values.empty() && twins.empty() && property_names.empty() &&
               items == nullptr;
    }

    const Node* Property(const std::string& name) const {
        for (size_t index = 0; index < property_names.size(); ++index) {
            if (property_names[index] == name) {
                return &property_rules[index];
            }
        }
        return nullptr;
    }

    const std::string& DeclaredName(const std::string& name) const {
        for (const auto& twin : twins) {
            if (twin.first == name) {
                return twin.second;
            }
        }
        return name;
    }

    static Node FromSchema(const Value& schema) {
        Node node;
        if (!schema.IsObject()) {
            return node;
        }
        if (const Value* values = schema.Find("enum");
            values != nullptr && values->IsArray()) {
            for (const Value& value : values->AsArray()) {
                if (!value.IsString()) {
                    node.enum_values.clear();
                    break;
                }
                node.enum_values.push_back(value.AsString());
            }
        }
        if (const Value* properties = schema.Find("properties");
            properties != nullptr && properties->IsObject()) {
            for (const auto& [name, child] : properties->AsObject()) {
                Node rule = FromSchema(child);
                if (!rule.Empty()) {
                    node.property_names.push_back(name);
                    node.property_rules.push_back(std::move(rule));
                }
                // Declared under both names, an argument is two arguments
                // and the tool's own business.
                const char* twin = RushArgumentTwin(name);
                if (twin != nullptr && properties->Find(twin) == nullptr) {
                    node.twins.emplace_back(twin, name);
                }
            }
        }
        if (const Value* items = schema.Find("items")) {
            Node rule = FromSchema(*items);
            if (!rule.Empty()) {
                node.items = std::make_shared<const Node>(std::move(rule));
            }
        }
        return node;
    }

    bool Normalize(const std::string& path, const Value& value,
                   Value& normalized, std::string& message) const {
        if (value.IsString() && !enum_values.empty()) {
            const std::string* spelling =
                CanonicalSpelling(enum_values, value.AsString());
            normalized =
                spelling != nullptr ? Value::MakeString(*spelling) : value;
            return true;
        }
        if (value.IsObject() && (!property_names.empty() || !twins.empty())) {
            normalized = Value::MakeObject();
            for (const auto& [name, child] : value.AsObject()) {
                const std::string& declared = DeclaredName(name);
                if (declared != name) {
                    if (const Value* given = value.Find(declared)) {
                        if (given->Dump() != child.Dump()) {
                            message = "'";
                            message += path;
                            message += "' was given both '";
                            message += declared;
                            message += "' and '";
                            message += name;
                            message +=
                                "' with different values: they are two "
                                "names for the same argument, send one";
                            return false;
                        }
                        continue;
                    }
                }
                const Node* rule = Property(declared);
                if (rule == nullptr) {
                    normalized.Set(declared, child);
                    continue;
                }
                std::string inside = path;
                inside += '.';
                inside += declared;
                Value inner;
                if (!rule->Normalize(inside, child, inner, message)) {
                    return false;
                }
                normalized.Set(declared, std::move(inner));
            }
            return true;
        }
        if (value.IsArray() && items != nullptr) {
            normalized = Value::MakeArray();
            for (size_t index = 0; index < value.AsArray().size(); ++index) {
                std::string inside = path;
                inside += '[';
                inside += std::to_string(index);
                inside += ']';
                Value inner;
                if (!items->Normalize(inside, value.AsArray()[index], inner,
                                      message)) {
                    return false;
                }
                normalized.Push(std::move(inner));
            }
            return true;
        }
        normalized = value;
        return true;
    }
};

McpArgumentRules::McpArgumentRules(const std::string& inputSchemaJson) {
    Value schema;
    std::string parseError;
    if (!Value::Parse(inputSchemaJson, schema, parseError)) {
        return;
    }
    Node root = Node::FromSchema(schema);
    if (!root.Empty()) {
        root_ = std::make_shared<const Node>(std::move(root));
    }
}

bool McpArgumentRules::Normalize(const std::string& toolName,
                                 const Value& arguments, Value& normalized,
                                 std::string& message) const {
    if (root_ == nullptr) {
        normalized = arguments;
        return true;
    }
    return root_->Normalize(toolName, arguments, normalized, message);
}
