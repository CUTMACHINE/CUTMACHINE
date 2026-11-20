#include "ChatLlmClient.h"

#include "LocalEnv.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace chat {

namespace {

using mcp_json::Value;

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

std::string EnvString(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

// ---------------------------------------------------------------------
// Request body assembly. Hand-built JSON text rather than mcp_json::Value
// trees, matching this project's existing convention at the same kind of
// boundary (see McpServer.cc's JsonRpcResult/ToolsListResultJson) --
// McpTool::input_schema_json in particular is already-serialized JSON text
// that only needs splicing in, not a parse/rebuild round trip.
// ---------------------------------------------------------------------

std::string DumpContentBlock(const ChatContentBlock& block) {
    switch (block.type) {
        case ChatBlockType::Text:
            return "{\"type\":\"text\",\"text\":\"" + Esc(block.text) + "\"}";
        case ChatBlockType::ToolUse:
            return "{\"type\":\"tool_use\",\"id\":\"" + Esc(block.tool_use_id) +
                   "\",\"name\":\"" + Esc(block.tool_name) +
                   "\",\"input\":" + block.tool_input.Dump() + "}";
        case ChatBlockType::ToolResult: {
            // A tool result carrying a picture becomes an array of blocks --
            // the text the tool returned, then the image. Anthropic accepts
            // an image inside a tool_result; the plain-string form stays for
            // every other tool so nothing about the existing traffic
            // changes.
            const std::string body =
                block.tool_image_base64.empty() || block.tool_image_mime.empty()
                    ? "\"" + Esc(block.text) + "\""
                    : "[{\"type\":\"text\",\"text\":\"" + Esc(block.text) +
                          "\"},{\"type\":\"image\",\"source\":{\"type\":"
                          "\"base64\",\"media_type\":\"" +
                          Esc(block.tool_image_mime) + "\",\"data\":\"" +
                          Esc(block.tool_image_base64) + "\"}}]";
            return "{\"type\":\"tool_result\",\"tool_use_id\":\"" +
                   Esc(block.tool_use_id) + "\",\"content\":" + body +
                   ",\"is_error\":" + (block.tool_is_error ? "true" : "false") +
                   "}";
        }
        case ChatBlockType::Opaque:
            return block.raw_json.empty() ? "{}" : block.raw_json;
    }
    return "{}";
}

// AGENT-2026-09 A3 -- ROADMAP.md. Anthropic's prompt caching: a request
// whose prefix -- tools, system prompt, then the conversation so far -- was
// seen within the last few minutes is read from cache at a tenth of the
// input price. The panel sends the whole catalog (over 200 kB) and the
// same system prompt on every one of its calls, up to eight per turn, so
// without a breakpoint every call paid for all of it again. Three
// breakpoints: the last tool, the system prompt and the last block of the
// last message, so the cached prefix grows with the conversation and each
// tool loop iteration reuses the one before. A prefix shorter than the
// model's minimum is simply not cached; nothing else changes.
constexpr char kCacheControl[] = ",\"cache_control\":{\"type\":\"ephemeral\"}";

// Marks a serialized JSON object as a cache breakpoint.
std::string WithCacheControl(std::string objectJson) {
    if (objectJson.size() < 2 || objectJson.back() != '}') return objectJson;
    if (objectJson == "{}") return std::string("{") + (kCacheControl + 1) + "}";
    objectJson.insert(objectJson.size() - 1, kCacheControl);
    return objectJson;
}

std::string DumpMessages(const std::vector<ChatMessage>& messages,
                         bool cacheLastBlock) {
    std::string json = "[";
    for (size_t index = 0; index < messages.size(); ++index) {
        if (index) json += ",";
        const ChatMessage& message = messages[index];
        json += "{\"role\":\"" + Esc(message.role) + "\",\"content\":[";
        for (size_t blockIndex = 0; blockIndex < message.content.size();
             ++blockIndex) {
            if (blockIndex) json += ",";
            std::string block = DumpContentBlock(message.content[blockIndex]);
            if (cacheLastBlock && index + 1 == messages.size() &&
                blockIndex + 1 == message.content.size())
                block = WithCacheControl(std::move(block));
            json += block;
        }
        json += "]}";
    }
    json += "]";
    return json;
}

std::string DumpTools(const std::vector<McpTool>& tools, bool cacheLastTool) {
    std::string json = "[";
    for (size_t index = 0; index < tools.size(); ++index) {
        if (index) json += ",";
        const McpTool& tool = tools[index];
        std::string entry = "{\"name\":\"" + Esc(tool.name) +
                            "\",\"description\":\"" + Esc(tool.description) +
                            "\",\"input_schema\":" + tool.input_schema_json +
                            "}";
        if (cacheLastTool && index + 1 == tools.size())
            entry = WithCacheControl(std::move(entry));
        json += entry;
    }
    json += "]";
    return json;
}

std::string BuildAnthropicRequestBody(const ChatLlmConfig& config,
                                      const std::vector<ChatMessage>& messages,
                                      const std::string& systemPrompt,
                                      const std::vector<McpTool>& tools) {
    return "{\"model\":\"" + Esc(config.model) +
           "\",\"max_tokens\":" + std::to_string(config.max_tokens) +
           ",\"system\":[" +
           WithCacheControl("{\"type\":\"text\",\"text\":\"" +
                            Esc(systemPrompt) + "\"}") +
           "],\"messages\":" + DumpMessages(messages, true) +
           ",\"tools\":" + DumpTools(tools, true) + "}";
}

std::string OpenAiTextContent(const ChatMessage& message) {
    std::string text;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::Text) continue;
        if (!text.empty()) text += "\n";
        text += block.text;
    }
    return text;
}

std::string DumpOpenAiAssistantMessage(const ChatMessage& message) {
    const std::string text = OpenAiTextContent(message);
    std::string json = "{\"role\":\"assistant\",\"content\":";
    json += text.empty() ? "null" : "\"" + Esc(text) + "\"";
    bool firstTool = true;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::ToolUse) continue;
        if (firstTool) json += ",\"tool_calls\":[";
        if (!firstTool) json += ",";
        firstTool = false;
        json += "{\"id\":\"" + Esc(block.tool_use_id) +
                "\",\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(block.tool_name) + "\",\"arguments\":\"" +
                Esc(block.tool_input.Dump()) + "\"}}";
    }
    if (!firstTool) json += "]";
    return json + "}";
}

std::string DumpOpenAiMessages(const std::vector<ChatMessage>& messages,
                               const std::string& systemPrompt) {
    std::string json =
        "[{\"role\":\"system\",\"content\":\"" + Esc(systemPrompt) + "\"}";
    for (const ChatMessage& message : messages) {
        if (message.role == "assistant") {
            json += "," + DumpOpenAiAssistantMessage(message);
            continue;
        }
        const std::string text = OpenAiTextContent(message);
        if (!text.empty())
            json += ",{\"role\":\"user\",\"content\":\"" + Esc(text) + "\"}";
        for (const ChatContentBlock& block : message.content) {
            if (block.type != ChatBlockType::ToolResult) continue;
            json += ",{\"role\":\"tool\",\"tool_call_id\":\"" +
                    Esc(block.tool_use_id) + "\",\"content\":\"" +
                    Esc(block.text) + "\"}";
        }
    }
    return json + "]";
}

std::string DumpOpenAiTools(const std::vector<McpTool>& tools) {
    std::string json = "[";
    for (size_t index = 0; index < tools.size(); ++index) {
        if (index) json += ",";
        const McpTool& tool = tools[index];
        json += "{\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(tool.name) + "\",\"description\":\"" +
                Esc(tool.description) +
                "\",\"parameters\":" + tool.input_schema_json + "}}";
    }
    return json + "]";
}

std::string BuildOpenAiRequestBody(const ChatLlmConfig& config,
                                   const std::vector<ChatMessage>& messages,
                                   const std::string& systemPrompt,
                                   const std::vector<McpTool>& tools) {
    std::string json =
        "{\"model\":\"" + Esc(config.model) +
        "\",\"max_tokens\":" + std::to_string(config.max_tokens) +
        ",\"messages\":" + DumpOpenAiMessages(messages, systemPrompt);
    if (!tools.empty())
        json += ",\"tools\":" + DumpOpenAiTools(tools) +
                ",\"tool_choice\":\"auto\"";
    return json + "}";
}

std::string DumpOllamaAssistantMessage(const ChatMessage& message) {
    const std::string text = OpenAiTextContent(message);
    std::string json =
        "{\"role\":\"assistant\",\"content\":\"" + Esc(text) + "\"";
    bool firstTool = true;
    for (const ChatContentBlock& block : message.content) {
        if (block.type != ChatBlockType::ToolUse) continue;
        if (firstTool) json += ",\"tool_calls\":[";
        if (!firstTool) json += ",";
        firstTool = false;
        json += "{\"type\":\"function\",\"function\":{\"name\":\"" +
                Esc(block.tool_name) +
                "\",\"arguments\":" + block.tool_input.Dump() + "}}";
    }
    if (!firstTool) json += "]";
    return json + "}";
}

std::string DumpOllamaMessages(const std::vector<ChatMessage>& messages,
                               const std::string& systemPrompt) {
    std::string json =
        "[{\"role\":\"system\",\"content\":\"" + Esc(systemPrompt) + "\"}";
    for (const ChatMessage& message : messages) {
        if (message.role == "assistant") {
            json += "," + DumpOllamaAssistantMessage(message);
            continue;
        }
        const std::string text = OpenAiTextContent(message);
        if (!text.empty())
            json += ",{\"role\":\"user\",\"content\":\"" + Esc(text) + "\"}";
        for (const ChatContentBlock& block : message.content) {
            if (block.type != ChatBlockType::ToolResult) continue;
            json += ",{\"role\":\"tool\",\"tool_name\":\"" +
                    Esc(block.tool_name) + "\",\"content\":\"" +
                    Esc(block.text) + "\"}";
        }
    }
    return json + "]";
}

std::string BuildOllamaRequestBody(const ChatLlmConfig& config,
                                   const std::vector<ChatMessage>& messages,
                                   const std::string& systemPrompt,
                                   const std::vector<McpTool>& tools) {
    std::string json =
        "{\"model\":\"" + Esc(config.model) +
        "\",\"stream\":false,\"options\":{\"temperature\":0," +
        "\"num_ctx\":" + std::to_string(config.context_tokens) +
        "},\"messages\":" + DumpOllamaMessages(messages, systemPrompt);
    if (!tools.empty()) json += ",\"tools\":" + DumpOpenAiTools(tools);
    return json + "}";
}

// Anthropic's error envelope:
// {"type":"error","error":{"type":...,"message":...}}. Falls back to the raw
// body when it doesn't match (matching sidecar/planner.py's _post_json, which
// reports the raw body text too).
std::string DescribeErrorBody(int statusCode, const std::string& body,
                              const std::string& apiName) {
    Value parsed;
    std::string parseError;
    std::string detail = body;
    if (Value::Parse(body, parsed, parseError) && parsed.IsObject()) {
        const Value* errorField = parsed.Find("error");
        const Value* messageField =
            errorField ? errorField->Find("message") : nullptr;
        if (messageField && messageField->IsString())
            detail = messageField->AsString();
    }
    return "HTTP " + std::to_string(statusCode) + " from " + apiName + ": " +
           detail;
}

// PRD22 -- a declined turn arrives as a 200 whose stop_reason is "refusal",
// with an optional stop_details naming the classifier's category (cyber,
// bio, reasoning_extraction...) and sometimes an explanation. Both are
// informational and may be null.
std::string DescribeRefusal(const Value& root) {
    std::string text =
        "the Anthropic API declined this request (stop_reason \"refusal\"";
    const Value* details = root.Find("stop_details");
    const Value* category = details ? details->Find("category") : nullptr;
    if (category && category->IsString() && !category->AsString().empty())
        text += ", category \"" + category->AsString() + "\"";
    text += ")";
    const Value* explanation = details ? details->Find("explanation") : nullptr;
    if (explanation && explanation->IsString() &&
        !explanation->AsString().empty())
        text += ": " + explanation->AsString();
    return text;
}

bool ParseAssistantContent(const Value& contentField,
                           std::vector<ChatContentBlock>& content,
                           std::string& error) {
    if (!contentField.IsArray()) {
        error = "response 'content' is not an array";
        return false;
    }
    for (const Value& blockValue : contentField.AsArray()) {
        const Value* typeField = blockValue.Find("type");
        if (!typeField || !typeField->IsString()) {
            error = "a content block is missing its 'type'";
            return false;
        }
        const std::string& blockType = typeField->AsString();
        if (blockType == "text") {
            const Value* textField = blockValue.Find("text");
            content.push_back(ChatContentBlock::MakeText(
                textField && textField->IsString() ? textField->AsString()
                                                   : std::string()));
        } else if (blockType == "tool_use") {
            const Value* idField = blockValue.Find("id");
            const Value* nameField = blockValue.Find("name");
            const Value* inputField = blockValue.Find("input");
            if (!idField || !idField->IsString() || !nameField ||
                !nameField->IsString()) {
                error = "a tool_use block is missing 'id' or 'name'";
                return false;
            }
            content.push_back(ChatContentBlock::MakeToolUse(
                idField->AsString(), nameField->AsString(),
                inputField ? *inputField : Value::MakeObject()));
        } else if (blockType == "thinking" ||
                   blockType == "redacted_thinking") {
            // Not acted on, but kept whole: a tool loop hands the model's
            // thinking back to it unchanged, signature included.
            content.push_back(ChatContentBlock::MakeOpaque(blockValue.Dump()));
        }
        // Any other block type is neither text nor a tool call the panel
        // acts on; skip rather than fail the turn.
    }
    return true;
}

// One non-negative integer count of a usage object. Absent, fractional or
// negative leaves `output` untouched and returns false: a count the provider
// did not give is not a zero.
bool ReadCount(const Value* object, const std::string& key, int64_t& output) {
    const Value* field = object != nullptr ? object->Find(key) : nullptr;
    int64_t value = 0;
    if (field == nullptr || !field->IsNumber() || !field->AsInt64(value) ||
        value < 0)
        return false;
    output = value;
    return true;
}

ChatUsage ParseAnthropicUsage(const Value& root) {
    ChatUsage usage;
    const Value* fields = root.Find("usage");
    bool any = ReadCount(fields, "input_tokens", usage.input_tokens);
    any |= ReadCount(fields, "cache_creation_input_tokens",
                     usage.cache_write_tokens);
    any |=
        ReadCount(fields, "cache_read_input_tokens", usage.cache_read_tokens);
    any |= ReadCount(fields, "output_tokens", usage.output_tokens);
    usage.reported = any;
    return usage;
}

ChatUsage ParseOpenAiUsage(const Value& root) {
    ChatUsage usage;
    const Value* fields = root.Find("usage");
    int64_t prompt = 0;
    bool any = ReadCount(fields, "prompt_tokens", prompt);
    any |= ReadCount(fields, "completion_tokens", usage.output_tokens);
    const Value* details =
        fields != nullptr ? fields->Find("prompt_tokens_details") : nullptr;
    ReadCount(details, "cached_tokens", usage.cache_read_tokens);
    // prompt_tokens already counts the cached part; split it out so the
    // uncached input means the same thing for every provider.
    usage.cache_read_tokens = std::min(usage.cache_read_tokens, prompt);
    usage.input_tokens = prompt - usage.cache_read_tokens;
    usage.reported = any;
    return usage;
}

ChatUsage ParseOllamaUsage(const Value& root) {
    ChatUsage usage;
    // Ollama drops prompt_eval_count when the whole prompt came from its KV
    // cache, so one count alone is still a report.
    bool any = ReadCount(&root, "prompt_eval_count", usage.input_tokens);
    any |= ReadCount(&root, "eval_count", usage.output_tokens);
    usage.reported = any;
    return usage;
}

bool ParseTextToolCall(const std::string& text,
                       const std::vector<McpTool>& tools,
                       ChatContentBlock& block) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(text, parsed, parseError) || !parsed.IsObject() ||
        parsed.AsObject().size() != 2)
        return false;
    const Value* name = parsed.Find("name");
    const Value* arguments = parsed.Find("arguments");
    if (!name || !name->IsString() || !arguments || !arguments->IsObject())
        return false;
    bool known = false;
    for (const McpTool& tool : tools) known |= tool.name == name->AsString();
    if (!known) return false;
    block = ChatContentBlock::MakeToolUse("ollama_text_tool", name->AsString(),
                                          *arguments);
    return true;
}

bool ParseOpenAiAssistant(const Value& root, const std::vector<McpTool>& tools,
                          ChatSendResult& result, std::string& error) {
    const Value* choices = root.Find("choices");
    if (!choices || !choices->IsArray() || choices->AsArray().empty()) {
        error = "response has no non-empty 'choices' array";
        return false;
    }
    const Value& choice = choices->AsArray().front();
    const Value* message = choice.Find("message");
    if (!message || !message->IsObject()) {
        error = "first choice has no 'message' object";
        return false;
    }
    const Value* toolCalls = message->Find("tool_calls");
    if (toolCalls) {
        if (!toolCalls->IsArray()) {
            error = "message 'tool_calls' is not an array";
            return false;
        }
        for (const Value& call : toolCalls->AsArray()) {
            const Value* id = call.Find("id");
            const Value* function = call.Find("function");
            const Value* name = function ? function->Find("name") : nullptr;
            const Value* arguments =
                function ? function->Find("arguments") : nullptr;
            if (!id || !id->IsString() || !name || !name->IsString() ||
                !arguments || !arguments->IsString()) {
                error = "a tool call is missing id, function name or arguments";
                return false;
            }
            Value input;
            std::string parseError;
            if (!Value::Parse(arguments->AsString(), input, parseError)) {
                error = "tool arguments are malformed JSON: " + parseError;
                return false;
            }
            result.content.push_back(ChatContentBlock::MakeToolUse(
                id->AsString(), name->AsString(), std::move(input)));
        }
    }
    const Value* text = message->Find("content");
    if (text && text->IsString() && !text->AsString().empty()) {
        ChatContentBlock textTool;
        if (!toolCalls && ParseTextToolCall(text->AsString(), tools, textTool))
            result.content.push_back(std::move(textTool));
        else
            result.content.push_back(
                ChatContentBlock::MakeText(text->AsString()));
    }
    const Value* finishReason = choice.Find("finish_reason");
    if (finishReason && finishReason->IsString()) {
        result.stop_reason = finishReason->AsString() == "tool_calls"
                                 ? "tool_use"
                                 : finishReason->AsString();
    }
    return true;
}

bool ParseOllamaAssistant(const Value& root, const std::vector<McpTool>& tools,
                          ChatSendResult& result, std::string& error) {
    const Value* message = root.Find("message");
    if (!message || !message->IsObject()) {
        error = "response has no 'message' object";
        return false;
    }
    const Value* toolCalls = message->Find("tool_calls");
    if (toolCalls) {
        if (!toolCalls->IsArray()) {
            error = "message 'tool_calls' is not an array";
            return false;
        }
        size_t index = 0;
        for (const Value& call : toolCalls->AsArray()) {
            const Value* function = call.Find("function");
            const Value* name = function ? function->Find("name") : nullptr;
            const Value* arguments =
                function ? function->Find("arguments") : nullptr;
            if (!name || !name->IsString() || !arguments ||
                !arguments->IsObject()) {
                error = "a tool call is missing function name or arguments";
                return false;
            }
            result.content.push_back(ChatContentBlock::MakeToolUse(
                "ollama_tool_" + std::to_string(index++), name->AsString(),
                *arguments));
        }
    }
    const Value* text = message->Find("content");
    if (text && text->IsString() && !text->AsString().empty()) {
        ChatContentBlock textTool;
        if (!toolCalls && ParseTextToolCall(text->AsString(), tools, textTool))
            result.content.push_back(std::move(textTool));
        else
            result.content.push_back(
                ChatContentBlock::MakeText(text->AsString()));
    }
    const Value* doneReason = root.Find("done_reason");
    result.stop_reason = doneReason != nullptr && doneReason->IsString()
                             ? doneReason->AsString()
                             : "end_turn";
    if (result.stop_reason != "length" && result.stop_reason != "max_tokens" &&
        !result.content.empty() &&
        result.content.back().type == ChatBlockType::ToolUse)
        result.stop_reason = "tool_use";
    return true;
}

}  // namespace

ChatContentBlock ChatContentBlock::MakeOpaque(std::string rawJson) {
    ChatContentBlock block;
    block.type = ChatBlockType::Opaque;
    block.raw_json = std::move(rawJson);
    return block;
}

ChatContentBlock ChatContentBlock::MakeText(std::string text) {
    ChatContentBlock block;
    block.type = ChatBlockType::Text;
    block.text = std::move(text);
    return block;
}

ChatContentBlock ChatContentBlock::MakeToolUse(std::string id, std::string name,
                                               mcp_json::Value input) {
    ChatContentBlock block;
    block.type = ChatBlockType::ToolUse;
    block.tool_use_id = std::move(id);
    block.tool_name = std::move(name);
    block.tool_input = std::move(input);
    return block;
}

ChatContentBlock ChatContentBlock::MakeToolResult(
    std::string toolUseId, std::string text, bool isError, std::string toolName,
    std::string imageBase64, std::string imageMime) {
    ChatContentBlock block;
    block.type = ChatBlockType::ToolResult;
    block.tool_use_id = std::move(toolUseId);
    block.text = std::move(text);
    block.tool_name = std::move(toolName);
    block.tool_is_error = isError;
    block.tool_image_base64 = std::move(imageBase64);
    block.tool_image_mime = std::move(imageMime);
    return block;
}

ChatLlmConfig ChatLlmConfig::FromEnvironment(std::string* missingKeyError) {
    local_env::LoadLocalEnvFileIfPresent();

    ChatLlmConfig config;
    const std::string explicitModel = EnvString("CUTMACHINE_ANTHROPIC_MODEL");
    const std::string genericModel = EnvString("CUTMACHINE_MODEL");
    config.model = !explicitModel.empty()  ? explicitModel
                   : !genericModel.empty() ? genericModel
                                           : config.model;
    config.api_key = EnvString("ANTHROPIC_API_KEY");
    const std::string baseUrl = EnvString("CUTMACHINE_ANTHROPIC_URL");
    if (!baseUrl.empty()) config.base_url = baseUrl;

    if (config.api_key.empty() && missingKeyError) {
        *missingKeyError =
            "ANTHROPIC_API_KEY is not set. CUTMACHINE talks to Anthropic "
            "directly with your own key (PHILOSOPHY.md: pas de service "
            "central) -- export ANTHROPIC_API_KEY, or put it in "
            "~/.config/cutmachine/.env, or point CUTMACHINE_ENV_FILE at a "
            "file that sets it.";
    }
    return config;
}

ChatLlmClient::ChatLlmClient(ChatLlmConfig config, ChatHttpTransport transport)
    : config_(std::move(config)), transport_(std::move(transport)) {}

ChatSendResult ChatLlmClient::SendMessages(
    const std::vector<ChatMessage>& messages, const std::string& systemPrompt,
    const std::vector<McpTool>& tools) const {
    ChatSendResult result;
    if (config_.provider == ChatLlmProvider::AnthropicMessages &&
        config_.api_key.empty()) {
        result.error =
            "ANTHROPIC_API_KEY is not set; see ChatLlmConfig::FromEnvironment.";
        return result;
    }

    ChatHttpRequest request;
    request.headers = {{"Content-Type", "application/json"}};
    request.timeout_seconds = config_.timeout_seconds;
    if (config_.provider == ChatLlmProvider::AnthropicMessages) {
        request.url = config_.base_url + "/v1/messages";
        request.headers.push_back({"x-api-key", config_.api_key});
        request.headers.push_back(
            {"anthropic-version", config_.anthropic_version});
        request.body =
            BuildAnthropicRequestBody(config_, messages, systemPrompt, tools);
    } else if (config_.provider == ChatLlmProvider::OpenAiCompatible) {
        request.url = config_.base_url + "/chat/completions";
        if (!config_.api_key.empty())
            request.headers.push_back(
                {"Authorization", "Bearer " + config_.api_key});
        request.body =
            BuildOpenAiRequestBody(config_, messages, systemPrompt, tools);
    } else {
        request.url = config_.base_url + "/api/chat";
        // PRD22 -- the engine dialog takes an optional key for Ollama and
        // the Keychain kept it, but nothing sent it. A local server needs
        // none and ignores the header; one behind a proxy, or ollama.com,
        // reads it as a bearer token.
        if (!config_.api_key.empty())
            request.headers.push_back(
                {"Authorization", "Bearer " + config_.api_key});
        request.body =
            BuildOllamaRequestBody(config_, messages, systemPrompt, tools);
    }

    ChatHttpResponse response;
    std::string transportError;
    if (!transport_) {
        result.error = "no HTTP transport configured";
        return result;
    }
    if (!transport_(request, response, transportError)) {
        result.error = "unable to reach " + request.url + ": " + transportError;
        return result;
    }
    if (response.status_code != 200) {
        const std::string apiName =
            config_.provider == ChatLlmProvider::AnthropicMessages
                ? "the Anthropic API"
            : config_.provider == ChatLlmProvider::Ollama
                ? "the Ollama API"
                : "the OpenAI-compatible API";
        result.error =
            DescribeErrorBody(response.status_code, response.body, apiName);
        return result;
    }

    Value parsed;
    std::string parseError;
    if (!Value::Parse(response.body, parsed, parseError)) {
        result.error = "the model API returned malformed JSON: " + parseError;
        return result;
    }
    if (!parsed.IsObject()) {
        result.error = "the model API response is not a JSON object";
        return result;
    }
    std::string contentError;
    if (config_.provider == ChatLlmProvider::AnthropicMessages) {
        const Value* contentField = parsed.Find("content");
        if (!contentField) {
            result.error = "the Anthropic API response has no 'content'";
            return result;
        }
        if (!ParseAssistantContent(*contentField, result.content,
                                   contentError)) {
            result.error =
                "the Anthropic API response is malformed: " + contentError;
            return result;
        }
        const Value* stopReasonField = parsed.Find("stop_reason");
        if (stopReasonField && stopReasonField->IsString())
            result.stop_reason = stopReasonField->AsString();
        // PRD22 -- the default model runs safety classifiers that can
        // decline a turn, its content then empty or partial. Ending the
        // turn as if it had finished told the person nothing.
        if (result.stop_reason == "refusal") {
            result.content.clear();
            result.error = DescribeRefusal(parsed);
            return result;
        }
    } else if (config_.provider == ChatLlmProvider::OpenAiCompatible &&
               !ParseOpenAiAssistant(parsed, tools, result, contentError)) {
        result.error =
            "the OpenAI-compatible response is malformed: " + contentError;
        return result;
    } else if (config_.provider == ChatLlmProvider::Ollama &&
               !ParseOllamaAssistant(parsed, tools, result, contentError)) {
        result.error = "the Ollama response is malformed: " + contentError;
        return result;
    }
    // What the call used, read first: a reply refused below was still paid.
    switch (config_.provider) {
        case ChatLlmProvider::AnthropicMessages:
            result.usage = ParseAnthropicUsage(parsed);
            break;
        case ChatLlmProvider::OpenAiCompatible:
            result.usage = ParseOpenAiUsage(parsed);
            break;
        case ChatLlmProvider::Ollama:
            result.usage = ParseOllamaUsage(parsed);
            break;
    }
    // FI8 -- docs/roadmap/FIABILITE-2026-10.md. A complete JSON tool input
    // can still belong to a reply cut before the rest of its calls. Apply
    // none of that reply, for every provider and for text-encoded calls too.
    // Anthropic also cuts a reply at the end of the model's context window.
    const bool callsTools =
        std::any_of(result.content.begin(), result.content.end(),
                    [](const ChatContentBlock& block) {
                        return block.type == ChatBlockType::ToolUse;
                    });
    const bool contextFull =
        result.stop_reason == "model_context_window_exceeded";
    if ((result.stop_reason == "max_tokens" || result.stop_reason == "length" ||
         contextFull) &&
        callsTools) {
        result.content.clear();
        result.error =
            "the model reply reached " + result.stop_reason +
            (contextFull
                 ? std::string()
                 : " (max_tokens " + std::to_string(config_.max_tokens) + ")") +
            "; none of its tool calls were run";
        return result;
    }
    result.ok = true;
    return result;
}

}  // namespace chat
