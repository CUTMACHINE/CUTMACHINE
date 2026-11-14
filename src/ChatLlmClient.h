#pragma once

// LLM client for the chat panel (ROADMAP.md F2.4).
//
// PHILOSOPHY.md's "pas de service central" non-but, unchanged by the F2.x
// amendment: CUTMACHINE never runs or proxies a model backend. This talks
// directly to Anthropic or an OpenAI-compatible endpoint such as a local
// Ollama server -- never a CUTMACHINE-operated account or credit balance.
// The Anthropic mode mirrors sidecar/planner.py's conventions, so a key or
// override already set up for the Python sidecar works unchanged:
// ANTHROPIC_API_KEY, CUTMACHINE_ANTHROPIC_MODEL (falling back to the
// sidecar's generic CUTMACHINE_MODEL), CUTMACHINE_ANTHROPIC_URL. See
// ChatLlmConfig::FromEnvironment.
//
// This file and ChatLlmClient.cc are plain C++ (Json.h/McpTools.h only, no
// AppKit, no sockets). The actual HTTPS request/response is an injected
// `ChatHttpTransport` function, so the request-building and response-
// parsing logic here is unit-testable against a fake transport without a
// network or a real API key (tests/chat_llm_client_tests.cc). The one real
// transport -- NSURLSession-backed, since this project has no other outbound
// dependency to reach for -- lives in ChatPanelView.mm and is AppKit-only,
// not exercised at runtime yet like the rest of this project's AppKit
// surface.

#include "Json.h"
#include "McpTools.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace chat {

enum class ChatLlmProvider { AnthropicMessages, OpenAiCompatible, Ollama };

// PRD22 -- ROADMAP.md. Each built-in default is written once, here:
// ChatLlmConfig below, the Agent panel's engine dialog (AiEngineForm.h,
// ChatPanelView.mm) and the tests read these.
//
// The Anthropic model is the one the claude-api skill names as the default
// (read on 3 October 2026), and the one the project's agent edits ran on
// (ROADMAP.md, A2). The skill's most capable model, claude-fable-5-1, is
// left to an explicit choice: it costs two and a half times as much, refuses
// an organisation without 30-day data retention, and a hard turn of it can
// outlast a request that is not streamed. claude-opus-5-5 thinks on every
// call. It refuses a request that turns thinking off or budgets it, sets a
// sampling parameter or forces a tool_choice -- BuildAnthropicRequestBody
// sends none of these -- and, for an account opened since 31 August 2026, a
// thinking block replayed after an earlier turn, the system prompt or the
// tools changed: ChatSession's history only grows
// (tests/chat_session_tests.cc). Sent without an effort, it runs at its
// default, medium. A stored dialog choice, CUTMACHINE_ANTHROPIC_MODEL or
// CUTMACHINE_MODEL overrides it.
inline constexpr char kDefaultAnthropicModel[] = "claude-opus-5-5";
inline constexpr char kDefaultAnthropicBaseUrl[] = "https://api.anthropic.com";
inline constexpr char kDefaultOllamaModel[] = "qwen2.5-coder:7b";
inline constexpr char kDefaultOllamaBaseUrl[] = "http://localhost:11434";

// One HTTPS POST request/response pair. Deliberately just strings and a
// status code -- everything about *how* the bytes get there (TLS, sockets,
// NSURLSession) is the transport's problem, not this client's.
struct ChatHttpRequest {
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    // How long the transport may wait without receiving anything
    // (ChatLlmConfig::timeout_seconds). Zero leaves it to the transport.
    double timeout_seconds = 0.0;
};

struct ChatHttpResponse {
    int status_code = 0;
    std::string body;
};

// Returns true once a response was received, whatever its status code --
// false is reserved for a transport-level failure (DNS, connect, TLS,
// timeout) that never produced an HTTP response at all, with `error`
// describing it. Mirrors _post_json's exception split in
// sidecar/planner.py: a non-2xx status is still "true" here and gets
// surfaced as a ChatSendResult error by SendMessages, exactly as an
// HTTPError there still carries a body to report.
using ChatHttpTransport =
    std::function<bool(const ChatHttpRequest& request,
                       ChatHttpResponse& response, std::string& error)>;

// Opaque: a block the panel does not act on but must hand back unchanged --
// the thinking and redacted_thinking blocks of a model that reasons by
// default (visual QA of 19 September 2026: they used to be dropped, and a
// tool loop must return them to the model as they came).
enum class ChatBlockType { Text, ToolUse, ToolResult, Opaque };

// Provider-neutral content block. A given instance only ever populates the
// fields its `type` uses; the rest stay at their defaults.
struct ChatContentBlock {
    ChatBlockType type = ChatBlockType::Text;
    std::string text;         // Text; also ToolResult's textual content
    std::string tool_use_id;  // ToolUse's own id; the id a ToolResult answers
    std::string tool_name;    // ToolUse only
    mcp_json::Value tool_input;  // ToolUse only -- the tool call's arguments
    bool tool_is_error = false;  // ToolResult only
    // ToolResult only: a picture the tool returned alongside its text, as
    // base64 with its MIME type. Empty for every tool but read_frame.
    // Serialised only for providers whose API accepts an image inside a tool
    // result -- see SendMessages; elsewhere the text still goes through and
    // the picture is dropped rather than faked into a description.
    std::string tool_image_base64;
    std::string tool_image_mime;
    // Opaque only: the block as the provider sent it, JSON, sent back as is.
    std::string raw_json;

    static ChatContentBlock MakeText(std::string text);
    static ChatContentBlock MakeOpaque(std::string rawJson);
    static ChatContentBlock MakeToolUse(std::string id, std::string name,
                                        mcp_json::Value input);
    static ChatContentBlock MakeToolResult(std::string toolUseId,
                                           std::string text, bool isError,
                                           std::string toolName = {},
                                           std::string imageBase64 = {},
                                           std::string imageMime = {});
};

// One turn of the conversation. `role` is "user" or "assistant", matching
// the Anthropic API verbatim -- this struct is a thin, ordered wrapper
// around one `messages[]` entry, not a richer model of its own.
struct ChatMessage {
    std::string role;
    std::vector<ChatContentBlock> content;
};

struct ChatLlmConfig {
    ChatLlmProvider provider = ChatLlmProvider::AnthropicMessages;
    std::string model = kDefaultAnthropicModel;
    std::string api_key;
    std::string base_url = kDefaultAnthropicBaseUrl;
    std::string anthropic_version = "2023-06-01";
    // PRD22 -- what one call may write, its thinking included: each call of
    // a tool loop has its own, so a turn that chains calls is bounded call
    // by call. The default model thinks on every call, and 4096 could cut
    // one that reasons and then writes a long tool input (an edit plan, a
    // list of spans) in the middle of that input. 16000 is the claude-api
    // skill's figure for a request that is not streamed -- its 64K is for
    // streamed ones, and this client does not stream. Every Anthropic model
    // still served accepts it.
    int max_tokens = 16000;
    int context_tokens = 32768;
    // PRD22 -- a response that is not streamed sends nothing until it is
    // complete, and the transport gives up after this long without a byte:
    // 120 s could cut a reply of 16000 tokens. 600 s is the official SDKs'
    // default, the bound the skill's 16000 is sized for. The panel's Stop
    // waits for the call in flight, up to this long on a stalled connection.
    double timeout_seconds = 600.0;

    // Reads ANTHROPIC_API_KEY, CUTMACHINE_ANTHROPIC_MODEL (falling back to
    // CUTMACHINE_MODEL, then the default above) and CUTMACHINE_ANTHROPIC_URL
    // from the process environment -- matching
    // sidecar/planner.py:AnthropicPlanner.__init__ field for field. Before
    // reading, also tries to fill in any of those three variables that
    // aren't already exported from a local dotenv file, in priority order:
    // $CUTMACHINE_ENV_FILE if set, else ~/.config/cutmachine/.env. That is
    // deliberately *not* the sidecar's project-root .env (PHILOSOPHY.md:
    // interface/tooling configuration is a local preference, never
    // something that lives in or next to the project) -- pointing
    // CUTMACHINE_ENV_FILE at the sidecar's own .env during development
    // reunifies the two if a developer wants that.
    //
    // Never overrides a variable already exported in the environment. If
    // ANTHROPIC_API_KEY is still unset afterward, api_key is left empty and
    // `missingKeyError` (if non-null) is set to an explanatory message --
    // this is the only "not configured" signal; the client never guesses or
    // silently disables itself otherwise.
    static ChatLlmConfig FromEnvironment(
        std::string* missingKeyError = nullptr);
};

// Token counts one model call reported (BENCH-2026-09). A bench of the
// in-app agent needs what each run spent, and only the response knows it.
// Instrumentation, not protocol: a response with no usage, or a malformed
// one, still parses. `reported` then stays false, so a caller summing calls
// can tell "the provider said nothing" apart from "the provider said zero".
//
// The three providers split input differently; this keeps the Anthropic
// split. OpenAI-compatible prompt_tokens include the cached part, which is
// moved to cache_read_tokens; native Ollama reports no cache at all.
struct ChatUsage {
    bool reported = false;
    int64_t input_tokens = 0;  // uncached input
    int64_t cache_write_tokens = 0;
    int64_t cache_read_tokens = 0;
    int64_t output_tokens = 0;
};

struct ChatSendResult {
    bool ok = false;
    std::vector<ChatContentBlock> content;  // assistant's reply, if ok
    std::string stop_reason;                // "end_turn", "tool_use", ...
    std::string error;                      // set when !ok
    ChatUsage usage;                        // meaningful only when ok
};

class ChatLlmClient {
public:
    ChatLlmClient(ChatLlmConfig config, ChatHttpTransport transport);

    // Sends the full conversation so far plus the tool catalog and returns
    // the assistant's next turn. Stateless: `messages` already contains
    // every prior user/assistant/tool_result turn -- see ChatSession, which
    // owns that history and this loop.
    //
    // PRD22 -- an Anthropic reply that was declined (stop_reason "refusal")
    // or cut at max_tokens with a tool call in it is an error: its content
    // is dropped, so nothing of it runs or enters the history. Native Ollama
    // gets the key, when there is one, as a bearer token.
    ChatSendResult SendMessages(const std::vector<ChatMessage>& messages,
                                const std::string& systemPrompt,
                                const std::vector<McpTool>& tools) const;

    const ChatLlmConfig& Config() const { return config_; }

private:
    ChatLlmConfig config_;
    ChatHttpTransport transport_;
};

}  // namespace chat
