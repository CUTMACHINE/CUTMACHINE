// PRD13 -- ROADMAP.md. Who the loopback HTTP server answers.
//
// The rule is a function of a request's method and header fields
// (HttpRequestPolicy.h), so it is tested here without a socket: every Host
// and Origin a browser could be made to send, the content types a page can
// post without asking, and the bearer token. tests/mcp_tests.cc proves the
// same refusals against a real server.
//
// Plain C++: no AppKit, no FFmpeg, no socket, no network.

#include "HttpRequestPolicy.h"

#include <cstddef>
#include <exception>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const char* const kToken = "s3cret-token";

// What a local client that is not a browser sends: a loopback Host, JSON,
// no Origin.
HttpHeaderFields LocalClient() {
    return {{"Host", "127.0.0.1:8765"}, {"Content-Type", "application/json"}};
}

HttpHeaderFields With(HttpHeaderFields headers, const std::string& name,
                      const std::string& value) {
    headers.push_back({name, value});
    return headers;
}

// The same request without any line of that name, whatever its case.
HttpHeaderFields Without(const HttpHeaderFields& headers,
                         const std::string& name) {
    HttpHeaderFields kept;
    for (const HttpHeaderField& field : headers) {
        std::string ignored;
        if (FindHttpHeader({field}, name, ignored) ==
            HttpHeaderPresence::Absent) {
            kept.push_back(field);
        }
    }
    return kept;
}

// The same request with that field replaced by one new line.
HttpHeaderFields Replacing(const HttpHeaderFields& headers,
                           const std::string& name, const std::string& value) {
    return With(Without(headers, name), name, value);
}

int Status(const std::string& method, const HttpHeaderFields& headers,
           const std::string& token = std::string()) {
    LocalRequestConfig config;
    config.bearer_token = token;
    return CheckLocalRequest(method, headers, config).status;
}

std::string Reason(const std::string& method, const HttpHeaderFields& headers,
                   const std::string& token = std::string()) {
    LocalRequestConfig config;
    config.bearer_token = token;
    return CheckLocalRequest(method, headers, config).reason;
}

void CheckHosts() {
    for (const std::string& host :
         {"127.0.0.1", "127.0.0.1:8765", "127.0.0.1:0", "127.0.0.1:65535",
          "localhost", "localhost:8765", "LOCALHOST", "LocalHost:80", "[::1]",
          "[::1]:8765", " 127.0.0.1:8765 ", "\tlocalhost\t"}) {
        Check(Status("POST", Replacing(LocalClient(), "host", host)) == 200,
              "Host '" + host + "' names the loopback and is accepted");
    }
    for (const std::string& host : {
             "evil.example",
             "evil.example:8765",
             // Names that only begin, or only end, like a loopback one.
             "localhost.evil.example",
             "localhost.evil.example:8765",
             "127.0.0.1.evil.example",
             "127.0.0.1.evil.example:8765",
             "localhost:8765.evil.example",
             "localhostx",
             "xlocalhost",
             "evil.example.localhost",
             "127.0.0.10",
             "127.0.0.2",
             "0.0.0.0",
             "192.168.1.20:8765",
             // A fully qualified spelling is not one of the three names.
             "localhost.",
             // IPv6 without its brackets, half bracketed, or another address.
             "::1",
             "[::1",
             "::1]",
             "[::1]x",
             "[::2]",
             "[::ffff:127.0.0.1]",
             "[0:0:0:0:0:0:0:1]",
             // Ports that are not ports.
             "localhost:",
             "localhost:65536",
             "localhost:123456",
             "localhost:80a",
             "localhost:-1",
             "localhost:8765/",
             "localhost:8765:8765",
             // More than one host in one field.
             "127.0.0.1 evil.example",
             "127.0.0.1,evil.example",
             "127.0.0.1, 127.0.0.1",
             "user@127.0.0.1",
             "http://127.0.0.1",
             "",
         }) {
        const HttpHeaderFields headers = Replacing(LocalClient(), "host", host);
        Check(Status("POST", headers) == 403,
              "Host '" + host + "' is refused with 403, got " +
                  std::to_string(Status("POST", headers)));
        Check(Reason("POST", headers)
                      .find("Host must be 127.0.0.1, localhost or [::1]") !=
                  std::string::npos,
              "the refusal of Host '" + host + "' says what Host must be");
    }
    // A field name has no case, and blanks around it are not part of it.
    for (const std::string& name : {"host", "HOST", "hOsT", " Host", "Host "}) {
        const HttpHeaderFields headers = {{name, "evil.example"},
                                          {"Content-Type", "application/json"}};
        Check(Status("POST", headers) == 403,
              "a hostile Host is seen under the name '" + name + "'");
    }
    // No Host at all: a minimal HTTP/1.0 client, which a browser never is.
    Check(Status("POST", Without(LocalClient(), "host")) == 200,
          "a request without Host is accepted");
    // Two Host lines: whichever one a reader would pick, the request is
    // refused -- the rule does not pick.
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
             {"127.0.0.1:8765", "evil.example"},
             {"evil.example", "127.0.0.1:8765"},
             {"127.0.0.1:8765", "127.0.0.1:8765"}}) {
        const HttpHeaderFields headers = {{"Host", pair.first},
                                          {"host", pair.second},
                                          {"Content-Type", "application/json"}};
        Check(Status("POST", headers) == 403 &&
                  Reason("POST", headers).find("more than once") !=
                      std::string::npos,
              "Host sent twice ('" + pair.first + "', '" + pair.second +
                  "') is refused as sent twice");
    }
}

void CheckOrigins() {
    for (const std::string& origin :
         {"http://127.0.0.1", "http://127.0.0.1:3000", "http://localhost",
          "http://localhost:5173", "http://[::1]", "http://[::1]:8080",
          "HTTP://LOCALHOST:3000", "Http://LocalHost", " http://localhost "}) {
        Check(Status("POST", With(LocalClient(), "Origin", origin)) == 200,
              "Origin '" + origin + "' is a loopback origin and is accepted");
    }
    for (const std::string& origin : {
             // What a sandboxed frame, a file:// page or a redirect sends.
             "null",
             "NULL",
             "https://evil.example",
             "http://evil.example",
             "http://evil.example:8765",
             "http://localhost.evil.example",
             "http://localhost.evil.example:8765",
             "http://127.0.0.1.evil.example",
             "http://evil.example.localhost",
             "http://localhostx",
             "http://127.0.0.2",
             "http://[::2]",
             "http://::1",
             // The server speaks no TLS: an https page is not its client.
             "https://localhost",
             "https://127.0.0.1:8765",
             "ws://localhost",
             "file://",
             // An origin has no path, not even a slash.
             "http://localhost/",
             "http://localhost:3000/page",
             "http://localhost:3000?x",
             "http://localhost#x",
             "http://user@localhost",
             "http://localhost:",
             "http://localhost:99999",
             "localhost",
             "127.0.0.1:8765",
             "//localhost",
             "http:/localhost",
             "http://",
             "http://localhost http://evil.example",
             "http://localhost, http://evil.example",
             "",
         }) {
        const HttpHeaderFields headers = With(LocalClient(), "Origin", origin);
        Check(Status("POST", headers) == 403,
              "Origin '" + origin + "' is refused with 403, got " +
                  std::to_string(Status("POST", headers)));
        Check(
            Reason("POST", headers).find("Origin must be a loopback origin") !=
                std::string::npos,
            "the refusal of Origin '" + origin + "' says what Origin must be");
    }
    for (const std::string& name :
         {"origin", "ORIGIN", "Origin ", "\tOrigin"}) {
        Check(Status("POST",
                     With(LocalClient(), name, "https://evil.example")) == 403,
              "a hostile Origin is seen under the name '" + name + "'");
    }
    // No Origin: curl, an MCP client, a script.
    Check(Status("POST", LocalClient()) == 200,
          "a request without Origin is accepted");
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
             {"http://localhost", "https://evil.example"},
             {"https://evil.example", "http://localhost"},
             {"http://localhost", "http://localhost"}}) {
        const HttpHeaderFields headers = With(
            With(LocalClient(), "Origin", pair.first), "origin", pair.second);
        Check(Status("POST", headers) == 403 &&
                  Reason("POST", headers).find("more than once") !=
                      std::string::npos,
              "Origin sent twice ('" + pair.first + "', '" + pair.second +
                  "') is refused as sent twice");
    }
    // The check is on every method: the preflight of a page from elsewhere
    // is refused like the request it announces.
    for (const std::string& method : {"GET", "OPTIONS", "DELETE", "PUT"}) {
        Check(Status(method, {{"Host", "127.0.0.1:8765"},
                              {"Origin", "https://evil.example"}}) == 403,
              "a hostile Origin is refused on " + method);
        Check(Status(method, {{"Host", "evil.example:8765"}}) == 403,
              "a hostile Host is refused on " + method);
    }
}

void CheckContentTypes() {
    for (const std::string& type :
         {"application/json", "application/json; charset=utf-8",
          "application/json;charset=UTF-8", "Application/JSON",
          "APPLICATION/JSON ; charset=utf-8", " application/json ",
          "application/json;"}) {
        Check(Status("POST", Replacing(LocalClient(), "content-type", type)) ==
                  200,
              "Content-Type '" + type + "' is JSON and is accepted");
    }
    for (const std::string& type : {
             // The three a page may post without a preflight.
             "text/plain",
             "text/plain;charset=UTF-8",
             "application/x-www-form-urlencoded",
             "multipart/form-data; boundary=x",
             // JSON named in a parameter, or beside another type.
             "text/plain; application/json",
             "text/plain;application/json",
             "application/json, text/plain",
             "text/plain, application/json",
             "application/json application/json",
             // Near misses.
             "application/jsonp",
             "application/json-rpc",
             "application/x-json",
             "application/vnd.api+json",
             "text/json",
             "application/",
             "/json",
             "json",
             "application /json",
             "",
         }) {
        const HttpHeaderFields headers =
            Replacing(LocalClient(), "content-type", type);
        Check(Status("POST", headers) == 415,
              "a POST with Content-Type '" + type +
                  "' is refused with 415, got " +
                  std::to_string(Status("POST", headers)));
        Check(Reason("POST", headers).find("Content-Type: application/json") !=
                  std::string::npos,
              "the refusal of Content-Type '" + type +
                  "' says which one to send");
    }
    Check(Status("POST", Without(LocalClient(), "content-type")) == 415,
          "a POST without Content-Type is refused with 415");
    for (const std::string& name :
         {"content-type", "CONTENT-TYPE", "Content-type", " Content-Type "}) {
        Check(Status("POST", {{"Host", "127.0.0.1"},
                              {name, "application/json"}}) == 200,
              "the content type is read under the name '" + name + "'");
        Check(Status("POST", {{"Host", "127.0.0.1"}, {name, "text/plain"}}) ==
                  415,
              "a wrong content type is seen under the name '" + name + "'");
    }
    for (const auto& pair : std::vector<std::pair<std::string, std::string>>{
             {"application/json", "text/plain"},
             {"text/plain", "application/json"},
             {"application/json", "application/json"}}) {
        Check(Status("POST", {{"Host", "127.0.0.1"},
                              {"Content-Type", pair.first},
                              {"content-type", pair.second}}) == 415,
              "Content-Type sent twice ('" + pair.first + "', '" + pair.second +
                  "') is refused");
    }
    // Only a POST carries a body the server reads. Another method is the
    // handler's to refuse (405), as it was: this rule lets it through.
    for (const std::string& method :
         {"GET", "HEAD", "OPTIONS", "DELETE", "PUT", "post", "Post", ""}) {
        Check(Status(method, {{"Host", "127.0.0.1:8765"}}) == 200,
              "'" + method + "' without Content-Type is left to the handler");
        Check(Status(method, {{"Host", "127.0.0.1:8765"},
                              {"Content-Type", "text/plain"}}) == 200,
              "'" + method + "' with text/plain is left to the handler");
    }
}

void CheckToken() {
    const HttpHeaderFields local = LocalClient();
    const auto bearer = [&](const std::string& value) {
        return With(local, "Authorization", value);
    };
    // Accepted: the token, whole, after the scheme.
    for (const std::string& value :
         {std::string("Bearer ") + kToken, std::string("bearer ") + kToken,
          std::string("BEARER ") + kToken, std::string("Bearer    ") + kToken,
          std::string("Bearer\t") + kToken,
          std::string("  Bearer ") + kToken + "  "}) {
        Check(Status("POST", bearer(value), kToken) == 200,
              "Authorization '" + value + "' carries the token");
    }
    Check(Status("POST",
                 With(local, "AUTHORIZATION", std::string("Bearer ") + kToken),
                 kToken) == 200,
          "the token is read whatever the case of the field name");

    // Refused: no token, another one, part of it, more than it.
    Check(Status("POST", local, kToken) == 401,
          "a request without Authorization is refused with 401");
    Check(Reason("POST", local, kToken).find("Authorization: Bearer") !=
              std::string::npos,
          "the refusal says which header to send: " +
              Reason("POST", local, kToken));
    for (const std::string& value : {
             std::string("Bearer wrong-token"),
             // A prefix of the right token, down to its first character.
             std::string("Bearer s3cret-toke"),
             std::string("Bearer s3cret"),
             std::string("Bearer s"),
             // The right token and more.
             std::string("Bearer s3cret-token-and-more"),
             std::string("Bearer s3cret-tokenn"),
             std::string("Bearer s3cret-token s3cret-token"),
             std::string("Bearer s3cret-token,"),
             // The same length, one character off, at either end.
             std::string("Bearer s3cret-tokeN"),
             std::string("Bearer S3cret-token"),
             std::string("Bearer S3CRET-TOKEN"),
             // No token, or no scheme.
             std::string("Bearer"),
             std::string("Bearer "),
             std::string(""),
             std::string("s3cret-token"),
             std::string("Bearers3cret-token"),
             std::string("Bearer=s3cret-token"),
             std::string("Basic s3cret-token"),
             std::string("Token s3cret-token"),
             // Another scheme as long as "Bearer".
             std::string("Digest s3cret-token"),
             std::string("Bearex s3cret-token"),
             std::string("Bearer \"s3cret-token\""),
         }) {
        Check(Status("POST", bearer(value), kToken) == 401,
              "Authorization '" + value + "' is refused with 401, got " +
                  std::to_string(Status("POST", bearer(value), kToken)));
    }
    // A token with a NUL in it is compared whole, not up to the NUL.
    Check(Status("POST",
                 bearer(std::string("Bearer s3cret-token") +
                        std::string(1, '\0') + "x"),
                 kToken) == 401,
          "a token followed by a NUL and more is refused");
    Check(Status("POST",
                 With(bearer(std::string("Bearer ") + kToken), "authorization",
                      std::string("Bearer ") + kToken),
                 kToken) == 401,
          "Authorization sent twice is refused, even right both times");

    // Every request is asked, not only a POST.
    for (const std::string& method : {"GET", "OPTIONS", "DELETE"}) {
        Check(Status(method, {{"Host", "127.0.0.1"}}, kToken) == 401,
              method + " without the token is refused with 401");
        Check(Status(method,
                     {{"Host", "127.0.0.1"},
                      {"Authorization", std::string("Bearer ") + kToken}},
                     kToken) == 200,
              method + " with the token is left to the handler");
    }

    // The sentence never holds a token: not the server's, not the one sent.
    for (const std::string& value :
         {std::string(), std::string("Bearer wrong-token"),
          std::string("Bearer s3cret-toke")}) {
        const std::string reason =
            Reason("POST", value.empty() ? local : bearer(value), kToken);
        Check(!reason.empty() && reason.find(kToken) == std::string::npos &&
                  reason.find("wrong-token") == std::string::npos &&
                  reason.find("s3cret") == std::string::npos,
              "a 401 names no token: " + reason);
    }

    // Without a configured token nothing is asked: an Authorization header
    // a client sends anyway changes nothing.
    Check(Status("POST", local) == 200,
          "no token configured: a request without Authorization passes");
    Check(Status("POST", bearer("Bearer anything")) == 200 &&
              Status("POST", bearer("Basic xyz")) == 200,
          "no token configured: an Authorization header is not looked at");
}

// Who is talking is settled before what is said: the first failed check is
// the one answered, Host, then Origin, then the token, then the content type.
void CheckOrder() {
    const HttpHeaderFields hostile = {{"Host", "evil.example"},
                                      {"Origin", "https://evil.example"},
                                      {"Content-Type", "text/plain"}};
    Check(Status("POST", hostile, kToken) == 403 &&
              Reason("POST", hostile, kToken).find("Host must be") == 0,
          "everything wrong: Host is what is answered");
    const HttpHeaderFields rebound = Replacing(hostile, "host", "127.0.0.1");
    Check(Status("POST", rebound, kToken) == 403 &&
              Reason("POST", rebound, kToken).find("Origin must be") == 0,
          "a loopback Host and a hostile Origin: Origin is what is answered");
    const HttpHeaderFields blind = Without(rebound, "origin");
    Check(Status("POST", blind, kToken) == 401,
          "no token and text/plain: the token is asked for first");
    Check(Status("POST",
                 With(blind, "Authorization", std::string("Bearer ") + kToken),
                 kToken) == 415,
          "the token and text/plain: the content type is refused");
    // The request measured on 2 October 2026, which was answered 200.
    Check(Status("POST", hostile) == 403,
          "the request of the audit is refused without any token configured");
    const HttpRequestVerdict accepted =
        CheckLocalRequest("POST", LocalClient(), LocalRequestConfig{});
    Check(accepted.Accepted() && accepted.status == 200 &&
              accepted.reason.empty(),
          "an accepted request is a 200 without a reason");
}

void CheckRefusalBody() {
    HttpRequestVerdict verdict;
    verdict.status = 403;
    verdict.reason = "Host must be 127.0.0.1";
    Check(HttpRefusalBodyJson(verdict) ==
              "{\"error\":\"Host must be 127.0.0.1\"}",
          "a refusal is one JSON object with its reason: " +
              HttpRefusalBodyJson(verdict));
    verdict.reason = std::string("a \"quote\", a \\ and a\ttab\n");
    Check(HttpRefusalBodyJson(verdict) ==
              "{\"error\":\"a \\\"quote\\\", a \\\\ and a\\u0009tab\\u000a\"}",
          "a reason is escaped for JSON: " + HttpRefusalBodyJson(verdict));
    // Every sentence the rule can answer is plain enough to need no escape.
    LocalRequestConfig config;
    config.bearer_token = kToken;
    for (const HttpHeaderFields& headers : std::vector<HttpHeaderFields>{
             {{"Host", "evil.example"}},
             {{"Host", "a"}, {"Host", "b"}},
             {{"Origin", "null"}},
             {{"Origin", "a"}, {"Origin", "b"}},
             {},
             {{"Authorization", "Bearer wrong"}},
             {{"Authorization", std::string("Bearer ") + kToken}},
         }) {
        const HttpRequestVerdict refusal =
            CheckLocalRequest("POST", headers, config);
        Check(!refusal.Accepted() && !refusal.reason.empty() &&
                  HttpRefusalBodyJson(refusal) ==
                      "{\"error\":\"" + refusal.reason + "\"}",
              "the sentence of a " + std::to_string(refusal.status) +
                  " goes into JSON as it is: " + refusal.reason);
    }
}

void CheckHeaderLookup() {
    const HttpHeaderFields headers = {{"Host", " 127.0.0.1 "},
                                      {"X-Twice", "a"},
                                      {"x-twice", "b"},
                                      {" Content-Type\t", "\ttext/plain "}};
    std::string value;
    Check(
        FindHttpHeader(headers, "host", value) == HttpHeaderPresence::Single &&
            value == "127.0.0.1",
        "a field found once comes back trimmed: '" + value + "'");
    Check(FindHttpHeader(headers, "content-type", value) ==
                  HttpHeaderPresence::Single &&
              value == "text/plain",
          "blanks around a field name are not part of it");
    Check(FindHttpHeader(headers, "x-twice", value) ==
              HttpHeaderPresence::Repeated,
          "a field sent twice is reported as repeated");
    Check(
        FindHttpHeader(headers, "origin", value) == HttpHeaderPresence::Absent,
        "a field not sent is absent");
    Check(FindHttpHeader(headers, "hos", value) == HttpHeaderPresence::Absent &&
              FindHttpHeader(headers, "hostx", value) ==
                  HttpHeaderPresence::Absent,
          "a field name is matched whole");
}

// PRD13b -- ROADMAP.md. From 2025-06-18 on, a client over HTTP names the
// revision it negotiated in MCP-Protocol-Version, and a server answers 400 to
// one it does not implement. Without the header nothing changes.
const std::vector<std::string> kVersions = {"2024-11-05", "2025-03-26",
                                            "2025-06-18", "2025-11-25"};

HttpRequestVerdict WithVersions(const std::string& method,
                                const HttpHeaderFields& headers,
                                const std::vector<std::string>& versions,
                                const std::string& token = std::string()) {
    LocalRequestConfig config;
    config.bearer_token = token;
    config.protocol_versions = versions;
    return CheckLocalRequest(method, headers, config);
}

void CheckProtocolVersion() {
    Check(WithVersions("POST", LocalClient(), kVersions).Accepted(),
          "a request without MCP-Protocol-Version passes as it did");
    for (const std::string& version : kVersions) {
        Check(WithVersions("POST",
                           With(LocalClient(), "MCP-Protocol-Version", version),
                           kVersions)
                  .Accepted(),
              "the revision " + version + " is one the server implements");
    }
    Check(WithVersions(
              "POST",
              With(LocalClient(), "mcp-protocol-version", " 2025-06-18\t"),
              kVersions)
              .Accepted(),
          "the field's name has no case and its value is trimmed");
    for (const std::string& version :
         {"2026-07-28", "2025-06-19", "2025-6-18", "latest", "",
          "2025-06-18, 2025-11-25", "2025-11-25 extra"}) {
        const HttpRequestVerdict refused = WithVersions(
            "POST", With(LocalClient(), "MCP-Protocol-Version", version),
            kVersions);
        Check(refused.status == 400 &&
                  refused.reason.find("2024-11-05, 2025-03-26, 2025-06-18 or "
                                      "2025-11-25") != std::string::npos &&
                  refused.reason.find("left out") != std::string::npos,
              "MCP-Protocol-Version '" + version +
                  "' is refused with the revisions the server implements: " +
                  refused.reason);
        Check(version.empty() ||
                  refused.reason.find(version) == std::string::npos,
              "the refusal does not repeat what the request sent");
    }
    const HttpRequestVerdict twice = WithVersions(
        "POST",
        With(With(LocalClient(), "MCP-Protocol-Version", "2025-06-18"),
             "MCP-Protocol-Version", "2025-06-18"),
        kVersions);
    Check(twice.status == 400 &&
              twice.reason.find("more than once") != std::string::npos,
          "MCP-Protocol-Version sent twice is refused: " + twice.reason);
    Check(
        WithVersions("POST",
                     With(LocalClient(), "MCP-Protocol-Version", "garbage"), {})
            .Accepted(),
        "a server that names no revision does not look at the header");
    Check(WithVersions(
              "GET",
              {{"Host", "127.0.0.1"}, {"MCP-Protocol-Version", "1999-01-01"}},
              kVersions)
                  .status == 400,
          "the header is read on any method");
    // Who is talking, and how, before which revision is spoken.
    const HttpHeaderFields unknown =
        With(LocalClient(), "MCP-Protocol-Version", "1999-01-01");
    Check(WithVersions("POST", With(unknown, "Origin", "https://evil.example"),
                       kVersions)
                      .status == 403 &&
              WithVersions("POST", unknown, kVersions, kToken).status == 401 &&
              WithVersions("POST",
                           Replacing(unknown, "content-type", "text/plain"),
                           kVersions)
                      .status == 415,
          "Host, Origin, the token and the content type are settled first");
}

// PRD13b -- how long a body is (RFC 9112, section 6.3): one Content-Length,
// or a 400; a chunked body, 411; any other Transfer-Encoding, 400.
void CheckBodyFraming() {
    constexpr size_t kMax = 1000;
    const auto framing = [](const HttpHeaderFields& headers) {
        return ReadBodyFraming(headers, kMax);
    };
    Check(framing({}).Readable() && framing({}).length == 0,
          "neither Content-Length nor Transfer-Encoding: no body");
    Check(framing({{"Content-Length", "42"}}).Readable() &&
              framing({{"Content-Length", "42"}}).length == 42 &&
              framing({{"content-length", " 042 "}}).length == 42 &&
              framing({{"Content-Length", "0"}}).Readable() &&
              framing({{"Content-Length", "0"}}).length == 0,
          "one Content-Length is the body's length, whatever its case and "
          "leading zeros");
    Check(
        framing({{"Content-Length", "42"}, {"Content-Length", "42"}}).length ==
                42 &&
            framing({{"Content-Length", "42, 42"}}).length == 42 &&
            framing({{"Content-Length", "42,,042"}}).length == 42,
        "the same value sent twice, or as a list, is that value");
    for (const HttpHeaderFields& headers : std::vector<HttpHeaderFields>{
             {{"Content-Length", "42"}, {"Content-Length", "43"}},
             {{"Content-Length", "43"}, {"Content-Length", "42"}},
             {{"Content-Length", "42, 43"}},
             {{"Content-Length", "abc"}},
             {{"Content-Length", "-1"}},
             {{"Content-Length", "+5"}},
             {{"Content-Length", "5x"}},
             {{"Content-Length", "0x10"}},
             {{"Content-Length", "4 2"}},
             {{"Content-Length", ""}},
             {{"Content-Length", " , "}},
         }) {
        const HttpBodyFraming refused = framing(headers);
        Check(refused.status == 400 && refused.length == 0 &&
                  refused.reason.find("Content-Length must be one number") == 0,
              "a Content-Length that is not one number is a 400: '" +
                  headers.back().value + "'");
    }
    Check(framing({{"Content-Length", "1000"}}).Readable() &&
              framing({{"Content-Length", "1001"}}).status == 413 &&
              framing({{"Content-Length", "99999999999999999999999"}}).status ==
                  413,
          "a body past the limit is a 413, however many digits");
    for (const HttpHeaderFields& headers : std::vector<HttpHeaderFields>{
             {{"Transfer-Encoding", "chunked"}},
             {{"transfer-encoding", "Chunked"}},
             {{"Transfer-Encoding", "gzip, chunked"}},
             {{"Transfer-Encoding", "gzip"}, {"Transfer-Encoding", "chunked"}},
             {{"Transfer-Encoding", "chunked"}, {"Content-Length", "5"}},
             {{"Transfer-Encoding", "chunked;ext=1"}},
         }) {
        const HttpBodyFraming refused = framing(headers);
        Check(refused.status == 411 &&
                  refused.reason.find("Content-Length") != std::string::npos,
              "a chunked body asks for a Content-Length (411): " +
                  headers.front().value);
    }
    for (const HttpHeaderFields& headers : std::vector<HttpHeaderFields>{
             {{"Transfer-Encoding", "gzip"}},
             {{"Transfer-Encoding", "chunked, gzip"}},
             {{"Transfer-Encoding", ""}},
         }) {
        Check(framing(headers).status == 400,
              "a Transfer-Encoding that does not end with chunked leaves the "
              "length unknown (400): '" +
                  headers.front().value + "'");
    }
}

void CheckUsableToken() {
    for (const std::string& token :
         {"a", "s3cret-token", "ABCxyz019-._~+/=", "!#$%&'()*,:;<>?@[]^`{|}",
          "0f8fad5b-d9cb-469f-a165-70867728950e"}) {
        Check(IsUsableBearerToken(token),
              "'" + token + "' is a token a client can send");
    }
    for (const std::string& token :
         {std::string(), std::string(" "), std::string("two words"),
          std::string(" leading"), std::string("trailing "),
          std::string("tab\there"), std::string("line\n"),
          std::string("crlf\r"), std::string("nul\0byte", 8),
          std::string("del\x7f"), std::string("caf\xc3\xa9")}) {
        Check(!IsUsableBearerToken(token),
              "a token with a blank, a control or a non-ASCII character, or "
              "an empty one, is not usable (" +
                  std::to_string(token.size()) + " bytes)");
    }
}

}  // namespace

int main() {
    try {
        CheckHosts();
        CheckOrigins();
        CheckContentTypes();
        CheckToken();
        CheckOrder();
        CheckRefusalBody();
        CheckHeaderLookup();
        CheckProtocolVersion();
        CheckBodyFraming();
        CheckUsableToken();
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: HTTP request policy test threw: " << failure.what()
                  << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " HTTP request policy check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: the loopback HTTP server's request policy\n";
    return 0;
}
