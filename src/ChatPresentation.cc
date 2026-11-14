#include "ChatPresentation.h"

#include "Json.h"

#include <cstdint>
#include <string_view>

namespace chat {
namespace {

std::string_view Trim(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

std::string_view Line(const std::string& text, size_t start, size_t& next) {
    const size_t end = text.find('\n', start);
    next = end == std::string::npos ? text.size() : end + 1;
    return std::string_view(text).substr(
        start, (end == std::string::npos ? text.size() : end) - start);
}

bool IsReport(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    if (!mcp_json::Value::Parse(text, value, error) || !value.IsObject()) {
        return false;
    }
    for (const char* key : {"compte_rendu", "etat_montage"}) {
        const auto* version = value.Find(key);
        int64_t number = 0;
        if (version != nullptr && version->AsInt64(number) && number == 1) {
            return true;
        }
    }
    return false;
}

}  // namespace

ChatMessagePresentation PresentChatMessage(const std::string& text) {
    ChatMessagePresentation result;
    size_t kept = 0;
    size_t position = 0;
    while (position < text.size()) {
        size_t next = position;
        const std::string_view line = Line(text, position, next);
        const size_t indent = line.find_first_not_of(' ');
        if (indent == std::string_view::npos || indent > 3 ||
            (line[indent] != '`' && line[indent] != '~')) {
            position = next;
            continue;
        }
        const char marker = line[indent];
        size_t count = 0;
        while (indent + count < line.size() && line[indent + count] == marker) {
            ++count;
        }
        if (count < 3) {
            position = next;
            continue;
        }
        const bool json = Trim(line.substr(indent + count)) == "json";
        const size_t body = next;
        size_t closing = body;
        size_t after = body;
        bool complete = false;
        while (closing < text.size()) {
            const auto closingLine = Line(text, closing, after);
            const size_t closingIndent = closingLine.find_first_not_of(' ');
            const auto candidate = Trim(closingLine);
            if (closingIndent <= 3 && candidate.size() >= count &&
                candidate.find_first_not_of(marker) == std::string_view::npos) {
                complete = true;
                break;
            }
            closing = after;
        }
        if (!complete) {
            break;
        }
        // Skip the whole fence, even for ordinary code: a nested-looking
        // json fence inside code is still code, not a second message.
        if (json && IsReport(text.substr(body, closing - body))) {
            result.markdown.append(text, kept, position - kept);
            if (!result.technical_report.empty()) {
                result.technical_report += "\n\n";
            }
            result.technical_report.append(text, body, closing - body);
            kept = after;
        }
        position = after;
    }
    result.markdown.append(text, kept, text.size() - kept);
    if (!result.technical_report.empty()) {
        result.markdown = std::string(Trim(result.markdown));
    }
    return result;
}

}  // namespace chat
