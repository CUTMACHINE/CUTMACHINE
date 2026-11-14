// Pure-C++ test for the chat panel's tool-calling loop (ROADMAP.md F2.4).
// Depends only on Document/EditLog/Operations/Json/IdResolver/McpTools/
// ChatLlmClient/ChatSession -- no ProjectStorage, no AppKit -- so it builds
// and runs on a plain Linux host, the same way tests/mcp_tools_tests.cc
// does for the MCP server itself.
//
// The whole point of this file: prove that a tool call the (fake) LLM
// requests is dispatched through the *real* McpToolRegistry::Call -- the
// exact function McpServer.cc's `tools/call` JSON-RPC handler calls -- and
// so ends up applied via EditLog::Apply against a real in-memory Document,
// not some parallel chat-only mutation path. The in-memory backend below is
// structurally identical to tests/mcp_tools_tests.cc's InMemoryBackend for
// exactly that reason: it is the same kind of backend, just reused here to
// drive ChatSession instead of the JSON-RPC transport.

#include "ChatLlmClient.h"
#include "ChatSession.h"
#include "Document.h"
#include "EditLog.h"
#include "IdResolver.h"
#include "Json.h"
#include "McpBackend.h"
#include "McpLiveBackend.h"
#include "McpTools.h"
#include "Operations.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

namespace {

using chat::ChatEntryKind;
using chat::ChatHttpRequest;
using chat::ChatHttpResponse;
using chat::ChatLlmClient;
using chat::ChatLlmConfig;
using chat::ChatSession;
using chat::ChatTranscriptEntry;
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

Document Fixture() {
    Document document;
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

// Same shape as tests/mcp_tools_tests.cc's InMemoryBackend: ApplyOperation/
// Undo/Redo call EditLog::Apply/Undo/Redo directly against a Document held
// in memory -- the same function ApplyOperationCommand's `--apply-op` path
// and McpLiveBackend (the app's real backend, McpLiveBackend.h) both call.
class InMemoryBackend : public McpBackend {
public:
    explicit InMemoryBackend(Document document, std::string describeJson = {})
        : document_(std::move(document)),
          describe_json_(std::move(describeJson)) {}

    bool SnapshotDocument(Document& document, std::string&) override {
        document = document_;
        return true;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Apply(document_, std::move(operation), error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Undo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Undo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Redo(std::string& resultJson, std::string& errorName,
              std::string&) override {
        EditError error = EditError::None;
        std::string message;
        if (!log_.Redo(document_, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
        resultJson = "{\"ok\":true}";
        return true;
    }

    bool Describe(std::string& json, std::string&) override {
        if (!describe_json_.empty()) {
            json = describe_json_;
            return true;
        }
        json = "{\"clip_count\":" +
               std::to_string(document_.sequence.tracks.at(0).clips.size()) +
               "}";
        return true;
    }

    const Document& CurrentDocument() const { return document_; }

private:
    Document document_;
    EditLog log_;
    std::string describe_json_;
};

// A scripted transport: each call to SendMessages consumes the next
// pre-canned response body in `responses`. Lets a test drive a multi-turn
// tool-calling exchange (propose a tool call, see the result, respond)
// deterministically, without a real model or network.
class ScriptedTransport {
public:
    explicit ScriptedTransport(std::vector<std::string> responses)
        : responses_(std::move(responses)) {}

    bool operator()(const ChatHttpRequest& request, ChatHttpResponse& response,
                    std::string& error) {
        if (call_count_ >= responses_.size()) {
            error = "scripted transport ran out of responses";
            return false;
        }
        response.status_code = 200;
        response.body = responses_[call_count_];
        requests_.push_back(request);
        ++call_count_;
        return true;
    }

    size_t CallCount() const { return call_count_; }
    const ChatHttpRequest& Request(size_t index) const {
        return requests_.at(index);
    }

private:
    std::vector<std::string> responses_;
    std::vector<ChatHttpRequest> requests_;
    size_t call_count_ = 0;
};

ChatLlmConfig TestConfig() {
    ChatLlmConfig config;
    config.api_key = "sk-test-key";
    return config;
}

ChatLlmConfig OllamaConfig() {
    ChatLlmConfig config;
    config.provider = chat::ChatLlmProvider::Ollama;
    config.model = "qwen2.5-coder:7b";
    config.base_url = "http://localhost:11434";
    return config;
}

}  // namespace

int main() {
    Test(
        "a single tool_use turn dispatches through McpToolRegistry and "
        "mutates the real Document via EditLog::Apply",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"content":[)"
                R"({"type":"text","text":"Je raccourcis le plan."},)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K30000000000000000000003",)"
                R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})",
                R"({"content":[{"type":"text","text":"C'est fait."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage(
                "Raccourcis le premier plan d'une image.", error);
            Check(ok, "the turn succeeds: " + error);
            Check(transport.CallCount() == 2,
                  "two LLM round trips: propose the tool, then confirm");

            const Document& document = backend.CurrentDocument();
            Check(
                document.sequence.tracks.at(0).clips.at(0).duration.value == 9,
                "the clip was actually trimmed (duration 10 -> 9 frames) "
                "through EditLog::Apply, not a parallel mutation path");

            const std::vector<ChatTranscriptEntry>& transcript =
                session.Transcript();
            bool sawToolCall = false;
            bool sawSuccessfulResult = false;
            for (const ChatTranscriptEntry& entry : transcript) {
                if (entry.kind == ChatEntryKind::ToolCall &&
                    entry.tool_name == "trim_clip")
                    sawToolCall = true;
                if (entry.kind == ChatEntryKind::ToolResult &&
                    entry.tool_name == "trim_clip" && entry.tool_ok)
                    sawSuccessfulResult = true;
            }
            Check(sawToolCall,
                  "the transcript records the trim_clip tool call");
            Check(sawSuccessfulResult,
                  "the transcript records a successful trim_clip result");
        });

    // PL1 -- ROADMAP.md. The real panel backend, minus the hop to the main
    // thread (ChatPanelView.mm's MainThreadBackend only forwards).
    Test("a chat turn is one undo step, labelled by its instruction", [] {
        Document document = Fixture();
        EditLog log;
        int persisted = 0;
        McpLiveBackend live(document, log, [&persisted] { ++persisted; });
        McpToolRegistry registry;
        const std::string original = document.SaveToString();
        ScriptedTransport transport({
            R"({"content":[)"
            R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
            R"("input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}},)"
            R"({"type":"tool_use","id":"toolu_2","name":"trim_clip",)"
            R"("input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
            R"(],"stop_reason":"tool_use"})",
            R"({"content":[{"type":"text","text":"C'est fait."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(live, registry, llm);
        std::string error;
        Check(session.SubmitUserMessage(
                  "Raccourcis le premier plan\nde deux images.", error),
              "the turn succeeds: " + error);
        const std::string edited = document.SaveToString();
        Check(document.sequence.tracks.at(0).clips.at(0).duration.value == 8,
              "both trims were applied");
        Check(log.AppliedCount() == 1,
              "two tool calls leave one history entry, not " +
                  std::to_string(log.AppliedCount()));
        const auto* batch =
            log.AppliedCount() == 1
                ? std::get_if<BatchOperation>(&log.AppliedEntries().back().op)
                : nullptr;
        Check(batch && batch->operations.size() == 2 &&
                  batch->label ==
                      "Agent : Raccourcis le premier plan de deux images.",
              "the step is labelled by the instruction");
        Check(persisted == 3, "each edit and the fold were persisted");
        EditError editError = EditError::None;
        std::string message;
        Check(log.Undo(document, editError, message) &&
                  document.SaveToString() == original,
              "one Undo reverts the whole turn");
        Check(log.Redo(document, editError, message) &&
                  document.SaveToString() == edited,
              "one Redo replays it");
    });

    // AX1 -- docs/roadmap/AX-2026-09.md. An agent relayed to the app reads
    // what an undo or a redo put back from its delta, as over the server.
    Test("the live backend's undo and redo answer with their delta", [] {
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        McpToolRegistry registry;
        mcp_json::Value trim;
        std::string parseError;
        mcp_json::Value::Parse(
            R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
            R"("delta":{"value":-2,"rate":25}})",
            trim, parseError);
        Check(registry.Call(live, "trim_clip", trim).ok, "the trim applies");
        const McpToolCallOutcome undone =
            registry.Call(live, "undo", mcp_json::Value::MakeObject());
        Check(undone.ok &&
                  undone.result_json.find("\"delta\"") != std::string::npos &&
                  undone.result_json.find("01K30000000000000000000003") !=
                      std::string::npos,
              "undo names the clip it put back: " + undone.result_json);
        const McpToolCallOutcome redone =
            registry.Call(live, "redo", mcp_json::Value::MakeObject());
        Check(redone.ok &&
                  redone.result_json.find("\"delta\"") != std::string::npos,
              "redo too: " + redone.result_json);
    });

    // SAVING_ROADMAP.md, a write refused under the editor (S3) -- 24 October
    // 2026. The window commits each edit of this backend before taking it
    // (main.mm, -persistStagedDocument:). When its storage refuses the
    // write, the caller is refused by name and the window's document and
    // journal stay byte for byte what they were. They used to be edited in
    // place and undone by the window, and the caller answered `ok` with
    // the delta of an edit nobody held any more.
    Test("a write the window refuses is refused by name, nothing moves", [] {
        Document document = Fixture();
        EditLog log;
        int refreshed = 0;
        McpLiveBackend live(document, log, [&refreshed] { ++refreshed; });
        bool refuse = false;
        std::string committedDocument = document.SaveToString();
        std::string committedLog = log.Serialize();
        live.SetEditCommitCallback([&](const Document& staged,
                                       const EditLog& stagedLog,
                                       std::string& message) {
            if (refuse) {
                message = "no space left on device (simulated)";
                return false;
            }
            committedDocument = staged.SaveToString();
            committedLog = stagedLog.Serialize();
            return true;
        });
        McpToolRegistry registry;
        mcp_json::Value trim;
        std::string parseError;
        mcp_json::Value::Parse(
            R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
            R"("delta":{"value":-1,"rate":25}})",
            trim, parseError);
        const mcp_json::Value none = mcp_json::Value::MakeObject();
        const auto refusedByName = [&](const McpToolCallOutcome& outcome,
                                       const std::string& what) {
            Check(!outcome.ok &&
                      outcome.result_json.find("\"IoError\"") !=
                          std::string::npos &&
                      outcome.result_json.find(
                          "no space left on device (simulated)") !=
                          std::string::npos,
                  what + " is refused as IoError, with the reason: " +
                      outcome.result_json);
        };
        const auto unmoved = [&](const std::string& what) {
            Check(document.SaveToString() == committedDocument &&
                      log.Serialize() == committedLog,
                  what +
                      " leaves the window's document and journal as "
                      "the package holds them");
        };

        Check(registry.Call(live, "trim_clip", trim).ok,
              "a first trim is committed");
        unmoved("a committed trim");
        const int refreshedBefore = refreshed;

        refuse = true;
        refusedByName(registry.Call(live, "trim_clip", trim), "a trim");
        unmoved("a refused trim");
        refusedByName(registry.Call(live, "undo", none), "an undo");
        unmoved("a refused undo");
        Check(refreshed == refreshedBefore,
              "nothing is refreshed after a refused write");

        refuse = false;
        Check(registry.Call(live, "undo", none).ok,
              "once writes are accepted, the undo goes through");
        unmoved("a committed undo");
        refuse = true;
        refusedByName(registry.Call(live, "redo", none), "a redo");
        unmoved("a refused redo");
        Check(log.UndoneCount() == 1, "the undone entry is still there");

        refuse = false;
        Check(registry.Call(live, "redo", none).ok, "the redo goes through");
        const McpToolCallOutcome begun =
            registry.Call(live, "begin_intervention", none);
        mcp_json::Value mark;
        Check(begun.ok &&
                  mcp_json::Value::Parse(begun.result_json, mark, parseError),
              "an intervention begins");
        Check(registry.Call(live, "trim_clip", trim).ok &&
                  registry.Call(live, "trim_clip", trim).ok,
              "two trims inside it are committed");
        mcp_json::Value end = mcp_json::Value::MakeObject();
        end.Set("intervention", mcp_json::Value::MakeString(
                                    mark.Find("intervention") != nullptr
                                        ? mark.Find("intervention")->AsString()
                                        : std::string()));
        end.Set("label", mcp_json::Value::MakeString("Agent : deux images"));
        refuse = true;
        refusedByName(registry.Call(live, "end_intervention", end),
                      "the fold of an intervention");
        unmoved("a refused fold");
        Check(log.AppliedCount() == 3, "the two trims stay two entries, not " +
                                           std::to_string(log.AppliedCount()));
        refuse = false;
        Check(registry.Call(live, "end_intervention", end).ok &&
                  log.AppliedCount() == 2,
              "once writes are accepted, the fold goes through");
        unmoved("a committed fold");
    });

    // F2.4 -- ROADMAP.md. The panel works on its window's project for the
    // window's whole life. Asked for another, its agent used to read "this
    // backend cannot create a project" and had nothing to tell the person;
    // the refusal now names the project held and the way to another, and
    // the model reads it in the next request.
    Test("asked for a new project, the panel's agent hears why and how", [] {
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        const std::string held =
            "/Volumes/LaCie/Film.cutmachine-project/project.cutmachine.json";
        live.SetProjectPath(held);
        McpToolRegistry registry;
        const std::string original = document.SaveToString();
        ScriptedTransport transport({
            R"({"content":[)"
            R"({"type":"tool_use","id":"toolu_1","name":"create_project",)"
            R"("input":{"path":"/tmp/Autre.cutmachine-project",)"
            R"("name":"Autre"}}],"stop_reason":"tool_use"})",
            R"({"content":[{"type":"text","text":"Impossible ici."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(live, registry, llm);
        std::string error;
        Check(session.SubmitUserMessage("Crée un nouveau projet.", error),
              "the turn ends normally: " + error);
        const ChatTranscriptEntry* refusal = nullptr;
        for (const ChatTranscriptEntry& entry : session.Transcript()) {
            if (entry.kind == ChatEntryKind::ToolResult) {
                refusal = &entry;
            }
        }
        Check(refusal != nullptr && !refusal->tool_ok &&
                  refusal->text.find("UnsupportedOperation") !=
                      std::string::npos &&
                  refusal->text.find(held) != std::string::npos &&
                  refusal->text.find("Nouveau projet") != std::string::npos &&
                  refusal->text.find("ingest_media") != std::string::npos,
              "the panel shows the refusal, the project held and the way "
              "to another: " +
                  (refusal != nullptr ? refusal->text : std::string()));
        Check(transport.CallCount() == 2 &&
                  transport.Request(1).body.find("Nouveau projet") !=
                      std::string::npos,
              "the model reads the refusal before it answers");
        Check(document.SaveToString() == original && log.AppliedCount() == 0,
              "the window's document is left as it was");
    });

    Test("a stopped turn answers every tool it did not run", [] {
        // Visual QA of 19 September 2026 -- the panel's Stop button.
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        McpToolRegistry registry;
        ScriptedTransport script({
            R"({"content":[{"type":"tool_use","id":"toolu_1","name":)"
            R"("trim_clip","input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}},)"
            R"({"type":"tool_use","id":"toolu_2","name":"trim_clip",)"
            R"("input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
            R"(],"stop_reason":"tool_use"})",
            R"({"content":[{"type":"text","text":"Reprise."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatSession* stopping = nullptr;
        ChatLlmClient llm(
            TestConfig(), [&](const ChatHttpRequest& request,
                              ChatHttpResponse& response, std::string& error) {
                const bool ok = script(request, response, error);
                // The person presses Stop while the model's
                // first answer is on its way.
                if (stopping != nullptr && script.CallCount() == 1)
                    stopping->RequestStop();
                return ok;
            });
        ChatSession session(live, registry, llm);
        stopping = &session;
        const std::string original = document.SaveToString();
        std::string error;
        Check(!session.SubmitUserMessage("Raccourcis deux fois.", error),
              "a stopped turn reports it stopped");
        Check(document.SaveToString() == original,
              "no tool runs once the turn is stopped");
        Check(!session.Transcript().empty() &&
                  session.Transcript().back().kind == ChatEntryKind::Error,
              "the turn ends on an Error entry");
        Check(session.SubmitUserMessage("Continue.", error),
              "the next turn runs normally: " + error);
        const std::string& body = script.Request(1).body;
        Check(body.find("toolu_1") != std::string::npos &&
                  body.find("toolu_2") != std::string::npos &&
                  body.find("stopped by the user") != std::string::npos,
              "each tool the model asked for got a stopped result, so the "
              "conversation stays well formed");
    });

    // GENERATIF-2026-10 GN2 -- this loop asks the person nothing before a
    // tool runs, so a tool that spends their money is not run from it.
    Test("the panel's own loop does not run a paid generation", [] {
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        McpToolRegistry registry;
        ScriptedTransport script({
            R"({"content":[{"type":"tool_use","id":"toolu_1","name":)"
            R"("generate_media","input":{"proposal_id":"P1",)"
            R"("confirmed_by_person":true}}],"stop_reason":"tool_use"})",
            R"({"content":[{"type":"text","text":"Compris."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(),
                          [&](const ChatHttpRequest& request,
                              ChatHttpResponse& response, std::string& error) {
                              return script(request, response, error);
                          });
        ChatSession session(live, registry, llm);
        std::string error;
        Check(session.SubmitUserMessage("Étends-la en 9:16.", error),
              "the turn ends: " + error);
        bool refused = false;
        for (const ChatTranscriptEntry& entry : session.Transcript()) {
            if (entry.kind == ChatEntryKind::ToolResult &&
                entry.tool_name == "generate_media")
                refused =
                    !entry.tool_ok &&
                    entry.text.find("ConfirmationRequired") !=
                        std::string::npos &&
                    entry.text.find("nothing was spent") != std::string::npos;
        }
        Check(refused, "refused, nothing spent, and the model is told why");
    });

    Test("a stop asked before the turn begins is kept", [] {
        // ACP-2026-09 -- Stop pressed right after Send, before the worker
        // thread reaches the turn: the turn used to clear the request as it
        // began, and ran to its end.
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        McpToolRegistry registry;
        ScriptedTransport script({
            R"({"content":[{"type":"text","text":"Reprise."}],)"
            R"("stop_reason":"end_turn"})",
            R"({"content":[{"type":"text","text":"Encore."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(script));
        ChatSession session(live, registry, llm);
        session.RequestStop();
        std::string error;
        Check(!session.SubmitUserMessage("Raccourcis.", error) &&
                  error == "tour arrêté à la demande de l'utilisateur",
              "the turn stops at once: " + error);
        Check(script.CallCount() == 0, "without calling the model");
        Check(session.SubmitUserMessage("Continue.", error),
              "the turn forgot the request when it ended: " + error);
        session.RequestStop();
        session.ClearStopRequest();
        Check(session.SubmitUserMessage("Encore.", error),
              "a request cleared before the turn does not stop it: " + error);
    });

    Test("a hand edit during a chat turn keeps every step apart", [] {
        Document document = Fixture();
        EditLog log;
        McpLiveBackend live(document, log);
        McpToolRegistry registry;
        ScriptedTransport transport({
            R"({"content":[{"type":"tool_use","id":"toolu_1","name":)"
            R"("trim_clip","input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}}],)"
            R"("stop_reason":"tool_use"})",
            R"({"content":[{"type":"tool_use","id":"toolu_2","name":)"
            R"("trim_clip","input":{"clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","delta":{"value":-1,"rate":25}}}],)"
            R"("stop_reason":"tool_use"})",
            R"({"content":[{"type":"text","text":"C'est fait."}],)"
            R"("stop_reason":"end_turn"})",
        });
        // While the agent waits for its second answer, the person trims the
        // same clip's head with the mouse: an entry the agent did not make.
        ChatLlmClient llm(
            TestConfig(), [&](const ChatHttpRequest& request,
                              ChatHttpResponse& response, std::string& error) {
                if (transport.CallCount() == 1) {
                    EditError editError = EditError::None;
                    std::string message;
                    log.Apply(document,
                              TrimClipOperation{"01K30000000000000000000003",
                                                TrimEdge::Head,
                                                {1, 25},
                                                std::nullopt},
                              editError, message);
                }
                return transport(request, response, error);
            });
        ChatSession session(live, registry, llm);
        std::string error;
        Check(session.SubmitUserMessage("Raccourcis le premier plan.", error),
              "the turn succeeds: " + error);
        Check(
            log.AppliedCount() == 3 &&
                std::none_of(
                    log.AppliedEntries().begin(), log.AppliedEntries().end(),
                    [](const Entry& entry) {
                        return std::holds_alternative<BatchOperation>(entry.op);
                    }),
            "the person's edit is not folded into the agent's step");
    });

    Test(
        "a text-only reply with no tool_use ends the turn after one round trip",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"content":[{"type":"text","text":"Je ne peux pas faire cela."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage(
                "Fais quelque chose d'impossible.", error);
            Check(ok, "the turn succeeds: " + error);
            Check(transport.CallCount() == 1, "exactly one LLM round trip");
            Check(backend.CurrentDocument()
                          .sequence.tracks.at(0)
                          .clips.at(0)
                          .duration.value == 10,
                  "the document is untouched when no tool is called");
        });

    Test(
        "a failed tool call is reported in the transcript, fed back as an "
        "error tool_result, and does not mutate the document",
        [] {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            // Same rejection McpTools.cc's ID resolver reports to any MCP
            // client for an unresolvable clip_id -- see IdResolver::Resolve.
            // Caught before an Operation is even constructed, so this also
            // proves a bad chat-driven call never reaches EditLog::Apply.
            ScriptedTransport transport({
                R"({"content":[)"
                R"({"type":"tool_use","id":"toolu_1","name":"trim_clip",)"
                R"("input":{"clip_id":"01K39999999999999999999999",)"
                R"("edge":"Tail","delta":{"value":-1,"rate":25}}})"
                R"(],"stop_reason":"tool_use"})",
                R"({"content":[{"type":"text","text":"Ce clip n'existe pas."}],)"
                R"("stop_reason":"end_turn"})",
            });
            ChatLlmClient llm(TestConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok =
                session.SubmitUserMessage("Raccourcis un clip inconnu.", error);
            Check(ok, "the turn still succeeds overall: " + error);
            Check(backend.CurrentDocument()
                          .sequence.tracks.at(0)
                          .clips.at(0)
                          .duration.value == 10,
                  "the rejected tool call left the document untouched");

            bool sawFailedResult = false;
            for (const ChatTranscriptEntry& entry : session.Transcript()) {
                if (entry.kind == ChatEntryKind::ToolResult && !entry.tool_ok) {
                    sawFailedResult = true;
                    Check(entry.text.find("ValidationFailed") !=
                                  std::string::npos &&
                              entry.text.find("no object matches") !=
                                  std::string::npos,
                          "the failure names the resolver's own rejection, "
                          "got: " +
                              entry.text);
                }
            }
            Check(sawFailedResult,
                  "the transcript records the failed tool call");
        });

    Test(
        "Ollama describe results are compacted before the model follow-up", [] {
            const std::string description =
                R"({"sequence":{"name":"Montage"},"timeline":{)"
                R"("duration":{"frames":10},"tracks":[{"items":[)"
                R"({"type":"clip","source_id":"used"}]}],"sources":[)"
                R"({"id":"used","file":"used.mov"},)"
                R"({"id":"unused","file":"unused.mov"}]},"library":[)"
                R"({"alias":"M1","id":"unused","filename":"unused.mov",)"
                R"("codec":"codec_marker","in_use":false}],)"
                R"("bins":[],"markers":[]})";
            InMemoryBackend backend(Fixture(), description);
            McpToolRegistry registry;
            ScriptedTransport transport({
                R"({"message":{"role":"assistant","content":)"
                R"("{\"name\":\"describe\",\"arguments\":{}}"},)"
                R"("done":true,"done_reason":"stop"})",
                R"({"message":{"role":"assistant","content":"Diagnostic."},)"
                R"("done":true,"done_reason":"stop"})",
            });
            ChatLlmClient llm(OllamaConfig(), std::ref(transport));
            ChatSession session(backend, registry, llm);

            std::string error;
            const bool ok = session.SubmitUserMessage("Décris.", error);
            Check(ok, "the Ollama describe loop succeeds: " + error);
            Check(transport.CallCount() == 2,
                  "textual describe call produces a model follow-up");

            Value request;
            std::string parseError;
            Check(Value::Parse(transport.Request(1).body, request, parseError),
                  "follow-up request is valid JSON: " + parseError);
            const Value* messages = request.Find("messages");
            const Value* content = nullptr;
            if (messages && messages->IsArray() && !messages->AsArray().empty())
                content = messages->AsArray().back().Find("content");
            Value compact;
            Check(content && content->IsString() &&
                      Value::Parse(content->AsString(), compact, parseError),
                  "tool content remains structured JSON: " + parseError);
            const Value* timeline = compact.Find("timeline");
            const Value* sources =
                timeline ? timeline->Find("sources") : nullptr;
            Check(
                sources && sources->IsArray() && sources->AsArray().size() == 1,
                "only timeline-used sources remain in the compact view");
            const Value* library = compact.Find("library");
            Check(library && library->IsArray() &&
                      library->AsArray().front().Find("codec") == nullptr,
                  "heavy library metadata is omitted for the local model");
        });

    Test("a transport failure stops the turn and is recorded as an error", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse&,
                            std::string& transportError) {
            transportError = "connection refused";
            return false;
        };
        ChatLlmClient llm(TestConfig(), transport);
        ChatSession session(backend, registry, llm);

        std::string error;
        const bool ok = session.SubmitUserMessage("Peu importe.", error);
        Check(!ok, "the turn is reported as failed");
        Check(error.find("connection refused") != std::string::npos,
              "the transport error is surfaced, got: " + error);
        Check(session.Transcript().back().kind == ChatEntryKind::Error,
              "the transcript ends with an Error entry");
    });

    Test("a model that keeps requesting tools forever trips the safety limit", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        std::vector<std::string> responses;
        for (int i = 0; i < 20; ++i) {
            responses.push_back(
                R"({"content":[{"type":"tool_use","id":"toolu_)" +
                std::to_string(i) +
                R"(","name":"describe","input":{}}],"stop_reason":"tool_use"})");
        }
        ScriptedTransport transport(std::move(responses));
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);

        std::string error;
        const bool ok = session.SubmitUserMessage("Boucle infinie.", error);
        Check(!ok, "the turn is reported as failed once the limit trips");
        Check(!error.empty(), "an explanatory error is set");
        Check(transport.CallCount() < 20,
              "the loop actually stopped instead of exhausting the script");
    });

    // BENCH-2026-09 -- a bench of the in-app agent reads what a whole
    // session spent, across tool rounds and across instructions.
    Test("usage sums every model call and counts the unreported ones", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        ScriptedTransport transport({
            R"({"content":[{"type":"tool_use","id":"toolu_1",)"
            R"("name":"describe","input":{}}],"stop_reason":"tool_use",)"
            R"("usage":{"input_tokens":10,"cache_creation_input_tokens":2000,)"
            R"("cache_read_input_tokens":0,"output_tokens":30}})",
            R"({"content":[{"type":"text","text":"Vu."}],)"
            R"("stop_reason":"end_turn"})",
            R"({"content":[{"type":"text","text":"Encore vu."}],)"
            R"("stop_reason":"end_turn","usage":{"input_tokens":5,)"
            R"("cache_creation_input_tokens":0,)"
            R"("cache_read_input_tokens":2000,"output_tokens":7}})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);

        std::string error;
        Check(session.SubmitUserMessage("Décris la timeline.", error),
              "the first instruction completes: " + error);
        Check(session.SubmitUserMessage("Et maintenant ?", error),
              "the second instruction completes: " + error);

        const chat::ChatUsageTotals& usage = session.Usage();
        Check(usage.calls == 3, "three model calls are counted");
        Check(usage.unreported_calls == 1,
              "the response without usage is counted as unreported");
        Check(usage.input_tokens == 15 && usage.cache_write_tokens == 2000 &&
                  usage.cache_read_tokens == 2000 && usage.output_tokens == 37,
              "the reported counts are summed across both instructions");
    });

    Test("a failed model call adds nothing to the usage", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        auto transport = [](const ChatHttpRequest&, ChatHttpResponse&,
                            std::string& transportError) {
            transportError = "connection refused";
            return false;
        };
        ChatLlmClient llm(TestConfig(), transport);
        ChatSession session(backend, registry, llm);

        std::string error;
        session.SubmitUserMessage("Peu importe.", error);
        Check(session.Usage().calls == 0, "no call is counted");
    });

    // PRD22 -- ROADMAP.md. The default model, claude-opus-5-5, ties each
    // thinking block to the conversation that produced it: for an account
    // opened since 31 August 2026, a block replayed after an earlier turn,
    // the system prompt or the tools changed is refused with a 400 (claude-api
    // skill, "preserved thinking"). Moving the cache breakpoint to the newest
    // block is allowed; anything else the panel rewrote would break every
    // turn after it.
    Test("each call repeats the previous one's system, tools and history", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        ScriptedTransport transport({
            R"({"content":[{"type":"thinking","thinking":"",)"
            R"("signature":"sig-1"},{"type":"tool_use","id":"toolu_1",)"
            R"("name":"describe","input":{}}],"stop_reason":"tool_use"})",
            R"({"content":[{"type":"thinking","thinking":"",)"
            R"("signature":"sig-2"},{"type":"text","text":"Vu."}],)"
            R"("stop_reason":"end_turn"})",
            R"({"content":[{"type":"text","text":"Encore vu."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);
        std::string error;
        Check(session.SubmitUserMessage("Décris la timeline.", error),
              "the first instruction completes: " + error);
        Check(session.SubmitUserMessage("Et maintenant ?", error),
              "the second instruction completes: " + error);
        Check(transport.CallCount() == 3, "three model calls");
        if (transport.CallCount() != 3) return;

        // A request as the model checks it: the cache breakpoints out.
        const auto checked = [&transport](size_t index) {
            std::string body = transport.Request(index).body;
            const std::string marker =
                R"(,"cache_control":{"type":"ephemeral"})";
            for (size_t at = body.find(marker); at != std::string::npos;
                 at = body.find(marker, at))
                body.erase(at, marker.size());
            Value value;
            std::string parseError;
            Check(Value::Parse(body, value, parseError),
                  "request " + std::to_string(index) +
                      " is valid JSON: " + parseError);
            return value;
        };
        const auto field = [](const Value& request, const char* key) {
            const Value* value = request.Find(key);
            return value ? value->Dump() : std::string();
        };
        for (size_t index = 1; index < 3; ++index) {
            const Value before = checked(index - 1);
            const Value after = checked(index);
            const std::string call = "call " + std::to_string(index + 1);
            Check(!field(after, "system").empty() &&
                      field(before, "system") == field(after, "system"),
                  call + " sends the same system prompt");
            Check(!field(after, "tools").empty() &&
                      field(before, "tools") == field(after, "tools"),
                  call + " sends the same tools");
            const Value* earlier = before.Find("messages");
            const Value* later = after.Find("messages");
            bool grows = earlier && later && earlier->IsArray() &&
                         later->IsArray() &&
                         later->AsArray().size() > earlier->AsArray().size();
            for (size_t message = 0;
                 grows && message < earlier->AsArray().size(); ++message)
                grows = earlier->AsArray()[message].Dump() ==
                        later->AsArray()[message].Dump();
            Check(grows, call +
                             " repeats every earlier message byte for "
                             "byte, then adds to them");
        }
        const std::string& last = transport.Request(2).body;
        Check(last.find(R"("signature":"sig-1")") != std::string::npos &&
                  last.find(R"("signature":"sig-2")") != std::string::npos,
              "both thinking blocks are replayed as they came");
    });

    Test("a declined reply ends the turn and stays out of the history", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        ScriptedTransport transport({
            R"({"content":[],"stop_reason":"refusal",)"
            R"("stop_details":{"type":"refusal","category":"cyber"}})",
            R"({"content":[{"type":"text","text":"D'accord."}],)"
            R"("stop_reason":"end_turn"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);

        std::string error;
        Check(!session.SubmitUserMessage("Première consigne.", error),
              "the declined turn is reported as failed");
        Check(error.find("refusal") != std::string::npos,
              "the error says the turn was declined, got: " + error);
        Check(session.Transcript().back().kind == ChatEntryKind::Error,
              "the transcript ends with an Error entry");
        Check(session.SubmitUserMessage("Autre consigne.", error),
              "the next instruction runs: " + error);
        Value body;
        std::string parseError;
        Check(transport.CallCount() == 2 &&
                  Value::Parse(transport.Request(1).body, body, parseError),
              "the second call's request parses: " + parseError);
        const Value* messages = body.Find("messages");
        bool usersOnly =
            messages && messages->IsArray() && messages->AsArray().size() == 2;
        for (size_t index = 0; usersOnly && index < 2; ++index) {
            const Value* role = messages->AsArray()[index].Find("role");
            usersOnly = role && role->IsString() && role->AsString() == "user";
        }
        Check(usersOnly,
              "the next call carries the two instructions and no reply");
    });

    Test("a reply cut inside a tool call applies nothing", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        ScriptedTransport transport({
            R"({"content":[{"type":"tool_use","id":"toolu_1",)"
            R"("name":"trim_clip","input":{"clip_id":)"
            R"("01K30000000000000000000003","edge":"Tail",)"
            R"("delta":{"value":-1,"rate":25}}}],"stop_reason":"max_tokens"})",
        });
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);

        std::string error;
        Check(!session.SubmitUserMessage("Raccourcis le plan.", error),
              "the turn is reported as failed");
        Check(error.find("max_tokens") != std::string::npos,
              "the error names the limit, got: " + error);
        Check(backend.CurrentDocument()
                      .sequence.tracks.at(0)
                      .clips.at(0)
                      .duration.value == 10,
              "the clip keeps its ten frames");
        bool called = false;
        for (const ChatTranscriptEntry& entry : session.Transcript())
            called |= entry.kind == ChatEntryKind::ToolCall;
        Check(!called, "no tool call is shown, none ran");
    });

    // FI8 -- docs/roadmap/FIABILITE-2026-10.md. Complete tool JSON can
    // arrive with a truncated reply: neither provider may run that subset.
    Test("length-truncated tool replies apply nothing", [] {
        for (const auto provider : {chat::ChatLlmProvider::OpenAiCompatible,
                                    chat::ChatLlmProvider::Ollama}) {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            const std::string before = backend.CurrentDocument().SaveToString();
            const bool openAi =
                provider == chat::ChatLlmProvider::OpenAiCompatible;
            ScriptedTransport transport(
                {openAi
                     ? R"({"choices":[{"message":{"role":"assistant",)"
                       R"("content":null,"tool_calls":[{"id":"cut",)"
                       R"("type":"function","function":{"name":"trim_clip",)"
                       R"("arguments":"{\"clip_id\":\"01K30000000000000000000003\",)"
                       R"(\"edge\":\"Tail\",\"delta\":{\"value\":-1,\"rate\":25}}")"
                       R"(}}]},"finish_reason":"length"}]})"
                     : R"({"message":{"role":"assistant","content":"",)"
                       R"("tool_calls":[{"function":{"name":"trim_clip",)"
                       R"("arguments":{"clip_id":"01K30000000000000000000003",)"
                       R"("edge":"Tail","delta":{"value":-1,"rate":25}}}}]},)"
                       R"("done":true,"done_reason":"length"})"});
            auto config = TestConfig();
            config.provider = provider;
            ChatLlmClient llm(config, std::ref(transport));
            ChatSession session(backend, registry, llm);
            std::string error;
            Check(!session.SubmitUserMessage("Raccourcis le plan.", error) &&
                      error.find("length") != std::string::npos,
                  "the provider's length limit fails the turn: " + error);
            Check(backend.CurrentDocument().SaveToString() == before,
                  "the complete document stays unchanged");
            const auto& transcript = session.Transcript();
            Check(transcript.size() == 2 &&
                      transcript.back().kind == ChatEntryKind::Error,
                  "the partial tool set is neither announced nor run");
        }
    });

    // FI8 -- Anthropic also cuts a reply where the model's context window
    // ends; the refused reply was still paid for, and its tokens count.
    Test("a tool call cut by the context window applies nothing", [] {
        InMemoryBackend backend(Fixture());
        McpToolRegistry registry;
        const std::string before = backend.CurrentDocument().SaveToString();
        ScriptedTransport transport(
            {R"({"content":[{"type":"tool_use","id":"cut",)"
             R"("name":"trim_clip","input":{)"
             R"("clip_id":"01K30000000000000000000003","edge":"Tail",)"
             R"("delta":{"value":-1,"rate":25}}}],)"
             R"("stop_reason":"model_context_window_exceeded",)"
             R"("usage":{"input_tokens":120,"output_tokens":7}})"});
        ChatLlmClient llm(TestConfig(), std::ref(transport));
        ChatSession session(backend, registry, llm);
        std::string error;
        Check(!session.SubmitUserMessage("Raccourcis le plan.", error) &&
                  error.find("model_context_window_exceeded") !=
                      std::string::npos,
              "the context window's end fails the turn: " + error);
        Check(backend.CurrentDocument().SaveToString() == before,
              "the document stays unchanged");
        Check(session.Usage().calls == 1 && session.Usage().output_tokens == 7,
              "the refused reply's tokens are counted");
    });

    Test("truncated text stays visible and ends with an error", [] {
        for (const auto provider : {chat::ChatLlmProvider::AnthropicMessages,
                                    chat::ChatLlmProvider::OpenAiCompatible,
                                    chat::ChatLlmProvider::Ollama}) {
            InMemoryBackend backend(Fixture());
            McpToolRegistry registry;
            const std::string body =
                provider == chat::ChatLlmProvider::AnthropicMessages
                    ? R"({"content":[{"type":"text","text":"Voici "}],)"
                      R"("stop_reason":"max_tokens"})"
                : provider == chat::ChatLlmProvider::OpenAiCompatible
                    ? R"({"choices":[{"message":{"role":"assistant",)"
                      R"("content":"Voici "},"finish_reason":"length"}]})"
                    : R"({"message":{"role":"assistant","content":"Voici "},)"
                      R"("done":true,"done_reason":"length"})";
            ScriptedTransport transport({body});
            auto config = TestConfig();
            config.provider = provider;
            ChatLlmClient llm(config, std::ref(transport));
            ChatSession session(backend, registry, llm);
            std::string error;
            Check(!session.SubmitUserMessage("Explique ce montage.", error),
                  "a token limit is not a successful completion");
            const auto& transcript = session.Transcript();
            Check(transcript.size() == 3 &&
                      transcript[1].kind == ChatEntryKind::AssistantText &&
                      transcript[1].text == "Voici " &&
                      transcript.back().kind == ChatEntryKind::Error,
                  "the partial answer and why it stopped are visible");
            Check(session.Usage().calls == 1,
                  "the text response still counts as a model call");
        }
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All chat session tests passed\n";
    return 0;
}
