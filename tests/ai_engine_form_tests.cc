// PRD22 -- ROADMAP.md. The "Moteur IA" dialog of the Agent panel as policy
// (AiEngineForm.h): which fields each protocol uses, what each one suggests
// when empty, what the fields hold when the dialog opens and when the
// protocol menu moves, and the ACP adapters pinned to a version. Plain C++,
// no AppKit: ChatPanelView.mm only applies what this decides, and the window
// test checks the real form against the same functions.

#include "AiEngineForm.h"
#include "ChatLlmClient.h"

#include <cctype>
#include <initializer_list>
#include <iostream>
#include <string>

namespace {

using ai_engine::EngineProtocol;
using ai_engine::FormContext;
using ai_engine::FormPolicy;
using ai_engine::FormTexts;

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
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

// "npx -y <package>@<major>.<minor>.<patch>", nothing after the version.
bool PinnedNpx(const std::string& command, const std::string& package) {
    const std::string prefix = "npx -y " + package + "@";
    if (command.compare(0, prefix.size(), prefix) != 0) {
        return false;
    }
    const std::string version = command.substr(prefix.size());
    int dots = 0;
    bool digit = false;
    for (const char c : version) {
        if (c == '.') {
            if (!digit) {
                return false;
            }
            ++dots;
            digit = false;
        } else if (std::isdigit(static_cast<unsigned char>(c)) != 0) {
            digit = true;
        } else {
            return false;
        }
    }
    return dots == 2 && digit;
}

FormTexts AllFilled() {
    FormTexts texts;
    texts.acp_command = "npx -y mon-agent";
    texts.model = "un-modele";
    texts.base_url = "https://proxy.example";
    texts.api_key = "une-cle";
    return texts;
}

bool SameTexts(const FormTexts& left, const FormTexts& right) {
    return left.acp_command == right.acp_command && left.model == right.model &&
           left.base_url == right.base_url && left.api_key == right.api_key;
}

FormContext KeysStored() {
    FormContext context;
    context.anthropic_key_stored = true;
    context.ollama_key_stored = true;
    return context;
}

}  // namespace

int main() {
    Test("the menu lists the three protocols in order", [] {
        Check(ai_engine::kEngineProtocols.size() == 3, "three protocols");
        for (size_t index = 0; index < ai_engine::kEngineProtocols.size();
             ++index) {
            const EngineProtocol protocol = ai_engine::kEngineProtocols[index];
            Check(ai_engine::MenuIndexOf(protocol) == static_cast<int>(index),
                  "a protocol's menu index is its place in the list");
            Check(ai_engine::ProtocolAtMenuIndex(static_cast<long>(index)) ==
                      protocol,
                  "a menu index reads back as its protocol");
        }
        Check(std::string(
                  ai_engine::MenuTitle(EngineProtocol::AnthropicMessages)) ==
                      "Anthropic Messages API" &&
                  std::string(ai_engine::MenuTitle(EngineProtocol::Ollama)) ==
                      "Ollama local" &&
                  std::string(ai_engine::MenuTitle(EngineProtocol::Acp)) ==
                      "Agent de code (ACP)",
              "the menu keeps its three titles");
        Check(ai_engine::ProtocolAtMenuIndex(-1) ==
                      EngineProtocol::AnthropicMessages &&
                  ai_engine::ProtocolAtMenuIndex(3) ==
                      EngineProtocol::AnthropicMessages,
              "no selection, or an index past the menu, is its first item");
    });

    Test("a stored provider reads as the panel has always read it", [] {
        Check(ai_engine::ProtocolOfStoredProvider("acp") == EngineProtocol::Acp,
              "\"acp\" is the coding agent");
        Check(ai_engine::ProtocolOfStoredProvider("ollama") ==
                      EngineProtocol::Ollama &&
                  ai_engine::ProtocolOfStoredProvider("openai-compatible") ==
                      EngineProtocol::Ollama,
              "\"ollama\" and the older \"openai-compatible\" are Ollama");
        Check(ai_engine::ProtocolOfStoredProvider("anthropic-messages") ==
                      EngineProtocol::AnthropicMessages &&
                  ai_engine::ProtocolOfStoredProvider("") ==
                      EngineProtocol::AnthropicMessages &&
                  ai_engine::ProtocolOfStoredProvider("autre") ==
                      EngineProtocol::AnthropicMessages,
              "anything else, nothing stored included, is Anthropic");
    });

    Test("Anthropic uses the model, the URL and the key, not the command", [] {
        const FormPolicy policy =
            ai_engine::PolicyFor(EngineProtocol::AnthropicMessages);
        Check(!policy.acp_command.enabled, "the command is greyed");
        Check(policy.model.enabled &&
                  policy.model.placeholder == chat::kDefaultAnthropicModel,
              "the model suggests the default model, got " +
                  policy.model.placeholder);
        Check(policy.model.placeholder == "claude-opus-5-5",
              "which is claude-opus-5-5");
        // CUTMACHINE_ANTHROPIC_URL and a stored URL are honoured, so the
        // field stays, suggesting the public endpoint.
        Check(policy.base_url.enabled &&
                  policy.base_url.placeholder == "https://api.anthropic.com",
              "the URL suggests api.anthropic.com");
        Check(policy.api_key.enabled &&
                  policy.api_key.placeholder == "Requise avec Anthropic",
              "without a key, the key field says it is required");
        Check(policy.remove_key_enabled, "a stored key can be deleted");
        Check(ai_engine::PolicyFor(EngineProtocol::AnthropicMessages,
                                   KeysStored())
                      .api_key.placeholder == ai_engine::kKeepStoredKey,
              "with a key, an empty field keeps it");
    });

    Test("an empty Anthropic field suggests what the environment names", [] {
        // CUTMACHINE_ANTHROPIC_MODEL (or CUTMACHINE_MODEL) and
        // CUTMACHINE_ANTHROPIC_URL win over the built-in defaults when
        // nothing is stored: the placeholder says what an empty field runs.
        FormContext context;
        context.anthropic_model = "claude-sonnet-4-6";
        context.anthropic_base_url = "https://proxy.example";
        const FormPolicy policy =
            ai_engine::PolicyFor(EngineProtocol::AnthropicMessages, context);
        Check(policy.model.placeholder == "claude-sonnet-4-6" &&
                  policy.base_url.placeholder == "https://proxy.example",
              "the environment's model and URL are suggested");
        const FormPolicy ollama =
            ai_engine::PolicyFor(EngineProtocol::Ollama, context);
        Check(ollama.model.placeholder == chat::kDefaultOllamaModel &&
                  ollama.base_url.placeholder == chat::kDefaultOllamaBaseUrl,
              "Ollama's suggestions do not follow Anthropic's environment");
        FormContext blank;
        blank.anthropic_model.clear();
        blank.anthropic_base_url.clear();
        const FormPolicy fallback =
            ai_engine::PolicyFor(EngineProtocol::AnthropicMessages, blank);
        Check(
            fallback.model.placeholder == chat::kDefaultAnthropicModel &&
                fallback.base_url.placeholder == chat::kDefaultAnthropicBaseUrl,
            "an empty context falls back on the built-in defaults");
    });

    Test("Ollama suggests its local model and server, its key is optional", [] {
        const FormPolicy policy = ai_engine::PolicyFor(EngineProtocol::Ollama);
        Check(!policy.acp_command.enabled, "the command is greyed");
        Check(policy.model.enabled &&
                  policy.model.placeholder == "qwen2.5-coder:7b" &&
                  policy.model.placeholder == chat::kDefaultOllamaModel,
              "the model suggests qwen2.5-coder:7b");
        Check(policy.base_url.enabled &&
                  policy.base_url.placeholder == "http://localhost:11434" &&
                  policy.base_url.placeholder == chat::kDefaultOllamaBaseUrl,
              "the URL suggests the local server");
        Check(policy.api_key.enabled &&
                  policy.api_key.placeholder == "Facultative avec Ollama local",
              "the key is optional");
        Check(ai_engine::PolicyFor(EngineProtocol::Ollama, KeysStored())
                      .api_key.placeholder == ai_engine::kKeepStoredKey,
              "with a key, an empty field keeps it");
    });

    Test("the coding agent uses its command alone", [] {
        const FormPolicy policy =
            ai_engine::PolicyFor(EngineProtocol::Acp, KeysStored());
        Check(policy.acp_command.enabled && policy.acp_command.placeholder ==
                                                ai_engine::kDefaultAcpCommand,
              "the command suggests the pinned Claude adapter");
        Check(!policy.model.enabled && !policy.base_url.enabled &&
                  !policy.api_key.enabled && !policy.remove_key_enabled,
              "model, URL, key and deletion are greyed");
        Check(!policy.model.placeholder.empty() &&
                  !policy.base_url.placeholder.empty() &&
                  !policy.api_key.placeholder.empty(),
              "each greyed field says why it is greyed");
    });

    Test("every field is used by exactly the protocols the ticket names", [] {
        // PRD22's table: Anthropic -- model, URL, key; Ollama -- model,
        // URL, key; ACP -- the command alone.
        for (const EngineProtocol protocol : ai_engine::kEngineProtocols) {
            for (const FormContext& context : {FormContext{}, KeysStored()}) {
                const FormPolicy policy =
                    ai_engine::PolicyFor(protocol, context);
                const bool agent = protocol == EngineProtocol::Acp;
                Check(policy.acp_command.enabled == agent &&
                          policy.model.enabled == !agent &&
                          policy.base_url.enabled == !agent &&
                          policy.api_key.enabled == !agent &&
                          policy.remove_key_enabled == !agent,
                      std::string("the fields of ") +
                          ai_engine::MenuTitle(protocol));
                Check(!policy.acp_command.placeholder.empty() &&
                          !policy.model.placeholder.empty() &&
                          !policy.base_url.placeholder.empty() &&
                          !policy.api_key.placeholder.empty(),
                      std::string("every field of ") +
                          ai_engine::MenuTitle(protocol) +
                          " has a placeholder");
            }
        }
        Check(ai_engine::PolicyFor(EngineProtocol::Ollama).model.placeholder !=
                  ai_engine::PolicyFor(EngineProtocol::AnthropicMessages)
                      .model.placeholder,
              "the model field no longer suggests an Ollama model to "
              "Anthropic");
    });

    Test("the ACP adapters are pinned to a version", [] {
        Check(PinnedNpx(ai_engine::kDefaultAcpCommand,
                        "@agentclientprotocol/claude-agent-acp"),
              std::string("the default command names a version, got ") +
                  ai_engine::kDefaultAcpCommand);
        Check(PinnedNpx(ai_engine::kCodexAcpCommandExample,
                        "@agentclientprotocol/codex-acp"),
              std::string("the Codex example names a version, got ") +
                  ai_engine::kCodexAcpCommandExample);
        Check(!PinnedNpx("npx -y @agentclientprotocol/claude-agent-acp",
                         "@agentclientprotocol/claude-agent-acp") &&
                  !PinnedNpx("npx -y @agentclientprotocol/claude-agent-acp@"
                             "latest",
                             "@agentclientprotocol/claude-agent-acp") &&
                  !PinnedNpx("npx -y @agentclientprotocol/claude-agent-acp@1.2",
                             "@agentclientprotocol/claude-agent-acp"),
              "an unversioned, tagged or partial version is not a pin");
    });

    Test("a field the protocol does not use holds nothing", [] {
        const FormTexts agent =
            ai_engine::OnlyUsedBy(EngineProtocol::Acp, AllFilled());
        Check(agent.acp_command == "npx -y mon-agent" && agent.model.empty() &&
                  agent.base_url.empty() && agent.api_key.empty(),
              "the agent keeps its command only");
        const FormTexts anthropic = ai_engine::OnlyUsedBy(
            EngineProtocol::AnthropicMessages, AllFilled());
        Check(anthropic.acp_command.empty() && anthropic.model == "un-modele" &&
                  anthropic.base_url == "https://proxy.example" &&
                  anthropic.api_key == "une-cle",
              "Anthropic keeps all but the command");
    });

    Test("the dialog opens with what is stored, nothing for the rest", [] {
        // Anthropic chosen, a model stored, no URL: the model in its field,
        // the URL field empty so that its default shows.
        const ai_engine::FormDrafts anthropic = ai_engine::DraftsFromStored(
            EngineProtocol::AnthropicMessages, "claude-sonnet-4-5", "",
            "npx -y autre-agent");
        FormTexts expected;
        expected.model = "claude-sonnet-4-5";
        Check(SameTexts(anthropic.Shown(EngineProtocol::AnthropicMessages),
                        expected),
              "Anthropic shows the stored model and an empty URL");
        Check(SameTexts(anthropic.Shown(EngineProtocol::Ollama), FormTexts{}),
              "Ollama, not chosen, shows nothing");
        FormTexts command;
        command.acp_command = "npx -y autre-agent";
        Check(SameTexts(anthropic.Shown(EngineProtocol::Acp), command),
              "the agent shows the stored command");

        // Nothing stored at all: every field empty, every default in grey.
        const ai_engine::FormDrafts bare = ai_engine::DraftsFromStored(
            EngineProtocol::AnthropicMessages, "", "", "");
        for (const EngineProtocol protocol : ai_engine::kEngineProtocols) {
            Check(SameTexts(bare.Shown(protocol), FormTexts{}),
                  std::string("nothing stored, ") +
                      ai_engine::MenuTitle(protocol) + " shows nothing");
        }

        // Ollama chosen through an OpenAI-compatible URL: shown as the
        // engine reads it.
        const ai_engine::FormDrafts ollama =
            ai_engine::DraftsFromStored(EngineProtocol::Ollama, "llama3.2",
                                        "http://localhost:11434/v1", "");
        FormTexts local;
        local.model = "llama3.2";
        local.base_url = "http://localhost:11434";
        Check(SameTexts(ollama.Shown(EngineProtocol::Ollama), local),
              "Ollama's URL loses the /v1 the engine drops");
        const ai_engine::FormDrafts proxy =
            ai_engine::DraftsFromStored(EngineProtocol::AnthropicMessages, "",
                                        "https://proxy.example/v1", "");
        Check(proxy.Shown(EngineProtocol::AnthropicMessages).base_url ==
                  "https://proxy.example/v1",
              "an Anthropic URL is shown as stored");

        // The coding agent chosen: a model and a URL left by an engine no
        // longer chosen show nowhere.
        const ai_engine::FormDrafts agent =
            ai_engine::DraftsFromStored(EngineProtocol::Acp, "claude-opus-5",
                                        "https://leftover.example", "");
        for (const EngineProtocol protocol : ai_engine::kEngineProtocols) {
            Check(SameTexts(agent.Shown(protocol), FormTexts{}),
                  std::string("with the agent chosen and no command, ") +
                      ai_engine::MenuTitle(protocol) + " shows nothing");
        }
    });

    Test("the menu brings back what each protocol's fields held", [] {
        ai_engine::FormDrafts drafts = ai_engine::DraftsFromStored(
            EngineProtocol::AnthropicMessages, "claude-sonnet-4-5",
            "https://api.anthropic.com",
            "npx -y @agentclientprotocol/codex-acp@2.1.1");
        FormTexts inUse;
        inUse.model = "claude-sonnet-4-5";
        inUse.base_url = "https://api.anthropic.com";
        FormTexts agent;
        agent.acp_command = "npx -y @agentclientprotocol/codex-acp@2.1.1";
        Check(SameTexts(drafts.Shown(EngineProtocol::AnthropicMessages), inUse),
              "the engine in use opens filled in");

        // To Ollama, never filled in: empty, so its defaults show.
        FormTexts shown = drafts.Move(EngineProtocol::AnthropicMessages, inUse,
                                      EngineProtocol::Ollama);
        Check(SameTexts(shown, FormTexts{}),
              "a protocol not in use opens empty");
        shown.model = "llama3.2";
        shown.api_key = "cle-ollama";

        // To the agent: its command, nothing else.
        const FormTexts onAgent =
            drafts.Move(EngineProtocol::Ollama, shown, EngineProtocol::Acp);
        Check(SameTexts(onAgent, agent), "the agent shows its stored command");

        // Back to Anthropic, then to Ollama: each finds its texts.
        FormTexts typedAgent = onAgent;
        typedAgent.acp_command = "npx -y autre-agent";
        Check(SameTexts(drafts.Move(EngineProtocol::Acp, typedAgent,
                                    EngineProtocol::AnthropicMessages),
                        inUse),
              "Anthropic finds its model and URL again");
        FormTexts expectedOllama;
        expectedOllama.model = "llama3.2";
        expectedOllama.api_key = "cle-ollama";
        Check(SameTexts(drafts.Move(EngineProtocol::AnthropicMessages, inUse,
                                    EngineProtocol::Ollama),
                        expectedOllama),
              "Ollama finds what was typed for it, key included");
        FormTexts expectedAgent;
        expectedAgent.acp_command = "npx -y autre-agent";
        Check(SameTexts(drafts.Shown(EngineProtocol::Acp), expectedAgent),
              "the edited command was kept too");
    });

    Test("a draft never carries a field its protocol does not use", [] {
        ai_engine::FormDrafts drafts;
        drafts.Set(EngineProtocol::Acp, AllFilled());
        drafts.Set(EngineProtocol::Ollama, AllFilled());
        const FormTexts agent = drafts.Shown(EngineProtocol::Acp);
        Check(agent.model.empty() && agent.base_url.empty() &&
                  agent.api_key.empty() && !agent.acp_command.empty(),
              "the agent's draft holds its command only");
        const FormTexts ollama = drafts.Shown(EngineProtocol::Ollama);
        Check(ollama.acp_command.empty() && !ollama.model.empty(),
              "Ollama's draft holds no command");
        // A move from the agent shows the next protocol's fields only.
        const FormTexts moved = drafts.Move(EngineProtocol::Acp, AllFilled(),
                                            EngineProtocol::AnthropicMessages);
        Check(SameTexts(moved, FormTexts{}),
              "Anthropic, never filled in, is still empty after the agent");
    });

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All AI engine form tests passed\n";
    return 0;
}
