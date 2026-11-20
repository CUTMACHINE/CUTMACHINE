// ACP1 and ACP2 -- docs/roadmap/ACP-2026-09.md. The Agent panel's client of
// an external coding agent, against a scripted agent: what CUTMACHINE sends
// at the start of a session (protocol version, no files or terminal, the
// engine's MCP server), how a turn becomes the panel's transcript, which
// tool calls pass without asking and which are put to the person, how a
// turn stops, and what happens when the agent dies, is too new, or is not
// logged in. Then the process transport itself, on /bin/sh.
//
// Plain C++ and POSIX: no AppKit, no model, no network.

#include "AcpClient.h"
#include "AcpProcess.h"
#include "ChatTranscript.h"
#include "Json.h"

#include <unistd.h>

#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "TestSupport.h"

namespace {

using acp::AcpChatSession;
using acp::AcpPermissionRequest;
using acp::AcpSessionConfig;
using chat::ChatEntryKind;
using chat::ChatTranscriptEntry;
using mcp_json::Value;

Value Parse(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        std::cerr << "unparsable test JSON: " << text << " (" << error << ")\n";
        std::abort();
    }
    return value;
}

std::string StringAt(const Value& root,
                     std::initializer_list<const char*> keys) {
    const Value* current = &root;
    for (const char* key : keys) {
        current = current->Find(key);
        if (current == nullptr) {
            return {};
        }
    }
    return current->IsString() ? current->AsString() : current->Dump();
}

class ScriptedAgent;

// What the agents a test starts share with the test: every message the
// client wrote, and how an agent answers each one.
struct Script {
    std::vector<Value> written;
    std::function<void(const Value& message, ScriptedAgent& agent)> answer;
    int starts = 0;
    bool closed = false;
    std::string hint;
};

// The agent side of the pipe, played by the test on the client's own
// thread: each message the client writes is answered at once, and the
// answer waits in a queue for the client's next reads. An action in the
// queue runs when the client reaches it, as if the agent did something at
// that moment of the turn.
class ScriptedAgent : public acp::AcpTransport {
public:
    explicit ScriptedAgent(Script& script) : script_(script) {}

    bool WriteLine(const std::string& line, std::string& error) override {
        if (script_.closed) {
            error = "closed";
            return false;
        }
        const Value message = Parse(line);
        script_.written.push_back(message);
        if (script_.answer) {
            script_.answer(message, *this);
        }
        return true;
    }
    bool ReadLine(std::string& line) override {
        while (!queue_.empty()) {
            auto next = std::move(queue_.front());
            queue_.pop_front();
            if (auto* action = std::get_if<std::function<void()>>(&next)) {
                (*action)();
                continue;
            }
            line = std::get<std::string>(next);
            return true;
        }
        return false;  // nothing more: the agent has gone
    }
    void Close() override {
        script_.closed = true;
        queue_.clear();
    }
    bool Closed() const override { return script_.closed; }
    std::string DiagnosticHint() const override { return script_.hint; }

    void Push(const std::string& line) { queue_.emplace_back(line); }
    void Then(std::function<void()> action) {
        queue_.emplace_back(std::move(action));
    }
    void Respond(const Value& request, const std::string& resultJson) {
        Push(R"({"jsonrpc":"2.0","id":)" + request.Find("id")->Dump() +
             R"(,"result":)" + resultJson + "}");
    }
    void Update(const std::string& updateJson) {
        Push(
            R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"S1","update":)" +
            updateJson + "}}");
    }
    void Ask(int id, const std::string& method, const std::string& params) {
        Push(R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
             R"(,"method":")" + method + R"(","params":)" + params + "}");
    }

private:
    Script& script_;
    std::deque<std::variant<std::string, std::function<void()>>> queue_;
};

std::string Method(const Value& message) {
    return StringAt(message, {"method"});
}

// An agent that starts normally and hands each prompt to `onPrompt`.
void StandardAgent(
    Script& script, std::function<void(const Value&, ScriptedAgent&)> onPrompt,
    std::function<void(const Value&, ScriptedAgent&)> onOther = nullptr) {
    script.answer = [onPrompt, onOther](const Value& message,
                                        ScriptedAgent& agent) {
        const std::string method = Method(message);
        if (method == "initialize") {
            agent.Respond(
                message,
                R"({"protocolVersion":1,"agentCapabilities":{},"agentInfo":{"name":"fake","title":"Agent de test","version":"1"}})");
        } else if (method == "session/new") {
            agent.Respond(message, R"({"sessionId":"S1"})");
        } else if (method == "session/prompt") {
            onPrompt(message, agent);
        } else if (onOther) {
            onOther(message, agent);
        }
    };
}

acp::AcpTransportFactory FactoryFor(Script& script) {
    return [&script](std::string&) -> std::unique_ptr<acp::AcpTransport> {
        ++script.starts;
        script.closed = false;
        return std::make_unique<ScriptedAgent>(script);
    };
}

AcpSessionConfig Config() {
    AcpSessionConfig config;
    config.cwd = "/tmp/projet";
    config.mcp_servers.push_back(
        {"cutmachine",
         "/Applications/cutmachine",
         {"--mcp-serve", "/tmp/projet/p.cutmachine.json", "--stdio"},
         {}});
    config.preamble = "PRÉAMBULE";
    return config;
}

std::vector<Value> WrittenWithMethod(const Script& script,
                                     const std::string& method) {
    std::vector<Value> found;
    for (const Value& message : script.written) {
        if (Method(message) == method) {
            found.push_back(message);
        }
    }
    return found;
}

// The client's answer to the agent's request `id`.
const Value* AnswerTo(const Script& script, int id) {
    for (const Value& message : script.written) {
        const Value* messageId = message.Find("id");
        if (message.Find("method") == nullptr && messageId != nullptr &&
            messageId->Dump() == std::to_string(id)) {
            return &message;
        }
    }
    return nullptr;
}

std::vector<ChatEntryKind> Kinds(const AcpChatSession& session) {
    std::vector<ChatEntryKind> kinds;
    for (const ChatTranscriptEntry& entry : session.Transcript()) {
        kinds.push_back(entry.kind);
    }
    return kinds;
}

std::string Slurp(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

}  // namespace

int main() {
    Test("handshake: version 1, no files or terminal, the engine's server", [] {
        Script script;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Update(
                R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Bon"}})");
            agent.Update(
                R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"jour"}})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(session.SubmitUserMessage("Première", error),
              "the first turn ends normally: " + error);
        Check(session.AgentTitle() == "Agent de test",
              "the agent's title is kept: " + session.AgentTitle());

        const auto initialize = WrittenWithMethod(script, "initialize");
        Check(initialize.size() == 1, "one initialize");
        if (!initialize.empty()) {
            const Value& params = *initialize[0].Find("params");
            Check(params.Find("protocolVersion")->Dump() == "1",
                  "protocol version 1");
            Check(StringAt(params, {"clientCapabilities", "fs",
                                    "readTextFile"}) == "false" &&
                      StringAt(params, {"clientCapabilities", "fs",
                                        "writeTextFile"}) == "false" &&
                      StringAt(params, {"clientCapabilities", "terminal"}) ==
                          "false",
                  "neither files nor a terminal are offered: " + params.Dump());
        }
        const auto created = WrittenWithMethod(script, "session/new");
        Check(created.size() == 1, "one session/new");
        if (!created.empty()) {
            const Value& params = *created[0].Find("params");
            Check(StringAt(params, {"cwd"}) == "/tmp/projet", "the cwd");
            const Value* servers = params.Find("mcpServers");
            Check(servers != nullptr && servers->IsArray() &&
                      servers->AsArray().size() == 1,
                  "one MCP server handed to the agent");
            if (servers != nullptr && servers->IsArray() &&
                !servers->AsArray().empty()) {
                const Value& server = servers->AsArray()[0];
                Check(
                    server.Dump() ==
                        R"({"name":"cutmachine","command":"/Applications/cutmachine","args":["--mcp-serve","/tmp/projet/p.cutmachine.json","--stdio"],"env":[]})",
                    "the engine's stdio server, as ACP describes one: " +
                        server.Dump());
            }
        }
        auto prompts = WrittenWithMethod(script, "session/prompt");
        Check(
            prompts.size() == 1 &&
                prompts[0].Find("params")->Find("prompt")->Dump() ==
                    R"([{"type":"text","text":"PRÉAMBULE"},{"type":"text","text":"Première"}])",
            "the preamble goes before the first instruction");
        Check(Kinds(session) ==
                  std::vector<ChatEntryKind>{ChatEntryKind::UserMessage,
                                             ChatEntryKind::AssistantText},
              "the chunks make one line");
        Check(session.Transcript().back().text == "Bonjour", "Bonjour");

        Check(session.SubmitUserMessage("Deuxième", error),
              "the second turn: " + error);
        prompts = WrittenWithMethod(script, "session/prompt");
        Check(script.starts == 1 &&
                  WrittenWithMethod(script, "initialize").size() == 1,
              "the same agent and session serve the next turn");
        Check(prompts.size() == 2 &&
                  prompts[1].Find("params")->Find("prompt")->Dump() ==
                      R"([{"type":"text","text":"Deuxième"}])",
              "no preamble the second time");
    });

    Test("an engine call passes without asking, in the transcript's order", [] {
        Script script;
        int asked = 0;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Update(
                R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Je regarde."}})");
            // Streamed: announced empty, completed by an update.
            agent.Update(
                R"({"sessionUpdate":"tool_call","toolCallId":"t1","title":"mcp__cutmachine__read_frame","kind":"other","status":"pending","rawInput":{}})");
            agent.Update(
                R"({"sessionUpdate":"tool_call_update","toolCallId":"t1","rawInput":{"clip":"A1"}})");
            agent.Ask(
                90, "session/request_permission",
                R"({"sessionId":"S1","toolCall":{"toolCallId":"t1","title":"Lire une image","_meta":{"claudeCode":{"toolName":"mcp__cutmachine__read_frame","mcpServer":{"name":"cutmachine","source":"config"}}}},"options":[{"optionId":"a","name":"Toujours","kind":"allow_always"},{"optionId":"o","name":"Oui","kind":"allow_once"},{"optionId":"r","name":"Non","kind":"reject_once"}]})");
            agent.Update(
                R"({"sessionUpdate":"tool_call_update","toolCallId":"t1","status":"completed","content":[{"type":"content","content":{"type":"text","text":"image lue"}},{"type":"content","content":{"type":"image","data":"iVBOR","mimeType":"image/png"}}]})");
            agent.Update(
                R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"C'est net."}})");
            agent.Respond(
                prompt,
                R"({"stopReason":"end_turn","usage":{"totalTokens":30,"inputTokens":10,"outputTokens":20,"cachedReadTokens":5}})");
        });
        AcpChatSession session(
            FactoryFor(script), Config(), nullptr,
            [&asked](const AcpPermissionRequest&) -> std::string {
                ++asked;
                return "r";
            });
        std::string error;
        Check(session.SubmitUserMessage("Regarde A1", error),
              "the turn ends: " + error);
        Check(asked == 0, "the person was not asked about an engine call");
        const Value* answer = AnswerTo(script, 90);
        Check(answer != nullptr &&
                  answer->Find("result")->Dump() ==
                      R"({"outcome":{"outcome":"selected","optionId":"o"}})",
              "allowed once, not always");
        Check(Kinds(session) ==
                  std::vector<ChatEntryKind>{
                      ChatEntryKind::UserMessage, ChatEntryKind::AssistantText,
                      ChatEntryKind::ToolCall, ChatEntryKind::ToolResult,
                      ChatEntryKind::AssistantText},
              "text, one call, its result, text");
        const auto& transcript = session.Transcript();
        if (transcript.size() == 5) {
            Check(transcript[2].tool_name == "read_frame",
                  "the tool's own name: " + transcript[2].tool_name);
            Check(transcript[2].tool_args_json == R"({"clip":"A1"})",
                  "with the arguments once streamed: " +
                      transcript[2].tool_args_json);
            Check(transcript[3].tool_ok && transcript[3].text == "image lue",
                  "the result: " + transcript[3].text);
            Check(transcript[3].image_base64 == "iVBOR" &&
                      transcript[3].image_mime == "image/png",
                  "and its picture");
        }
        Check(session.Usage().turns == 1 &&
                  session.Usage().input_tokens == 10 &&
                  session.Usage().output_tokens == 20 &&
                  session.Usage().cache_read_tokens == 5,
              "the turn's usage");
    });

    Test("anything else is put to the person, and their answer given", [] {
        Script script;
        std::vector<AcpPermissionRequest> asked;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Update(
                R"({"sessionUpdate":"tool_call","toolCallId":"t2","title":"rm -rf rushes","kind":"execute","status":"pending","rawInput":{"command":"rm -rf rushes"},"_meta":{"claudeCode":{"toolName":"Bash"}}})");
            agent.Ask(
                91, "session/request_permission",
                R"({"sessionId":"S1","toolCall":{"toolCallId":"t2","title":"rm -rf rushes","kind":"execute"},"options":[{"optionId":"o","name":"Oui","kind":"allow_once"},{"optionId":"r","name":"Non","kind":"reject_once"}]})");
            agent.Update(
                R"({"sessionUpdate":"tool_call_update","toolCallId":"t2","status":"failed","content":[{"type":"content","content":{"type":"text","text":"refusé"}}]})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(
            FactoryFor(script), Config(), nullptr,
            [&asked](const AcpPermissionRequest& request) -> std::string {
                asked.push_back(request);
                return "r";
            });
        std::string error;
        Check(session.SubmitUserMessage("Range", error), "the turn: " + error);
        Check(asked.size() == 1, "asked once");
        if (asked.size() == 1) {
            Check(asked[0].title == "rm -rf rushes" &&
                      asked[0].kind == "execute" &&
                      asked[0].raw_input_json.find("rm -rf") !=
                          std::string::npos &&
                      asked[0].options.size() == 2,
                  "about the command, with its options");
        }
        const Value* answer = AnswerTo(script, 91);
        Check(answer != nullptr &&
                  answer->Find("result")->Dump() ==
                      R"({"outcome":{"outcome":"selected","optionId":"r"}})",
              "the person's refusal is the answer");
        Check(!session.Transcript().back().tool_ok &&
                  session.Transcript().back().text == "refusé",
              "and the call shows as failed");
        int calls = 0;
        for (const auto& entry : session.Transcript()) {
            calls += entry.kind == ChatEntryKind::ToolCall ? 1 : 0;
        }
        Check(calls == 1, "shown once");

        // Nobody to ask: refused.
        Script alone;
        StandardAgent(alone, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Ask(
                92, "session/request_permission",
                R"({"sessionId":"S1","toolCall":{"toolCallId":"t3","title":"Edit README","kind":"edit"},"options":[{"optionId":"o","name":"Oui","kind":"allow_once"},{"optionId":"never","name":"Jamais","kind":"reject_always"}]})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession unattended(FactoryFor(alone), Config());
        Check(unattended.SubmitUserMessage("Écris", error), "the turn");
        const Value* refused = AnswerTo(alone, 92);
        Check(
            refused != nullptr &&
                refused->Find("result")->Dump() ==
                    R"({"outcome":{"outcome":"selected","optionId":"never"}})",
            "without a prompt, the call is refused");
    });

    // GENERATIF-2026-10 GN2 -- an engine call that spends the person's
    // money is theirs to approve, once at a time; nobody to ask, refused.
    Test("a paid engine call is put to the person, never always", [] {
        const auto play = [](Script& script) {
            StandardAgent(script, [](const Value& prompt,
                                     ScriptedAgent& agent) {
                agent.Update(
                    R"({"sessionUpdate":"tool_call","toolCallId":"g1","title":"mcp__cutmachine__generate_media","kind":"other","status":"pending","rawInput":{"proposal_id":"P1","confirmed_by_person":true}})");
                agent.Ask(
                    93, "session/request_permission",
                    R"({"sessionId":"S1","toolCall":{"toolCallId":"g1","title":"Générer","_meta":{"claudeCode":{"toolName":"mcp__cutmachine__generate_media","mcpServer":{"name":"cutmachine","source":"config"}}}},"options":[{"optionId":"a","name":"Toujours","kind":"allow_always"},{"optionId":"o","name":"Oui","kind":"allow_once"},{"optionId":"r","name":"Non","kind":"reject_once"}]})");
                agent.Respond(prompt, R"({"stopReason":"end_turn"})");
            });
        };
        Script script;
        play(script);
        std::vector<AcpPermissionRequest> asked;
        AcpChatSession session(
            FactoryFor(script), Config(), nullptr,
            [&asked](const AcpPermissionRequest& request) -> std::string {
                asked.push_back(request);
                return "o";
            });
        std::string error;
        Check(session.SubmitUserMessage("Étends-la en 9:16", error),
              "the turn: " + error);
        Check(asked.size() == 1, "the person was asked");
        if (asked.size() == 1) {
            bool always = false;
            for (const auto& option : asked[0].options)
                always = always || option.kind == "allow_always";
            Check(asked[0].title == "generate_media" && !always &&
                      asked[0].options.size() == 2,
                  "about generate_media, without 'always'");
        }
        const Value* answer = AnswerTo(script, 93);
        Check(answer != nullptr &&
                  answer->Find("result")->Dump() ==
                      R"({"outcome":{"outcome":"selected","optionId":"o"}})",
              "their yes, once, is the answer");

        Script alone;
        play(alone);
        AcpChatSession unattended(FactoryFor(alone), Config());
        Check(unattended.SubmitUserMessage("Étends-la", error), "the turn");
        const Value* refused = AnswerTo(alone, 93);
        Check(refused != nullptr &&
                  refused->Find("result")->Dump() ==
                      R"({"outcome":{"outcome":"selected","optionId":"r"}})",
              "with nobody to ask, the spend is refused");
    });

    // GN2 -- an after_* call passes without asking.
    const char* const engineCallsAsked =
        "an after_* call passes without asking";
    Test(engineCallsAsked, [] {
        struct Call {
            const char* id;
            const char* tool;
            const char* input;
            bool asked;
        };
        const std::vector<Call> calls = {
            {"a1", "after_describe_comp", R"({"comp":"Main"})", false},
            {"a2", "after_render_frame", R"({"comp":"Main"})", false},
        };
        for (const Call& call : calls) {
            Script script;
            StandardAgent(script, [call](const Value& prompt,
                                         ScriptedAgent& agent) {
                agent.Update(std::string(R"({"sessionUpdate":"tool_call",)") +
                             R"("toolCallId":")" + call.id +
                             R"(","title":"mcp__cutmachine__)" + call.tool +
                             R"(","kind":"other","status":"pending",)" +
                             R"("rawInput":)" + call.input + "}");
                agent.Ask(94, "session/request_permission",
                          std::string(R"({"sessionId":"S1","toolCall":{)") +
                              R"("toolCallId":")" + call.id +
                              R"(","title":"Outil du moteur","_meta":{)" +
                              R"("claudeCode":{"toolName":)" +
                              R"("mcp__cutmachine__)" + call.tool +
                              R"(","mcpServer":{"name":"cutmachine",)" +
                              R"("source":"config"}}}},"options":[)" +
                              R"({"optionId":"a","name":"Toujours",)" +
                              R"("kind":"allow_always"},{"optionId":"o",)" +
                              R"("name":"Oui","kind":"allow_once"},)" +
                              R"({"optionId":"r","name":"Non",)" +
                              R"("kind":"reject_once"}]})");
                agent.Respond(prompt, R"({"stopReason":"end_turn"})");
            });
            std::vector<AcpPermissionRequest> asked;
            AcpChatSession session(
                FactoryFor(script), Config(), nullptr,
                [&asked](const AcpPermissionRequest& request) -> std::string {
                    asked.push_back(request);
                    return "o";
                });
            std::string error;
            Check(session.SubmitUserMessage("Outil du moteur", error),
                  std::string("the turn of ") + call.id + ": " + error);
            const Value* answer = AnswerTo(script, 94);
            if (call.asked) {
                bool always = false;
                for (const AcpPermissionRequest& request : asked) {
                    for (const auto& option : request.options) {
                        always = always || option.kind == "allow_always";
                    }
                }
                Check(
                    asked.size() == 1 && asked[0].title == call.tool && !always,
                    std::string(call.id) + " " + call.input +
                        " is put to the person, without 'always'");
            } else {
                Check(
                    asked.empty() && answer != nullptr &&
                        answer->Find("result")->Dump() ==
                            R"({"outcome":{"outcome":"selected","optionId":"o"}})",
                    std::string(call.id) + " " + call.input +
                        " is approved without asking");
            }
        }
    });

    Test("Codex's names for an engine call", [] {
        Check(
            acp::IsEngineToolCall(
                Parse(R"({"title":"mcp.cutmachine.describe"})"), "cutmachine"),
            "mcp.<server>.<tool>");
        Check(
            acp::IsEngineToolCall(
                Parse(
                    R"({"title":"x","rawInput":{"server":"cutmachine","tool":"describe","arguments":{}}})"),
                "cutmachine"),
            "{server, tool} raw input");
        Check(!acp::IsEngineToolCall(Parse(R"({"title":"mcp.autre.describe"})"),
                                     "cutmachine"),
              "another server");
        Check(!acp::IsEngineToolCall(
                  Parse(R"({"title":"mcp__cutmachineX__describe"})"),
                  "cutmachine"),
              "a server whose name only starts the same");
        Check(!acp::IsEngineToolCall(Parse(R"({"title":"mcp__cutmachine__"})"),
                                     "cutmachine"),
              "a prefix without a tool");
        Check(
            !acp::IsEngineToolCall(Parse(R"({"title":"Bash"})"), "cutmachine"),
            "a shell");
        Check(acp::DisplayToolName("mcp.cutmachine.add_marker", "cutmachine") ==
                      "add_marker" &&
                  acp::DisplayToolName("git status", "cutmachine") ==
                      "git status",
              "display names");

        // Codex asks about an engine call it already announced, by its id.
        Script script;
        int asked = 0;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Update(
                R"({"sessionUpdate":"tool_call","toolCallId":"c1","title":"mcp.cutmachine.add_marker","kind":"execute","status":"in_progress","rawInput":{"server":"cutmachine","tool":"add_marker","arguments":{"name":"M"}}})");
            agent.Ask(
                93, "session/request_permission",
                R"({"sessionId":"S1","toolCall":{"toolCallId":"c1","kind":"execute","status":"pending"},"_meta":{"is_mcp_tool_approval":true},"options":[{"optionId":"approved","name":"Approve","kind":"allow_once"},{"optionId":"denied","name":"Deny","kind":"reject_once"}]})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(
            FactoryFor(script), Config(), nullptr,
            [&asked](const AcpPermissionRequest&) -> std::string {
                ++asked;
                return "denied";
            });
        std::string error;
        Check(session.SubmitUserMessage("Marque", error), "the turn");
        const Value* answer = AnswerTo(script, 93);
        Check(asked == 0 && answer != nullptr &&
                  StringAt(*answer, {"result", "outcome", "optionId"}) ==
                      "approved",
              "granted from what the call was announced as");
    });

    Test("stop during a turn: session/cancel, then the agent's cancelled", [] {
        Script script;
        AcpChatSession* live = nullptr;
        StandardAgent(
            script,
            [&live](const Value& prompt, ScriptedAgent& agent) {
                agent.Update(
                    R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Je commence"}})");
                agent.Then([&live] { live->RequestStop(); });
                (void)prompt;
            },
            [](const Value& message, ScriptedAgent& agent) {
                if (Method(message) == "session/cancel") {
                    agent.Push(
                        R"({"jsonrpc":"2.0","id":3,"result":{"stopReason":"cancelled"}})");
                }
            });
        AcpChatSession session(FactoryFor(script), Config());
        live = &session;
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error),
              "a stopped turn is not a success");
        const auto cancels = WrittenWithMethod(script, "session/cancel");
        Check(cancels.size() == 1 &&
                  StringAt(cancels[0], {"params", "sessionId"}) == "S1" &&
                  cancels[0].Find("id") == nullptr,
              "one session/cancel notification for the session");
        Check(session.Transcript().back().kind == ChatEntryKind::Error &&
                  session.Transcript().back().text ==
                      "tour arrêté à la demande de l'utilisateur",
              "the same words as the built-in agent: " +
                  session.Transcript().back().text);
        Check(session.Transcript()[1].text == "Je commence",
              "what the agent said before stopping stays");

        script.answer = nullptr;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        Check(session.SubmitUserMessage("Reprends", error),
              "the next turn runs: " + error);
        Check(script.starts == 1, "in the same session");
    });

    Test("a stop asked before the turn begins is kept, then forgotten", [] {
        Script script;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(FactoryFor(script), Config());
        // Stop pressed right after Send, before the worker reaches the turn.
        session.RequestStop();
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error) &&
                  error == "tour arrêté à la demande de l'utilisateur",
              "the turn does not start: " + error);
        Check(script.starts == 0, "and no agent is started for it");
        Check(session.SubmitUserMessage("Monte", error),
              "the turn forgot the request when it ended: " + error);
        Check(script.starts == 1, "the next one starts the agent");

        // Between two turns of a running session.
        session.RequestStop();
        Check(!session.SubmitUserMessage("Encore", error),
              "stopped before its prompt");
        Check(WrittenWithMethod(script, "session/prompt").size() == 1,
              "which was never sent");
        session.RequestStop();
        session.ClearStopRequest();
        Check(session.SubmitUserMessage("Encore", error),
              "a request cleared before the turn does not stop it: " + error);
    });

    Test("an agent that dies mid-turn: its log, then a new conversation", [] {
        Script script;
        script.hint = "/tmp/agent.stderr.log";
        StandardAgent(script, [](const Value&, ScriptedAgent& agent) {
            agent.Update(
                R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Je"}})");
            // ...and nothing more: the output ends.
        });
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error), "the turn fails");
        const ChatTranscriptEntry& last = session.Transcript().back();
        Check(
            last.kind == ChatEntryKind::Error &&
                last.text.find("l'agent s'est arrêté") != std::string::npos &&
                last.text.find("/tmp/agent.stderr.log") != std::string::npos &&
                last.text.find("nouvelle conversation") != std::string::npos,
            "says so, where to look, and what happens next: " + last.text);
        Check(session.Transcript()[1].kind == ChatEntryKind::AssistantText,
              "what it said first is kept");

        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        Check(session.SubmitUserMessage("Encore", error),
              "the next instruction: " + error);
        Check(script.starts == 2 &&
                  WrittenWithMethod(script, "initialize").size() == 2,
              "starts a new agent");
        const auto prompts = WrittenWithMethod(script, "session/prompt");
        Check(prompts.back().Find("params")->Find("prompt")->AsArray().size() ==
                  2,
              "and gives it the preamble again");
    });

    // FI7, lines already read -- docs/roadmap/FIABILITE-2026-10.md. The
    // transport closed itself (a broken pipe, an undeliverable cancel) but
    // still hands over the answer it had received.
    Test(
        "an answer read after the transport closed: kept, then a new "
        "conversation",
        [] {
            Script script;
            StandardAgent(script, [&script](const Value& prompt,
                                            ScriptedAgent& agent) {
                agent.Update(
                    R"({"sessionUpdate":"agent_message_chunk","content":{"type":"text","text":"Fini"}})");
                agent.Then([&script] { script.closed = true; });
                agent.Respond(prompt, R"({"stopReason":"end_turn"})");
            });
            AcpChatSession session(FactoryFor(script), Config());
            std::string error;
            Check(!session.SubmitUserMessage("Monte", error),
                  "a turn whose agent is gone is not a plain success");
            Check(session.Usage().turns == 1,
                  "its answer is counted as a turn");
            Check(
                session.Transcript()[1].kind == ChatEntryKind::AssistantText &&
                    session.Transcript()[1].text == "Fini",
                "what the agent answered is kept");
            const ChatTranscriptEntry& last = session.Transcript().back();
            Check(last.kind == ChatEntryKind::Error &&
                      last.text.find("après sa réponse") != std::string::npos &&
                      last.text.find("nouvelle conversation") !=
                          std::string::npos,
                  "says the agent stopped and what happens next: " + last.text);

            StandardAgent(
                script, [](const Value& prompt, ScriptedAgent& agent) {
                    agent.Respond(prompt, R"({"stopReason":"end_turn"})");
                });
            Check(session.SubmitUserMessage("Encore", error),
                  "the next instruction runs instead of failing: " + error);
            Check(script.starts == 2, "in a new agent");
        });

    Test("an agent of another protocol version is refused", [] {
        Script script;
        script.answer = [](const Value& message, ScriptedAgent& agent) {
            if (Method(message) == "initialize") {
                agent.Respond(message, R"({"protocolVersion":2})");
            }
        };
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error), "refused");
        Check(error.find("version 2") != std::string::npos &&
                  WrittenWithMethod(script, "session/new").empty(),
              "before any session: " + error);
    });

    Test("an agent that is not logged in says how to log in", [] {
        Script script;
        script.answer = [](const Value& message, ScriptedAgent& agent) {
            if (Method(message) == "initialize") {
                agent.Respond(
                    message,
                    R"({"protocolVersion":1,"authMethods":[{"id":"claude-login","name":"Log in with Claude"}]})");
            } else if (Method(message) == "session/new") {
                agent.Push(
                    R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Authentication required"}})");
            }
        };
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error), "no session");
        Check(error.find("connecté") != std::string::npos &&
                  error.find("Log in with Claude") != std::string::npos &&
                  error.find("/login") != std::string::npos,
              "names the agent's own way to log in: " + error);
    });

    Test("files and terminals are the agent's own business", [] {
        Script script;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            agent.Ask(7, "fs/read_text_file",
                      R"({"sessionId":"S1","path":"/etc/passwd"})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(session.SubmitUserMessage("Lis", error), "the turn goes on");
        const Value* answer = AnswerTo(script, 7);
        Check(answer != nullptr && answer->Find("result") == nullptr &&
                  StringAt(*answer, {"error", "code"}) == "-32601",
              "Method not found");
    });

    Test("the plan: one line per change, marks by state", [] {
        Script script;
        StandardAgent(script, [](const Value& prompt, ScriptedAgent& agent) {
            const std::string first =
                R"({"sessionUpdate":"plan","entries":[{"content":"Lire","priority":"high","status":"pending"},{"content":"Couper","priority":"high","status":"pending"}]})";
            const std::string second =
                R"({"sessionUpdate":"plan","entries":[{"content":"Lire","priority":"high","status":"completed"},{"content":"Couper","priority":"high","status":"in_progress"}]})";
            agent.Update(first);
            agent.Update(second);
            agent.Update(second);
            agent.Update(
                R"({"sessionUpdate":"usage_update","used":1200,"size":200000})");
            agent.Respond(prompt, R"({"stopReason":"end_turn"})");
        });
        AcpChatSession session(FactoryFor(script), Config());
        std::string error;
        Check(session.SubmitUserMessage("Monte", error), "the turn");
        std::vector<std::string> plans;
        for (const auto& entry : session.Transcript()) {
            if (entry.kind == ChatEntryKind::Plan) {
                plans.push_back(entry.text);
            }
        }
        Check(plans.size() == 2, "an unchanged plan adds nothing");
        Check(!plans.empty() && plans.back() == "✓ Lire\n▸ Couper",
              "marks: " + (plans.empty() ? std::string() : plans.back()));
        Check(session.Usage().context_used == 1200 &&
                  session.Usage().context_size == 200000,
              "the context in use");
        Check(session.Usage().unreported_turns == 1,
              "a turn without usage is counted as such");
    });

    Test("a turn the agent could not finish is an error that says why", [] {
        for (const auto& [reason, words] :
             std::vector<std::pair<std::string, std::string>>{
                 {"max_turn_requests", "nombre maximal"},
                 {"refusal", "refusé"},
                 {"max_tokens", "limite de sortie"}}) {
            Script script;
            const std::string stop = reason;
            StandardAgent(script, [stop](const Value& prompt,
                                         ScriptedAgent& agent) {
                agent.Respond(prompt, R"({"stopReason":")" + stop + R"("})");
            });
            AcpChatSession session(FactoryFor(script), Config());
            std::string error;
            Check(!session.SubmitUserMessage("Monte", error), reason);
            Check(error.find(words) != std::string::npos,
                  reason + ": " + error);
        }
    });

    Test("stop while the agent starts closes it", [] {
        Script script;
        AcpChatSession* live = nullptr;
        script.answer = [&live](const Value& message, ScriptedAgent& agent) {
            if (Method(message) == "initialize") {
                agent.Then([&live] { live->RequestStop(); });
            }
        };
        AcpChatSession session(FactoryFor(script), Config());
        live = &session;
        std::string error;
        Check(!session.SubmitUserMessage("Monte", error), "no turn");
        Check(script.closed, "the agent was closed");
        Check(error == "tour arrêté à la demande de l'utilisateur",
              "and the stop is what the panel says: " + error);
    });

    Test("the login shell's environment, between its markers", [] {
        const std::string marker = "__M__";
        std::string output = "bienvenue\n" + marker;
        output += std::string("A=1") + '\0' + "PATH=/opt/homebrew/bin:/bin" +
                  '\0' + "EMPTY=" + '\0' + "EQ=a=b" + '\0';
        output += marker + "au revoir";
        acp::Environment environment;
        Check(acp::ParseShellEnvironment(output, marker, environment),
              "parsed");
        Check(
            environment == acp::Environment{{"A", "1"},
                                            {"PATH", "/opt/homebrew/bin:/bin"},
                                            {"EMPTY", ""},
                                            {"EQ", "a=b"}},
            "every variable, values kept whole");
        Check(!acp::ParseShellEnvironment("bienvenue\n" + marker + "A=1",
                                          marker, environment),
              "a missing end marker fails");

        acp::Environment nested = {{"CLAUDECODE", "1"},
                                   {"PATH", "/bin"},
                                   {"CLAUDE_CODE_ENTRYPOINT", "cli"}};
        acp::StripNestedAgentVariables(nested);
        Check(nested == acp::Environment{{"PATH", "/bin"}},
              "the nested-session variables go");
    });

    Test("the process transport: lines both ways, cwd, stderr, close", [] {
        const std::filesystem::path directory =
            std::filesystem::canonical(std::filesystem::temp_directory_path());
        const std::filesystem::path log =
            directory /
            ("acp-client-tests-" + std::to_string(::getpid()) + ".stderr.log");
        std::string error;

        acp::AcpProcessOptions echo;
        echo.command = "cat";
        echo.cwd = directory.string();
        echo.environment = acp::CurrentEnvironment();
        auto cat = acp::AcpProcessTransport::Start(echo, error);
        Check(cat != nullptr, "cat starts: " + error);
        if (cat) {
            std::string line;
            Check(cat->WriteLine(R"({"a":"é"})", error) &&
                      cat->ReadLine(line) && line == R"({"a":"é"})",
                  "a line comes back whole: " + line);
            // Longer than a pipe holds: written while it is read back.
            const std::string big(300000, 'x');
            bool wrote = false;
            std::string writeError;
            std::thread writer(
                [&] { wrote = cat->WriteLine(big, writeError); });
            const bool read = cat->ReadLine(line);
            writer.join();
            Check(wrote && read && line == big,
                  "a long line too (a picture in a tool result)");
            cat->Close();
            Check(!cat->ReadLine(line), "closed, the output ends");
            Check(!cat->WriteLine("x", error), "and nothing more is written");
        }

        acp::AcpProcessOptions where = echo;
        where.command = "pwd; echo 'sur stderr' >&2";
        where.stderr_path = log.string();
        auto pwd = acp::AcpProcessTransport::Start(where, error);
        Check(pwd != nullptr, "pwd starts: " + error);
        if (pwd) {
            std::string line;
            Check(pwd->ReadLine(line) && line == directory.string(),
                  "the agent runs in its cwd: " + line);
            Check(!pwd->ReadLine(line), "then its output ends");
            Check(pwd->DiagnosticHint() == log.string(), "the hint is the log");
            pwd.reset();
            Check(Slurp(log).find("sur stderr") != std::string::npos,
                  "stderr went to the log");
        }
        std::filesystem::remove(log);

        acp::AcpProcessOptions none = echo;
        none.command.clear();
        Check(acp::AcpProcessTransport::Start(none, error) == nullptr,
              "no command, no agent");
    });

    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. The agent deliberately
    // stops reading. Its finite sleep also bounds a failed regression.
    Test("a saturated agent pipe cannot block sending or closing", [] {
        acp::AcpProcessOptions options;
        options.command = "echo ready; exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the stalled agent starts: " + error);
        if (!transport) {
            return;
        }
        std::string line;
        Check(transport->ReadLine(line) && line == "ready", "it is ready");
        const auto start = std::chrono::steady_clock::now();
        Check(transport->WriteLine(std::string(size_t{2} * 1024 * 1024, 'x'),
                                   error),
              "the prompt is accepted for delivery");
        Check(transport->WriteLine("cancel", error),
              "a cancellation can follow the prompt");
        transport->Close();
        Check(!transport->ReadLine(line), "close interrupts reading");
        const auto elapsed = std::chrono::steady_clock::now() - start;
        Check(elapsed < std::chrono::seconds(2),
              "neither sending nor closing waited for the agent");
    });

    Test("close interrupts a reader even when SIGTERM is ignored", [] {
        acp::AcpProcessOptions options;
        options.command = "trap '' TERM; echo ready; exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the resistant agent starts: " + error);
        if (!transport) {
            return;
        }
        std::string line;
        Check(transport->ReadLine(line) && line == "ready", "the trap is set");
        bool read = true;
        std::thread reader([&] { read = transport->ReadLine(line); });
        const auto start = std::chrono::steady_clock::now();
        transport->Close();
        reader.join();
        Check(!read && std::chrono::steady_clock::now() - start <
                           std::chrono::seconds(1),
              "the reader stops before the process dies");
        transport.reset();
        Check(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(4),
            "destruction kills and reaps after the grace");
    });

    Test("an undeliverable cancellation closes the stalled agent", [] {
        acp::AcpProcessOptions options;
        options.command = "echo ready; exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error, line;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the stalled agent starts: " + error);
        if (!transport) {
            return;
        }
        Check(transport->ReadLine(line) && line == "ready", "it is ready");
        const auto start = std::chrono::steady_clock::now();
        transport->WriteLine(std::string(size_t{2} * 1024 * 1024, 'x'), error);
        Check(transport->WriteCancellation("cancel", error),
              "the cancellation is queued without waiting");
        Check(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "the panel can return immediately");
        Check(!transport->ReadLine(line),
              "delivery timeout interrupts the waiting turn");
        Check(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(4),
            "the agent was not waited for indefinitely");
    });

    Test(
        "queued cancellation follows a complete large line and clears its "
        "deadline",
        [] {
            acp::AcpProcessOptions options;
            options.command = "exec cat";
            options.environment = acp::CurrentEnvironment();
            std::string error, line;
            auto transport = acp::AcpProcessTransport::Start(options, error);
            Check(transport != nullptr,
                  "the responsive agent starts: " + error);
            if (!transport) {
                return;
            }
            const std::string big(size_t{2} * 1024 * 1024, 'x');
            Check(transport->WriteLine(big, error) &&
                      transport->WriteCancellation("cancel", error),
                  "both lines are accepted before reading either");
            Check(transport->ReadLine(line) && line == big,
                  "the large prompt is whole and comes first");
            Check(transport->ReadLine(line) && line == "cancel",
                  "the cancellation follows without interleaving");
            std::this_thread::sleep_for(std::chrono::milliseconds(2200));
            Check(transport->WriteLine("alive", error) &&
                      transport->ReadLine(line) && line == "alive",
                  "delivery cleared the deadline; the session stays reusable");
        });

    // FI7, lines already read -- docs/roadmap/FIABILITE-2026-10.md. Once
    // the session is closed, the complete lines the agent had already sent
    // are still the turn's: its final answer arrived just before the close.
    // Each agent stops reading and sleeps, which bounds a failed regression.
    Test("closing returns the complete lines already read, then ends", [] {
        acp::AcpProcessOptions options;
        // One write: both lines and the unterminated tail arrive together.
        options.command = "printf 'one\\ntwo\\nthr'; exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error, line;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the agent starts: " + error);
        if (!transport) {
            return;
        }
        Check(transport->ReadLine(line) && line == "one", "the first line");
        const auto start = std::chrono::steady_clock::now();
        transport->Close();
        Check(
            transport->ReadLine(line) && line == "two",
            "the line read with it is still returned after the close: " + line);
        Check(!transport->ReadLine(line),
              "then the output ends; the unterminated tail is dropped");
        Check(!transport->ReadLine(line), "and stays ended");
        Check(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(1),
            "without waiting for the agent");
    });

    // The writer closes the session by itself; this waits until a new line
    // is refused, which is how the turn's thread would find out.
    const auto waitForClose = [](acp::AcpProcessTransport& transport) {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        std::string error;
        while (std::chrono::steady_clock::now() < deadline) {
            if (!transport.WriteLine("probe", error)) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        return false;
    };

    Test("a final answer survives an undeliverable cancellation", [&] {
        acp::AcpProcessOptions options;
        options.command =
            "printf '{\"id\":3,\"result\":{\"stopReason\":\"end_turn\"}}\\n'; "
            "exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error, line;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the stalled agent starts: " + error);
        if (!transport) {
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        transport->WriteLine(std::string(size_t{2} * 1024 * 1024, 'x'), error);
        transport->WriteCancellation("cancel", error);
        Check(waitForClose(*transport),
              "the stuck cancellation closes the session");
        Check(transport->ReadLine(line) &&
                  line == R"({"id":3,"result":{"stopReason":"end_turn"}})",
              "the answer sent before the close is still read: " + line);
        Check(!transport->ReadLine(line), "then the output ends");
        Check(
            std::chrono::steady_clock::now() - start < std::chrono::seconds(5),
            "the agent was not waited for");
    });

    Test("a final answer survives a broken pipe", [&] {
        acp::AcpProcessOptions options;
        // The agent answers, then closes its input: the next write fails.
        options.command = "printf 'final\\n'; exec 0<&-; exec sleep 6";
        options.environment = acp::CurrentEnvironment();
        std::string error, line;
        auto transport = acp::AcpProcessTransport::Start(options, error);
        Check(transport != nullptr, "the agent starts: " + error);
        if (!transport) {
            return;
        }
        Check(waitForClose(*transport), "the broken pipe closes the session");
        Check(transport->ReadLine(line) && line == "final",
              "the answer sent before the break is still read: " + line);
        Check(!transport->ReadLine(line), "then the output ends");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All ACP client tests passed\n";
    return 0;
}
