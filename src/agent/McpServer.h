#pragma once

// MCP server: JSON-RPC 2.0 over HTTP (ROADMAP.md F1.1), implementing the
// Model Context Protocol's `initialize`, `tools/list` and `tools/call`
// methods on top of HttpServer.h -- and, since PRD18, `prompts/list` and
// `prompts/get`, one prompt per procedure the binary carries
// (Procedures.h). Tool semantics live entirely in McpTools.cc/McpBackend.h;
// this file only owns the JSON-RPC envelope: request parsing, method
// dispatch, response/notification/error shaping.
//
// Deliberate scope limits versus the full MCP "Streamable HTTP" transport:
//   - Every response is a single `application/json` value (never an SSE
//     stream): one object, or the array that answers a batch. The spec
//     allows this for transports that never need to stream additional
//     server-initiated messages, which is true here: this server never
//     sends unsolicited notifications, and answers a GET 405.
//   - A JSON-RPC request with no "id" (a notification, e.g.
//     `notifications/initialized`) is accepted and acknowledged with an
//     empty HTTP 202, matching "notifications get no JSON-RPC response"; so
//     is a batch of notifications. A JSON-RPC response posted by a client is
//     answered 400 (PRD13b): this server sends no requests, so there is
//     nothing such a response could answer.
//   - Tool execution failures (unknown clip, overlap, unknown tool name,
//     bad arguments, ...) are reported as a `tools/call` result with
//     `isError:true`, never as a JSON-RPC protocol-level error -- per spec,
//     that is what lets the calling agent see and react to the failure.
//     JSON-RPC error responses are reserved for envelope-level problems:
//     malformed JSON, an unknown method, or malformed `tools/call` params.
//
// Every `tools/call` that reaches this server is executed with the full
// authority of the user's project -- there is no consent prompt or audit
// gate in front of it. Over HTTP that is only tenable because HttpServer.h
// binds loopback and nothing else, and because it refuses the requests a web
// page of this machine's browser could send (PRD13, HttpRequestPolicy.h);
// a bearer token can be asked on top of that (SetBearerToken), and none is
// by default. Read the warning at the top of HttpServer.h before changing
// how this server is reached.
//
// PRD13 -- ROADMAP.md. `initialize` used to answer whatever protocol version
// the client asked for, so the server claimed revisions it had never heard
// of. It now answers the requested version when it is one of
// McpProtocolVersions(), and the latest of them otherwise, which is what the
// protocol's version negotiation asks of a server.
//
// AGENT-2026-09 A1 -- ROADMAP.md. The same envelope also serves the MCP
// "stdio" transport (ServeStdio): one JSON-RPC message per line on stdin,
// one per line on stdout, nothing else on stdout. That is how an MCP client
// such as Claude Code spawns a server itself from a `.mcp.json` -- an HTTP
// server has to be started by hand first, and until this transport existed
// every session in this repository reached the tools through
// `--call-tool` in a shell, without the deltas, tasks, idempotency keys and
// plans only a running server offers. Authority is the same as over HTTP:
// whoever can spawn the process can edit the project, which is the client
// the user configured.

#include "HttpServer.h"
#include "McpBackend.h"
#include "McpSession.h"
#include "McpTools.h"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// PRD13 -- ROADMAP.md. The revisions of the protocol `initialize` agrees to,
// oldest first; the last one answers a request for any other.
//
// They are the four revisions built on the `initialize` handshake, which is
// all this server implements. Claude Code negotiates 2025-11-25 with it
// (measured on 2 October 2026), and a version this server granted yesterday
// is still granted today.
//
// PRD13b -- ROADMAP.md. What these revisions ask of a server that answers in
// JSON is held, read on the published specification on 3 October 2026. A
// JSON-RPC batch, which 2025-03-26 requires a server to receive, is answered
// as one on both transports, whatever the version negotiated: the later
// revisions only stopped asking for batches, and refusing them there would
// take a state per connection this server does not keep. Over HTTP, from
// 2025-06-18 on, a request whose MCP-Protocol-Version header names a
// revision not in this list is answered 400 (HttpRequestPolicy.h), and a
// request without the header is served as before. A JSON-RPC response
// posted by a client is answered 400 over HTTP, and passed over on stdio.
//
// Left out, as the transports page allows: no SSE stream (a GET is answered
// 405, `Allow: POST`), so the server never speaks first, and no
// Mcp-Session-Id. Over HTTP a client of 2024-11-05 expects that revision's
// HTTP+SSE transport, which this server does not have: it reaches the
// server over stdio.
//
// 2026-07-28 is not in the list. That revision has no handshake: the version
// travels in every request's `_meta`, and a server must implement
// `server/discover` and answer UnsupportedProtocolVersionError. None of it
// exists here, so a client asking for it is answered 2025-11-25, and one
// probing with `server/discover` gets -32601, which is how such a client
// recognises a server of the handshake era and falls back to `initialize`.
const std::vector<std::string>& McpProtocolVersions();

// The version `initialize` answers to a client asking for `requested`.
std::string NegotiateMcpProtocolVersion(const std::string& requested);

// PRD13b -- ROADMAP.md. What a request body, or a line of the stdio
// transport, calls for.
enum class McpReply {
    // The answer: one JSON-RPC response, or the array of them that answers
    // a batch. HTTP answers 200.
    kAnswer,
    // Notifications only: nothing to answer. HTTP answers 202, stdio writes
    // nothing.
    kNone,
    // JSON-RPC responses, and notifications at most: a client answering a
    // request this server never sent. HTTP answers 400, the body a JSON-RPC
    // error without an id; stdio passes over it, as over a notification.
    kClientResponse,
};

// Whether McpServer::HandleJsonRpc answers `message`, read on its shape the
// way the server reads it. A relay (McpEndpoint.h) waits for an answer
// exactly when this is true: waiting for one that never comes would block
// it, and not reading one would leave it in front of the next.
bool McpJsonRpcGetsAnswer(const std::string& message);

// PRD13b -- the value of `--mcp-serve --port`: a whole number from 1 to
// 65535, or 0 for a free port the system picks (the banner prints it; the
// tests start their servers so). False, with `error` naming the value,
// otherwise: "abc" used to be read as 0 without a word.
bool ParseMcpPort(const std::string& text, int& port, std::string& error);

// PRD13 -- ROADMAP.md. The bearer token `--mcp-serve --port` asks of its
// clients, and where it came from.
struct McpHttpToken {
    // Empty: none is asked.
    std::string token;
    // "--token" or "CUTMACHINE_MCP_TOKEN": what a banner may print. The
    // token itself is printed nowhere.
    std::string source;
};

// `option` is the value given to --token, or null when the option was not
// given; it wins. Without it the token is CUTMACHINE_MCP_TOKEN, read like
// every machine-local setting: from the environment, else from the settings
// file (LocalEnv.h). Neither set: no token, and the answer is true.
//
// False, with `error`, when the one that applies is empty or could not be
// sent in a header (IsUsableBearerToken). A token asked for and not usable
// stops the server from starting rather than leaving it open: an empty value
// is what a shell substitutes for a file that was not there. `error` names
// the source and never repeats the value.
bool ResolveMcpHttpToken(const std::string* option, McpHttpToken& resolved,
                         std::string& error);

class McpServer {
public:
    // Does not take ownership of `backend`; it must outlive the server.
    // PROFIL-2026-11 PF1 -- `profile` chooses the tools it lists, accepts
    // and names in its instructions (McpToolProfiles.h).
    explicit McpServer(McpBackend& backend,
                       const McpToolProfile& profile = FullMcpToolProfile());

    // PF1 -- what the profile serves, for a relay in front of the server
    // (McpEndpoint.h) that answers for it what the app's catalog would not.
    const McpToolRegistry& Tools() const { return tools_; }

    // Starts listening on 127.0.0.1:port (0 asks the OS for a free port --
    // see Port()). The single path handled is `path` (default "/mcp");
    // everything else gets HTTP 404.
    bool Start(int port, std::string& error, const std::string& path = "/mcp");
    void Stop();
    bool IsRunning() const { return http_.IsRunning(); }
    int Port() const { return http_.Port(); }

    // Exposed for tests that want to exercise the JSON-RPC envelope
    // directly without going through a real HTTP round trip. Returns true
    // and fills `responseBody` for an ordinary request; returns false (no
    // body) for a notification, mirroring HTTP 202-with-no-body. PRD13b --
    // a batch with a request in it is answered (an array), a JSON-RPC
    // response from the client is not; the HTTP transport tells the two
    // cases of false apart by HandleJsonRpcMessage.
    bool HandleJsonRpc(const std::string& requestBody,
                       std::string& responseBody);
    McpReply HandleJsonRpcMessage(const std::string& requestBody,
                                  std::string& responseBody);

    // The stdio transport: reads `input` line by line until end of file,
    // handles each non-blank line as one JSON-RPC message and writes each
    // answer as one line on `output`, flushed at once; a notification gets
    // no line. Blocks the calling thread for the life of the connection, so
    // a client's requests are served one at a time in order, as over HTTP.
    // Returns 0 when the client closed its end, 1 when `input` or `output`
    // failed. Not to be combined with Start(): one transport per server.
    int ServeStdio(std::FILE* input, std::FILE* output);

    // IA5 -- ROADMAP.md. How long a connection may stay silent before the
    // server drops it (HttpServer.h). Set before Start().
    void SetIdleTimeoutMilliseconds(int milliseconds) {
        http_.SetIdleTimeoutMilliseconds(milliseconds);
    }
    // PRD13 -- ROADMAP.md. The bearer token every HTTP request must carry
    // (HttpServer.h); empty, the default, asks for none. Set before Start().
    // It has no meaning for ServeStdio: whoever holds the pipe is the client.
    void SetBearerToken(std::string token) {
        http_.SetBearerToken(std::move(token));
    }
    // Background tasks and idempotency keys live here, for as long as the
    // server runs (McpSession.h).
    McpSession& Session() { return session_; }

    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). Each `tools/call` this
    // server answered, alone or in a batch, once its answer is made and
    // before it is written: the tool, the class the catalog declares for it,
    // the arguments as the client sent them and the outcome as the client
    // gets it (the session's `revision` included). On the thread that
    // handles the request. Only the app's endpoint sets one, to show its
    // window what an agent does (McpEndpoint.h): the engine log's line is
    // McpToolRegistry::Call's, and this is not a second log. Set before the
    // first request.
    using ToolCallObserver = std::function<void(
        const std::string& tool, McpToolClass toolClass,
        const mcp_json::Value& arguments, const McpToolCallOutcome& outcome)>;
    void SetToolCallObserver(ToolCallObserver observer) {
        tool_call_observer_ = std::move(observer);
    }
    // The class the catalog declares for `tool`, Unclassified for a name it
    // does not know.
    McpToolClass ToolClassOf(const std::string& tool) const;

private:
    // The answer to one request, alone or in a batch (`inBatch`), where
    // `initialize` is refused.
    std::string HandleRequest(const mcp_json::Value& request, bool inBatch);

    McpBackend& backend_;
    McpToolRegistry tools_;
    // After tools_ and before http_: the HTTP thread is joined before the
    // session goes, and the session's worker before the catalog it runs.
    McpSession session_;
    HttpServer http_;
    std::string path_;
    ToolCallObserver tool_call_observer_;
};
