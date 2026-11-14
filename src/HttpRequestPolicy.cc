#include "HttpRequestPolicy.h"

#include <algorithm>
#include <cstddef>

namespace {

// What a refusal tells whoever misconfigured a client. Fixed sentences on
// purpose: nothing a request carried is echoed back, the token least of all.
const char* const kHostRefusal =
    "Host must be 127.0.0.1, localhost or [::1], with or without a port: "
    "this server only answers a client on this machine";
const char* const kHostRepeated =
    "Host is sent more than once: send it once, as 127.0.0.1, localhost or "
    "[::1]";
const char* const kOriginRefusal =
    "Origin must be a loopback origin (http://127.0.0.1, http://localhost or "
    "http://[::1], any port) or be absent: a web page from elsewhere may not "
    "call this server";
const char* const kOriginRepeated =
    "Origin is sent more than once: send it once, as a loopback origin, or "
    "not at all";
const char* const kTokenMissing =
    "this server requires a token: send Authorization: Bearer <token>, the "
    "token it was started with (--token or CUTMACHINE_MCP_TOKEN)";
const char* const kTokenWrong =
    "the bearer token is not the one this server was started with (--token "
    "or CUTMACHINE_MCP_TOKEN)";
const char* const kContentTypeRefusal =
    "a POST must declare Content-Type: application/json";
const char* const kProtocolVersionRepeated =
    "MCP-Protocol-Version is sent more than once: send it once, or leave it "
    "out";
const char* const kChunkedRefusal =
    "this server reads a request body by its Content-Length only: send the "
    "body with a Content-Length header and no Transfer-Encoding";
const char* const kTransferEncodingRefusal =
    "a Transfer-Encoding whose last coding is not chunked leaves the body's "
    "length unknown: send the body with a Content-Length header instead";
const char* const kContentLengthRefusal =
    "Content-Length must be one number of bytes: digits only, and the same "
    "value when it is sent more than once";
const char* const kBodyTooLarge = "request body too large";

// ASCII only, whatever the locale: header names, hosts, schemes and media
// types are ASCII, and a locale's idea of case has no say on them.
std::string LowerAscii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

bool IsBlank(char c) { return c == ' ' || c == '\t'; }

// The optional whitespace of RFC 9110: space and horizontal tab, no more.
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

bool IsPort(const std::string& text) {
    if (text.empty() || text.size() > 5) {
        return false;
    }
    int port = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        port = port * 10 + (c - '0');
    }
    return port <= 65535;
}

// What a Host field holds, and what follows the scheme in an origin: one of
// the three loopback names, then nothing, or a colon and a port. Read whole:
// "localhost.evil.example" and "127.0.0.1.evil.example" begin like a loopback
// name and are not one.
bool IsLoopbackAuthority(const std::string& authority) {
    static const std::string kLoopbackNames[] = {"127.0.0.1", "localhost",
                                                 "[::1]"};
    const std::string lowered = LowerAscii(authority);
    for (const std::string& name : kLoopbackNames) {
        if (lowered.compare(0, name.size(), name) != 0) {
            continue;
        }
        if (lowered.size() == name.size()) {
            return true;
        }
        if (lowered[name.size()] == ':' &&
            IsPort(lowered.substr(name.size() + 1))) {
            return true;
        }
    }
    return false;
}

// A serialized origin: scheme, "://", host, optional port, and nothing after
// -- no path, no trailing slash. Plain http only: the server speaks no TLS.
bool IsLoopbackOrigin(const std::string& origin) {
    static const std::string kScheme = "http://";
    const std::string lowered = LowerAscii(origin);
    return lowered.compare(0, kScheme.size(), kScheme) == 0 &&
           IsLoopbackAuthority(lowered.substr(kScheme.size()));
}

// The media type itself, before any parameter such as "; charset=utf-8".
// Compared whole as well: "application/json, text/plain" is not JSON, and
// neither is "text/plain; application/json".
bool IsJsonContentType(const std::string& value) {
    return LowerAscii(TrimBlanks(value.substr(0, value.find(';')))) ==
           "application/json";
}

// "Bearer", a blank, then the token. The scheme's case is free (RFC 9110
// 11.1); the token's is not.
bool BearerTokenOf(const std::string& authorization, std::string& token) {
    static const std::string kScheme = "bearer";
    if (authorization.size() <= kScheme.size() ||
        LowerAscii(authorization.substr(0, kScheme.size())) != kScheme ||
        !IsBlank(authorization[kScheme.size()])) {
        return false;
    }
    token = TrimBlanks(authorization.substr(kScheme.size()));
    return !token.empty();
}

// "a, b, c or d".
std::string SpokenList(const std::vector<std::string>& items) {
    std::string spoken;
    for (size_t index = 0; index < items.size(); ++index) {
        if (index > 0) {
            spoken += index + 1 == items.size() ? " or " : ", ";
        }
        spoken += items[index];
    }
    return spoken;
}

// The elements of a comma-separated field value, trimmed; empty ones are
// left out, as RFC 9110 (section 5.6.1) has a recipient ignore them.
std::vector<std::string> ListElements(const std::string& value) {
    std::vector<std::string> elements;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) {
            end = value.size();
        }
        const std::string element =
            TrimBlanks(value.substr(start, end - start));
        if (!element.empty()) {
            elements.push_back(element);
        }
        start = end + 1;
    }
    return elements;
}

// Takes as long as `presented` is, whatever `expected` holds and wherever
// the two first differ: a comparison that stopped at the first wrong
// character would let a token be guessed one character at a time, by timing
// the refusals.
bool SameToken(const std::string& presented, const std::string& expected) {
    unsigned char difference = presented.size() == expected.size() ? 0 : 1;
    for (size_t index = 0; index < presented.size(); ++index) {
        const unsigned char other =
            index < expected.size()
                ? static_cast<unsigned char>(expected[index])
                : static_cast<unsigned char>(0);
        difference = static_cast<unsigned char>(
            difference |
            (static_cast<unsigned char>(presented[index]) ^ other));
    }
    return difference == 0;
}

}  // namespace

HttpHeaderPresence FindHttpHeader(const HttpHeaderFields& headers,
                                  const std::string& name, std::string& value) {
    HttpHeaderPresence presence = HttpHeaderPresence::Absent;
    for (const HttpHeaderField& field : headers) {
        if (LowerAscii(TrimBlanks(field.name)) != name) {
            continue;
        }
        if (presence != HttpHeaderPresence::Absent) {
            return HttpHeaderPresence::Repeated;
        }
        presence = HttpHeaderPresence::Single;
        value = TrimBlanks(field.value);
    }
    return presence;
}

HttpRequestVerdict CheckLocalRequest(const std::string& method,
                                     const HttpHeaderFields& headers,
                                     const LocalRequestConfig& config) {
    std::string value;
    switch (FindHttpHeader(headers, "host", value)) {
        case HttpHeaderPresence::Absent:
            break;
        case HttpHeaderPresence::Repeated:
            return {403, kHostRepeated};
        case HttpHeaderPresence::Single:
            if (!IsLoopbackAuthority(value)) {
                return {403, kHostRefusal};
            }
            break;
    }
    switch (FindHttpHeader(headers, "origin", value)) {
        case HttpHeaderPresence::Absent:
            break;
        case HttpHeaderPresence::Repeated:
            return {403, kOriginRepeated};
        case HttpHeaderPresence::Single:
            if (!IsLoopbackOrigin(value)) {
                return {403, kOriginRefusal};
            }
            break;
    }
    if (!config.bearer_token.empty()) {
        std::string presented;
        if (FindHttpHeader(headers, "authorization", value) !=
                HttpHeaderPresence::Single ||
            !BearerTokenOf(value, presented)) {
            return {401, kTokenMissing};
        }
        if (!SameToken(presented, config.bearer_token)) {
            return {401, kTokenWrong};
        }
    }
    if (method == "POST" && (FindHttpHeader(headers, "content-type", value) !=
                                 HttpHeaderPresence::Single ||
                             !IsJsonContentType(value))) {
        return {415, kContentTypeRefusal};
    }
    // PRD13b -- compared whole and as sent: a revision is a date, and no
    // other spelling of it names one.
    const std::vector<std::string>& versions = config.protocol_versions;
    if (!versions.empty()) {
        switch (FindHttpHeader(headers, "mcp-protocol-version", value)) {
            case HttpHeaderPresence::Absent:
                break;
            case HttpHeaderPresence::Repeated:
                return {400, kProtocolVersionRepeated};
            case HttpHeaderPresence::Single:
                if (std::find(versions.begin(), versions.end(), value) ==
                    versions.end()) {
                    return {400,
                            "MCP-Protocol-Version must name a revision of the "
                            "protocol this server implements, " +
                                SpokenList(versions) + ", or be left out"};
                }
                break;
        }
    }
    return {};
}

HttpBodyFraming ReadBodyFraming(const HttpHeaderFields& headers,
                                size_t maxBodyBytes) {
    // Transfer-Encoding first: when it is sent, it decides how the body is
    // framed, whatever Content-Length says (RFC 9112, section 6.3). Its last
    // coding, over every field line, is the one that frames the body.
    bool transferEncoding = false;
    std::string lastCoding;
    for (const HttpHeaderField& field : headers) {
        if (LowerAscii(TrimBlanks(field.name)) != "transfer-encoding") {
            continue;
        }
        transferEncoding = true;
        for (const std::string& coding : ListElements(field.value)) {
            lastCoding =
                LowerAscii(TrimBlanks(coding.substr(0, coding.find(';'))));
        }
    }
    if (transferEncoding) {
        return lastCoding == "chunked"
                   ? HttpBodyFraming{411, 0, kChunkedRefusal}
                   : HttpBodyFraming{400, 0, kTransferEncodingRefusal};
    }
    // Every value of every Content-Length line, compared as written once
    // their leading zeros are put aside: they must all be the same number.
    std::string digits;
    for (const HttpHeaderField& field : headers) {
        if (LowerAscii(TrimBlanks(field.name)) != "content-length") {
            continue;
        }
        const std::vector<std::string> values = ListElements(field.value);
        if (values.empty()) {
            return {400, 0, kContentLengthRefusal};
        }
        for (const std::string& value : values) {
            if (value.find_first_not_of("0123456789") != std::string::npos) {
                return {400, 0, kContentLengthRefusal};
            }
            const size_t first = value.find_first_not_of('0');
            const std::string number =
                first == std::string::npos ? "0" : value.substr(first);
            if (!digits.empty() && number != digits) {
                return {400, 0, kContentLengthRefusal};
            }
            digits = number;
        }
    }
    HttpBodyFraming framing;
    if (digits.empty()) {
        return framing;
    }
    // Fifteen digits are more than any body this server takes: no number
    // read here overflows.
    if (digits.size() > 15) {
        return {413, 0, kBodyTooLarge};
    }
    size_t length = 0;
    for (const char digit : digits) {
        length = length * 10 + static_cast<size_t>(digit - '0');
    }
    if (length > maxBodyBytes) {
        return {413, 0, kBodyTooLarge};
    }
    framing.length = length;
    return framing;
}

std::string HttpRefusalBodyJson(const HttpRequestVerdict& verdict) {
    static const char kHex[] = "0123456789abcdef";
    std::string body = "{\"error\":\"";
    for (const char c : verdict.reason) {
        const unsigned char byte = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            body += '\\';
            body += c;
        } else if (byte < 0x20) {
            body += "\\u00";
            body += kHex[byte >> 4];
            body += kHex[byte & 0x0F];
        } else {
            body += c;
        }
    }
    body += "\"}";
    return body;
}

bool IsUsableBearerToken(const std::string& token) {
    if (token.empty()) {
        return false;
    }
    for (const char c : token) {
        if (c < 0x21 || c > 0x7E) {
            return false;
        }
    }
    return true;
}
