#pragma once

// AX1 -- docs/roadmap/AX-2026-09.md. An agent edits the project the app has
// open, in the app, while the editor watches.
//
// Until this existed, the MCP server Claude Code starts from `.mcp.json`
// (`--mcp-serve --stdio`) and the app excluded each other: the server takes
// the project's session lock, and the app refuses a locked project. Every
// run of the bench was edited out of sight and judged on its export; the
// editor's own notes on it came back as markers placed in another editor.
//
// Two halves, both plain POSIX and C++:
//
//   - McpEndpoint, in the app: the same McpServer the stdio transport runs,
//     over the app's own backend, listening on a Unix socket. The socket
//     lives in a directory only this user can enter (sockets cannot be
//     created on the exFAT volumes projects often sit on), and its path is
//     written into the project's session lock directory, so it is announced
//     exactly as long as the app holds the lock.
//
//   - ServeMcpStdio, in the server Claude Code spawns: McpServer::ServeStdio,
//     until open_project names a project whose lock announces an endpoint.
//     From then on it relays every message to the app, byte for byte, and
//     answers with what the app answers. Nothing changes for the client: the
//     same `.mcp.json`, the same catalog, the same session semantics (tasks,
//     idempotency keys, revisions), held by the app's server. PROFIL-2026-11
//     PF1 -- except under a profile (McpToolProfiles.h): the stdio server
//     then answers initialize, tools/list and a call outside the profile
//     itself, since the app serves the whole catalog.
//
// Authority is the same as over stdio: whoever can reach the socket edits
// the project, and only this user can reach it.

#include "Json.h"
#include "McpBackend.h"
#include "McpServer.h"
#include "McpTools.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The clips a tool's answer
// says it touched, read from its `delta` (DocumentDelta.h): counted, and
// named but for those a ripple shifted, which a delta states as rules
// rather than clip by clip.
struct McpDeltaSummary {
    // Created: new clips. Changed: clips that existed and changed in any
    // field, the created ones excluded. Shifted: the clips the delta's
    // `shifted` rules move, summed over its rules.
    int64_t created = 0;
    int64_t changed = 0;
    int64_t removed = 0;
    int64_t shifted = 0;
    std::vector<std::string> created_clip_ids;
    std::vector<std::string> changed_clip_ids;
    std::vector<std::string> removed_clip_ids;
};

// `delta` as a tool's answer holds it (SerializeDocumentDelta). What is not
// shaped like a delta counts nothing.
McpDeltaSummary SummarizeMcpDelta(const mcp_json::Value& delta);

// PRD25 -- whether a call of a tool of `toolClass` can change something: a
// timeline, the project, a file, a cache, another program. A read, the
// session's own bookkeeping (a plan, a task, the project it opened) and a
// tool the catalog does not know cannot (McpTools.h).
bool McpToolClassActs(McpToolClass toolClass);

// Where an app holding `projectPath` (a resolved project file,
// ResolveMcpProjectPath) names its endpoint: a file in the session lock
// directory, which the lock's release removes with it.
std::string McpEndpointFilePath(const std::string& projectPath);

// The socket an app announces for `projectPath`, when there is one to try.
bool FindMcpAppEndpoint(const std::string& projectPath,
                        std::string& socketPath);

// A path for a new endpoint socket named `name`, in a directory created for
// this user alone (0700, owned by the caller; refused otherwise). Short:
// a Unix socket path holds 104 bytes on macOS.
bool PrepareMcpSocketPath(const std::string& name, std::string& socketPath,
                          std::string& error);

class McpEndpoint {
public:
    // Does not take ownership of `backend`; it must outlive the endpoint.
    explicit McpEndpoint(McpBackend& backend);
    ~McpEndpoint();
    McpEndpoint(const McpEndpoint&) = delete;
    McpEndpoint& operator=(const McpEndpoint&) = delete;

    // Listens on `socketPath` (0600). Connections are served one message at
    // a time across all of them, in arrival order, as the HTTP transport
    // does: the server and its session are not meant to run two calls at
    // once.
    bool Start(const std::string& socketPath, std::string& error);
    // Writes the socket path into `projectPath`'s lock directory.
    bool Announce(const std::string& projectPath, std::string& error);

    // Stops listening, removes the socket and the announcement, ends every
    // connection and waits for their threads.
    void Stop();
    // The same without waiting. For the app quitting: a connection may be
    // blocked handing a call to the main thread, the very thread quitting,
    // and the process ends right after. The endpoint must then never be
    // destroyed.
    void Abandon();

    bool IsRunning() const { return running_.load(); }
    const std::string& SocketPath() const { return socket_path_; }
    McpServer& Server() { return server_; }

    // What the app shows of an agent at work: a connection opening (with
    // its first message) or closing, and each tool call once answered.
    // Called on the connection's thread.
    struct Event {
        enum class Kind { Connected, Disconnected, ToolCall };
        Kind kind = Kind::ToolCall;
        std::string tool;
        bool ok = true;

        // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). What the window's
        // journal writes a line from (AgentJournal.h). All of it is read
        // from the call and from its answer as the client gets it: no tool
        // reports any of it for the journal.
        //
        // The connection the event came on, one per agent session: the same
        // number on its Connected, on each of its calls and on its
        // Disconnected. Numbered from 1.
        uint64_t connection = 0;

        // The rest describes a ToolCall: one per `tools/call`, a batch's
        // included.
        McpToolClass tool_class = McpToolClass::Unclassified;
        // The refusal's name (`error`), when the call was refused.
        std::string error;
        // The timeline the call named (`timeline_id`), or else the one its
        // answer names: a plan's, an intervention's, a timeline created.
        // Empty: the call acted on the timeline the window shows.
        std::string timeline_id;
        // The timeline the window showed when the call came in, before an
        // edit of another timeline switched the window to it (PRD17); read
        // through the backend for a call that can change something
        // (McpBackend::ReadEditorView). Empty when the backend has no window.
        std::string shown_timeline_id;
        // The answer's `label`, or else the call's: an intervention's, a
        // plan's.
        std::string label;
        // The token begin_intervention answered, or the one
        // end_intervention was given.
        std::string intervention;
        // The plan prepare_edit_plan answered, or the one apply_edit_plan
        // was given.
        std::string plan_id;
        // The project's revision the answer carries (McpSession.h), and the
        // one the endpoint's previous answer carried: equal, the call left
        // the project as it was -- unless the person edited in between,
        // which the endpoint does not see. Empty when not known. OR2 -- a
        // durable run step uses its receipt's own before/after instead:
        // session metadata may already include an intervening human edit.
        std::string revision;
        std::string previous_revision;
        // An idempotency key's replay: nothing was applied again.
        bool replayed = false;
        // Answered with a task_id: the call started a background task.
        bool background = false;
        // The answer carries a delta, and what it counts.
        bool has_delta = false;
        McpDeltaSummary delta;
        // apply_edit_plan's short answer: the delta is the one
        // prepare_edit_plan answered for the same plan.
        bool delta_as_prepared = false;
    };
    void SetObserver(std::function<void(const Event&)> observer);

private:
    void AcceptLoop();
    void Serve(int connection, uint64_t id);
    void Notify(const Event& event);
    void Shutdown(bool join);
    // PRD25 -- the server's ToolCallObserver: one event for each call the
    // request being served answered (serve_mutex_ held).
    void RecordToolCall(const std::string& tool, McpToolClass toolClass,
                        const mcp_json::Value& arguments,
                        const McpToolCallOutcome& outcome);

    McpBackend& backend_;
    McpServer server_;
    std::mutex serve_mutex_;
    std::mutex state_mutex_;
    std::vector<int> connections_;
    std::vector<std::thread> threads_;
    std::function<void(const Event&)> observer_;
    std::atomic<int> listen_fd_{-1};
    std::atomic_bool running_{false};
    std::thread accept_thread_;
    std::string socket_path_;
    std::string endpoint_file_;
    std::atomic<uint64_t> last_connection_{0};
    // PRD25 -- held under serve_mutex_: the request being served, what its
    // calls gave, and the revision of the last answer.
    uint64_t serving_connection_ = 0;
    std::string serving_shown_timeline_;
    std::vector<Event> serving_calls_;
    std::string last_revision_;
};

// The server Claude Code spawns: McpServer::ServeStdio(input, output), which
// becomes a relay to the app as soon as `startupProject` (when given) or an
// open_project call names a project the app announces an endpoint for. When
// the app goes away mid-session, the call in flight is answered with an
// `AppClosed` refusal and the server serves locally again: open_project then
// opens the project here, the app's lock being gone. Returns as ServeStdio.
int ServeMcpStdio(McpServer& server, std::FILE* input, std::FILE* output,
                  const std::string& startupProject = {});
