#include "HttpServer.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <sstream>

namespace {

std::string Trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin])))
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])))
        --end;
    return text.substr(begin, end - begin);
}

using Deadline = std::chrono::steady_clock::time_point;

// IA5 -- one recv() of whatever has arrived, waiting no later than
// `deadline`: a silent client costs the serial loop at most the idle timeout
// for its whole request, not a timeout per byte it trickles in.
ssize_t ReceiveBefore(int fd, char* chunk, size_t size, Deadline deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
        return -1;
    }
    pollfd waiting{fd, POLLIN, 0};
    if (::poll(&waiting, 1, static_cast<int>(remaining.count())) <= 0) {
        return -1;
    }
    return ::recv(fd, chunk, size, 0);
}

// Reads exactly `count` more bytes into `buffer`, blocking as needed.
// Returns false on EOF/error/deadline before `count` bytes were read.
bool ReadExact(int fd, std::string& buffer, size_t count, Deadline deadline) {
    char chunk[4096];
    while (buffer.size() < count) {
        const size_t want = std::min(sizeof(chunk), count - buffer.size());
        const ssize_t got = ReceiveBefore(fd, chunk, want, deadline);
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(got));
    }
    return true;
}

// PRD13 -- ROADMAP.md. Reads `count` more bytes and keeps none: what is left
// of the body of a request the policy refused. A refusal is answered like
// any other request, once the request has been read whole. Closing on bytes
// the client is still sending can reset the connection, and on a stack that
// then drops what the client had not read yet, the client reports the reset
// instead of the refusal telling it what to correct. (macOS over the
// loopback is not such a stack: measured with curl and a 4 MB body, the
// refusal is read either way.) A refused body has no business in memory, so
// none of it is kept. Bounded like any read by `deadline`.
void DiscardBytes(int fd, size_t count, Deadline deadline) {
    char chunk[4096];
    while (count > 0) {
        const ssize_t got =
            ReceiveBefore(fd, chunk, std::min(sizeof(chunk), count), deadline);
        if (got <= 0) {
            return;
        }
        count -= static_cast<size_t>(got);
    }
}

// Reads until the buffer contains the header terminator, growing `buffer`
// as needed. Returns false on EOF/error/oversized headers/deadline before
// that.
bool ReadHeaders(int fd, std::string& buffer, Deadline deadline) {
    constexpr size_t kMaxHeaderBytes = 64 * 1024;
    char chunk[4096];
    while (buffer.find("\r\n\r\n") == std::string::npos) {
        if (buffer.size() > kMaxHeaderBytes) return false;
        const ssize_t got = ReceiveBefore(fd, chunk, sizeof(chunk), deadline);
        if (got <= 0) return false;
        buffer.append(chunk, static_cast<size_t>(got));
    }
    return true;
}

void SendAll(int fd, const std::string& data) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t wrote =
            ::send(fd, data.data() + sent, data.size() - sent, kFlags);
        if (wrote <= 0) return;
        sent += static_cast<size_t>(wrote);
    }
}

const char* ReasonPhrase(int status) {
    switch (status) {
        case 200:
            return "OK";
        case 202:
            return "Accepted";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 411:
            return "Length Required";
        case 413:
            return "Payload Too Large";
        case 415:
            return "Unsupported Media Type";
        default:
            return "Internal Server Error";
    }
}

}  // namespace

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { Stop(); }

bool HttpServer::Start(int port, Handler handler, std::string& error) {
    if (running_.load()) {
        error = "server is already running";
        return false;
    }
    handler_ = std::move(handler);

    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        error = "socket() failed";
        return false;
    }
    const int reuse = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    // Loopback only, deliberately: this server is never reachable from
    // outside the host (ROADMAP.md F1.1: "no service you don't control").
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        error = "bind(127.0.0.1:" + std::to_string(port) + ") failed";
        ::close(fd);
        return false;
    }
    if (::listen(fd, 16) != 0) {
        error = "listen() failed";
        ::close(fd);
        return false;
    }

    sockaddr_in bound{};
    socklen_t boundLength = sizeof(bound);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &boundLength) ==
        0) {
        port_ = ntohs(bound.sin_port);
    } else {
        port_ = port;
    }

    listen_fd_ = fd;
    running_.store(true);
    thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

void HttpServer::Stop() {
    if (!running_.exchange(false)) return;
    const int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    if (thread_.joinable()) thread_.join();
}

void HttpServer::AcceptLoop() {
    while (running_.load()) {
        const int fd = listen_fd_.load();
        if (fd < 0) break;
        sockaddr_in peer{};
        socklen_t peerLength = sizeof(peer);
        const int connection =
            ::accept(fd, reinterpret_cast<sockaddr*>(&peer), &peerLength);
        if (connection < 0) {
            // Stop() closing listen_fd_ unblocks accept() with an error;
            // treat any accept failure while stopped as a clean exit.
            if (!running_.load()) break;
            continue;
        }
        // Loopback-only defense in depth: refuse anything that somehow
        // reached us from a non-loopback source address.
        if (ntohl(peer.sin_addr.s_addr) != INADDR_LOOPBACK) {
            ::close(connection);
            continue;
        }
        const int noDelay = 1;
        ::setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &noDelay,
                     sizeof(noDelay));
        // IA5 -- a client that gave up on a long call and closed its end
        // must not take the server down with it: writing the late answer to
        // that socket raised SIGPIPE, whose default action ends the process.
#ifdef SO_NOSIGPIPE
        const int noSigPipe = 1;
        ::setsockopt(connection, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe,
                     sizeof(noSigPipe));
#endif
        timeval sendTimeout{};
        sendTimeout.tv_sec = idle_timeout_milliseconds_ / 1000;
        sendTimeout.tv_usec = (idle_timeout_milliseconds_ % 1000) * 1000;
        ::setsockopt(connection, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout,
                     sizeof(sendTimeout));
        const Deadline deadline =
            std::chrono::steady_clock::now() +
            std::chrono::milliseconds(idle_timeout_milliseconds_);

        std::string buffer;
        if (!ReadHeaders(connection, buffer, deadline)) {
            ::close(connection);
            continue;
        }
        const size_t headerEnd = buffer.find("\r\n\r\n");
        const std::string headerBlock = buffer.substr(0, headerEnd);
        std::string body = buffer.substr(headerEnd + 4);

        std::istringstream lines(headerBlock);
        std::string requestLine;
        std::getline(lines, requestLine);
        if (!requestLine.empty() && requestLine.back() == '\r')
            requestLine.pop_back();
        std::istringstream requestLineStream(requestLine);
        std::string method;
        std::string path;
        std::string httpVersion;
        requestLineStream >> method >> path >> httpVersion;

        // PRD13 -- every field line as it came, for the request policy: a
        // map would keep one Host out of two, and the policy must see both.
        // PRD13b -- and so must the body's framing: two Content-Length lines
        // that disagree used to be read as the last one.
        HttpHeaderFields fields;
        std::string headerLine;
        while (std::getline(lines, headerLine)) {
            if (!headerLine.empty() && headerLine.back() == '\r')
                headerLine.pop_back();
            if (headerLine.empty()) continue;
            const size_t colon = headerLine.find(':');
            if (colon == std::string::npos) continue;
            fields.push_back({Trim(headerLine.substr(0, colon)),
                              Trim(headerLine.substr(colon + 1))});
        }

        int statusCode = 200;
        std::string contentType = "application/json";
        std::string responseBody;

        if (method.empty() || path.empty()) {
            statusCode = 400;
            responseBody = "{\"error\":\"malformed request line\"}";
        } else {
            constexpr size_t kMaxBodyBytes = size_t{32} * 1024 * 1024;
            // PRD13b -- ROADMAP.md. How long the body is, read by the same
            // pure rule as who may send it (HttpRequestPolicy.h): a
            // Content-Length that is not one number of bytes is a 400, a
            // chunked body a 411, and neither is read.
            const HttpBodyFraming framing =
                ReadBodyFraming(fields, kMaxBodyBytes);
            // PRD13 -- who is talking is settled before anything else is
            // looked at: a request the policy refuses never reaches the
            // handler, whatever its path, its method or its body.
            const HttpRequestVerdict verdict =
                CheckLocalRequest(method, fields, request_config_);
            if (!verdict.Accepted()) {
                if (framing.Readable() && framing.length > body.size()) {
                    DiscardBytes(connection, framing.length - body.size(),
                                 deadline);
                }
                statusCode = verdict.status;
                responseBody = HttpRefusalBodyJson(verdict);
            } else if (!framing.Readable()) {
                statusCode = framing.status;
                responseBody = HttpRefusalBodyJson(
                    HttpRequestVerdict{framing.status, framing.reason});
            } else if (!ReadExact(connection, body, framing.length, deadline)) {
                statusCode = 400;
                responseBody = "{\"error\":\"truncated request body\"}";
            } else {
                if (body.size() > framing.length) body.resize(framing.length);
                handler_(method, path, body, statusCode, contentType,
                         responseBody);
            }
        }

        std::ostringstream response;
        response << "HTTP/1.1 " << statusCode << " " << ReasonPhrase(statusCode)
                 << "\r\n"
                 << "Content-Type: " << contentType << "\r\n"
                 << "Content-Length: " << responseBody.size() << "\r\n";
        // PRD13 -- a 401 names the scheme it expects (RFC 9110 11.6.1), and
        // nothing more: no realm, and certainly not the token.
        if (statusCode == 401) {
            response << "WWW-Authenticate: Bearer\r\n";
        }
        // PRD13b -- a 405 lists the methods the resource does allow (RFC
        // 9110 15.5.6, a MUST): POST is the only one this server answers. A
        // GET is how a client of the protocol asks for an event stream, and
        // 405 is how a server without one says no.
        if (statusCode == 405) {
            response << "Allow: POST\r\n";
        }
        response << "Connection: close\r\n\r\n" << responseBody;
        SendAll(connection, response.str());
        ::shutdown(connection, SHUT_WR);
        ::close(connection);
    }
}
