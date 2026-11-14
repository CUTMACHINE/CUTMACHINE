// Pure-C++ test for the model API client (ROADMAP.md F2.4).
// Depends only on Json.h/McpTools.h/ChatLlmClient.h -- no AppKit, no
// sockets, no real network -- so it builds and runs on a plain Linux host,
// the same way tests/mcp_tools_tests.cc does for the MCP tool dispatcher.
// The real HTTPS transport (NSURLSession-backed) lives in ChatPanelView.mm
// and is AppKit-only, not exercised at runtime yet; everything this file
// exercises -- request assembly, response parsing, error handling -- is
// exactly what that transport is a thin, swappable I/O shim around.

#include "ChatLlmClient.h"
#include "Json.h"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using chat::ChatBlockType;
using chat::ChatContentBlock;
using chat::ChatHttpRequest;
using chat::ChatHttpResponse;
using chat::ChatLlmClient;
using chat::ChatLlmConfig;
using chat::ChatMessage;
using chat::ChatSendResult;
using mcp_json::Value;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

ChatLlmConfig TestConfig() {
    ChatLlmConfig config;
    config.model = "claude-sonnet-4-5";
    config.api_key = "sk-test-key";
    config.base_url = "https://api.example.invalid";
    return config;
}

ChatLlmConfig OllamaConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::OpenAiCompatible;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434/v1";
    return config;
}

ChatLlmConfig OllamaNativeConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::Ollama;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434";
    config.context_tokens = 32768;
    return config;
}

std::vector<McpTool> OneToolCatalog() {
    return {{"trim_clip", "Trim a clip.",
             "{\"type\":\"object\",\"properties\":{\"clip_id\":{\"type\":"
             "\"string\"}},\"required\":[\"clip_id\"],\"additionalProperties\":"
             "false}"}};
}

}  // namespace

int main() {
    Test("SendMessages builds the Anthropic request shape and headers", [] {
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body =
                R"({"content":[{"type":"text","text":"ok"}],"stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        ChatMessage user;
        user.role = "user";
        user.content.push_back(
            ChatContentBlock::MakeText("Coupe le premier plan."));
        const ChatSendResult result =
            client.SendMessages({user}, "Tu es un agent.", OneToolCatalog());

        Check(result.ok, "the call succeeds: " + result.error);
        Check(
            captured.url == "https://api.example.invalid/v1/messages",
            "the request targets base_url + /v1/messages, got " + captured.url);

        bool sawApiKeyHeader = false;
        bool sawVersionHeader = false;
        for (const auto& header : captured.headers) {
            if (header.first == "x-api-key")
                sawApiKeyHeader = header.second == "sk-test-key";
            if (header.first == "anthropic-version")
                sawVersionHeader = header.second == "2023-06-01";
        }
        Check(sawApiKeyHeader, "x-api-key carries the configured key");
        Check(sawVersionHeader, "anthropic-version is set");

        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "request body is valid JSON: " + parseError);
        const Value* model = body.Find("model");
        Check(model && model->IsString() &&
                  model->AsString() == "claude-sonnet-4-5",
              "request body carries the configured model");
        // AGENT-2026-09 A3 -- the prefix the panel repeats on every call is
        // marked for Anthropic's prompt cache: the system prompt, the last
        // tool and the last block of the last message carry a breakpoint.
        const auto cached = [](const Value* block) {
            const Value* control =
                block ? block->Find("cache_control") : nullptr;
            const Value* type = control ? control->Find("type") : nullptr;
            return type && type->IsString() && type->AsString() == "ephemeral";
        };
        const Value* system = body.Find("system");
        const Value* systemBlock =
            system && system->IsArray() && system->AsArray().size() == 1
                ? &system->AsArray()[0]
                : nullptr;
        Check(systemBlock && systemBlock->Find("text") &&
                  systemBlock->Find("text")->IsString() &&
                  systemBlock->Find("text")->AsString() == "Tu es un agent." &&
                  cached(systemBlock),
              "request body carries the system prompt as one cached block");
        const Value* tools = body.Find("tools");
        Check(tools && tools->IsArray() && tools->AsArray().size() == 1,
              "request body carries the tool catalog");
        if (tools && tools->IsArray() && tools->AsArray().size() == 1) {
            const Value& tool = tools->AsArray()[0];
            const Value* name = tool.Find("name");
            Check(name && name->IsString() && name->AsString() == "trim_clip",
                  "the tool entry names trim_clip");
            const Value* schema = tool.Find("input_schema");
            Check(schema && schema->IsObject(),
                  "the tool entry's input_schema round-trips as an object");
            Check(cached(&tool), "the last tool is a cache breakpoint");
        }
        const Value* messages = body.Find("messages");
        Check(
            messages && messages->IsArray() && messages->AsArray().size() == 1,
            "request body carries exactly the one user message");
        if (messages && messages->IsArray() &&
            messages->AsArray().size() == 1) {
            const Value& message = messages->AsArray()[0];
            const Value* role = message.Find("role");
            Check(role && role->IsString() && role->AsString() == "user",
                  "the message role is user");
            const Value* content = message.Find("content");
            Check(
                content && content->IsArray() && content->AsArray().size() == 1,
                "the message has one content block");
            Check(content && content->IsArray() &&
                      content->AsArray().size() == 1 &&
                      cached(&content->AsArray()[0]) &&
                      content->AsArray()[0].Find("text") &&
                      content->AsArray()[0].Find("text")->AsString() ==
                          "Coupe le premier plan.",
                  "the last block of the last message is a cache breakpoint, "
                  "text intact");
        }
    });

    Test("a text-only response parses to one Text block and end_turn", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body = R"({"content":[{"type":"text","text":"Bonjour."}],)"
                            R"("stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.stop_reason == "end_turn", "stop_reason is end_turn");
        Check(result.content.size() == 1, "exactly one content block");
        if (result.content.size() == 1) {
            Check(result.content[0].type == ChatBlockType::Text,
                  "the block is text");
            Check(result.content[0].text == "Bonjour.", "text matches");
        }
    });

    Test("a tool_use response parses id/name/input and reports tool_use", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body =
                R"({"content":[)"
                R"({"type":"text","text":"D'accord."},)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K300000000000000000003","edge":"Tail",)"
                R"("delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.stop_reason == "tool_use", "stop_reason is tool_use");
        Check(result.content.size() == 2, "two content blocks");
        if (result.content.size() == 2) {
            Check(result.content[0].type == ChatBlockType::Text,
                  "first is text");
            Check(result.content[1].type == ChatBlockType::ToolUse,
                  "second is tool_use");
            Check(result.content[1].tool_use_id == "toolu_1",
                  "tool_use id matches");
            Check(result.content[1].tool_name == "trim_clip",
                  "tool name matches");
            const Value* clipId = result.content[1].tool_input.Find("clip_id");
            Check(clipId && clipId->IsString() &&
                      clipId->AsString() == "01K300000000000000000003",
                  "tool input round-trips clip_id");
        }
    });

    Test("thinking blocks come back to the model unchanged", [] {
        // Visual QA of 19 September 2026 -- current models reason by
        // default; a tool loop must return their thinking as it came.
        ChatHttpRequest captured;
        int calls = 0;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body =
                ++calls == 1
                    ? R"({"content":[{"type":"thinking","thinking":"",)"
                      R"("signature":"sig-123"},{"type":"redacted_thinking",)"
                      R"("data":"opaque-456"},{"type":"tool_use","id":)"
                      R"("toolu_1","name":"trim_clip","input":{}}],)"
                      R"("stop_reason":"tool_use"})"
                    : R"({"content":[{"type":"text","text":"fait."}],)"
                      R"("stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        ChatMessage user;
        user.role = "user";
        user.content.push_back(ChatContentBlock::MakeText("coupe"));
        const ChatSendResult first = client.SendMessages({user}, "sys", {});
        Check(first.ok && first.content.size() == 3 &&
                  first.content[0].type == ChatBlockType::Opaque &&
                  first.content[1].type == ChatBlockType::Opaque &&
                  first.content[2].type == ChatBlockType::ToolUse,
              "thinking and redacted thinking are kept, in order, before the "
              "tool call");
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content = first.content;
        ChatMessage toolResults;
        toolResults.role = "user";
        toolResults.content.push_back(ChatContentBlock::MakeToolResult(
            "toolu_1", "{\"ok\":true}", false));
        client.SendMessages({user, assistant, toolResults}, "sys", {});
        Check(captured.body.find(R"("signature":"sig-123")") !=
                      std::string::npos &&
                  captured.body.find(R"("data":"opaque-456")") !=
                      std::string::npos,
              "the next request hands both blocks back, signature and data "
              "intact");
    });

    // PRD22 -- ROADMAP.md. The defaults are written once
    // (ChatLlmClient.h), read from the claude-api skill on 3 October 2026.
    Test("the default Anthropic model is the current one", [] {
        Check(ChatLlmConfig().model == chat::kDefaultAnthropicModel,
              "an unconfigured chat reads the one default");
        Check(std::string(chat::kDefaultAnthropicModel) == "claude-opus-5-5",
              "which is claude-opus-5-5, got " +
                  std::string(chat::kDefaultAnthropicModel));
        Check(ChatLlmConfig().base_url == "https://api.anthropic.com" &&
                  ChatLlmConfig().anthropic_version == "2023-06-01",
              "on the public endpoint, API version 2023-06-01");
    });

    Test("a call has room to think, then write a long tool input", [] {
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body = R"({"content":[{"type":"text","text":"ok"}],)"
                            R"("stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmConfig config;
        config.api_key = "sk-test-key";
        Check(config.max_tokens == 16000,
              "max_tokens is 16000, the skill's figure for a request that "
              "is not streamed");
        Check(config.timeout_seconds == 600.0,
              "the transport waits 600 s, the SDKs' default");
        ChatLlmClient client(config, transport);
        Check(client.SendMessages({}, "sys", OneToolCatalog()).ok,
              "the call succeeds");
        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "request body is valid JSON: " + parseError);
        const Value* model = body.Find("model");
        const Value* maxTokens = body.Find("max_tokens");
        int64_t tokens = 0;
        Check(model && model->IsString() &&
                  model->AsString() == chat::kDefaultAnthropicModel,
              "the request names the default model");
        Check(maxTokens && maxTokens->AsInt64(tokens) && tokens == 16000,
              "the request carries max_tokens 16000");
        Check(captured.timeout_seconds == 600.0,
              "the transport is handed the client's timeout");
    });

    Test("the request sends nothing the default model refuses", [] {
        // claude-opus-5-5 answers 400 to thinking turned off or budgeted, to
        // a sampling parameter and to a forced tool_choice (claude-api
        // skill). The request leaves all of them to the model.
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body = R"({"content":[{"type":"text","text":"ok"}],)"
                            R"("stop_reason":"end_turn"})";
            return true;
        };
        ChatLlmConfig config;
        config.api_key = "sk-test-key";
        ChatLlmClient client(config, transport);
        ChatMessage user;
        user.role = "user";
        user.content.push_back(ChatContentBlock::MakeText("coupe"));
        client.SendMessages({user}, "sys", OneToolCatalog());
        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "request body is valid JSON: " + parseError);
        for (const char* refused :
             {"thinking", "temperature", "top_p", "top_k", "tool_choice"})
            Check(body.Find(refused) == nullptr,
                  std::string("the request carries no ") + refused);
    });

    Test("the environment's model wins over the default", [] {
        // PRD22 -- the default moved; whoever named a model keeps it:
        // CUTMACHINE_ANTHROPIC_MODEL, else CUTMACHINE_MODEL. No dotenv file
        // is read here (CUTMACHINE_ENV_FILE points at nothing).
        const auto saved = [](const char* name) {
            const char* value = std::getenv(name);
            return std::pair<bool, std::string>(value != nullptr,
                                                value ? value : "");
        };
        const auto restore = [](const char* name,
                                const std::pair<bool, std::string>& value) {
            if (value.first)
                ::setenv(name, value.second.c_str(), 1);
            else
                ::unsetenv(name);
        };
        const auto envFile = saved("CUTMACHINE_ENV_FILE");
        const auto explicitModel = saved("CUTMACHINE_ANTHROPIC_MODEL");
        const auto genericModel = saved("CUTMACHINE_MODEL");
        ::setenv("CUTMACHINE_ENV_FILE",
                 "/nonexistent-cutmachine-prd22/absent.env", 1);
        ::unsetenv("CUTMACHINE_ANTHROPIC_MODEL");
        ::unsetenv("CUTMACHINE_MODEL");
        Check(ChatLlmConfig::FromEnvironment().model ==
                  chat::kDefaultAnthropicModel,
              "nothing named: the default");
        ::setenv("CUTMACHINE_MODEL", "claude-sonnet-4-6", 1);
        Check(ChatLlmConfig::FromEnvironment().model == "claude-sonnet-4-6",
              "CUTMACHINE_MODEL wins over the default");
        ::setenv("CUTMACHINE_ANTHROPIC_MODEL", "claude-haiku-4-5", 1);
        Check(ChatLlmConfig::FromEnvironment().model == "claude-haiku-4-5",
              "CUTMACHINE_ANTHROPIC_MODEL wins over CUTMACHINE_MODEL");
        restore("CUTMACHINE_ENV_FILE", envFile);
        restore("CUTMACHINE_ANTHROPIC_MODEL", explicitModel);
        restore("CUTMACHINE_MODEL", genericModel);
    });

    Test("a declined turn is an error that names its category", [] {
        // PRD22 -- the default model's classifiers can decline a turn: a 200
        // whose stop_reason is "refusal", its content empty or partial.
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body =
                R"({"content":[{"type":"text","text":"Je com"}],)"
                R"("stop_reason":"refusal","stop_details":{"type":"refusal",)"
                R"("category":"cyber","explanation":"policy"}})";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok, "the call is reported as failed");
        Check(result.content.empty(), "nothing of the reply is kept");
        Check(result.error.find("refusal") != std::string::npos &&
                  result.error.find("cyber") != std::string::npos &&
                  result.error.find("policy") != std::string::npos,
              "the error says it was declined, and why: " + result.error);

        auto bare = [](const ChatHttpRequest&, ChatHttpResponse& response,
                       std::string&) {
            response.status_code = 200;
            response.body = R"({"content":[],"stop_reason":"refusal",)"
                            R"("stop_details":null})";
            return true;
        };
        ChatLlmClient bareClient(TestConfig(), bare);
        const ChatSendResult unexplained =
            bareClient.SendMessages({}, "sys", {});
        Check(!unexplained.ok &&
                  unexplained.error.find("refusal") != std::string::npos,
              "a refusal without details is still an error: " +
                  unexplained.error);
    });

    Test("a reply cut at max_tokens inside a tool call runs nothing", [] {
        auto cut = [](const ChatHttpRequest&, ChatHttpResponse& response,
                      std::string&) {
            response.status_code = 200;
            response.body =
                R"({"content":[{"type":"thinking","thinking":"",)"
                R"("signature":"sig"},{"type":"tool_use","id":"toolu_1",)"
                R"("name":"trim_clip","input":{"clip_id":"01K3"}}],)"
                R"("stop_reason":"max_tokens"})";
            return true;
        };
        ChatLlmClient client(TestConfig(), cut);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok && result.content.empty(),
              "the call fails and hands back no block to run");
        Check(result.error.find("max_tokens") != std::string::npos &&
                  result.error.find("16000") != std::string::npos,
              "the error names the limit reached: " + result.error);

        auto text = [](const ChatHttpRequest&, ChatHttpResponse& response,
                       std::string&) {
            response.status_code = 200;
            response.body = R"({"content":[{"type":"text","text":"Voici "}],)"
                            R"("stop_reason":"max_tokens"})";
            return true;
        };
        ChatLlmClient textClient(TestConfig(), text);
        const ChatSendResult partial = textClient.SendMessages({}, "sys", {});
        Check(partial.ok && partial.stop_reason == "max_tokens" &&
                  partial.content.size() == 1 &&
                  partial.content.front().text == "Voici ",
              "a text cut short is still shown, nothing to run in it");
    });

    Test("a tool_use and tool_result block round-trip through the request body",
         [] {
             ChatHttpRequest captured;
             auto transport = [&](const ChatHttpRequest& request,
                                  ChatHttpResponse& response, std::string&) {
                 captured = request;
                 response.status_code = 200;
                 response.body =
                     R"({"content":[{"type":"text","text":"fait."}],)"
                     R"("stop_reason":"end_turn"})";
                 return true;
             };
             ChatLlmClient client(TestConfig(), transport);

             Value input = Value::MakeObject();
             input.Set("clip_id", Value::MakeString("01K3X"));
             ChatMessage assistant;
             assistant.role = "assistant";
             assistant.content.push_back(
                 ChatContentBlock::MakeToolUse("toolu_1", "trim_clip", input));
             ChatMessage toolResults;
             toolResults.role = "user";
             toolResults.content.push_back(ChatContentBlock::MakeToolResult(
                 "toolu_1", "{\"ok\":true}", false));

             client.SendMessages({assistant, toolResults}, "sys", {});

             Value body;
             std::string parseError;
             Check(Value::Parse(captured.body, body, parseError),
                   "request body is valid JSON: " + parseError);
             const Value* messages = body.Find("messages");
             Check(messages && messages->IsArray() &&
                       messages->AsArray().size() == 2,
                   "both messages are present");
             if (messages && messages->AsArray().size() == 2) {
                 const Value& firstContent =
                     *messages->AsArray()[0].Find("content");
                 const Value& toolUseBlock = firstContent.AsArray()[0];
                 const Value* toolUseType = toolUseBlock.Find("type");
                 Check(toolUseType && toolUseType->AsString() == "tool_use",
                       "assistant message's block is tool_use");
                 const Value* toolUseInput = toolUseBlock.Find("input");
                 const Value* clipId =
                     toolUseInput ? toolUseInput->Find("clip_id") : nullptr;
                 Check(clipId && clipId->AsString() == "01K3X",
                       "tool_use input round-trips through Dump()");

                 const Value& secondContent =
                     *messages->AsArray()[1].Find("content");
                 const Value& toolResultBlock = secondContent.AsArray()[0];
                 const Value* toolResultType = toolResultBlock.Find("type");
                 Check(toolResultType &&
                           toolResultType->AsString() == "tool_result",
                       "second message's block is tool_result");
                 const Value* isError = toolResultBlock.Find("is_error");
                 Check(
                     isError && isError->IsBool() && isError->AsBool() == false,
                     "tool_result carries is_error:false");
             }
         });

    Test(
        "a non-200 status is reported as an error, not a crash or false ok",
        [] {
            auto transport = [](const ChatHttpRequest&,
                                ChatHttpResponse& response, std::string&) {
                response.status_code = 401;
                response.body =
                    R"({"type":"error","error":{"type":"authentication_error",)"
                    R"("message":"invalid x-api-key"}})";
                return true;
            };
            ChatLlmClient client(TestConfig(), transport);
            const ChatSendResult result = client.SendMessages({}, "sys", {});
            Check(!result.ok, "the call is reported as failed");
            Check(result.error.find("invalid x-api-key") != std::string::npos,
                  "the error surfaces the API's own message, got: " +
                      result.error);
        });

    Test("malformed JSON in a 200 response is reported as an error", [] {
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse& response,
                            std::string&) {
            response.status_code = 200;
            response.body = "not json";
            return true;
        };
        ChatLlmClient client(TestConfig(), transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok, "the call is reported as failed");
        Check(!result.error.empty(), "an error message is set");
    });

    Test(
        "a transport-level failure is reported without touching the response "
        "body",
        [] {
            auto transport = [](const ChatHttpRequest&, ChatHttpResponse&,
                                std::string& error) {
                error = "connection refused";
                return false;
            };
            ChatLlmClient client(TestConfig(), transport);
            const ChatSendResult result = client.SendMessages({}, "sys", {});
            Check(!result.ok, "the call is reported as failed");
            Check(
                result.error.find("connection refused") != std::string::npos,
                "the transport's own error is surfaced, got: " + result.error);
        });

    Test(
        "Ollama uses OpenAI chat completions without requiring an API key", [] {
            ChatHttpRequest captured;
            auto transport = [&](const ChatHttpRequest& request,
                                 ChatHttpResponse& response, std::string&) {
                captured = request;
                response.status_code = 200;
                response.body = R"({"choices":[{"message":{"role":"assistant",)"
                                R"("content":"Bonjour depuis Qwen."},)"
                                R"("finish_reason":"stop"}]})";
                return true;
            };
            ChatMessage user;
            user.role = "user";
            user.content.push_back(ChatContentBlock::MakeText("Bonjour"));
            ChatLlmClient client(OllamaConfig(), transport);
            const ChatSendResult result =
                client.SendMessages({user}, "Tu montes.", OneToolCatalog());

            Check(result.ok, "the Ollama call succeeds: " + result.error);
            Check(captured.url == "http://localhost:11434/v1/chat/completions",
                  "the request targets Ollama's OpenAI endpoint");
            bool sawAuthorization = false;
            for (const auto& header : captured.headers)
                sawAuthorization |= header.first == "Authorization";
            Check(!sawAuthorization,
                  "a local Ollama request sends no fake credential");

            Value body;
            std::string parseError;
            Check(
                Value::Parse(captured.body, body, parseError),
                "OpenAI-compatible request body is valid JSON: " + parseError);
            const Value* messages = body.Find("messages");
            Check(messages && messages->IsArray() &&
                      messages->AsArray().size() == 2,
                  "system and user messages are sent");
            const Value* tools = body.Find("tools");
            Check(tools && tools->IsArray() && tools->AsArray().size() == 1,
                  "MCP tools use the OpenAI function schema");
            Check(result.content.size() == 1 &&
                      result.content.front().text == "Bonjour depuis Qwen.",
                  "the OpenAI text response is parsed");
        });

    Test("OpenAI tool calls and tool results bridge the agent loop", [] {
        ChatHttpRequest captured;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            captured = request;
            response.status_code = 200;
            response.body =
                R"({"choices":[{"message":{"role":"assistant",)"
                R"("content":null,"tool_calls":[{"id":"call_1",)"
                R"("type":"function","function":{"name":"trim_clip",)"
                R"("arguments":"{\"clip_id\":\"01K3X\"}"}}]},)"
                R"("finish_reason":"tool_calls"}]})";
            return true;
        };
        ChatLlmClient client(OllamaConfig(), transport);
        const ChatSendResult result =
            client.SendMessages({}, "sys", OneToolCatalog());
        Check(result.ok, "the tool response succeeds: " + result.error);
        Check(result.stop_reason == "tool_use",
              "OpenAI tool_calls maps to the session's tool_use reason");
        Check(result.content.size() == 1 &&
                  result.content.front().type == ChatBlockType::ToolUse,
              "the function call becomes a ToolUse block");
        if (!result.content.empty()) {
            const Value* clipId =
                result.content.front().tool_input.Find("clip_id");
            Check(clipId && clipId->IsString() && clipId->AsString() == "01K3X",
                  "function arguments parse as structured tool input");
        }

        Value input = Value::MakeObject();
        input.Set("clip_id", Value::MakeString("01K3X"));
        ChatMessage assistant;
        assistant.role = "assistant";
        assistant.content.push_back(
            ChatContentBlock::MakeToolUse("call_1", "trim_clip", input));
        ChatMessage toolResult;
        toolResult.role = "user";
        toolResult.content.push_back(
            ChatContentBlock::MakeToolResult("call_1", "{\"ok\":true}", false));
        client.SendMessages({assistant, toolResult}, "sys", {});

        Value body;
        std::string parseError;
        Check(Value::Parse(captured.body, body, parseError),
              "follow-up request body is valid JSON: " + parseError);
        const Value* messages = body.Find("messages");
        Check(
            messages && messages->IsArray() && messages->AsArray().size() == 3,
            "system, assistant tool call and tool result are sent");
        if (messages && messages->IsArray() &&
            messages->AsArray().size() == 3) {
            const Value* role = messages->AsArray()[2].Find("role");
            Check(role && role->IsString() && role->AsString() == "tool",
                  "tool result uses the OpenAI tool role");
        }
    });

    Test(
        "native Ollama requests a larger context and accepts Qwen textual "
        "tools",
        [] {
            ChatHttpRequest captured;
            auto transport = [&](const ChatHttpRequest& request,
                                 ChatHttpResponse& response, std::string&) {
                captured = request;
                response.status_code = 200;
                response.body =
                    R"({"message":{"role":"assistant","content":)"
                    R"("{\"name\":\"trim_clip\",\"arguments\":{\"clip_id\":\"01K3X\"}}"},)"
                    R"("done":true,"done_reason":"stop"})";
                return true;
            };
            ChatLlmClient client(OllamaNativeConfig(), transport);
            const ChatSendResult result =
                client.SendMessages({}, "sys", OneToolCatalog());

            Check(result.ok,
                  "the native Ollama call succeeds: " + result.error);
            Check(captured.url == "http://localhost:11434/api/chat",
                  "native Ollama targets /api/chat");
            Value body;
            std::string parseError;
            Check(Value::Parse(captured.body, body, parseError),
                  "native Ollama request is valid JSON: " + parseError);
            const Value* options = body.Find("options");
            const Value* context = options ? options->Find("num_ctx") : nullptr;
            int64_t contextTokens = 0;
            Check(context && context->AsInt64(contextTokens) &&
                      contextTokens == 32768,
                  "native Ollama receives the configured 32K context");
            Check(result.stop_reason == "tool_use" &&
                      result.content.size() == 1 &&
                      result.content.front().type == ChatBlockType::ToolUse,
                  "Qwen's strict textual tool object becomes a tool call");
            if (!result.content.empty())
                Check(result.content.front().tool_name == "trim_clip",
                      "the textual tool name is preserved");
        });

    Test("native Ollama history carries function calls and named tool results",
         [] {
             ChatHttpRequest captured;
             auto transport = [&](const ChatHttpRequest& request,
                                  ChatHttpResponse& response, std::string&) {
                 captured = request;
                 response.status_code = 200;
                 response.body =
                     R"({"message":{"role":"assistant","content":"fait"},)"
                     R"("done":true,"done_reason":"stop"})";
                 return true;
             };
             Value input = Value::MakeObject();
             input.Set("clip_id", Value::MakeString("01K3X"));
             ChatMessage assistant;
             assistant.role = "assistant";
             assistant.content.push_back(ChatContentBlock::MakeToolUse(
                 "ollama_text_tool", "trim_clip", input));
             ChatMessage resultMessage;
             resultMessage.role = "user";
             resultMessage.content.push_back(ChatContentBlock::MakeToolResult(
                 "ollama_text_tool", "{\"ok\":true}", false, "trim_clip"));
             ChatLlmClient client(OllamaNativeConfig(), transport);
             const ChatSendResult result = client.SendMessages(
                 {assistant, resultMessage}, "sys", OneToolCatalog());
             Check(result.ok, "the follow-up succeeds: " + result.error);

             Value body;
             std::string parseError;
             Check(Value::Parse(captured.body, body, parseError),
                   "follow-up body is valid JSON: " + parseError);
             const Value* messages = body.Find("messages");
             Check(messages && messages->IsArray() &&
                       messages->AsArray().size() == 3,
                   "system, assistant and tool messages are present");
             if (messages && messages->IsArray() &&
                 messages->AsArray().size() == 3) {
                 const Value& tool = messages->AsArray()[2];
                 const Value* role = tool.Find("role");
                 const Value* name = tool.Find("tool_name");
                 Check(role && role->IsString() && role->AsString() == "tool" &&
                           name && name->IsString() &&
                           name->AsString() == "trim_clip",
                       "native tool result names the invoked function");
             }
         });

    Test("native Ollama sends its optional key as a bearer token", [] {
        // PRD22 -- the engine dialog takes an optional key for Ollama; it
        // was kept in the Keychain and never sent.
        std::vector<std::pair<std::string, std::string>> sent;
        auto transport = [&](const ChatHttpRequest& request,
                             ChatHttpResponse& response, std::string&) {
            sent = request.headers;
            response.status_code = 200;
            response.body = R"({"message":{"role":"assistant","content":"ok"},)"
                            R"("done":true,"done_reason":"stop"})";
            return true;
        };
        const auto authorization = [&sent]() {
            for (const auto& header : sent)
                if (header.first == "Authorization") return header.second;
            return std::string();
        };
        ChatLlmConfig keyed = OllamaNativeConfig();
        keyed.api_key = "cle-ollama";
        ChatLlmClient client(keyed, transport);
        Check(client.SendMessages({}, "sys", {}).ok, "the keyed call succeeds");
        Check(
            authorization() == "Bearer cle-ollama",
            "the key goes as a bearer token, got \"" + authorization() + "\"");
        ChatLlmClient bare(OllamaNativeConfig(), transport);
        Check(bare.SendMessages({}, "sys", {}).ok, "the bare call succeeds");
        Check(authorization().empty(),
              "without a key, no Authorization header at all");
    });

    Test("a missing API key short-circuits before the transport runs", [] {
        bool transportCalled = false;
        auto transport = [&](const ChatHttpRequest&, ChatHttpResponse& response,
                             std::string&) {
            transportCalled = true;
            response.status_code = 200;
            response.body = "{}";
            return true;
        };
        ChatLlmConfig config = TestConfig();
        config.api_key.clear();
        ChatLlmClient client(config, transport);
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(!result.ok, "the call is reported as failed");
        Check(!transportCalled, "the transport is never invoked without a key");
    });

    // BENCH-2026-09 -- what a call spent is read from each provider's own
    // usage shape into one split, and a missing or malformed usage never
    // fails the turn it describes.
    auto respond = [](std::string body) {
        return [body](const ChatHttpRequest&, ChatHttpResponse& response,
                      std::string&) {
            response.status_code = 200;
            response.body = body;
            return true;
        };
    };

    Test("Anthropic usage keeps its input, cache and output split", [&] {
        ChatLlmClient client(
            TestConfig(),
            respond(
                R"({"content":[{"type":"text","text":"ok"}],)"
                R"("stop_reason":"end_turn","usage":{"input_tokens":12,)"
                R"("cache_creation_input_tokens":3400,)"
                R"("cache_read_input_tokens":56000,"output_tokens":789,)"
                R"("cache_creation":{"ephemeral_5m_input_tokens":3400}}})"));
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.usage.reported, "the usage is marked as reported");
        Check(result.usage.input_tokens == 12 &&
                  result.usage.cache_write_tokens == 3400 &&
                  result.usage.cache_read_tokens == 56000 &&
                  result.usage.output_tokens == 789,
              "each Anthropic count lands in its own field");
    });

    Test("a response without usage is not reported as zero tokens", [&] {
        ChatLlmClient client(
            TestConfig(),
            respond(R"({"content":[{"type":"text","text":"ok"}]})"));
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call still succeeds: " + result.error);
        Check(!result.usage.reported, "the usage is marked as not reported");
    });

    Test("a malformed usage is ignored instead of failing the turn", [&] {
        ChatLlmClient client(
            TestConfig(),
            respond(R"({"content":[{"type":"text","text":"ok"}],)"
                    R"("usage":{"input_tokens":"12","output_tokens":-4}})"));
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call still succeeds: " + result.error);
        Check(!result.usage.reported && result.usage.input_tokens == 0 &&
                  result.usage.output_tokens == 0,
              "neither a string nor a negative count is taken");
    });

    Test("OpenAI-compatible cached prompt tokens move out of the input", [&] {
        ChatLlmClient client(
            OllamaConfig(),
            respond(R"({"choices":[{"message":{"role":"assistant",)"
                    R"("content":"ok"},"finish_reason":"stop"}],)"
                    R"("usage":{"prompt_tokens":1000,"completion_tokens":50,)"
                    R"("prompt_tokens_details":{"cached_tokens":800}}})"));
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.usage.reported && result.usage.input_tokens == 200 &&
                  result.usage.cache_read_tokens == 800 &&
                  result.usage.cache_write_tokens == 0 &&
                  result.usage.output_tokens == 50,
              "prompt_tokens splits into uncached input and cache read");
    });

    Test("native Ollama eval counts become input and output", [&] {
        ChatLlmClient client(
            OllamaNativeConfig(),
            respond(R"({"message":{"role":"assistant","content":"ok"},)"
                    R"("done_reason":"stop","prompt_eval_count":2048,)"
                    R"("eval_count":96})"));
        const ChatSendResult result = client.SendMessages({}, "sys", {});
        Check(result.ok, "the call succeeds: " + result.error);
        Check(result.usage.reported && result.usage.input_tokens == 2048 &&
                  result.usage.output_tokens == 96,
              "prompt_eval_count and eval_count are read");
    });

    // QC-2026-08 -- a tool result carrying a picture must reach Anthropic as
    // an image block, not as a description of one. Without this the whole
    // read_frame path is a no-op the model never sees.
    {
        chat::ChatLlmConfig imageConfig;
        imageConfig.provider = chat::ChatLlmProvider::AnthropicMessages;
        imageConfig.model = "claude-sonnet-4-5";
        imageConfig.api_key = "test-key";
        imageConfig.base_url = "https://api.anthropic.test";
        std::string captured;
        chat::ChatLlmClient imageClient(
            imageConfig, [&captured](const ChatHttpRequest& request,
                                     ChatHttpResponse& response, std::string&) {
                captured = request.body;
                response.status_code = 200;
                response.body = R"({"content":[{"type":"text","text":"vu"}]})";
                return true;
            });
        chat::ChatMessage message;
        message.role = "user";
        message.content.push_back(chat::ChatContentBlock::MakeToolResult(
            "toolu_1", "{\"source_id\":\"M1\"}", false, "read_frame",
            "Zm9vYmFy", "image/jpeg"));
        std::vector<McpTool> noTools;
        const chat::ChatSendResult result =
            imageClient.SendMessages({message}, "system", noTools);
        Check(result.ok, "the image-carrying turn is sent: " + result.error);
        Check(captured.find("\"type\":\"image\"") != std::string::npos,
              "the request carries an image block");
        Check(
            captured.find("\"media_type\":\"image/jpeg\"") != std::string::npos,
            "with its MIME type");
        Check(captured.find("\"data\":\"Zm9vYmFy\"") != std::string::npos,
              "and the base64 payload itself");
        Check(captured.find("\"type\":\"text\",\"text\":\"{") !=
                  std::string::npos,
              "the tool's own text stays alongside the picture");

        // Every other tool result must keep the plain string form, so adding
        // pictures changed nothing about the traffic that already worked.
        std::string plainBody;
        chat::ChatLlmClient plainClient(
            imageConfig,
            [&plainBody](const ChatHttpRequest& request,
                         ChatHttpResponse& response, std::string&) {
                plainBody = request.body;
                response.status_code = 200;
                response.body = R"({"content":[{"type":"text","text":"ok"}]})";
                return true;
            });
        chat::ChatMessage plain;
        plain.role = "user";
        plain.content.push_back(chat::ChatContentBlock::MakeToolResult(
            "toolu_2", "{\"ok\":true}", false, "describe"));
        Check(plainClient.SendMessages({plain}, "system", noTools).ok,
              "a textual tool result still sends");
        Check(plainBody.find("\"type\":\"image\"") == std::string::npos &&
                  plainBody.find("\"content\":\"{") != std::string::npos,
              "a textual tool result keeps the plain string content form");
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All chat LLM client tests passed\n";
    return 0;
}
