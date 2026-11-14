#include "AcpClient.h"

#include "Generation.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace acp {

namespace {

using mcp_json::Value;

// The same words as chat::ChatSession's stop, so a stopped turn reads the
// same in the panel whichever agent ran it.
constexpr char kStopped[] = "tour arrêté à la demande de l'utilisateur";
// What a turn adds when its agent is gone: the conversation goes with it.
constexpr char kNewConversation[] =
    " ; la consigne suivante ouvrira une nouvelle conversation";

// JSON-RPC's "the server needs authentication first", as ACP uses it.
constexpr int64_t kAuthRequired = -32000;
constexpr int64_t kMethodNotFound = -32601;

std::string StringAt(const Value* value) {
    return value != nullptr && value->IsString() ? value->AsString()
                                                 : std::string();
}

int64_t IntAt(const Value* value) {
    int64_t number = 0;
    if (value != nullptr && value->IsNumber()) {
        value->AsInt64(number);
    }
    return number;
}

// GN2 -- docs/roadmap/GENERATIF-2026-10.md. The engine calls the panel puts
// to the person rather than approving them on their behalf: one that spends
// money on their account. The after_* calls stay approved, like every other
// engine tool. Each is put to them once: "allow_always" would approve every
// later one without showing it, so it is not offered.
bool EngineCallGoesToPerson(const std::string& tool,
                            const std::string& rawInputJson) {
    if (ToolSpendsOnPersonsAccount(tool)) {
        return true;
    }
    (void)rawInputJson;
    return false;
}

const Value* FindPath(const Value& root,
                      std::initializer_list<const char*> keys) {
    const Value* current = &root;
    for (const char* key : keys) {
        current = current->Find(key);
        if (current == nullptr) {
            return nullptr;
        }
    }
    return current;
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.size() > prefix.size() &&
           text.compare(0, prefix.size(), prefix) == 0;
}

// Arguments worth showing: present, and not the empty object an agent sends
// before it has streamed them.
bool HasInput(const Value* value) {
    if (value == nullptr || value->IsNull()) {
        return false;
    }
    return !value->IsObject() || !value->AsObject().empty();
}

bool IsBlank(const std::string& text) {
    for (const char character : text) {
        if (character != ' ' && character != '\n' && character != '\r' &&
            character != '\t') {
            return false;
        }
    }
    return true;
}

Value TextBlock(const std::string& text) {
    Value block = Value::MakeObject();
    block.Set("type", Value::MakeString("text"));
    block.Set("text", Value::MakeString(text));
    return block;
}

Value Outcome(const std::string& optionId) {
    Value outcome = Value::MakeObject();
    if (optionId.empty()) {
        outcome.Set("outcome", Value::MakeString("cancelled"));
    } else {
        outcome.Set("outcome", Value::MakeString("selected"));
        outcome.Set("optionId", Value::MakeString(optionId));
    }
    Value result = Value::MakeObject();
    result.Set("outcome", std::move(outcome));
    return result;
}

std::string StopReasonText(const std::string& reason) {
    if (reason == "max_tokens") {
        return "l'agent a atteint sa limite de sortie avant la fin du tour";
    }
    if (reason == "max_turn_requests") {
        return "l'agent a atteint le nombre maximal de requêtes du tour";
    }
    if (reason == "refusal") {
        return "l'agent a refusé de poursuivre";
    }
    return "l'agent a arrêté le tour (" +
           (reason.empty() ? std::string("sans raison donnée") : reason) + ")";
}

std::string PlanText(const Value& entries) {
    std::string text;
    for (const Value& entry : entries.AsArray()) {
        const std::string status = StringAt(entry.Find("status"));
        const char* mark = status == "completed"     ? "✓"
                           : status == "in_progress" ? "▸"
                                                     : "○";
        if (!text.empty()) {
            text += '\n';
        }
        text += std::string(mark) + " " + StringAt(entry.Find("content"));
    }
    return text;
}

Value McpServerJson(const AcpMcpServer& server) {
    Value json = Value::MakeObject();
    json.Set("name", Value::MakeString(server.name));
    json.Set("command", Value::MakeString(server.command));
    Value args = Value::MakeArray();
    for (const std::string& arg : server.args) {
        args.Push(Value::MakeString(arg));
    }
    json.Set("args", std::move(args));
    Value env = Value::MakeArray();
    for (const auto& [name, value] : server.env) {
        Value variable = Value::MakeObject();
        variable.Set("name", Value::MakeString(name));
        variable.Set("value", Value::MakeString(value));
        env.Push(std::move(variable));
    }
    json.Set("env", std::move(env));
    return json;
}

}  // namespace

bool IsEngineToolCall(const Value& toolCall, const std::string& serverName) {
    if (serverName.empty() || !toolCall.IsObject()) {
        return false;
    }
    const std::string claudePrefix = "mcp__" + serverName + "__";
    const std::string codexPrefix = "mcp." + serverName + ".";
    if (const Value* meta = FindPath(toolCall, {"_meta", "claudeCode"})) {
        if (StringAt(FindPath(*meta, {"mcpServer", "name"})) == serverName ||
            StartsWith(StringAt(meta->Find("toolName")), claudePrefix)) {
            return true;
        }
    }
    for (const char* key : {"title", "name"}) {
        const std::string text = StringAt(toolCall.Find(key));
        if (StartsWith(text, claudePrefix) || StartsWith(text, codexPrefix)) {
            return true;
        }
    }
    const Value* rawInput = toolCall.Find("rawInput");
    return rawInput != nullptr && rawInput->IsObject() &&
           StringAt(rawInput->Find("server")) == serverName &&
           rawInput->Find("tool") != nullptr;
}

std::string DisplayToolName(const std::string& title,
                            const std::string& serverName) {
    for (const std::string& prefix :
         {"mcp__" + serverName + "__", "mcp." + serverName + "."}) {
        if (StartsWith(title, prefix)) {
            return title.substr(prefix.size());
        }
    }
    return title;
}

AcpChatSession::AcpChatSession(
    AcpTransportFactory factory, AcpSessionConfig config,
    std::function<void(const chat::ChatTranscriptEntry&)> onEntry,
    AcpPermissionPrompt permissionPrompt)
    : factory_(std::move(factory)),
      config_(std::move(config)),
      on_entry_(std::move(onEntry)),
      permission_prompt_(std::move(permissionPrompt)) {}

AcpChatSession::~AcpChatSession() { Reset(); }

void AcpChatSession::Emit(chat::ChatTranscriptEntry entry) {
    if (on_entry_) {
        on_entry_(entry);
    }
    transcript_.push_back(std::move(entry));
}

void AcpChatSession::EmitError(const std::string& text) {
    FlushText();
    chat::ChatTranscriptEntry entry;
    entry.kind = chat::ChatEntryKind::Error;
    entry.text = text;
    Emit(std::move(entry));
}

void AcpChatSession::FlushText() {
    if (IsBlank(text_buffer_)) {
        text_buffer_.clear();
        return;
    }
    chat::ChatTranscriptEntry entry;
    entry.kind = chat::ChatEntryKind::AssistantText;
    entry.text = std::move(text_buffer_);
    text_buffer_.clear();
    Emit(std::move(entry));
}

bool AcpChatSession::Send(const Value& message, std::string& error) {
    const std::string line = message.Dump();
    if (trace_) {
        trace_(true, line);
    }
    if (!transport_) {
        error = "aucun agent en cours";
        return false;
    }
    return transport_->WriteLine(line, error);
}

int64_t AcpChatSession::SendRequest(const std::string& method, Value params,
                                    std::string& error) {
    const int64_t id = next_request_id_++;
    Value request = Value::MakeObject();
    request.Set("jsonrpc", Value::MakeString("2.0"));
    request.Set("id", Value::MakeInt(id));
    request.Set("method", Value::MakeString(method));
    request.Set("params", std::move(params));
    return Send(request, error) ? id : 0;
}

AcpChatSession::CallOutcome AcpChatSession::Call(const std::string& method,
                                                 Value params, Value& result,
                                                 std::string& error) {
    const int64_t id = SendRequest(method, std::move(params), error);
    if (id == 0) {
        return CallOutcome::WriteFailed;
    }
    return AwaitResponse(id, result, error);
}

AcpChatSession::CallOutcome AcpChatSession::AwaitResponse(int64_t id,
                                                          Value& result,
                                                          std::string& error) {
    const std::string expectedId = std::to_string(id);
    std::string line;
    while (transport_->ReadLine(line)) {
        if (trace_) {
            trace_(false, line);
        }
        Value message;
        std::string parseError;
        // Stdout is the protocol's; a line that is not a JSON-RPC message
        // answers nothing and is skipped rather than ending the session.
        if (!Value::Parse(line, message, parseError) || !message.IsObject()) {
            continue;
        }
        if (message.Find("method") != nullptr) {
            HandleMessage(message);
            continue;
        }
        const Value* responseId = message.Find("id");
        if (responseId == nullptr || responseId->Dump() != expectedId) {
            continue;
        }
        if (const Value* failure = message.Find("error")) {
            last_error_code_ = IntAt(failure->Find("code"));
            error = StringAt(failure->Find("message"));
            if (error.empty()) {
                error = failure->Dump();
            }
            return CallOutcome::RpcError;
        }
        const Value* value = message.Find("result");
        result = value != nullptr ? *value : Value::MakeNull();
        return CallOutcome::Result;
    }
    error = "l'agent s'est arrêté";
    const std::string hint = transport_->DiagnosticHint();
    if (!hint.empty()) {
        error += " (sa sortie d'erreur : " + hint + ")";
    }
    return CallOutcome::Closed;
}

void AcpChatSession::HandleMessage(const Value& message) {
    const std::string method = StringAt(message.Find("method"));
    Value params = Value::MakeObject();
    if (const Value* given = message.Find("params");
        given != nullptr && given->IsObject()) {
        params = *given;
    }
    const Value* id = message.Find("id");
    if (id == nullptr) {
        if (method == "session/update" && !session_id_.empty() &&
            StringAt(params.Find("sessionId")) == session_id_) {
            if (const Value* update = params.Find("update")) {
                HandleUpdate(*update);
            }
        }
        return;
    }
    Value response = Value::MakeObject();
    response.Set("jsonrpc", Value::MakeString("2.0"));
    response.Set("id", *id);
    if (method == "session/request_permission") {
        response.Set("result", DecidePermission(params));
    } else {
        // Files and terminals stay the agent's own: CUTMACHINE advertises
        // neither, so an agent asking anyway is told the method is absent.
        Value failure = Value::MakeObject();
        failure.Set("code", Value::MakeInt(kMethodNotFound));
        failure.Set("message",
                    Value::MakeString("method not found: " + method));
        response.Set("error", std::move(failure));
    }
    std::string ignored;
    Send(response, ignored);
}

void AcpChatSession::HandleUpdate(const Value& update) {
    const std::string kind = StringAt(update.Find("sessionUpdate"));
    if (kind == "agent_message_chunk") {
        const Value* content = update.Find("content");
        if (content != nullptr && StringAt(content->Find("type")) == "text") {
            text_buffer_ += StringAt(content->Find("text"));
        }
        return;
    }
    if (kind == "tool_call" || kind == "tool_call_update") {
        TrackTool(update);
        return;
    }
    if (kind == "plan") {
        const Value* entries = update.Find("entries");
        if (entries == nullptr || !entries->IsArray()) {
            return;
        }
        std::string text = PlanText(*entries);
        if (text == last_plan_) {
            return;
        }
        last_plan_ = text;
        FlushText();
        chat::ChatTranscriptEntry entry;
        entry.kind = chat::ChatEntryKind::Plan;
        entry.text = std::move(text);
        Emit(std::move(entry));
        return;
    }
    if (kind == "usage_update") {
        usage_.context_used = IntAt(update.Find("used"));
        usage_.context_size = IntAt(update.Find("size"));
    }
    // Thoughts, commands, modes and titles change nothing the panel shows.
}

void AcpChatSession::TrackTool(const Value& toolCall) {
    const std::string id = StringAt(toolCall.Find("toolCallId"));
    if (id.empty()) {
        return;
    }
    ToolState& tool = tools_[id];
    if (tool.finished) {
        return;
    }
    const bool engine = IsEngineToolCall(toolCall, config_.engine_server_name);
    tool.engine = tool.engine || engine;
    // An engine call is named by its tool, which the Claude adapter keeps in
    // _meta when its title is a sentence; any other call by its title, which
    // says more than a tool name ("ls -la" rather than "Bash").
    const std::string metaName =
        StringAt(FindPath(toolCall, {"_meta", "claudeCode", "toolName"}));
    const std::string title = StringAt(toolCall.Find("title"));
    const std::string name = StringAt(toolCall.Find("name"));
    if (engine && !metaName.empty()) {
        tool.title = metaName;
    } else if (!title.empty() && (!tool.engine || tool.title.empty())) {
        tool.title = title;
    } else if (tool.title.empty()) {
        tool.title = name;
    }
    const Value* rawInput = toolCall.Find("rawInput");
    if (HasInput(rawInput)) {
        tool.raw_input_json = rawInput->Dump();
    }
    const std::string status = StringAt(toolCall.Find("status"));
    if (status == "completed" || status == "failed") {
        FinishTool(tool, toolCall);
        return;
    }
    if (!tool.announced &&
        (!tool.raw_input_json.empty() || status == "in_progress")) {
        AnnounceTool(tool);
    }
}

void AcpChatSession::AnnounceTool(ToolState& tool) {
    FlushText();
    tool.announced = true;
    chat::ChatTranscriptEntry entry;
    entry.kind = chat::ChatEntryKind::ToolCall;
    entry.tool_name = DisplayToolName(tool.title, config_.engine_server_name);
    entry.tool_args_json = tool.raw_input_json;
    Emit(std::move(entry));
}

void AcpChatSession::FinishTool(ToolState& tool, const Value& update) {
    if (!tool.announced) {
        AnnounceTool(tool);
    }
    tool.finished = true;
    chat::ChatTranscriptEntry entry;
    entry.kind = chat::ChatEntryKind::ToolResult;
    entry.tool_name = DisplayToolName(tool.title, config_.engine_server_name);
    entry.tool_ok = StringAt(update.Find("status")) == "completed";
    std::string text;
    const auto append = [&text](const std::string& piece) {
        if (piece.empty()) {
            return;
        }
        if (!text.empty()) {
            text += '\n';
        }
        text += piece;
    };
    const Value* content = update.Find("content");
    if (content != nullptr && content->IsArray()) {
        for (const Value& item : content->AsArray()) {
            const std::string type = StringAt(item.Find("type"));
            if (type == "content") {
                const Value* block = item.Find("content");
                if (block == nullptr) {
                    continue;
                }
                const std::string blockType = StringAt(block->Find("type"));
                if (blockType == "text") {
                    append(StringAt(block->Find("text")));
                } else if (blockType == "image" && entry.image_base64.empty()) {
                    entry.image_base64 = StringAt(block->Find("data"));
                    entry.image_mime = StringAt(block->Find("mimeType"));
                } else if (blockType == "resource_link") {
                    append(StringAt(block->Find("uri")));
                }
            } else if (type == "diff") {
                append("diff " + StringAt(item.Find("path")));
            } else if (type == "terminal") {
                append("terminal " + StringAt(item.Find("terminalId")));
            }
        }
    }
    if (text.empty()) {
        const Value* rawOutput = update.Find("rawOutput");
        if (rawOutput != nullptr && !rawOutput->IsNull()) {
            text = rawOutput->IsString() ? rawOutput->AsString()
                                         : rawOutput->Dump();
        }
    }
    entry.text = std::move(text);
    Emit(std::move(entry));
}

Value AcpChatSession::DecidePermission(const Value& params) {
    if (stop_requested_) {
        return Outcome({});
    }
    AcpPermissionRequest request;
    if (const Value* options = params.Find("options");
        options != nullptr && options->IsArray()) {
        for (const Value& option : options->AsArray()) {
            request.options.push_back({StringAt(option.Find("optionId")),
                                       StringAt(option.Find("name")),
                                       StringAt(option.Find("kind"))});
        }
    }
    const auto firstOfKind =
        [&request](std::initializer_list<const char*> kinds) -> std::string {
        for (const char* kind : kinds) {
            for (const AcpPermissionOption& option : request.options) {
                if (option.kind == kind && !option.id.empty()) {
                    return option.id;
                }
            }
        }
        return {};
    };

    ToolState* tool = nullptr;
    if (const Value* toolCall = params.Find("toolCall");
        toolCall != nullptr && toolCall->IsObject()) {
        const std::string id = StringAt(toolCall->Find("toolCallId"));
        if (!id.empty()) {
            TrackTool(*toolCall);
            tool = &tools_[id];
        }
        request.kind = StringAt(toolCall->Find("kind"));
        if (tool == nullptr) {
            request.title = StringAt(toolCall->Find("title"));
        }
    }
    if (tool != nullptr) {
        request.title =
            DisplayToolName(tool->title, config_.engine_server_name);
        request.raw_input_json = tool->raw_input_json;
        // GENERATIF-2026-10 GN2 -- docs/roadmap/GENERATIF-2026-10.md. The
        // engine's tools are approved on the person's behalf, save the ones
        // EngineCallGoesToPerson names: those go to the person, and only
        // once ("allow_always" would approve every later one, so it is not
        // offered).
        if (tool->engine &&
            EngineCallGoesToPerson(request.title, request.raw_input_json)) {
            request.options.erase(
                std::remove_if(request.options.begin(), request.options.end(),
                               [](const AcpPermissionOption& option) {
                                   return option.kind == "allow_always";
                               }),
                request.options.end());
        } else if (tool->engine) {
            const std::string allow =
                firstOfKind({"allow_once", "allow_always"});
            if (!allow.empty()) {
                return Outcome(allow);
            }
        }
    }
    if (!permission_prompt_) {
        return Outcome(firstOfKind({"reject_once", "reject_always"}));
    }
    if (tool != nullptr && !tool->announced && !tool->finished) {
        AnnounceTool(*tool);
    }
    const std::string chosen = permission_prompt_(request);
    for (const AcpPermissionOption& option : request.options) {
        if (!chosen.empty() && option.id == chosen) {
            return Outcome(chosen);
        }
    }
    return Outcome({});
}

bool AcpChatSession::EnsureSession(std::string& error) {
    if (transport_ && transport_closed_) {
        Reset();
    }
    if (transport_ && !session_id_.empty()) {
        return true;
    }
    if (!transport_) {
        if (stop_requested_) {
            error = kStopped;
            return false;
        }
        if (!factory_) {
            error = "aucun agent configuré";
            return false;
        }
        std::unique_ptr<AcpTransport> transport = factory_(error);
        if (!transport) {
            if (error.empty()) {
                error = "l'agent n'a pas pu démarrer";
            }
            return false;
        }
        const std::lock_guard<std::mutex> lock(transport_mutex_);
        transport_ = std::move(transport);
    }
    if (stop_requested_) {
        Reset();
        error = kStopped;
        return false;
    }

    Value fs = Value::MakeObject();
    fs.Set("readTextFile", Value::MakeBool(false));
    fs.Set("writeTextFile", Value::MakeBool(false));
    Value capabilities = Value::MakeObject();
    capabilities.Set("fs", std::move(fs));
    capabilities.Set("terminal", Value::MakeBool(false));
    Value clientInfo = Value::MakeObject();
    clientInfo.Set("name", Value::MakeString("cutmachine"));
    clientInfo.Set("title", Value::MakeString("CUTMACHINE"));
    clientInfo.Set("version", Value::MakeString("dev"));
    Value initialize = Value::MakeObject();
    initialize.Set("protocolVersion", Value::MakeInt(kProtocolVersion));
    initialize.Set("clientCapabilities", std::move(capabilities));
    initialize.Set("clientInfo", std::move(clientInfo));

    Value agent;
    if (Call("initialize", std::move(initialize), agent, error) !=
        CallOutcome::Result) {
        error = stop_requested_
                    ? std::string(kStopped)
                    : "l'agent n'a pas répondu à l'initialisation : " + error;
        Reset();
        return false;
    }
    int64_t version = 0;
    const Value* versionValue = agent.Find("protocolVersion");
    if (versionValue == nullptr || !versionValue->AsInt64(version) ||
        version != kProtocolVersion) {
        error = "l'agent parle la version " +
                (versionValue != nullptr ? versionValue->Dump()
                                         : std::string("inconnue")) +
                " du protocole ACP, CUTMACHINE la version " +
                std::to_string(kProtocolVersion);
        Reset();
        return false;
    }
    agent_title_ = StringAt(FindPath(agent, {"agentInfo", "title"}));
    if (agent_title_.empty()) {
        agent_title_ = StringAt(FindPath(agent, {"agentInfo", "name"}));
    }
    std::string authMethods;
    if (const Value* methods = agent.Find("authMethods");
        methods != nullptr && methods->IsArray()) {
        for (const Value& method : methods->AsArray()) {
            const std::string name = StringAt(method.Find("name"));
            if (!name.empty()) {
                authMethods += (authMethods.empty() ? "" : ", ") + name;
            }
        }
    }

    Value servers = Value::MakeArray();
    for (const AcpMcpServer& server : config_.mcp_servers) {
        servers.Push(McpServerJson(server));
    }
    Value newSession = Value::MakeObject();
    newSession.Set("cwd", Value::MakeString(config_.cwd));
    newSession.Set("mcpServers", std::move(servers));
    Value session;
    const CallOutcome outcome =
        Call("session/new", std::move(newSession), session, error);
    if (outcome != CallOutcome::Result) {
        if (stop_requested_) {
            error = kStopped;
        } else if (outcome == CallOutcome::RpcError &&
                   last_error_code_ == kAuthRequired) {
            error = "l'agent demande à être connecté à son compte" +
                    (authMethods.empty() ? std::string()
                                         : " (" + authMethods + ")") +
                    " : faites-le une fois dans un terminal, par exemple "
                    "`claude` puis /login ou `codex login`, puis renvoyez la "
                    "consigne";
        } else {
            error = "l'agent n'a pas ouvert de session : " + error;
        }
        Reset();
        return false;
    }
    const std::string sessionId = StringAt(session.Find("sessionId"));
    if (sessionId.empty()) {
        error = "l'agent a ouvert une session sans identifiant";
        Reset();
        return false;
    }
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    session_id_ = sessionId;
    session_prompted_ = false;
    return true;
}

bool AcpChatSession::SubmitUserMessage(const std::string& text,
                                       std::string& error) {
    chat::ChatTranscriptEntry userEntry;
    userEntry.kind = chat::ChatEntryKind::UserMessage;
    userEntry.text = text;
    Emit(std::move(userEntry));

    // A stop asked before this thread got here still counts; the turn
    // forgets it when it ends, however it ends.
    struct ClearStopOnExit {
        std::atomic_bool& stop;
        ~ClearStopOnExit() { stop = false; }
    } clearStop{stop_requested_};
    text_buffer_.clear();
    tools_.clear();
    last_plan_.clear();
    if (!EnsureSession(error)) {
        EmitError(error);
        return false;
    }

    Value prompt = Value::MakeArray();
    if (!session_prompted_ && !config_.preamble.empty()) {
        prompt.Push(TextBlock(config_.preamble));
    }
    prompt.Push(TextBlock(text));
    session_prompted_ = true;
    Value params = Value::MakeObject();
    params.Set("sessionId", Value::MakeString(session_id_));
    params.Set("prompt", std::move(prompt));

    // Under the lock RequestStop takes: a stop asked now either finds the
    // prompt not sent, and it is not sent, or finds it sent, and its cancel
    // follows it on the pipe. A cancel that arrived first would be lost.
    int64_t promptId = 0;
    bool stoppedBeforePrompt = false;
    {
        const std::lock_guard<std::mutex> lock(transport_mutex_);
        if (stop_requested_) {
            stoppedBeforePrompt = true;
        } else {
            prompt_in_flight_ = true;
            promptId = SendRequest("session/prompt", std::move(params), error);
        }
    }
    if (stoppedBeforePrompt) {
        error = kStopped;
        EmitError(error);
        return false;
    }
    Value result;
    const CallOutcome outcome = promptId == 0
                                    ? CallOutcome::WriteFailed
                                    : AwaitResponse(promptId, result, error);
    prompt_in_flight_ = false;
    FlushText();

    if (outcome == CallOutcome::Closed || outcome == CallOutcome::WriteFailed) {
        if (stop_requested_) {
            error = kStopped;
        }
        error += kNewConversation;
        Reset();
        EmitError(error);
        return false;
    }
    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. The answer may have been read
    // after the transport closed itself (an undeliverable session/cancel, a
    // broken pipe): the turn keeps it, but the agent is gone. The person is
    // told now that the next instruction opens a new conversation, instead
    // of that instruction failing on a dead pipe.
    const bool agentGone = transport_->Closed();
    if (outcome == CallOutcome::RpcError) {
        error = "l'agent n'a pas pu traiter la consigne : " + error;
        if (agentGone) {
            error += kNewConversation;
            Reset();
        }
        EmitError(error);
        return false;
    }

    ++usage_.turns;
    const Value* usage = result.Find("usage");
    if (usage != nullptr && usage->IsObject()) {
        usage_.input_tokens += IntAt(usage->Find("inputTokens"));
        usage_.output_tokens += IntAt(usage->Find("outputTokens"));
        usage_.thought_tokens += IntAt(usage->Find("thoughtTokens"));
        usage_.cache_read_tokens += IntAt(usage->Find("cachedReadTokens"));
        usage_.cache_write_tokens += IntAt(usage->Find("cachedWriteTokens"));
    } else {
        ++usage_.unreported_turns;
    }

    const std::string reason = StringAt(result.Find("stopReason"));
    if (reason == "end_turn" && !agentGone) {
        return true;
    }
    if (reason == "end_turn") {
        error = "l'agent s'est arrêté après sa réponse";
    } else {
        error = reason == "cancelled" ? std::string(kStopped)
                                      : StopReasonText(reason);
    }
    if (agentGone) {
        error += kNewConversation;
        Reset();
    }
    EmitError(error);
    return false;
}

void AcpChatSession::RequestStop() {
    const std::lock_guard<std::mutex> lock(transport_mutex_);
    stop_requested_ = true;
    if (!transport_) {
        return;
    }
    if (session_id_.empty()) {
        // Still starting (an agent fetched by npx can take a while): nothing
        // to cancel yet, so the agent itself goes.
        transport_->Close();
        transport_closed_ = true;
        return;
    }
    if (!prompt_in_flight_) {
        return;
    }
    Value params = Value::MakeObject();
    params.Set("sessionId", Value::MakeString(session_id_));
    Value notification = Value::MakeObject();
    notification.Set("jsonrpc", Value::MakeString("2.0"));
    notification.Set("method", Value::MakeString("session/cancel"));
    notification.Set("params", std::move(params));
    const std::string line = notification.Dump();
    if (trace_) {
        trace_(true, line);
    }
    std::string ignored;
    transport_->WriteCancellation(line, ignored);
}

void AcpChatSession::Shutdown() { Reset(); }

void AcpChatSession::Reset() {
    std::unique_ptr<AcpTransport> ending;
    {
        const std::lock_guard<std::mutex> lock(transport_mutex_);
        ending = std::move(transport_);
        session_id_.clear();
        transport_closed_ = false;
    }
    session_prompted_ = false;
    // The process may take a moment to go; not under the lock, so a stop
    // asked meanwhile finds no agent instead of waiting.
    ending.reset();
}

}  // namespace acp
