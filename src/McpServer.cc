#include "McpServer.h"

#include "BuildInfo.h"
#include "Json.h"
#include "LocalEnv.h"
#include "Procedures.h"

#include <sys/types.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace {

using mcp_json::Value;

std::string Esc(const std::string& text) {
    return mcp_json::EscapeJsonString(text);
}

std::string JsonRpcResult(const Value& id, const std::string& resultJson) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id.Dump() +
           ",\"result\":" + resultJson + "}";
}

std::string JsonRpcError(const Value& id, int code, const std::string& text) {
    return "{\"jsonrpc\":\"2.0\",\"id\":" + id.Dump() +
           ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" +
           Esc(text) + "\"}}";
}

// DELTA-2026-08 -- a delta nobody is told about is dead weight. This is the
// one place the protocol lets the server explain how to use its own results,
// and it is what stops a caller re-reading the whole timeline after every
// edit the way one did before the delta existed.
//
// PRD18 -- ROADMAP.md. Claude Code keeps the first 2,048 characters of these
// instructions and drops the rest, and says so in its MCP log ("Server
// instructions truncated from 3561 to 2048 chars"): the cut fell after the
// first line of the map below, so seven of its eight lines, and the note on
// the app, never arrived. Held under 2,048 (tests/mcp_tools_tests.cc), in
// this order of priority: describe once and keep the model by the deltas,
// what holds for every tool, the procedures, the whole map. What a tool's
// own description or an injected argument says is not repeated here: the
// map's notes are in each tool's description; the finer points of a delta
// (`created`, `count`, a rule emitted only when it explains its whole
// stretch, empty sections left out) and when to read again went to
// describe's; the reversal's delta to undo's and redo's; the app's relay to
// open_project's; async, expected_revision and idempotency_key to their own
// arguments, which every tool that takes them carries. What holds for every
// tool is said here instead of on each: an edit is one undo step, an ID
// takes a prefix or an alias, and an analysis measures without judging.
// The procedures are named for what they hold, not as an order: a model
// that edits something else need not call one. ASCII only, so that a
// character is a byte.
const char* kServerInstructionsHead =
    "Read the project once with `describe` and keep your own model: patch it "
    "with the `delta` each edit returns instead of reading again. A delta's "
    "`shifted` moves the clips of `track_id` at or after `from` (before "
    "`until` if given) by `by`, positions as before the edit; "
    "`sequence_changed`: reread the header.\n\n"
    "Each edit is one undo step unless its description says otherwise. An "
    "ID argument also takes a unique prefix or an alias from `describe`. "
    "Analyses measure, never judge: what to cut or keep is yours.\n\n"
    "`get_procedure` gives the procedure for editing an interview, motion "
    "design, a reel or an ad.\n\n";

// AX6 -- docs/roadmap/AX-2026-09.md. A client that loads tools on demand
// finds them by name; the names used to come from a skill only, and an agent
// without it had 163 to guess among. Every name below is checked against the
// catalog by tests/mcp_tools_tests.cc.
// PROFIL-2026-11 PF1 -- a server started with a profile names only the
// tools it serves, and drops a line it serves none of.
struct InstructionsMapLine {
    const char* task;
    std::vector<const char*> tools;
};

const std::vector<InstructionsMapLine>& InstructionsMap() {
    static const std::vector<InstructionsMapLine> map = {
        {"project",
         {"create_project", "open_project", "ingest_media", "describe",
          "list_timelines", "media_sheet"}},
        {"data", {"archive_project", "query_project_archive"}},
        {"speech",
         {"transcribe_media", "analyze_speech_onset", "align_words",
          "analyze_speakers", "list_speakers", "list_source_words",
          "list_speech_blocks", "get_timeline_transcript"}},
        {"word cuts",
         {"create_interview_short", "remove_words", "remove_source_ranges",
          "list_disfluencies", "clean_disfluencies", "tighten_pauses",
          "trim_boundary_air", "close_junction_air", "ripple_trim", "roll_edit",
          "list_timeline_words"}},
        {"as one step",
         {"prepare_edit_plan", "apply_edit_plan", "begin_intervention",
          "end_intervention"}},
        {"pictures",
         {"analyze_shot_quality", "list_stable_windows", "read_frame",
          "contact_sheet", "cut_sheet", "inspect_cut", "insert_clip",
          "paste_clips"}},
        {"colour",
         {"conform_color_management", "propose_grade", "measure_color",
          "match_color"}},
        {"captions",
         {"add_caption_style", "generate_captions",
          "check_caption_legibility"}},
        {"motion",
         {"list_motion_templates", "add_motion_template",
          "add_motion_composition", "edit_motion_composition",
          "render_motion_frame", "place_motion_clip", "save_motion_template"}},
        {"delivery",
         {"check_timeline", "review_cuts", "export_timeline", "verify_export",
          "export_resolve_xml"}},
        {"in the app", {"get_editor_view"}},
    };
    return map;
}

// The instructions `registry` gives at connection: the head, the profile it
// serves when it is not the whole catalog, then the map of what it serves.
std::string ServerInstructions(const McpToolRegistry& registry) {
    std::string text = kServerInstructionsHead;
    const McpToolProfile& profile = registry.Profile();
    if (!profile.everything) {
        text += "This server runs the `" + profile.name +
                "` profile: " + std::to_string(registry.Tools().size()) +
                " of " + std::to_string(registry.CatalogSize()) +
                " tools, for " + profile.summary +
                ". Any other tool is refused as ToolNotInProfile.\n\n";
    }
    text += "Tools by task:";
    for (const InstructionsMapLine& line : InstructionsMap()) {
        std::string names;
        for (const char* tool : line.tools) {
            if (!registry.Offers(tool)) continue;
            names += (names.empty() ? "" : ", ") + std::string(tool);
        }
        if (!names.empty())
            text += std::string("\n- ") + line.task + ": " + names;
    }
    return text;
}

// PRD19 -- ROADMAP.md. The version every surface reports, declared once in
// CMakeLists.txt; the server used to write 0.1.0 by hand.
// PRD18 -- the procedures are offered as prompts too.
std::string InitializeResultJson(const std::string& protocolVersion,
                                 const std::string& instructions) {
    return "{\"protocolVersion\":\"" + Esc(protocolVersion) +
           "\",\"capabilities\":{\"tools\":{},\"prompts\":{}},\"serverInfo\":"
           "{\"name\":\"cutmachine\",\"version\":\"" +
           Esc(CurrentBuild().version) + "\"},\"instructions\":\"" +
           Esc(instructions) + "\"}";
}

// PRD18 -- ROADMAP.md (ACP4). One prompt per procedure (Procedures.h), for a
// person who starts one by hand: a client offers prompts as commands (in
// Claude Code, /mcp__cutmachine__<name>) and no model reads one of its own
// accord, which is why get_procedure exists beside them. A procedure takes
// no argument: it is the whole prompt.
std::string PromptsListResultJson() {
    Value prompts = Value::MakeArray();
    for (const Procedure& procedure : ListProcedures()) {
        Value prompt = Value::MakeObject();
        prompt.Set("name", Value::MakeString(procedure.name));
        prompt.Set("description", Value::MakeString(procedure.description));
        prompts.Push(std::move(prompt));
    }
    Value result = Value::MakeObject();
    result.Set("prompts", std::move(prompts));
    return result.Dump();
}

// The procedure's SKILL.md as the user's message, after one sentence that
// says where the files it cites are read. False with `refusal` naming the
// procedures there are.
bool PromptGetResultJson(const std::string& name, std::string& resultJson,
                         std::string& refusal) {
    ProcedureText skill;
    std::string errorName;
    if (!ReadProcedureFile(name, kProcedureMainFile, skill, errorName,
                           refusal)) {
        return false;
    }
    Value content = Value::MakeObject();
    content.Set("type", Value::MakeString("text"));
    content.Set("text",
                Value::MakeString(
                    "Procédure « " + skill.name +
                    " » de CUTMACHINE : les fichiers qu'elle cite se lisent "
                    "par l'outil get_procedure (name « " +
                    skill.name +
                    " », file : le chemin du lien), dont chaque réponse "
                    "donne aussi les liens résolus (links).\n\n" +
                    skill.text));
    Value message = Value::MakeObject();
    message.Set("role", Value::MakeString("user"));
    message.Set("content", std::move(content));
    Value messages = Value::MakeArray();
    messages.Push(std::move(message));
    Value result = Value::MakeObject();
    result.Set("description", Value::MakeString(SkillFrontMatterField(
                                  skill.text, "description")));
    result.Set("messages", std::move(messages));
    resultJson = result.Dump();
    return true;
}

// PRD15 -- ROADMAP.md. The listing `--tools` prints too (McpTools.h), with
// each tool's annotations.
std::string ToolsListResultJson(const McpToolRegistry& registry) {
    return "{\"tools\":" + McpToolsListingJson(registry.Tools()) + "}";
}

// PRD13b -- ROADMAP.md. What one JSON-RPC message is, read on its shape
// before anything runs. A request has an id and is answered, with a result
// or an error -- and so is anything that is not an object, as "Invalid
// Request". A notification has no id and never is. A response has no method
// and a result or an error: a client answering a request, which this server
// never sends. The server and a relay in front of it (McpJsonRpcGetsAnswer)
// read a message the same way: a relay that waited for an answer the server
// never writes would wait forever.
enum class MessageKind { kRequest, kNotification, kClientResponse };

MessageKind KindOf(const Value& message) {
    if (!message.IsObject()) {
        return MessageKind::kRequest;
    }
    if (message.Find("method") == nullptr &&
        (message.Find("result") != nullptr ||
         message.Find("error") != nullptr)) {
        return MessageKind::kClientResponse;
    }
    return message.Find("id") == nullptr ? MessageKind::kNotification
                                         : MessageKind::kRequest;
}

// What a client that posted a JSON-RPC response is told: an error without an
// id, as the transports page allows in the body of the 400.
std::string ClientResponseRefusal() {
    return JsonRpcError(Value::MakeNull(), -32600,
                        "Invalid Request: this server sends no requests, so "
                        "it has no response to accept; post requests and "
                        "notifications");
}

std::string ToolCallResultJson(const McpToolCallOutcome& outcome) {
    std::string content =
        "{\"type\":\"text\",\"text\":\"" + Esc(outcome.result_json) + "\"}";
    // A tool that returned a picture contributes a second content block, in
    // the shape the MCP specification gives image results. The text block
    // stays first so a client that ignores images still gets the answer.
    if (!outcome.image_base64.empty() && !outcome.image_mime.empty()) {
        content += ",{\"type\":\"image\",\"data\":\"" +
                   Esc(outcome.image_base64) + "\",\"mimeType\":\"" +
                   Esc(outcome.image_mime) + "\"}";
    }
    return "{\"content\":[" + content +
           "],\"isError\":" + (outcome.ok ? "false" : "true") + "}";
}

}  // namespace

const std::vector<std::string>& McpProtocolVersions() {
    static const std::vector<std::string> kVersions = {
        "2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25"};
    return kVersions;
}

std::string NegotiateMcpProtocolVersion(const std::string& requested) {
    const std::vector<std::string>& versions = McpProtocolVersions();
    return std::find(versions.begin(), versions.end(), requested) !=
                   versions.end()
               ? requested
               : versions.back();
}

bool McpJsonRpcGetsAnswer(const std::string& message) {
    Value parsed;
    std::string parseError;
    // Text that is not JSON is answered with a parse error.
    if (!Value::Parse(message, parsed, parseError)) {
        return true;
    }
    if (!parsed.IsArray()) {
        return KindOf(parsed) == MessageKind::kRequest;
    }
    // An empty batch is answered with an error.
    if (parsed.AsArray().empty()) {
        return true;
    }
    for (const Value& element : parsed.AsArray()) {
        if (KindOf(element) == MessageKind::kRequest) {
            return true;
        }
    }
    return false;
}

bool ParseMcpPort(const std::string& text, int& port, std::string& error) {
    const bool digits =
        !text.empty() && text.size() <= 5 &&
        text.find_first_not_of("0123456789") == std::string::npos;
    int value = 0;
    for (const char digit : digits ? text : std::string()) {
        value = value * 10 + (digit - '0');
    }
    if (!digits || value > 65535) {
        error =
            "--mcp-serve option '--port' takes a number from 1 to 65535, "
            "or 0 for a free port the system picks, not '" +
            text + "'";
        return false;
    }
    port = value;
    return true;
}

bool ResolveMcpHttpToken(const std::string* option, McpHttpToken& resolved,
                         std::string& error) {
    static const char kVariable[] = "CUTMACHINE_MCP_TOKEN";
    resolved = McpHttpToken{};
    std::string token;
    std::string source;
    std::string where;
    if (option != nullptr) {
        token = *option;
        source = "--token";
    } else {
        local_env::LoadLocalEnvFileIfPresent();
        const char* value = std::getenv(kVariable);
        if (value == nullptr) {
            return true;
        }
        token = value;
        source = kVariable;
        where = " (set in the environment or in " +
                local_env::LocalEnvFilePath() + ")";
    }
    if (token.empty()) {
        error = source + " is empty" + where +
                ": give it a value, or remove it to ask for no token";
        return false;
    }
    if (!IsUsableBearerToken(token)) {
        error = source + where +
                " holds a blank, a control character or a character that is "
                "not ASCII: a client could not send it in a header. Use "
                "visible ASCII characters only";
        return false;
    }
    resolved.token = std::move(token);
    resolved.source = std::move(source);
    return true;
}

McpServer::McpServer(McpBackend& backend, const McpToolProfile& profile)
    : backend_(backend), tools_(profile) {}

McpToolClass McpServer::ToolClassOf(const std::string& tool) const {
    for (const McpTool& entry : tools_.Tools()) {
        if (entry.name == tool) {
            return entry.tool_class;
        }
    }
    return McpToolClass::Unclassified;
}

bool McpServer::Start(int port, std::string& error, const std::string& path) {
    path_ = path;
    // PRD13b -- a request that names a revision this server does not
    // implement is refused before it reaches the handler.
    http_.SetProtocolVersions(McpProtocolVersions());
    return http_.Start(
        port,
        [this](const std::string& method, const std::string& requestPath,
               const std::string& body, int& statusCode,
               std::string& contentType, std::string& responseBody) {
            contentType = "application/json";
            if (requestPath != path_) {
                statusCode = 404;
                responseBody = "{\"error\":\"not found\"}";
                return;
            }
            if (method != "POST") {
                statusCode = 405;
                responseBody = "{\"error\":\"method not allowed, use POST\"}";
                return;
            }
            std::string jsonRpcResponse;
            switch (HandleJsonRpcMessage(body, jsonRpcResponse)) {
                case McpReply::kAnswer:
                    statusCode = 200;
                    responseBody = std::move(jsonRpcResponse);
                    break;
                case McpReply::kNone:
                    // Notifications: acknowledged, no body.
                    statusCode = 202;
                    responseBody.clear();
                    break;
                case McpReply::kClientResponse:
                    // PRD13b -- input the server cannot accept gets an HTTP
                    // error, as the transports page asks.
                    statusCode = 400;
                    responseBody = std::move(jsonRpcResponse);
                    break;
            }
        },
        error);
}

void McpServer::Stop() { http_.Stop(); }

int McpServer::ServeStdio(std::FILE* input, std::FILE* output) {
    // getline rather than a fixed buffer: a tools/call carrying a whole
    // subtitle file or a long text is one line, however long.
    char* line = nullptr;
    size_t capacity = 0;
    ssize_t length = 0;
    int status = 0;
    while ((length = getline(&line, &capacity, input)) != -1) {
        std::string request(line, static_cast<size_t>(length));
        while (!request.empty() &&
               (request.back() == '\n' || request.back() == '\r'))
            request.pop_back();
        if (request.find_first_not_of(" \t") == std::string::npos) continue;
        std::string response;
        if (!HandleJsonRpc(request, response)) continue;
        response.push_back('\n');
        if (std::fwrite(response.data(), 1, response.size(), output) !=
                response.size() ||
            std::fflush(output) != 0) {
            status = 1;
            break;
        }
    }
    std::free(line);
    if (status == 0 && std::ferror(input) != 0) status = 1;
    return status;
}

bool McpServer::HandleJsonRpc(const std::string& requestBody,
                              std::string& responseBody) {
    return HandleJsonRpcMessage(requestBody, responseBody) == McpReply::kAnswer;
}

McpReply McpServer::HandleJsonRpcMessage(const std::string& requestBody,
                                         std::string& responseBody) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(requestBody, parsed, parseError)) {
        responseBody = JsonRpcError(Value::MakeNull(), -32700,
                                    "Parse error: " + parseError);
        return McpReply::kAnswer;
    }
    if (!parsed.IsArray()) {
        switch (KindOf(parsed)) {
            case MessageKind::kRequest:
                responseBody = HandleRequest(parsed, false);
                return McpReply::kAnswer;
            case MessageKind::kNotification:
                // No server-initiated behavior depends on any notification
                // MCP clients send today (e.g. notifications/initialized);
                // acknowledge and move on.
                responseBody.clear();
                return McpReply::kNone;
            case MessageKind::kClientResponse:
                responseBody = ClientResponseRefusal();
                return McpReply::kClientResponse;
        }
    }
    // PRD13b -- ROADMAP.md. A batch, which 2025-03-26 requires a server to
    // receive: each message handled in turn as if it came alone, the answers
    // in one array, in the order of the requests. Accepted whatever the
    // version negotiated: refusing it under the later revisions, which only
    // stopped asking for it, would take a state per connection this server
    // does not keep.
    if (parsed.AsArray().empty()) {
        responseBody = JsonRpcError(Value::MakeNull(), -32600,
                                    "Invalid Request: an empty batch");
        return McpReply::kAnswer;
    }
    std::string answers;
    bool clientResponse = false;
    for (const Value& message : parsed.AsArray()) {
        switch (KindOf(message)) {
            case MessageKind::kRequest:
                answers +=
                    (answers.empty() ? "" : ",") + HandleRequest(message, true);
                break;
            case MessageKind::kNotification:
                break;
            case MessageKind::kClientResponse:
                clientResponse = true;
                break;
        }
    }
    if (!answers.empty()) {
        responseBody = "[" + answers + "]";
        return McpReply::kAnswer;
    }
    if (clientResponse) {
        responseBody = ClientResponseRefusal();
        return McpReply::kClientResponse;
    }
    responseBody.clear();
    return McpReply::kNone;
}

std::string McpServer::HandleRequest(const Value& request, bool inBatch) {
    if (!request.IsObject()) {
        return JsonRpcError(Value::MakeNull(), -32600,
                            "Invalid Request: expected a JSON object");
    }
    const Value* idField = request.Find("id");
    const Value id = idField ? *idField : Value::MakeNull();

    const Value* jsonrpcField = request.Find("jsonrpc");
    const Value* methodField = request.Find("method");
    if (!jsonrpcField || !jsonrpcField->IsString() ||
        jsonrpcField->AsString() != "2.0" || !methodField ||
        !methodField->IsString()) {
        return JsonRpcError(
            id, -32600,
            "Invalid Request: 'jsonrpc' must be \"2.0\" and 'method' a string");
    }

    const std::string method = methodField->AsString();
    static const Value kEmptyObject = Value::MakeObject();
    const Value* paramsField = request.Find("params");
    const Value& params = paramsField ? *paramsField : kEmptyObject;

    if (method == "initialize") {
        // PRD13b -- the protocol's lifecycle forbids it (2025-03-26): nothing
        // can be asked of a server before it is initialised, so the request
        // that does it comes alone.
        if (inBatch) {
            return JsonRpcError(id, -32600,
                                "Invalid Request: initialize must not be part "
                                "of a batch; send it alone, first");
        }
        // PRD13 -- the version asked for when this server knows it, its
        // latest otherwise (McpServer.h): a request without a version, or
        // with one that is not a string, is one it does not know.
        const Value* requested = params.Find("protocolVersion");
        const std::string protocolVersion = NegotiateMcpProtocolVersion(
            requested != nullptr && requested->IsString()
                ? requested->AsString()
                : std::string());
        return JsonRpcResult(
            id,
            InitializeResultJson(protocolVersion, ServerInstructions(tools_)));
    }
    if (method == "tools/list") {
        return JsonRpcResult(id, ToolsListResultJson(tools_));
    }
    // PRD18 -- ROADMAP.md (ACP4). An unknown prompt is a bad parameter,
    // as the protocol's prompts page has it.
    if (method == "prompts/list") {
        return JsonRpcResult(id, PromptsListResultJson());
    }
    if (method == "prompts/get") {
        const Value* nameField =
            params.IsObject() ? params.Find("name") : nullptr;
        if (nameField == nullptr || !nameField->IsString()) {
            return JsonRpcError(id, -32602,
                                "Invalid params: 'name' is required");
        }
        std::string result;
        std::string refusal;
        if (!PromptGetResultJson(nameField->AsString(), result, refusal)) {
            return JsonRpcError(id, -32602, "Invalid params: " + refusal);
        }
        return JsonRpcResult(id, result);
    }
    // A client checks a spawned server is alive with `ping`; the answer is
    // an empty result, per the protocol.
    if (method == "ping") {
        return JsonRpcResult(id, "{}");
    }
    if (method == "tools/call") {
        if (!params.IsObject()) {
            return JsonRpcError(id, -32602,
                                "Invalid params: expected an object");
        }
        const Value* nameField = params.Find("name");
        if (!nameField || !nameField->IsString()) {
            return JsonRpcError(id, -32602,
                                "Invalid params: 'name' is required");
        }
        const Value* argumentsField = params.Find("arguments");
        const Value arguments =
            argumentsField ? *argumentsField : Value::MakeObject();
        const McpToolCallOutcome outcome =
            session_.Call(tools_, backend_, nameField->AsString(), arguments);
        // PRD25 -- the endpoint's window reads the call here (McpServer.h).
        if (tool_call_observer_) {
            tool_call_observer_(nameField->AsString(),
                                ToolClassOf(nameField->AsString()), arguments,
                                outcome);
        }
        return JsonRpcResult(id, ToolCallResultJson(outcome));
    }

    return JsonRpcError(id, -32601, "Method not found: " + method);
}
