#pragma once

// PRD22 -- ROADMAP.md. The "Moteur IA" dialog of the Agent panel is one form
// for three protocols, and every field of it used to stay editable whatever
// the protocol -- the Model field suggesting an Ollama model while Anthropic
// was chosen. Which fields a protocol uses, what each suggests when empty,
// what the fields hold when the dialog opens and when the protocol menu
// moves are decided here, in plain C++ (tests/ai_engine_form_tests.cc).
// ChatPanelView.mm applies it, at opening and on every move of the menu, and
// the window test checks the dialog against it.
//
// A field the chosen protocol does not use is disabled and shown empty, its
// placeholder saying why; what it held comes back with its protocol. An
// empty Model or Base URL field stands for what the engine takes when
// nothing is stored -- the placeholder shows it -- and the dialog stores
// nothing for it, so a later change of default reaches whoever never chose
// one, as an empty ACP command already did. The dialog used to open with the
// model in use written in the field, and saving it stored that model: a
// person who saved it before PRD22 keeps the model then in use until the
// field is emptied.

#include "ChatLlmClient.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace ai_engine {

// The ACP adapters, pinned to a version: `npx -y <package>` without one runs
// whatever was published last, at every launch of the agent. Both were the
// latest release on 3 October 2026 (`npm view`); the handshake was last
// played against claude-agent-acp 0.81.2 and codex-acp 1.13.1
// (docs/roadmap/ACP-2026-09.md), not against these. To move one: `npm view
// @agentclientprotocol/claude-agent-acp version` (or codex-acp) gives the
// latest, its release notes say what changed; play the "Agent de code (ACP)"
// section of VISUAL_QA_CHECKLIST.md with the new command, then change it
// here. A command the person stored in the dialog is never rewritten.
inline constexpr char kDefaultAcpCommand[] =
    "npx -y @agentclientprotocol/claude-agent-acp@0.85.1";
// The Codex example the dialog's help text gives.
inline constexpr char kCodexAcpCommandExample[] =
    "npx -y @agentclientprotocol/codex-acp@2.1.1";

// The protocol menu's items, in menu order.
enum class EngineProtocol : std::uint8_t { AnthropicMessages, Ollama, Acp };

inline constexpr std::array<EngineProtocol, 3> kEngineProtocols = {
    EngineProtocol::AnthropicMessages, EngineProtocol::Ollama,
    EngineProtocol::Acp};

inline int MenuIndexOf(EngineProtocol protocol) {
    return static_cast<int>(protocol);
}

// What the menu's selected index stands for. No selection (-1) or an index
// past the menu reads as its first item, the dialog's default.
inline EngineProtocol ProtocolAtMenuIndex(long index) {
    if (index < 0 || index >= static_cast<long>(kEngineProtocols.size())) {
        return EngineProtocol::AnthropicMessages;
    }
    return kEngineProtocols[static_cast<size_t>(index)];
}

inline const char* MenuTitle(EngineProtocol protocol) {
    switch (protocol) {
        case EngineProtocol::AnthropicMessages:
            return "Anthropic Messages API";
        case EngineProtocol::Ollama:
            return "Ollama local";
        case EngineProtocol::Acp:
            return "Agent de code (ACP)";
    }
    return "";
}

// The protocol a stored AiEngine.Provider.v1 value selects: "acp" the coding
// agent, "ollama" or the older "openai-compatible" Ollama, anything else --
// nothing stored included -- Anthropic. The panel reads the preference with
// this, for the engine it builds and for the dialog.
inline EngineProtocol ProtocolOfStoredProvider(const std::string& stored) {
    if (stored == "acp") {
        return EngineProtocol::Acp;
    }
    if (stored == "ollama" || stored == "openai-compatible") {
        return EngineProtocol::Ollama;
    }
    return EngineProtocol::AnthropicMessages;
}

// One text field of the form: whether the protocol uses it, and the text it
// shows while empty.
struct FieldPolicy {
    bool enabled = false;
    std::string placeholder;
};

struct FormPolicy {
    FieldPolicy acp_command;
    FieldPolicy model;
    FieldPolicy base_url;
    FieldPolicy api_key;
    // The "Supprimer la clé enregistrée" checkbox.
    bool remove_key_enabled = false;
};

// What the dialog knows of this Mac when it opens.
struct FormContext {
    // What an empty Model or Base URL field stands for with Anthropic: the
    // engine's choice when nothing is stored, the environment's included
    // (ChatLlmConfig::FromEnvironment: CUTMACHINE_ANTHROPIC_MODEL, then
    // CUTMACHINE_MODEL; CUTMACHINE_ANTHROPIC_URL). Ollama's never vary.
    std::string anthropic_model = chat::kDefaultAnthropicModel;
    std::string anthropic_base_url = chat::kDefaultAnthropicBaseUrl;
    // A key already in effect -- in the Keychain, or ANTHROPIC_API_KEY for
    // Anthropic -- which an empty Key field keeps. Only the Key field's
    // placeholder depends on it.
    bool anthropic_key_stored = false;
    bool ollama_key_stored = false;
};

inline constexpr char kKeepStoredKey[] = "Laisser vide pour conserver la clé";

inline FormPolicy PolicyFor(EngineProtocol protocol,
                            const FormContext& context = {}) {
    FormPolicy policy;
    switch (protocol) {
        case EngineProtocol::AnthropicMessages:
            policy.acp_command = {false, "Réservé à l'agent de code (ACP)"};
            policy.model = {true, context.anthropic_model.empty()
                                      ? chat::kDefaultAnthropicModel
                                      : context.anthropic_model};
            // ChatLlmConfig honours another Anthropic endpoint (a proxy,
            // CUTMACHINE_ANTHROPIC_URL, a stored URL), so the field stays.
            policy.base_url = {true, context.anthropic_base_url.empty()
                                         ? chat::kDefaultAnthropicBaseUrl
                                         : context.anthropic_base_url};
            policy.api_key = {true, context.anthropic_key_stored
                                        ? kKeepStoredKey
                                        : "Requise avec Anthropic"};
            policy.remove_key_enabled = true;
            break;
        case EngineProtocol::Ollama:
            policy.acp_command = {false, "Réservé à l'agent de code (ACP)"};
            policy.model = {true, chat::kDefaultOllamaModel};
            policy.base_url = {true, chat::kDefaultOllamaBaseUrl};
            // Sent as a bearer token when given (ChatLlmClient): a local
            // server needs none, one behind a proxy or ollama.com does.
            policy.api_key = {true, context.ollama_key_stored
                                        ? kKeepStoredKey
                                        : "Facultative avec Ollama local"};
            policy.remove_key_enabled = true;
            break;
        case EngineProtocol::Acp:
            // The agent logs in with its own account and picks its own
            // model; the command is all CUTMACHINE knows of it.
            policy.acp_command = {true, kDefaultAcpCommand};
            policy.model = {false, "Choisi par l'agent de code"};
            policy.base_url = {false, "Sans objet pour l'agent de code"};
            policy.api_key = {false, "L'agent utilise son propre compte"};
            policy.remove_key_enabled = false;
            break;
    }
    return policy;
}

// The text of each text field.
struct FormTexts {
    std::string acp_command;
    std::string model;
    std::string base_url;
    std::string api_key;
};

// `texts` with every field `protocol` does not use emptied.
inline FormTexts OnlyUsedBy(EngineProtocol protocol, FormTexts texts) {
    const FormPolicy policy = PolicyFor(protocol);
    if (!policy.acp_command.enabled) {
        texts.acp_command.clear();
    }
    if (!policy.model.enabled) {
        texts.model.clear();
    }
    if (!policy.base_url.enabled) {
        texts.base_url.clear();
    }
    if (!policy.api_key.enabled) {
        texts.api_key.clear();
    }
    return texts;
}

// What each protocol's fields hold while the dialog is open. The dialog
// opens with what is stored (DraftsFromStored); moving the menu keeps what
// the fields held for the protocol left and brings back what the next one
// held -- nothing, the first time, so its placeholders show its defaults.
class FormDrafts {
public:
    void Set(EngineProtocol protocol, const FormTexts& texts) {
        drafts_[Slot(protocol)] = OnlyUsedBy(protocol, texts);
    }

    // What the fields show for `protocol`.
    FormTexts Shown(EngineProtocol protocol) const {
        return drafts_[Slot(protocol)];
    }

    // The fields held `shown` for `from`; returns what they show for `to`.
    FormTexts Move(EngineProtocol from, const FormTexts& shown,
                   EngineProtocol to) {
        Set(from, shown);
        return Shown(to);
    }

private:
    static size_t Slot(EngineProtocol protocol) {
        return static_cast<size_t>(MenuIndexOf(protocol));
    }

    std::array<FormTexts, 3> drafts_{};
};

// What the fields hold when the dialog opens, from the stored preferences
// (AiEngine.Model.v1, .BaseUrl.v1, .AcpCommand.v1): the model and the URL
// belong to the stored protocol -- with the coding agent chosen they are what
// an engine no longer chosen left, and no field shows them -- and the
// command to the agent. Ollama's URL is shown as the engine reads it, without
// the "/v1" an OpenAI-compatible setting ended with. No key is ever shown.
inline FormDrafts DraftsFromStored(EngineProtocol storedProtocol,
                                   const std::string& model,
                                   std::string baseUrl,
                                   const std::string& acpCommand) {
    FormDrafts drafts;
    if (storedProtocol != EngineProtocol::Acp) {
        const std::string suffix = "/v1";
        if (storedProtocol == EngineProtocol::Ollama &&
            baseUrl.size() >= suffix.size() &&
            baseUrl.compare(baseUrl.size() - suffix.size(), suffix.size(),
                            suffix) == 0) {
            baseUrl.resize(baseUrl.size() - suffix.size());
        }
        FormTexts endpoint;
        endpoint.model = model;
        endpoint.base_url = baseUrl;
        drafts.Set(storedProtocol, endpoint);
    }
    FormTexts agent;
    agent.acp_command = acpCommand;
    drafts.Set(EngineProtocol::Acp, agent);
    return drafts;
}

}  // namespace ai_engine
