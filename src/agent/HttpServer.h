#pragma once

// A minimal, blocking HTTP/1.1 server for exactly one purpose: carrying
// JSON-RPC 2.0 POST requests for the MCP server (McpServer.h). It is
// deliberately not a general-purpose web server:
//   - Binds to 127.0.0.1 only, never 0.0.0.0 or a public interface. This is
//     a purely local server; nothing here ever accepts a non-loopback
//     connection (ROADMAP.md F1.1 hard constraint: "no service you don't
//     control").
//   - One request per connection (`Connection: close`); no keep-alive, no
//     chunked transfer-encoding, no TLS. A local agent process making one
//     JSON-RPC call at a time does not need any of that. PRD13b -- a body is
//     read by its Content-Length alone (ReadBodyFraming,
//     HttpRequestPolicy.h): a chunked one is answered 411 (Length
//     Required), a Content-Length that is not one number 400, and a 405
//     names the one method allowed (`Allow: POST`).
//   - Plain POSIX sockets only (sys/socket.h, not AppKit/Metal/FFmpeg), so
//     it builds and is testable on a plain Linux host as well as macOS.
//
// PRD13 -- ROADMAP.md. Binding the loopback was not enough: it keeps other
// machines out, not a web page open in a browser on this one. Every request
// now goes through CheckLocalRequest (HttpRequestPolicy.h, where the attack
// and the rule are described) before the handler is called: Host and Origin
// must name the loopback when they are sent, a POST must declare
// application/json, and a bearer token is asked of every request when one is
// configured. A refused request gets 403, 415 or 401 with a JSON body saying
// what to correct, and never reaches the handler; so does, with 400, one
// whose MCP-Protocol-Version names a revision the server does not implement
// (PRD13b). No answer of this server carries an Access-Control-* header: a
// browser's preflight is never granted.
//
// !! DO NOT EXPOSE THIS BEYOND LOOPBACK AS IT STANDS. !!
//
// Anything that widens reach -- binding a non-loopback address, forwarding
// the port, tunnelling it, putting a proxy in front -- has to land together
// with the following, none of which exists yet:
//
//   - Transport security. There is no TLS: beyond the loopback the requests,
//     the answers and the bearer token would travel in the clear.
//   - Authentication by default. The token is optional, because existing
//     configurations point at the bare URL: without one, every local process
//     that can open a socket to the port is fully trusted and can mutate the
//     user's project. The Host and Origin rules are also written for the
//     loopback names only.
//   - Request limits beyond the size caps. IA5 bounded the worst of it: a
//     connection must deliver its whole request within the idle timeout
//     (10 s by default) and accept its answer at the same pace, so one that
//     opens and never sends no longer wedges the serial accept loop -- or
//     Stop() -- forever. A client can still hold the loop that long, and a
//     slow tool call holds it for as long as the call runs. There is no rate
//     limit.

#include "HttpRequestPolicy.h"

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class HttpServer {
public:
    // method/path/body in; statusCode/contentType/responseBody out. Called
    // synchronously on the server's accept thread -- one request is fully
    // handled before the next connection is accepted. Only called for a
    // request CheckLocalRequest accepted (PRD13).
    using Handler = std::function<void(
        const std::string& method, const std::string& path,
        const std::string& body, int& statusCode, std::string& contentType,
        std::string& responseBody)>;

    HttpServer();
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds 127.0.0.1:port (port 0 asks the OS for an ephemeral port --
    // see Port() to read back what was actually bound) and starts a
    // background accept loop. Returns false with `error` set on failure.
    bool Start(int port, Handler handler, std::string& error);

    // Stops the accept loop and joins its thread. Safe to call even if
    // Start() was never called or already failed.
    void Stop();

    bool IsRunning() const { return running_.load(); }

    // IA5 -- ROADMAP.md. The loop serves one connection at a time, so a
    // client that connects and says nothing used to block every other one
    // until it went away. A request must now arrive whole within this many
    // milliseconds of the connection, and each write of the answer complete
    // within as long; past that the connection is closed. Set before Start().
    static constexpr int kDefaultIdleTimeoutMilliseconds = 10000;
    void SetIdleTimeoutMilliseconds(int milliseconds) {
        idle_timeout_milliseconds_ = milliseconds;
    }

    // PRD13 -- ROADMAP.md. With a token, every request must carry
    // `Authorization: Bearer <token>` or is answered 401; empty (the
    // default) asks for none. The Host, Origin and content type rules do not
    // depend on it and cannot be turned off. Set before Start().
    void SetBearerToken(std::string token) {
        request_config_.bearer_token = std::move(token);
    }
    // PRD13b -- ROADMAP.md. The revisions of the MCP protocol a request may
    // name in its MCP-Protocol-Version header; another is answered 400
    // (HttpRequestPolicy.h). Empty, the default: the header is not looked
    // at. Set before Start().
    void SetProtocolVersions(std::vector<std::string> versions) {
        request_config_.protocol_versions = std::move(versions);
    }

    // Valid only while IsRunning(); the actual bound port (useful when
    // Start() was called with port 0).
    int Port() const { return port_; }

private:
    void AcceptLoop();

    Handler handler_;
    LocalRequestConfig request_config_;
    // Read by the accept-loop thread on every iteration and written by
    // Stop() from whichever thread calls it; must be atomic.
    std::atomic<int> listen_fd_{-1};
    int port_ = 0;
    int idle_timeout_milliseconds_ = kDefaultIdleTimeoutMilliseconds;
    std::thread thread_;
    std::atomic_bool running_{false};
};
