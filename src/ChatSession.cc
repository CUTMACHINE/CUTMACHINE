#include "ChatSession.h"

#include "ChatPresentation.h"
#include "Generation.h"

#include <set>
#include <utility>

namespace chat {

namespace {

// A runaway agentic loop (model keeps requesting tools and never settles)
// is a bug in the model or the prompt, not a normal outcome -- stop and
// surface it rather than let the chat panel hang or burn API budget
// indefinitely. Sized generously above what any single CUTMACHINE
// instruction should need (a `describe` call plus a small handful of
// edits), the same spirit as the sidecar's own two-attempt retry cap in
// sidecar/repl.py, just wider because this loop's tool catalog is the full
// F1.2 surface rather than one structured plan.
constexpr int kMaxToolIterations = 8;

void AddUsage(const ChatUsage& usage, ChatUsageTotals& totals) {
    ++totals.calls;
    if (!usage.reported) {
        ++totals.unreported_calls;
        return;
    }
    totals.input_tokens += usage.input_tokens;
    totals.cache_write_tokens += usage.cache_write_tokens;
    totals.cache_read_tokens += usage.cache_read_tokens;
    totals.output_tokens += usage.output_tokens;
}

void CopyField(const mcp_json::Value& source, const std::string& key,
               mcp_json::Value& destination) {
    const mcp_json::Value* value = source.Find(key);
    if (value) destination.Set(key, *value);
}

std::string CompactDescribeForOllama(const std::string& json) {
    mcp_json::Value root;
    std::string parseError;
    if (!mcp_json::Value::Parse(json, root, parseError) || !root.IsObject())
        return json;
    const mcp_json::Value* timeline = root.Find("timeline");
    if (!timeline || !timeline->IsObject()) return json;

    std::set<std::string> usedSourceIds;
    const mcp_json::Value* tracks = timeline->Find("tracks");
    if (tracks && tracks->IsArray()) {
        for (const mcp_json::Value& track : tracks->AsArray()) {
            const mcp_json::Value* items = track.Find("items");
            if (!items || !items->IsArray()) continue;
            for (const mcp_json::Value& item : items->AsArray()) {
                const mcp_json::Value* sourceId = item.Find("source_id");
                if (sourceId && sourceId->IsString())
                    usedSourceIds.insert(sourceId->AsString());
            }
        }
    }

    mcp_json::Value compactTimeline = mcp_json::Value::MakeObject();
    CopyField(*timeline, "duration", compactTimeline);
    CopyField(*timeline, "tracks", compactTimeline);
    mcp_json::Value usedSources = mcp_json::Value::MakeArray();
    const mcp_json::Value* sources = timeline->Find("sources");
    if (sources && sources->IsArray()) {
        for (const mcp_json::Value& source : sources->AsArray()) {
            const mcp_json::Value* id = source.Find("id");
            if (id && id->IsString() && usedSourceIds.count(id->AsString()))
                usedSources.Push(source);
        }
    }
    compactTimeline.Set("sources", std::move(usedSources));

    mcp_json::Value compact = mcp_json::Value::MakeObject();
    CopyField(root, "sequence", compact);
    compact.Set("timeline", std::move(compactTimeline));
    const mcp_json::Value* library = root.Find("library");
    if (library && library->IsArray()) {
        mcp_json::Value compactLibrary = mcp_json::Value::MakeArray();
        for (const mcp_json::Value& item : library->AsArray()) {
            mcp_json::Value summary = mcp_json::Value::MakeObject();
            CopyField(item, "alias", summary);
            CopyField(item, "id", summary);
            CopyField(item, "filename", summary);
            CopyField(item, "bin_id", summary);
            CopyField(item, "in_use", summary);
            compactLibrary.Push(std::move(summary));
        }
        compact.Set("library", std::move(compactLibrary));
    }
    CopyField(root, "bins", compact);
    CopyField(root, "markers", compact);
    const std::string result = compact.Dump();
    return result.size() < json.size() ? result : json;
}

}  // namespace

// PL1 -- ROADMAP.md. See ChatSession.h.
std::string TurnLabel(const std::string& instruction) {
    constexpr size_t kMaxBytes = 120;
    std::string line;
    for (const char character : instruction) {
        const bool space = character == ' ' || character == '\n' ||
                           character == '\r' || character == '\t';
        if (space) {
            if (!line.empty() && line.back() != ' ') {
                line.push_back(' ');
            }
        } else {
            line.push_back(character);
        }
    }
    while (!line.empty() && line.back() == ' ') {
        line.pop_back();
    }
    if (line.size() > kMaxBytes) {
        size_t cut = kMaxBytes;
        while (cut > 0 &&
               (static_cast<unsigned char>(line[cut]) & 0xc0) == 0x80) {
            --cut;
        }
        line = line.substr(0, cut) + "…";
    }
    return "Agent : " +
           (line.empty() ? std::string("tour sans consigne") : line);
}

TurnIntervention::TurnIntervention(McpBackend& backend, std::string label)
    : backend_(backend), label_(std::move(label)) {
    std::string result;
    std::string errorName;
    std::string message;
    if (!backend_.BeginIntervention(result, errorName, message)) {
        return;
    }
    mcp_json::Value parsed;
    std::string parseError;
    if (!mcp_json::Value::Parse(result, parsed, parseError)) {
        return;
    }
    const mcp_json::Value* token = parsed.Find("intervention");
    if (token != nullptr && token->IsString()) {
        token_ = token->AsString();
    }
}

TurnIntervention::~TurnIntervention() {
    if (token_.empty()) {
        return;
    }
    std::string result;
    std::string errorName;
    std::string message;
    backend_.EndIntervention(token_, label_, result, errorName, message);
}

const std::string kDefaultSystemPrompt =
    std::string(
        "Tu es l'agent de montage intégré à CUTMACHINE, un éditeur vidéo "
        "déterministe. Tu ne modifies jamais le montage directement : chaque "
        "changement passe par un appel d'outil, qui applique une opération "
        "réversible et journalisée. N'invente jamais un identifiant ; commence "
        "par l'outil `describe` pour observer l'état actuel de la timeline "
        "(pistes, clips, marqueurs, médiathèque) si tu ne l'as pas déjà fait "
        "dans cette conversation ou si la demande porte sur un état que tu "
        "n'as pas encore vu. Les identifiants complets ou tout préfixe non "
        "ambigu sont acceptés par les outils qui prennent un ID ; les aliases "
        "visibles comme A1, T1, M1 et K1 le sont aussi. Préfère les outils "
        "d'intention déterministes comme `shorten_linked_clip` aux opérations "
        "brutes lorsqu'ils correspondent exactement à la demande.\n"
        // PRD18 -- ROADMAP.md (ACP4). The procedures the engine carries
        // (Procedures.h): the panel's agent never had the repository's skills.
        "Avant de monter une interview, de faire du motion design ou de monter "
        "un reel ou une pub, lis la procédure qui s'y rapporte avec "
        "`get_procedure` : sans argument la liste, avec `name` la procédure et "
        "ses fichiers, avec `file` un fichier qu'elle cite.\n"
        "Pour monter une interview depuis sa transcription, appelle "
        "`list_speech_blocks` (des phrases entières) ou "
        "`get_timeline_transcript` (des cartons de sous-titres), puis désigne "
        "les blocs ou les spans retenus par leur `block_id` ou leur `span_id` "
        "dans `create_interview_short`, avec la `transcript_revision` lue en "
        "même temps : ces identifiants sont des positions dans une lecture, et "
        "une lecture périmée est refusée (StaleSelection) ; relis alors la "
        "transcription. Ne recopie jamais un temps : CUTMACHINE résout "
        "lui-même "
        "chaque identifiant en position exacte, ce qui garantit qu'une coupe "
        "ne "
        "tombe pas au milieu d'un mot. Quand une idée couvre plusieurs blocs "
        "ou "
        "spans consécutifs, donne aussi `end_block_id` ou `end_span_id` : le "
        "moteur les fusionne en une seule plage. Avant de construire, "
        "`inspect_speech_selection` montre ce que chaque morceau laisse de "
        "côté "
        "juste avant et juste après : un « également » ou un « ça » dont "
        "l'antécédent n'est pas gardé est une faute de montage. Cet outil crée "
        "une nouvelle timeline et préserve l'originale.\n"
        "Pour retirer les hésitations d'un plan, appelle `clean_disfluencies` "
        "plutôt que d'énumérer des mots toi-même : la détection est "
        "déterministe et la coupe tient en une seule opération réversible. "
        "Utilise `list_disfluencies` seulement quand l'utilisateur veut "
        "valider la liste avant de couper. Les répétitions ne sont retirées "
        "que sur demande explicite, parce qu'un mot répété peut être une "
        "insistance voulue.\n"
        "Avant de placer une illustration ou un plan de coupe, appelle "
        "`list_shot_quality` et écarte ce qui est `needs_attention`. Ces notes "
        "sont mesurées sur les images, pas jugées : ne les contredis pas. Un "
        "plan listé sous `unanalyzed` n'est pas mauvais, il est inconnu : "
        "lance "
        "`analyze_shot_quality` sur son média plutôt que de l'utiliser à "
        "l'aveugle.\n"
        "La mesure ne dit pas tout. Avant de poser une illustration, "
        "regarde-la "
        "avec `read_frame` : un plan peut être parfaitement net et stable et "
        "rester inutilisable, par exemple s'il montre quelqu'un en train de "
        "parler — le spectateur verra une bouche articuler autre chose que ce "
        "qu'il entend. Ce jugement-là t'appartient, aucun seuil ne le "
        "remplace.\n"
        "Si une demande est ambiguë ou impossible avec les outils disponibles, "
        "explique pourquoi en texte plutôt que d'appeler un outil au hasard. "
        "Si un outil échoue, le message d'erreur t'est renvoyé : corrige ton "
        "appel ou explique le blocage à l'utilisateur, ne réessaie pas "
        "indéfiniment le même appel. Réponds en français.\n") +
    kAgentReplyGuidance;

ChatSession::ChatSession(
    McpBackend& backend, const McpToolRegistry& registry,
    const ChatLlmClient& llm, std::string systemPrompt,
    std::function<void(const ChatTranscriptEntry&)> onEntry)
    : backend_(backend),
      registry_(registry),
      llm_(llm),
      system_prompt_(std::move(systemPrompt)),
      on_entry_(std::move(onEntry)) {}

void ChatSession::Emit(ChatTranscriptEntry entry) {
    if (on_entry_) on_entry_(entry);
    transcript_.push_back(std::move(entry));
}

bool ChatSession::SubmitUserMessage(const std::string& text,
                                    std::string& error) {
    ChatTranscriptEntry userEntry;
    userEntry.kind = ChatEntryKind::UserMessage;
    userEntry.text = text;
    Emit(std::move(userEntry));

    ChatMessage userMessage;
    userMessage.role = "user";
    userMessage.content.push_back(ChatContentBlock::MakeText(text));
    messages_.push_back(std::move(userMessage));

    const TurnIntervention intervention(backend_, TurnLabel(text));
    // A stop asked before this thread got here still counts; the turn
    // forgets it when it ends, however it ends.
    struct ClearStopOnExit {
        std::atomic_bool& stop;
        ~ClearStopOnExit() { stop = false; }
    } clearStop{stop_requested_};
    const auto stopped = [&]() {
        error = "tour arrêté à la demande de l'utilisateur";
        ChatTranscriptEntry stopEntry;
        stopEntry.kind = ChatEntryKind::Error;
        stopEntry.text = error;
        Emit(std::move(stopEntry));
        return false;
    };

    for (int iteration = 0; iteration < kMaxToolIterations; ++iteration) {
        if (stop_requested_) return stopped();
        const ChatSendResult result =
            llm_.SendMessages(messages_, system_prompt_, registry_.Tools());
        if (!result.ok) {
            // FI8 -- a reply refused once decoded (a truncated tool call)
            // was still paid for: its tokens count.
            if (result.usage.reported) {
                AddUsage(result.usage, usage_);
            }
            error = result.error;
            ChatTranscriptEntry errorEntry;
            errorEntry.kind = ChatEntryKind::Error;
            errorEntry.text = error;
            Emit(std::move(errorEntry));
            return false;
        }
        AddUsage(result.usage, usage_);

        ChatMessage assistantMessage;
        assistantMessage.role = "assistant";
        assistantMessage.content = result.content;
        messages_.push_back(std::move(assistantMessage));

        std::vector<ChatContentBlock> toolUses;
        for (const ChatContentBlock& block : result.content) {
            if (block.type == ChatBlockType::Text) {
                if (block.text.empty()) continue;
                ChatTranscriptEntry textEntry;
                textEntry.kind = ChatEntryKind::AssistantText;
                textEntry.text = block.text;
                Emit(std::move(textEntry));
            } else if (block.type == ChatBlockType::ToolUse) {
                toolUses.push_back(block);
            }
        }

        if (toolUses.empty()) {
            // FI8 -- docs/roadmap/FIABILITE-2026-10.md. Keep the partial text
            // visible, but do not present a token limit as a completed turn.
            if (result.stop_reason == "max_tokens" ||
                result.stop_reason == "length") {
                error =
                    "réponse du modèle interrompue par la limite de "
                    "sortie (" +
                    result.stop_reason + ")";
                ChatTranscriptEntry errorEntry;
                errorEntry.kind = ChatEntryKind::Error;
                errorEntry.text = error;
                Emit(std::move(errorEntry));
                return false;
            }
            return true;
        }

        ChatMessage toolResultsMessage;
        toolResultsMessage.role = "user";
        for (const ChatContentBlock& use : toolUses) {
            if (stop_requested_) {
                toolResultsMessage.content.push_back(
                    ChatContentBlock::MakeToolResult(
                        use.tool_use_id,
                        R"({"ok":false,"error":"Stopped","detail":"stopped by )"
                        R"(the user before this call ran"})",
                        true, use.tool_name));
                continue;
            }
            ChatTranscriptEntry callEntry;
            callEntry.kind = ChatEntryKind::ToolCall;
            callEntry.tool_name = use.tool_name;
            mcp_json::Value arguments = use.tool_input;
            callEntry.tool_args_json = arguments.Dump();
            Emit(std::move(callEntry));

            // GENERATIF-2026-10 GN2 -- docs/roadmap/GENERATIF-2026-10.md.
            // This loop runs every tool the model asks for without showing
            // the call to the person first, so it cannot run one that spends
            // their money: the model's own `confirmed_by_person` is no
            // person's yes. Refused here, before the catalog sees it.
            McpToolCallOutcome outcome;
            if (ToolSpendsOnPersonsAccount(use.tool_name)) {
                outcome.error_name = "ConfirmationRequired";
                outcome.message =
                    "nothing was spent: this panel runs tools without "
                    "asking the person first, so it does not run '" +
                    use.tool_name +
                    "'. Show the proposal to the person; a paid generation "
                    "runs from a coding agent (ACP) or Claude Code, which "
                    "put the call to them";
                mcp_json::Value refusal = mcp_json::Value::MakeObject();
                refusal.Set("ok", mcp_json::Value::MakeBool(false));
                refusal.Set("error",
                            mcp_json::Value::MakeString(outcome.error_name));
                refusal.Set("detail",
                            mcp_json::Value::MakeString(outcome.message));
                outcome.result_json = refusal.Dump();
            } else {
                outcome = registry_.Call(backend_, use.tool_name, arguments);
            }
            const std::string& resultText = outcome.result_json;
            const std::string modelResultText =
                outcome.ok && use.tool_name == "describe" &&
                        llm_.Config().provider == ChatLlmProvider::Ollama
                    ? CompactDescribeForOllama(resultText)
                    : resultText;

            ChatTranscriptEntry resultEntry;
            resultEntry.kind = ChatEntryKind::ToolResult;
            resultEntry.tool_name = use.tool_name;
            resultEntry.tool_ok = outcome.ok;
            resultEntry.text = resultText;
            resultEntry.image_base64 = outcome.image_base64;
            resultEntry.image_mime = outcome.image_mime;
            Emit(std::move(resultEntry));

            // The picture, when the tool returned one, goes to the model
            // alongside the text -- but only where the provider can carry it.
            // Anthropic accepts an image inside a tool result; the local
            // Ollama path here does not, and dropping it is better than
            // substituting a description the model would then treat as
            // observation.
            const bool carriesImages =
                llm_.Config().provider == ChatLlmProvider::AnthropicMessages;
            toolResultsMessage.content.push_back(
                ChatContentBlock::MakeToolResult(
                    use.tool_use_id, modelResultText, !outcome.ok,
                    use.tool_name,
                    carriesImages ? outcome.image_base64 : std::string(),
                    carriesImages ? outcome.image_mime : std::string()));
        }
        messages_.push_back(std::move(toolResultsMessage));
        if (stop_requested_) return stopped();
    }

    error = "l'assistant a demandé des outils sans s'arrêter après " +
            std::to_string(kMaxToolIterations) +
            " allers-retours ; arrêt de sécurité.";
    ChatTranscriptEntry limitEntry;
    limitEntry.kind = ChatEntryKind::Error;
    limitEntry.text = error;
    Emit(std::move(limitEntry));
    return false;
}

}  // namespace chat
