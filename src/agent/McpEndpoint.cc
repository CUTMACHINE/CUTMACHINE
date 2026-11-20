#include "McpEndpoint.h"

#include "Json.h"
#include "McpProjectBackend.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <utility>

namespace {

using mcp_json::Value;

constexpr char kEndpointFileName[] = "mcp-endpoint";

bool FillAddress(const std::string& path, sockaddr_un& address,
                 std::string& error) {
    std::memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path)) {
        error = "socket path '" + path + "' does not fit a Unix socket (" +
                std::to_string(sizeof(address.sun_path) - 1) + " bytes)";
        return false;
    }
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    return true;
}

// A client that went away must not take the app down with it: writing to
// its closed socket raises SIGPIPE, whose default action ends the process.
void IgnoreSigPipe(int fd) {
#ifdef SO_NOSIGPIPE
    const int on = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

bool WriteAll(int fd, const std::string& data) {
#ifdef MSG_NOSIGNAL
    constexpr int kFlags = MSG_NOSIGNAL;
#else
    constexpr int kFlags = 0;
#endif
    size_t written = 0;
    while (written < data.size()) {
        const ssize_t sent =
            ::send(fd, data.data() + written, data.size() - written, kFlags);
        if (sent < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        written += static_cast<size_t>(sent);
    }
    return true;
}

// One JSON-RPC message per line, as over stdio.
class LineReader {
public:
    explicit LineReader(int fd) : fd_(fd) {}

    bool Next(std::string& line) {
        for (;;) {
            const size_t end = buffer_.find('\n');
            if (end != std::string::npos) {
                line = buffer_.substr(0, end);
                buffer_.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                return true;
            }
            char chunk[65536];
            const ssize_t received = ::recv(fd_, chunk, sizeof(chunk), 0);
            if (received < 0 && errno == EINTR) continue;
            if (received <= 0) return false;
            buffer_.append(chunk, static_cast<size_t>(received));
        }
    }

private:
    int fd_;
    std::string buffer_;
};

bool IsBlank(const std::string& line) {
    return line.find_first_not_of(" \t") == std::string::npos;
}

// The method, the tool of a tools/call, and the id of a JSON-RPC message;
// empty for what does not parse, which the server then answers itself.
struct Message {
    std::string method;
    std::string tool;
    const Value* id = nullptr;
    Value root;
};

bool ReadMessage(const std::string& line, Message& message) {
    std::string error;
    if (!Value::Parse(line, message.root, error) || !message.root.IsObject())
        return false;
    message.id = message.root.Find("id");
    if (const Value* method = message.root.Find("method");
        method != nullptr && method->IsString())
        message.method = method->AsString();
    if (message.method == "tools/call") {
        if (const Value* params = message.root.Find("params");
            params != nullptr && params->IsObject()) {
            if (const Value* name = params->Find("name");
                name != nullptr && name->IsString())
                message.tool = name->AsString();
        }
    }
    return true;
}

// The client-side end of a relay: one connection to the app's endpoint.
class AppLink {
public:
    ~AppLink() { Close(); }

    bool Connect(const std::string& socketPath, std::string& error) {
        Close();
        sockaddr_un address{};
        if (!FillAddress(socketPath, address, error)) return false;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            error = std::strerror(errno);
            return false;
        }
        if (::connect(fd, reinterpret_cast<sockaddr*>(&address),
                      sizeof(address)) != 0) {
            error = std::strerror(errno);
            ::close(fd);
            return false;
        }
        IgnoreSigPipe(fd);
        fd_ = fd;
        reader_ = std::make_unique<LineReader>(fd);
        return true;
    }
    bool Connected() const { return fd_ >= 0; }
    bool Send(const std::string& line) {
        return Connected() && WriteAll(fd_, line + "\n");
    }
    bool Receive(std::string& line) {
        return Connected() && reader_->Next(line);
    }
    void Close() {
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
        reader_.reset();
    }

private:
    int fd_ = -1;
    std::unique_ptr<LineReader> reader_;
};

// What the client gets for the call the app did not answer: a refusal it
// can read, in the shape of any other tool failure, and nothing it could
// mistake for the app's own answer.
std::string AppClosedAnswer(const Message& message) {
    const std::string detail =
        "CUTMACHINE closed the connection before answering (the app quit or "
        "closed the project); whether this call was applied is unknown: call "
        "open_project again, which opens the project here now that the app "
        "released it, and read the revision";
    Value answer = Value::MakeObject();
    answer.Set("jsonrpc", Value::MakeString("2.0"));
    answer.Set("id", message.id != nullptr ? *message.id : Value::MakeNull());
    if (message.method == "tools/call") {
        Value payload = Value::MakeObject();
        payload.Set("ok", Value::MakeBool(false));
        payload.Set("error", Value::MakeString("AppClosed"));
        payload.Set("detail", Value::MakeString(detail));
        Value text = Value::MakeObject();
        text.Set("type", Value::MakeString("text"));
        text.Set("text", Value::MakeString(payload.Dump()));
        Value content = Value::MakeArray();
        content.Push(std::move(text));
        Value result = Value::MakeObject();
        result.Set("content", std::move(content));
        result.Set("isError", Value::MakeBool(true));
        answer.Set("result", std::move(result));
    } else {
        Value error = Value::MakeObject();
        error.Set("code", Value::MakeInt(-32000));
        error.Set("message", Value::MakeString(detail));
        answer.Set("error", std::move(error));
    }
    return answer.Dump();
}

// PRD25 -- the string `key` holds in `object`, or empty.
std::string TextOf(const Value& object, const char* key) {
    const Value* value = object.IsObject() ? object.Find(key) : nullptr;
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

bool TrueIn(const Value& object, const char* key) {
    const Value* value = object.Find(key);
    return value != nullptr && value->IsBool() && value->AsBool();
}

// The revision a server answer leads with (McpSession.cc writes it first),
// read without parsing the answer: a read's answer can be a 250 KB describe.
std::string LeadingRevision(const std::string& answer) {
    static const std::string kMember = "\"revision\":\"";
    const size_t open = answer.find_first_not_of(" \t\r\n");
    if (open == std::string::npos || answer[open] != '{' ||
        answer.compare(open + 1, kMember.size(), kMember) != 0) {
        return {};
    }
    const size_t start = open + 1 + kMember.size();
    const size_t end = answer.find('"', start);
    return end == std::string::npos ? std::string()
                                    : answer.substr(start, end - start);
}

}  // namespace

McpDeltaSummary SummarizeMcpDelta(const Value& delta) {
    McpDeltaSummary summary;
    if (!delta.IsObject()) {
        return summary;
    }
    if (const Value* clips = delta.Find("clips");
        clips != nullptr && clips->IsArray()) {
        for (const Value& clip : clips->AsArray()) {
            const std::string id = TextOf(clip, "clip_id");
            if (id.empty()) {
                continue;
            }
            if (clip.IsObject() && TrueIn(clip, "created")) {
                ++summary.created;
                summary.created_clip_ids.push_back(id);
            } else {
                ++summary.changed;
                summary.changed_clip_ids.push_back(id);
            }
        }
    }
    if (const Value* removed = delta.Find("removed_clip_ids");
        removed != nullptr && removed->IsArray()) {
        for (const Value& id : removed->AsArray()) {
            if (id.IsString()) {
                ++summary.removed;
                summary.removed_clip_ids.push_back(id.AsString());
            }
        }
    }
    if (const Value* shifted = delta.Find("shifted");
        shifted != nullptr && shifted->IsArray()) {
        for (const Value& rule : shifted->AsArray()) {
            const Value* count = rule.IsObject() ? rule.Find("count") : nullptr;
            int64_t moved = 0;
            if (count != nullptr && count->AsInt64(moved) && moved > 0) {
                summary.shifted += moved;
            }
        }
    }
    return summary;
}

bool McpToolClassActs(McpToolClass toolClass) {
    switch (toolClass) {
        case McpToolClass::Analysis:
        case McpToolClass::JournaledEdit:
        case McpToolClass::ProjectEdit:
        case McpToolClass::FileWrite:
        case McpToolClass::Unjournaled:
        case McpToolClass::External:
            return true;
        case McpToolClass::Unclassified:
        case McpToolClass::Read:
        case McpToolClass::Session:
        case McpToolClass::ExternalRead:
            return false;
    }
    return false;
}

std::string McpEndpointFilePath(const std::string& projectPath) {
    return (std::filesystem::path(projectPath + ".lock") / kEndpointFileName)
        .string();
}

bool FindMcpAppEndpoint(const std::string& projectPath,
                        std::string& socketPath) {
    std::ifstream input(McpEndpointFilePath(projectPath));
    std::string line;
    if (!input || !std::getline(input, line) || line.empty()) return false;
    // Only a socket this user owns: the announcement sits next to the
    // project, which another account may be able to write.
    struct stat status {};
    if (::lstat(line.c_str(), &status) != 0 || !S_ISSOCK(status.st_mode) ||
        status.st_uid != ::getuid())
        return false;
    socketPath = line;
    return true;
}

bool PrepareMcpSocketPath(const std::string& name, std::string& socketPath,
                          std::string& error) {
    const std::string directory =
        "/tmp/cutmachine-" + std::to_string(::getuid());
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) {
        error = "cannot create '" + directory + "': " + std::strerror(errno);
        return false;
    }
    struct stat status {};
    if (::lstat(directory.c_str(), &status) != 0 || !S_ISDIR(status.st_mode) ||
        status.st_uid != ::getuid() || (status.st_mode & 077) != 0) {
        error = "'" + directory +
                "' is not a directory this user alone can enter; remove it";
        return false;
    }
    // Sockets nobody listens on any more: an app killed, or a test that
    // ended without quitting. Each is one directory entry; they would pile
    // up with every launch otherwise.
    std::error_code listError;
    for (std::filesystem::directory_iterator item(directory, listError), end;
         !listError && item != end; item.increment(listError)) {
        const std::string candidate = item->path().string();
        struct stat entry {};
        sockaddr_un probe{};
        std::string ignored;
        if (item->path().extension() != ".sock" ||
            ::lstat(candidate.c_str(), &entry) != 0 ||
            !S_ISSOCK(entry.st_mode) || entry.st_uid != ::getuid() ||
            !FillAddress(candidate, probe, ignored))
            continue;
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) break;
        if (::connect(fd, reinterpret_cast<sockaddr*>(&probe), sizeof(probe)) !=
                0 &&
            errno == ECONNREFUSED)
            ::unlink(candidate.c_str());
        ::close(fd);
    }
    socketPath = directory + "/" + name + ".sock";
    sockaddr_un address{};
    return FillAddress(socketPath, address, error);
}

McpEndpoint::McpEndpoint(McpBackend& backend)
    : backend_(backend), server_(backend) {
    // PRD25 -- every call the server answers becomes an event, a batch's
    // included, read from the outcome itself rather than from the text of
    // the reply.
    server_.SetToolCallObserver(
        [this](const std::string& tool, McpToolClass toolClass,
               const Value& arguments, const McpToolCallOutcome& outcome) {
            RecordToolCall(tool, toolClass, arguments, outcome);
        });
}

McpEndpoint::~McpEndpoint() { Stop(); }

bool McpEndpoint::Start(const std::string& socketPath, std::string& error) {
    if (running_.load()) {
        error = "the endpoint is already listening on '" + socket_path_ + "'";
        return false;
    }
    sockaddr_un address{};
    if (!FillAddress(socketPath, address, error)) return false;
    struct stat status {};
    if (::lstat(socketPath.c_str(), &status) == 0) {
        // A socket left by an app that did not quit cleanly.
        if (!S_ISSOCK(status.st_mode) || status.st_uid != ::getuid()) {
            error = "'" + socketPath + "' exists and is not this user's socket";
            return false;
        }
        ::unlink(socketPath.c_str());
    }
    const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        error = std::strerror(errno);
        return false;
    }
    if (::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
            0 ||
        ::chmod(socketPath.c_str(), 0600) != 0 || ::listen(fd, 8) != 0) {
        error =
            "cannot listen on '" + socketPath + "': " + std::strerror(errno);
        ::close(fd);
        ::unlink(socketPath.c_str());
        return false;
    }
    socket_path_ = socketPath;
    listen_fd_.store(fd);
    running_.store(true);
    accept_thread_ = std::thread([this] { AcceptLoop(); });
    return true;
}

bool McpEndpoint::Announce(const std::string& projectPath, std::string& error) {
    if (!running_.load()) {
        error = "the endpoint is not listening";
        return false;
    }
    const std::filesystem::path file(McpEndpointFilePath(projectPath));
    const std::filesystem::path staging =
        file.string() + ".tmp-" + std::to_string(::getpid());
    {
        std::ofstream output(staging, std::ios::trunc);
        output << socket_path_ << '\n';
        output.close();
        if (!output) {
            error = "cannot write '" + staging.string() + "'";
            return false;
        }
    }
    std::error_code renameError;
    std::filesystem::rename(staging, file, renameError);
    if (renameError) {
        std::error_code ignored;
        std::filesystem::remove(staging, ignored);
        error = "cannot announce the endpoint in '" + file.string() +
                "': " + renameError.message();
        return false;
    }
    endpoint_file_ = file.string();
    return true;
}

void McpEndpoint::SetObserver(std::function<void(const Event&)> observer) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    observer_ = std::move(observer);
}

void McpEndpoint::Notify(const Event& event) {
    std::function<void(const Event&)> observer;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        observer = observer_;
    }
    if (observer) observer(event);
}

void McpEndpoint::AcceptLoop() {
    while (running_.load()) {
        const int fd = listen_fd_.load();
        if (fd < 0) break;
        const int connection = ::accept(fd, nullptr, nullptr);
        if (connection < 0) {
            if (!running_.load()) break;
            continue;
        }
        IgnoreSigPipe(connection);
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!running_.load()) {
            ::close(connection);
            break;
        }
        connections_.push_back(connection);
        const uint64_t id = ++last_connection_;
        threads_.emplace_back(
            [this, connection, id] { Serve(connection, id); });
    }
}

void McpEndpoint::RecordToolCall(const std::string& tool,
                                 McpToolClass toolClass, const Value& arguments,
                                 const McpToolCallOutcome& outcome) {
    Event event;
    event.kind = Event::Kind::ToolCall;
    event.tool = tool;
    event.ok = outcome.ok;
    event.connection = serving_connection_;
    event.tool_class = toolClass;
    if (!outcome.ok) event.error = outcome.error_name;
    event.shown_timeline_id = serving_shown_timeline_;
    if (arguments.IsObject()) {
        event.timeline_id = TextOf(arguments, "timeline_id");
        event.label = TextOf(arguments, "label");
        event.intervention = TextOf(arguments, "intervention");
        event.plan_id = TextOf(arguments, "plan_id");
    }
    event.previous_revision = last_revision_;
    std::string sessionRevision;
    // A read's answer is not parsed: it holds no delta, and it can be a
    // whole describe. Only its revision is read, from its first member.
    // begin_intervention is a read whose token the journal needs.
    if (McpToolClassActs(toolClass) || toolClass == McpToolClass::Session ||
        tool == "begin_intervention") {
        Value answer;
        std::string ignored;
        if (Value::Parse(outcome.result_json, answer, ignored) &&
            answer.IsObject()) {
            if (event.timeline_id.empty())
                event.timeline_id = TextOf(answer, "timeline_id");
            if (const std::string label = TextOf(answer, "label");
                !label.empty())
                event.label = label;
            if (event.intervention.empty())
                event.intervention = TextOf(answer, "intervention");
            if (event.plan_id.empty())
                event.plan_id = TextOf(answer, "plan_id");
            sessionRevision = TextOf(answer, "revision");
            event.revision = sessionRevision;
            // OR2 -- a durable run receipt names the state before its own
            // effect. Observation-only steps must not look like edits when
            // another call (or the person) left last_revision_ behind.
            if (tool == "commit_run_step" && outcome.ok) {
                event.previous_revision = TextOf(answer, "base_revision");
                event.revision = TextOf(answer, "result_revision");
            }
            event.replayed = TrueIn(answer, "replayed");
            event.background = !TextOf(answer, "task_id").empty();
            event.delta_as_prepared = TrueIn(answer, "delta_as_prepared");
            if (const Value* delta = answer.Find("delta");
                delta != nullptr && delta->IsObject()) {
                event.has_delta = true;
                event.delta = SummarizeMcpDelta(*delta);
            }
        }
    } else {
        sessionRevision = LeadingRevision(outcome.result_json);
        event.revision = sessionRevision;
    }
    if (!sessionRevision.empty()) {
        last_revision_ = sessionRevision;
    }
    serving_calls_.push_back(std::move(event));
}

void McpEndpoint::Serve(int connection, uint64_t id) {
    // Announced with its first message: a connection that says nothing is
    // another endpoint checking this socket is alive (PrepareMcpSocketPath),
    // not an agent, and the app has nothing to show for it.
    bool announced = false;
    Event connected;
    connected.kind = Event::Kind::Connected;
    connected.connection = id;
    LineReader reader(connection);
    std::string line;
    while (reader.Next(line)) {
        if (IsBlank(line)) continue;
        if (!announced) {
            announced = true;
            Notify(connected);
        }
        Message message;
        ReadMessage(line, message);
        std::string response;
        bool answered = false;
        std::vector<Event> calls;
        {
            std::lock_guard<std::mutex> lock(serve_mutex_);
            serving_connection_ = id;
            serving_shown_timeline_.clear();
            // PRD25 -- the timeline the window shows as a call that can
            // change something comes in: an edit of another timeline
            // switches the window to it, and the journal names that timeline
            // when the person was looking at another one. A batch's calls
            // are not read this way, and say nothing of it.
            if (!message.tool.empty() &&
                McpToolClassActs(server_.ToolClassOf(message.tool))) {
                EditorView view;
                std::string ignored;
                if (backend_.ReadEditorView(view, ignored))
                    serving_shown_timeline_ = view.timeline_id;
            }
            answered = server_.HandleJsonRpc(line, response);
            calls.swap(serving_calls_);
        }
        if (answered && !WriteAll(connection, response + "\n")) break;
        for (const Event& call : calls) Notify(call);
    }
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        connections_.erase(
            std::remove(connections_.begin(), connections_.end(), connection),
            connections_.end());
    }
    ::close(connection);
    if (announced) {
        Event disconnected;
        disconnected.kind = Event::Kind::Disconnected;
        disconnected.connection = id;
        Notify(disconnected);
    }
}

void McpEndpoint::Shutdown(bool join) {
    if (!running_.exchange(false)) return;
    const int fd = listen_fd_.exchange(-1);
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
    }
    // The announcement first: from here on, no relay should try us.
    std::error_code ignored;
    if (!endpoint_file_.empty()) {
        std::filesystem::remove(endpoint_file_, ignored);
        endpoint_file_.clear();
    }
    if (!socket_path_.empty()) ::unlink(socket_path_.c_str());
    std::vector<std::thread> threads;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        for (int connection : connections_) ::shutdown(connection, SHUT_RDWR);
        threads.swap(threads_);
    }
    if (join) {
        if (accept_thread_.joinable()) accept_thread_.join();
        for (std::thread& thread : threads)
            if (thread.joinable()) thread.join();
    } else {
        if (accept_thread_.joinable()) accept_thread_.detach();
        for (std::thread& thread : threads)
            if (thread.joinable()) thread.detach();
    }
}

void McpEndpoint::Stop() { Shutdown(true); }

void McpEndpoint::Abandon() { Shutdown(false); }

int ServeMcpStdio(McpServer& server, std::FILE* input, std::FILE* output,
                  const std::string& startupProject) {
    AppLink app;
    std::string detail;
    if (!startupProject.empty()) {
        std::string socketPath;
        if (FindMcpAppEndpoint(startupProject, socketPath))
            app.Connect(socketPath, detail);
    }
    // The client's handshake, replayed to the app when the relay starts
    // after it: the app's server has not met this client yet.
    std::string initializeLine;

    const auto write = [&](const std::string& answer) {
        const std::string line = answer + "\n";
        return std::fwrite(line.data(), 1, line.size(), output) ==
                   line.size() &&
               std::fflush(output) == 0;
    };

    char* buffer = nullptr;
    size_t capacity = 0;
    ssize_t length = 0;
    int status = 0;
    while ((length = getline(&buffer, &capacity, input)) != -1) {
        std::string line(buffer, static_cast<size_t>(length));
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (IsBlank(line)) continue;
        Message message;
        ReadMessage(line, message);

        // PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. The app's
        // server serves the whole catalog. A server started with a profile
        // answers for itself what the app would answer for all of it: the
        // list of tools, the instructions that map them, and a call to a tool
        // the profile leaves out. The app still meets the client: its answer
        // to initialize is read and dropped. A batch goes to the app as it
        // is (McpToolProfiles.h: a profile is not a permission).
        const McpToolRegistry& served = server.Tools();
        if (app.Connected() && !served.Profile().everything &&
            (message.method == "initialize" || message.method == "tools/list" ||
             (message.method == "tools/call" &&
              !served.Offers(message.tool)))) {
            if (message.method == "initialize") {
                std::string dropped;
                if (!app.Send(line) || !app.Receive(dropped)) app.Close();
            }
            std::string answer;
            if (server.HandleJsonRpc(line, answer) && !write(answer)) {
                status = 1;
                break;
            }
            continue;
        }

        if (app.Connected()) {
            // PRD13b -- ROADMAP.md. The app's server answers a line exactly
            // when this one would (McpJsonRpcGetsAnswer): a batch that holds
            // a request is answered though it has no id of its own, and a
            // client's response is not though it has one. Waiting on the id
            // alone, a response would block the relay, and a batch's answer
            // would be read as the next call's.
            const bool answered = McpJsonRpcGetsAnswer(line);
            std::string answer;
            if (app.Send(line) && (!answered || app.Receive(answer))) {
                if (answered && !write(answer)) {
                    status = 1;
                    break;
                }
                continue;
            }
            app.Close();
            if (!answered) continue;
            if (!write(AppClosedAnswer(message))) {
                status = 1;
                break;
            }
            continue;
        }

        if (message.method == "initialize") initializeLine = line;
        if (message.tool == "open_project") {
            const Value* params = message.root.Find("params");
            const Value* arguments =
                params != nullptr ? params->Find("arguments") : nullptr;
            const Value* path =
                arguments != nullptr ? arguments->Find("path") : nullptr;
            std::string projectPath;
            std::string socketPath;
            if (path != nullptr && path->IsString() &&
                ResolveMcpProjectPath(path->AsString(), projectPath, detail) &&
                FindMcpAppEndpoint(projectPath, socketPath) &&
                app.Connect(socketPath, detail)) {
                std::string discarded;
                std::string answer;
                const bool relayed =
                    (initializeLine.empty() ||
                     (app.Send(initializeLine) && app.Receive(discarded))) &&
                    app.Send(
                        R"({"jsonrpc":"2.0","method":"notifications/initialized"})") &&
                    app.Send(line) && app.Receive(answer);
                if (relayed) {
                    if (!write(answer)) {
                        status = 1;
                        break;
                    }
                    continue;
                }
                // The app went away between its announcement and now: its
                // lock is stale, and opening here reclaims it.
                app.Close();
            }
        }

        std::string response;
        if (!server.HandleJsonRpc(line, response)) continue;
        if (!write(response)) {
            status = 1;
            break;
        }
    }
    std::free(buffer);
    if (status == 0 && std::ferror(input) != 0) status = 1;
    return status;
}
