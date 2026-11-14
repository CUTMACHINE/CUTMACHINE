// GN6b -- docs/roadmap/GENERATIF-2026-10.md. The two tools of the local
// voice-over: design_voice makes a voice from a description, and
// generate_voice_over has that voice say a text. Both run Qwen3-TTS on this
// machine through scripts/voice_over.py (VoiceOver.h), write the WAV into
// the package's Sound folder, ingest it and mark it with its provenance
// (GeneratedVoice.h).
//
// DEC48 -- a local generation spends nothing: no proposal, no
// confirmed_by_person, no permission prompt, and neither tool is in
// ToolSpendsOnPersonsAccount. It is still made only when the person asks
// for it, which the descriptions say, and marked like every generation.
//
// Each call blocks while the model runs, a minute or more: a background
// task cannot ingest or mark a media (McpBackend::ForkForTask), so neither
// tool runs in the background. Its media work still runs as a task of its
// own, on the runner of the call when there is one.

#include "GeneratedVoice.h"
#include "Generation.h"
#include "McpToolsInternal.h"
#include "VoiceOver.h"

#include <cstdint>
#include <filesystem>
#include <random>
#include <system_error>

using namespace mcp_tools;

namespace {

namespace fs = std::filesystem;

// A seed given by the caller, to reproduce a take, or one drawn here. The
// GPU does not promise the same samples twice for the same seed
// (scripts/voice_over.py); the seed is recorded either way.
bool ReadVoiceSeed(const Value& args, const std::string& tool,
                   std::uint32_t& seed, std::string& message) {
    if (args.Find("seed") == nullptr) {
        std::random_device device;
        seed = static_cast<std::uint32_t>(device());
        return true;
    }
    int64_t value = 0;
    if (!ReadInt64(args, "seed", tool, true, 0, value, message)) {
        return false;
    }
    if (value < 0 || value > static_cast<int64_t>(UINT32_MAX)) {
        message = "'" + tool + ".seed' must be an integer from 0 to " +
                  std::to_string(UINT32_MAX);
        return false;
    }
    seed = static_cast<std::uint32_t>(value);
    return true;
}

// The language of the words said, by ISO 639-1 code; "fr" when unsaid.
bool ReadVoiceLanguage(const Value& args, const std::string& tool,
                       std::string& language, std::string& message) {
    if (!ReadString(args, "language", tool, false, "fr", language, message)) {
        return false;
    }
    if (VoiceOverLanguageName(language).empty()) {
        std::string codes;
        for (const std::string& code : VoiceOverLanguageCodes()) {
            codes += (codes.empty() ? "" : ", ") + code;
        }
        message = "'" + tool + ".language' must be one of " + codes +
                  " (ISO 639-1), not '" + language + "'";
        return false;
    }
    return true;
}

std::vector<std::string> LanguageCodes() { return VoiceOverLanguageCodes(); }

Value GenerationValue(const MediaGeneration& generation) {
    Value value = Value::MakeObject();
    value.Set("service", Value::MakeString(generation.service));
    value.Set("model", Value::MakeString(generation.model));
    value.Set("kind", Value::MakeString(generation.kind));
    value.Set("prompt", Value::MakeString(generation.prompt));
    value.Set("seed", Value::MakeString(generation.seed));
    value.Set("created_at", Value::MakeString(generation.created_at));
    if (!generation.source_media_id.empty()) {
        value.Set("source_media_id",
                  Value::MakeString(generation.source_media_id));
    }
    if (!generation.reference_text.empty()) {
        value.Set("reference_text",
                  Value::MakeString(generation.reference_text));
    }
    return value;
}

// What one call makes: a WAV in the package, then a media of the library
// marked with its provenance. `request` names everything but the paths.
struct VoiceCall {
    // "voix" or "voix-off": the file's name in Sound/, before its ULID.
    std::string file_prefix;
    std::string kind;
    VoiceOverRequest request;
    // What the provenance records as asked (MediaGeneration::prompt).
    std::string prompt;
    std::string reference_text;
    Ulid source_media_id;
};

// Runs the script on the call's task runner when it has one (a fork's),
// otherwise as a task of a manager of its own that it waits on, as the CLI
// runs its media work (Cli.cc, RunCommandMediaTask).
bool RunVoice(McpBackend& backend, const VoiceOverSettings& settings,
              const VoiceOverRequest& request, VoiceOverResult& result,
              std::string& errorName, std::string& message) {
    MediaTaskManager::Work work = [&settings, &request, &result](
                                      MediaTaskContext& context,
                                      std::string& error) {
        return RunVoiceOver(settings, request, context, result, error);
    };
    const MediaTaskRunner runner = backend.BackgroundTaskRunner();
    MediaTaskSnapshot task;
    if (runner) {
        task = runner(MediaTaskKind::VoiceOver, "Voix off", std::move(work));
    } else {
        MediaTaskManager tasks(1);
        task = RunMediaTaskAndWait(tasks, MediaTaskKind::VoiceOver, "Voix off",
                                   std::move(work));
    }
    if (task.state == MediaTaskState::Succeeded) {
        return true;
    }
    if (task.state == MediaTaskState::Cancelled) {
        errorName = "Cancelled";
        message =
            "the voice-over was cancelled" +
            (task.error.empty() ? std::string() : " (" + task.error + ")");
        return false;
    }
    errorName = "GenerationFailed";
    message = "the voice-over model did not complete: " + task.error;
    return false;
}

bool MakeVoiceMedia(McpBackend& backend, VoiceCall call,
                    std::string& resultJson, std::string& errorName,
                    std::string& message) {
    const VoiceOverSettings settings = VoiceOverSettings::FromEnvironment();
    if (!settings.refusal.empty()) {
        errorName = kVoiceOverNotFoundErrorName;
        message = settings.refusal;
        return false;
    }
    const std::string package = backend.PackageDirectory();
    if (package.empty()) {
        errorName = "UnsupportedOperation";
        message =
            "no project package is open: a generated voice is kept in "
            "the package's Sound folder";
        return false;
    }
    const Ulid name = GenerateUlid();
    const fs::path folder = fs::path(package) / "Sound";
    const fs::path wav = folder / (call.file_prefix + "-" + name + ".wav");
    const fs::path report =
        fs::temp_directory_path() / ("cutmachine-voice-" + name + ".json");
    std::error_code folderError;
    fs::create_directories(folder, folderError);
    if (folderError) {
        errorName = "IoError";
        message = "unable to create '" + folder.string() +
                  "': " + folderError.message();
        return false;
    }
    call.request.output_path = wav.string();
    call.request.report_path = report.string();
    VoiceOverResult result;
    const bool ran =
        RunVoice(backend, settings, call.request, result, errorName, message);
    std::error_code removeError;
    fs::remove(report, removeError);
    if (!ran) {
        return false;
    }

    std::string ingestJson;
    if (!backend.IngestMedia(wav.string(), false, std::nullopt, ingestJson,
                             errorName, message)) {
        message = "the voice is at '" + wav.string() +
                  "' but could not be ingested: " + message;
        return false;
    }
    Document ingested;
    if (!backend.SnapshotDocument(ingested, message)) {
        errorName = "IoError";
        return false;
    }
    const std::string fileName = wav.filename().string();
    const LibraryMedia* added = nullptr;
    for (const LibraryMedia& item : ingested.library) {
        if (item.filename == fileName) {
            added = &item;
        }
    }
    if (added == nullptr) {
        errorName = "IoError";
        message =
            "'" + wav.string() + "' was ingested but is not in the library";
        return false;
    }
    const Ulid mediaId = added->id;
    const MediaGeneration provenance =
        VoiceProvenance(call.kind, result, call.prompt, call.reference_text,
                        call.source_media_id, generation::UtcNow());
    std::string applied;
    if (!backend.ApplyOperation(
            SetMediaGenerationOperation{mediaId, provenance}, applied,
            errorName, message)) {
        message =
            "media '" + mediaId +
            "' is in the library but its provenance was refused: " + message;
        return false;
    }

    Value reply = Value::MakeObject();
    reply.Set("media_id", Value::MakeString(mediaId));
    reply.Set("path", Value::MakeString(wav.string()));
    reply.Set("duration_s",
              Value::MakeDouble(static_cast<double>(result.samples) /
                                static_cast<double>(result.sample_rate)));
    reply.Set("sample_rate", Value::MakeInt(result.sample_rate));
    reply.Set("device", Value::MakeString(result.device));
    reply.Set("generation", GenerationValue(provenance));
    Value parsed;
    std::string parseError;
    if (Value::Parse(applied, parsed, parseError)) {
        reply.Set("result", std::move(parsed));
    }
    resultJson = reply.Dump();
    return true;
}

bool DispatchDesignVoice(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "design_voice";
    // `timeline_id` is read by the catalog: the provenance joins that
    // timeline's history, as generate_media's does.
    if (!CheckKnownKeys(args,
                        {"description", "language", "seed", "timeline_id"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    VoiceCall call;
    call.file_prefix = "voix";
    call.kind = kVoiceDesignKind;
    call.request.mode = VoiceOverMode::kDesign;
    if (!ReadString(args, "description", tool, true, "",
                    call.request.description, message) ||
        !ReadVoiceLanguage(args, tool, call.request.language, message) ||
        !ReadVoiceSeed(args, tool, call.request.seed, message)) {
        return Fail(errorName, message, message);
    }
    if (call.request.description.empty()) {
        return Fail(errorName, message,
                    "'design_voice.description' must say the voice to make");
    }
    call.request.text = VoiceReferenceText(call.request.language);
    call.prompt = call.request.description;
    call.reference_text = call.request.text;
    return MakeVoiceMedia(backend, std::move(call), resultJson, errorName,
                          message);
}

bool DispatchGenerateVoiceOver(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    const std::string tool = "generate_voice_over";
    if (!CheckKnownKeys(
            args, {"voice_media_id", "text", "language", "seed", "timeline_id"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    VoiceCall call;
    call.file_prefix = "voix-off";
    call.kind = kVoiceOverKind;
    call.request.mode = VoiceOverMode::kClone;
    Ulid voiceId;
    if (!ReadId(args, "voice_media_id", tool, resolver, true, voiceId,
                message) ||
        !ReadString(args, "text", tool, true, "", call.request.text, message) ||
        !ReadVoiceLanguage(args, tool, call.request.language, message) ||
        !ReadVoiceSeed(args, tool, call.request.seed, message)) {
        return Fail(errorName, message, message);
    }
    if (call.request.text.empty()) {
        return Fail(errorName, message,
                    "'generate_voice_over.text' must say what to say");
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const LibraryMedia* voice = document.FindLibraryMedia(voiceId);
    if (voice == nullptr) {
        errorName = "UnknownMedia";
        message = "unknown media_id '" + voiceId + "'";
        return false;
    }
    // DEC49 -- checked before anything runs: nothing but a voice the
    // engine made is ever handed to the model as a reference.
    if (const std::string refusal = CloneRefusal(*voice); !refusal.empty()) {
        errorName = kNotAnEngineVoiceErrorName;
        message = refusal;
        return false;
    }
    // A library path is relative to the package (Ingest.h, StoredMediaPath).
    fs::path reference(voice->path);
    const std::string package = backend.PackageDirectory();
    if (reference.is_relative() && !package.empty()) {
        reference = fs::path(package) / reference;
    }
    call.request.reference_audio = reference.lexically_normal().string();
    call.request.reference_text = voice->generation.reference_text;
    call.prompt = call.request.text;
    call.source_media_id = voice->id;
    return MakeVoiceMedia(backend, std::move(call), resultJson, errorName,
                          message);
}

}  // namespace

void RegisterVoiceTools(const McpToolAdder& add) {
    add("design_voice",
        "Make a voice for a voice-over, on this machine (Qwen3-TTS, free, "
        "nothing leaves the machine): a voice drawn from `description` says a "
        "fixed reference phrase in `language`, and the WAV enters the library "
        "as a media marked generated (describe shows `generation`, kind "
        "voice_design). Then generate_voice_over says any text in that "
        "voice. Only when the person asked for a voice-over or a voice: "
        "nothing is spent and no permission is asked, but a generation is "
        "never made on the agent's own initiative. Each call draws a new "
        "voice, even for the same description; keep the one the person "
        "likes. Never a voice meant to pass for a real person's. Blocks "
        "while the model runs, about a minute.",
        SchemaBuilder()
            .Field("description",
                   StringSchema("The voice, in English: gender, age, "
                                "timbre, pace, style (\"a warm, calm French "
                                "male voice in his thirties, documentary "
                                "voice-over style\")."),
                   true)
            .Field("language",
                   EnumSchema(LanguageCodes(),
                              "Language of the reference phrase, the "
                              "voice's own; default fr."),
                   false)
            .Field("seed",
                   IntSchema("Optional, 0 to 4294967295: recorded "
                             "with the voice."),
                   false)
            .Build("design_voice arguments"),
        DispatchDesignVoice);

    add("generate_voice_over",
        "Say `text` in a voice design_voice made, on this machine (Qwen3-TTS, "
        "free): the voice is cloned, so it stays the same from one take to "
        "the next, and the WAV enters the library marked generated (kind "
        "voice_over, source_media_id the voice). Lay it with insert_clip like "
        "any sound. The text goes to the model as written: write numbers, "
        "dates and times as they should be said when it matters. REFUSED "
        "(NotAnEngineVoice) for any media that is not a voice design_voice "
        "made -- a recording, a rush, an interview, a voice-over: no voice "
        "of a real person is ever cloned. Only when the person asked for a "
        "voice-over. Blocks while the model runs, about twice the length of "
        "the speech.",
        SchemaBuilder()
            .Field("voice_media_id",
                   IdSchema("The voice: a media design_voice made."), true)
            .Field("text", StringSchema("What the voice says."), true)
            .Field("language",
                   EnumSchema(LanguageCodes(),
                              "Language of the text; default fr. It may "
                              "differ from the voice's."),
                   false)
            .Field("seed",
                   IntSchema("Optional, 0 to 4294967295: recorded "
                             "with the take."),
                   false)
            .Build("generate_voice_over arguments"),
        DispatchGenerateVoiceOver);
}
