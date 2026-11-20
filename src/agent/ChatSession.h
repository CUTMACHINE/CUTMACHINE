#pragma once

// The chat panel's tool-calling loop (ROADMAP.md F2.4): the piece that
// takes one user instruction, talks to the LLM, and dispatches whatever
// tool calls it asks for -- through McpToolRegistry::Call, the exact same
// entry point McpServer.cc's `tools/call` JSON-RPC handler uses. There is
// no second tool-execution path here: every mutation this class causes is
// `registry.Call(backend, name, arguments)`, i.e. McpTools.cc's dispatch
// for `name` constructing an Operation and handing it to `backend`, which
// for the running app (McpLiveBackend.h) means EditLog::Apply against the
// same in-memory Document a mouse gesture edits. See PHILOSOPHY.md
// principle 3: "un geste à la souris et une instruction en langage naturel
// produisent la même opération... passent par le même journal."
//
// Plain C++ (Json.h/McpTools.h/ChatLlmClient.h only) so the loop itself --
// message bookkeeping, when to stop asking for more tool calls, how a tool
// failure is fed back to the model -- is unit-testable on a plain Linux
// host against a fake ChatLlmClient transport and an in-memory McpBackend,
// without AppKit or a real Anthropic key. See tests/chat_session_tests.cc.

#include "ChatLlmClient.h"
#include "ChatTranscript.h"
#include "McpBackend.h"
#include "McpTools.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace chat {

// PL1 -- ROADMAP.md. What the history calls a chat turn: "Agent : " and the
// instruction, on one line, cut on a character boundary.
std::string TurnLabel(const std::string& instruction);

// PL1 -- ROADMAP.md. A turn is one intervention: whatever the agent applies
// before it answers is folded into one undo step when the turn ends,
// however it ends. A backend without history, or a fold it refuses -- the
// person edited the timeline by hand meanwhile, the agent moved to another
// timeline -- leaves the steps as they are; the turn does not fail for it.
// ACP-2026-09: an external agent's edits reach the same live backend through
// the app's endpoint, so the panel frames its turns with this too.
class TurnIntervention {
public:
    TurnIntervention(McpBackend& backend, std::string label);
    ~TurnIntervention();
    TurnIntervention(const TurnIntervention&) = delete;
    TurnIntervention& operator=(const TurnIntervention&) = delete;

private:
    McpBackend& backend_;
    std::string label_;
    std::string token_;
};

// Everything this session's model calls reported, summed (BENCH-2026-09).
// A failed call never counts. A call whose response carried no usage counts
// in `unreported_calls` and adds no tokens, so a total with unreported calls
// is a floor, not a measure.
struct ChatUsageTotals {
    int64_t calls = 0;
    int64_t unreported_calls = 0;
    int64_t input_tokens = 0;
    int64_t cache_write_tokens = 0;
    int64_t cache_read_tokens = 0;
    int64_t output_tokens = 0;
};

// A single instruction's worth of default guidance, in French to match the
// rest of this project's UI copy (see PanelLayout.h's panel titles,
// sidecar/planner.py's own SYSTEM_PROMPT). Told to call the `describe` tool
// before proposing an edit rather than being handed a pre-fetched timeline
// view, so context-gathering goes through McpToolRegistry exactly like a
// mutation does -- see this file's header comment.
extern const std::string kDefaultSystemPrompt;

class ChatSession {
public:
    // Neither `backend` nor `registry` nor `llm` is owned; all three must
    // outlive this session. `onEntry`, if set, is invoked synchronously
    // (on whatever thread calls SubmitUserMessage) as each transcript entry
    // is produced, so a caller can stream partial progress to the UI
    // instead of waiting for the whole turn; Transcript() always holds the
    // complete history regardless.
    ChatSession(
        McpBackend& backend, const McpToolRegistry& registry,
        const ChatLlmClient& llm,
        std::string systemPrompt = kDefaultSystemPrompt,
        std::function<void(const ChatTranscriptEntry&)> onEntry = nullptr);

    // Runs one full user turn: records the instruction, then alternates
    // LLM calls and tool dispatches until the model stops asking for tools,
    // it asks for more than kMaxToolIterations rounds in a row (a safety
    // limit against a runaway loop, not a normal outcome), or a call to the
    // LLM itself fails. Returns false only in the latter two cases --
    // `error` is also appended to Transcript() as a ChatEntryKind::Error
    // either way. Any tool calls that already ran (successfully or not)
    // before a stopping condition are recorded in Transcript() regardless
    // of the return value: a rejected tool call is not a turn failure, the
    // model is expected to see it and adjust.
    bool SubmitUserMessage(const std::string& text, std::string& error);

    // Visual QA of 19 September 2026 -- a turn could not be stopped from the
    // panel. Asked from any thread, honoured before the next call to the
    // model and before each tool call: the tools already run stay run, every
    // tool the model asked for and did not get answers "stopped by the
    // user" so the conversation stays well formed, and the turn ends with
    // an Error entry. A long media task the turn waits on is cancelled by
    // its runner (the app's Stop button), which then returns here.
    //
    // Asked before the worker has begun the turn, the turn does not start: a
    // turn clears the request when it ends, not when it begins (found on the
    // ACP client, ACP-2026-09, which shares the panel's Stop button).
    void RequestStop() { stop_requested_ = true; }

    // Forgets a stop asked for a turn that has already ended. The panel
    // calls it on the main thread before it hands the next instruction to
    // the worker, so a Stop pressed as a turn finishes does not stop the
    // next one.
    void ClearStopRequest() { stop_requested_ = false; }

    const std::vector<ChatTranscriptEntry>& Transcript() const {
        return transcript_;
    }

    // Summed over every SubmitUserMessage so far, including a turn that
    // stopped early: the calls it already made were still spent.
    const ChatUsageTotals& Usage() const { return usage_; }

private:
    void Emit(ChatTranscriptEntry entry);

    McpBackend& backend_;
    const McpToolRegistry& registry_;
    const ChatLlmClient& llm_;
    std::string system_prompt_;
    std::function<void(const ChatTranscriptEntry&)> on_entry_;
    std::vector<ChatMessage> messages_;
    std::vector<ChatTranscriptEntry> transcript_;
    ChatUsageTotals usage_;
    std::atomic_bool stop_requested_{false};
};

}  // namespace chat
