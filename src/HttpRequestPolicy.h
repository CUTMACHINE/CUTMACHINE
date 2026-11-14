#pragma once

// PRD13 -- ROADMAP.md. Who the loopback HTTP server (HttpServer.h) answers,
// decided on a request's method and header fields alone: no socket, no
// AppKit, so the rule has its own portable suite
// (tests/http_request_policy_tests.cc).
//
// Binding 127.0.0.1 keeps other machines out. It does not keep out a web
// page open in a browser on this machine. Measured on 2 October 2026: a POST
// carrying `Origin: https://evil.example`, `Host: evil.example` and
// `Content-Type: text/plain` was answered 200 with the tool's result. A
// text/plain POST is a CORS "simple request", sent without the preflight a
// server could refuse, so any page could call the tools blind; and since
// Host was not looked at, a page whose name is re-pointed at 127.0.0.1 (DNS
// rebinding) became same-origin and could read the answers too. The tools
// take file paths. Three checks close this, a fourth is optional:
//
//   - Host, when sent, names the loopback. A rebound name is refused.
//   - Origin, when sent, is a loopback origin. A page from anywhere else is
//     refused, `null` included.
//   - A POST declares application/json. That is not a simple request: a
//     browser has to ask first (OPTIONS), and it is never told yes, because
//     the server sends no Access-Control-* header at all.
//   - With a token configured, every request carries it as a bearer token.
//
// An absent Host and an absent Origin are accepted. A browser always sends
// Host, and always sends Origin with a cross-origin POST: their absence is a
// client that is not a browser (curl, an MCP client, an HTTP/1.0 script).
//
// Nothing here is a reason to reach this server from another machine: there
// is still no TLS, and the token then travels in the clear.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// One header field line of a request: what stood before the first colon, and
// what stood after it. Case and surrounding blanks are this file's business,
// not the caller's.
struct HttpHeaderField {
    std::string name;
    std::string value;
};
// Every field line, in the order received. A name sent twice is there twice:
// a map would keep one Host out of two, and the rule must see both.
using HttpHeaderFields = std::vector<HttpHeaderField>;

enum class HttpHeaderPresence : std::uint8_t { Absent, Single, Repeated };

// Looks `name` up without regard to case (give it in lower case). `value` is
// set, trimmed of blanks, when the field is there exactly once.
HttpHeaderPresence FindHttpHeader(const HttpHeaderFields& headers,
                                  const std::string& name, std::string& value);

struct LocalRequestConfig {
    // Empty: no token is asked for. Otherwise every request must carry
    // `Authorization: Bearer <bearer_token>`.
    std::string bearer_token;
    // PRD13b -- ROADMAP.md. The revisions of the MCP protocol the server
    // implements (McpProtocolVersions()). From 2025-06-18 on, a client over
    // HTTP names the revision it negotiated in an MCP-Protocol-Version
    // header, and a server must answer 400 to one it does not implement;
    // without the header nothing changes. Empty: the header is not looked
    // at.
    std::vector<std::string> protocol_versions;
};

struct HttpRequestVerdict {
    // 200 when the request may reach the handler. Otherwise the status to
    // answer with: 401 (token), 403 (Host, Origin), 415 (content type) or
    // 400 (protocol version).
    int status = 200;
    // What to correct, for whoever misconfigured a client; empty when
    // accepted. Nothing the request carried is repeated in it, and the token
    // never is: fixed sentences, and for the protocol version the list of
    // those the server implements.
    std::string reason;

    bool Accepted() const { return status == 200; }
};

// The rule, in the order it is checked: Host, Origin, token, content type,
// protocol version. Who is talking comes before what is said, so a page from
// elsewhere learns nothing about the token. `method` is the request line's,
// as sent: the content type is only asked of a "POST".
HttpRequestVerdict CheckLocalRequest(const std::string& method,
                                     const HttpHeaderFields& headers,
                                     const LocalRequestConfig& config);

// PRD13b -- ROADMAP.md. How long the body of a request is, read on its header
// fields alone (RFC 9112, section 6.3). This server reads a body by its
// Content-Length and no other way.
struct HttpBodyFraming {
    // 0 when the body can be read: `length` bytes, 0 when neither
    // Content-Length nor Transfer-Encoding is sent. Otherwise the status to
    // answer with, before any byte of the body is read:
    //   - 411, a body sent chunked: RFC 9110 gives 411 (Length Required) to a
    //     server that refuses a request without a defined Content-Length, and
    //     the client is told to add one -- the request is not malformed, it
    //     is framed in a way this server does not read;
    //   - 400, another Transfer-Encoding, after which the body's length
    //     cannot be determined (RFC 9112 asks for 400 then), or a
    //     Content-Length that is not one number of bytes: two that differ, a
    //     list of different values, anything but digits;
    //   - 413, a length beyond `maxBodyBytes`.
    int status = 0;
    size_t length = 0;
    // What to correct; empty when the body can be read. Fixed sentences.
    std::string reason;

    bool Readable() const { return status == 0; }
};

// A Content-Length sent twice with the same value, or as a list of the same
// value ("42, 42"), is that value, as RFC 9110 (section 8.6) allows.
HttpBodyFraming ReadBodyFraming(const HttpHeaderFields& headers,
                                size_t maxBodyBytes);

// The body of a refusal: {"error":"<reason>"}, the shape of the server's
// other errors.
std::string HttpRefusalBodyJson(const HttpRequestVerdict& verdict);

// A token a client is able to send: one or more visible ASCII characters
// (0x21 to 0x7E). A blank or a control character -- the CR a file with CRLF
// line ends leaves behind -- would be trimmed off the header or cut it short,
// and the server would refuse everyone, its owner included.
bool IsUsableBearerToken(const std::string& token);
