// AX1 -- docs/roadmap/AX-2026-09.md. An agent reaches the project the app
// holds: the app's endpoint (McpEndpoint) over a Unix socket, and the stdio
// server Claude Code spawns relaying to it once open_project names that
// project (ServeMcpStdio).
//
// The "app" here is a McpProjectBackend that holds the project's session
// lock, which is what the window holds; the endpoint in the app runs over
// the live backend through MainThreadBackend, which needs AppKit's main
// queue and is not exercised here. The relay is driven twice: in process,
// over pipes, and through the real `--mcp-serve --stdio` binary.
//
// McpProjectBackend needs CommonCrypto (ProjectStorage), so like
// tests/mcp_tests.cc this builds on macOS only.

#include "AgentJournal.h"
#include "CanonicalHash.h"
#include "Cli.h"
#include "Json.h"
#include "McpEndpoint.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "McpTools.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "RunStep.h"
#include "Ulid.h"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

namespace {

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    document.sequence.id = "01K30000000000000000000005";
    document.sequence.name = "MONTAGE";
    document.sources = {
        {"01K30000000000000000000001", "folder/A.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {"01K30000000000000000000002",
         "video",
         0,
         {{"01K30000000000000000000003",
           "01K30000000000000000000001",
           {100, 25},
           {10, 25},
           {5, 25}}}},
    };
    return document;
}

std::string ToolCall(int id, const std::string& tool,
                     const std::string& arguments) {
    return R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
           R"(,"method":"tools/call","params":{"name":")" + tool +
           R"(","arguments":)" + arguments + "}}";
}

const std::string kInitialize =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
    R"({"protocolVersion":"2025-06-18","capabilities":{},)"
    R"("clientInfo":{"name":"endpoint-tests","version":"1"}}})";
const std::string kInitialized =
    R"({"jsonrpc":"2.0","method":"notifications/initialized"})";

// The tool's own answer inside a tools/call result, and whether the result
// is an error.
mcp_json::Value Payload(const std::string& line, bool& isError) {
    mcp_json::Value root;
    std::string error;
    isError = true;
    if (!mcp_json::Value::Parse(line, root, error)) return {};
    const mcp_json::Value* result = root.Find("result");
    if (result == nullptr) return {};
    const mcp_json::Value* flag = result->Find("isError");
    isError = flag != nullptr && flag->AsBool();
    const mcp_json::Value* content = result->Find("content");
    if (content == nullptr || content->AsArray().empty()) return {};
    mcp_json::Value payload;
    mcp_json::Value::Parse(content->AsArray()[0].Find("text")->AsString(),
                           payload, error);
    return payload;
}

std::string Text(const mcp_json::Value& value, const std::string& key) {
    const mcp_json::Value* found = value.Find(key);
    return found != nullptr && found->IsString() ? found->AsString() : "";
}

// The stdio side, in process: ServeMcpStdio on a thread, fed and read
// through two pipes.
class StdioClient {
public:
    explicit StdioClient(McpServer& server) {
        int toServer[2];
        int fromServer[2];
        if (::pipe(toServer) != 0 || ::pipe(fromServer) != 0) std::abort();
        input_ = ::fdopen(toServer[1], "w");
        output_ = ::fdopen(fromServer[0], "r");
        std::FILE* serverIn = ::fdopen(toServer[0], "r");
        std::FILE* serverOut = ::fdopen(fromServer[1], "w");
        thread_ = std::thread([&server, serverIn, serverOut, this] {
            status_ = ServeMcpStdio(server, serverIn, serverOut);
            std::fclose(serverIn);
            std::fclose(serverOut);
        });
    }
    ~StdioClient() { Close(); }

    void Send(const std::string& line) {
        std::fputs((line + "\n").c_str(), input_);
        std::fflush(input_);
    }
    std::string Receive() {
        char* line = nullptr;
        size_t capacity = 0;
        const ssize_t length = getline(&line, &capacity, output_);
        std::string text = length > 0 ? std::string(line, length) : "";
        std::free(line);
        while (!text.empty() && text.back() == '\n') text.pop_back();
        return text;
    }
    std::string Call(const std::string& line) {
        Send(line);
        return Receive();
    }
    int Close() {
        if (input_ != nullptr) {
            std::fclose(input_);
            input_ = nullptr;
        }
        if (thread_.joinable()) thread_.join();
        if (output_ != nullptr) {
            std::fclose(output_);
            output_ = nullptr;
        }
        return status_;
    }

private:
    std::FILE* input_ = nullptr;
    std::FILE* output_ = nullptr;
    std::thread thread_;
    int status_ = -1;
};

struct Recorder {
    std::mutex mutex;
    std::vector<std::string> events;
    // PRD25 -- the events whole, for what the window's journal reads.
    std::vector<McpEndpoint::Event> calls;
    void Record(const McpEndpoint::Event& event) {
        std::lock_guard<std::mutex> lock(mutex);
        if (event.kind == McpEndpoint::Event::Kind::ToolCall)
            calls.push_back(event);
        switch (event.kind) {
            case McpEndpoint::Event::Kind::Connected:
                events.push_back("connected");
                break;
            case McpEndpoint::Event::Kind::Disconnected:
                events.push_back("disconnected");
                break;
            case McpEndpoint::Event::Kind::ToolCall:
                events.push_back(event.tool + (event.ok ? "" : "!"));
                break;
        }
    }
    bool Saw(const std::string& event) {
        std::lock_guard<std::mutex> lock(mutex);
        for (const std::string& seen : events)
            if (seen == event) return true;
        return false;
    }
    // The calls of `tool` seen so far.
    size_t Count(const std::string& tool) {
        std::lock_guard<std::mutex> lock(mutex);
        size_t count = 0;
        for (const McpEndpoint::Event& call : calls)
            if (call.tool == tool) ++count;
        return count;
    }
    // The last call of `tool`, once there are `count` of them: an event is
    // given after the answer is written, so it may follow the answer by a
    // moment.
    McpEndpoint::Event Last(const std::string& tool, size_t count = 1) {
        for (int attempt = 0; attempt < 400 && Count(tool) < count; ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::lock_guard<std::mutex> lock(mutex);
        for (auto call = calls.rbegin(); call != calls.rend(); ++call)
            if (call->tool == tool) return *call;
        return {};
    }
    // An event that must come: like Last, it may follow the answer by a
    // moment (two seconds at most). Absence is checked with Saw, at once.
    bool SawSoon(const std::string& event) {
        for (int attempt = 0; attempt < 400 && !Saw(event); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return Saw(event);
    }
};

}  // namespace

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-mcp-endpoint-tests");
    std::filesystem::create_directory(directory);
    std::string error;

    // ---- A socket path this user alone can reach -------------------------
    std::string socketPath;
    Check(PrepareMcpSocketPath(GenerateUlid(), socketPath, error),
          "a socket path is prepared: " + error);
    struct stat status {};
    const std::string socketDirectory =
        std::filesystem::path(socketPath).parent_path().string();
    Check(::lstat(socketDirectory.c_str(), &status) == 0 &&
              S_ISDIR(status.st_mode) && (status.st_mode & 077) == 0 &&
              status.st_uid == ::getuid(),
          "its directory is this user's alone: " + socketDirectory);
    Check(socketPath.size() < 104, "and it fits a Unix socket address");
    {
        // A socket left by an app that was killed: nobody listens on it,
        // and the next endpoint prepared sweeps it away.
        McpProjectBackend nobody;
        McpEndpoint dead(nobody);
        std::string deadPath;
        PrepareMcpSocketPath(GenerateUlid(), deadPath, error);
        Check(dead.Start(deadPath, error), "a throwaway endpoint listens");
        dead.Abandon();
        // Abandon removes its socket; put a dead one back in its place.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::strncpy(address.sun_path, deadPath.c_str(),
                     sizeof(address.sun_path) - 1);
        ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        ::close(fd);
        Check(std::filesystem::exists(deadPath), "a dead socket is left");
        std::string swept;
        PrepareMcpSocketPath(GenerateUlid(), swept, error);
        Check(!std::filesystem::exists(deadPath),
              "preparing the next one sweeps it away");
    }

    Project project = Project::FromDocument(Fixture(), "Endpoint fixture");
    std::string projectPath;
    const std::filesystem::path package =
        directory / "Endpoint.cutmachine-project";
    Check(CreatePortableProject(package.string(), project, projectPath, error),
          "fixture package saves: " + error);

    // ---- The app holds the project and announces its endpoint -------------
    McpProjectBackend app;
    std::string opened;
    std::string errorName;
    Check(app.OpenProject(projectPath, opened, errorName, error),
          "the app opens and locks the project: " + error);
    McpEndpoint endpoint(app);
    Recorder recorder;
    endpoint.SetObserver([&recorder](const McpEndpoint::Event& event) {
        recorder.Record(event);
    });
    Check(endpoint.Start(socketPath, error), "the endpoint listens: " + error);
    Check(::lstat(socketPath.c_str(), &status) == 0 &&
              S_ISSOCK(status.st_mode) && (status.st_mode & 077) == 0,
          "the socket is this user's alone");
    std::string announced;
    Check(!FindMcpAppEndpoint(projectPath, announced),
          "nothing is announced before Announce");
    Check(endpoint.Announce(projectPath, error),
          "the endpoint is announced: " + error);
    Check(FindMcpAppEndpoint(projectPath, announced) && announced == socketPath,
          "in the project's lock directory: " + announced);
    {
        // Another endpoint checking which sockets are alive connects and
        // leaves without a word: not an agent, nothing to show.
        std::string probe;
        PrepareMcpSocketPath(GenerateUlid(), probe, error);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        Check(!recorder.Saw("connected") && !recorder.Saw("disconnected"),
              "a silent probe is not announced as an agent");
    }

    // ---- A stdio server with no project relays once open_project names it --
    {
        McpProjectBackend local;
        McpServer server(local);
        StdioClient client(server);
        bool isError = false;
        const std::string initialized = client.Call(kInitialize);
        Check(initialized.find("\"protocolVersion\"") != std::string::npos,
              "the local server answers initialize: " + initialized);
        client.Send(kInitialized);
        // The package directory, as an agent names it: resolved the way
        // open_project resolves it, to the lock the app announced in.
        const mcp_json::Value openAnswer = Payload(
            client.Call(ToolCall(2, "open_project",
                                 R"({"path":")" + package.string() + R"("})")),
            isError);
        Check(!isError && Text(openAnswer, "path") == projectPath,
              "open_project is answered by the app, which holds it: " +
                  openAnswer.Dump());
        Check(!local.HasOpenProject(),
              "the stdio server did not try to open it itself");
        const mcp_json::Value renamed =
            Payload(client.Call(ToolCall(
                        3, "rename_timeline",
                        R"({"timeline_id":"01K30000000000000000000005",)"
                        R"("name":"RELAYÉ"})")),
                    isError);
        Check(!isError, "an edit goes through the app: " + renamed.Dump());
        Check(Read(projectPath).find("RELAYÉ") != std::string::npos,
              "and lands in the project the app holds");
        Check(recorder.SawSoon("connected") &&
                  recorder.SawSoon("open_project") &&
                  recorder.SawSoon("rename_timeline"),
              "the app sees the agent connect and each call");

        // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). What an event
        // carries for the window's journal, read from the call and from its
        // answer, whatever the tool.
        {
            using C = McpToolClass;
            const McpEndpoint::Event rename = recorder.Last("rename_timeline");
            Check(rename.ok && rename.tool_class == C::ProjectEdit &&
                      rename.connection > 0 &&
                      rename.timeline_id == "01K30000000000000000000005" &&
                      !rename.revision.empty() && !rename.has_delta,
                  "a project edit: its class, its connection, the timeline "
                  "it named, the revision it left");
            Check(rename.shown_timeline_id.empty(),
                  "a backend with no window says nothing of what it shows");
            Payload(client.Call(ToolCall(
                        30, "split_clip",
                        R"({"clip_id":"01K30000000000000000000003",)"
                        R"("timeline_position":{"value":10,"rate":25}})")),
                    isError);
            const McpEndpoint::Event split = recorder.Last("split_clip");
            Check(!isError && split.ok &&
                      split.tool_class == C::JournaledEdit && split.has_delta &&
                      split.delta.changed == 1 && split.delta.created == 1 &&
                      split.delta.changed_clip_ids ==
                          std::vector<std::string>{
                              "01K30000000000000000000003"} &&
                      split.delta.created_clip_ids.size() == 1 &&
                      split.connection == rename.connection,
                  "an edit's delta, counted and named: " +
                      std::to_string(split.delta.changed) + " changed, " +
                      std::to_string(split.delta.created) + " created");
            Check(split.previous_revision == rename.revision &&
                      !split.revision.empty() &&
                      split.revision != split.previous_revision,
                  "and the revision it moved, from the previous answer's");
            Payload(client.Call(ToolCall(
                        31, "remove_clip",
                        R"({"clip_id":"01K3ZZZZZZZZZZZZZZZZZZZZZZ"})")),
                    isError);
            const McpEndpoint::Event refused = recorder.Last("remove_clip");
            Check(isError && !refused.ok && !refused.error.empty() &&
                      refused.tool_class == C::JournaledEdit &&
                      refused.revision.empty(),
                  "a refusal: not ok, the error's name (" + refused.error +
                      "), no revision");
            client.Call(ToolCall(32, "describe", "{}"));
            const McpEndpoint::Event described = recorder.Last("describe");
            Check(described.ok && described.tool_class == C::Read &&
                      !described.has_delta &&
                      described.revision == split.revision,
                  "a read: its class, and the revision it leads with, read "
                  "without parsing it: " +
                      described.revision);
            const mcp_json::Value began = Payload(
                client.Call(ToolCall(33, "begin_intervention", "{}")), isError);
            const McpEndpoint::Event begin =
                recorder.Last("begin_intervention");
            Check(!isError && !begin.intervention.empty() &&
                      begin.intervention == Text(began, "intervention") &&
                      begin.timeline_id == "01K30000000000000000000005",
                  "begin_intervention, a read: its token and its timeline");
            client.Call(ToolCall(34, "set_clip_opacity",
                                 R"({"clip_id":"01K30000000000000000000003",)"
                                 R"("opacity":{"num":1,"den":2}})"));
            const mcp_json::Value ended =
                Payload(client.Call(ToolCall(35, "end_intervention",
                                             R"({"intervention":")" +
                                                 begin.intervention +
                                                 R"(","label":"Deux pas"})")),
                        isError);
            const McpEndpoint::Event end = recorder.Last("end_intervention");
            Check(!isError && end.ok && end.label == "Deux pas" &&
                      end.intervention == begin.intervention,
                  "end_intervention: its label and its token: " + ended.Dump());
            // A batch's calls are events too, one each.
            client.Call(
                R"([{"jsonrpc":"2.0","id":36,"method":"tools/call",)"
                R"("params":{"name":"timeline_stats","arguments":{}}},)"
                R"({"jsonrpc":"2.0","id":37,"method":"tools/call",)"
                R"("params":{"name":"timeline_stats","arguments":{}}}])");
            Check(recorder.Last("timeline_stats", 2).ok &&
                      recorder.Count("timeline_stats") == 2,
                  "a batch of two calls is two events");
        }
        // PRD13b -- ROADMAP.md. The relay waits for an answer exactly when
        // the app's server writes one: a client's response gets none and
        // must not hold the relay; a batch has no id of its own and is
        // answered. Either mistake would hand the next call the wrong line,
        // or none.
        client.Send(R"({"jsonrpc":"2.0","id":41,"result":{}})");
        const std::string batch = client.Call(
            R"([{"jsonrpc":"2.0","id":42,"method":"ping"},)"
            R"({"jsonrpc":"2.0","method":"notifications/initialized"},)"
            R"({"jsonrpc":"2.0","id":43,"method":"ping"}])");
        Check(batch == R"([{"jsonrpc":"2.0","id":42,"result":{}},)"
                       R"({"jsonrpc":"2.0","id":43,"result":{}}])",
              "a batch relayed to the app comes back whole, and a client's "
              "response is passed over: " +
                  batch);
        const std::string after =
            client.Call(R"({"jsonrpc":"2.0","id":44,"method":"ping"})");
        Check(after == R"({"jsonrpc":"2.0","id":44,"result":{}})",
              "the next call gets its own answer: " + after);

        // PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. A second
        // agent whose server runs the motion profile: relayed to the app,
        // which serves the whole catalog, it is still shown and served the
        // profile alone.
        {
            McpProjectBackend profiled;
            McpServer motionServer(profiled, *FindMcpToolProfile("motion"));
            StdioClient motionClient(motionServer);
            motionClient.Call(kInitialize);
            motionClient.Send(kInitialized);
            const mcp_json::Value relayedOpen =
                Payload(motionClient.Call(
                            ToolCall(60, "open_project",
                                     R"({"path":")" + projectPath + R"("})")),
                        isError);
            Check(!isError && !profiled.HasOpenProject(),
                  "under a profile, open_project is relayed to the app: " +
                      relayedOpen.Dump());
            const auto parse = [](const std::string& text) {
                mcp_json::Value root;
                std::string parseError;
                mcp_json::Value::Parse(text, root, parseError);
                return root;
            };
            const mcp_json::Value listed = parse(motionClient.Call(
                R"({"jsonrpc":"2.0","id":61,"method":"tools/list"})"));
            const mcp_json::Value* tools =
                listed.Find("result") ? listed.Find("result")->Find("tools")
                                      : nullptr;
            Check(tools != nullptr &&
                      tools->AsArray().size() ==
                          motionServer.Tools().Tools().size() &&
                      tools->AsArray().size() <
                          motionServer.Tools().CatalogSize(),
                  "tools/list answers the profile, not the app's catalog");
            const mcp_json::Value initialized = parse(motionClient.Call(
                R"({"jsonrpc":"2.0","id":62,"method":"initialize","params":)"
                R"({"protocolVersion":"2025-06-18"}})"));
            const mcp_json::Value* instructions =
                initialized.Find("result")
                    ? initialized.Find("result")->Find("instructions")
                    : nullptr;
            Check(instructions != nullptr &&
                      instructions->AsString().find("`motion` profile") !=
                          std::string::npos,
                  "and so does initialize, with the profile's instructions");
            const size_t removedBefore = recorder.Count("remove_words");
            const mcp_json::Value refused = Payload(
                motionClient.Call(ToolCall(63, "remove_words", "{}")), isError);
            Check(isError && Text(refused, "error") == "ToolNotInProfile",
                  "a tool outside the profile is refused before the app: " +
                      refused.Dump());
            const mcp_json::Value timelines =
                Payload(motionClient.Call(ToolCall(64, "list_timelines", "{}")),
                        isError);
            Check(
                !isError && recorder.Last("list_timelines").ok,
                "a tool it serves is answered by the app: " + timelines.Dump());
            Check(recorder.Count("remove_words") == removedBefore,
                  "the app never saw the refused call");
            Check(motionClient.Close() == 0,
                  "the profiled stdio server ends with its input");
        }

        // The app closes: the call in flight is refused as such, the
        // announcement is gone.
        endpoint.Stop();
        Check(!FindMcpAppEndpoint(projectPath, announced),
              "a stopped endpoint is no longer announced");
        const mcp_json::Value closed =
            Payload(client.Call(ToolCall(4, "describe", "{}")), isError);
        Check(isError && Text(closed, "error") == "AppClosed",
              "a call the app never answered says so: " + closed.Dump());
        // The app lets the project go (it moves its lock to another);
        // open_project then opens it here.
        Project otherProject = Project::FromDocument(Fixture(), "Autre");
        std::string otherPath;
        CreatePortableProject((directory / "Other.cutmachine-project").string(),
                              otherProject, otherPath, error);
        Check(app.OpenProject(otherPath, opened, errorName, error),
              "the app moves to another project: " + error);
        const mcp_json::Value reopened = Payload(
            client.Call(ToolCall(5, "open_project",
                                 R"({"path":")" + projectPath + R"("})")),
            isError);
        Check(!isError && local.HasOpenProject(),
              "after the app, open_project opens the project locally: " +
                  reopened.Dump());
        Check(client.Close() == 0, "the stdio server ends with its input");
    }
    Check(recorder.SawSoon("disconnected"), "the app sees the agent leave");

    // ---- Through the real binary, started on the project -------------------
    // The stdio server above released the project when it ended; a second
    // app takes it.
    {
        // OR2 -- session metadata can describe a newer window state than
        // the receipt (a person acted before the answer). Deliberately make
        // it differ; attribution must use the receipt's own before/after.
        class DifferentSessionRevision : public McpProjectBackend {
        public:
            bool Revision(std::string& revision,
                          std::string& message) override {
                if (!McpProjectBackend::Revision(revision, message)) {
                    return false;
                }
                revision = CanonicalHash("session:" + revision);
                return true;
            }
        } held;
        Check(held.OpenProject(projectPath, opened, errorName, error),
              "a second app holds its project: " + error);
        McpEndpoint second(held);
        Recorder seen;
        second.SetObserver(
            [&seen](const McpEndpoint::Event& event) { seen.Record(event); });
        std::string secondSocket;
        PrepareMcpSocketPath(GenerateUlid(), secondSocket, error);
        Check(second.Start(secondSocket, error) &&
                  second.Announce(projectPath, error),
              "its endpoint is announced: " + error);
        const std::filesystem::path requests = directory / "requests.jsonl";
        const std::filesystem::path answers = directory / "answers.jsonl";
        const std::filesystem::path log = directory / "stderr.log";
        {
            std::ofstream output(requests);
            output << kInitialize << '\n'
                   << kInitialized << '\n'
                   << ToolCall(2, "describe", "{}") << '\n';
        }
        const std::string command = std::string("'") + CUTMACHINE_EXECUTABLE +
                                    "' --mcp-serve --stdio '" + projectPath +
                                    "' < '" + requests.string() + "' > '" +
                                    answers.string() + "' 2> '" + log.string() +
                                    "'";
        Check(std::system(command.c_str()) == 0,
              "the binary serves and exits: " + Read(log));
        Check(Read(log).find("relaying") != std::string::npos,
              "it says it relays to the app: " + Read(log));
        Check(seen.Saw("describe"),
              "the binary's describe was answered by the app");
        std::string lastLine;
        std::istringstream lines(Read(answers));
        for (std::string line; std::getline(lines, line);)
            if (!line.empty()) lastLine = line;
        bool isError = true;
        const mcp_json::Value described = Payload(lastLine, isError);
        Check(!isError && described.Find("sequence") != nullptr,
              "and its answer reached the client: " + lastLine);

        // OR2 -- a receipt supplies its own edit boundary and preview, not
        // whatever revision the last agent connection happened to read.
        {
            McpProjectBackend local;
            McpServer server(local);
            StdioClient client(server);
            client.Call(kInitialize);
            client.Send(kInitialized);
            client.Call(ToolCall(20, "open_project",
                                 R"({"path":")" + projectPath + R"("})"));
            Project saved;
            EditLog log;
            ProjectEditLog projectLog;
            EditError editError = EditError::None;
            Check(LoadStoredProject(projectPath, saved, error) &&
                      EditLog::Load(TimelineEditLogPathForProject(
                                        projectPath, saved.active_timeline_id),
                                    log, editError, error) &&
                      ProjectEditLog::Load(
                          ProjectEditLogPathForProject(projectPath), projectLog,
                          editError, error),
                  "read the receipt's starting point: " + error);
            RunStepRequest request;
            request.run_id = GenerateUlid();
            request.step_id = GenerateUlid();
            request.stage = "analysis";
            request.timeline_id = saved.active_timeline_id;
            request.base_revision = CanonicalHash(saved.SaveToString());
            request.base_history_hash = CanonicalHash(log.Serialize());
            request.base_project_history_hash =
                CanonicalHash(projectLog.Serialize());
            const auto commit = [&](int id) {
                mcp_json::Value args = mcp_json::Value::MakeObject();
                args.Set("request_json", mcp_json::Value::MakeString(
                                             SerializeRunStepRequest(request)));
                return Payload(
                    client.Call(ToolCall(id, "commit_run_step", args.Dump())),
                    isError);
            };
            const auto observed = commit(21);
            Check(!isError, "observation receipt commits: " + observed.Dump());
            const auto observation = seen.Last("commit_run_step");
            AgentJournal journal;
            AgentJournalContext context;
            context.time = "10:00:00";
            Check(observation.timeline_id == request.timeline_id &&
                      observation.previous_revision == request.base_revision &&
                      observation.revision == request.base_revision &&
                      !journal.Record(observation, context),
                  "an observation receipt never enters the edit journal");

            const Document document = saved.MakeActiveDocument();
            const Ulid clipId =
                document.sequence.tracks.front().clips.front().id;
            SetClipOpacityOperation opacity;
            opacity.clip_id = clipId;
            opacity.opacity = {1, 3};
            EditPlanDraft draft(document);
            Check(draft.Apply(opacity, editError, error), error);
            EditPlan plan;
            Check(FinishEditPlan(draft, "Finition du run",
                                 mcp_json::Value::MakeArray(), plan, errorName,
                                 error),
                  error);
            request.step_id = GenerateUlid();
            request.stage = "finishing";
            request.plan_json = SerializeEditPlan(plan);
            const auto edited = commit(22);
            Check(!isError, "planned receipt commits: " + edited.Dump());
            const auto event = seen.Last("commit_run_step", 2);
            Check(
                event.label == plan.label && event.has_delta &&
                    event.delta.changed_clip_ids == std::vector<Ulid>{clipId} &&
                    event.previous_revision == request.base_revision &&
                    event.revision != request.base_revision &&
                    journal.Record(event, context),
                "the run entry carries its reviewed label and clip effect");
            commit(23);
            const auto replay = seen.Last("commit_run_step", 3);
            Check(!isError && replay.replayed &&
                      !journal.Record(replay, context) &&
                      journal.Entries().size() == 1,
                  "retrying the receipt adds no second Agent entry");
        }
        second.Stop();
    }

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All MCP endpoint tests passed\n";
    return 0;
}
