#pragma once

// ACP1 -- docs/roadmap/ACP-2026-09.md. The Agent panel as a client of any
// coding agent that speaks the Agent Client Protocol: Claude Code and Codex
// through their adapters (@agentclientprotocol/claude-agent-acp,
// @agentclientprotocol/codex-acp), Gemini CLI natively, or whatever the
// person runs. The built-in tool loop (ChatSession.h) caps a turn at eight
// model calls and knows none of what a real editing run needs (a 36-minute
// run made 142 calls); the agents people already use have the long loop,
// the compaction and the skills. This file makes CUTMACHINE the editor side
// of that protocol instead of a second agent.
//
// What stays CUTMACHINE's: the agent edits the timeline only through the
// engine's MCP server, which the session hands to the agent at creation
// (`session/new` mcpServers). For the app, that server is the binary itself
// in `--mcp-serve <project> --stdio`, which relays to the window's endpoint
// (AX1): one journal with the mouse, and the panel folds each turn into one
// Undo (chat::TurnIntervention). Calls to that server pass without asking;
// anything else the agent wants to run (a shell, a file edit) is put to the
// person. That gate is a convenience, not a boundary: an agent decides by
// itself which of its tools need permission, and which server a call
// belongs to is read from how each adapter labels it (IsEngineToolCall).
//
// JSON-RPC 2.0, one message per line, over the agent's stdin and stdout;
// protocol version 1. Plain C++ (Json.h only): the transport is injected, so
// the whole exchange is tested against a scripted agent without a process
// (tests/acp_client_tests.cc). The process itself is AcpProcess.h.

#include "ChatTranscript.h"
#include "Json.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace acp {

constexpr int kProtocolVersion = 1;

// The agent's side of the pipe. ReadLine is only ever called from the thread
// running the turn; WriteLine and Close may also come from the thread that
// asked to stop, so an implementation serialises writes.
class AcpTransport {
public:
    virtual ~AcpTransport() = default;
    // One message, without its newline.
    virtual bool WriteLine(const std::string& line, std::string& error) = 0;
    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. Follows the prompt in order;
    // a process transport also bounds its delivery if the agent stops
    // reading. Scripted transports can deliver it like any other line.
    virtual bool WriteCancellation(const std::string& line,
                                   std::string& error) {
        return WriteLine(line, error);
    }
    // Blocks for the next line. False once the agent has closed its output:
    // it exited, crashed, or was closed. After a close, the complete lines
    // already received are still returned first (FI7): a final answer that
    // arrived just before is not lost.
    virtual bool ReadLine(std::string& line) = 0;
    // Ends the agent, from any thread; a blocked ReadLine stops waiting,
    // returns what had already arrived, then false.
    virtual void Close() = 0;
    // FI7. True once nothing more can be written: after Close, or when the
    // transport closed itself (an undeliverable cancellation, a broken
    // pipe). ReadLine may still return lines received before.
    virtual bool Closed() const { return false; }
    // Where to look when the agent dies, e.g. the file its stderr went to.
    virtual std::string DiagnosticHint() const { return {}; }
};

using AcpTransportFactory =
    std::function<std::unique_ptr<AcpTransport>(std::string& error)>;

// An MCP server over stdio, as `session/new` describes it to the agent.
struct AcpMcpServer {
    std::string name;
    std::string command;
    std::vector<std::string> args;
    std::vector<std::pair<std::string, std::string>> env;
};

struct AcpSessionConfig {
    // Absolute: the agent refuses a relative one.
    std::string cwd;
    std::vector<AcpMcpServer> mcp_servers;
    // The server whose tool calls are granted without asking.
    std::string engine_server_name = "cutmachine";
    // Sent before the first instruction of every agent session: where the
    // agent is and what it may touch. Empty sends nothing.
    std::string preamble;
};

struct AcpPermissionOption {
    std::string id;
    std::string name;
    std::string kind;  // allow_once, allow_always, reject_once, reject_always
};

// A tool call the agent wants to run and asks the person about.
struct AcpPermissionRequest {
    std::string title;
    std::string kind;            // read, edit, execute... ("" if not given)
    std::string raw_input_json;  // the call's arguments, when the agent gave
    std::vector<AcpPermissionOption> options;
};

// Returns the chosen option's id, or "" to cancel. Called on the thread
// running the turn; the panel shows its question on the main thread.
using AcpPermissionPrompt =
    std::function<std::string(const AcpPermissionRequest& request)>;

// What the agent reported, summed over the session's turns. A turn whose
// answer carried no usage counts in `unreported_turns`; the totals are then
// a floor, not a measure (BENCH-2026-09).
struct AcpUsageTotals {
    int64_t turns = 0;
    int64_t unreported_turns = 0;
    int64_t input_tokens = 0;
    int64_t output_tokens = 0;
    int64_t thought_tokens = 0;
    int64_t cache_read_tokens = 0;
    int64_t cache_write_tokens = 0;
    // The last `usage_update`: context used and its size, in tokens.
    int64_t context_used = 0;
    int64_t context_size = 0;
};

// Whether a tool call, as an agent describes it in `tool_call`,
// `tool_call_update` or `session/request_permission`, is a call to the MCP
// server named `serverName`. Each adapter labels it its own way: the Claude
// adapter names the server in `_meta.claudeCode.mcpServer` and the tool
// `mcp__<server>__<tool>`, the Codex adapter titles it
// `mcp.<server>.<tool>` with `{server, tool, arguments}` as raw input.
bool IsEngineToolCall(const mcp_json::Value& toolCall,
                      const std::string& serverName);

// The tool's own name when the title is one of the forms above
// ("mcp__cutmachine__describe" -> "describe"), the title otherwise.
std::string DisplayToolName(const std::string& title,
                            const std::string& serverName);

// One conversation with one agent process, started at the first instruction
// and kept across turns. Same surface as chat::ChatSession: the panel runs
// SubmitUserMessage on a worker thread and may call RequestStop from the
// main thread.
class AcpChatSession {
public:
    AcpChatSession(
        AcpTransportFactory factory, AcpSessionConfig config,
        std::function<void(const chat::ChatTranscriptEntry&)> onEntry = nullptr,
        AcpPermissionPrompt permissionPrompt = nullptr);
    ~AcpChatSession();
    AcpChatSession(const AcpChatSession&) = delete;
    AcpChatSession& operator=(const AcpChatSession&) = delete;

    // Every line in and out, for the local protocol log. `outgoing` is true
    // for what CUTMACHINE sends. Called from the turn's thread, and from the
    // stopping thread for `session/cancel`.
    void SetTrace(
        std::function<void(bool outgoing, const std::string& line)> trace) {
        trace_ = std::move(trace);
    }

    // Runs one instruction to the end of the agent's turn. Starts the agent
    // and its session first if there is none (again after the agent died,
    // which loses the conversation, and says so). Returns true only when the
    // agent ends its turn normally; every other ending is an Error entry.
    bool SubmitUserMessage(const std::string& text, std::string& error);

    // From any thread. During a turn: `session/cancel`, and the agent ends
    // the turn as cancelled once its tools have stopped. While the agent is
    // still starting: the agent is closed. Asked before the turn's thread
    // has begun, the turn does not start. A turn clears the request when it
    // ends, however it ends.
    void RequestStop();

    // Forgets a stop asked for a turn that has already ended. The panel
    // calls it on the main thread before it hands the next instruction to
    // the worker: a Stop pressed in the moment a turn finishes then does not
    // stop the next one.
    void ClearStopRequest() { stop_requested_ = false; }

    // Ends the agent process. Not during a turn.
    void Shutdown();

    const std::vector<chat::ChatTranscriptEntry>& Transcript() const {
        return transcript_;
    }
    const AcpUsageTotals& Usage() const { return usage_; }
    // agentInfo's title (else its name) once the agent has answered.
    const std::string& AgentTitle() const { return agent_title_; }

private:
    struct ToolState {
        std::string title;
        std::string raw_input_json;
        bool engine = false;  // a call to config_.engine_server_name
        bool announced = false;
        bool finished = false;
    };
    enum class CallOutcome { Result, RpcError, Closed, WriteFailed };

    bool EnsureSession(std::string& error);
    // Writes a request and returns its id, 0 if it could not be written.
    int64_t SendRequest(const std::string& method, mcp_json::Value params,
                        std::string& error);
    // Reads until the answer to `id`, serving the agent's own requests and
    // notifications meanwhile.
    CallOutcome AwaitResponse(int64_t id, mcp_json::Value& result,
                              std::string& error);
    CallOutcome Call(const std::string& method, mcp_json::Value params,
                     mcp_json::Value& result, std::string& error);
    bool Send(const mcp_json::Value& message, std::string& error);
    void HandleMessage(const mcp_json::Value& message);
    void HandleUpdate(const mcp_json::Value& update);
    mcp_json::Value DecidePermission(const mcp_json::Value& params);
    void TrackTool(const mcp_json::Value& toolCall);
    void AnnounceTool(ToolState& tool);
    void FinishTool(ToolState& tool, const mcp_json::Value& update);
    void FlushText();
    void Emit(chat::ChatTranscriptEntry entry);
    void EmitError(const std::string& text);
    void Reset();

    AcpTransportFactory factory_;
    AcpSessionConfig config_;
    std::function<void(const chat::ChatTranscriptEntry&)> on_entry_;
    AcpPermissionPrompt permission_prompt_;
    std::function<void(bool, const std::string&)> trace_;

    // Written only by the turn's thread, under the mutex; RequestStop reads
    // them under it.
    std::mutex transport_mutex_;
    std::unique_ptr<AcpTransport> transport_;
    std::string session_id_;
    bool transport_closed_ = false;  // RequestStop closed it

    std::atomic_bool stop_requested_{false};
    std::atomic_bool prompt_in_flight_{false};
    bool session_prompted_ = false;
    int64_t next_request_id_ = 1;
    int64_t last_error_code_ = 0;  // of the last RpcError
    std::string agent_title_;
    std::string text_buffer_;
    std::map<std::string, ToolState> tools_;
    std::string last_plan_;
    std::vector<chat::ChatTranscriptEntry> transcript_;
    AcpUsageTotals usage_;
};

}  // namespace acp
