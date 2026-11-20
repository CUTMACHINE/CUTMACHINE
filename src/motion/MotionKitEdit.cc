#include "MotionKitEdit.h"

#include "MotionTemplates.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// CH1 -- docs/roadmap/CHARTE-2026-11.md. The edits rebuild the tree level
// by level rather than mutate it: mcp_json::Value has no erase and no
// mutable access to a child, and a copy keeps what the panel cares about
// for free -- every key in its rank, every number as its literal.

namespace {

using mcp_json::Value;

// A path of the grammar (MotionKitEdit.h), split and judged.
struct KitPath {
    std::vector<std::string> segments;
    // The last segment is an index into a list (palette.<index>...).
    bool item = false;
    size_t index = 0;
};

bool IsSegment(const std::string& segment) {
    return !segment.empty() && segment.size() <= 64 &&
           std::all_of(segment.begin(), segment.end(), [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                      (c >= '0' && c <= '9') || c == '_' || c == '-';
           });
}

// A decimal without a leading zero, as FindMotionKitPath reads one.
bool IsIndex(const std::string& segment, size_t& index) {
    if (segment.empty() || segment.size() > 9 ||
        (segment.size() > 1 && segment[0] == '0')) {
        return false;
    }
    index = 0;
    for (const char digit : segment) {
        if (digit < '0' || digit > '9') {
            return false;
        }
        index = index * 10 + static_cast<size_t>(digit - '0');
    }
    return true;
}

bool IsCharterField(const std::string& head) {
    return head == "palette" || head == "fonts" || head == "sources" ||
           head == "captions" || head == "pieces" || head == "withdrawn";
}

std::string Joined(const std::vector<std::string>& segments, size_t count) {
    std::string text;
    for (size_t index = 0; index < count && index < segments.size(); ++index) {
        text += (index == 0 ? "" : ".") + segments[index];
    }
    return text;
}

// Whether `path` is one of the places a charter has.
bool ParseKitPath(const std::string& path, KitPath& parsed, std::string& why) {
    parsed = KitPath{};
    size_t start = 0;
    while (true) {
        const size_t dot = path.find('.', start);
        parsed.segments.push_back(path.substr(
            start, dot == std::string::npos ? std::string::npos : dot - start));
        if (dot == std::string::npos) {
            break;
        }
        start = dot + 1;
    }
    for (const std::string& segment : parsed.segments) {
        if (!IsSegment(segment)) {
            why = "'" + path +
                  "' is not a path: dot-separated names of letters, digits, "
                  "'_' and '-', 64 at most each";
            return false;
        }
    }
    const std::vector<std::string>& s = parsed.segments;
    const size_t n = s.size();
    const std::string& head = s[0];
    bool known = false;
    if (head == "format" || head == "description" || head == "principle") {
        known = n == 1;
    } else if (head == "props" || head == "withdrawn") {
        known = n == 2;
    } else if (head == "palette" || head == "fonts" || head == "sources") {
        known = n == 1 || (n == 2 && IsIndex(s[1], parsed.index));
        parsed.item = n == 2;
    } else if (head == "captions") {
        known = n == 1 || (n == 2 && s[1] != "formats") ||
                ((n == 3 || n == 4) && s[1] == "formats");
    } else if (head == "pieces") {
        known = n == 2 || (n == 3 && s[2] != "formats") ||
                ((n == 4 || n == 5) && s[2] == "formats");
    }
    if (!known) {
        why = "'" + path +
              "' is not a place a charter has: format, description, "
              "principle, props.<role>, palette, fonts or sources (whole "
              "or .<index>), captions, captions.<setting>, "
              "captions.formats.<f>[.<setting>], pieces.<p>, "
              "pieces.<p>.<prop>, pieces.<p>.formats.<f>[.<prop>], "
              "withdrawn.<p>";
        return false;
    }
    return true;
}

Value WithoutKey(const Value& object, const std::string& key) {
    Value rebuilt = Value::MakeObject();
    for (const auto& [name, child] : object.AsObject()) {
        if (name != key) {
            rebuilt.Set(name, child);
        }
    }
    return rebuilt;
}

bool IsEmptyContainer(const Value& value) {
    return (value.IsObject() && value.AsObject().empty()) ||
           (value.IsArray() && value.AsArray().empty());
}

// `node` (null: missing) with `value` written at `path` from `depth` on,
// into `out`. Missing objects are created; a list is created only by an
// insert at index 0 (the first colour of a palette that has none).
bool SetAt(const Value* node, const KitPath& path, size_t depth,
           const Value& value, bool insert, Value& out, std::string& why) {
    const std::vector<std::string>& s = path.segments;
    const bool last = depth + 1 == s.size();
    if (last && path.item) {
        const std::string list = Joined(s, depth);
        if (node == nullptr) {
            if (insert && path.index == 0) {
                out = Value::MakeArray();
                out.Push(value);
                return true;
            }
            why = "'" + list + "' has no item " + s[depth] + ": there is no '" +
                  list + "'";
            return false;
        }
        if (!node->IsArray()) {
            why = "'" + list + "' is not a list";
            return false;
        }
        const std::vector<Value>& items = node->AsArray();
        if (path.index > items.size() ||
            (!insert && path.index == items.size())) {
            why = "'" + list + "' has " + std::to_string(items.size()) +
                  " item(s): no index " + s[depth] +
                  (insert ? "" : " to replace (insert appends at its size)");
            return false;
        }
        out = Value::MakeArray();
        for (size_t index = 0; index <= items.size(); ++index) {
            if (index == path.index) {
                out.Push(value);
                if (!insert) {
                    continue;
                }
            }
            if (index < items.size()) {
                out.Push(items[index]);
            }
        }
        return true;
    }
    if (node != nullptr && !node->IsObject()) {
        why = "'" + Joined(s, depth) + "' is not an object: no '" +
              Joined(s, s.size()) + "' can be written in it";
        return false;
    }
    out = node != nullptr ? *node : Value::MakeObject();
    if (last) {
        out.Set(s[depth], value);
        return true;
    }
    Value child;
    if (!SetAt(node != nullptr ? node->Find(s[depth]) : nullptr, path,
               depth + 1, value, insert, child, why)) {
        return false;
    }
    out.Set(s[depth], std::move(child));
    return true;
}

// `node` without the value at `path` from `depth` on, into `out`;
// `removed` false (and `out` untouched) when there was nothing there. A
// container the removal leaves empty is removed from its parent in turn.
void RemoveAt(const Value& node, const KitPath& path, size_t depth, Value& out,
              bool& removed) {
    removed = false;
    const std::vector<std::string>& s = path.segments;
    const bool last = depth + 1 == s.size();
    if (last && path.item) {
        if (!node.IsArray() || path.index >= node.AsArray().size()) {
            return;
        }
        out = Value::MakeArray();
        for (size_t index = 0; index < node.AsArray().size(); ++index) {
            if (index != path.index) {
                out.Push(node.AsArray()[index]);
            }
        }
        removed = true;
        return;
    }
    const Value* child = node.IsObject() ? node.Find(s[depth]) : nullptr;
    if (child == nullptr) {
        return;
    }
    if (last) {
        out = WithoutKey(node, s[depth]);
        removed = true;
        return;
    }
    Value rest;
    RemoveAt(*child, path, depth + 1, rest, removed);
    if (!removed) {
        return;
    }
    if (IsEmptyContainer(rest)) {
        out = WithoutKey(node, s[depth]);
    } else {
        out = node;
        out.Set(s[depth], std::move(rest));
    }
}

bool InvalidEdit(MotionKitError& error, const std::string& path,
                 std::string message) {
    error = MotionKitError{kMotionKitInvalidEdit, path, std::move(message)};
    return false;
}

}  // namespace

std::string DumpMotionKit(const mcp_json::Value& file) {
    return DumpIndentedMotionJson(file) + "\n";
}

bool EditMotionKit(const std::string& name, const mcp_json::Value& file,
                   const std::vector<MotionKitEdit>& edits,
                   mcp_json::Value& output, bool& changed,
                   MotionKitError& error) {
    changed = false;
    error = MotionKitError{};
    if (!file.IsObject()) {
        error =
            MotionKitError{kMotionKitInvalidKit, "", "a kit is a JSON object"};
        return false;
    }
    // Every edit is judged before any is made: all of them or none.
    std::vector<KitPath> paths(edits.size());
    bool upgrades = false;
    for (size_t index = 0; index < edits.size(); ++index) {
        const MotionKitEdit& edit = edits[index];
        std::string why;
        if (!ParseKitPath(edit.path, paths[index], why)) {
            return InvalidEdit(error, edit.path, why);
        }
        if (edit.value.has_value() && edit.value->IsNull()) {
            return InvalidEdit(
                error, edit.path,
                "'" + edit.path +
                    "' is given null: a value is removed, not set to null");
        }
        if (edit.insert && (!paths[index].item || !edit.value.has_value())) {
            return InvalidEdit(error, edit.path,
                               "an insert puts a value in a list, before "
                               "<index>: '" +
                                   edit.path + "' is not a list item" +
                                   (edit.value.has_value()
                                        ? std::string()
                                        : std::string(", or has no value")));
        }
        if (edit.path == "format") {
            if (!edit.value.has_value() || !edit.value->IsString() ||
                edit.value->AsString() != kBrandKitFormat) {
                return InvalidEdit(
                    error, edit.path,
                    std::string("'format' only goes to \"") + kBrandKitFormat +
                        "\", and is never removed: a charter does not go "
                        "back to a format that cannot hold what it has");
            }
            upgrades = true;
        }
    }
    const Value* format = file.Find("format");
    const bool brand = format != nullptr && format->IsString() &&
                       format->AsString() == kBrandKitFormat;
    if (!brand && !upgrades) {
        for (size_t index = 0; index < edits.size(); ++index) {
            if (IsCharterField(paths[index].segments[0])) {
                return InvalidEdit(
                    error, edits[index].path,
                    "'" + paths[index].segments[0] +
                        "' is a field of a brand's charter: set \"format\" "
                        "to \"" +
                        kBrandKitFormat + "\" in the same edits");
            }
        }
    }
    Value result = file;
    for (size_t index = 0; index < edits.size(); ++index) {
        const MotionKitEdit& edit = edits[index];
        Value next;
        if (edit.value.has_value()) {
            std::string why;
            if (!SetAt(&result, paths[index], 0, *edit.value, edit.insert, next,
                       why)) {
                return InvalidEdit(error, edit.path, why);
            }
        } else {
            bool removed = false;
            RemoveAt(result, paths[index], 0, next, removed);
            if (!removed) {
                continue;
            }
        }
        result = std::move(next);
    }
    const std::string dumped = DumpMotionKit(result);
    MotionKitInfo kit;
    std::string reason;
    if (!ParseMotionKit(name, dumped, kit, reason)) {
        error = MotionKitError{kMotionKitInvalidKit,
                               edits.size() == 1 ? edits[0].path : "", reason};
        return false;
    }
    changed = dumped != DumpMotionKit(file);
    output = std::move(result);
    return true;
}

bool ParseMotionKitEdits(const mcp_json::Value& json,
                         std::vector<MotionKitEdit>& out,
                         MotionKitError& error) {
    out.clear();
    error = MotionKitError{};
    if (!json.IsArray() || json.AsArray().empty()) {
        return InvalidEdit(error, "edits",
                           "'edits' is a non-empty array of {path, value} "
                           "or {path, remove: true}");
    }
    std::vector<MotionKitEdit> edits;
    for (size_t index = 0; index < json.AsArray().size(); ++index) {
        const Value& item = json.AsArray()[index];
        const std::string at = "edits[" + std::to_string(index) + "]";
        if (!item.IsObject()) {
            return InvalidEdit(error, at,
                               "'" + at +
                                   "' must be an object: {path, value} or "
                                   "{path, remove: true}");
        }
        for (const auto& entry : item.AsObject()) {
            if (entry.first != "path" && entry.first != "value" &&
                entry.first != "remove" && entry.first != "insert") {
                return InvalidEdit(error, at,
                                   "'" + at + "' has an unknown key '" +
                                       entry.first +
                                       "': path, value, remove, insert");
            }
        }
        const Value* path = item.Find("path");
        const Value* value = item.Find("value");
        const Value* remove = item.Find("remove");
        const Value* insert = item.Find("insert");
        if (path == nullptr || !path->IsString() || path->AsString().empty()) {
            return InvalidEdit(error, at,
                               "'" + at + ".path' must be a non-empty string");
        }
        if (value != nullptr && value->IsNull()) {
            return InvalidEdit(error, at,
                               "'" + at +
                                   ".value' is null: write \"remove\": true "
                                   "to remove a value");
        }
        if (remove != nullptr && !remove->IsBool()) {
            return InvalidEdit(error, at,
                               "'" + at + ".remove' must be true or false");
        }
        if (insert != nullptr && !insert->IsBool()) {
            return InvalidEdit(error, at,
                               "'" + at + ".insert' must be true or false");
        }
        const bool removing = remove != nullptr && remove->AsBool();
        const bool inserting = insert != nullptr && insert->AsBool();
        if ((value != nullptr) == removing) {
            return InvalidEdit(error, at,
                               "'" + at +
                                   "' must either set a value or say "
                                   "\"remove\": true, not " +
                                   (removing ? "both" : "neither"));
        }
        if (inserting && value == nullptr) {
            return InvalidEdit(error, at,
                               "'" + at +
                                   "' inserts nothing: an insert "
                                   "takes a value");
        }
        MotionKitEdit edit;
        edit.path = path->AsString();
        if (value != nullptr) {
            edit.value = *value;
        }
        edit.insert = inserting;
        edits.push_back(std::move(edit));
    }
    out = std::move(edits);
    return true;
}
