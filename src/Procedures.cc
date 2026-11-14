#include "Procedures.h"

#include <algorithm>
#include <cstdint>
#include <map>
#include <utility>

const char kProcedureMainFile[] = "SKILL.md";

namespace {

using mcp_json::Value;
using Entries = std::vector<const EmbeddedProcedureFile*>;

std::string LowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string Joined(const std::vector<std::string>& items,
                   const std::string& separator) {
    std::string joined;
    for (const std::string& item : items) {
        if (!joined.empty()) {
            joined += separator;
        }
        joined += item;
    }
    return joined;
}

std::string TextOf(const EmbeddedProcedureFile& file) {
    return std::string(reinterpret_cast<const char*>(file.bytes), file.size);
}

// SKILL.md first: it is the one a reader starts from.
bool MainFileFirst(const EmbeddedProcedureFile* left,
                   const EmbeddedProcedureFile* right) {
    const std::string leftPath = left->path;
    const std::string rightPath = right->path;
    const bool leftMain = leftPath == kProcedureMainFile;
    const bool rightMain = rightPath == kProcedureMainFile;
    if (leftMain != rightMain) {
        return leftMain;
    }
    return leftPath < rightPath;
}

std::map<std::string, Entries> FilesByProcedure() {
    size_t count = 0;
    const EmbeddedProcedureFile* files = EmbeddedProcedureFiles(count);
    std::map<std::string, Entries> byProcedure;
    for (size_t index = 0; index < count; ++index) {
        byProcedure[files[index].procedure].push_back(&files[index]);
    }
    for (auto& entry : byProcedure) {
        std::sort(entry.second.begin(), entry.second.end(), MainFileFirst);
    }
    return byProcedure;
}

std::vector<ProcedureFile> FilesOf(const Entries& entries) {
    std::vector<ProcedureFile> files;
    for (const EmbeddedProcedureFile* entry : entries) {
        files.push_back({entry->path, entry->size});
    }
    return files;
}

std::vector<std::string> PathsOf(const Entries& entries) {
    std::vector<std::string> paths;
    for (const EmbeddedProcedureFile* entry : entries) {
        paths.emplace_back(entry->path);
    }
    return paths;
}

// The item named `wanted`, or the only one named so in another case.
template <typename Item, typename NameOf>
const Item* FindByName(const std::vector<Item>& items,
                       const std::string& wanted, NameOf nameOf) {
    for (const Item& item : items) {
        if (nameOf(item) == wanted) {
            return &item;
        }
    }
    const Item* found = nullptr;
    for (const Item& item : items) {
        if (LowerAscii(nameOf(item)) == LowerAscii(wanted)) {
            if (found != nullptr) {
                return nullptr;
            }
            found = &item;
        }
    }
    return found;
}

// The segments of a path, "." and empty ones left out: "./a//b/" is a/b.
std::vector<std::string> Segments(const std::string& path) {
    std::vector<std::string> segments;
    std::string segment;
    for (size_t index = 0; index <= path.size(); ++index) {
        if (index == path.size() || path[index] == '/') {
            if (!segment.empty() && segment != ".") {
                segments.push_back(segment);
            }
            segment.clear();
        } else {
            segment.push_back(path[index]);
        }
    }
    return segments;
}

// ---- The front matter of a SKILL.md -------------------------------------

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) {
            end = text.size();
        }
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
        start = end + 1;
    }
    return lines;
}

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

std::string TrimBlanks(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && IsBlank(text[begin])) {
        ++begin;
    }
    while (end > begin && IsBlank(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string WithoutFinalBreaks(std::string text) {
    while (!text.empty() &&
           (text.back() == '\n' || text.back() == ' ' || text.back() == '\t')) {
        text.pop_back();
    }
    return text;
}

// The lines of a flow scalar folded as YAML folds them: each line trimmed,
// a line break between two lines read as a space, each empty line as a
// break.
std::string FoldedLines(const std::vector<std::string>& lines) {
    std::string folded;
    size_t emptyLines = 0;
    for (const std::string& raw : lines) {
        const std::string line = TrimBlanks(raw);
        if (line.empty()) {
            ++emptyLines;
            continue;
        }
        if (!folded.empty()) {
            folded += emptyLines > 0 ? std::string(emptyLines, '\n') : " ";
        }
        emptyLines = 0;
        folded += line;
    }
    return folded;
}

void AppendUtf8(std::string& text, uint32_t codePoint) {
    if (codePoint < 0x80) {
        text.push_back(static_cast<char>(codePoint));
    } else if (codePoint < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// What follows the opening quote of a double-quoted scalar, up to its
// closing one, escapes read.
std::string DoubleQuoted(const std::string& text) {
    std::string value;
    for (size_t index = 1; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '"') {
            break;
        }
        if (c != '\\' || index + 1 >= text.size()) {
            value.push_back(c);
            continue;
        }
        const char escape = text[++index];
        size_t digits = 0;
        switch (escape) {
            case 'n':
                value.push_back('\n');
                break;
            case 't':
                value.push_back('\t');
                break;
            case 'r':
                value.push_back('\r');
                break;
            case 'x':
                digits = 2;
                break;
            case 'u':
                digits = 4;
                break;
            case 'U':
                digits = 8;
                break;
            case '_':
                AppendUtf8(value, 0xA0);
                break;
            default:
                // \" \\ \/ and an escaped blank stand for themselves.
                value.push_back(escape);
                break;
        }
        if (digits > 0 && index + digits < text.size()) {
            uint32_t codePoint = 0;
            bool hex = true;
            for (size_t offset = 1; offset <= digits && hex; ++offset) {
                const char digit = text[index + offset];
                codePoint <<= 4;
                if (digit >= '0' && digit <= '9') {
                    codePoint |= static_cast<uint32_t>(digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                    codePoint |= static_cast<uint32_t>(digit - 'a' + 10);
                } else if (digit >= 'A' && digit <= 'F') {
                    codePoint |= static_cast<uint32_t>(digit - 'A' + 10);
                } else {
                    hex = false;
                }
            }
            if (hex && codePoint <= 0x10FFFF) {
                AppendUtf8(value, codePoint);
                index += digits;
            } else {
                value.push_back(escape);
            }
        } else if (digits > 0) {
            value.push_back(escape);
        }
    }
    return value;
}

std::string SingleQuoted(const std::string& text) {
    std::string value;
    for (size_t index = 1; index < text.size(); ++index) {
        if (text[index] != '\'') {
            value.push_back(text[index]);
        } else if (index + 1 < text.size() && text[index + 1] == '\'') {
            value.push_back('\'');
            ++index;
        } else {
            break;
        }
    }
    return value;
}

bool IsFileOf(const Entries& entries, const std::string& path) {
    const std::vector<std::string> paths = PathsOf(entries);
    return std::find(paths.begin(), paths.end(), path) != paths.end();
}

bool IsFolderOf(const Entries& entries, const std::string& folder) {
    for (const EmbeddedProcedureFile* entry : entries) {
        if (std::string(entry->path)
                .compare(0, folder.size() + 1, folder + "/") == 0) {
            return true;
        }
    }
    return false;
}

// The targets of the inline links of a Markdown text, `[text](target)`,
// outside fenced code: once each, in order, the "#anchor" dropped. A target
// with a scheme (https:, mailto:) or that is only an anchor is not a file.
std::vector<std::string> RelativeLinkTargets(
    const std::vector<std::string>& lines) {
    std::vector<std::string> targets;
    bool fenced = false;
    for (const std::string& line : lines) {
        const std::string trimmed = TrimBlanks(line);
        if (trimmed.compare(0, 3, "```") == 0 ||
            trimmed.compare(0, 3, "~~~") == 0) {
            fenced = !fenced;
            continue;
        }
        if (fenced) {
            continue;
        }
        for (size_t at = line.find("]("); at != std::string::npos;
             at = line.find("](", at + 2)) {
            const size_t begin = at + 2;
            size_t end = begin;
            while (end < line.size() && line[end] != ')' &&
                   !IsBlank(line[end])) {
                ++end;
            }
            std::string target = line.substr(begin, end - begin);
            target = target.substr(0, target.find('#'));
            const size_t colon = target.find(':');
            const bool scheme =
                colon != std::string::npos && target.find('/') > colon;
            if (target.empty() || scheme ||
                std::find(targets.begin(), targets.end(), target) !=
                    targets.end()) {
                continue;
            }
            targets.push_back(std::move(target));
        }
    }
    return targets;
}

// Where `target`, written in the file `from` of `procedure`, leads: walked
// from the directory that holds the skills, so that ".." out of one
// procedure lands in another, and out of them all in nothing served.
ProcedureLink ResolveLink(const std::map<std::string, Entries>& byProcedure,
                          const std::string& procedure, const std::string& from,
                          const std::string& target) {
    ProcedureLink resolved;
    resolved.link = target;
    std::vector<std::string> walked = {procedure};
    const std::vector<std::string> fromSegments = Segments(from);
    if (!fromSegments.empty()) {
        walked.insert(walked.end(), fromSegments.begin(),
                      fromSegments.end() - 1);
    }
    for (const std::string& segment : Segments(target)) {
        if (segment != "..") {
            walked.push_back(segment);
        } else if (walked.empty()) {
            return resolved;
        } else {
            walked.pop_back();
        }
    }
    const auto owner =
        walked.empty() ? byProcedure.end() : byProcedure.find(walked[0]);
    if (owner == byProcedure.end()) {
        return resolved;
    }
    const std::string rest =
        Joined(std::vector<std::string>(walked.begin() + 1, walked.end()), "/");
    if (rest.empty() || IsFileOf(owner->second, rest)) {
        resolved.procedure = owner->first;
        resolved.file = rest.empty() ? std::string(kProcedureMainFile) : rest;
    } else if (IsFolderOf(owner->second, rest)) {
        resolved.procedure = owner->first;
        resolved.folder = rest;
    }
    return resolved;
}

// A block scalar: `|` keeps the line breaks, `>` folds them. Its lines lose
// the indentation of the first one that is not empty.
std::string BlockScalar(char style, const std::vector<std::string>& lines) {
    size_t indentation = std::string::npos;
    for (const std::string& line : lines) {
        const size_t first = line.find_first_not_of(" \t");
        if (first != std::string::npos) {
            indentation = first;
            break;
        }
    }
    std::vector<std::string> content;
    content.reserve(lines.size());
    for (const std::string& line : lines) {
        content.push_back(indentation != std::string::npos &&
                                  line.size() > indentation
                              ? line.substr(indentation)
                              : TrimBlanks(line));
    }
    return style == '|' ? Joined(content, "\n") : FoldedLines(content);
}

}  // namespace

std::string SkillFrontMatterField(const std::string& skillText,
                                  const std::string& key) {
    const std::vector<std::string> lines = Lines(skillText);
    if (lines.empty() || TrimBlanks(lines[0]) != "---") {
        return std::string();
    }
    size_t end = 1;
    while (end < lines.size() && TrimBlanks(lines[end]) != "---" &&
           TrimBlanks(lines[end]) != "...") {
        ++end;
    }
    if (end == lines.size()) {
        return std::string();
    }
    for (size_t index = 1; index < end; ++index) {
        const std::string& line = lines[index];
        if (line.compare(0, key.size(), key) != 0) {
            continue;
        }
        size_t colon = key.size();
        while (colon < line.size() && IsBlank(line[colon])) {
            ++colon;
        }
        if (colon >= line.size() || line[colon] != ':' ||
            (colon + 1 < line.size() && !IsBlank(line[colon + 1]))) {
            continue;
        }
        const std::string value = TrimBlanks(line.substr(colon + 1));
        // The value goes on over the lines that are indented, or empty,
        // until the next key.
        std::vector<std::string> continued;
        for (size_t next = index + 1;
             next < end && (lines[next].empty() || IsBlank(lines[next][0]));
             ++next) {
            continued.push_back(lines[next]);
        }
        if (!value.empty() && (value[0] == '|' || value[0] == '>')) {
            return WithoutFinalBreaks(BlockScalar(value[0], continued));
        }
        std::vector<std::string> flow = {value};
        flow.insert(flow.end(), continued.begin(), continued.end());
        const std::string folded = FoldedLines(flow);
        if (!folded.empty() && folded[0] == '"') {
            return WithoutFinalBreaks(DoubleQuoted(folded));
        }
        if (!folded.empty() && folded[0] == '\'') {
            return WithoutFinalBreaks(SingleQuoted(folded));
        }
        // A plain scalar ends where a comment begins.
        const size_t comment = folded.find(" #");
        return WithoutFinalBreaks(TrimBlanks(folded.substr(
            0, comment == std::string::npos ? folded.size() : comment)));
    }
    return std::string();
}

std::vector<Procedure> ListProcedures() {
    std::vector<Procedure> procedures;
    for (const auto& [name, entries] : FilesByProcedure()) {
        Procedure procedure;
        procedure.name = name;
        procedure.files = FilesOf(entries);
        for (const EmbeddedProcedureFile* entry : entries) {
            if (std::string(entry->path) == kProcedureMainFile) {
                procedure.description =
                    SkillFrontMatterField(TextOf(*entry), "description");
            }
        }
        procedures.push_back(std::move(procedure));
    }
    return procedures;
}

bool ReadProcedureFile(const std::string& name, const std::string& path,
                       ProcedureText& file, std::string& errorName,
                       std::string& message) {
    const std::map<std::string, Entries> byProcedure = FilesByProcedure();
    std::vector<std::string> names;
    names.reserve(byProcedure.size());
    for (const auto& entry : byProcedure) {
        names.push_back(entry.first);
    }
    const std::string* procedure =
        FindByName(names, name, [](const std::string& item) { return item; });
    if (procedure == nullptr) {
        errorName = "UnknownProcedure";
        message = "unknown procedure '" + name + "'; known: " +
                  (names.empty() ? "none (this binary was built without the "
                                   "skills)"
                                 : Joined(names, ", "));
        return false;
    }
    const Entries& entries = byProcedure.at(*procedure);
    const std::vector<std::string> paths = PathsOf(entries);
    const std::string where = "procedure '" + *procedure + "'";

    if (!path.empty() &&
        (path[0] == '/' || path.find('\\') != std::string::npos)) {
        errorName = "ValidationFailed";
        message = "file '" + path + "' is not a path inside " + where +
                  ": write it relative to the procedure, as its links do "
                  "(SKILL.md, a/b.md)";
        return false;
    }
    const std::vector<std::string> segments = Segments(path);
    const auto lastUp = std::find(segments.rbegin(), segments.rend(), "..");
    if (lastUp != segments.rend()) {
        errorName = "ValidationFailed";
        message = "file '" + path + "' leaves " + where;
        // What follows the last "..", read as a path of this procedure (a
        // link copied from one of its subfolders, "../exemples/a.json") or
        // as another procedure and one of its files ("../other/b.md").
        const std::vector<std::string> remainder(lastUp.base(), segments.end());
        const std::string own = Joined(remainder, "/");
        const auto other = remainder.size() >= 2
                               ? byProcedure.find(remainder[0])
                               : byProcedure.end();
        const std::string otherFile =
            remainder.size() >= 2
                ? Joined(std::vector<std::string>(remainder.begin() + 1,
                                                  remainder.end()),
                         "/")
                : std::string();
        if (!own.empty() && IsFileOf(entries, own)) {
            message += ": it reads as its file '" + own +
                       "', ask for that file (a link is relative to the file "
                       "that holds it; the answer's `links` give each one "
                       "resolved)";
        } else if (other != byProcedure.end() &&
                   IsFileOf(other->second, otherFile)) {
            message += ": it is file '" + otherFile + "' of procedure '" +
                       other->first + "', ask for that name and that file";
        } else {
            message +=
                ", and nothing outside the procedures is served; its "
                "files: " +
                Joined(paths, ", ");
        }
        return false;
    }

    const std::string wanted = segments.empty()
                                   ? std::string(kProcedureMainFile)
                                   : Joined(segments, "/");
    const std::string* found =
        FindByName(paths, wanted, [](const std::string& item) { return item; });
    if (found == nullptr) {
        std::vector<std::string> inFolder;
        for (const std::string& candidate : paths) {
            if (candidate.compare(0, wanted.size() + 1, wanted + "/") == 0) {
                inFolder.push_back(candidate);
            }
        }
        errorName = "UnknownProcedureFile";
        message =
            inFolder.empty()
                ? where + " has no file '" + path +
                      "'; its files: " + Joined(paths, ", ")
                : "'" + path + "' is a folder of " + where +
                      ": name one of its files: " + Joined(inFolder, ", ");
        return false;
    }
    const EmbeddedProcedureFile* entry =
        entries[static_cast<size_t>(found - paths.data())];
    file.name = *procedure;
    file.path = entry->path;
    file.text = TextOf(*entry);
    file.files = FilesOf(entries);
    file.links.clear();
    const std::string extension = ".md";
    if (file.path.size() > extension.size() &&
        file.path.compare(file.path.size() - extension.size(), extension.size(),
                          extension) == 0) {
        for (const std::string& target :
             RelativeLinkTargets(Lines(file.text))) {
            file.links.push_back(
                ResolveLink(byProcedure, file.name, file.path, target));
        }
    }
    return true;
}

namespace {

Value FilesJson(const std::vector<ProcedureFile>& files) {
    Value list = Value::MakeArray();
    for (const ProcedureFile& file : files) {
        Value entry = Value::MakeObject();
        entry.Set("path", Value::MakeString(file.path));
        entry.Set("bytes", Value::MakeInt(static_cast<int64_t>(file.bytes)));
        list.Push(std::move(entry));
    }
    return list;
}

}  // namespace

Value ProcedureListingJson() {
    Value procedures = Value::MakeArray();
    for (const Procedure& procedure : ListProcedures()) {
        Value entry = Value::MakeObject();
        entry.Set("name", Value::MakeString(procedure.name));
        entry.Set("description", Value::MakeString(procedure.description));
        entry.Set("files", FilesJson(procedure.files));
        procedures.Push(std::move(entry));
    }
    Value answer = Value::MakeObject();
    answer.Set("procedures", std::move(procedures));
    return answer;
}

bool ProcedureFileJson(const std::string& name, const std::string& path,
                       Value& answer, std::string& errorName,
                       std::string& message) {
    ProcedureText file;
    if (!ReadProcedureFile(name, path, file, errorName, message)) {
        return false;
    }
    answer = Value::MakeObject();
    answer.Set("name", Value::MakeString(file.name));
    answer.Set("file", Value::MakeString(file.path));
    answer.Set("files", FilesJson(file.files));
    if (!file.links.empty()) {
        Value links = Value::MakeArray();
        for (const ProcedureLink& link : file.links) {
            Value entry = Value::MakeObject();
            entry.Set("link", Value::MakeString(link.link));
            if (link.procedure.empty()) {
                entry.Set("served", Value::MakeBool(false));
            } else {
                entry.Set("name", Value::MakeString(link.procedure));
                entry.Set(link.file.empty() ? "folder" : "file",
                          Value::MakeString(link.file.empty() ? link.folder
                                                              : link.file));
            }
            links.Push(std::move(entry));
        }
        answer.Set("links", std::move(links));
    }
    answer.Set("text", Value::MakeString(file.text));
    return true;
}
