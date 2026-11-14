// Integration test for the MCP server against the real, on-disk project
// backend (ROADMAP.md F1.1/F1.2): starts an McpServer bound to a
// McpProjectBackend over a real portable project package, drives it purely
// over HTTP + JSON-RPC, and asserts the resulting project file is
// byte-identical to what a direct ApplyOperationCommand/`--apply-op` call
// on a duplicate fixture would produce.
//
// McpProjectBackend depends on ProjectStorage, which -- like the rest of
// the project-facing CLI surface tested in tests/cli_tests.cc -- needs
// CommonCrypto and therefore only builds on macOS. See
// tests/mcp_tools_tests.cc for the pure-C++ counterpart that exercises the
// same HTTP/JSON-RPC/tool-dispatch/ID-resolver code in a plain Linux build,
// against an in-memory backend instead of a project file.

#include "CanonicalHash.h"
#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "IdResolver.h"
#include "Ingest.h"
#include "Json.h"
#include "McpLiveBackend.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "McpTools.h"
#include "MediaTaskManager.h"
#include "MotionCli.h"
#include "MotionDocument.h"
#include "Operations.h"
#include "Procedures.h"
#include "ProcessLauncher.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "SpeechOnset.h"
#include "TimelineSheets.h"
#include "TimelineTranscription.h"
#include "Transcription.h"
#include "Ulid.h"
#include "motion_humanize_fixtures.h"
#include "resolve_database_fixture.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <mutex>
#include <optional>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

Document Fixture() {
    Document document;
    // Pinned, not left to default to a fresh GenerateUlid(): the assertions
    // below compare two independently-built copies of this fixture
    // byte-for-byte after the same edit, so every ID the serializer writes
    // has to be fixed up front or the comparison can never hold.
    document.sequence.id = "01K30000000000000000000005";
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
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
    };
    return document;
}

std::string HttpPostJson(int port, const std::string& path,
                         const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }
    std::ostringstream request;
    request << "POST " << path << " HTTP/1.1\r\n"
            << "Host: 127.0.0.1\r\n"
            << "Content-Type: application/json\r\n"
            << "Content-Length: " << body.size() << "\r\n"
            << "Connection: close\r\n\r\n"
            << body;
    const std::string requestText = request.str();
    size_t sent = 0;
    while (sent < requestText.size()) {
        const ssize_t wrote =
            ::send(fd, requestText.data() + sent, requestText.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<size_t>(wrote);
    }
    std::string response;
    char chunk[4096];
    ssize_t got;
    while ((got = ::recv(fd, chunk, sizeof(chunk), 0)) > 0)
        response.append(chunk, static_cast<size_t>(got));
    ::close(fd);
    return response;
}

std::string HttpBody(const std::string& httpResponse) {
    const size_t headerEnd = httpResponse.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return "";
    return httpResponse.substr(headerEnd + 4);
}

LibraryMedia AudioRush(const Ulid& id, const std::string& file) {
    LibraryMedia media;
    media.id = id;
    media.path = file;
    media.filename = file;
    media.codec = "pcm_s16le";
    media.has_video = false;
    media.orientation = "audio";
    media.rate = {25, 1};
    media.duration = {1000, 25};
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 1;
    return media;
}

// CHAT-PARITE -- ROADMAP.md F2.4 and P7. transcribe_media,
// transcribe_timeline and align_transcript failed from the chat panel while
// they worked over MCP. The live backend now runs the same commands on the
// same package as McpProjectBackend: their answers must be the same bytes,
// its Whisper work must go through the runner the app hands it (the
// editor's task manager), and none of this may need a Whisper model.
void CheckChatTranscriptionParity(const std::filesystem::path& directory) {
    // The model is a local setting (LocalEnv.h): point the settings file at
    // nothing, so the developer's own configuration never leaks in.
    ::setenv("CUTMACHINE_ENV_FILE", (directory / "absent.env").c_str(), 1);
    ::unsetenv("CUTMACHINE_WHISPER_MODEL");

    const Ulid speech = "01K31000000000000000000001";
    const Ulid mute = "01K31000000000000000000002";
    const Ulid onDisk = "01K31000000000000000000010";
    const Ulid onScreen = "01K31000000000000000000020";
    Document document;
    document.sequence.id = onDisk;
    document.sequence.name = "Active on disk";
    document.sources = {{speech, "speech.wav", {25, 1}, {1000, 25}},
                        {mute, "mute.wav", {25, 1}, {1000, 25}}};
    document.library = {AudioRush(speech, "speech.wav"),
                        AudioRush(mute, "mute.wav")};
    document.library[1].audio_level_measured = true;
    document.library[1].audio_level = 200;
    document.sequence.tracks = {{"01K31000000000000000000011",
                                 "audio",
                                 0,
                                 {{"01K31000000000000000000012",
                                   speech,
                                   {0, 25},
                                   {100, 25},
                                   {0, 25}}}}};
    Project project = Project::FromDocument(document, "Chat parity");
    DocumentSequence shown = project.timelines.front();
    shown.id = onScreen;
    shown.name = "On screen";
    shown.tracks[0].id = "01K31000000000000000000021";
    shown.tracks[0].clips[0].id = "01K31000000000000000000022";
    shown.tracks[0].clips[0].duration = {50, 25};
    project.timelines.push_back(shown);
    std::string path;
    std::string error;
    Check(
        CreatePortableProject((directory / "Chat.cutmachine-project").string(),
                              project, path, error),
        "chat parity package saves: " + error);
    const std::filesystem::path cache =
        std::filesystem::path(path).parent_path() / ".cutmachine";

    McpToolRegistry registry;
    McpProjectBackend server(path);
    // The window shows the second timeline; the project file calls the
    // first one active.
    Document liveDocument = project.MakeDocument(onScreen);
    EditLog liveLog;
    McpLiveBackend live(liveDocument, liveLog);
    live.SetProjectPath(path);
    struct Routed {
        MediaTaskKind kind = MediaTaskKind::Probe;
        std::string label;
        int calls = 0;
    } routed;
    bool runWork = false;
    MediaTaskManager editorTasks(1);
    live.SetMediaTaskRunner([&](MediaTaskKind kind, std::string label,
                                MediaTaskManager::Work work) {
        routed.kind = kind;
        routed.label = label;
        ++routed.calls;
        if (runWork) {
            return RunMediaTaskAndWait(editorTasks, kind, std::move(label),
                                       std::move(work));
        }
        // What the task bar's cancel button leaves behind.
        MediaTaskSnapshot cancelled;
        cancelled.kind = kind;
        cancelled.state = MediaTaskState::Cancelled;
        return cancelled;
    });
    const auto call = [&](McpBackend& backend, const std::string& tool,
                          const std::string& argumentsJson) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };

    // No model configured: the same named reason from both surfaces.
    const std::string speechArguments = "{\"media_id\":\"" + speech + "\"}";
    const McpToolCallOutcome serverUnconfigured =
        call(server, "transcribe_media", speechArguments);
    const McpToolCallOutcome liveUnconfigured =
        call(live, "transcribe_media", speechArguments);
    Check(!liveUnconfigured.ok &&
              liveUnconfigured.message.find("CUTMACHINE_WHISPER_MODEL") !=
                  std::string::npos &&
              liveUnconfigured.result_json == serverUnconfigured.result_json &&
              routed.calls == 0,
          "an unconfigured model reads the same from the chat panel: " +
              liveUnconfigured.result_json);

    // A mute rush needs no model at all: both answer, byte for byte.
    const std::string muteArguments = "{\"media_id\":\"" + mute + "\"}";
    const McpToolCallOutcome serverMute =
        call(server, "transcribe_media", muteArguments);
    const McpToolCallOutcome liveMute =
        call(live, "transcribe_media", muteArguments);
    Check(liveMute.ok && liveMute.result_json == serverMute.result_json &&
              liveMute.result_json.find("\"skipped_count\":1") !=
                  std::string::npos,
          "the live backend skips a mute rush exactly as the server does: " +
              liveMute.result_json);

    // With a model configured, the live backend's Whisper work goes to the
    // runner -- in the app, the editor's own task manager -- and a
    // cancellation from the task bar comes back as a named reason. The model
    // file is never read: nothing here runs Whisper.
    const std::filesystem::path model = directory / "ggml-test.bin";
    std::ofstream(model) << "not a model";
    ::setenv("CUTMACHINE_WHISPER_MODEL", model.c_str(), 1);
    const McpToolCallOutcome liveCancelled =
        call(live, "transcribe_media", speechArguments);
    Check(routed.calls == 1 && routed.kind == MediaTaskKind::Transcription &&
              routed.label == "Whisper 1 media",
          "the chat's transcription runs as a task of the app's manager");
    Check(
        !liveCancelled.ok && liveCancelled.message == "transcription cancelled",
        "a cancelled transcription says so: " + liveCancelled.message);

    // Timeline transcription, served from its cache so no model runs: the
    // cache is keyed by the exact audible arrangement, as Whisper would
    // have written it.
    const auto seedTimelineCache = [&](const Ulid& timelineId) {
        TimelineAudioPlan plan;
        std::string planError;
        Check(BuildTimelineAudioPlan(project.MakeDocument(timelineId), path,
                                     plan, planError),
              "timeline audio plan: " + planError);
        Transcript cached;
        cached.media_id = plan.cache_identity;
        cached.whisper_model = "ggml-test.bin";
        cached.language = "fr";
        cached.source_rate = {25, 1};
        cached.words = {
            {"Bonjour", {0, 25}, {10, 25}},
            {timelineId == onScreen ? "écran" : "disque", {12, 25}, {20, 25}}};
        std::filesystem::create_directories(cache / "timeline-transcripts");
        Check(
            SaveAudioTranscript(
                TimelineTranscriptCachePath(cache, timelineId, false).string(),
                cached, planError),
            "timeline transcript seeds: " + planError);
    };
    seedTimelineCache(onScreen);
    runWork = true;
    routed = {};
    const std::string namedTimeline =
        "{\"timeline_id\":\"" + onScreen + "\",\"language\":\"fr\"}";
    const McpToolCallOutcome serverTimeline =
        call(server, "transcribe_timeline", namedTimeline);
    const McpToolCallOutcome liveTimeline =
        call(live, "transcribe_timeline", namedTimeline);
    Check(
        serverTimeline.ok && liveTimeline.ok &&
            liveTimeline.result_json == serverTimeline.result_json &&
            liveTimeline.result_json.find("\"cached\":true") !=
                std::string::npos &&
            liveTimeline.result_json.find("Bonjour écran") != std::string::npos,
        "a named timeline transcribes to the same bytes from both: " +
            liveTimeline.result_json + " / " + serverTimeline.result_json);
    Check(routed.calls == 1 && routed.label == "Whisper timeline",
          "timeline transcription also runs on the app's manager");

    // Without a timeline_id the chat means the timeline on screen, as for
    // every other live tool -- not the one the file calls active. Frozen on
    // one thread and run on another, the way the chat adapter splits it.
    const McpLiveBackend::CacheTarget target = live.FreezeCacheTarget();
    std::string workerResult;
    std::string workerMessage;
    bool workerOk = false;
    std::thread worker([&] {
        workerOk = McpLiveBackend::RunTranscribeTimeline(
            target, "", "fr", false, workerResult, workerMessage);
    });
    worker.join();
    Check(workerOk && workerResult == liveTimeline.result_json &&
              workerResult.find("\"timeline_id\":\"" + onScreen + "\"") !=
                  std::string::npos,
          "an unnamed timeline is the one on screen: " + workerResult +
              workerMessage);

    // align_transcript reads and rewrites the transcript cache. A word Whisper
    // put on silence is moved onto the speech that follows it.
    SpeechOnsetReport envelope;
    envelope.media_id = speech;
    envelope.windows_per_second = 50;
    envelope.decode_sample_rate = 16000;
    envelope.speech_level = 100000;
    envelope.noise_floor = 1000;
    for (int window = 0; window < 200; ++window) {
        envelope.levels.push_back(window < 100 ? 500 : 100000);
    }
    std::filesystem::create_directories(cache / "speechonset");
    std::ofstream(cache / "speechonset" / (speech + ".json"))
        << SerializeSpeechOnset(envelope);
    Transcript words;
    words.media_id = speech;
    words.whisper_model = "ggml-test.bin";
    words.source_rate = {25, 1};
    words.words = {{"Alors", {40, 25}, {52, 25}}};
    std::filesystem::create_directories(cache / "transcripts");
    const std::string transcriptPath =
        (cache / "transcripts" / (speech + ".json")).string();
    Check(SaveAudioTranscript(transcriptPath, words, error),
          "source transcript seeds: " + error);
    const McpToolCallOutcome serverReport =
        call(server, "align_transcript", "{}");
    const McpToolCallOutcome liveReport = call(live, "align_transcript", "{}");
    Check(liveReport.ok && liveReport.result_json == serverReport.result_json &&
              liveReport.result_json.find(speech) != std::string::npos,
          "the alignment report is the same bytes from both: " +
              liveReport.result_json);
    const McpToolCallOutcome liveApplied =
        call(live, "align_transcript", "{\"apply\":true}");
    Transcript aligned;
    Check(liveApplied.ok &&
              liveApplied.result_json.find("\"written\":true") !=
                  std::string::npos &&
              LoadAudioTranscript(transcriptPath, aligned, error) &&
              aligned.speech_aligned,
          "the chat panel can install the aligned boundaries: " +
              liveApplied.result_json + error);

    // An unsaved project has no cache to write into.
    Document unsavedDocument = project.MakeDocument(onScreen);
    EditLog unsavedLog;
    McpLiveBackend unsaved(unsavedDocument, unsavedLog);
    const McpToolCallOutcome unsavedOutcome =
        call(unsaved, "transcribe_media", speechArguments);
    Check(!unsavedOutcome.ok && unsavedOutcome.message.find(
                                    "save the project") != std::string::npos,
          "an unsaved project is refused with a reason");

    ::unsetenv("CUTMACHINE_WHISPER_MODEL");
}

// CHAT-ANALYSES -- ROADMAP.md F2.4. analyze_shot_quality and
// analyze_speech_onset decoded the whole rush on the main thread when the
// chat panel asked for them, and read_frame and the timeline sheets ran
// FFmpeg there too. The live backend now runs the server's own commands and
// renderers from whatever thread calls it: the answers must be the server's,
// byte for byte, the decoding must go through the runner the app hands it
// (the editor's task manager), and a refusal must read the command's
// `detail`, never its whole envelope -- analyze_audio_beats used to hand the
// envelope over as the message. The rush is generated here; no model runs.
// F2.4 -- ROADMAP.md. ingest_media was refused from the Agent panel and from
// the agents relayed to the window ("this backend cannot ingest media"). The
// live backend now runs IngestCommand's steps on the window's document: the
// answers must be the server's, byte for byte, and the library entries the
// same but for their IDs. Here the window is a stand-in that does what
// -commitProbedMedia: does with the document (main.mm); the window itself is
// played by the window test.
void CheckChatIngestParity(const std::filesystem::path& directory) {
    // The audio level is measured through "ffmpeg", found as in the app.
    const std::string ffmpegDirectory =
        std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string();
    const char* inherited = std::getenv("PATH");
    const std::string searchPath = inherited != nullptr ? inherited : "";
    ::setenv("PATH", (ffmpegDirectory + ':' + searchPath).c_str(), 1);

    // Two packages side by side, so that a rush beside them is stored under
    // the same relative path by both.
    const std::filesystem::path root = directory / "ingest-parity";
    const std::filesystem::path rushes = root / "RUSHES";
    std::filesystem::create_directories(rushes / "son");
    const std::string generateClip =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error "
        "-f lavfi -i 'color=c=gray:s=64x36:r=25:d=1' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:v mpeg4 -c:a aac -shortest -y '" +
        (rushes / "C0001.mp4").string() + "'";
    const std::string generateSound =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error "
        "-f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=1' "
        "-y '" +
        (rushes / "son" / "voix.wav").string() + "'";
    Check(std::system(generateClip.c_str()) == 0 &&
              std::system(generateSound.c_str()) == 0,
          "FFmpeg generates the rushes to ingest");
    std::ofstream(rushes / "notes.txt") << "not a media file\n";
    // What macOS leaves beside a file on an exFAT card: passed over.
    std::ofstream(rushes / "._C0001.mp4") << "AppleDouble";

    Document empty;
    empty.sequence.id = "01K33000000000000000000001";
    empty.sequence.name = "Ingest";
    const Project project = Project::FromDocument(empty, "Ingest");
    std::string serverPath;
    std::string livePath;
    std::string error;
    Check(
        CreatePortableProject((root / "Server.cutmachine-project").string(),
                              project, serverPath, error) &&
            CreatePortableProject((root / "Window.cutmachine-project").string(),
                                  project, livePath, error),
        "ingest packages save: " + error);

    McpToolRegistry registry;
    McpProjectBackend server(serverPath);
    Document liveDocument = project.MakeActiveDocument();
    EditLog liveLog;
    McpLiveBackend live(liveDocument, liveLog);
    live.SetProjectPath(livePath);
    const std::filesystem::path liveDirectory =
        std::filesystem::path(livePath).parent_path();
    int commits = 0;
    // What the window does with the document (-commitProbedMedia:, main.mm),
    // its write and its screen aside: DEC32 (PRD7), the plan's operation in
    // the window's journal.
    live.SetIngestCommitCallback(
        [&](const std::vector<ProbedIngestFile>& files,
            const std::optional<Ulid>& targetBin, IngestOutcome& outcome,
            std::string& errorName, std::string& message) {
            ++commits;
            const IngestPlan plan =
                PlanProbedMedia(liveDocument, liveDirectory, files, targetBin);
            outcome = plan.outcome;
            Document staged = liveDocument;
            EditLog stagedLog = liveLog;
            ApplyIngestRepairs(staged, plan);
            EditError editError = EditError::None;
            if (plan.HasJournalEntry() &&
                !stagedLog.Apply(staged, plan.operation, editError, message)) {
                errorName = EditErrorName(editError);
                return false;
            }
            if (outcome.changed && !staged.Validate(message)) {
                errorName = "ValidationFailed";
                return false;
            }
            liveDocument = std::move(staged);
            liveLog = std::move(stagedLog);
            return true;
        });
    struct Routed {
        MediaTaskKind kind = MediaTaskKind::Transcription;
        std::string label;
        int calls = 0;
    } routed;
    bool runWork = true;
    MediaTaskManager editorTasks(1);
    live.SetMediaTaskRunner([&](MediaTaskKind kind, std::string label,
                                MediaTaskManager::Work work) {
        routed.kind = kind;
        routed.label = label;
        ++routed.calls;
        if (runWork) {
            return RunMediaTaskAndWait(editorTasks, kind, std::move(label),
                                       std::move(work));
        }
        // What the task bar's cancel button leaves behind.
        MediaTaskSnapshot cancelled;
        cancelled.kind = kind;
        cancelled.state = MediaTaskState::Cancelled;
        return cancelled;
    });
    const auto call = [&](McpBackend& backend, const std::string& tool,
                          const std::string& argumentsJson) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };
    const auto ingest = [&](const std::filesystem::path& path, bool recursive) {
        return "{\"path\":\"" + path.string() +
               "\",\"recursive\":" + (recursive ? "true" : "false") + "}";
    };
    // The library of each, its IDs set aside: the entries are otherwise the
    // same, stored path, metadata and audio level included.
    const auto library = [](std::vector<LibraryMedia> media) {
        std::sort(media.begin(), media.end(),
                  [](const LibraryMedia& left, const LibraryMedia& right) {
                      return left.filename < right.filename;
                  });
        Document holder;
        holder.sequence.id = "01K33000000000000000000098";
        for (LibraryMedia& entry : media) {
            entry.id = "01K33000000000000000000099";
            holder.library.push_back(entry);
        }
        return holder.SaveToString();
    };
    const auto serverLibrary = [&]() {
        Project stored;
        std::string loadError;
        Check(Project::Load(serverPath, stored, loadError),
              "the server's package reads back: " + loadError);
        return stored.rushes;
    };

    // A folder, its subfolder included: the same answer from the window.
    const std::string folder = ingest(rushes, true);
    const McpToolCallOutcome serverFolder =
        call(server, "ingest_media", folder);
    const McpToolCallOutcome liveFolder = call(live, "ingest_media", folder);
    Check(
        serverFolder.ok && liveFolder.ok &&
            liveFolder.result_json == serverFolder.result_json &&
            liveFolder.result_json.find("\"added\":2") != std::string::npos &&
            liveFolder.result_json.find("\"ignored\":1") != std::string::npos &&
            liveFolder.result_json.find("notes.txt") != std::string::npos,
        "ingest_media answers from the window as from the server: " +
            liveFolder.result_json + " / " + serverFolder.result_json);
    Check(commits == 1 && liveDocument.library.size() == 2 &&
              liveDocument.sources.size() == 2 &&
              library(liveDocument.library) == library(serverLibrary()),
          "the window's library gets the server's entries, their IDs "
          "aside: " +
              library(liveDocument.library));
    // DEC32 (PRD7) -- one step of the timeline's journal on both sides:
    // `undo` takes the import back, `redo` brings it again.
    Check(liveLog.AppliedCount() == 1 &&
              std::holds_alternative<IngestMediaOperation>(
                  liveLog.AppliedEntries().back().op),
          "the window's ingest is one entry of its journal");
    const McpToolCallOutcome undone = call(server, "undo", "{}");
    Check(undone.ok && serverLibrary().empty(),
          "undo takes the server's import back: " + undone.result_json +
              undone.message);
    const McpToolCallOutcome redone = call(server, "redo", "{}");
    Check(redone.ok && serverLibrary().size() == 2 &&
              library(liveDocument.library) == library(serverLibrary()),
          "redo brings it again: " + redone.result_json + redone.message);
    const McpToolCallOutcome unknownBin = call(
        server, "ingest_media",
        "{\"path\":\"" + rushes.string() +
            "\",\"recursive\":true,\"bin_id\":\"01K33000000000000000000077\"}");
    // The id is resolved as every tool resolves one, before anything else.
    Check(!unknownBin.ok && unknownBin.error_name == "ValidationFailed" &&
              unknownBin.message.find("ingest_media.bin_id") !=
                  std::string::npos &&
              serverLibrary().size() == 2,
          "a bin the project does not have is refused, nothing read: " +
              unknownBin.error_name + " " + unknownBin.message);
    Check(routed.calls == 1 && routed.kind == MediaTaskKind::Probe &&
              routed.label == "Import 3 files",
          "the window's ingest reads its files as a task of the editor's "
          "manager, named as the File menu's import names it: " +
              routed.label);

    // Again: nothing new, the same answer from both.
    const McpToolCallOutcome serverAgain = call(server, "ingest_media", folder);
    const McpToolCallOutcome liveAgain = call(live, "ingest_media", folder);
    Check(serverAgain.ok && liveAgain.ok &&
              liveAgain.result_json == serverAgain.result_json &&
              liveAgain.result_json.find("\"added\":0") != std::string::npos &&
              liveDocument.library.size() == 2,
          "a folder ingested again adds nothing, from both: " +
              liveAgain.result_json);

    // A file that is not a media: the same refusal, report included.
    const std::string stray = ingest(rushes / "notes.txt", false);
    const McpToolCallOutcome serverStray = call(server, "ingest_media", stray);
    const McpToolCallOutcome liveStray = call(live, "ingest_media", stray);
    Check(!serverStray.ok && !liveStray.ok &&
              liveStray.error_name == "IoError" &&
              liveStray.result_json == serverStray.result_json &&
              liveStray.result_json.find("\"result\":") != std::string::npos,
          "an ingest that read nothing is refused alike, with its report: " +
              liveStray.result_json);

    // A relative path: the same refusal.
    const McpToolCallOutcome serverRelative =
        call(server, "ingest_media", R"({"path":"RUSHES"})");
    const McpToolCallOutcome liveRelative =
        call(live, "ingest_media", R"({"path":"RUSHES"})");
    Check(!liveRelative.ok &&
              liveRelative.result_json == serverRelative.result_json &&
              liveRelative.message == "media path must be absolute",
          "a relative path is refused alike: " + liveRelative.result_json);

    // Cancelled from the task bar: nothing reaches the window.
    std::filesystem::create_directories(root / "PLUS");
    std::filesystem::copy_file(rushes / "C0001.mp4",
                               root / "PLUS" / "C0002.mp4");
    runWork = false;
    const int commitsBefore = commits;
    const McpToolCallOutcome cancelled =
        call(live, "ingest_media", ingest(root / "PLUS", false));
    Check(!cancelled.ok && cancelled.error_name == "IoError" &&
              cancelled.message == "import cancelled" &&
              commits == commitsBefore && liveDocument.library.size() == 2,
          "an import cancelled from the task bar says so and adds nothing: " +
              cancelled.result_json);
    runWork = true;

    // Read on one thread, run on another, committed on the first: the way
    // the adapter splits it (MainThreadBackend.h).
    const McpLiveBackend::IngestTarget target = live.FreezeIngestTarget();
    McpLiveBackend::ProbedIngest probed;
    std::string probeName;
    std::string probeMessage;
    bool probedOk = false;
    std::thread worker([&] {
        probedOk = McpLiveBackend::RunProbeIngest(
            target, (root / "PLUS").string(), false, probed, probeName,
            probeMessage);
    });
    worker.join();
    std::string splitResult;
    std::string splitName;
    std::string splitMessage;
    const bool splitOk = probedOk && live.CommitIngest(probed, splitResult,
                                                       splitName, splitMessage);
    Check(splitOk && splitResult.find("\"added\":1") != std::string::npos &&
              liveDocument.library.size() == 3,
          "the split ingest adds the rush on the thread that owns the "
          "document: " +
              splitResult + probeMessage + splitMessage);

    // A window whose project is not saved, or without a window to commit
    // into: refused by name.
    Document unsavedDocument = project.MakeActiveDocument();
    EditLog unsavedLog;
    McpLiveBackend unsaved(unsavedDocument, unsavedLog);
    const McpToolCallOutcome unsavedOutcome =
        call(unsaved, "ingest_media", folder);
    Check(!unsavedOutcome.ok &&
              unsavedOutcome.message.find("save the project") !=
                  std::string::npos &&
              unsavedDocument.library.empty(),
          "an unsaved project is refused with a reason: " +
              unsavedOutcome.message);
    unsaved.SetProjectPath(livePath);
    const McpToolCallOutcome windowless = call(unsaved, "ingest_media", folder);
    Check(!windowless.ok && windowless.error_name == "UnsupportedOperation" &&
              unsavedDocument.library.empty(),
          "without a window to import into, the ingest is refused: " +
              windowless.message);

    // F2.4 -- create_project and open_project stay refused from the window,
    // which holds its project until it closes: by name, with the project
    // held and the way to another.
    const McpToolCallOutcome created =
        call(live, "create_project",
             "{\"path\":\"" + (root / "Autre.cutmachine-project").string() +
                 "\",\"name\":\"Autre\"}");
    Check(
        !created.ok && created.error_name == "UnsupportedOperation" &&
            created.message.find(livePath) != std::string::npos &&
            created.message.find("« Nouveau projet… »") != std::string::npos &&
            // DEC39 -- by the Fichier menu, no longer by quitting.
            created.message.find("Fichier") != std::string::npos &&
            created.message.find("quit CUTMACHINE") == std::string::npos &&
            created.message.find("ingest_media") != std::string::npos &&
            !std::filesystem::exists(root / "Autre.cutmachine-project"),
        "create_project from the window is refused with the way to "
        "another project: " +
            created.message);
    const McpToolCallOutcome openedOther =
        call(live, "open_project", "{\"path\":\"" + serverPath + "\"}");
    Check(!openedOther.ok && openedOther.error_name == "UnsupportedOperation" &&
              openedOther.message.find(livePath) != std::string::npos &&
              openedOther.message.find("« Ouvrir un projet… »") !=
                  std::string::npos &&
              openedOther.message.find("Fichier") != std::string::npos,
          "open_project of another project is refused with the way to "
          "it: " +
              openedOther.message);
    const McpToolCallOutcome openedSame =
        call(live, "open_project", "{\"path\":\"" + livePath + "\"}");
    Check(openedSame.ok &&
              openedSame.result_json.find("\"app\":true") != std::string::npos,
          "open_project of the window's own project still answers: " +
              openedSame.result_json);
}

// F2.4 (R10) -- ROADMAP.md. import_resolve_timeline was refused from the
// Agent panel and from the agents relayed to the window ("this backend
// cannot read a Resolve timeline"), the last tool of the catalog that was.
// The live backend now runs the import's steps (ResolveImport.h) on the
// window's document and journal: the answers must be the server's, byte for
// byte -- DEC15's grades included, their clip ids aside --, a refusal must
// leave the window as it was, and the read and the commit must hold on two
// threads, as the adapter splits them (MainThreadBackend.h). The window
// itself is played by nobody here: the screen is locked at night.
void CheckChatResolveTimelineParity(const std::filesystem::path& directory) {
    const std::string ffmpegDirectory =
        std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string();
    const char* inherited = std::getenv("PATH");
    const std::string searchPath = inherited != nullptr ? inherited : "";
    ::setenv("PATH", (ffmpegDirectory + ':' + searchPath).c_str(), 1);

    const std::filesystem::path root = directory / "resolve-timeline-parity";
    const std::filesystem::path rushes = root / "RUSHES";
    std::filesystem::create_directories(rushes);
    for (const char* name : {"A001.mp4", "A002.mp4"}) {
        const std::string generate =
            std::string("'") + FFMPEG_EXECUTABLE +
            "' -hide_banner -loglevel error "
            "-f lavfi -i 'color=c=gray:s=64x36:r=25:d=1' -c:v mpeg4 -y '" +
            (rushes / name).string() + "'";
        Check(std::system(generate.c_str()) == 0,
              std::string("FFmpeg generates the rush ") + name);
    }

    Document empty;
    empty.sequence.id = "01K33000000000000000000011";
    empty.sequence.name = "Relue";
    const Project project = Project::FromDocument(empty, "Relue");
    std::string error;
    std::string output;
    const auto package = [&](const std::string& name) {
        std::string path;
        Check(CreatePortableProject(
                  (root / (name + ".cutmachine-project")).string(), project,
                  path, error) &&
                  IngestCommand(path, rushes.string(), false, output) == 0,
              name + " holds the rushes: " + error + output);
        return path;
    };
    const std::string serverPath = package("Server");
    const std::string livePath = package("Window");
    const std::string gradedServerPath = package("ServerGrades");
    const std::string gradedLivePath = package("WindowGrades");

    // Two clips on V1, at speed 1, from the two rushes.
    const auto manifest = [&](const std::string& file, const std::string& name,
                              const std::string& secondPath) {
        std::ostringstream json;
        json << R"({"schema":"cutmachine.resolve-timeline-read.v1",)"
             << R"("project":"P","name":")" << name
             << R"(","frame_rate":25,"origin":90000,"width":1920,)"
             << R"("height":1080,"unpublished":[],"tracks":[{"kind":"video",)"
             << R"("index":1,"name":"","clips":[)"
             << R"({"name":"A001.mp4","record_start":0,"record_end":10,)"
             << R"("duration":10,"source_start":0,"fusion_comps":0,"path":")"
             << (rushes / "A001.mp4").string()
             << R"(","left_offset":0,"right_offset":15,"media_frames":25,)"
             << R"("media_rate":25},)"
             << R"({"name":"A002.mp4","record_start":10,"record_end":20,)"
             << R"("duration":10,"source_start":5,"fusion_comps":0,"path":")"
             << secondPath
             << R"(","left_offset":5,"right_offset":10,"media_frames":25,)"
             << R"("media_rate":25}],"transitions":[]}]})";
        const std::filesystem::path path = root / file;
        std::ofstream(path) << json.str();
        return path.string();
    };
    const std::string good =
        manifest("relue.json", "RELUE", (rushes / "A002.mp4").string());
    const std::string unknown =
        manifest("inconnu.json", "RELUE", "/ailleurs/A009.mp4");

    McpToolRegistry registry;
    const auto call = [&](McpBackend& backend, const std::string& tool,
                          const std::string& argumentsJson) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };
    const auto arguments = [](const std::string& manifestPath,
                              const std::string& database) {
        return "{\"manifest_path\":\"" + manifestPath + "\"" +
               (database.empty()
                    ? std::string()
                    : ",\"resolve_database\":\"" + database + "\"") +
               "}";
    };
    const auto clipCount = [](const Document& document) {
        size_t count = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            count += track.clips.size();
        return count;
    };
    // The two packages ingest the same rushes under IDs of their own, and an
    // import generates the IDs of its tracks and clips: the answers are
    // compared with every ULID masked, the rest byte for byte.
    static const std::regex kUlid("\"[0-9A-HJKMNP-TV-Z]{26}\"");
    const auto withoutIds = [](const std::string& json) {
        return std::regex_replace(json, kUlid, "\"ULID\"");
    };
    const auto loadWindow = [&](const std::string& path, Document& document) {
        Project stored;
        std::string loadError;
        Check(Project::Load(path, stored, loadError),
              "the window's package reads: " + loadError);
        document = stored.MakeActiveDocument();
    };

    McpProjectBackend server(serverPath);
    Document liveDocument;
    loadWindow(livePath, liveDocument);
    // DEC32 (PRD7) -- the window holds its package's journal, and since the
    // ingest is journaled that journal already holds the one that filled the
    // library, as the server's does.
    EditLog liveLog;
    {
        Project window;
        EditError loadError = EditError::None;
        Check(Project::Load(livePath, window, error) &&
                  EditLog::Load(TimelineEditLogPathForProject(
                                    livePath, window.active_timeline_id),
                                liveLog, loadError, error),
              "the window's journal reads: " + error);
    }
    const size_t journaled = liveLog.AppliedCount();
    int saves = 0;
    McpLiveBackend live(liveDocument, liveLog, [&] { ++saves; });
    live.SetProjectPath(livePath);

    // A clip whose rush the library does not hold: the same refusal, and
    // the window as it was.
    const McpToolCallOutcome serverRefused =
        call(server, "import_resolve_timeline", arguments(unknown, ""));
    const McpToolCallOutcome liveRefused =
        call(live, "import_resolve_timeline", arguments(unknown, ""));
    Check(!serverRefused.ok && !liveRefused.ok &&
              liveRefused.error_name == "ValidationFailed" &&
              liveRefused.error_name == serverRefused.error_name &&
              liveRefused.message == serverRefused.message &&
              liveRefused.result_json == serverRefused.result_json &&
              liveRefused.result_json.find("A009.mp4") != std::string::npos,
          "a refused import answers from the window as from the server: " +
              liveRefused.result_json + " / " + serverRefused.result_json);
    Check(clipCount(liveDocument) == 0 && liveLog.AppliedCount() == journaled &&
              saves == 0,
          "a refused import leaves the window's timeline as it was");

    // The import: the server's answer, its IDs aside.
    const McpToolCallOutcome serverImport =
        call(server, "import_resolve_timeline", arguments(good, ""));
    const McpToolCallOutcome liveImport =
        call(live, "import_resolve_timeline", arguments(good, ""));
    Check(serverImport.ok && liveImport.ok &&
              withoutIds(liveImport.result_json) ==
                  withoutIds(serverImport.result_json) &&
              liveImport.result_json.find("\"delta\":") != std::string::npos &&
              liveImport.result_json.find("\"clips\":2") != std::string::npos,
          "import_resolve_timeline answers from the window as from the "
          "server: " +
              liveImport.result_json + " / " + serverImport.result_json +
              liveImport.message);
    Project stored;
    Check(Project::Load(serverPath, stored, error),
          "the server's package reads back: " + error);
    const Document serverDocument = stored.MakeActiveDocument();
    Check(clipCount(liveDocument) == 2 && clipCount(serverDocument) == 2 &&
              liveDocument.sequence.name == "RELUE" &&
              liveDocument.sequence.name == serverDocument.sequence.name &&
              saves == 1,
          "the window's timeline gets the server's cut, saved once");
    EditLog serverLog;
    EditError logError = EditError::None;
    Check(EditLog::Load(TimelineEditLogPathForProject(
                            serverPath, stored.active_timeline_id),
                        serverLog, logError, error),
          "the server's journal reads back: " + error);
    Check(liveLog.AppliedCount() > journaled &&
              serverLog.AppliedCount() == liveLog.AppliedCount(),
          "the window's journal takes the import's operations, as the "
          "server's: " +
              std::to_string(liveLog.AppliedCount()) + " / " +
              std::to_string(serverLog.AppliedCount()));
    std::string undoResult;
    std::string undoName;
    std::string undoMessage;
    Check(live.Undo(undoResult, undoName, undoMessage) &&
              live.Redo(undoResult, undoName, undoMessage) &&
              clipCount(liveDocument) == 2,
          "the import undoes and redoes in the window: " + undoMessage);

    // The same manifest again: the timeline already holds a cut.
    const McpToolCallOutcome serverAgain =
        call(server, "import_resolve_timeline", arguments(good, ""));
    const McpToolCallOutcome liveAgain =
        call(live, "import_resolve_timeline", arguments(good, ""));
    Check(!serverAgain.ok && !liveAgain.ok &&
              liveAgain.message == serverAgain.message &&
              liveAgain.message.find("porte déjà") != std::string::npos,
          "a second import is refused alike: " + liveAgain.message);

    // A relative path, from both.
    const McpToolCallOutcome serverRelative = call(
        server, "import_resolve_timeline", R"({"manifest_path":"RELUE.json"})");
    const McpToolCallOutcome liveRelative = call(
        live, "import_resolve_timeline", R"({"manifest_path":"RELUE.json"})");
    Check(!liveRelative.ok && liveRelative.message == serverRelative.message &&
              liveRelative.message == "manifest path must be absolute",
          "a relative manifest is refused alike: " + liveRelative.message);

    // SAVING_ROADMAP.md, a write refused under the editor (S3) -- the window
    // refuses to save the import: refused by name, and the window keeps the
    // timeline and the journal it had, as for any other edit of the panel.
    {
        const std::string refusedPath = package("WindowRefused");
        Document refusedDocument;
        loadWindow(refusedPath, refusedDocument);
        EditLog refusedLog;
        int shown = 0;
        McpLiveBackend refusing(refusedDocument, refusedLog, [&] { ++shown; });
        refusing.SetProjectPath(refusedPath);
        refusing.SetEditCommitCallback(
            [](const Document&, const EditLog&, std::string& message) {
                message = "No space left on device";
                return false;
            });
        const McpToolCallOutcome refused =
            call(refusing, "import_resolve_timeline", arguments(good, ""));
        Check(!refused.ok && refused.error_name == "IoError" &&
                  refused.message.find("No space left on device") !=
                      std::string::npos,
              "an import the window cannot save is refused by name: " +
                  refused.error_name + " " + refused.message);
        Check(clipCount(refusedDocument) == 0 &&
                  refusedLog.AppliedCount() == 0 && shown == 0,
              "an import the window cannot save leaves its timeline and "
              "journal as they were");
    }

    // DEC15 -- with the Resolve database: the grades come back from the
    // window too, under the ids the window gives its clips. Read on a
    // worker, committed on this thread.
    const auto bodies = resolve_fixture::LoadBodies(
        std::filesystem::path(RESOLVE_GRADE_FIXTURES_DIR) / "bodies.txt");
    resolve_fixture::Timeline graded;
    graded.name = "RELUE";
    resolve_fixture::Item first;
    first.start = 90000;
    first.duration = 10;
    first.name = "A001.mp4";
    first.path = (rushes / "A001.mp4").string();
    first.body = bodies.at("offset_window");
    resolve_fixture::Item second = first;
    second.start = 90010;
    second.name = "A002.mp4";
    second.path = (rushes / "A002.mp4").string();
    second.correction = false;
    second.body = bodies.at("quiet_flags");
    graded.items = {first, second};
    const std::filesystem::path database = root / "Project.db";
    Check(resolve_fixture::Write(database, {graded}, error),
          "the Resolve database of the test is written: " + error);

    McpProjectBackend gradedServer(gradedServerPath);
    Document gradedDocument;
    loadWindow(gradedLivePath, gradedDocument);
    EditLog gradedLog;
    McpLiveBackend gradedLive(gradedDocument, gradedLog);
    gradedLive.SetProjectPath(gradedLivePath);
    const McpToolCallOutcome serverGrades =
        call(gradedServer, "import_resolve_timeline",
             arguments(good, database.string()));
    McpLiveBackend::ResolveTimelineFiles files;
    std::string readName;
    std::string readMessage;
    bool read = false;
    std::thread worker([&] {
        read = McpLiveBackend::RunReadResolveTimeline(
            good, database.string(), files, readName, readMessage);
    });
    worker.join();
    std::string gradesResult;
    std::string gradesName;
    std::string gradesMessage;
    const bool committed =
        read && gradedLive.CommitResolveTimeline(files, gradesResult,
                                                 gradesName, gradesMessage);
    Ulid gradedId;
    for (const DocumentTrack& track : gradedDocument.sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (clip.timeline_in == RationalTime{0, 25}) gradedId = clip.id;
    // Called past the catalog, the split answer has no `delta`, which the
    // catalog adds to every journaled edit (PRD40).
    const auto withoutDelta = [](const std::string& json) {
        mcp_json::Value value;
        std::string parseError;
        if (!mcp_json::Value::Parse(json, value, parseError) ||
            !value.IsObject())
            return json;
        mcp_json::Value kept = mcp_json::Value::MakeObject();
        for (const auto& [key, member] : value.AsObject())
            if (key != "delta") kept.Set(key, member);
        return kept.Dump();
    };
    Check(
        serverGrades.ok && committed &&
            withoutIds(withoutDelta(gradesResult)) ==
                withoutIds(withoutDelta(serverGrades.result_json)) &&
            gradesResult.find("\"clip_id\":\"" + gradedId + "\"") !=
                std::string::npos &&
            gradesResult.find("fenêtre (correcteur 4)") != std::string::npos &&
            gradesResult.find("\"ungraded\":1") != std::string::npos,
        "the grades come back from the window, under its own clip ids, "
        "read and committed on two threads: " +
            gradesResult + " / " + serverGrades.result_json + readMessage +
            gradesMessage);

    // A window whose project is not saved: refused by name, nothing read in.
    Document unsavedDocument = project.MakeActiveDocument();
    EditLog unsavedLog;
    McpLiveBackend unsaved(unsavedDocument, unsavedLog);
    const McpToolCallOutcome unsavedOutcome =
        call(unsaved, "import_resolve_timeline", arguments(good, ""));
    Check(!unsavedOutcome.ok &&
              unsavedOutcome.message.find("save the project") !=
                  std::string::npos &&
              clipCount(unsavedDocument) == 0,
          "an unsaved project is refused with a reason: " +
              unsavedOutcome.message);
}

std::string WithoutAddresses(const std::string& text) {
    static const std::regex kAddress("0x[0-9a-fA-F]+");
    return std::regex_replace(text, kAddress, "0x");
}

void CheckChatAnalysisParity(const std::filesystem::path& directory) {
    // The commands and renderers call "ffmpeg" by name, as in the app.
    const std::string ffmpegDirectory =
        std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string();
    const char* inherited = std::getenv("PATH");
    const std::string searchPath = inherited != nullptr ? inherited : "";
    ::setenv("PATH", (ffmpegDirectory + ':' + searchPath).c_str(), 1);

    const Ulid rushId = "01K32000000000000000000001";
    const Ulid absentId = "01K32000000000000000000002";
    const auto videoRush = [](const Ulid& id, const std::string& file) {
        LibraryMedia media;
        media.id = id;
        media.path = file;
        media.filename = file;
        media.codec = "mpeg4";
        media.width = 160;
        media.height = 90;
        media.orientation = "landscape";
        media.rate = {25, 1};
        media.duration = {50, 25};
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 1;
        return media;
    };
    Document document;
    document.sequence.id = "01K32000000000000000000010";
    document.sequence.name = "Analyses";
    document.sources = {{rushId, "rush.mp4", {25, 1}, {50, 25}},
                        {absentId, "absent.mp4", {25, 1}, {50, 25}}};
    document.library = {videoRush(rushId, "rush.mp4"),
                        videoRush(absentId, "absent.mp4")};
    // Two shots of the rush with a jump between them: one cut to sheet.
    document.sequence.tracks = {
        {"01K32000000000000000000011",
         "video",
         0,
         {{"01K32000000000000000000012", rushId, {0, 25}, {20, 25}, {0, 25}},
          {"01K32000000000000000000013",
           rushId,
           {28, 25},
           {20, 25},
           {20, 25}}}},
        {"01K32000000000000000000014",
         "audio",
         1,
         {{"01K32000000000000000000015", rushId, {0, 25}, {40, 25}, {0, 25}}}}};
    const Project project = Project::FromDocument(document, "Analyses");
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Analyses.cutmachine-project").string(), project,
              path, error),
          "chat analysis package saves: " + error);
    const std::filesystem::path rush =
        std::filesystem::path(path).parent_path() / "rush.mp4";
    const std::string generate =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=160x90:rate=25:duration=2' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=2' "
        "-c:v mpeg4 -c:a aac -ac 1 -shortest -y '" +
        rush.string() + "'";
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg generates the analysed rush");

    McpToolRegistry registry;
    McpProjectBackend server(path);
    Document liveDocument = project.MakeActiveDocument();
    EditLog liveLog;
    McpLiveBackend live(liveDocument, liveLog);
    live.SetProjectPath(path);
    struct Routed {
        MediaTaskKind kind = MediaTaskKind::Probe;
        std::string label;
        int calls = 0;
    } routed;
    bool runWork = true;
    MediaTaskManager editorTasks(2);
    live.SetMediaTaskRunner([&](MediaTaskKind kind, std::string label,
                                MediaTaskManager::Work work) {
        routed.kind = kind;
        routed.label = label;
        ++routed.calls;
        if (runWork) {
            return RunMediaTaskAndWait(editorTasks, kind, std::move(label),
                                       std::move(work));
        }
        // What the task bar's cancel button leaves behind.
        MediaTaskSnapshot cancelled;
        cancelled.kind = kind;
        cancelled.state = MediaTaskState::Cancelled;
        return cancelled;
    });
    const auto call = [&](McpBackend& backend, const std::string& tool,
                          const std::string& argumentsJson) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(argumentsJson, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };

    // Each analysis: the same bytes from both surfaces, and from the chat,
    // one task of the app's manager named as the task bar shows it.
    struct Analysis {
        std::string tool;
        MediaTaskKind kind;
        std::string label;
        std::string cancelled;
    };
    const std::vector<Analysis> analyses = {
        {"analyze_shot_quality", MediaTaskKind::ShotQuality,
         "Analyse image rush.mp4", "shot quality analysis cancelled"},
        {"analyze_speech_onset", MediaTaskKind::SpeechOnset,
         "Analyse parole rush.mp4", "speech onset analysis cancelled"},
        {"analyze_audio_beats", MediaTaskKind::Beat,
         "Analyse musicale rush.mp4", "audio beat analysis cancelled"},
    };
    const std::string rushArguments = "{\"media_id\":\"" + rushId + "\"}";
    const std::string absentArguments = "{\"media_id\":\"" + absentId + "\"}";
    for (const Analysis& analysis : analyses) {
        runWork = true;
        routed = {};
        const McpToolCallOutcome served =
            call(server, analysis.tool, rushArguments);
        const McpToolCallOutcome chatted =
            call(live, analysis.tool, rushArguments);
        Check(served.ok && chatted.ok &&
                  chatted.result_json == served.result_json,
              analysis.tool +
                  " answers the same bytes from both: " + chatted.result_json +
                  chatted.message + " / " + served.result_json);
        Check(routed.calls == 1 && routed.kind == analysis.kind &&
                  routed.label == analysis.label,
              analysis.tool + " runs as a task of the app's manager, got '" +
                  routed.label + "'");

        // A rush whose file is gone: the command's own reason, read from its
        // envelope the same way on both sides. FFmpeg's message names the
        // address of its demuxer, the one thing that differs between runs.
        const McpToolCallOutcome servedAbsent =
            call(server, analysis.tool, absentArguments);
        const McpToolCallOutcome chattedAbsent =
            call(live, analysis.tool, absentArguments);
        Check(
            !servedAbsent.ok && !chattedAbsent.ok &&
                WithoutAddresses(chattedAbsent.result_json) ==
                    WithoutAddresses(servedAbsent.result_json) &&
                chattedAbsent.message.find("absent.mp4") != std::string::npos &&
                chattedAbsent.message.front() != '{',
            analysis.tool +
                " refuses a missing file with the command's "
                "detail: " +
                chattedAbsent.message + " / " + servedAbsent.message);

        // Cancelled from the task bar: a named reason, not a hang.
        runWork = false;
        const McpToolCallOutcome cancelled =
            call(live, analysis.tool, rushArguments);
        Check(!cancelled.ok && cancelled.message == analysis.cancelled,
              analysis.tool + " cancelled says so: " + cancelled.message);
    }

    // Frozen on one thread, run on another: the way the chat adapter splits
    // it, with the answer the server gave.
    runWork = true;
    const McpToolCallOutcome servedOnset =
        call(server, "analyze_speech_onset", rushArguments);
    const McpLiveBackend::CacheTarget cacheTarget = live.FreezeCacheTarget();
    std::string workerResult;
    std::string workerMessage;
    bool workerOk = false;
    std::thread analysisWorker([&] {
        workerOk = McpLiveBackend::RunAnalyzeSourceSpeechOnset(
            cacheTarget, rushId, SpeechOnsetSettings{}, workerResult,
            workerMessage);
    });
    analysisWorker.join();
    Check(workerOk && workerResult == servedOnset.result_json,
          "a speech onset analysis runs from the worker: " + workerResult +
              workerMessage);

    // Pictures: a frame and both sheets, rendered from the same document.
    const std::string frameArguments =
        "{\"media_id\":\"" + rushId +
        "\",\"source_in\":{\"value\":10,\"rate\":25}}";
    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, std::string>>{
             {"read_frame", frameArguments},
             {"contact_sheet", "{}"},
             {"cut_sheet", "{}"}}) {
        const McpToolCallOutcome served = call(server, tool, arguments);
        const McpToolCallOutcome chatted = call(live, tool, arguments);
        Check(served.ok && chatted.ok && !chatted.image_base64.empty() &&
                  chatted.image_base64 == served.image_base64 &&
                  chatted.result_json == served.result_json,
              tool + " renders the same JPEG from both: " + chatted.message +
                  served.message);
    }
    // G1 -- docs/roadmap/GRADE-2026-09.md. The colour measure, decoded
    // from the same file through the same document on both sides.
    const std::string measureArguments =
        "{\"clip_ids\":[\"01K32000000000000000000012\"],"
        "\"region\":{\"x\":{\"num\":0,\"den\":1},\"y\":{\"num\":0,"
        "\"den\":1},\"width\":{\"num\":1,\"den\":2},\"height\":{"
        "\"num\":1,\"den\":2}}}";
    {
        const McpToolCallOutcome served =
            call(server, "measure_color", measureArguments);
        const McpToolCallOutcome chatted =
            call(live, "measure_color", measureArguments);
        Check(
            served.ok && chatted.ok &&
                chatted.result_json == served.result_json &&
                served.result_json.find("\"luma_percent\"") !=
                    std::string::npos &&
                served.result_json.find("\"hue_degrees\"") != std::string::npos,
            "measure_color reads the same numbers from both: " +
                chatted.message + served.message + served.result_json);
    }
    // G2 -- the match, read from the same file through the same document on
    // both sides: two shots of the same rush.
    {
        const std::string matchArguments =
            "{\"clip_id\":\"01K32000000000000000000013\","
            "\"reference_clip_id\":\"01K32000000000000000000012\"}";
        const McpToolCallOutcome served =
            call(server, "match_color", matchArguments);
        const McpToolCallOutcome chatted =
            call(live, "match_color", matchArguments);
        Check(served.ok && chatted.ok &&
                  chatted.result_json == served.result_json &&
                  served.result_json.find("\"tool\":\"set_clip_effects\"") !=
                      std::string::npos,
              "match_color gives the same step from both: " + chatted.message +
                  served.message);
    }
    // G1b -- the graded frame, rendered from the same document on both
    // sides and said to be graded.
    {
        const std::string gradedArguments =
            "{\"clip_id\":\"01K32000000000000000000012\",\"graded\":true}";
        const McpToolCallOutcome served =
            call(server, "read_frame", gradedArguments);
        const McpToolCallOutcome chatted =
            call(live, "read_frame", gradedArguments);
        Check(
            served.ok && chatted.ok && !served.image_base64.empty() &&
                chatted.image_base64 == served.image_base64 &&
                served.result_json.find("\"graded\":true") != std::string::npos,
            "read_frame graded renders the same JPEG from both: " +
                chatted.message + served.message);
    }
    // A time inside a source frame reads that frame, from its first
    // instant, not the next one FFmpeg's seek lands on (agentic QA of 23
    // September 2026): 21/50 s is frame 10.5 of the 25 i/s rush.
    const auto frameAt = [&](int64_t value, int64_t rate) {
        return call(server, "read_frame",
                    "{\"media_id\":\"" + rushId +
                        "\",\"source_in\":{\"value\":" + std::to_string(value) +
                        ",\"rate\":" + std::to_string(rate) + "}}");
    };
    const McpToolCallOutcome inside = frameAt(21, 50);
    const McpToolCallOutcome tenth = frameAt(10, 25);
    const McpToolCallOutcome eleventh = frameAt(11, 25);
    Check(inside.ok && tenth.ok && eleventh.ok &&
              inside.image_base64 == tenth.image_base64 &&
              tenth.image_base64 != eleventh.image_base64 &&
              inside.result_json.find(
                  "\"source_in\":{\"value\":10,\"rate\":25}") !=
                  std::string::npos,
          "read_frame inside frame 10 reads frame 10 and says so: " +
              inside.result_json + inside.message);
    TimelineSheetSettings sheetSettings;
    TimelineSheetPlan sheetPlan;
    Check(BuildTimelineSheetPlan(liveDocument, TimelineSheetKind::Contact,
                                 sheetSettings, sheetPlan, error),
          "contact plan builds: " + error);
    const McpLiveBackend::PictureTarget pictureTarget =
        live.FreezePictureTarget();
    std::string frozenSheet;
    std::string liveSheet;
    workerOk = false;
    std::thread pictureWorker([&] {
        workerOk = McpLiveBackend::RunCaptureTimelineSheet(
            pictureTarget, sheetPlan, sheetSettings, frozenSheet,
            workerMessage);
    });
    pictureWorker.join();
    Check(workerOk &&
              live.CaptureTimelineSheet(sheetPlan, sheetSettings, liveSheet,
                                        error) &&
              frozenSheet == liveSheet,
          "a sheet renders from the worker on a frozen copy: " + workerMessage +
              error);
    Check(!live.CaptureSourceFrame(absentId, Ulid(), {10, 25}, liveSheet,
                                   error) &&
              !error.empty(),
          "a missing file cannot be framed, and says why");
    // PRD40 -- ROADMAP.md. An offline rush is named, path included, before
    // FFmpeg is asked: read_frame said "FFmpeg could not render the
    // requested frame" (IoError) while the server's log named the file.
    for (McpBackend* backend : std::vector<McpBackend*>{&server, &live}) {
        for (const auto& [tool, arguments] :
             std::vector<std::pair<std::string, std::string>>{
                 {"read_frame", "{\"media_id\":\"" + absentId +
                                    "\",\"source_in\":{\"value\":10,"
                                    "\"rate\":25}}"},
                 {"media_sheet", absentArguments}}) {
            const McpToolCallOutcome offline = call(*backend, tool, arguments);
            Check(!offline.ok && offline.error_name == "MediaMissing" &&
                      offline.message.find("absent.mp4") != std::string::npos,
                  tool + " names the offline rush and its path: " +
                      offline.error_name + ": " + offline.message);
        }
    }

    // An unsaved project has nowhere to cache an analysis, and no directory
    // its relative media paths resolve against.
    Document unsavedDocument = project.MakeActiveDocument();
    EditLog unsavedLog;
    McpLiveBackend unsaved(unsavedDocument, unsavedLog);
    for (const auto& [tool, arguments] :
         std::vector<std::pair<std::string, std::string>>{
             {"analyze_shot_quality", rushArguments},
             {"read_frame", frameArguments},
             {"measure_color", measureArguments},
             {"contact_sheet", "{}"}}) {
        const McpToolCallOutcome refused = call(unsaved, tool, arguments);
        Check(!refused.ok &&
                  refused.message.find("save the project") != std::string::npos,
              tool + " refuses an unsaved project with a reason: " +
                  refused.message);
    }
    ::setenv("PATH", searchPath.c_str(), 1);
}

// Sends a whole request and hangs up without reading the answer: what a
// client that timed out, or lost its connection, leaves the server with.
void HttpPostAndHangUp(int port, const std::string& body) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }
    const std::string request =
        "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: "
        "application/json\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
    ::send(fd, request.data(), request.size(), 0);
    ::close(fd);
}

// PL2/PL3 -- ROADMAP.md. Edit plans over the real project backend and HTTP:
// a server session keeps a plan between prepare and apply, prepares on a
// timeline it does not activate, and writes nothing before the apply. The
// plan's own freshness is its timeline's revision; the project's revision
// (IA5) stays available as expected_revision, and an apply whose answer
// was lost replays under its idempotency_key.
void CheckEditPlansOverServer(const std::filesystem::path& directory) {
    Project project = Project::FromDocument(Fixture(), "Plans");
    const Ulid active = project.active_timeline_id;
    const Ulid other = "01K3P000000000000000000010";
    DocumentSequence version = project.timelines.front();
    version.id = other;
    version.name = "Version B";
    version.tracks[0].id = "01K3P000000000000000000011";
    version.tracks[0].clips[0].id = "01K3P000000000000000000012";
    version.tracks[0].clips[1].id = "01K3P000000000000000000013";
    project.timelines.push_back(version);
    std::string path;
    std::string error;
    Check(
        CreatePortableProject((directory / "Plans.cutmachine-project").string(),
                              project, path, error),
        "plan fixture package saves: " + error);
    const std::filesystem::path package =
        std::filesystem::path(path).parent_path();
    const auto packageBytes = [&] {
        std::vector<std::filesystem::path> paths;
        for (const auto& entry :
             std::filesystem::recursive_directory_iterator(package)) {
            paths.push_back(entry.path());
        }
        std::sort(paths.begin(), paths.end());
        std::string all;
        for (const auto& item : paths) {
            // The session lock is the server's, not the project's.
            if (item.filename().string().find(".lock") != std::string::npos ||
                item.parent_path().filename().string().find(".lock") !=
                    std::string::npos) {
                continue;
            }
            all += item.lexically_relative(package).string() + '\n';
            if (std::filesystem::is_regular_file(item)) {
                all += Read(item) + '\n';
            }
        }
        return all;
    };
    const auto journalEntries = [&](const Ulid& timelineId) {
        const std::string journalPath =
            path + ".timeline-" + timelineId + ".editlog.json";
        EditLog journal;
        EditError journalError = EditError::None;
        std::string journalMessage;
        if (!std::filesystem::exists(journalPath)) {
            return size_t{0};
        }
        EditLog::Load(journalPath, journal, journalError, journalMessage);
        return journal.AppliedCount();
    };

    McpProjectBackend backend(path);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError), "plan server starts: " + startError);
    int requestId = 900;
    const auto body = [&](const std::string& tool,
                          const std::string& arguments) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
               ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
               "\",\"arguments\":" + arguments + "}}";
    };
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        mcp_json::Value response;
        mcp_json::Value payload;
        std::string failure;
        isError = true;
        if (!mcp_json::Value::Parse(
                HttpBody(
                    HttpPostJson(server.Port(), "/mcp", body(tool, arguments))),
                response, failure)) {
            return payload;
        }
        const mcp_json::Value* result = response.Find("result");
        if (result == nullptr) {
            return payload;
        }
        const mcp_json::Value* flag = result->Find("isError");
        isError = flag == nullptr || flag->AsBool();
        const mcp_json::Value* content = result->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty()) {
            mcp_json::Value::Parse(
                content->AsArray()[0].Find("text")->AsString(), payload,
                failure);
        }
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const std::string& key) {
        const mcp_json::Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    const auto activeTimeline = [&] {
        Project loaded;
        std::string loadError;
        LoadStoredProject(path, loaded, loadError);
        return loaded.active_timeline_id;
    };

    // Prepare on the inactive timeline: nothing written, nothing activated.
    bool isError = true;
    const std::string before = packageBytes();
    const mcp_json::Value described =
        call("describe", "{\"timeline_id\":\"" + other + "\"}", isError);
    const mcp_json::Value* sequence = described.Find("sequence");
    Check(!isError && sequence != nullptr && text(*sequence, "id") == other,
          "describe reads the inactive timeline over MCP");
    const std::string plan =
        "{\"timeline_id\":\"" + other +
        "\",\"label\":\"Version B plus courte\",\"steps\":["
        "{\"tool\":\"remove_clip\",\"arguments\":{\"clip_id\":\"A1\"}},"
        "{\"tool\":\"split_clip\",\"arguments\":{\"clip_id\":\"A2\","
        "\"timeline_position\":{\"value\":15,\"rate\":25}}}]}";
    const mcp_json::Value prepared = call("prepare_edit_plan", plan, isError);
    const std::string planId = text(prepared, "plan_id");
    const std::string projectRevision = text(prepared, "revision");
    Check(!isError && !planId.empty() && projectRevision.size() == 16 &&
              text(prepared, "base_doc_hash").size() == 16,
          "the server prepares a plan and answers with both revisions: " +
              prepared.Dump());
    Check(packageBytes() == before && activeTimeline() == active &&
              journalEntries(other) == 0,
          "no byte of the project changes before the plan is applied");

    // A plan's freshness is its own timeline: an edit of another timeline
    // leaves it applicable, while the project revision moved.
    call("trim_clip",
         "{\"timeline_id\":\"" + active +
             "\",\"clip_id\":\"01K30000000000000000000004\",\"edge\":\"Tail\","
             "\"delta\":{\"value\":-1,\"rate\":25}}",
         isError);
    Check(!isError, "an edit of the active timeline meanwhile");
    mcp_json::Value refused =
        call("apply_edit_plan",
             "{\"plan_id\":\"" + planId + "\",\"expected_revision\":\"" +
                 projectRevision + "\"}",
             isError);
    Check(isError && text(refused, "error") == "StaleRevision" &&
              refused.Find("result") != nullptr &&
              !text(*refused.Find("result"), "current_revision").empty() &&
              journalEntries(other) == 0,
          "the project revision, asked for, refuses the plan like any "
          "mutation: " +
              refused.Dump());

    // The answer to the apply is lost; sent again under its key, it is
    // replayed -- not refused as an unknown plan, not applied twice.
    const std::string apply =
        "{\"plan_id\":\"" + planId + "\",\"idempotency_key\":\"plan-1\"}";
    HttpPostAndHangUp(server.Port(), body("apply_edit_plan", apply));
    for (int attempt = 0; attempt < 400 && journalEntries(other) == 0;
         ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const mcp_json::Value applied = call("apply_edit_plan", apply, isError);
    const mcp_json::Value* replayed = applied.Find("replayed");
    Check(!isError && replayed != nullptr && replayed->AsBool() &&
              text(applied, "status") == "applied" &&
              text(applied, "doc_hash") == text(prepared, "result_doc_hash") &&
              journalEntries(other) == 1 && activeTimeline() == active,
          "a lost apply is replayed, applied once, on its own timeline: " +
              applied.Dump());
    refused =
        call("apply_edit_plan", "{\"plan_id\":\"" + planId + "\"}", isError);
    Check(isError && text(refused, "error") == "UnknownPlan",
          "an applied plan is no longer kept: " + refused.Dump());

    server.Stop();
}

// OR3 -- ROADMAP.md. Caption plans must read their own edited copy, with
// the same cache projection in the standalone and live backends.
void CheckPlannedCaptions(const std::filesystem::path& directory) {
    Document document;
    const Ulid source = "01K3P100000000000000000001";
    const Ulid track = "01K3P100000000000000000002";
    const Ulid clip = "01K3P100000000000000000003";
    document.sequence.id = "01K3P100000000000000000004";
    document.sources = {{source, "speech.wav", {25, 1}, {100, 25}}};
    document.library = {AudioRush(source, "speech.wav")};
    document.sequence.tracks = {
        {track, "audio", 0, {{clip, source, {0, 25}, {50, 25}, {0, 25}}}}};
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "CaptionPlans.cutmachine-project").string(),
              Project::FromDocument(document), path, error),
          "caption plan project saves: " + error);
    const auto cache = std::filesystem::path(path).parent_path() /
                       ".cutmachine" / "transcripts";
    std::filesystem::create_directories(cache);
    Transcript transcript;
    transcript.media_id = source;
    transcript.source_rate = {25, 1};
    transcript.words = {{"Bonjour", {0, 25}, {10, 25}},
                        {"Nouveau", {70, 25}, {80, 25}}};
    Check(SaveAudioTranscript((cache / (source + ".json")).string(), transcript,
                              error),
          "caption source transcript saves: " + error);
    McpProjectBackend disk(path);
    Document liveDocument = document;
    EditLog log;
    McpLiveBackend live(liveDocument, log);
    live.SetProjectPath(path);
    const std::string steps =
        R"({"label":"Captions on draft","steps":[)"
        R"({"tool":"remove_clip","arguments":{"clip_id":")" +
        clip + R"("}},{"tool":"insert_clip","arguments":{"track_id":")" +
        track + R"(","source_id":")" + source +
        R"(","source_in":{"value":60,"rate":25},)"
        R"("duration":{"value":40,"rate":25},)"
        R"("timeline_in":{"value":0,"rate":25}}},)"
        R"({"tool":"generate_captions","arguments":{"max_characters":20}}]})";
    for (McpBackend* backend :
         {static_cast<McpBackend*>(&disk), static_cast<McpBackend*>(&live)}) {
        McpToolRegistry registry;
        mcp_json::Value arguments;
        Check(mcp_json::Value::Parse(steps, arguments, error), error);
        const std::string before = Read(path);
        const auto prepared =
            registry.Call(*backend, "prepare_edit_plan", arguments);
        Check(prepared.ok &&
                  prepared.result_json.find("Nouveau") != std::string::npos &&
                  prepared.result_json.find("Bonjour") == std::string::npos,
              "caption preview projects the draft, not the session: " +
                  prepared.result_json);
        Check(
            Read(path) == before &&
                liveDocument.SaveToString() == document.SaveToString(),
            "caption preparation leaves both saved and live documents intact");
        mcp_json::Value preview;
        Check(mcp_json::Value::Parse(prepared.result_json, preview, error),
              error);
        mcp_json::Value apply = mcp_json::Value::MakeObject();
        if (const auto* id = preview.Find("plan_id")) {
            apply.Set("plan_id", *id);
        }
        const auto applied = registry.Call(*backend, "apply_edit_plan", apply);
        Check(applied.ok, "caption plan applies: " + applied.result_json);
        const auto undone =
            registry.Call(*backend, "undo", mcp_json::Value::MakeObject());
        Document restored;
        Check(undone.ok && backend->SnapshotDocument(restored, error) &&
                  restored.SaveToString() == document.SaveToString(),
              "one undo restores the caption plan byte for byte: " + error);
    }
}

// IA5 -- ROADMAP.md. Tasks, replays and revisions over the real project
// backend and HTTP. No Whisper model runs: the session's media work is
// replaced by a stand-in that holds until released and honours
// cancellation, and the transcription command around it is the real one.
void CheckBackgroundTasksAndReplays(const std::filesystem::path& directory) {
    ::setenv("CUTMACHINE_ENV_FILE", (directory / "absent-ia5.env").c_str(), 1);
    const std::filesystem::path model = directory / "ggml-ia5.bin";
    std::ofstream(model) << "not a model";
    ::setenv("CUTMACHINE_WHISPER_MODEL", model.c_str(), 1);

    const Ulid speech = "01K32000000000000000000001";
    const Ulid audioTrack = "01K32000000000000000000002";
    const Ulid audioClip = "01K32000000000000000000003";
    Document document;
    document.sequence.id = "01K32000000000000000000004";
    document.sequence.name = "Dérushage";
    document.sources = {{speech, "speech.wav", {25, 1}, {1000, 25}}};
    document.library = {AudioRush(speech, "speech.wav")};
    document.sequence.tracks = {
        {audioTrack,
         "audio",
         0,
         {{audioClip, speech, {0, 25}, {100, 25}, {0, 25}}}}};
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Tasks.cutmachine-project").string(),
              Project::FromDocument(document, "Tâches"), path, error),
          "task fixture package saves: " + error);

    std::atomic<int> runs{0};
    std::atomic<bool> release{false};
    McpProjectBackend backend(path);
    McpServer server(backend);
    server.SetIdleTimeoutMilliseconds(300);
    server.Session().SetMediaWorkDecorator(
        [&](MediaTaskKind, const std::string&, MediaTaskManager::Work) {
            return MediaTaskManager::Work([&](MediaTaskContext& context,
                                              std::string& taskError) {
                ++runs;
                while (!release.load()) {
                    if (context.Cancelled()) {
                        taskError = "transcription cancelled";
                        return false;
                    }
                    context.SetProgress(0.5, "Transcription");
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                }
                return true;
            });
        });
    std::string startError;
    Check(server.Start(0, startError), "task server starts: " + startError);
    int requestId = 500;
    const auto body = [&](const std::string& tool,
                          const std::string& arguments) {
        return "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
               ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
               "\",\"arguments\":" + arguments + "}}";
    };
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        mcp_json::Value response;
        mcp_json::Value payload;
        std::string failure;
        isError = true;
        if (!mcp_json::Value::Parse(
                HttpBody(
                    HttpPostJson(server.Port(), "/mcp", body(tool, arguments))),
                response, failure))
            return payload;
        const mcp_json::Value* result = response.Find("result");
        if (result == nullptr) return payload;
        const mcp_json::Value* flag = result->Find("isError");
        isError = flag == nullptr || flag->AsBool();
        const mcp_json::Value* content = result->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty())
            mcp_json::Value::Parse(
                content->AsArray()[0].Find("text")->AsString(), payload,
                failure);
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const std::string& key) {
        const mcp_json::Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    const auto waitFor = [](const std::function<bool()>& condition) {
        for (int attempt = 0; attempt < 400 && !condition(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return condition();
    };
    bool isError = false;

    // ---- disconnect after the start, then resume by the same key ----
    const std::string start =
        "{\"async\":true,\"language\":\"fr\",\"idempotency_key\":\"t-1\"}";
    HttpPostAndHangUp(server.Port(), body("transcribe_timeline", start));
    Check(waitFor([&] { return runs.load() == 1; }),
          "the task started by a call whose answer was lost runs");
    mcp_json::Value handle = call("transcribe_timeline", start, isError);
    const std::string taskId = text(handle, "task_id");
    const mcp_json::Value* replayed = handle.Find("replayed");
    Check(!isError && !taskId.empty() && replayed != nullptr &&
              replayed->AsBool() && text(handle, "state") == "running",
          "sent again with its key, the start returns the running task: " +
              handle.Dump());
    handle = call("get_task", "{\"task_id\":\"" + taskId + "\"}", isError);
    Check(!isError && text(handle, "state") == "running" &&
              text(handle, "detail") == "Transcription",
          "get_task reads the running task's progress: " + handle.Dump());
    release = true;
    handle =
        call("get_task", "{\"task_id\":\"" + taskId + "\",\"wait_seconds\":5}",
             isError);
    const mcp_json::Value* result = handle.Find("result");
    const mcp_json::Value* stale = handle.Find("stale");
    Check(!isError && text(handle, "state") == "succeeded" &&
              result != nullptr && result->Find("words") != nullptr &&
              stale != nullptr && !stale->AsBool(),
          "the result is read after the disconnect: " + handle.Dump());
    Check(runs.load() == 1,
          "the model ran once for the lost start and its "
          "replay");

    // ---- a timeline edited while it is transcribed: named stale ----
    release = false;
    handle = call("transcribe_timeline", "{\"async\":true}", isError);
    const std::string staleId = text(handle, "task_id");
    Check(!isError && waitFor([&] { return runs.load() == 2; }),
          "a second transcription starts");
    mcp_json::Value refused =
        call("transcribe_timeline", "{\"language\":\"fr\"}", isError);
    Check(isError && text(refused, "error") == "AnalysisBusy",
          "a synchronous analysis is refused while a task may rewrite its "
          "caches: " +
              refused.Dump());
    refused = call("open_project", "{\"path\":\"" + path + "\"}", isError);
    Check(
        isError && text(refused, "error") == "TasksRunning",
        "the project is not switched under a running task: " + refused.Dump());
    mcp_json::Value edit =
        call("trim_clip",
             "{\"clip_id\":\"" + audioClip +
                 "\",\"edge\":\"Tail\",\"delta\":{\"value\":-10,\"rate\":25}}",
             isError);
    Check(!isError,
          "the montage is edited while the task runs: " + edit.Dump());
    release = true;
    handle =
        call("get_task", "{\"task_id\":\"" + staleId + "\",\"wait_seconds\":5}",
             isError);
    stale = handle.Find("stale");
    Check(!isError && text(handle, "state") == "succeeded" &&
              stale != nullptr && stale->AsBool() &&
              text(handle, "stale_reason").find(document.sequence.id) !=
                  std::string::npos,
          "a result whose timeline changed before it was installed is named "
          "stale: " +
              handle.Dump());

    // ---- cancellation, bounded and measured ----
    release = false;
    handle = call("transcribe_timeline", "{\"async\":true}", isError);
    const std::string cancelId = text(handle, "task_id");
    Check(waitFor([&] { return runs.load() == 3; }), "a third task runs");
    const auto before = std::chrono::steady_clock::now();
    handle = call("cancel_task", "{\"task_id\":\"" + cancelId + "\"}", isError);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - before);
    std::cout << "IA5 cancel_task answered in " << elapsed.count() << " ms\n";
    Check(!isError && text(handle, "state") == "cancelled" &&
              handle.Find("cancel_requested") == nullptr &&
              elapsed.count() < 1000,
          "cancel_task stops a running task within a second: " + handle.Dump() +
              " in " + std::to_string(elapsed.count()) + " ms");
    handle = call("list_tasks", "{}", isError);
    const mcp_json::Value* tasks = handle.Find("tasks");
    Check(!isError && tasks != nullptr && tasks->AsArray().size() == 3 &&
              text(tasks->AsArray()[0], "task_id") == cancelId,
          "list_tasks lists the three tasks, newest first: " + handle.Dump());

    // ---- a creation whose answer was lost: replayed, not repeated ----
    const auto timelineCount = [&] {
        Project project;
        std::string loadError;
        return LoadStoredProject(path, project, loadError)
                   ? project.timelines.size()
                   : size_t{0};
    };
    const size_t timelinesBefore = timelineCount();
    const std::string add =
        "{\"name\":\"Montage\",\"width\":1080,\"height\":1920,"
        "\"frame_rate\":{\"num\":25,\"den\":1},\"idempotency_key\":\"a-1\"}";
    HttpPostAndHangUp(server.Port(), body("add_timeline", add));
    Check(waitFor([&] { return timelineCount() == timelinesBefore + 1; }),
          "the lost add_timeline created its timeline");
    mcp_json::Value added = call("add_timeline", add, isError);
    replayed = added.Find("replayed");
    Check(!isError && replayed != nullptr && replayed->AsBool() &&
              added.Find("created_timeline_ids") != nullptr &&
              timelineCount() == timelinesBefore + 1,
          "sent again with its key, add_timeline returns its first answer "
          "and creates nothing: " +
              added.Dump());
    refused = call("add_timeline",
                   "{\"name\":\"Autre\",\"width\":1080,\"height\":1920,"
                   "\"frame_rate\":{\"num\":25,\"den\":1},"
                   "\"idempotency_key\":\"a-1\"}",
                   isError);
    Check(isError && text(refused, "error") == "IdempotencyKeyReused" &&
              timelineCount() == timelinesBefore + 1,
          "a key reused for another call is refused: " + refused.Dump());

    // ---- expected revision ----
    mcp_json::Value described = call("list_timelines", "{}", isError);
    const std::string revision = text(described, "revision");
    Check(!isError && revision.size() == 16,
          "every answer carries the project revision: " + described.Dump());
    mcp_json::Value renamed =
        call("add_timeline",
             "{\"name\":\"Version B\",\"width\":1080,\"height\":1920,"
             "\"frame_rate\":{\"num\":25,\"den\":1},\"expected_revision\":\"" +
                 revision + "\"}",
             isError);
    Check(!isError && text(renamed, "revision") != revision &&
              text(renamed, "revision") == text(renamed, "project_hash"),
          "a mutation at the expected revision applies, and its revision is "
          "its project_hash: " +
              renamed.Dump());
    refused = call("add_timeline",
                   "{\"name\":\"Version C\",\"width\":1080,\"height\":1920,"
                   "\"frame_rate\":{\"num\":25,\"den\":1},"
                   "\"expected_revision\":\"" +
                       revision + "\"}",
                   isError);
    const mcp_json::Value* refusal = refused.Find("result");
    Check(isError && text(refused, "error") == "StaleRevision" &&
              refusal != nullptr &&
              text(*refusal, "current_revision") == text(renamed, "revision"),
          "a stale expected revision is refused and names the current one: " +
              refused.Dump());

    // ---- a replay inside an intervention is not a second step ----
    // The edits name their timeline: add_timeline never activated the one
    // it created (PRD17 checks it), but naming is what a series asks.
    mcp_json::Value began =
        call("begin_intervention",
             "{\"timeline_id\":\"" + document.sequence.id + "\"}", isError);
    const std::string token = text(began, "intervention");
    const std::string trim =
        "{\"timeline_id\":\"" + document.sequence.id + "\",\"clip_id\":\"" +
        audioClip +
        "\",\"edge\":\"Tail\",\"delta\":{\"value\":-5,\"rate\":25},"
        "\"idempotency_key\":\"i-1\"}";
    call("trim_clip", trim, isError);
    const bool firstTrim = !isError;
    mcp_json::Value again = call("trim_clip", trim, isError);
    replayed = again.Find("replayed");
    const std::string end = "{\"intervention\":\"" + token +
                            "\",\"label\":\"Resserrer\","
                            "\"idempotency_key\":\"i-end\"}";
    mcp_json::Value ended = call("end_intervention", end, isError);
    const mcp_json::Value* grouped = ended.Find("grouped");
    int64_t groupedCount = 0;
    Check(firstTrim && replayed != nullptr && replayed->AsBool() && !isError &&
              grouped != nullptr && grouped->AsInt64(groupedCount) &&
              groupedCount == 1,
          "a replayed edit adds no step to the intervention: " + ended.Dump());
    ended = call("end_intervention", end, isError);
    replayed = ended.Find("replayed");
    Check(!isError && replayed != nullptr && replayed->AsBool(),
          "a replayed end_intervention returns its fold: " + ended.Dump());

    // ---- a silent connection no longer holds the server ----
    const int silent = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(server.Port()));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Check(::connect(silent, reinterpret_cast<sockaddr*>(&address),
                    sizeof(address)) == 0,
          "a silent client connects");
    const auto silentStart = std::chrono::steady_clock::now();
    call("list_tasks", "{}", isError);
    const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - silentStart);
    ::close(silent);
    std::cout << "IA5 a request behind a silent connection waited "
              << waited.count() << " ms\n";
    Check(!isError && waited.count() < 3000,
          "a request behind a silent connection is served after the idle "
          "timeout: " +
              std::to_string(waited.count()) + " ms");

    server.Stop();
    ::unsetenv("CUTMACHINE_WHISPER_MODEL");

    // ---- every CLI writer waits for no one: it is refused ----
    // The binary itself, so the lock main.mm takes is the one tested.
    ProjectSessionLock resident;
    std::string owner;
    Check(resident.Acquire(path, owner, error),
          "a resident holder takes the project: " + error);
    const std::string beforeWrite = Read(path);
    const std::string operation = SerializeOperation(TrimClipOperation{
        audioClip, TrimEdge::Tail, RationalTime{-1, 25}, std::nullopt});
    const std::filesystem::path operationFile = directory / "ia5-op.json";
    std::ofstream(operationFile) << operation;
    const std::filesystem::path answer = directory / "ia5-answer.json";
    const std::string command = std::string("'") + CUTMACHINE_EXECUTABLE +
                                "' --apply-op '" + path + "' \"$(cat '" +
                                operationFile.string() + "')\" > '" +
                                answer.string() + "'";
    const int refusedStatus = std::system(command.c_str());
    Check(refusedStatus != 0 &&
              Read(answer).find("\"error\":\"IoError\"") != std::string::npos &&
              Read(path) == beforeWrite,
          "--apply-op is refused while another holds the project: " +
              Read(answer));
    resident.Release();
    Check(std::system(command.c_str()) == 0 && Read(path) != beforeWrite &&
              !std::filesystem::exists(path + ".lock"),
          "--apply-op applies once the project is free, and lets it go: " +
              Read(answer));
}

// PRD14 -- ROADMAP.md. A background task reads its arguments the way the
// catalog does. `async: true` used to check the keys against the published
// schema before the catalog put them in its own vocabulary, so a rush named
// `source_id` was refused by every analysis that declares `media_id`, while
// the same call without `async` went through.
// DEC10 -- docs/roadmap/AX-2026-09.md (AX7). The app's endpoint gives its
// session the editor's task manager: an agent relayed to the window has its
// background tasks in the window's task bar, named for it, cancelled there,
// and still read and cancelled by get_task and cancel_task.
void CheckBackgroundTasksOnSharedManager(
    const std::filesystem::path& directory) {
    const Ulid rush = "01K33000000000000000000011";
    LibraryMedia media;
    media.id = rush;
    media.path = "a.mp4";
    media.filename = "a.mp4";
    media.codec = "h264";
    media.width = 160;
    media.height = 90;
    media.orientation = "landscape";
    media.rate = {25, 1};
    media.duration = {50, 25};
    Document document;
    document.sequence.id = "01K33000000000000000000013";
    document.sources = {{rush, "a.mp4", {25, 1}, {50, 25}}};
    document.library = {media};
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "SharedTasks.cutmachine-project").string(),
              Project::FromDocument(document, "Barre des tâches"), path, error),
          "shared-manager fixture package saves: " + error);

    // The editor's manager, then the backend, then the endpoint's server:
    // the session goes first, and waits for its tasks.
    MediaTaskManager editor(2);
    McpProjectBackend backend(path);
    std::atomic<int> runs{0};
    std::atomic<bool> release{false};
    {
        McpServer server(backend);
        server.Session().SetTaskManager(&editor, "Agent · ");
        server.Session().SetMediaWorkDecorator([&](MediaTaskKind,
                                                   const std::string&,
                                                   MediaTaskManager::Work) {
            return MediaTaskManager::Work([&](MediaTaskContext& context,
                                              std::string& taskError) {
                ++runs;
                while (!release.load()) {
                    if (context.Cancelled()) {
                        taskError = "analysis cancelled";
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return true;
            });
        });
        int requestId = 900;
        const auto call = [&](const std::string& tool,
                              const std::string& arguments) {
            std::string response;
            server.HandleJsonRpc(
                "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
                    ",\"method\":\"tools/call\",\"params\":{\"name\":\"" +
                    tool + "\",\"arguments\":" + arguments + "}}",
                response);
            mcp_json::Value parsed;
            mcp_json::Value payload;
            std::string failure;
            if (!mcp_json::Value::Parse(response, parsed, failure) ||
                parsed.Find("result") == nullptr)
                return payload;
            mcp_json::Value::Parse(parsed.Find("result")
                                       ->Find("content")
                                       ->AsArray()[0]
                                       .Find("text")
                                       ->AsString(),
                                   payload, failure);
            return payload;
        };
        const auto text = [](const mcp_json::Value& value,
                             const std::string& key) {
            const mcp_json::Value* field = value.Find(key);
            return field != nullptr && field->IsString() ? field->AsString()
                                                         : std::string();
        };
        const auto inEditor = [&](const std::string& id) {
            for (const MediaTaskSnapshot& task : editor.Snapshot())
                if (task.id == id)
                    return std::optional<MediaTaskSnapshot>(task);
            return std::optional<MediaTaskSnapshot>();
        };

        const std::string first =
            text(call("analyze_shot_quality",
                      R"({"source_id":")" + rush + R"(","async":true})"),
                 "task_id");
        const std::optional<MediaTaskSnapshot> shown = inEditor(first);
        Check(!first.empty() && shown &&
                  shown->label == "Agent · analyze_shot_quality" &&
                  (shown->state == MediaTaskState::Queued ||
                   shown->state == MediaTaskState::Running),
              "the agent's task is in the editor's manager, named for the "
              "task bar");
        // The task bar's cancel: the editor's manager, not the session.
        Check(editor.Cancel(first), "the task bar cancels it");
        const mcp_json::Value cancelled = call(
            "get_task", R"({"task_id":")" + first + R"(","wait_seconds":5})");
        Check(text(cancelled, "state") == "cancelled",
              "get_task reads the cancel made in the task bar: " +
                  cancelled.Dump());

        const std::string second =
            text(call("analyze_shot_quality",
                      R"({"source_id":")" + rush + R"(","async":true})"),
                 "task_id");
        const mcp_json::Value stopped =
            call("cancel_task", R"({"task_id":")" + second + R"("})");
        const std::optional<MediaTaskSnapshot> after = inEditor(second);
        Check(text(stopped, "state") == "cancelled" && after &&
                  after->state == MediaTaskState::Cancelled,
              "cancel_task cancels it in the editor's manager: " +
                  stopped.Dump());

        // Left running as the endpoint goes: cancelled and waited for.
        const std::string third =
            text(call("analyze_shot_quality",
                      R"({"source_id":")" + rush + R"(","async":true})"),
                 "task_id");
        Check(!third.empty(), "a third task starts");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (runs.load() < 3 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    bool thirdOver = true;
    for (const MediaTaskSnapshot& task : editor.Snapshot())
        if (task.state == MediaTaskState::Queued ||
            task.state == MediaTaskState::Running)
            thirdOver = false;
    Check(thirdOver,
          "the session cancelled its running task and waited for "
          "it before it went");
}

void CheckBackgroundTaskSynonyms(const std::filesystem::path& directory) {
    const Ulid first = "01K33000000000000000000001";
    const Ulid second = "01K33000000000000000000002";
    const auto videoRush = [](const Ulid& id, const std::string& file) {
        LibraryMedia media;
        media.id = id;
        media.path = file;
        media.filename = file;
        media.codec = "h264";
        media.width = 160;
        media.height = 90;
        media.orientation = "landscape";
        media.rate = {25, 1};
        media.duration = {50, 25};
        return media;
    };
    Document document;
    document.sequence.id = "01K33000000000000000000003";
    document.sources = {{first, "a.mp4", {25, 1}, {50, 25}},
                        {second, "b.mp4", {25, 1}, {50, 25}}};
    document.library = {videoRush(first, "a.mp4"), videoRush(second, "b.mp4")};
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Synonyms.cutmachine-project").string(),
              Project::FromDocument(document, "Synonymes"), path, error),
          "synonym fixture package saves: " + error);

    std::atomic<int> runs{0};
    std::atomic<bool> release{false};
    McpProjectBackend backend(path);
    McpServer server(backend);
    server.Session().SetMediaWorkDecorator(
        [&](MediaTaskKind, const std::string&, MediaTaskManager::Work) {
            return MediaTaskManager::Work([&](MediaTaskContext& context,
                                              std::string& taskError) {
                ++runs;
                while (!release.load()) {
                    if (context.Cancelled()) {
                        taskError = "analysis cancelled";
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                return true;
            });
        });
    int requestId = 700;
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        std::string response;
        server.HandleJsonRpc(
            "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
                ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
                "\",\"arguments\":" + arguments + "}}",
            response);
        mcp_json::Value parsed;
        mcp_json::Value payload;
        std::string failure;
        isError = true;
        if (!mcp_json::Value::Parse(response, parsed, failure) ||
            parsed.Find("result") == nullptr)
            return payload;
        const mcp_json::Value* result = parsed.Find("result");
        const mcp_json::Value* flag = result->Find("isError");
        isError = flag == nullptr || flag->AsBool();
        mcp_json::Value::Parse(
            result->Find("content")->AsArray()[0].Find("text")->AsString(),
            payload, failure);
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const std::string& key) {
        const mcp_json::Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };

    bool isError = true;
    mcp_json::Value started = call(
        "analyze_shot_quality", R"({"source_id":"M1","async":true})", isError);
    const std::string taskId = text(started, "task_id");
    const mcp_json::Value* arguments = started.Find("arguments");
    Check(!isError && !taskId.empty() && arguments != nullptr &&
              text(*arguments, "media_id") == "M1" &&
              arguments->Find("source_id") == nullptr,
          "an analysis started in the background takes the rush as "
          "source_id, and the task holds it as media_id: " +
              started.Dump());
    release = true;
    const mcp_json::Value finished =
        call("get_task", "{\"task_id\":\"" + taskId + "\",\"wait_seconds\":5}",
             isError);
    const mcp_json::Value* result = finished.Find("result");
    Check(!isError && text(finished, "state") == "succeeded" &&
              result != nullptr && text(*result, "media_id") == first &&
              runs.load() == 1,
          "the task analysed the rush source_id named: " + finished.Dump());

    const mcp_json::Value refused =
        call("analyze_shot_quality",
             R"({"source_id":"M1","media_id":"M2","async":true})", isError);
    Check(isError && text(refused, "error") == "ValidationFailed" &&
              text(refused, "detail").find("source_id") != std::string::npos &&
              text(refused, "detail").find("media_id") != std::string::npos &&
              runs.load() == 1,
          "a rush named twice with two values is refused before anything "
          "is queued: " +
              refused.Dump());
    const mcp_json::Value tasks = call("list_tasks", "{}", isError);
    Check(!isError && tasks.Find("tasks") != nullptr &&
              tasks.Find("tasks")->AsArray().size() == 1,
          "the refused call left no task behind: " + tasks.Dump());
}

// PRD16 -- ROADMAP.md. A stand-in for FFmpeg, for a test that names it in
// CUTMACHINE_FFMPEG: an export is the call that asks for -progress, and a
// capability probe goes unanswered, which refuses nothing. It writes the
// partial file Exporter::Run hands it, reports a frame count as FFmpeg's
// -progress does, and holds while `hold` exists -- so the kill of a running
// encoder and the removal of its partial file are the exporter's own.
std::filesystem::path WriteFfmpegStandIn(const std::filesystem::path& script,
                                         const std::filesystem::path& hold) {
    std::ofstream(script)
        << "#!/bin/sh\n"
           "case \" $* \" in *\" -progress \"*) ;; *) exit 1 ;; esac\n"
           "for last in \"$@\"; do :; done\n"
           "printf 'partial' > \"$last\"\n"
           "while [ -f '"
        << hold.string()
        << "' ]; do\n"
           "  printf 'frame=12\\nprogress=continue\\n'\n"
           "  sleep 0.02\n"
           "done\n"
           "printf 'frame=25\\nprogress=end\\n'\n"
           "exit 0\n";
    std::filesystem::permissions(script,
                                 std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::others_read);
    return script;
}

// PRD16 -- ROADMAP.md. Exports as background tasks, over the real project
// backend and HTTP. FFmpeg is stood in for by a script CUTMACHINE_FFMPEG
// names: it writes the partial file Exporter::Run handed it, reports a frame
// count as FFmpeg's -progress does, and holds while a file exists -- so the
// kill of a running encoder and the removal of its partial file are the
// exporter's own, and no long encode is run. The session's media work
// decorator reaches each file an export encodes; it injects the failure of
// the second file of a batch. One real encode, verified, closes the run.
void CheckBackgroundExports(const std::filesystem::path& directory) {
    const std::filesystem::path folder = directory / "Exports";
    std::filesystem::create_directories(folder);
    const Ulid rush = "01K34000000000000000000001";
    LibraryMedia media;
    media.id = rush;
    media.path = "rush.mp4";
    media.filename = "rush.mp4";
    media.codec = "h264";
    media.width = 160;
    media.height = 90;
    media.rate = {25, 1};
    media.duration = {25, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 1;
    Document document;
    document.sequence.id = "01K34000000000000000000002";
    document.sequence.name = "Série";
    document.sequence.width = 160;
    document.sequence.height = 90;
    document.sequence.frame_rate = {25, 1};
    document.library = {media};
    document.sources = {{rush, "rush.mp4", {25, 1}, {25, 25}}};
    const Ulid clip = "01K34000000000000000000004";
    document.sequence.tracks = {{"01K34000000000000000000003",
                                 "video",
                                 0,
                                 {{clip, rush, {0, 25}, {25, 25}, {0, 25}}}}};
    std::string path;
    std::string error;
    Check(CreatePortableProject((folder / "Serie.cutmachine-project").string(),
                                Project::FromDocument(document, "Série"), path,
                                error),
          "export fixture package saves: " + error);
    const std::filesystem::path package =
        std::filesystem::path(path).parent_path();
    const std::string generate =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=160x90:rate=25:duration=1' "
        "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
        "-c:v libx264 -pix_fmt yuv420p -c:a aac -ac 1 -shortest -y '" +
        (package / "rush.mp4").string() + "'";
    Check(std::system(generate.c_str()) == 0, "FFmpeg generates the rush");

    const std::filesystem::path hold = directory / "prd16-hold";
    const std::filesystem::path encoder =
        WriteFfmpegStandIn(directory / "prd16-ffmpeg", hold);
    ::setenv("CUTMACHINE_FFMPEG", encoder.c_str(), 1);

    std::atomic<bool> failSecond{false};
    std::atomic<int> exportWorks{0};
    std::mutex labelsMutex;
    std::vector<std::string> labels;
    McpProjectBackend backend(path);
    McpServer server(backend);
    server.Session().SetMediaWorkDecorator([&](MediaTaskKind kind,
                                               const std::string& label,
                                               MediaTaskManager::Work work) {
        if (kind != MediaTaskKind::Export) {
            return work;
        }
        return MediaTaskManager::Work(
            [&, label, work](MediaTaskContext& context,
                             std::string& taskError) {
                {
                    std::lock_guard<std::mutex> lock(labelsMutex);
                    labels.push_back(label);
                }
                if (failSecond.load() && ++exportWorks == 2) {
                    taskError = "stand-in failure of the second file";
                    return false;
                }
                return work(context, taskError);
            });
    });
    Check(server.Start(0, error), "export task server starts: " + error);
    int requestId = 1600;
    const auto call = [&](const std::string& tool, const std::string& arguments,
                          bool& isError) {
        mcp_json::Value response;
        mcp_json::Value payload;
        std::string failure;
        isError = true;
        const std::string body =
            "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
            ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
            "\",\"arguments\":" + arguments + "}}";
        if (!mcp_json::Value::Parse(
                HttpBody(HttpPostJson(server.Port(), "/mcp", body)), response,
                failure) ||
            response.Find("result") == nullptr)
            return payload;
        const mcp_json::Value* result = response.Find("result");
        const mcp_json::Value* flag = result->Find("isError");
        isError = flag == nullptr || flag->AsBool();
        const mcp_json::Value* content = result->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty())
            mcp_json::Value::Parse(
                content->AsArray()[0].Find("text")->AsString(), payload,
                failure);
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const std::string& key) {
        const mcp_json::Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    // Generous: three builds may share the machine with this run.
    const auto waitFor = [](const std::function<bool()>& condition) {
        for (int attempt = 0; attempt < 2000 && !condition(); ++attempt)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return condition();
    };
    const auto partials = [&] {
        size_t count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(folder))
            if (entry.path().filename().string().find(".part.") !=
                std::string::npos)
                ++count;
        return count;
    };
    const std::string timeline = document.sequence.id;
    bool isError = true;

    // ---- refused before anything is queued ----
    mcp_json::Value refused = call(
        "export_timeline", R"({"output_path":"x.mp4","async":true})", isError);
    Check(isError && text(refused, "error") == "ValidationFailed" &&
              text(refused, "detail").find("'Série' (" + timeline + ")") !=
                  std::string::npos,
          "a background export naming no timeline is refused with the "
          "timelines there are: " +
              refused.Dump());
    refused = call("export_timeline",
                   R"({"timeline_id":")" + timeline +
                       R"(","output_path":"x.mp4","preset":"h264",)"
                       R"("codec":"hevc","async":true})",
                   isError);
    Check(isError && text(refused, "error") == "ValidationFailed" &&
              text(refused, "detail").find("contradicts codec 'hevc'") !=
                  std::string::npos,
          "a preset that contradicts the codec is refused before the task "
          "is queued: " +
              refused.Dump());

    // PRD39 -- ROADMAP.md. export_cdl writes the file its caller names: a
    // relative path lands in the project's folder, as an export's does,
    // and the answer names where. It landed in the server's directory.
    const mcp_json::Value cdl =
        call("export_cdl",
             R"({"timeline_id":")" + timeline + R"(","clip_ids":[")" + clip +
                 R"("],"path":"grades/../grade.cdl"})",
             isError);
    Check(!isError && text(cdl, "written") == (folder / "grade.cdl").string() &&
              std::filesystem::exists(folder / "grade.cdl"),
          "export_cdl writes a relative path in the project's folder: " +
              cdl.Dump());

    // ---- progress, then cancellation: the partial file goes ----
    std::ofstream(hold) << "hold";
    mcp_json::Value handle = call("export_timeline",
                                  R"({"timeline_id":")" + timeline +
                                      R"(","output_path":"held.mp4",)"
                                      R"("software":true,"async":true})",
                                  isError);
    const std::string heldId = text(handle, "task_id");
    Check(
        !isError && !heldId.empty(),
        "export_timeline with async answers with a task_id: " + handle.Dump());
    mcp_json::Value read;
    Check(waitFor([&] {
              read =
                  call("get_task", "{\"task_id\":\"" + heldId + "\"}", isError);
              return text(read, "detail").find("frames 12/25 of ") !=
                     std::string::npos;
          }),
          "get_task reads the frames encoded out of the total: " + read.Dump());
    Check(text(read, "state") == "running" &&
              text(read, "detail") ==
                  "frames 12/25 of " + (folder / "held.mp4").string() &&
              partials() == 1,
          "the export runs, into a partial file beside its destination, in "
          "the project's folder: " +
              read.Dump());
    const auto cancelStart = std::chrono::steady_clock::now();
    handle = call("cancel_task", "{\"task_id\":\"" + heldId + "\"}", isError);
    const auto cancelled =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - cancelStart);
    std::cout << "PRD16 cancel_task stopped an export in " << cancelled.count()
              << " ms\n";
    Check(!isError && text(handle, "state") == "cancelled" && partials() == 0 &&
              !std::filesystem::exists(folder / "held.mp4") &&
              cancelled.count() < 2000,
          "cancel_task stops the encoder and the partial file goes with it: " +
              handle.Dump());

    // ---- the project as it was asked on, and `stale` after an edit ----
    handle = call("export_timeline",
                  R"({"timeline_id":")" + timeline +
                      R"(","output_path":"film.mp4","async":true})",
                  isError);
    const std::string filmId = text(handle, "task_id");
    Check(waitFor([&] { return partials() == 1; }),
          "a second export is encoding");
    const mcp_json::Value trimmed =
        call("trim_clip",
             "{\"timeline_id\":\"" + timeline + "\",\"clip_id\":\"" + clip +
                 "\",\"edge\":\"Tail\",\"delta\":{\"value\":-10,\"rate\":25}}",
             isError);
    Check(!isError,
          "the montage is edited while it is exported: " + trimmed.Dump());
    std::filesystem::remove(hold);
    handle =
        call("get_task", "{\"task_id\":\"" + filmId + "\",\"wait_seconds\":10}",
             isError);
    const mcp_json::Value* result = handle.Find("result");
    const mcp_json::Value* stale = handle.Find("stale");
    int64_t frames = 0;
    Check(!isError && text(handle, "state") == "succeeded" &&
              result != nullptr &&
              text(*result, "path") == (folder / "film.mp4").string() &&
              text(*result, "codec") == "h264" &&
              result->Find("frames") != nullptr &&
              result->Find("frames")->AsInt64(frames) && frames == 25 &&
              std::filesystem::exists(folder / "film.mp4") &&
              stale != nullptr && stale->AsBool() &&
              text(handle, "stale_reason").find(timeline) != std::string::npos,
          "the export encodes the 25 frames the timeline had when it was "
          "asked for, at the absolute path in the project's folder, and says "
          "the timeline moved since: " +
              handle.Dump());

    // ---- PRD40: an export sent again under its key is not run twice ----
    const std::string keyed = R"({"timeline_id":")" + timeline +
                              R"(","output_path":"keyed.mp4","async":true,)"
                              R"("idempotency_key":"export-keyed"})";
    handle = call("export_timeline", keyed, isError);
    const std::string keyedId = text(handle, "task_id");
    const bool keyedStarted = !isError && !keyedId.empty();
    mcp_json::Value again = call("export_timeline", keyed, isError);
    const mcp_json::Value* replayedExport = again.Find("replayed");
    Check(keyedStarted && !isError && text(again, "task_id") == keyedId &&
              replayedExport != nullptr && replayedExport->AsBool(),
          "an export sent again under its key answers its own task: " +
              again.Dump());
    handle =
        call("get_task",
             "{\"task_id\":\"" + keyedId + "\",\"wait_seconds\":10}", isError);
    Check(!isError && text(handle, "state") == "succeeded" &&
              std::filesystem::exists(folder / "keyed.mp4"),
          "the keyed export runs once, to its end: " + handle.Dump());
    const std::string batchKeyed =
        R"({"preview":true,"idempotency_key":"batch-keyed","outputs":[)"
        R"({"timeline_id":")" +
        timeline + R"(","output_path":"preview.mp4"}]})";
    call("export_timelines", batchKeyed, isError);
    const bool batchFirst = !isError;
    again = call("export_timelines", batchKeyed, isError);
    replayedExport = again.Find("replayed");
    Check(batchFirst && !isError && replayedExport != nullptr &&
              replayedExport->AsBool(),
          "export_timelines takes a key too: " + again.Dump());

    // ---- a batch of three, the second failing ----
    failSecond = true;
    handle = call("export_timelines",
                  R"({"preview":false,"async":true,"outputs":[)"
                  R"({"timeline_id":")" +
                      timeline +
                      R"(","output_path":"one.mp4"},)"
                      R"({"timeline_id":")" +
                      timeline +
                      R"(","output_path":"two.mp4"},)"
                      R"({"timeline_id":")" +
                      timeline + R"(","output_path":"three.mp4"}]})",
                  isError);
    const std::string batchId = text(handle, "task_id");
    handle =
        call("get_task",
             "{\"task_id\":\"" + batchId + "\",\"wait_seconds\":10}", isError);
    failSecond = false;
    const mcp_json::Value* failure = handle.Find("failure");
    const mcp_json::Value* report =
        failure != nullptr ? failure->Find("result") : nullptr;
    const mcp_json::Value* outputs =
        report != nullptr ? report->Find("outputs") : nullptr;
    int64_t completed = -1;
    Check(!isError && !batchId.empty() && text(handle, "state") == "failed" &&
              report != nullptr && report->Find("completed") != nullptr &&
              report->Find("completed")->AsInt64(completed) && completed == 1 &&
              outputs != nullptr && outputs->AsArray().size() == 2 &&
              text(outputs->AsArray()[0], "status") == "complete" &&
              text(outputs->AsArray()[1], "detail") ==
                  "stand-in failure of the second file" &&
              std::filesystem::exists(folder / "one.mp4") &&
              !std::filesystem::exists(folder / "two.mp4") &&
              !std::filesystem::exists(folder / "three.mp4") && partials() == 0,
          "a batch that fails at its second file keeps and lists the first, "
          "as a synchronous one does: " +
              handle.Dump());
    {
        std::lock_guard<std::mutex> lock(labelsMutex);
        Check(labels.size() >= 2 &&
                  labels[labels.size() - 2] ==
                      "Export " + (folder / "one.mp4").string() &&
                  labels.back() == "Export " + (folder / "two.mp4").string(),
              "each file of the batch is one piece of the task's media work, "
              "which the session's decorator reaches");
    }

    // ---- a motion composition rendered in the background ----
    InstallMotionFrameRenderer();
    const mcp_json::Value added =
        call("add_motion_composition",
             R"({"name":"Fond","definition":{"width":64,"height":64,"fps":25,)"
             R"("duration":5,"root":{"type":"fill","style":)"
             R"({"backgroundColor":"#101820"}}}})",
             isError);
    Project project;
    Check(!isError && LoadStoredProject(path, project, error) &&
              project.compositions.size() == 1,
          "a composition is added: " + added.Dump());
    if (project.compositions.size() == 1) {
        handle = call("render_motion_video",
                      R"({"composition_id":")" +
                          project.compositions.front().id.substr(0, 20) +
                          R"(","output":"fond.mp4","preview":false,)"
                          R"("async":true})",
                      isError);
        const std::string renderId = text(handle, "task_id");
        handle = call("get_task",
                      "{\"task_id\":\"" + renderId + "\",\"wait_seconds\":10}",
                      isError);
        result = handle.Find("result");
        outputs = result != nullptr ? result->Find("outputs") : nullptr;
        stale = handle.Find("stale");
        Check(!isError && !renderId.empty() &&
                  text(handle, "state") == "succeeded" && outputs != nullptr &&
                  outputs->AsArray().size() == 1 &&
                  text(outputs->AsArray()[0], "status") == "complete" &&
                  std::filesystem::exists(folder / "fond.mp4") &&
                  stale != nullptr && !stale->AsBool(),
              "render_motion_video runs in the background, by a prefix of "
              "the composition's id, into the project's folder: " +
                  handle.Dump());
    }

    // ---- the app's backend takes the analyses' path ----
    // What the relay to the window runs, less the main queue it reads the
    // window through (MainThreadBackend.h): the project the window holds,
    // pinned, and the package's fork encoding on the task's worker.
    {
        Project held;
        Check(LoadStoredProject(path, held, error),
              "the window's project loads: " + error);
        Document liveDocument = held.MakeActiveDocument();
        EditLog liveLog;
        McpLiveBackend live(liveDocument, liveLog);
        live.SetProjectPath(path);
        McpServer liveServer(live);
        const auto liveCall = [&](const std::string& tool,
                                  const std::string& arguments) {
            std::string response;
            liveServer.HandleJsonRpc(
                "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
                    ",\"method\":\"tools/call\",\"params\":{\"name\":\"" +
                    tool + "\",\"arguments\":" + arguments + "}}",
                response);
            mcp_json::Value parsed;
            mcp_json::Value payload;
            std::string failure;
            if (mcp_json::Value::Parse(response, parsed, failure) &&
                parsed.Find("result") != nullptr)
                mcp_json::Value::Parse(parsed.Find("result")
                                           ->Find("content")
                                           ->AsArray()[0]
                                           .Find("text")
                                           ->AsString(),
                                       payload, failure);
            return payload;
        };
        const mcp_json::Value started =
            liveCall("export_timeline", R"({"timeline_id":")" + timeline +
                                            R"(","output_path":"live.mp4",)"
                                            R"("async":true})");
        const mcp_json::Value finished =
            liveCall("get_task", "{\"task_id\":\"" + text(started, "task_id") +
                                     "\",\"wait_seconds\":10}");
        const mcp_json::Value* exported = finished.Find("result");
        Check(text(finished, "state") == "succeeded" && exported != nullptr &&
                  text(*exported, "path") == (folder / "live.mp4").string() &&
                  std::filesystem::exists(folder / "live.mp4"),
              "the app's backend exports in the background, into the "
              "folder of the window's package: " +
                  started.Dump() + " " + finished.Dump());
    }

    // ---- a relative path from --call-tool is the shell's ----
    const std::filesystem::path shell = directory / "prd16-shell";
    std::filesystem::create_directories(shell);
    const std::filesystem::path previous = std::filesystem::current_path();
    std::filesystem::current_path(shell);
    std::string output;
    const int status = CallToolCommand(
        path, "export_timeline",
        R"({"timeline_id":")" + timeline + R"(","output_path":"cli.mp4"})",
        output);
    std::filesystem::current_path(previous);
    Check(
        status == 0 &&
            output.find(
                "\"path\":\"" +
                std::filesystem::weakly_canonical(shell / "cli.mp4").string() +
                "\"") != std::string::npos &&
            !std::filesystem::exists(folder / "cli.mp4"),
        "--call-tool, typed in a shell, keeps the shell's directory: " +
            output);

    // ---- one real encode, verified in the same task ----
    ::setenv("CUTMACHINE_FFMPEG", FFMPEG_EXECUTABLE, 1);
    handle = call("export_timeline",
                  R"({"timeline_id":")" + timeline +
                      R"(","output_path":"real.mp4","software":true,)"
                      R"("verify":true,"async":true})",
                  isError);
    const std::string realId = text(handle, "task_id");
    handle =
        call("get_task", "{\"task_id\":\"" + realId + "\",\"wait_seconds\":30}",
             isError);
    result = handle.Find("result");
    stale = handle.Find("stale");
    Check(
        !isError && text(handle, "state") == "succeeded" && result != nullptr &&
            result->Find("verification") != nullptr &&
            text(*result, "profile") == "high" &&
            std::filesystem::exists(folder / "real.mp4") && stale != nullptr &&
            !stale->AsBool(),
        "a real background export carries its verification: " + handle.Dump());
    // DEC19 -- docs/roadmap/PRODUIT-2026-10b.md. The cartons of that file
    // are measured by the name export_timeline was given: a relative path
    // is the project folder's, as for verify_export, and no longer this
    // process's working directory, where no real.mp4 is.
    const mcp_json::Value legibility = call(
        "check_caption_legibility",
        R"({"timeline_id":")" + timeline + R"(","path":"real.mp4"})", isError);
    int64_t measured = -1;
    Check(!isError && !std::filesystem::exists("real.mp4") &&
              legibility.Find("cartons") != nullptr &&
              legibility.Find("cartons")->AsInt64(measured) && measured == 0,
          "check_caption_legibility reads a relative path in the project's "
          "folder: " +
              legibility.Dump());
    ::unsetenv("CUTMACHINE_FFMPEG");
    server.Stop();
}

// PRD17 -- ROADMAP.md. A series: N agents editing N timelines of one project
// through one server, their calls interleaved -- sub-agents sharing a Claude
// Code session's stdio server, or N HTTP connections. Three clients, three
// timelines, in an order chosen to hurt: alternate edits, a plan prepared on
// one timeline while another moves, an intervention folded while another
// client edits, exports and an analysis in the background while the montage
// goes on, a client that forgets its timeline. After every call the
// timelines its client does not own are, byte for byte, what they were; at
// the end each timeline holds exactly its client's edits. Client B speaks
// HTTP, A and C go through the server's JSON-RPC entry point: both reach the
// session the same way.
void CheckSeriesOverOneServer(const std::filesystem::path& directory) {
    const std::filesystem::path folder = directory / "Series";
    std::filesystem::create_directories(folder);
    const Ulid rush = "01K35000000000000000000001";
    const Ulid timelineA = "01K35000000000000000000002";
    const Ulid timelineB = "01K35000000000000000000003";
    const Ulid timelineC = "01K35000000000000000000004";
    LibraryMedia media;
    media.id = rush;
    media.path = "rush.mp4";
    media.filename = "rush.mp4";
    media.codec = "h264";
    media.width = 160;
    media.height = 90;
    media.rate = {25, 1};
    media.duration = {100, 25};
    media.orientation = "landscape";
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 1;
    Document document;
    document.sequence.id = timelineA;
    document.sequence.name = "A";
    document.sequence.width = 160;
    document.sequence.height = 90;
    document.sequence.frame_rate = {25, 1};
    document.library = {media};
    document.sources = {{rush, "rush.mp4", {25, 1}, {100, 25}}};
    document.sequence.tracks = {{"01K35000000000000000000005", "video", 0, {}},
                                {"01K35000000000000000000006", "audio", 1, {}}};
    Project project = Project::FromDocument(document, "Série");
    // propose_grade measures in the working space.
    project.settings.color_management.enabled = true;
    DocumentSequence other = document.sequence;
    other.id = timelineB;
    other.name = "B";
    other.tracks[0].id = "01K35000000000000000000007";
    other.tracks[1].id = "01K35000000000000000000008";
    project.timelines.push_back(other);
    other.id = timelineC;
    other.name = "C";
    other.tracks[0].id = "01K35000000000000000000009";
    other.tracks[1].id = "01K3500000000000000000000A";
    project.timelines.push_back(other);
    project.active_timeline_id = timelineA;
    std::string path;
    std::string error;
    Check(CreatePortableProject((folder / "Serie.cutmachine-project").string(),
                                project, path, error),
          "series fixture package saves: " + error);
    const std::string generate =
        std::string("'") + FFMPEG_EXECUTABLE +
        "' -hide_banner -loglevel error "
        "-f lavfi -i 'testsrc2=size=160x90:rate=25:duration=4' "
        "-f lavfi -i 'sine=frequency=330:sample_rate=48000:duration=4' "
        "-vf 'eq=brightness=-0.3' -c:v libx264 -pix_fmt yuv420p -c:a aac "
        "-ac 1 -shortest -y '" +
        (std::filesystem::path(path).parent_path() / "rush.mp4").string() + "'";
    Check(std::system(generate.c_str()) == 0,
          "FFmpeg generates the series rush");

    std::atomic<bool> holdAnalysis{false};
    McpProjectBackend backend;
    McpServer server(backend);
    server.Session().SetMediaWorkDecorator([&](MediaTaskKind kind,
                                               const std::string&,
                                               MediaTaskManager::Work work) {
        if (kind != MediaTaskKind::Beat) {
            return work;
        }
        return MediaTaskManager::Work(
            [&](MediaTaskContext& context, std::string& taskError) {
                while (holdAnalysis.load()) {
                    if (context.Cancelled()) {
                        taskError = "analysis cancelled";
                        return false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
                taskError = "stand-in: no analysis";
                return false;
            });
    });
    Check(server.Start(0, error), "series server starts: " + error);

    struct Client {
        std::string name;
        Ulid timeline;
        bool http;
    };
    const Client clientA{"A", timelineA, false};
    const Client clientB{"B", timelineB, true};
    const Client clientC{"C", timelineC, false};
    const std::vector<Ulid> owned = {timelineA, timelineB, timelineC};
    const auto timelineBytes = [&] {
        std::map<Ulid, std::string> bytes;
        Project loaded;
        std::string loadError;
        if (LoadStoredProject(path, loaded, loadError)) {
            for (const Ulid& id : owned) {
                if (const DocumentSequence* timeline =
                        loaded.FindTimeline(id)) {
                    Document alone;
                    alone.sequence = *timeline;
                    bytes[id] = alone.SaveToString();
                }
            }
        }
        return bytes;
    };
    int requestId = 1700;
    const auto send = [&](bool http, const std::string& tool,
                          const std::string& arguments, bool& isError) {
        const std::string body =
            "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
            ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + tool +
            "\",\"arguments\":" + arguments + "}}";
        std::string response;
        if (http) {
            response = HttpBody(HttpPostJson(server.Port(), "/mcp", body));
        } else {
            server.HandleJsonRpc(body, response);
        }
        mcp_json::Value parsed;
        mcp_json::Value payload;
        std::string failure;
        isError = true;
        if (!mcp_json::Value::Parse(response, parsed, failure) ||
            parsed.Find("result") == nullptr)
            return payload;
        const mcp_json::Value* result = parsed.Find("result");
        const mcp_json::Value* flag = result->Find("isError");
        isError = flag == nullptr || flag->AsBool();
        const mcp_json::Value* content = result->Find("content");
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty())
            mcp_json::Value::Parse(
                content->AsArray()[0].Find("text")->AsString(), payload,
                failure);
        return payload;
    };
    // One call of one client (none for a call that must change nothing):
    // every timeline it does not own is byte for byte as it was.
    const auto call = [&](const Client* client, const std::string& tool,
                          const std::string& arguments, bool& isError) {
        const std::map<Ulid, std::string> before = timelineBytes();
        mcp_json::Value payload =
            send(client != nullptr && client->http, tool, arguments, isError);
        const std::map<Ulid, std::string> after = timelineBytes();
        for (const Ulid& id : owned) {
            if (client != nullptr && id == client->timeline) {
                continue;
            }
            Check(before.count(id) != 0 && after.count(id) != 0 &&
                      before.at(id) == after.at(id),
                  "series: " + (client != nullptr ? client->name : "nobody") +
                      "'s " + tool + " leaves timeline " + id +
                      " as it was: " + payload.Dump().substr(0, 400));
        }
        return payload;
    };
    const auto text = [](const mcp_json::Value& value, const std::string& key) {
        const mcp_json::Value* field = value.Find(key);
        return field != nullptr && field->IsString() ? field->AsString()
                                                     : std::string();
    };
    const auto createdClip = [](const mcp_json::Value& payload) {
        const mcp_json::Value* delta = payload.Find("delta");
        const mcp_json::Value* clips =
            delta != nullptr ? delta->Find("clips") : nullptr;
        if (clips == nullptr || !clips->IsArray()) return std::string();
        for (const mcp_json::Value& clip : clips->AsArray()) {
            const mcp_json::Value* created = clip.Find("created");
            const mcp_json::Value* id = clip.Find("clip_id");
            if (created != nullptr && created->AsBool() && id != nullptr)
                return id->AsString();
        }
        return std::string();
    };
    const auto insert = [&](const Client& client, int64_t sourceIn,
                            int64_t duration, int64_t at) {
        bool isError = true;
        const mcp_json::Value payload =
            call(&client, "insert_clip",
                 "{\"timeline_id\":\"" + client.timeline +
                     "\",\"track_id\":\"T1\",\"source_id\":\"M1\","
                     "\"source_in\":{\"value\":" +
                     std::to_string(sourceIn) +
                     ",\"rate\":25},\"duration\":{\"value\":" +
                     std::to_string(duration) +
                     ",\"rate\":25},\"timeline_in\":{\"value\":" +
                     std::to_string(at) + ",\"rate\":25}}",
                 isError);
        Check(!isError, client.name + " lays a shot on its timeline: " +
                            payload.Dump().substr(0, 300));
        return createdClip(payload);
    };
    const auto trimArguments = [](const Client& client, const std::string& clip,
                                  int64_t frames) {
        return "{\"timeline_id\":\"" + client.timeline + "\",\"clip_id\":\"" +
               clip + "\",\"edge\":\"Tail\",\"delta\":{\"value\":" +
               std::to_string(frames) + ",\"rate\":25}}";
    };
    const auto trim = [&](const Client& client, const std::string& clip,
                          int64_t frames) {
        bool isError = true;
        const mcp_json::Value payload = call(
            &client, "trim_clip", trimArguments(client, clip, frames), isError);
        Check(!isError, client.name + " trims its shot: " +
                            payload.Dump().substr(0, 300));
        return payload;
    };
    const auto activeTimeline = [&] {
        Project loaded;
        std::string loadError;
        LoadStoredProject(path, loaded, loadError);
        return loaded.active_timeline_id;
    };
    const auto namesEveryTimeline = [&](const std::string& detail) {
        return detail.find("'A' (" + timelineA + ")") != std::string::npos &&
               detail.find("'B' (" + timelineB + ")") != std::string::npos &&
               detail.find("'C' (" + timelineC + ")") != std::string::npos;
    };
    bool isError = true;

    // ---- the series opens the project in strict mode ----
    mcp_json::Value payload =
        call(nullptr, "open_project",
             "{\"path\":\"" + path + "\",\"require_timeline\":true}", isError);
    const mcp_json::Value* strict = payload.Find("require_timeline");
    Check(!isError && strict != nullptr && strict->AsBool(),
          "open_project turns strict mode on for this server and says so: " +
              payload.Dump());
    if (isError) {
        payload = call(nullptr, "open_project", "{\"path\":\"" + path + "\"}",
                       isError);
    }

    // ---- a client that forgets its timeline ----
    payload = call(nullptr, "insert_clip",
                   "{\"track_id\":\"T1\",\"source_id\":\"M1\","
                   "\"source_in\":{\"value\":0,\"rate\":25},"
                   "\"duration\":{\"value\":10,\"rate\":25},"
                   "\"timeline_in\":{\"value\":0,\"rate\":25}}",
                   isError);
    Check(isError && text(payload, "error") == "TimelineRequired" &&
              namesEveryTimeline(text(payload, "detail")),
          "an edit naming no timeline is refused, with the timelines there "
          "are: " +
              payload.Dump());
    payload = call(nullptr, "describe", "{}", isError);
    Check(isError && text(payload, "error") == "TimelineRequired" &&
              namesEveryTimeline(text(payload, "detail")),
          "so is a read: the active timeline means nothing in a series: " +
              payload.Dump());
    // PRD40 -- ROADMAP.md. A rush is no timeline: its frame is read without
    // naming one, even in strict mode.
    payload = call(nullptr, "read_frame",
                   "{\"media_id\":\"M1\",\"source_in\":{\"value\":10,"
                   "\"rate\":25}}",
                   isError);
    Check(!isError && text(payload, "source_id").size() == 26,
          "a frame of a rush is read in strict mode without a timeline: " +
              payload.Dump().substr(0, 300));

    // ---- alternate edits ----
    const std::string clipA1 = insert(clientA, 0, 25, 0);
    const std::string clipB1 = insert(clientB, 25, 25, 0);
    const std::string clipC1 = insert(clientC, 50, 25, 0);
    trim(clientA, clipA1, -5);
    trim(clientC, clipC1, -5);
    trim(clientB, clipB1, -5);

    // ---- A's plan survives B's edit ----
    payload = call(&clientA, "prepare_edit_plan",
                   "{\"timeline_id\":\"" + timelineA +
                       "\",\"label\":\"A plus court\",\"steps\":[{\"tool\":"
                       "\"trim_clip\",\"arguments\":{\"clip_id\":\"" +
                       clipA1 +
                       "\",\"edge\":\"Tail\",\"delta\":{\"value\":-2,"
                       "\"rate\":25}}}]}",
                   isError);
    const std::string planA = text(payload, "plan_id");
    Check(!isError && !planA.empty(),
          "A prepares a plan on its timeline: " + payload.Dump());
    const std::string clipB2 = insert(clientB, 0, 10, 20);
    payload = call(&clientA, "apply_edit_plan",
                   "{\"plan_id\":\"" + planA + "\"}", isError);
    Check(
        !isError && text(payload, "status") == "applied",
        "A's plan applies after B edited its own timeline: " + payload.Dump());

    // ---- C's intervention folds C's steps only ----
    const std::map<Ulid, std::string> beforeIntervention = timelineBytes();
    payload = call(&clientC, "begin_intervention",
                   "{\"timeline_id\":\"" + timelineC + "\"}", isError);
    const std::string token = text(payload, "intervention");
    Check(!isError && !token.empty(),
          "C opens an intervention: " + payload.Dump());
    const std::string clipC2 = insert(clientC, 0, 10, 20);
    trim(clientB, clipB2, -2);
    trim(clientC, clipC2, -3);
    payload =
        call(&clientC, "end_intervention",
             "{\"intervention\":\"" + token + "\",\"label\":\"C resserré\"}",
             isError);
    int64_t grouped = 0;
    Check(!isError && payload.Find("grouped") != nullptr &&
              payload.Find("grouped")->AsInt64(grouped) && grouped == 2,
          "C's intervention folds C's two steps, not B's: " + payload.Dump());
    payload = call(&clientC, "undo", "{\"timeline_id\":\"" + timelineC + "\"}",
                   isError);
    Check(!isError &&
              timelineBytes().at(timelineC) == beforeIntervention.at(timelineC),
          "one undo takes C's intervention back, and only it: " +
              payload.Dump().substr(0, 300));

    // ---- exports in the background while the montage goes on ----
    const std::filesystem::path hold = directory / "prd17-hold";
    ::setenv("CUTMACHINE_FFMPEG",
             WriteFfmpegStandIn(directory / "prd17-ffmpeg", hold).c_str(), 1);
    std::ofstream(hold) << "hold";
    payload = call(&clientA, "export_timeline",
                   "{\"timeline_id\":\"" + timelineA +
                       "\",\"output_path\":\"A.mp4\",\"async\":true}",
                   isError);
    const std::string exportA = text(payload, "task_id");
    payload = call(&clientB, "export_timeline",
                   "{\"timeline_id\":\"" + timelineB +
                       "\",\"output_path\":\"B.mp4\",\"async\":true}",
                   isError);
    const std::string exportB = text(payload, "task_id");
    Check(!exportA.empty() && !exportB.empty(),
          "A and B export in the background: " + payload.Dump());
    payload =
        call(nullptr, "open_project", "{\"path\":\"" + path + "\"}", isError);
    Check(isError && text(payload, "error") == "TasksRunning" &&
              text(payload, "detail").find(exportA) != std::string::npos &&
              text(payload, "detail").find("export_timeline") !=
                  std::string::npos &&
              text(payload, "detail").find("get_task") != std::string::npos,
          "the project is not switched under the exports, and the refusal "
          "names them and what to do: " +
              payload.Dump());
    const std::string clipC3 = insert(clientC, 30, 10, 20);
    const std::string clipB3 = insert(clientB, 60, 5, 28);
    // What the timelines share, changed through B's own timeline: a bin no
    // export reads. A's export stays current (B's is stale for its shot).
    payload =
        call(&clientB, "add_bin",
             "{\"timeline_id\":\"" + timelineB + "\",\"name\":\"Rushes de B\"}",
             isError);
    Check(!isError, "B files a bin: " + payload.Dump().substr(0, 300));
    std::filesystem::remove(hold);
    payload =
        call(nullptr, "get_task",
             "{\"task_id\":\"" + exportA + "\",\"wait_seconds\":10}", isError);
    const mcp_json::Value* exported = payload.Find("result");
    int64_t frames = 0;
    Check(text(payload, "state") == "succeeded" && exported != nullptr &&
              exported->Find("frames") != nullptr &&
              exported->Find("frames")->AsInt64(frames) && frames == 18 &&
              payload.Find("stale") != nullptr &&
              !payload.Find("stale")->AsBool(),
          "A's export encodes A as it was asked, and the edits of the other "
          "timelines leave it current: " +
              payload.Dump());
    payload =
        call(nullptr, "get_task",
             "{\"task_id\":\"" + exportB + "\",\"wait_seconds\":10}", isError);
    Check(
        text(payload, "state") == "succeeded" &&
            payload.Find("stale") != nullptr &&
            payload.Find("stale")->AsBool() &&
            text(payload, "stale_reason").find(timelineB) != std::string::npos,
        "B's export, whose timeline B edited after, says so: " +
            payload.Dump());

    // ---- an analysis in the background makes a synchronous one wait ----
    holdAnalysis = true;
    payload = call(&clientB, "analyze_audio_beats",
                   "{\"media_id\":\"M1\",\"async\":true}", isError);
    const std::string beats = text(payload, "task_id");
    payload =
        call(&clientA, "analyze_audio_beats", "{\"media_id\":\"M1\"}", isError);
    Check(isError && text(payload, "error") == "AnalysisBusy" &&
              text(payload, "detail").find(beats) != std::string::npos &&
              text(payload, "detail").find("analyze_audio_beats") !=
                  std::string::npos &&
              text(payload, "detail").find("async") != std::string::npos,
          "a synchronous analysis behind a background one is refused with "
          "the task named and what to do: " +
              payload.Dump());
    // PRD40 -- ROADMAP.md. One that cannot run in the background is told to
    // wait for the task, never to pass the async it would be refused for.
    payload =
        call(&clientA, "analyze_hesitations", "{\"media_id\":\"M1\"}", isError);
    Check(isError && text(payload, "error") == "AnalysisBusy" &&
              text(payload, "detail").find(beats) != std::string::npos &&
              text(payload, "detail").find("get_task") != std::string::npos &&
              text(payload, "detail").find("async: true") == std::string::npos,
          "analyze_hesitations behind a background analysis is told to wait, "
          "not to pass async: " +
              payload.Dump());
    holdAnalysis = false;
    call(nullptr, "get_task",
         "{\"task_id\":\"" + beats + "\",\"wait_seconds\":10}", isError);

    // ---- two clients, one idempotency key, two calls ----
    payload =
        call(&clientA, "trim_clip",
             trimArguments(clientA, clipA1, -1)
                     .substr(0, trimArguments(clientA, clipA1, -1).size() - 1) +
                 ",\"idempotency_key\":\"series-key\"}",
             isError);
    Check(!isError, "A trims with a key: " + payload.Dump().substr(0, 300));
    payload =
        call(nullptr, "trim_clip",
             trimArguments(clientB, clipB1, -1)
                     .substr(0, trimArguments(clientB, clipB1, -1).size() - 1) +
                 ",\"idempotency_key\":\"series-key\"}",
             isError);
    Check(isError && text(payload, "error") == "IdempotencyKeyReused" &&
              payload.Find("delta") == nullptr,
          "B's call under A's key is refused by name, never answered with "
          "A's answer: " +
              payload.Dump());

    // ---- StaleRevision says which grain moved ----
    payload = call(&clientA, "describe",
                   "{\"timeline_id\":\"" + timelineA + "\"}", isError);
    const std::string seenByA = text(payload, "revision");
    trim(clientB, clipB2, -1);
    const auto withRevision = [](std::string arguments,
                                 const std::string& revision) {
        arguments.pop_back();
        return arguments + ",\"expected_revision\":\"" + revision + "\"}";
    };
    payload = call(&clientA, "trim_clip",
                   withRevision(trimArguments(clientA, clipA1, -1), seenByA),
                   isError);
    const mcp_json::Value* refusal = payload.Find("result");
    const std::string current =
        refusal != nullptr ? text(*refusal, "current_revision") : "";
    Check(isError && text(payload, "error") == "StaleRevision" &&
              text(payload, "detail").find("other timeline") !=
                  std::string::npos &&
              text(payload, "detail").find(timelineB) != std::string::npos &&
              !current.empty(),
          "a mutation refused because another timeline moved says so, and "
          "that sending it again at the current revision is enough: " +
              payload.Dump());
    payload = call(&clientA, "trim_clip",
                   withRevision(trimArguments(clientA, clipA1, -1), current),
                   isError);
    Check(!isError, "sent again at the current revision, it applies: " +
                        payload.Dump().substr(0, 300));
    const std::string ownSeen = text(payload, "revision");
    trim(clientA, clipA1, 1);
    payload = call(&clientA, "trim_clip",
                   withRevision(trimArguments(clientA, clipA1, -1), ownSeen),
                   isError);
    Check(
        isError && text(payload, "error") == "StaleRevision" &&
            text(payload, "detail").find("timeline " + timelineA) !=
                std::string::npos &&
            text(payload, "detail").find("read it again") != std::string::npos,
        "refused because its own timeline moved, it is told to read it "
        "again: " +
            payload.Dump());

    // ---- nine plans, three per timeline ----
    std::vector<std::string> plans;
    for (const Client* client : {&clientA, &clientB, &clientC}) {
        const std::string clip = client == &clientA   ? clipA1
                                 : client == &clientB ? clipB1
                                                      : clipC1;
        for (int index = 0; index < 3; ++index) {
            payload =
                call(client, "prepare_edit_plan",
                     "{\"timeline_id\":\"" + client->timeline +
                         "\",\"label\":\"Variante " + std::to_string(index) +
                         "\",\"steps\":[{\"tool\":\"trim_clip\","
                         "\"arguments\":{\"clip_id\":\"" +
                         clip +
                         "\",\"edge\":\"Tail\",\"delta\":{\"value\":-1,"
                         "\"rate\":25}}}]}",
                     isError);
            Check(!isError,
                  client->name + " keeps plan " + std::to_string(index) +
                      " beside the others': " + payload.Dump().substr(0, 300));
            plans.push_back(text(payload, "plan_id"));
        }
    }
    for (const std::string& plan : plans) {
        call(nullptr, "discard_edit_plan", "{\"plan_id\":\"" + plan + "\"}",
             isError);
    }
    payload = call(nullptr, "apply_edit_plan",
                   "{\"plan_id\":\"" + plans.front() + "\"}", isError);
    Check(isError && text(payload, "error") == "UnknownPlan",
          "a plan no longer kept is refused by name: " + payload.Dump());

    // ---- creating does not move the default target ----
    payload = call(&clientA, "add_timeline",
                   "{\"name\":\"D\",\"width\":160,\"height\":90,"
                   "\"frame_rate\":{\"num\":25,\"den\":1}}",
                   isError);
    Check(!isError && activeTimeline() == timelineA,
          "add_timeline leaves the active timeline where it was: " +
              payload.Dump().substr(0, 300));
    payload = call(
        &clientB, "clone_timeline",
        "{\"source_timeline_id\":\"" + timelineB + "\",\"name\":\"B bis\"}",
        isError);
    Check(!isError && activeTimeline() == timelineA,
          "clone_timeline too: " + payload.Dump().substr(0, 300));
    payload = call(&clientC, "add_timeline",
                   "{\"name\":\"E\",\"width\":160,\"height\":90,"
                   "\"frame_rate\":{\"num\":25,\"den\":1},\"activate\":true}",
                   isError);
    Check(!isError && activeTimeline() != timelineA &&
              activeTimeline() != timelineB && activeTimeline() != timelineC,
          "activate: true activates what is created: " +
              payload.Dump().substr(0, 300));

    // ---- propose_grade, applied to a timeline that is not the active one ----
    ::setenv("CUTMACHINE_FFMPEG", FFMPEG_EXECUTABLE, 1);
    const std::string gradedBefore = timelineBytes().at(timelineB);
    payload = call(&clientB, "propose_grade",
                   "{\"timeline_id\":\"" + timelineB +
                       "\",\"anchor\":\"median\",\"apply\":true}",
                   isError);
    int64_t applied = 0;
    Check(!isError && payload.Find("applied") != nullptr &&
              payload.Find("applied")->AsInt64(applied) && applied > 0 &&
              timelineBytes().at(timelineB) != gradedBefore,
          "propose_grade applied on B lands on B, and only there: " +
              payload.Dump().substr(0, 400));
    ::unsetenv("CUTMACHINE_FFMPEG");

    // ---- a wrong open_project keeps the series' project ----
    payload = call(nullptr, "open_project",
                   "{\"path\":\"" +
                       (folder / "Absent.cutmachine-project").string() + "\"}",
                   isError);
    Check(isError && text(payload, "detail").find("is still open") !=
                         std::string::npos,
          "an open_project that fails says the series' project is still "
          "open: " +
              payload.Dump());
    std::string heldPath;
    Check(CreatePortableProject((folder / "Held.cutmachine-project").string(),
                                Project::FromDocument(document, "Tenu"),
                                heldPath, error),
          "a second package saves: " + error);
    ProjectSessionLock elsewhere;
    std::string owner;
    Check(elsewhere.Acquire(heldPath, owner, error),
          "another holder takes the second project: " + error);
    payload = call(nullptr, "open_project", "{\"path\":\"" + heldPath + "\"}",
                   isError);
    Check(
        isError &&
            text(payload, "detail").find("is still open") != std::string::npos,
        "so does one refused for a project held elsewhere: " + payload.Dump());
    elsewhere.Release();
    // The same mode from the start of a server: `--mcp-serve
    // --require-timeline`, through the binary, as .mcp.json starts it.
    {
        const std::filesystem::path requests = folder / "strict-requests.jsonl";
        const std::filesystem::path answers = folder / "strict-answers.jsonl";
        std::ofstream(requests)
            << R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":)"
               R"({"name":"describe","arguments":{}}})"
            << '\n';
        const std::string command =
            std::string("'") + CUTMACHINE_EXECUTABLE + "' --mcp-serve '" +
            heldPath + "' --stdio --require-timeline < '" + requests.string() +
            "' > '" + answers.string() + "' 2> /dev/null";
        Check(std::system(command.c_str()) == 0 &&
                  Read(answers).find("TimelineRequired") != std::string::npos &&
                  Read(answers).find("'A' (" + timelineA + ")") !=
                      std::string::npos,
              "--require-timeline starts a server in strict timeline mode: " +
                  Read(answers).substr(0, 300));
    }
    ProjectSessionLock intruder;
    Check(!intruder.Acquire(path, owner, error),
          "the server never let the series' project go: " + owner);
    payload = call(nullptr, "list_timelines", "{}", isError);
    Check(!isError && payload.Find("timelines") != nullptr &&
              payload.Find("timelines")->AsArray().size() == 6,
          "the series goes on with its project: " +
              payload.Dump().substr(0, 300));

    // ---- each timeline holds exactly its client's edits ----
    Project final;
    Check(LoadStoredProject(path, final, error), "the series reloads");
    const auto clipIds = [&](const Ulid& id) {
        std::set<std::string> ids;
        if (const DocumentSequence* timeline = final.FindTimeline(id))
            for (const DocumentTrack& track : timeline->tracks)
                for (const DocumentClip& clip : track.clips)
                    ids.insert(clip.id);
        return ids;
    };
    Check(clipIds(timelineA) == std::set<std::string>{clipA1},
          "A holds A's shot alone");
    Check(clipIds(timelineB) == std::set<std::string>{clipB1, clipB2, clipB3},
          "B holds B's three shots");
    Check(clipIds(timelineC) == std::set<std::string>{clipC1, clipC3},
          "C holds C's shots, its intervention taken back");
    server.Stop();
}

}  // namespace

// MOTION-2026-09 M3 -- a composition posed and edited over MCP reads back,
// byte for byte, the same from the CLI; a frame rendered over MCP carries the
// geometry the CLI describes for that frame.
mcp_json::Value ToolCall(int port, int id, const std::string& tool,
                         const std::string& arguments) {
    const std::string body = HttpBody(
        HttpPostJson(port, "/mcp",
                     R"({"jsonrpc":"2.0","id":)" + std::to_string(id) +
                         R"(,"method":"tools/call","params":{"name":")" + tool +
                         R"(","arguments":)" + arguments + "}}"));
    mcp_json::Value parsed;
    std::string error;
    Check(mcp_json::Value::Parse(body, parsed, error),
          tool + " answers JSON-RPC: " + error);
    const mcp_json::Value* result = parsed.Find("result");
    return (result != nullptr) ? *result : mcp_json::Value();
}

std::string ToolText(const mcp_json::Value& result) {
    const mcp_json::Value* content = result.Find("content");
    if ((content == nullptr) || !content->IsArray() ||
        content->AsArray().empty()) {
        return "";
    }
    const mcp_json::Value* text = content->AsArray()[0].Find("text");
    return (text != nullptr) && text->IsString() ? text->AsString() : "";
}

bool ToolOk(const mcp_json::Value& result) {
    const mcp_json::Value* isError = result.Find("isError");
    return (isError != nullptr) && isError->IsBool() && !isError->AsBool();
}

std::string WithoutNewline(std::string text) {
    if (!text.empty() && text.back() == '\n') {
        text.pop_back();
    }
    return text;
}

// IA5 -- every answer of the server carries the project's `revision` as its
// first key; the CLI's does not. M3 compares what the two say of a
// composition, so the revision is left out of that comparison.
std::string WithoutRevision(std::string text) {
    const std::string key = "{\"revision\":\"";
    if (text.rfind(key, 0) == 0) {
        const size_t end = text.find("\",", key.size());
        if (end != std::string::npos) {
            text.erase(1, end + 1);
        }
    }
    return text;
}

void CheckMotionCompositionsOverMcp(const std::filesystem::path& directory) {
    InstallMotionFrameRenderer();
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Motion.cutmachine-project").string(),
              Project::FromDocument(Fixture(), "Motion"), path, error),
          "motion package saves: " + error);
    McpProjectBackend backend(path);
    McpServer server(backend);
    Check(server.Start(0, error), "motion MCP server starts: " + error);
    const int port = server.Port();

    const mcp_json::Value added = ToolCall(
        port, 1, "add_motion_composition",
        R"({"name":"Carton","definition":{"width":540,"height":960,"fps":25,)"
        R"("duration":50,"props":{"titre":{"type":"text","default":"Titre"},)"
        R"("taille":{"type":"number","default":64,"min":10,"max":200}},)"
        R"("root":{"type":"fill","style":{"justifyContent":"center",)"
        R"("alignItems":"center","backgroundColor":"#101820"},"children":[)"
        R"({"type":"text","id":"titre","content":{"prop":"titre"},)"
        R"("style":{"fontFamily":"Fira Sans Condensed","fontWeight":600,)"
        R"("fontSize":{"prop":"taille"},"lineHeight":1.2,"color":"#fff"},)"
        R"("animate":{"opacity":{"interpolate":{"frames":[0,6],)"
        R"("values":[0,1]}}}}]}}})");
    Check(ToolOk(added), "add_motion_composition over MCP: " + ToolText(added));
    Project project;
    Check(LoadStoredProject(path, project, error) &&
              project.compositions.size() == 1,
          "the composition is in the stored project: " + error);
    if (project.compositions.size() != 1) {
        server.Stop();
        return;
    }
    const Ulid id = project.compositions.front().id;
    const mcp_json::Value edited = ToolCall(
        port, 2, "edit_motion_composition",
        R"({"composition_id":")" + id +
            R"(","edits":[{"op":"set_style","node":"titre",)"
            R"("property":"letterSpacing","value":2.5},)"
            R"({"op":"set_props","props":{"titre":"Posé par MCP","taille":72}}]})");
    Check(
        ToolOk(edited) &&
            ToolText(edited).find("\"changed_composition_ids\":[\"" + id +
                                  "\"]") != std::string::npos,
        "edit_motion_composition over MCP, and its delta: " + ToolText(edited));

    const std::string mcpList = WithoutRevision(
        ToolText(ToolCall(port, 3, "list_motion_compositions", "{}")));
    const std::string mcpDescribe = WithoutRevision(
        ToolText(ToolCall(port, 4, "describe_motion_composition",
                          R"({"composition_id":")" + id + R"("})")));
    std::string cliList;
    std::string cliDescribe;
    Check(DescribeCompositionsCommand({path}, cliList) == 0 &&
              DescribeCompositionsCommand({path, id}, cliDescribe) == 0,
          "the CLI reads the compositions back");
    Check(WithoutNewline(cliList) == mcpList,
          "--describe-compositions lists what list_motion_compositions "
          "does:\n  " +
              cliList + "\n  " + mcpList);
    Check(WithoutNewline(cliDescribe) == mcpDescribe &&
              mcpDescribe.find("\"letterSpacing\":2.5") != std::string::npos &&
              mcpDescribe.find("Posé par MCP") != std::string::npos,
          "the CLI reads the MCP edit back byte for byte:\n  " + cliDescribe +
              "\n  " + mcpDescribe);

    // An edit through --apply-op reads back identically over MCP.
    std::string applied;
    const Operation cliEdit = SetMotionPropsOperation{
        id, [] {
            mcp_json::Value props = mcp_json::Value::MakeObject();
            props.Set("taille", mcp_json::Value::MakeRawNumber("80.5"));
            return props;
        }()};
    Check(
        ApplyOperationCommand(path, SerializeOperation(cliEdit), applied) == 0,
        "--apply-op edits the composition: " + applied);
    std::string cliAfter;
    DescribeCompositionsCommand({path, id}, cliAfter);
    Check(WithoutNewline(cliAfter) ==
                  WithoutRevision(ToolText(
                      ToolCall(port, 5, "describe_motion_composition",
                               R"({"composition_id":")" + id + R"("})"))) &&
              cliAfter.find("\"default\":80.5") != std::string::npos,
          "and MCP reads the --apply-op edit back byte for byte");

    const std::string fonts = MOTION_FONT_DIR;
    const mcp_json::Value rendered =
        ToolCall(port, 6, "render_motion_frame",
                 R"({"composition_id":")" + id.substr(0, 20) +
                     R"(","frame":10,"font_dirs":[")" + fonts + R"("]})");
    Check(ToolOk(rendered),
          "render_motion_frame over MCP: " + ToolText(rendered));
    const mcp_json::Value* content = rendered.Find("content");
    const mcp_json::Value* image = (content != nullptr) && content->IsArray() &&
                                           content->AsArray().size() == 2
                                       ? &content->AsArray()[1]
                                       : nullptr;
    Check((image != nullptr) && (image->Find("mimeType") != nullptr) &&
              image->Find("mimeType")->AsString() == "image/png" &&
              (image->Find("data") != nullptr) &&
              image->Find("data")->AsString().rfind("iVBORw0KGgo", 0) == 0,
          "the picture is a PNG");
    std::string cliFrame;
    Check(DescribeMotionFrameCommand(
              {path, "10", "--composition", id, "--font-dir", fonts},
              cliFrame) == 0,
          "the CLI describes the same frame: " + cliFrame);
    Check(WithoutNewline(cliFrame) == WithoutRevision(ToolText(rendered)) &&
              cliFrame.find("\"uid\":") != std::string::npos,
          "the geometry beside the picture is the CLI's, uids included");
    const mcp_json::Value missing = ToolCall(
        port, 7, "render_motion_frame",
        R"({"composition_id":")" + id + R"(","frame":0,"props":{"taille":9}})");
    Check(!ToolOk(missing) &&
              ToolText(missing).find("\"error\":\"PropOutOfRange\"") !=
                  std::string::npos,
          "a prop override outside its bounds is refused by name: " +
              ToolText(missing));

    // M6 -- a version and a format posed over MCP, rendered over MCP, and
    // described by the CLI: the same geometry.
    const mcp_json::Value versioned = ToolCall(
        port, 8, "edit_motion_composition",
        R"({"composition_id":")" + id +
            R"(","edits":[{"op":"set_format","format":"16x9","width":960,)"
            R"("height":540,"props":{"taille":48}},{"op":"set_variant",)"
            R"("variant":"court","props":{"titre":"Court"}}]})");
    Check(ToolOk(versioned),
          "versions and formats over MCP: " + ToolText(versioned));
    const mcp_json::Value inFormat = ToolCall(
        port, 9, "render_motion_frame",
        R"({"composition_id":")" + id +
            R"(","frame":10,"variant":"court","format":"16x9","font_dirs":[")" +
            fonts + R"("]})");
    std::string cliVersion;
    Check(DescribeMotionFrameCommand(
              {path, "10", "--composition", id, "--variant", "court",
               "--format", "16x9", "--font-dir", fonts},
              cliVersion) == 0,
          "the CLI describes that version in that format: " + cliVersion);
    Check(
        ToolOk(inFormat) &&
            WithoutNewline(cliVersion) == WithoutRevision(ToolText(inFormat)) &&
            cliVersion.rfind("{\"width\":960,\"height\":540,", 0) == 0 &&
            cliVersion.find("\"text\":\"Court\"") != std::string::npos,
        "render_motion_frame renders the version in the format the CLI "
        "describes: " +
            ToolText(inFormat).substr(0, 200));
    const mcp_json::Value unknown = ToolCall(
        port, 10, "render_motion_frame",
        R"({"composition_id":")" + id + R"(","frame":0,"variant":"long"})");
    Check(
        !ToolOk(unknown) &&
            ToolText(unknown).find("\"error\":\"UnknownVariant\"") !=
                std::string::npos,
        "an unknown version is refused by name over MCP: " + ToolText(unknown));

    // M6 -- the same batch planned over MCP and by the CLI, then encoded
    // over MCP.
    const std::filesystem::path videos = directory / "motion-videos";
    std::filesystem::create_directories(videos);
    const std::string pattern =
        (videos / "carton_{variant}_{format}.mov").string();
    const std::string videoArguments =
        R"({"composition_id":")" + id + R"(","output":")" + pattern +
        R"(","variants":["court"],"alpha":true,"font_dirs":[")" + fonts +
        R"("])";
    const mcp_json::Value planned =
        ToolCall(port, 11, "render_motion_video", videoArguments + "}");
    std::string cliPlan;
    Check(RenderMotionVideoCommand(
              {path, pattern, "--composition", id, "--variants", "court",
               "--alpha", "--preview", "--font-dir", fonts},
              cliPlan) == 0,
          "the CLI plans the batch: " + cliPlan);
    Check(
        ToolOk(planned) &&
            WithoutNewline(cliPlan) == WithoutRevision(ToolText(planned)) &&
            cliPlan.find("\"preview\":true,\"total\":1") != std::string::npos &&
            std::filesystem::is_empty(videos),
        "render_motion_video previews by default, as the CLI plans: " +
            ToolText(planned));
    const mcp_json::Value encoded =
        ToolCall(port, 12, "render_motion_video",
                 videoArguments + R"(,"preview":false})");
    std::error_code sized;
    Check(ToolOk(encoded) &&
              ToolText(encoded).find("\"completed\":1") != std::string::npos &&
              std::filesystem::file_size(videos / "carton_court_16x9.mov",
                                         sized) > 0,
          "and encodes it when asked: " + ToolText(encoded));
    const mcp_json::Value again =
        ToolCall(port, 13, "render_motion_video",
                 videoArguments + R"(,"preview":false})");
    Check(
        !ToolOk(again) && ToolText(again).find("\"error\":\"InvalidExport\"") !=
                              std::string::npos,
        "an existing file is refused before encoding: " + ToolText(again));
    std::error_code ignored;
    std::filesystem::remove_all(videos, ignored);
    server.Stop();
}

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. humanize_motion_composition
// over MCP: shown without being applied, then applied as one edit, proved
// by rendering every frame, and taken back whole by one undo.
void CheckHumanizeOverMcp(const std::filesystem::path& directory) {
    InstallMotionFrameRenderer();
    const Ulid id = "01K8H000000000000000000001";
    mcp_json::Value definition;
    std::string error;
    Check(mcp_json::Value::Parse(
              R"({"width": 320, "height": 180, "fps": 25, "duration": 12,
                "root": {"type": "fill", "style": {"backgroundColor": "#101010",
                  "color": "#ffffff"}, "children": [
                  {"type": "sequence", "from": 0, "duration": 12, "children": [)" +
                  TimecodeRow(0, 12, 11, -1,
                              R"("style": {"fontFamily": "Menlo",
                                 "fontSize": 18, "lineHeight": 1.0})") +
                  R"(,
                    {"type": "box", "style": {"position": "absolute",
                      "left": 20, "top": 20},
                     "children": [{"type": "text", "content": "Titre",
                       "style": {"fontFamily": "Menlo", "fontSize": 24}}]}]}]}})",
              definition, error),
          "humanize fixture parses: " + error);
    Document document = Fixture();
    AddMotionCompositionOperation add;
    add.composition = {id, "Machine", definition};
    EditLog log;
    EditError failure = EditError::None;
    Check(log.Apply(document, add, failure, error),
          "the composition is added with its identities: " + error);
    std::string path;
    Check(CreatePortableProject(
              (directory / "Humanize.cutmachine-project").string(),
              Project::FromDocument(document, "Humanize"), path, error),
          "humanize package saves: " + error);
    McpProjectBackend backend(path);
    McpServer server(backend);
    Check(server.Start(0, error), "humanize MCP server starts: " + error);
    const int port = server.Port();
    const auto stored = [&]() {
        Document snapshot;
        std::string message;
        backend.SnapshotDocument(snapshot, message);
        const DocumentComposition* found = snapshot.FindComposition(id);
        return found != nullptr ? found->definition.Dump() : std::string();
    };
    const std::string original = stored();

    const mcp_json::Value shown =
        ToolCall(port, 1, "humanize_motion_composition",
                 R"({"composition_id":")" + id + R"(","apply":false})");
    mcp_json::Value shownResult;
    mcp_json::Value::Parse(ToolText(shown), shownResult, error);
    Check(ToolOk(shown) && shownResult.Find("applied") != nullptr &&
              !shownResult.Find("applied")->AsBool() &&
              shownResult.Find("definition") != nullptr &&
              shownResult.Find("timecodes")->Dump() == "1" &&
              shownResult.Find("wrappers")->Dump() == "1" &&
              shownResult.Find("frames_compared")->Dump() == "12" &&
              stored() == original,
          "apply false shows the rewrite and leaves the project: " +
              ToolText(shown));

    const mcp_json::Value applied =
        ToolCall(port, 2, "humanize_motion_composition",
                 R"({"composition_id":")" + id + R"("})");
    Check(
        ToolOk(applied) &&
            ToolText(applied).find("\"applied\":true") != std::string::npos &&
            stored() != original &&
            stored().find("\"timecode\":\"composition\"") != std::string::npos,
        "applied, the composition reads its timecode: " + ToolText(applied));

    const mcp_json::Value undone = ToolCall(port, 3, "undo", "{}");
    Check(ToolOk(undone) && stored() == original,
          "one undo takes the whole rewrite back");
    server.Stop();
}

// M5 -- docs/roadmap/MOTION-2026-09.md. A template added at the timeline's
// size, posed on a clip by anchor with the real fonts, refused when its text
// does not fit or leaves the safe area; the clip after it on its track does
// not move, and one undo takes it off.
// PERF5 -- ROADMAP.md. An MCP edit starts from the session's cache and
// leaves the committed project in it rather than parsing the package back.
// That is only right if the project it keeps is the one the package now
// holds: after every kind of edit, the cache, the revision and every
// timeline must equal a fresh parse of the files just written.
// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The project's edits say why,
// through the catalog, on a real package: a clone, a timeline created and
// activated, a rename left without a reason, the person's choice between
// timelines (DEC41) -- refused without its reason or its author -- read back
// by list_motifs over the project's journal; the same choice by
// `--apply-project-op`; a project undo that keeps the reason on the undone
// entry; and the window's backend handing the motif to the window.
void CheckProjectMotifs(const std::filesystem::path& directory) {
    std::string path;
    std::string error;
    const Project stored = Project::FromDocument(Fixture(), "Motifs");
    Check(CreatePortableProject(
              (directory / "ProjectMotifs.cutmachine-project").string(), stored,
              path, error),
          "the motif package saves: " + error);
    const Ulid main = stored.active_timeline_id;
    const McpToolRegistry registry;
    McpProjectBackend backend(path);
    const auto call = [&](McpBackend& on, const std::string& tool,
                          const std::string& json) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(json, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(on, tool, arguments);
    };
    const auto journal = [&] {
        ProjectEditLog log;
        EditError editError = EditError::None;
        std::string message;
        Check(LoadProjectEditLogIfAny(path, log, editError, message),
              "the project journal reads: " + message);
        return log;
    };

    const McpToolCallOutcome cloned =
        call(backend, "clone_timeline",
             R"({"source_timeline_id":")" + main +
                 R"(","name":"Serrée","motif":{"reason":"rhythm",)"
                 R"("note":"une version plus serrée"}})");
    ProjectEditLog log = journal();
    Check(cloned.ok && log.AppliedCount() == 1 &&
              log.AppliedEntries()[0].motifs.size() == 1 &&
              log.AppliedEntries()[0].motifs[0].motif.reason ==
                  MotifReason::Rhythm &&
              log.AppliedEntries()[0].motifs[0].motif.author ==
                  MotifAuthor::Agent &&
              log.AppliedEntries()[0].motifs[0].motif.note ==
                  "une version plus serrée",
          "a clone carries its motif into the project's journal: " +
              cloned.message);
    Check(Read(ProjectEditLogPathForProject(path))
                  .rfind("{\"version\":2,\"kind\":\"project\"", 0) == 0,
          "a project journal with a motif is written as version 2");
    const Ulid clone =
        std::get<CloneProjectTimelineOperation>(log.AppliedEntries()[0].op)
            .timeline_id;
    Check(!backend.CurrentEditMotifs().motif,
          "the call's motif is cleared once it has ended");

    const McpToolCallOutcome added = call(
        backend, "add_timeline",
        R"({"name":"Verticale","width":1080,"height":1920,)"
        R"("frame_rate":{"num":25,"den":1},"activate":true,)"
        R"("motif":{"reason":"format","note":"la déclinaison du brief"}})");
    log = journal();
    Check(added.ok && log.AppliedCount() == 3 &&
              log.AppliedEntries()[1].motifs.size() == 1 &&
              log.AppliedEntries()[1].motifs[0].motif.reason ==
                  MotifReason::Format &&
              log.AppliedEntries()[2].motifs.size() == 1 &&
              std::holds_alternative<SetActiveProjectTimelineOperation>(
                  log.AppliedEntries()[2].op),
          "a creation and its activation, two entries, each with the "
          "call's motif: " +
              added.message);
    const Ulid vertical =
        std::get<AddProjectTimelineOperation>(log.AppliedEntries()[1].op)
            .timeline_id;
    const McpToolCallOutcome renamed =
        call(backend, "rename_timeline",
             R"({"timeline_id":")" + vertical + R"(","name":"Verticale 2"})");
    log = journal();
    Check(renamed.ok && log.AppliedCount() == 4 &&
              log.AppliedEntries()[3].motifs.empty(),
          "a project edit without a motif writes none: " + renamed.message);

    // The choice: refused without its reason, without its author, among
    // nothing or among a timeline the project lacks -- and nothing written.
    const std::string choose = R"({"timeline_id":")" + clone +
                               R"(","alternative_timeline_ids":[")" + main +
                               R"(",")" + vertical + R"("])";
    for (
        const auto& [arguments, label] :
        std::vector<std::pair<std::string, std::string>>{
            {choose + "}", "without a motif"},
            {choose + R"(,"motif":{"reason":"selection"}})",
             "without saying who chose"},
            {R"({"timeline_id":")" + clone +
                 R"(","alternative_timeline_ids":[],)"
                 R"("motif":{"reason":"selection","author":"person"}})",
             "among nothing"},
            {R"({"timeline_id":")" + clone +
                 R"(","alternative_timeline_ids":["01K3ZZZZZZZZZZZZZZZZZZZZZZ"],)"
                 R"("motif":{"reason":"selection","author":"person"}})",
             "among a timeline the project lacks"}}) {
        const McpToolCallOutcome refused =
            call(backend, "choose_timeline_variant", arguments);
        Check(!refused.ok && journal().AppliedCount() == 4,
              "a choice " + label + " is refused and writes nothing: " +
                  refused.error_name + " " + refused.message);
    }
    const McpToolCallOutcome chosen =
        call(backend, "choose_timeline_variant",
             choose + R"(,"activate":true,"motif":{"reason":"selection",)"
                      R"("author":"person","note":"l'auteur garde la serrée"}})");
    log = journal();
    Project after;
    Check(chosen.ok && log.AppliedCount() == 5 &&
              std::holds_alternative<ChooseProjectTimelineOperation>(
                  log.AppliedEntries()[4].op) &&
              log.AppliedEntries()[4].motifs.size() == 1 &&
              log.AppliedEntries()[4].motifs[0].motif.author ==
                  MotifAuthor::Person &&
              LoadStoredProject(path, after, error) &&
              after.active_timeline_id == clone,
          "the person's choice is journaled with its reason, the kept "
          "timeline made active: " +
              chosen.message);

    // Read back.
    const McpToolCallOutcome listed =
        call(backend, "list_motifs", R"({"journal":"project"})");
    mcp_json::Value answer;
    std::string parseError;
    int64_t without = -1;
    Check(listed.ok &&
              mcp_json::Value::Parse(listed.result_json, answer, parseError) &&
              answer.Find("groups") != nullptr &&
              answer.Find("groups")->AsArray().size() == 3 &&
              answer.Find("operations_without_motif")->AsInt64(without) &&
              without == 1,
          "list_motifs over the project's journal: rhythm, format and "
          "selection, the rename unexplained: " +
              listed.result_json + listed.message);
    Check(listed.result_json.find(R"("name":"Serrée")") != std::string::npos &&
              listed.result_json.find(R"("type":"ChooseProjectTimeline")") !=
                  std::string::npos,
          "the choice names the timeline kept: " + listed.result_json);
    const McpToolCallOutcome both =
        call(backend, "list_motifs",
             R"({"journal":"project","timeline_id":")" + main + R"("})");
    Check(!both.ok && both.error_name == "ValidationFailed",
          "the project's journal takes no timeline_id: " + both.message);
    backend.SetRequireExplicitTimeline(true);
    Check(call(backend, "list_motifs", R"({"journal":"project"})").ok,
          "strict mode reads the project's journal without a timeline");
    backend.SetRequireExplicitTimeline(false);

    // The same choice written by hand, with its motif.
    EditMotif byHand;
    byHand.reason = MotifReason::Selection;
    byHand.author = MotifAuthor::Person;
    std::string output;
    Check(ApplyProjectOperationCommand(
              path,
              SerializeProjectOperation(
                  ChooseProjectTimelineOperation{main, {clone}, false}),
              output, ProjectEntryMotifsFor(byHand)) == 0,
          "--apply-project-op records a choice with its motif: " + output);
    log = journal();
    Check(log.AppliedCount() == 6 && log.AppliedEntries()[5].motifs.size() == 1,
          "the hand-written choice carries its motif");

    // A project undo moves the entry with its motif and restores the bytes.
    Project beforeChoice;
    Check(LoadStoredProject(path, beforeChoice, error), "reload: " + error);
    Check(UndoProjectOperationCommand(path, output) == 0, "undo: " + output);
    log = journal();
    Project undone;
    Check(log.AppliedCount() == 5 && log.UndoneCount() == 1 &&
              log.UndoneEntries()[0].motifs.size() == 1 &&
              LoadStoredProject(path, undone, error) &&
              undone.SaveToString() == beforeChoice.SaveToString(),
          "undoing an inactive choice keeps the bytes and the motif");

    // The window's backend hands the call's motif to the window, which
    // journals it.
    Document liveDocument = stored.MakeActiveDocument();
    EditLog liveLog;
    std::vector<EntryMotif> handed;
    McpLiveBackend live(liveDocument, liveLog, nullptr,
                        [&](ProjectOperation, std::vector<EntryMotif> motifs,
                            std::string& json, std::string&, std::string&) {
                            handed = std::move(motifs);
                            json = "{\"ok\":true}";
                            return true;
                        });
    Check(call(live, "add_timeline",
               R"({"name":"Carrée","width":1080,"height":1080,)"
               R"("frame_rate":{"num":25,"den":1},)"
               R"("motif":{"reason":"format"}})")
                  .ok &&
              handed.size() == 1 &&
              handed[0].motif.reason == MotifReason::Format,
          "the window's backend hands the motif to the window");
}

// BINS_SPEC.md -- set_timeline_bin files a timeline as set_media_bin files a
// rush: one entry of the project's journal, read back by list_timelines, and
// undone by filing it back at the root. A bin holding a timeline stays.
void CheckTimelineBins(const std::filesystem::path& directory) {
    std::string path;
    std::string error;
    const Project stored = Project::FromDocument(Fixture(), "Chutiers");
    Check(CreatePortableProject(
              (directory / "TimelineBins.cutmachine-project").string(), stored,
              path, error),
          "the timeline bins package saves: " + error);
    const Ulid main = stored.active_timeline_id;
    const McpToolRegistry registry;
    McpProjectBackend backend(path);
    const auto call = [&](const std::string& tool, const std::string& json) {
        mcp_json::Value arguments;
        std::string parseError;
        Check(mcp_json::Value::Parse(json, arguments, parseError),
              tool + " arguments parse: " + parseError);
        return registry.Call(backend, tool, arguments);
    };
    const auto load = [&] {
        Project project;
        std::string loadError;
        Check(LoadStoredProject(path, project, loadError),
              "the timeline bins project reloads: " + loadError);
        return project;
    };

    const McpToolCallOutcome bin = call("add_bin", R"({"name":"TIMELINES"})");
    const Project before = load();
    Check(bin.ok && before.bins.size() == 1,
          "a bin for the timelines: " + bin.message);
    const Ulid binId = before.bins.empty() ? Ulid() : before.bins.front().id;

    const McpToolCallOutcome filed =
        call("set_timeline_bin", R"({"timeline_id":")" + main.substr(0, 12) +
                                     R"(","bin_id":")" + binId + R"("})");
    Project project = load();
    const auto placed = project.timeline_bin_ids.find(main);
    Check(filed.ok && placed != project.timeline_bin_ids.end() &&
              placed->second == binId,
          "set_timeline_bin files the timeline a prefix names: " +
              filed.error_name + " " + filed.message);
    ProjectEditLog log;
    EditError editError = EditError::None;
    std::string message;
    Check(LoadProjectEditLogIfAny(path, log, editError, message) &&
              log.AppliedCount() == 1 &&
              std::holds_alternative<SetProjectTimelineBinOperation>(
                  log.AppliedEntries()[0].op),
          "the filing is one entry of the project's journal: " + message);
    const McpToolCallOutcome listed = call("list_timelines", "{}");
    Check(listed.ok && listed.result_json.find(R"("bin_id":")" + binId +
                                               "\"") != std::string::npos,
          "list_timelines says where the timeline is filed: " +
              listed.result_json);

    // Refused as an edit, by the timeline's name: an agent told IoError
    // suspected the disk.
    const McpToolCallOutcome removed =
        call("remove_bin", R"({"bin_id":")" + binId + R"("})");
    Check(!removed.ok && removed.error_name == "InvalidOperation" &&
              removed.message.find("still holds the timeline") !=
                  std::string::npos &&
              load().bins.size() == 1,
          "a bin holding a timeline is not removed: " + removed.error_name +
              " " + removed.message);
    const McpToolCallOutcome undone = call("undo", "{}");
    Check(!undone.ok && undone.error_name == "InvalidOperation" &&
              load().bins.size() == 1,
          "undoing the bin's creation is refused while it holds a timeline: " +
              undone.error_name + " " + undone.message);
    const McpToolCallOutcome unknownBin = call(
        "set_timeline_bin", R"({"timeline_id":")" + main +
                                R"(","bin_id":"01K3ZZZZZZZZZZZZZZZZZZZZZZ"})");
    Check(!unknownBin.ok &&
              load().timeline_bin_ids == std::map<Ulid, Ulid>{{main, binId}},
          "a bin the project lacks is refused, the filing kept: " +
              unknownBin.error_name + " " + unknownBin.message);
    const McpToolCallOutcome unknownTimeline = call(
        "set_timeline_bin", R"({"timeline_id":"01K3ZZZZZZZZZZZZZZZZZZZZZZ",)"
                            R"("bin_id":")" +
                                binId + R"("})");
    Check(
        !unknownTimeline.ok && unknownTimeline.error_name == "UnknownSequence",
        "a timeline the project lacks is refused: " +
            unknownTimeline.error_name + " " + unknownTimeline.message);

    const McpToolCallOutcome root = call(
        "set_timeline_bin", R"({"timeline_id":")" + main + R"(","bin_id":""})");
    project = load();
    Check(root.ok && project.timeline_bin_ids.empty() &&
              project.SaveToString() == before.SaveToString(),
          "an empty bin_id files it back at the root, the project as it "
          "was: " +
              root.message);
    const McpToolCallOutcome relisted = call("list_timelines", "{}");
    Check(relisted.ok &&
              relisted.result_json.find("\"bin_id\"") == std::string::npos,
          "a timeline at the root has no bin_id: " + relisted.result_json);
    Check(call("remove_bin", R"({"bin_id":")" + binId + R"("})").ok &&
              load().bins.empty(),
          "the emptied bin is removed");
}

void CheckEditCacheMatchesPackage(const std::filesystem::path& directory) {
    std::string path;
    std::string error;
    Project stored = Project::FromDocument(Fixture(), "Cache");
    Check(CreatePortableProject(
              (directory / "EditCache.cutmachine-project").string(), stored,
              path, error),
          "cache package saves: " + error);
    McpProjectBackend backend(path);
    const auto matches = [&](const std::string& label) {
        Project cached;
        Project fresh;
        std::string message;
        std::string revision;
        Check(backend.SnapshotProject(cached, message) &&
                  LoadStoredProject(path, fresh, message) &&
                  backend.Revision(revision, message),
              label + ": both read: " + message);
        Check(cached.SaveToString() == fresh.SaveToString() &&
                  revision == CanonicalHash(fresh.SaveToString()),
              label + ": the cache and its revision are the package's");
        for (const DocumentSequence& timeline : fresh.timelines) {
            Check(cached.MakeDocument(timeline.id).SaveToString() ==
                      fresh.MakeDocument(timeline.id).SaveToString(),
                  label + ": timeline " + timeline.id + " is the package's");
        }
    };
    const auto apply = [&](const std::string& label, Operation operation,
                           const std::string& timelineId = {}) {
        std::string result;
        std::string errorName;
        std::string message;
        Check(backend.SelectTimelineForEdit(timelineId, errorName, message),
              label + ": timeline selected: " + message);
        Check(backend.ApplyOperation(std::move(operation), result, errorName,
                                     message),
              label + " applies: " + errorName + " " + message);
        backend.EndTimelineEdit();
        matches(label);
    };
    matches("opened");
    DocumentMarker marker;
    marker.name = "Cache";
    marker.time = {3, 25};
    apply("add a marker", AddMarkerOperation{marker, -1});
    apply("trim a head", TrimClipOperation{"01K30000000000000000000003",
                                           TrimEdge::Head,
                                           {2, 25},
                                           std::nullopt});
    SplitClipOperation split;
    split.clip_id = "01K30000000000000000000004";
    split.timeline_position = {25, 25};
    apply("split a clip", split);
    AddTrackOperation track;
    track.track_id = "01K30000000000000000000009";
    track.kind = "video";
    track.index = 1;
    apply("add a track", track);
    MoveClipOperation move;
    move.clip_id = "01K30000000000000000000003";
    move.track_id = "01K30000000000000000000009";
    move.timeline_in = {40, 25};
    apply("move a clip across tracks", move);
    CaptionStyle style;
    style.id = "01K3000000000000000000000A";
    apply("add a caption style", AddCaptionStyleOperation{style, -1});

    // A failed edit leaves nothing cached that the package does not hold.
    std::string result;
    std::string errorName;
    std::string message;
    Check(
        !backend.ApplyOperation(TrimClipOperation{"01K3000000000000000000000Z",
                                                  TrimEdge::Head,
                                                  {2, 25},
                                                  std::nullopt},
                                result, errorName, message),
        "an edit of an unknown clip is refused");
    matches("after a refused edit");

    // Another timeline, edited without being activated.
    CloneProjectTimelineOperation copy;
    copy.source_timeline_id = "01K30000000000000000000005";
    copy.name = "Copie";
    ProjectOperation clone = copy;
    Check(backend.ApplyProjectEdit(clone, result, errorName, message),
          "a timeline is cloned: " + message);
    matches("after a project edit");
    Project cloned;
    Check(LoadStoredProject(path, cloned, message) &&
              cloned.timelines.size() == 2,
          "two timelines");
    const Ulid other = cloned.timelines.back().id;
    DocumentMarker elsewhere;
    elsewhere.name = "Ailleurs";
    elsewhere.time = {7, 25};
    apply("add a marker to an inactive timeline",
          AddMarkerOperation{elsewhere, -1}, other);
    Check(backend.Undo(result, errorName, message), "undo: " + message);
    matches("after an undo");
    Check(backend.Redo(result, errorName, message), "redo: " + message);
    matches("after a redo");
    Check(backend.AlignSourceTranscripts(false, result, message),
          "an analysis runs: " + message);
    matches("after an analysis");

    // The cache no longer drops itself after every command: it trusts the
    // commit generation. A commit this backend did not make -- as a command
    // that starts writing the package would -- leaving the project file the
    // same length, its date put back: the size/mtime second opinion cannot
    // tell, the generation must.
    Project before;
    Check(backend.SnapshotProject(before, message), "the cache is filled");
    const std::filesystem::path projectFile(path);
    const std::uintmax_t size = std::filesystem::file_size(projectFile);
    const std::filesystem::file_time_type written =
        std::filesystem::last_write_time(projectFile);
    Project renamed = before;
    renamed.name = "Cachf";
    Check(renamed.name.size() == before.name.size() &&
              CommitStoredProject(path, renamed, message),
          "a commit outside the backend: " + message);
    std::filesystem::last_write_time(projectFile, written);
    Check(std::filesystem::file_size(projectFile) == size &&
              std::filesystem::last_write_time(projectFile) == written,
          "the project file keeps its length and its date");
    matches("after a commit the backend did not make");

    // A background task starts from the session's cache, and still sees an
    // edit that lands after it was forked.
    std::unique_ptr<McpBackend> task =
        backend.ForkForTask(MediaTaskRunner{}, message);
    Check(task != nullptr, "a task forks: " + message);
    DocumentMarker late;
    late.name = "Tard";
    late.time = {9, 25};
    apply("an edit after a fork", AddMarkerOperation{late, -1});
    Project seen;
    Project fresh;
    Check(task->SnapshotProject(seen, message) &&
              LoadStoredProject(path, fresh, message) &&
              seen.SaveToString() == fresh.SaveToString(),
          "the task's copy of the cache follows the edit: " + message);
}

// MB1 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The shelf over MCP,
// both ways: a composition of a project goes to this machine's library,
// the catalog lists it beside the shipped templates with what it says of
// itself, and a project takes it back as a composition of its own. The
// library is the directory main() points CUTMACHINE_MOTION_LIBRARY at.
void CheckMotionLibraryOverMcp(const std::filesystem::path& directory) {
    const std::filesystem::path library = directory / "motion-library";
    std::filesystem::remove_all(library);
    Document document;
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Library.cutmachine-project").string(),
              Project::FromDocument(document, "Library"), path, error),
          "library package saves: " + error);
    McpProjectBackend backend(path);
    McpServer server(backend);
    Check(server.Start(0, error), "library MCP server starts: " + error);
    const int port = server.Port();
    const auto parsed = [&](const mcp_json::Value& result) {
        mcp_json::Value value;
        mcp_json::Value::Parse(ToolText(result), value, error);
        return value;
    };
    const auto text = [](const mcp_json::Value& value, const char* key) {
        const mcp_json::Value* found = value.Find(key);
        return found != nullptr && found->IsString() ? found->AsString()
                                                     : std::string();
    };

    const mcp_json::Value empty =
        ToolCall(port, 1, "list_motion_templates", "{}");
    const mcp_json::Value emptyResult = parsed(empty);
    Check(ToolOk(empty) && emptyResult.Find("library") != nullptr &&
              text(*emptyResult.Find("library"), "directory") ==
                  library.string() &&
              text(*emptyResult.Find("library"), "base") ==
                  (library / "library.sqlite").string() &&
              emptyResult.Find("library")->Find("issues") == nullptr &&
              ToolText(empty).find("\"source\":\"shipped\"") !=
                  std::string::npos &&
              ToolText(empty).find("\"source\":\"library\"") ==
                  std::string::npos &&
              !std::filesystem::exists(library),
          "an empty library is named, listed empty and not created: " +
              ToolText(empty));

    const mcp_json::Value added =
        ToolCall(port, 2, "add_motion_template", R"({"template":"nom"})");
    const mcp_json::Value addedResult = parsed(added);
    const std::string first = text(addedResult, "composition_id");
    Check(ToolOk(added) && !first.empty(),
          "a shipped template is added: " + ToolText(added));
    if (first.empty()) {
        server.Stop();
        return;
    }

    const std::string saving =
        R"({"composition_id":")" + first +
        R"(","template":"nom-client","description":"Nom et fonction.",)"
        R"("use":"Une personne qui parle.","avoid":"Un plan large.",)"
        R"("invariants":["Le nom entre par la gauche."],)"
        R"("origin":"essai MB1"})";
    const mcp_json::Value saved =
        ToolCall(port, 3, "save_motion_template", saving);
    const mcp_json::Value savedResult = parsed(saved);
    // MB10 -- in the personal base, as its first version; no file.
    const std::filesystem::path file = library / "nom-client.json";
    Check(
        ToolOk(saved) &&
            text(savedResult, "base") ==
                (library / "library.sqlite").string() &&
            ToolText(saved).find("\"version\":1,") != std::string::npos &&
            !std::filesystem::exists(file) &&
            ToolText(saved).find("\"replaced\":false") != std::string::npos &&
            ToolText(saved).find(
                R"("formats":[{"name":"9x16","width":1080,"height":1920}])") !=
                std::string::npos,
        "a composition goes to the shelf, at the size it has: " +
            ToolText(saved));
    // Nothing of the project changed: no revision, nothing to undo.
    Check(text(savedResult, "revision").empty() ||
              text(savedResult, "revision") == text(addedResult, "revision"),
          "saving a piece leaves the project's revision alone: " +
              ToolText(saved));

    const mcp_json::Value again =
        ToolCall(port, 4, "save_motion_template", saving);
    Check(!ToolOk(again) &&
              ToolText(again).find("\"error\":\"TemplateExists\"") !=
                  std::string::npos,
          "a piece of that name is not replaced unasked: " + ToolText(again));
    std::string replacing = saving;
    replacing.insert(replacing.size() - 1, R"(,"overwrite":true)");
    const mcp_json::Value replaced =
        ToolCall(port, 5, "save_motion_template", replacing);
    Check(
        ToolOk(replaced) &&
            ToolText(replaced).find("\"replaced\":true") != std::string::npos &&
            ToolText(replaced).find("\"version\":2,") != std::string::npos,
        "and gets its next version, when asked: " + ToolText(replaced));
    const mcp_json::Value shadow =
        ToolCall(port, 6, "save_motion_template",
                 R"({"composition_id":")" + first +
                     R"(","template":"nom","description":"Un autre nom.",)"
                     R"("overwrite":true})");
    Check(!ToolOk(shadow) &&
              ToolText(shadow).find("\"error\":\"TemplateExists\"") !=
                  std::string::npos &&
              !std::filesystem::exists(library / "nom.json"),
          "a shipped template's name is never taken: " + ToolText(shadow));
    const mcp_json::Value outside =
        ToolCall(port, 7, "save_motion_template",
                 R"({"composition_id":")" + first +
                     R"(","template":"../dehors","description":"Ailleurs."})");
    Check(!ToolOk(outside) &&
              ToolText(outside).find("\"error\":\"ValidationFailed\"") !=
                  std::string::npos &&
              !std::filesystem::exists(directory / "dehors.json"),
          "a name that is a path is refused: " + ToolText(outside));
    const mcp_json::Value mute = ToolCall(
        port, 8, "save_motion_template",
        R"({"composition_id":")" + first + R"(","template":"muette"})");
    Check(!ToolOk(mute) && !std::filesystem::exists(library / "muette.json"),
          "a piece without a description is refused: " + ToolText(mute));

    // A composition that instantiates another holds something of the
    // project.
    const mcp_json::Value holder =
        ToolCall(port, 9, "add_motion_composition",
                 R"({"name":"Double","definition":{"width":1080,"height":1920,)"
                 R"("fps":25,"duration":50,"root":{"type":"fill","children":[)"
                 R"({"type":"composition","id":"reprise","composition":")" +
                     first + R"("}]}}})");
    const mcp_json::Value holderResult = parsed(holder);
    const std::string second =
        holderResult.Find("composition") != nullptr
            ? text(*holderResult.Find("composition"), "id")
            : std::string();
    const mcp_json::Value bound = ToolCall(
        port, 10, "save_motion_template",
        R"({"composition_id":")" + second +
            R"(","template":"double","description":"Deux fois le nom."})");
    Check(ToolOk(holder) && !ToolOk(bound) &&
              ToolText(bound).find("\"error\":\"ProjectBound\"") !=
                  std::string::npos &&
              ToolText(bound).find("'reprise' (composition)") !=
                  std::string::npos &&
              !std::filesystem::exists(library / "double.json"),
          "a composition that holds another is refused, node named: " +
              ToolText(holder) + " / " + ToolText(bound));

    // A file that is not a piece is reported, and hides nothing else.
    {
        std::ofstream broken(library / "cassee.json");
        broken << "{\"width\":1080}";
    }
    const mcp_json::Value listed =
        ToolCall(port, 11, "list_motion_templates", "{}");
    const mcp_json::Value listedResult = parsed(listed);
    const mcp_json::Value* templates = listedResult.Find("templates");
    const mcp_json::Value* piece = nullptr;
    size_t shipped = 0;
    if (templates != nullptr && templates->IsArray()) {
        for (const mcp_json::Value& entry : templates->AsArray()) {
            if (text(entry, "name") == "nom-client") piece = &entry;
            if (text(entry, "source") == "shipped") ++shipped;
        }
    }
    Check(ToolOk(listed) && piece != nullptr && shipped >= 3 &&
              text(*piece, "source") == "library" &&
              piece->Find("version") != nullptr &&
              piece->Find("version")->Dump() == "2" &&
              text(*piece, "description") == "Nom et fonction." &&
              text(*piece, "use") == "Une personne qui parle." &&
              text(*piece, "avoid") == "Un plan large." &&
              text(*piece, "origin") == "essai MB1" &&
              piece->Find("invariants") != nullptr &&
              piece->Find("invariants")->Dump() ==
                  R"(["Le nom entre par la gauche."])" &&
              piece->Find("props") != nullptr &&
              piece->Find("props")->Find("nom") != nullptr,
          "the catalog lists the piece with what it says of itself: " +
              ToolText(listed));
    const mcp_json::Value* issues =
        listedResult.Find("library") != nullptr
            ? listedResult.Find("library")->Find("issues")
            : nullptr;
    Check(issues != nullptr && issues->IsArray() &&
              issues->AsArray().size() == 1 &&
              text(issues->AsArray()[0], "path") ==
                  (library / "cassee.json").string() &&
              text(issues->AsArray()[0], "message")
                      .find("unknown field 'width'") != std::string::npos,
          "and names the file that is not one: " + ToolText(listed));
    // DEC11 -- docs/roadmap/MOTION-WEB-2026-10.md. A search of the shelf:
    // the library piece by its use, a shipped one by its description, and
    // nothing at all, each with the size of the whole shelf.
    const auto names = [&](const mcp_json::Value& result) {
        std::vector<std::string> found;
        const mcp_json::Value* list = result.Find("templates");
        if (list != nullptr && list->IsArray()) {
            for (const mcp_json::Value& entry : list->AsArray()) {
                found.push_back(text(entry, "name"));
            }
        }
        return found;
    };
    const size_t shelf = templates != nullptr && templates->IsArray()
                             ? templates->AsArray().size()
                             : 0;
    const mcp_json::Value byUse = ToolCall(port, 40, "list_motion_templates",
                                           R"({"query":"PERSONNE qui parle"})");
    const mcp_json::Value byUseResult = parsed(byUse);
    Check(ToolOk(byUse) &&
              names(byUseResult) == std::vector<std::string>{"nom-client"} &&
              text(byUseResult, "query") == "PERSONNE qui parle" &&
              byUseResult.Find("total") != nullptr &&
              byUseResult.Find("total")->Dump() == std::to_string(shelf) &&
              byUseResult.Find("kits") != nullptr,
          "a search finds the library piece by its use: " + ToolText(byUse));
    const mcp_json::Value byDescription = ToolCall(
        port, 41, "list_motion_templates", R"({"query":"name on screen"})");
    Check(ToolOk(byDescription) &&
              names(parsed(byDescription)) == std::vector<std::string>{"nom"},
          "and a shipped one by its description: " + ToolText(byDescription));
    const mcp_json::Value none = ToolCall(port, 42, "list_motion_templates",
                                          R"({"query":"karaoke volcan"})");
    Check(ToolOk(none) && names(parsed(none)).empty() &&
              ToolText(none).find("\"templates\":[]") != std::string::npos,
          "a search that finds nothing is an empty list: " + ToolText(none));
    const mcp_json::Value notText =
        ToolCall(port, 43, "list_motion_templates", R"({"query":3})");
    Check(!ToolOk(notText) &&
              ToolText(notText).find("query") != std::string::npos,
          "a query is text: " + ToolText(notText));
    const mcp_json::Value notOne =
        ToolCall(port, 12, "add_motion_template", R"({"template":"cassee"})");
    Check(!ToolOk(notOne) &&
              ToolText(notOne).find("\"error\":\"UnknownTemplate\"") !=
                  std::string::npos &&
              ToolText(notOne).find("is not a piece") != std::string::npos,
          "adding it says why it is not a piece: " + ToolText(notOne));

    // And back into a project: a copy, with identities of its own.
    const mcp_json::Value taken = ToolCall(port, 13, "add_motion_template",
                                           R"({"template":"nom-client"})");
    const mcp_json::Value takenResult = parsed(taken);
    const std::string third = text(takenResult, "composition_id");
    const auto rootUid = [&](const mcp_json::Value& result) {
        const mcp_json::Value* composition = result.Find("composition");
        const mcp_json::Value* definition =
            composition != nullptr ? composition->Find("definition") : nullptr;
        const mcp_json::Value* root =
            definition != nullptr ? definition->Find("root") : nullptr;
        return root != nullptr ? text(*root, "uid") : std::string();
    };
    Check(ToolOk(taken) && !third.empty() && third != first &&
              text(takenResult, "template") == "nom-client" &&
              text(takenResult, "format") == "9x16" &&
              takenResult.Find("invariants") != nullptr &&
              !rootUid(takenResult).empty() &&
              rootUid(takenResult) != rootUid(addedResult),
          "the piece comes back as a composition of its own, invariants in "
          "hand: " +
              ToolText(taken));
    // The whole library gone, base and inbox: the project keeps its copy.
    std::filesystem::remove_all(library);
    const mcp_json::Value kept =
        ToolCall(port, 14, "describe_motion_composition",
                 R"({"composition_id":")" + third + R"("})");
    Check(ToolOk(kept) && ToolText(kept).find("\"uid\"") != std::string::npos,
          "and stays in the project once the piece leaves the shelf: " +
              ToolText(kept));

    // A piece added with its formats goes back to the shelf with all of
    // them, not only the timeline's.
    const mcp_json::Value whole =
        ToolCall(port, 15, "add_motion_template",
                 R"({"template":"nom","keep_formats":true})");
    const mcp_json::Value wholeResult = parsed(whole);
    const std::string three =
        R"("formats":[{"name":"9x16","width":1080,"height":1920},)"
        R"({"name":"16x9","width":1920,"height":1080},)"
        R"({"name":"1x1","width":1080,"height":1080}])";
    Check(ToolOk(whole) && text(wholeResult, "format") == "9x16" &&
              ToolText(whole).find(three) != std::string::npos,
          "keep_formats keeps every format of the piece: " + ToolText(whole));
    const mcp_json::Value wholeSaved = ToolCall(
        port, 16, "save_motion_template",
        R"({"composition_id":")" + text(wholeResult, "composition_id") +
            R"(","template":"nom-entier","description":"Nom, trois formats."})");
    Check(ToolOk(wholeSaved) &&
              ToolText(wholeSaved).find(three) != std::string::npos,
          "and the piece saved from it declares the three: " +
              ToolText(wholeSaved));
    const mcp_json::Value notAFlag =
        ToolCall(port, 17, "add_motion_template",
                 R"({"template":"nom","keep_formats":"oui"})");
    Check(!ToolOk(notAFlag) &&
              ToolText(notAFlag).find("\"error\":\"ValidationFailed\"") !=
                  std::string::npos,
          "keep_formats is a boolean: " + ToolText(notAFlag));

    // MB4 -- the charters of the library: listed, and one piece added with
    // two of them gives two compositions to the charter.
    std::filesystem::create_directories(library / "kits");
    {
        std::ofstream(library / "kits" / "bleu.json")
            << R"({"format":"motion-kit.v1","description":"Charte bleue.",)"
               R"("principle":"Une barre sous chaque titre.",)"
               R"("props":{"accent":"#0000FF","fond":"#101010",)"
               R"("police":"Avenir Next"}})";
        std::ofstream(library / "kits" / "rose.json")
            << R"({"format":"motion-kit.v1","description":"Charte rose.",)"
               R"("props":{"fond":"#E0A030","police":"Helvetica"}})";
    }
    const mcp_json::Value withKits =
        ToolCall(port, 18, "list_motion_templates", "{}");
    Check(ToolOk(withKits) &&
              ToolText(withKits).find(
                  R"("kits":[{"name":"bleu","version":1,)"
                  R"("description":"Charte bleue.",)"
                  R"("principle":"Une barre sous chaque titre.",)") !=
                  std::string::npos,
          "list_motion_templates lists the kits: " + ToolText(withKits));
    const mcp_json::Value blue =
        ToolCall(port, 19, "add_motion_template",
                 R"({"template":"carton","name":"Carton bleu","kit":"bleu"})");
    const mcp_json::Value pink =
        ToolCall(port, 20, "add_motion_template",
                 R"({"template":"carton","name":"Carton rose","kit":"rose"})");
    Check(ToolOk(blue) &&
              ToolText(blue).find(
                  R"("kit":{"name":"bleu","applied":["fond","police"],)"
                  R"("not_in_piece":["accent"],)"
                  R"("principle":"Une barre sous chaque titre."})") !=
                  std::string::npos &&
              ToolText(blue).find(
                  R"("police":{"type":"text","default":"Avenir Next"})") !=
                  std::string::npos &&
              ToolText(blue).find(
                  R"("fond":{"type":"color","default":"#101010"})") !=
                  std::string::npos,
          "a card added with the blue kit: " + ToolText(blue));
    Check(ToolOk(pink) &&
              ToolText(pink).find(
                  R"("police":{"type":"text","default":"Helvetica"})") !=
                  std::string::npos &&
              ToolText(pink).find(
                  R"("fond":{"type":"color","default":"#E0A030"})") !=
                  std::string::npos,
          "the same card with the pink kit: " + ToolText(pink));
    const mcp_json::Value noKit =
        ToolCall(port, 21, "add_motion_template",
                 R"({"template":"carton","kit":"vert"})");
    Check(!ToolOk(noKit) &&
              ToolText(noKit).find("\"error\":\"UnknownKit\"") !=
                  std::string::npos &&
              ToolText(noKit).find("known: bleu, rose") != std::string::npos,
          "an unknown kit is refused with the kits there are: " +
              ToolText(noKit));

    // DEC6 -- a brand's charter: a caption style made from it at the
    // timeline's size, and the check that names it in the policy.
    {
        // M5b -- `pieces`: the second line of nom in green on white.
        std::ofstream(library / "kits" / "marque.json")
            << R"({"format":"brand-kit.v1","description":"Marque.",)"
               R"("props":{"fond":"#0A7C66","police":"Avenir Next"},)"
               R"("palette":["#FFFFFF"],)"
               R"("pieces":{"nom":{"fond_fonction":"#FFFFFF",)"
               R"("texte_fonction":"#0A7C66"}},)"
               R"("captions":{"font_family":"Avenir Next",)"
               R"("color":"#FFFFFF","formats":{)"
               R"("9x16":{"width":1080,"height":1920,"font_size":72},)"
               R"("16x9":{"width":1920,"height":1080,"font_size":56}}}})";
    }
    const mcp_json::Value branded =
        ToolCall(port, 22, "add_caption_style",
                 R"({"kit":"marque","background_color":"#000000@0.5"})");
    Check(ToolOk(branded) && !text(parsed(branded), "style_id").empty() &&
              ToolText(branded).find(
                  R"("kit":{"name":"marque","version":1,"format":"9x16",)"
                  R"("set":["font_family","color","font_size"]})") !=
                  std::string::npos,
          "a caption style from the charter's 9:16 captions: " +
              ToolText(branded));
    const mcp_json::Value overridden = ToolCall(
        port, 23, "add_caption_style", R"({"kit":"marque","font_size":80})");
    Check(ToolOk(overridden) &&
              ToolText(overridden).find(R"("overridden":["font_size"])") !=
                  std::string::npos,
          "a setting given beside the charter wins, and is said: " +
              ToolText(overridden));
    const mcp_json::Value motionKit =
        ToolCall(port, 24, "add_caption_style", R"({"kit":"bleu"})");
    Check(!ToolOk(motionKit) &&
              ToolText(motionKit).find("\"error\":\"KitMismatch\"") !=
                  std::string::npos &&
              ToolText(motionKit).find("says nothing of captions") !=
                  std::string::npos,
          "a motion kit has no captions to give: " + ToolText(motionKit));
    const mcp_json::Value judged =
        ToolCall(port, 25, "check_timeline", R"({"policy":{"kit":"marque"}})");
    Check(ToolOk(judged) &&
              ToolText(judged).find(
                  R"("name":"charter","status":"passed","kit":)"
                  R"({"name":"marque","version":1,)") != std::string::npos,
          "check_timeline judges against the charter it names: " +
              ToolText(judged));
    const mcp_json::Value unknown =
        ToolCall(port, 26, "check_timeline", R"({"policy":{"kit":"vert"}})");
    Check(
        !ToolOk(unknown) &&
            ToolText(unknown).find("\"error\":\"UnknownKit\"") !=
                std::string::npos,
        "a charter the library does not hold is refused: " + ToolText(unknown));
    // M5b -- the charter's `pieces` reach the piece through the server, and
    // the shelf lists them.
    const mcp_json::Value named =
        ToolCall(port, 27, "add_motion_template",
                 R"({"template":"nom","kit":"marque"})");
    Check(ToolOk(named) &&
              ToolText(named).find(
                  R"("kit":{"name":"marque","applied":["police",)"
                  R"("fond_fonction","texte_fonction"],)"
                  R"("not_in_piece":["fond"])") != std::string::npos &&
              ToolText(named).find(
                  R"("texte_fonction":{"type":"color","default":"#0A7C66"})") !=
                  std::string::npos,
          "nom added with the charter wears its second line in green on "
          "white: " +
              ToolText(named));
    const mcp_json::Value withPieces =
        ToolCall(port, 28, "list_motion_templates", "{}");
    Check(ToolOk(withPieces) &&
              ToolText(withPieces)
                      .find(R"("pieces":{"nom":{"fond_fonction":"#FFFFFF",)"
                            R"("texte_fonction":"#0A7C66"}})") !=
                  std::string::npos,
          "the shelf lists what the charter gives each piece: " +
              ToolText(withPieces));
    {
        std::ofstream(library / "kits" / "autre.json")
            << R"({"format":"brand-kit.v1","description":"Autre.",)"
               R"("pieces":{"nom":{"couleur":"#000000"}}})";
    }
    const mcp_json::Value mismatched = ToolCall(
        port, 29, "add_motion_template", R"({"template":"nom","kit":"autre"})");
    Check(!ToolOk(mismatched) &&
              ToolText(mismatched).find("\"error\":\"KitMismatch\"") !=
                  std::string::npos &&
              ToolText(mismatched).find("has no prop 'couleur'") !=
                  std::string::npos,
          "a charter naming a prop the piece lacks is refused by name: " +
              ToolText(mismatched));
    // M5b -- a charter that withdraws a piece refuses it with its reason,
    // and the shelf says so under the charter.
    {
        std::ofstream(library / "kits" / "retrait.json")
            << R"({"format":"brand-kit.v1","description":"Retrait.",)"
               R"("withdrawn":{"carton":"pas besoin pour cette marque"}})";
    }
    const mcp_json::Value withdrawnCard =
        ToolCall(port, 30, "add_motion_template",
                 R"({"template":"carton","kit":"retrait"})");
    Check(!ToolOk(withdrawnCard) &&
              ToolText(withdrawnCard).find("\"error\":\"WithdrawnByKit\"") !=
                  std::string::npos &&
              ToolText(withdrawnCard).find("pas besoin pour cette marque") !=
                  std::string::npos,
          "a piece the charter withdraws is refused with the reason: " +
              ToolText(withdrawnCard));
    const mcp_json::Value listedWithdrawal =
        ToolCall(port, 31, "list_motion_templates", "{}");
    Check(ToolOk(listedWithdrawal) &&
              ToolText(listedWithdrawal)
                      .find(R"("withdrawn":{"carton":"pas besoin pour )"
                            R"(cette marque"})") != std::string::npos,
          "the shelf lists the pieces a charter withdraws: " +
              ToolText(listedWithdrawal));
    server.Stop();
}

void CheckMotionTemplatesOverMcp(const std::filesystem::path& directory) {
    InstallMotionFrameRenderer();
    Document document;
    document.sequence.width = 1080;
    document.sequence.height = 1920;
    const Ulid source = "01K50000000000000000000001";
    const Ulid v1 = "01K50000000000000000000002";
    const Ulid v2 = "01K50000000000000000000003";
    const Ulid clipA = "01K50000000000000000000004";
    const Ulid clipB = "01K50000000000000000000005";
    const Ulid cutaway = "01K50000000000000000000006";
    document.sources = {{source, "folder/A.MP4", {25, 1}, {1000, 25}}};
    DocumentClip a;
    a.id = clipA;
    a.source_id = source;
    a.source_in = {0, 25};
    a.duration = {150, 25};
    a.timeline_in = {0, 25};
    DocumentClip b = a;
    b.id = clipB;
    b.source_in = {150, 25};
    b.timeline_in = {150, 25};
    DocumentClip over = a;
    over.id = cutaway;
    over.source_in = {500, 25};
    over.duration = {25, 25};
    over.timeline_in = {250, 25};
    // IA3 -- the sound of A and B, for a name placed on a block of speech.
    DocumentClip soundA = a;
    soundA.id = "01K50000000000000000000008";
    DocumentClip soundB = b;
    soundB.id = "01K50000000000000000000009";
    document.sequence.tracks = {
        {v1, "video", 0, {a, b}},
        {v2, "video", 1, {over}},
        {"01K50000000000000000000007", "audio", 2, {soundA, soundB}}};
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Templates.cutmachine-project").string(),
              Project::FromDocument(document, "Templates"), path, error),
          "templates package saves: " + error);
    {
        Transcript words;
        words.media_id = source;
        words.whisper_model = "ggml-test.bin";
        words.source_rate = {25, 1};
        words.words = {
            {"Bonjour", {10, 25}, {30, 25}},  {"à", {30, 25}, {36, 25}},
            {"tous.", {36, 25}, {60, 25}},    {"Je", {70, 25}, {80, 25}},
            {"dirige", {80, 25}, {100, 25}},  {"le", {100, 25}, {106, 25}},
            {"studio.", {106, 25}, {140, 25}}};
        const std::filesystem::path transcripts =
            std::filesystem::path(path).parent_path() / ".cutmachine" /
            "transcripts";
        std::filesystem::create_directories(transcripts);
        Check(SaveAudioTranscript((transcripts / (source + ".json")).string(),
                                  words, error),
              "templates transcript saves: " + error);
    }
    McpProjectBackend backend(path);
    McpServer server(backend);
    Check(server.Start(0, error), "templates MCP server starts: " + error);
    const int port = server.Port();

    const mcp_json::Value listed =
        ToolCall(port, 1, "list_motion_templates", "{}");
    Check(ToolOk(listed) &&
              ToolText(listed).find("\"name\":\"nom\"") != std::string::npos &&
              ToolText(listed).find("\"name\":\"logo\"") != std::string::npos,
          "list_motion_templates over MCP: " + ToolText(listed));
    // AX-2026-09 AX6 -- the families a composition can name, a registered
    // directory's among them, narrowed by part of a name without case; and
    // whether a caption export finds each, or why that is not known.
    const mcp_json::Value fonts =
        ToolCall(port, 31, "list_fonts",
                 R"({"query":"FIRA sans","font_dirs":[")" +
                     std::string(MOTION_FONT_DIR) + R"("]})");
    Check(ToolOk(fonts) &&
              ToolText(fonts).find("\"Fira Sans Condensed\"") !=
                  std::string::npos &&
              ToolText(fonts).find("\"captions\":{\"checked\":") !=
                  std::string::npos,
          "list_fonts over MCP: " + ToolText(fonts));
    const mcp_json::Value noFonts = ToolCall(
        port, 32, "list_fonts", R"({"font_dirs":["/nonexistent/fonts"]})");
    Check(!ToolOk(noFonts) && ToolText(noFonts).find("\"error\":\"IoError\"") !=
                                  std::string::npos,
          "list_fonts refuses a font directory that is not there: " +
              ToolText(noFonts));
    const mcp_json::Value added =
        ToolCall(port, 2, "add_motion_template", R"({"template":"nom"})");
    mcp_json::Value addedResult;
    mcp_json::Value::Parse(ToolText(added), addedResult, error);
    const mcp_json::Value* composition = addedResult.Find("composition_id");
    Check(ToolOk(added) && composition != nullptr &&
              ToolText(added).find("\"format\":\"9x16\"") != std::string::npos,
          "add_motion_template picks the 9x16 layout: " + ToolText(added));
    if (composition == nullptr) {
        server.Stop();
        return;
    }
    const std::string id = composition->AsString();
    const mcp_json::Value unknown =
        ToolCall(port, 3, "add_motion_template", R"({"template":"generique"})");
    Check(!ToolOk(unknown) &&
              ToolText(unknown).find("UnknownTemplate") != std::string::npos,
          "an unknown template is refused by name: " + ToolText(unknown));

    // A word too long for the band, and a margin inside the safe area:
    // refused, and nothing is posed. DEC34 -- this timeline is 9:16, whose
    // default zone is now the social one: these cases name the 5 % zone
    // they were written against.
    const mcp_json::Value split =
        ToolCall(port, 4, "place_motion_clip",
                 R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
                     R"(","zone":"default",)"
                     R"("props":{"nom":"Anticonstitutionnellementissimement",)"
                     R"("corps":110}})");
    Check(!ToolOk(split) &&
              ToolText(split).find("\"error\":\"TextDoesNotFit\"") !=
                  std::string::npos &&
              ToolText(split).find("Anticonstitution") != std::string::npos,
          "a word broken between two letters is refused: " + ToolText(split));
    const mcp_json::Value outside = ToolCall(
        port, 5, "place_motion_clip",
        R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
            R"(","zone":"default","props":{"nom":"Camille Martin","marge":2}})");
    Check(!ToolOk(outside) &&
              ToolText(outside).find("\"error\":\"OutsideSafeArea\"") !=
                  std::string::npos &&
              ToolText(outside).find("left edge") != std::string::npos,
          "a name 2 % from the edge is refused: " + ToolText(outside));
    // PRD38 -- the zone of a vertical social video. The name 22 % from the
    // bottom, its old 9:16 height, sits under the apps' caption: refused in
    // that zone, which the default one would have taken (the case of 29
    // September 2026). A zone and a uniform margin together are refused.
    const mcp_json::Value social =
        ToolCall(port, 50, "place_motion_clip",
                 R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
                     R"(","props":{"nom":"Camille Martin",)"
                     R"("fonction":"Directrice artistique","hauteur":22},)"
                     R"("zone":"social_vertical"})");
    Check(!ToolOk(social) &&
              ToolText(social).find("\"error\":\"OutsideSafeArea\"") !=
                  std::string::npos &&
              ToolText(social).find("social_vertical safe area") !=
                  std::string::npos &&
              ToolText(social).find("bottom edge") != std::string::npos,
          "in the social zone the default name is refused by its bottom: " +
              ToolText(social));
    // DEC34 -- docs/roadmap/MOTION-2026-09.md. The same name without a
    // zone: on this 9:16 timeline the social zone is the default, and the
    // refusal says it chose it.
    const mcp_json::Value unnamed =
        ToolCall(port, 56, "place_motion_clip",
                 R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
                     R"(","props":{"nom":"Camille Martin",)"
                     R"("fonction":"Directrice artistique","hauteur":22}})");
    Check(
        !ToolOk(unnamed) &&
            ToolText(unnamed).find("\"error\":\"OutsideSafeArea\"") !=
                std::string::npos &&
            ToolText(unnamed).find(
                "social_vertical safe area (top 14 %, bottom 35 %, left 6 "
                "%, right 12.96 %), the default on a 9:16 timeline") !=
                std::string::npos &&
            ToolText(unnamed).find("bottom edge") != std::string::npos,
        "on a 9:16 timeline, no zone is the social zone: " + ToolText(unnamed));
    const mcp_json::Value mixed =
        ToolCall(port, 51, "place_motion_clip",
                 R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
                     R"(","zone":"social_vertical","margin_percent":10})");
    Check(
        !ToolOk(mixed) &&
            ToolText(mixed).find("margin_percent") != std::string::npos,
        "a zone and a margin_percent together are refused: " + ToolText(mixed));
    Project project;
    Check(LoadStoredProject(path, project, error) &&
              project.timelines.front().tracks[1].clips.size() == 1,
          "a refused placement poses nothing: " + error);

    // On clip A: V2 over [0, 100), the cutaway at 250 left where it was.
    // In the 5 % zone, named (DEC34), at the old 9:16 height so the checks
    // below can read it outside the social zone.
    const mcp_json::Value placed =
        ToolCall(port, 6, "place_motion_clip",
                 R"({"composition_id":")" + id + R"(","clip_id":")" + clipA +
                     R"(","zone":"default","props":{"nom":"Camille Martin",)"
                     R"("fonction":"Directrice artistique","hauteur":22}})");
    const std::string placedText = ToolText(placed);
    Check(
        ToolOk(placed) &&
            placedText.find("\"track_id\":\"" + v2 + "\"") !=
                std::string::npos &&
            placedText.find("\"timeline_in\":{\"value\":0,\"rate\":25}") !=
                std::string::npos &&
            placedText.find("\"duration\":{\"value\":100,\"rate\":25}") !=
                std::string::npos &&
            placedText.find("\"checked_frames\":100,\"inside\":true") !=
                std::string::npos &&
            placedText.find("zone_chosen_by") == std::string::npos,
        "place_motion_clip poses the name on V2 over [0, 100): " + placedText);
    Check(LoadStoredProject(path, project, error) &&
              project.timelines.front().tracks[1].clips.size() == 2,
          "the name is on V2: " + error);
    const std::vector<DocumentClip>& onV2 =
        project.timelines.front().tracks[1].clips;
    Check(std::any_of(onV2.begin(), onV2.end(),
                      [&](const DocumentClip& clip) {
                          return clip.id == cutaway &&
                                 clip.timeline_in == RationalTime{250, 25};
                      }),
          "the cutaway after it has not moved");
    mcp_json::Value placedResult;
    mcp_json::Value::Parse(placedText, placedResult, error);
    const mcp_json::Value* clipId = placedResult.Find("clip_id");
    const mcp_json::Value checked = ToolCall(
        port, 7, "check_motion_safe_area",
        R"({"clip_id":")" + (clipId != nullptr ? clipId->AsString() : "") +
            R"(","margin_percent":10})");
    Check(ToolOk(checked) &&
              ToolText(checked).find("\"margin_percent\":10") !=
                  std::string::npos &&
              ToolText(checked).find("\"kind\":\"outside_safe_area\"") !=
                  std::string::npos,
          "at a 10 % margin the 8 % name is outside: " + ToolText(checked));
    // PRD38 -- the posed name, read again in the social zone and with a
    // bottom margin of its own: the report names the zone it applied.
    const std::string posedId = clipId != nullptr ? clipId->AsString() : "";
    const mcp_json::Value socialCheck = ToolCall(
        port, 52, "check_motion_safe_area",
        R"({"clip_id":")" + posedId + R"(","zone":"social_vertical"})");
    const std::string socialText = ToolText(socialCheck);
    Check(
        ToolOk(socialCheck) &&
            socialText.find("\"zone\":\"social_vertical\"") !=
                std::string::npos &&
            socialText.find("\"inside\":false") != std::string::npos &&
            socialText.find("\"bottom\"]") != std::string::npos &&
            socialText.find("margin_percent") == std::string::npos,
        "in the social zone the posed name crosses the bottom: " + socialText);
    // DEC34 -- read again without a zone: the timeline's, social_vertical,
    // and the report says the timeline chose it.
    const mcp_json::Value unnamedCheck =
        ToolCall(port, 57, "check_motion_safe_area",
                 R"({"clip_id":")" + posedId + R"("})");
    const std::string unnamedText = ToolText(unnamedCheck);
    Check(ToolOk(unnamedCheck) &&
              unnamedText.find("\"zone\":\"social_vertical\"") !=
                  std::string::npos &&
              unnamedText.find("\"zone_chosen_by\":\"timeline_9x16\"") !=
                  std::string::npos &&
              unnamedText.find("\"inside\":false") != std::string::npos,
          "without a zone, the posed name is read in the social zone: " +
              unnamedText);
    // The shipped 9:16 layout, 36 % from the bottom and 9 % from the left
    // (the author, 24 October 2026), fits the social zone on every frame, its
    // slide-in included, with a long name and function.
    const mcp_json::Value shippedSocial =
        ToolCall(port, 59, "check_motion_safe_area",
                 R"({"composition_id":")" + id +
                     R"(","zone":"social_vertical","props":)"
                     R"({"nom":"Marie-Hélène Dubois",)"
                     R"("fonction":"Responsable du studio"}})");
    Check(ToolOk(shippedSocial) &&
              ToolText(shippedSocial).find("\"inside\":true") !=
                  std::string::npos,
          "the shipped 9:16 name fits the social zone: " +
              ToolText(shippedSocial));
    // A composition checked alone has no timeline: the default zone.
    const mcp_json::Value aloneUnnamed =
        ToolCall(port, 58, "check_motion_safe_area",
                 R"({"composition_id":")" + id +
                     R"(","props":{"nom":"Camille Martin",)"
                     R"("fonction":"Directrice artistique"}})");
    Check(
        ToolOk(aloneUnnamed) &&
            ToolText(aloneUnnamed).find("\"zone\":\"default\"") !=
                std::string::npos &&
            ToolText(aloneUnnamed).find("zone_chosen_by") == std::string::npos,
        "a composition alone keeps the default zone: " +
            ToolText(aloneUnnamed));
    const mcp_json::Value customCheck =
        ToolCall(port, 53, "check_motion_safe_area",
                 R"({"clip_id":")" + posedId + R"(","margins":{"bottom":35}})");
    Check(
        ToolOk(customCheck) &&
            ToolText(customCheck).find("\"zone\":\"custom\"") !=
                std::string::npos &&
            ToolText(customCheck)
                    .find("\"margins\":{\"top\":5,\"bottom\":35,") !=
                std::string::npos &&
            ToolText(customCheck).find("\"inside\":false") != std::string::npos,
        "a bottom margin of 35 % alone: " + ToolText(customCheck));
    // MM3 -- the composition rendered alone, as render_motion_video renders
    // it: every frame, the name held at the bottom (its old 9:16 height),
    // at rest.
    const mcp_json::Value alone =
        ToolCall(port, 54, "check_motion_safe_area",
                 R"({"composition_id":")" + id +
                     R"(","props":{"nom":"Camille Martin",)"
                     R"("fonction":"Directrice artistique","hauteur":22},)"
                     R"("zone":"social_vertical","at_rest":true})");
    const std::string aloneText = ToolText(alone);
    Check(ToolOk(alone) &&
              aloneText.find("\"composition_id\":\"" + id + "\"") !=
                  std::string::npos &&
              aloneText.find("\"at_rest_only\":true") != std::string::npos &&
              aloneText.find("\"inside\":false") != std::string::npos &&
              aloneText.find("\"at_rest\":true") != std::string::npos,
          "a composition alone, checked frame by frame: the name stands "
          "below the social zone: " +
              aloneText);
    const mcp_json::Value twice = ToolCall(
        port, 55, "check_motion_safe_area",
        R"({"composition_id":")" + id + R"(","clip_id":")" + posedId + R"("})");
    Check(!ToolOk(twice) && ToolText(twice).find("exactly one of clip_id") !=
                                std::string::npos,
          "a clip and a composition together are refused: " + ToolText(twice));
    const mcp_json::Value undone = ToolCall(port, 8, "undo", "{}");
    Check(ToolOk(undone) && LoadStoredProject(path, project, error) &&
              project.timelines.front().tracks[1].clips.size() == 1 &&
              project.timelines.front().tracks[1].clips[0].timeline_in ==
                  RationalTime{250, 25},
          "one undo takes the name off and leaves the cutaway: " +
              ToolText(undone));

    // IA3 -- on a block of speech: the second sentence, whole, with the
    // revision it was read with.
    const mcp_json::Value blocks =
        ToolCall(port, 9, "list_speech_blocks", "{}");
    mcp_json::Value blockView;
    mcp_json::Value::Parse(ToolText(blocks), blockView, error);
    const mcp_json::Value* revision = blockView.Find("transcript_revision");
    const mcp_json::Value* blockList = blockView.Find("blocks");
    Check(ToolOk(blocks) && revision != nullptr && blockList != nullptr &&
              blockList->IsArray() && blockList->AsArray().size() == 2,
          "two sentences, two blocks: " + ToolText(blocks));
    if (revision == nullptr || blockList == nullptr ||
        blockList->AsArray().size() != 2) {
        server.Stop();
        return;
    }
    const mcp_json::Value& second = blockList->AsArray()[1];
    int64_t secondIn = -1;
    int64_t secondLength = -1;
    second.Find("timeline_in")->Find("value")->AsInt64(secondIn);
    second.Find("duration")->Find("value")->AsInt64(secondLength);
    const std::string onBlock = R"({"composition_id":")" + id +
                                R"(","block_id":"B2","transcript_revision":")" +
                                revision->AsString() +
                                R"(","zone":"default",)"
                                R"("props":{"nom":"Camille Martin"}})";
    const mcp_json::Value posed =
        ToolCall(port, 10, "place_motion_clip", onBlock);
    Check(ToolOk(posed) &&
              ToolText(posed).find(
                  "\"timeline_in\":{\"value\":" + std::to_string(secondIn) +
                  ",") != std::string::npos &&
              ToolText(posed).find(
                  "\"duration\":{\"value\":" + std::to_string(secondLength) +
                  ",") != std::string::npos,
          "a name on B2 covers the sentence, first word to last: " +
              ToolText(posed));
    Check(ToolOk(ToolCall(port, 11, "undo", "{}")), "the name comes off");
    const mcp_json::Value shorter = ToolCall(
        port, 12, "place_motion_clip",
        R"({"composition_id":")" + id +
            R"(","block_id":"B2","duration":{"value":50,"rate":25},)"
            R"("zone":"default","transcript_revision":")" +
            revision->AsString() + R"(","props":{"nom":"Camille Martin"}})");
    Check(
        ToolOk(shorter) &&
            ToolText(shorter).find(
                "\"timeline_in\":{\"value\":" + std::to_string(secondIn) +
                ",") != std::string::npos &&
            ToolText(shorter).find("\"duration\":{\"value\":50,\"rate\":25}") !=
                std::string::npos,
        "with a duration, it starts on the block and lasts that: " +
            ToolText(shorter));
    Check(ToolOk(ToolCall(port, 13, "undo", "{}")), "the name comes off again");
    const mcp_json::Value stale = ToolCall(
        port, 14, "place_motion_clip",
        R"({"composition_id":")" + id +
            R"(","block_id":"B2","transcript_revision":"0000000000000000",)"
            R"("props":{"nom":"Camille Martin"}})");
    Check(!ToolOk(stale) &&
              ToolText(stale).find("\"error\":\"StaleSelection\"") !=
                  std::string::npos,
          "a block read from another view is refused: " + ToolText(stale));
    const mcp_json::Value unknownBlock = ToolCall(
        port, 15, "place_motion_clip",
        R"({"composition_id":")" + id +
            R"(","block_id":"B9","transcript_revision":")" +
            revision->AsString() + R"(","props":{"nom":"Camille Martin"}})");
    Check(!ToolOk(unknownBlock) &&
              ToolText(unknownBlock).find("\"error\":\"UnknownBlock\"") !=
                  std::string::npos,
          "an unknown block is refused by name: " + ToolText(unknownBlock));
    server.Stop();
}

// AGENT-2026-09 A1 -- ROADMAP.md. The stdio transport, end to end through
// the built binary, as an MCP client spawns it from a `.mcp.json`: requests
// go in one per line, answers come out one per line, and nothing else
// reaches stdout -- the banner goes to stderr, a notification and a blank
// line get no answer, a line that is not JSON gets a parse error with a
// null id, and closing stdin ends the server, which lets the project go.
void CheckStdioTransport(const std::filesystem::path& directory) {
    std::string path;
    std::string error;
    Project stored = Project::FromDocument(Fixture(), "Stdio");
    Check(
        CreatePortableProject((directory / "Stdio.cutmachine-project").string(),
                              stored, path, error),
        "stdio package saves: " + error);
    const std::filesystem::path requests = directory / "stdio-requests.jsonl";
    const std::filesystem::path answers = directory / "stdio-answers.jsonl";
    const std::filesystem::path log = directory / "stdio-stderr.txt";
    {
        std::ofstream out(requests);
        out << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
               R"({"protocolVersion":"2025-06-18","capabilities":{},)"
               R"("clientInfo":{"name":"test","version":"0"}}})"
            << '\n'
            << R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
            << "\n\n"
            << R"({"jsonrpc":"2.0","id":2,"method":"ping"})" << '\n'
            << R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})" << '\n'
            << R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":)"
               R"({"name":"list_timelines","arguments":{}}})"
            << '\n'
            << "not json\n";
    }
    const std::string command = std::string("'") + CUTMACHINE_EXECUTABLE +
                                "' --mcp-serve --stdio '" + path + "' < '" +
                                requests.string() + "' > '" + answers.string() +
                                "' 2> '" + log.string() + "'";
    Check(std::system(command.c_str()) == 0,
          "the stdio server exits 0 once its client closes stdin");
    std::vector<mcp_json::Value> lines;
    {
        std::istringstream input(Read(answers));
        std::string line;
        while (std::getline(input, line)) {
            mcp_json::Value parsed;
            std::string parseError;
            Check(mcp_json::Value::Parse(line, parsed, parseError),
                  "every line on stdout is one JSON answer: " + parseError +
                      " in '" + line.substr(0, 80) + "'");
            lines.push_back(parsed);
        }
    }
    Check(lines.size() == 5,
          "five answers for five requests, none for the notification or "
          "the blank line: " +
              std::to_string(lines.size()));
    if (lines.size() == 5) {
        const auto id = [&](size_t index) {
            const mcp_json::Value* found = lines[index].Find("id");
            return found ? found->Dump() : std::string("(absent)");
        };
        Check(id(0) == "1" && id(1) == "2" && id(2) == "3" && id(3) == "4",
              "answers carry their request's id, in order");
        const mcp_json::Value* initialized = lines[0].Find("result");
        Check(initialized && initialized->Find("protocolVersion") &&
                  initialized->Find("protocolVersion")->AsString() ==
                      "2025-06-18" &&
                  initialized->Find("instructions") &&
                  !initialized->Find("instructions")->AsString().empty() &&
                  initialized->Find("serverInfo") &&
                  initialized->Find("serverInfo")->Find("name") &&
                  initialized->Find("serverInfo")->Find("name")->AsString() ==
                      "cutmachine",
              "initialize answers the client's version, with the server's "
              "instructions");
        Check(lines[1].Find("result") && lines[1].Find("result")->IsObject(),
              "ping answers an empty result");
        const mcp_json::Value* tools =
            lines[2].Find("result") ? lines[2].Find("result")->Find("tools")
                                    : nullptr;
        const McpToolRegistry registry;
        Check(tools && tools->IsArray() &&
                  tools->AsArray().size() == registry.Tools().size(),
              "tools/list over stdio is the whole catalog");
        const mcp_json::Value* content =
            lines[3].Find("result") ? lines[3].Find("result")->Find("content")
                                    : nullptr;
        mcp_json::Value payload;
        std::string parseError;
        Check(content && content->IsArray() && !content->AsArray().empty() &&
                  content->AsArray()[0].Find("text") &&
                  mcp_json::Value::Parse(
                      content->AsArray()[0].Find("text")->AsString(), payload,
                      parseError) &&
                  payload.Find("timelines") &&
                  lines[3].Find("result")->Find("isError") &&
                  !lines[3].Find("result")->Find("isError")->AsBool(),
              "a tool call over stdio answers like one over HTTP");
        const mcp_json::Value* parseFailure = lines[4].Find("error");
        Check(parseFailure && parseFailure->Find("code") &&
                  parseFailure->Find("code")->Dump() == "-32700" &&
                  lines[4].Find("id") && lines[4].Find("id")->IsNull(),
              "a line that is not JSON gets a parse error with a null id");
    }
    Check(Read(log).find("MCP server on stdio") != std::string::npos,
          "the banner goes to stderr, not into the protocol stream");
    Check(!std::filesystem::exists(path + ".lock"),
          "the project is let go when the client hangs up");

    // --stdio serves the process that spawned it: a port makes no sense.
    const std::string refused = std::string("'") + CUTMACHINE_EXECUTABLE +
                                "' --mcp-serve --stdio --port 1 '" + path +
                                "' > '" + answers.string() + "' 2> '" +
                                log.string() + "'";
    Check(std::system(refused.c_str()) != 0 &&
              Read(answers).find("\"error\":\"ValidationFailed\"") !=
                  std::string::npos,
          "--stdio with --port is refused: " + Read(answers));
}

// PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. `--profile` through
// the binary, as a client's configuration starts it: the server lists and
// serves the profile, says so on stderr, and a name no profile has stops it
// from starting. `--tools --profile` prints what it lists.
void CheckProfileThroughBinary(const std::filesystem::path& directory) {
    const McpToolRegistry motion(*FindMcpToolProfile("motion"));
    const std::filesystem::path requests = directory / "profile-requests.jsonl";
    const std::filesystem::path answers = directory / "profile-answers.jsonl";
    const std::filesystem::path log = directory / "profile-stderr.txt";
    {
        std::ofstream out(requests);
        out << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
               R"({"protocolVersion":"2025-06-18"}})"
            << '\n'
            << R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})" << '\n'
            << R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":)"
               R"({"name":"remove_words","arguments":{}}})"
            << '\n';
    }
    const std::string executable =
        std::string("'") + CUTMACHINE_EXECUTABLE + "'";
    const std::string command = executable +
                                " --mcp-serve --stdio --profile motion < '" +
                                requests.string() + "' > '" + answers.string() +
                                "' 2> '" + log.string() + "'";
    Check(std::system(command.c_str()) == 0,
          "a server started with --profile motion serves its client");
    const std::string answered = Read(answers);
    std::vector<mcp_json::Value> lines;
    {
        std::istringstream input(answered);
        std::string line;
        while (std::getline(input, line)) {
            mcp_json::Value parsed;
            std::string parseError;
            mcp_json::Value::Parse(line, parsed, parseError);
            lines.push_back(parsed);
        }
    }
    Check(lines.size() == 3, "three answers: " + answered.substr(0, 300));
    if (lines.size() == 3) {
        const mcp_json::Value* instructions =
            lines[0].Find("result")
                ? lines[0].Find("result")->Find("instructions")
                : nullptr;
        Check(instructions != nullptr &&
                  instructions->AsString().find("`motion` profile") !=
                      std::string::npos,
              "its instructions name the profile");
        const mcp_json::Value* tools =
            lines[1].Find("result") ? lines[1].Find("result")->Find("tools")
                                    : nullptr;
        Check(tools != nullptr &&
                  tools->AsArray().size() == motion.Tools().size(),
              "it lists the profile's tools: " +
                  std::to_string(tools ? tools->AsArray().size() : 0));
        Check(lines[2].Dump().find("ToolNotInProfile") != std::string::npos,
              "and refuses a tool outside it: " + lines[2].Dump());
    }
    Check(Read(log).find(
              "Profile 'motion': " + std::to_string(motion.Tools().size()) +
              " of " + std::to_string(motion.CatalogSize()) + " tools") !=
              std::string::npos,
          "stderr says the profile: " + Read(log));

    const auto refusedStart = [&](const std::string& options,
                                  const std::string& said) {
        // A refusal of the command line is its JSON answer, on stdout.
        const std::string refused = executable + " --mcp-serve --stdio " +
                                    options + " < /dev/null > '" +
                                    log.string() + "' 2> /dev/null";
        Check(std::system(refused.c_str()) != 0 &&
                  Read(log).find(said) != std::string::npos,
              "--mcp-serve " + options + " does not start: " + Read(log));
    };
    refusedStart("--profile nope",
                 "takes one of: full, motion, itw, reel; 'nope'");
    refusedStart("--profile", "needs a value: one of full, motion, itw, reel");

    const std::filesystem::path listing = directory / "profile-tools.json";
    Check(std::system((executable + " --tools --profile motion > '" +
                       listing.string() + "'")
                          .c_str()) == 0,
          "--tools --profile motion prints a listing");
    mcp_json::Value printed;
    std::string parseError;
    mcp_json::Value::Parse(Read(listing), printed, parseError);
    Check(printed.Find("tools") != nullptr &&
              printed.Find("tools")->AsArray().size() == motion.Tools().size(),
          "of the profile's tools");
}

// PRD18 -- ROADMAP.md (ACP4). The procedures, from the binary a client
// spawns with no project: what Claude Code started outside the repository
// sees. The prompts a person starts by hand, the tool a model calls, and the
// tools that still need a project, refused as before.
void CheckProceduresOverStdio(const std::filesystem::path& directory) {
    const std::filesystem::path requests =
        directory / "procedure-requests.jsonl";
    const std::filesystem::path answers = directory / "procedure-answers.jsonl";
    const std::filesystem::path log = directory / "procedure-stderr.txt";
    {
        std::ofstream out(requests);
        out << R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)"
               R"({"protocolVersion":"2025-11-25","capabilities":{},)"
               R"("clientInfo":{"name":"test","version":"0"}}})"
            << '\n'
            << R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
            << '\n'
            << R"({"jsonrpc":"2.0","id":2,"method":"prompts/list"})" << '\n'
            << R"({"jsonrpc":"2.0","id":3,"method":"prompts/get","params":)"
               R"({"name":"monter-une-itw"}})"
            << '\n'
            << R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":)"
               R"({"name":"get_procedure","arguments":{}}})"
            << '\n'
            << R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":)"
               R"({"name":"get_procedure","arguments":{"name":)"
               R"("motion-design","file":"references/motifs.md"}}})"
            << '\n'
            << R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":)"
               R"({"name":"describe","arguments":{}}})"
            << '\n';
    }
    // Run from the scratch directory: nothing of the repository is at hand.
    const std::string command =
        "cd '" + directory.string() + "' && '" + CUTMACHINE_EXECUTABLE +
        "' --mcp-serve --stdio < '" + requests.string() + "' > '" +
        answers.string() + "' 2> '" + log.string() + "'";
    Check(std::system(command.c_str()) == 0,
          "the stdio server without a project exits 0: " + Read(log));
    std::vector<mcp_json::Value> lines;
    {
        std::istringstream input(Read(answers));
        std::string line;
        while (std::getline(input, line)) {
            mcp_json::Value parsed;
            std::string parseError;
            Check(mcp_json::Value::Parse(line, parsed, parseError),
                  "each answer is one line of JSON: " + parseError);
            lines.push_back(parsed);
        }
    }
    Check(lines.size() == 6,
          "six answers for six requests: " + std::to_string(lines.size()));
    if (lines.size() != 6) return;
    const auto result = [&](size_t index) {
        return lines[index].Find("result");
    };
    // The payload of a tool call's text block.
    const auto payload = [&](size_t index) {
        mcp_json::Value parsed;
        std::string parseError;
        const mcp_json::Value* content =
            result(index) != nullptr ? result(index)->Find("content") : nullptr;
        if (content != nullptr && content->IsArray() &&
            !content->AsArray().empty() &&
            content->AsArray()[0].Find("text") != nullptr) {
            mcp_json::Value::Parse(
                content->AsArray()[0].Find("text")->AsString(), parsed,
                parseError);
        }
        return parsed;
    };
    const mcp_json::Value* capabilities =
        result(0) != nullptr ? result(0)->Find("capabilities") : nullptr;
    Check(capabilities != nullptr && capabilities->Find("prompts") != nullptr,
          "the server declares its prompts: " + lines[0].Dump().substr(0, 300));
    const mcp_json::Value* prompts =
        result(1) != nullptr ? result(1)->Find("prompts") : nullptr;
    Check(prompts != nullptr && prompts->IsArray() &&
              prompts->AsArray().size() == ListProcedures().size() &&
              lines[1].Dump().find("\"name\":\"monter-une-itw\"") !=
                  std::string::npos,
          "prompts/list names every procedure: " +
              lines[1].Dump().substr(0, 300));
    ProcedureText skill;
    std::string errorName;
    std::string message;
    ReadProcedureFile("monter-une-itw", "SKILL.md", skill, errorName, message);
    const std::string prompt = lines[2].Dump();
    Check(result(2) != nullptr && result(2)->Find("messages") != nullptr &&
              !skill.text.empty() &&
              prompt.find(mcp_json::EscapeJsonString(skill.text)) !=
                  std::string::npos &&
              prompt.find("get_procedure") != std::string::npos,
          "prompts/get gives the whole SKILL.md: " + prompt.substr(0, 300));
    const mcp_json::Value listing = payload(3);
    Check(listing.Find("procedures") != nullptr &&
              listing.Find("procedures")->AsArray().size() ==
                  ListProcedures().size() &&
              result(3)->Find("isError") != nullptr &&
              !result(3)->Find("isError")->AsBool(),
          "get_procedure answers with no project open: " +
              lines[3].Dump().substr(0, 300));
    const mcp_json::Value motifs = payload(4);
    Check(motifs.Find("file") != nullptr &&
              motifs.Find("file")->AsString() == "references/motifs.md" &&
              motifs.Find("links") != nullptr &&
              motifs.Find("links")->Dump().find(
                  "\"file\":\"exemples/compteur.json\"") != std::string::npos,
          "a file of a subfolder comes with its links resolved: " +
              lines[4].Dump().substr(0, 300));
    const mcp_json::Value refused = payload(5);
    Check(refused.Find("error") != nullptr &&
              refused.Find("error")->AsString() == "NoProjectOpen",
          "a tool that needs a project is still refused without one: " +
              lines[5].Dump().substr(0, 300));
}

// PRD13 -- ROADMAP.md. Sends `request` byte for byte and returns the whole
// answer. The requests below are the ones a browser, or a careless client,
// writes: with header lines HttpPostJson never sends.
std::string HttpSendRaw(int port, const std::string& request) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) throw std::runtime_error("socket() failed");
#ifdef SO_NOSIGPIPE
    // A server that hung up on a body it was still being sent must fail a
    // check, not end the suite with SIGPIPE.
    const int noSigPipe = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) !=
        0) {
        ::close(fd);
        throw std::runtime_error("connect() failed");
    }
    size_t sent = 0;
    while (sent < request.size()) {
        const ssize_t wrote =
            ::send(fd, request.data() + sent, request.size() - sent, 0);
        if (wrote <= 0) break;
        sent += static_cast<size_t>(wrote);
    }
    std::string response;
    char chunk[4096];
    ssize_t got;
    while ((got = ::recv(fd, chunk, sizeof(chunk), 0)) > 0)
        response.append(chunk, static_cast<size_t>(got));
    ::close(fd);
    return response;
}

// One request as a client writes it: the request line, the header lines
// given, the length of the body, the body.
std::string RawRequest(const std::string& method, const std::string& path,
                       const std::vector<std::string>& headerLines,
                       const std::string& body) {
    std::string request = method + " " + path + " HTTP/1.1\r\n";
    for (const std::string& line : headerLines) request += line + "\r\n";
    request += "Content-Length: " + std::to_string(body.size()) +
               "\r\nConnection: close\r\n\r\n" + body;
    return request;
}

// The status of an answer, 0 when it is not one.
int HttpStatus(const std::string& httpResponse) {
    static const std::string kVersion = "HTTP/1.1 ";
    if (httpResponse.rfind(kVersion, 0) != 0) return 0;
    return std::atoi(httpResponse.c_str() + kVersion.size());
}

// The status line and header fields of an answer, in lower case: a field
// name has no case.
std::string HttpHead(const std::string& httpResponse) {
    std::string head = httpResponse.substr(0, httpResponse.find("\r\n\r\n"));
    std::transform(head.begin(), head.end(), head.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return head;
}

// What a refusal says: the "error" string of its JSON body, empty when the
// body is anything else.
std::string RefusalReason(const std::string& httpResponse) {
    mcp_json::Value body;
    std::string failure;
    if (!mcp_json::Value::Parse(HttpBody(httpResponse), body, failure) ||
        !body.IsObject())
        return std::string();
    const mcp_json::Value* reason = body.Find("error");
    return reason != nullptr && reason->IsString() ? reason->AsString()
                                                   : std::string();
}

// Every file of a package with its bytes, the session lock aside: what a
// tool call that wrote anything would have changed.
std::string PackageContent(const std::filesystem::path& package) {
    std::vector<std::filesystem::path> paths;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(package)) {
        paths.push_back(entry.path());
    }
    std::sort(paths.begin(), paths.end());
    std::string all;
    for (const auto& item : paths) {
        if (item.filename().string().find(".lock") != std::string::npos ||
            item.parent_path().filename().string().find(".lock") !=
                std::string::npos) {
            continue;
        }
        all += item.lexically_relative(package).string() + '\n';
        if (std::filesystem::is_regular_file(item)) all += Read(item) + '\n';
    }
    return all;
}

// PRD13 -- ROADMAP.md. The loopback HTTP server only answers a local client.
// Against a real server on a real project: the requests a web page can make
// a browser send are refused before any tool runs, a client that is not a
// browser passes as it did, and a server given a token asks every request
// for it. The rule itself, case by case, is in
// tests/http_request_policy_tests.cc.
void CheckLocalRequestsOverServer(const std::filesystem::path& directory) {
    Project project = Project::FromDocument(Fixture(), "Local requests");
    std::string path;
    std::string error;
    Check(
        CreatePortableProject((directory / "Local.cutmachine-project").string(),
                              project, path, error),
        "local-request fixture package saves: " + error);
    const std::filesystem::path package =
        std::filesystem::path(path).parent_path();
    const auto timelineCount = [&] {
        Project stored;
        std::string loadError;
        return LoadStoredProject(path, stored, loadError)
                   ? stored.timelines.size()
                   : size_t{0};
    };

    McpProjectBackend backend(path);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "the local-request server starts: " + startError);
    const int port = server.Port();
    const std::string localHost = "Host: 127.0.0.1:" + std::to_string(port);
    const std::string json = "Content-Type: application/json";
    const std::string evilOrigin = "Origin: https://evil.example";
    // A call that writes. Had one refused request reached the tool, the
    // project would hold one timeline more, and the session would remember
    // the key.
    const std::string addTimeline =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/call","params":)"
        R"({"name":"add_timeline","arguments":{"name":"Hostile",)"
        R"("width":1080,"height":1920,"frame_rate":{"num":25,"den":1},)"
        R"("idempotency_key":"prd13-refused"}}})";
    const std::string ping = R"({"jsonrpc":"2.0","id":2,"method":"ping"})";
    const size_t timelinesBefore = timelineCount();
    const std::string contentBefore = PackageContent(package);

    struct Refused {
        std::string what;
        std::string method;
        std::string path;
        std::vector<std::string> headers;
        int status;
        std::string says;
    };
    const std::vector<Refused> refused = {
        // The three hostile requests, one fault each.
        {"a Host that names another machine, as DNS rebinding sends it",
         "POST",
         "/mcp",
         {"Host: evil.example:" + std::to_string(port), json},
         403,
         "Host must be 127.0.0.1, localhost or [::1]"},
        {"an Origin that is a page from elsewhere",
         "POST",
         "/mcp",
         {localHost, evilOrigin, json},
         403,
         "Origin must be a loopback origin"},
        {"text/plain, the POST a page sends without asking first",
         "POST",
         "/mcp",
         {localHost, "Content-Type: text/plain"},
         415,
         "Content-Type: application/json"},
        // The request measured on 2 October 2026, answered 200 then.
        {"the request of the audit",
         "POST",
         "/mcp",
         {evilOrigin, "Host: evil.example", "Content-Type: text/plain"},
         403,
         "Host must be"},
        {"Origin: null",
         "POST",
         "/mcp",
         {localHost, "Origin: null", json},
         403,
         "Origin must be"},
        {"a Host that only begins like localhost",
         "POST",
         "/mcp",
         {"Host: localhost.evil.example:" + std::to_string(port), json},
         403,
         "Host must be"},
        // The server's own reading of the header lines: a name in capitals,
        // a blank before the colon, a line sent twice.
        {"HOST in capitals",
         "POST",
         "/mcp",
         {"HOST: evil.example", json},
         403,
         "Host must be"},
        {"a blank before the colon",
         "POST",
         "/mcp",
         {"Host : evil.example", json},
         403,
         "Host must be"},
        {"two Host lines, the loopback one last",
         "POST",
         "/mcp",
         {"Host: evil.example", localHost, json},
         403,
         "more than once"},
        {"two Host lines, the loopback one first",
         "POST",
         "/mcp",
         {localHost, "Host: evil.example", json},
         403,
         "more than once"},
        {"two Origin lines",
         "POST",
         "/mcp",
         {localHost, "Origin: http://localhost", evilOrigin, json},
         403,
         "more than once"},
        {"two Content-Type lines, JSON last",
         "POST",
         "/mcp",
         {localHost, "Content-Type: text/plain", json},
         415,
         "Content-Type: application/json"},
        {"a form post",
         "POST",
         "/mcp",
         {localHost, "Content-Type: application/x-www-form-urlencoded"},
         415,
         "Content-Type: application/json"},
        {"a POST without Content-Type",
         "POST",
         "/mcp",
         {localHost},
         415,
         "Content-Type: application/json"},
        // Whatever the path and the method: who is talking comes first.
        {"a hostile Origin on another path",
         "POST",
         "/elsewhere",
         {localHost, evilOrigin, json},
         403,
         "Origin must be"},
        {"a hostile Origin on a GET",
         "GET",
         "/mcp",
         {localHost, evilOrigin},
         403,
         "Origin must be"},
        // What a browser sends before a JSON POST to another origin.
        {"a preflight from elsewhere",
         "OPTIONS",
         "/mcp",
         {localHost, evilOrigin, "Access-Control-Request-Method: POST",
          "Access-Control-Request-Headers: content-type"},
         403,
         "Origin must be"},
    };
    for (const Refused& request : refused) {
        const std::string response = HttpSendRaw(
            port,
            RawRequest(request.method, request.path, request.headers,
                       request.method == "POST" ? addTimeline : std::string()));
        Check(HttpStatus(response) == request.status,
              request.what + " is refused with " +
                  std::to_string(request.status) + ": " +
                  response.substr(0, response.find("\r\n")));
        Check(RefusalReason(response).find(request.says) != std::string::npos,
              request.what + " is told what to correct: " + HttpBody(response));
        Check(HttpHead(response).find("access-control-") == std::string::npos &&
                  HttpHead(response).find("content-type: application/json") !=
                      std::string::npos,
              request.what +
                  " gets a JSON answer and no Access-Control-* header");
    }
    Check(timelineCount() == timelinesBefore &&
              PackageContent(package) == contentBefore,
          "no refused request reached the tool: the package is byte for byte "
          "what it was");

    // A preflight from a loopback origin passes the rule and meets the
    // handler, which only knows POST. Still no Access-Control-* header: the
    // browser never sends the request it was announcing.
    const std::string preflight = HttpSendRaw(
        port, RawRequest("OPTIONS", "/mcp",
                         {localHost, "Origin: http://localhost:5173",
                          "Access-Control-Request-Method: POST",
                          "Access-Control-Request-Headers: content-type"},
                         ""));
    Check(HttpStatus(preflight) == 405 &&
              HttpHead(preflight).find("access-control-") == std::string::npos,
          "a preflight from a loopback origin is not granted either: " +
              preflight.substr(0, preflight.find("\r\n")));
    // The codes a local client had before are the ones it has now.
    const std::string get =
        HttpSendRaw(port, RawRequest("GET", "/mcp", {localHost}, ""));
    Check(HttpStatus(get) == 405 &&
              RefusalReason(get) == "method not allowed, use POST",
          "a GET from a local client is still a 405: " + get);
    const std::string elsewhere = HttpSendRaw(
        port, RawRequest("POST", "/elsewhere", {localHost, json}, ping));
    Check(
        HttpStatus(elsewhere) == 404 && RefusalReason(elsewhere) == "not found",
        "another path from a local client is still a 404: " + elsewhere);

    // The body of a refused request is read to its end and dropped before
    // the refusal is sent: one larger than any socket buffer neither wedges
    // the exchange nor costs its sender the answer.
    const std::string large =
        HttpSendRaw(port, RawRequest("POST", "/mcp",
                                     {localHost, "Content-Type: text/plain"},
                                     std::string(4 * 1024 * 1024, 'x')));
    Check(
        HttpStatus(large) == 415 &&
            RefusalReason(large).find("application/json") != std::string::npos,
        "a refused request with a 4 MB body still reads its refusal: " +
            large.substr(0, 120));
    // A body announced beyond what the server reads is not waited for,
    // refused or not: the answer comes at once, with no body ever sent.
    const auto answeredAtOnce = [&](const std::string& contentType, int status,
                                    const std::string& what) {
        const auto before = std::chrono::steady_clock::now();
        const std::string response = HttpSendRaw(
            port, "POST /mcp HTTP/1.1\r\n" + localHost + "\r\n" + contentType +
                      "\r\nContent-Length: 67108864\r\n\r\n");
        const auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - before);
        Check(HttpStatus(response) == status && elapsed.count() < 5000,
              what + ": " + response.substr(0, response.find("\r\n")) + " in " +
                  std::to_string(elapsed.count()) + " ms");
    };
    answeredAtOnce("Content-Type: text/plain", 415,
                   "a refused request announcing 64 MB is refused at once");
    answeredAtOnce(json, 413,
                   "an accepted request announcing 64 MB is still a 413");
    Check(timelineCount() == timelinesBefore,
          "none of this created a timeline");

    // ---- a local client passes, as before ----
    const std::string accepted = HttpSendRaw(
        port, RawRequest("POST", "/mcp", {localHost, json}, addTimeline));
    Check(HttpStatus(accepted) == 200 &&
              accepted.find("\"isError\":false") != std::string::npos &&
              accepted.find("replayed") == std::string::npos &&
              timelineCount() == timelinesBefore + 1,
          "the same call from a local client runs, for the first time, and "
          "creates its timeline: " +
              accepted.substr(0, 300));
    Check(HttpHead(accepted).find("access-control-") == std::string::npos,
          "an accepted answer carries no Access-Control-* header either");
    const std::string again = HttpSendRaw(
        port, RawRequest("POST", "/mcp", {localHost, json}, addTimeline));
    Check(again.find("replayed") != std::string::npos &&
              timelineCount() == timelinesBefore + 1,
          "sent again with its key the call is replayed: the key was free "
          "until a local client used it");
    const std::vector<std::vector<std::string>> local = {
        {"Host: localhost:" + std::to_string(port),
         "Content-Type: application/json; charset=utf-8"},
        {"host: LOCALHOST:" + std::to_string(port),
         "content-type: Application/JSON"},
        {"Host: [::1]:" + std::to_string(port), json},
        {"Host: 127.0.0.1", json},
        {localHost, "Origin: http://localhost:5173", json},
        {localHost, "Origin: http://127.0.0.1:" + std::to_string(port), json},
        {localHost, "Origin: http://[::1]:8080", json},
        // No Host at all: a minimal client, which a browser never is.
        {json},
    };
    for (const std::vector<std::string>& headers : local) {
        const std::string response =
            HttpSendRaw(port, RawRequest("POST", "/mcp", headers, ping));
        Check(HttpStatus(response) == 200 &&
                  HttpBody(response).find("\"result\":{}") != std::string::npos,
              "a local client sending '" + headers.front() + "'" +
                  (headers.size() > 1 ? " and '" + headers[1] + "'"
                                      : std::string()) +
                  " is answered: " + response.substr(0, 200));
    }
    server.Stop();

    // ---- with a token ----
    // Nine characters on purpose, here and below: the secret scan of the CI
    // (gitleaks) reads a longer made-up token as a leaked one.
    const std::string token = "Tok-prd13";
    std::string upperCased = token;
    std::transform(upperCased.begin(), upperCased.end(), upperCased.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    McpProjectBackend none;
    McpServer guarded(none);
    guarded.SetBearerToken(token);
    Check(guarded.Start(0, startError),
          "a server with a token starts: " + startError);
    const std::string guardedHost =
        "Host: 127.0.0.1:" + std::to_string(guarded.Port());
    const std::string list =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})";
    const auto ask = [&](const std::string& method,
                         std::vector<std::string> headers) {
        headers.insert(headers.begin(), guardedHost);
        return HttpSendRaw(guarded.Port(),
                           RawRequest(method, "/mcp", headers,
                                      method == "POST" ? list : std::string()));
    };
    const std::vector<std::pair<std::string, std::vector<std::string>>>
        unauthorized = {
            {"no Authorization", {json}},
            {"another token", {json, "Authorization: Bearer Tok-other"}},
            {"a prefix of the token",
             {json,
              "Authorization: Bearer " + token.substr(0, token.size() - 1)}},
            {"the token and more",
             {json, "Authorization: Bearer " + token + "4"}},
            {"the token in another case",
             {json, "Authorization: Bearer " + upperCased}},
            {"the token under another scheme",
             {json, "Authorization: Basic " + token}},
            {"the token without a scheme", {json, "Authorization: " + token}},
            {"the token sent twice",
             {json, "Authorization: Bearer " + token,
              "Authorization: Bearer " + token}},
        };
    for (const auto& [what, headers] : unauthorized) {
        const std::string response = ask("POST", headers);
        Check(HttpStatus(response) == 401 &&
                  HttpHead(response).find("\r\nwww-authenticate: bearer\r\n") !=
                      std::string::npos,
              what + " is refused with 401 and a Bearer challenge: " +
                  response.substr(0, 200));
        Check(RefusalReason(response).find("token") != std::string::npos &&
                  response.find(token) == std::string::npos &&
                  HttpBody(response).find("Tok-") == std::string::npos &&
                  HttpBody(response).find("TOK-") == std::string::npos,
              what + " is told a token is needed, and is shown none: " +
                  HttpBody(response));
    }
    const std::string authorized =
        ask("POST", {json, "Authorization: Bearer " + token});
    Check(
        HttpStatus(authorized) == 200 &&
            HttpBody(authorized).find("\"tools\":[") != std::string::npos &&
            HttpHead(authorized).find("www-authenticate") == std::string::npos,
        "the token opens the server: " + authorized.substr(0, 200));
    Check(HttpStatus(ask("POST", {json, "authorization: bearer " + token})) ==
              200,
          "the field name and the scheme have no case");
    // The token does not excuse the rest of the rule, and the rest of the
    // rule comes first.
    Check(HttpStatus(ask("POST", {json, "Authorization: Bearer " + token,
                                  evilOrigin})) == 403,
          "the right token from a hostile Origin is still refused");
    Check(HttpStatus(ask("POST", {json, evilOrigin})) == 403,
          "a hostile Origin without a token is told about the Origin, not "
          "about the token");
    Check(HttpStatus(ask("POST", {"Content-Type: text/plain",
                                  "Authorization: Bearer " + token})) == 415,
          "the right token with text/plain is still refused");
    // Every request is asked, whatever its method.
    Check(HttpStatus(ask("GET", {})) == 401,
          "a GET without the token is a 401, not a 405");
    Check(HttpStatus(ask("GET", {"Authorization: Bearer " + token})) == 405,
          "a GET with the token meets the handler's 405");
    guarded.Stop();
}

// PRD13 -- ROADMAP.md. `initialize` answers the protocol version asked for
// when the server knows it, and its latest otherwise. It used to answer
// whatever it was asked, a revision published after it was written included.
void CheckProtocolVersionNegotiation() {
    McpProjectBackend none;
    McpServer server(none);
    // The version answered to `initialize` with these params.
    const auto answered = [&](const std::string& paramsJson) {
        std::string response;
        server.HandleJsonRpc(
            R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":)" +
                paramsJson + "}",
            response);
        mcp_json::Value parsed;
        std::string failure;
        if (!mcp_json::Value::Parse(response, parsed, failure))
            return "(not JSON) " + response.substr(0, 200);
        const mcp_json::Value* result = parsed.Find("result");
        const mcp_json::Value* version =
            result != nullptr ? result->Find("protocolVersion") : nullptr;
        return version != nullptr && version->IsString()
                   ? version->AsString()
                   : "(no version) " + response.substr(0, 200);
    };
    const auto asking = [&](const std::string& version) {
        return answered("{\"protocolVersion\":\"" +
                        mcp_json::EscapeJsonString(version) +
                        "\",\"capabilities\":{},\"clientInfo\":"
                        "{\"name\":\"test\",\"version\":\"0\"}}");
    };

    const std::vector<std::string> known = {"2024-11-05", "2025-03-26",
                                            "2025-06-18", "2025-11-25"};
    const std::string latest = "2025-11-25";
    Check(McpProtocolVersions() == known,
          "the server knows the four revisions built on the handshake, "
          "oldest first");
    for (const std::string& version : known) {
        Check(asking(version) == version, "a client asking for " + version +
                                              " is answered " + version +
                                              ", got " + asking(version));
        Check(NegotiateMcpProtocolVersion(version) == version,
              version + " negotiates to itself");
    }
    // A revision published after this server was written, dates that are no
    // revision, and strings that are not even dates.
    for (const std::string& version : std::vector<std::string>{
             "2026-07-28",
             "2099-01-01",
             "2025-11-26",
             "2025-06-17",
             "2024-11-04",
             "2024-10-07",
             "latest",
             "draft",
             "1.0",
             "",
             " 2025-06-18",
             "2025-06-18 ",
             "2025-06-18\n",
             "20250618",
             "2025-06-18-preview",
             "n'importe quoi \"entre guillemets\"",
         }) {
        Check(asking(version) == latest,
              "a client asking for '" + version +
                  "' is answered the latest known, " + latest + ", got " +
                  asking(version));
        Check(NegotiateMcpProtocolVersion(version) == latest,
              "'" + version + "' negotiates to the latest known");
    }
    // No version, or one that is not a string: not a version the server
    // knows either.
    for (const std::string& params : std::vector<std::string>{
             "{}",
             R"({"protocolVersion":20250618})",
             R"({"protocolVersion":null})",
             R"({"protocolVersion":["2025-06-18"]})",
             R"({"protocolVersion":{"version":"2025-06-18"}})",
         }) {
        Check(answered(params) == latest, "initialize with params " + params +
                                              " is answered " + latest +
                                              ", got " + answered(params));
    }
    std::string withoutParams;
    server.HandleJsonRpc(R"({"jsonrpc":"2.0","id":1,"method":"initialize"})",
                         withoutParams);
    Check(withoutParams.find("\"protocolVersion\":\"" + latest + "\"") !=
              std::string::npos,
          "initialize without params is answered " + latest);
    // PRD18 -- the procedures are offered as prompts beside the tools.
    Check(
        withoutParams.find("\"capabilities\":{\"tools\":{},\"prompts\":{}}") !=
                std::string::npos &&
            withoutParams.find("\"serverInfo\":{") != std::string::npos &&
            withoutParams.find("\"instructions\":\"") != std::string::npos,
        "the rest of the initialize answer is still there");

    // The revision that follows has no handshake: its clients probe a
    // server with `server/discover`, and take any error that is not one of
    // that revision's own for a server of the handshake era. This server is
    // one: the method must stay unknown, not half answered.
    std::string discover;
    server.HandleJsonRpc(
        R"({"jsonrpc":"2.0","id":7,"method":"server/discover","params":)"
        R"({"_meta":{"io.modelcontextprotocol/protocolVersion":"2026-07-28"}}})",
        discover);
    Check(discover.find("\"code\":-32601") != std::string::npos &&
              discover.find("\"id\":7") != std::string::npos,
          "server/discover is an unknown method: " + discover);
}

// PRD13 -- ROADMAP.md. Where the token of `--mcp-serve --port` comes from:
// the option, else the variable, read like every machine-local setting. One
// that was asked for and cannot serve stops the server from starting.
void CheckHttpTokenResolution(const std::filesystem::path& directory) {
    const char* previousFile = std::getenv("CUTMACHINE_ENV_FILE");
    const std::string savedFile = previousFile ? previousFile : "";
    ::setenv("CUTMACHINE_ENV_FILE", (directory / "absent-prd13.env").c_str(),
             1);
    ::unsetenv("CUTMACHINE_MCP_TOKEN");

    McpHttpToken resolved;
    std::string error;
    Check(ResolveMcpHttpToken(nullptr, resolved, error) &&
              resolved.token.empty() && resolved.source.empty(),
          "neither the option nor the variable: no token is asked");
    const std::string option = "from-the-option";
    Check(ResolveMcpHttpToken(&option, resolved, error) &&
              resolved.token == option && resolved.source == "--token",
          "--token gives the token");
    ::setenv("CUTMACHINE_MCP_TOKEN", "from-the-variable", 1);
    Check(ResolveMcpHttpToken(nullptr, resolved, error) &&
              resolved.token == "from-the-variable" &&
              resolved.source == "CUTMACHINE_MCP_TOKEN",
          "CUTMACHINE_MCP_TOKEN gives the token when the option is absent");
    Check(ResolveMcpHttpToken(&option, resolved, error) &&
              resolved.token == option && resolved.source == "--token",
          "the option wins over the variable");

    for (const std::string& unusable : std::vector<std::string>{
             "", "two words", "tab\there", "ends-with-cr\r", "caf\xc3\xa9"}) {
        resolved.token = "stale";
        error.clear();
        Check(
            !ResolveMcpHttpToken(&unusable, resolved, error) &&
                resolved.token.empty() && error.rfind("--token", 0) == 0 &&
                (unusable.empty() || error.find(unusable) == std::string::npos),
            "a --token that cannot serve is refused without being "
            "repeated: " +
                error);
        ::setenv("CUTMACHINE_MCP_TOKEN", unusable.c_str(), 1);
        resolved.token = "stale";
        error.clear();
        Check(
            !ResolveMcpHttpToken(nullptr, resolved, error) &&
                resolved.token.empty() &&
                error.rfind("CUTMACHINE_MCP_TOKEN", 0) == 0 &&
                (unusable.empty() || error.find(unusable) == std::string::npos),
            "a CUTMACHINE_MCP_TOKEN that cannot serve is refused without "
            "being repeated: " +
                error);
        // The option decides alone: an unusable variable it overrides is
        // not looked at.
        Check(ResolveMcpHttpToken(&option, resolved, error) &&
                  resolved.token == option,
              "an unusable variable does not matter under --token");
    }

    // The settings file counts, as for every CUTMACHINE_* setting, and the
    // environment wins over it.
    ::unsetenv("CUTMACHINE_MCP_TOKEN");
    const std::filesystem::path settings = directory / "prd13.env";
    {
        std::ofstream out(settings);
        out << "CUTMACHINE_MCP_TOKEN=from-the-file\n";
    }
    ::setenv("CUTMACHINE_ENV_FILE", settings.c_str(), 1);
    Check(ResolveMcpHttpToken(nullptr, resolved, error) &&
              resolved.token == "from-the-file" &&
              resolved.source == "CUTMACHINE_MCP_TOKEN",
          "the token is read from the settings file: " + error);
    ::setenv("CUTMACHINE_MCP_TOKEN", "from-the-environment", 1);
    Check(ResolveMcpHttpToken(nullptr, resolved, error) &&
              resolved.token == "from-the-environment",
          "the environment wins over the settings file");

    // The loader exports what it reads: leave nothing behind for the checks
    // that follow.
    ::unsetenv("CUTMACHINE_MCP_TOKEN");
    if (savedFile.empty())
        ::unsetenv("CUTMACHINE_ENV_FILE");
    else
        ::setenv("CUTMACHINE_ENV_FILE", savedFile.c_str(), 1);
}

// PRD13 -- the built binary serving HTTP in the background, as
// `--mcp-serve --port 0` starts it. The port the system gave it is read
// from the banner on its error output; it is stopped the way a terminal
// stops it, and waited for.
class ServedBinary {
public:
    ServedBinary(const std::filesystem::path& directory,
                 const std::string& name)
        : output_(directory / (name + "-stdout.txt")),
          log_(directory / (name + "-stderr.txt")),
          pid_(directory / (name + ".pid")) {}
    ~ServedBinary() { Stop(); }
    ServedBinary(const ServedBinary&) = delete;
    ServedBinary& operator=(const ServedBinary&) = delete;

    // `environment` is a list of sh assignments put in front of the
    // command; `arguments` follow `--mcp-serve`. False when no banner came.
    bool Start(const std::string& environment, const std::string& arguments) {
        // `exec`: the shell becomes the server, so the pid written first is
        // the server's and the signal that stops it reaches it.
        const std::string command =
            "echo $$ > '" + pid_.string() + "'; " + environment + " exec '" +
            CUTMACHINE_EXECUTABLE + "' --mcp-serve " + arguments + " > '" +
            output_.string() + "' 2> '" + log_.string() + "' < /dev/null";
        runner_ = std::thread([this, command] {
            process::Request request;
            request.arguments = {"/bin/sh", "-c", command};
            request.cancelled = [this] { return stop_.load(); };
            process::Run(request);
            finished_.store(true);
        });
        static const std::regex kBanner("http://127\\.0\\.0\\.1:([0-9]+)/mcp");
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(60);
        while (std::chrono::steady_clock::now() < deadline &&
               !finished_.load()) {
            const std::string text = Read(log_);
            std::smatch match;
            if (std::regex_search(text, match, kBanner)) {
                port_ = std::stoi(match[1].str());
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return false;
    }

    void Stop() {
        if (!runner_.joinable()) return;
        stop_.store(true);
        runner_.join();
    }

    int Port() const { return port_; }
    std::string Banner() const { return Read(log_); }
    std::string Output() const { return Read(output_); }
    // The arguments of the process as `ps` shows them to any user.
    std::string ListedArguments() const {
        std::string pid = Read(pid_);
        while (!pid.empty() && (pid.back() == '\n' || pid.back() == ' '))
            pid.pop_back();
        const std::filesystem::path listing = pid_.string() + ".ps";
        const std::string command =
            "ps -ww -o args= -p " + pid + " > '" + listing.string() + "' 2>&1";
        if (pid.empty() || std::system(command.c_str()) != 0)
            return std::string();
        return Read(listing);
    }

private:
    std::filesystem::path output_;
    std::filesystem::path log_;
    std::filesystem::path pid_;
    std::atomic_bool stop_{false};
    std::atomic_bool finished_{false};
    std::thread runner_;
    int port_ = 0;
};

// PRD13 -- ROADMAP.md. The options of `--mcp-serve` over HTTP, through the
// built binary: what main() refuses before it serves, where the token comes
// from, and that nothing it prints -- a refusal, the banner, its own
// argument list -- ever shows the token.
void CheckHttpServeThroughBinary(const std::filesystem::path& directory) {
    // Neither the developer's settings file nor a variable of their shell
    // may give these servers a token.
    ::setenv("CUTMACHINE_ENV_FILE", (directory / "absent-prd13.env").c_str(),
             1);
    ::unsetenv("CUTMACHINE_MCP_TOKEN");
    const std::string optionToken = "Opt-prd13";
    const std::string variableToken = "Var-prd13";
    const std::filesystem::path output = directory / "serve-refused-stdout.txt";
    const std::filesystem::path log = directory / "serve-refused-stderr.txt";
    const auto refusedAtStart =
        [&](const std::string& environment, const std::string& arguments,
            const std::string& says, const std::string& what) {
            const std::string command =
                environment + " '" + CUTMACHINE_EXECUTABLE + "' --mcp-serve " +
                arguments + " > '" + output.string() + "' 2> '" + log.string() +
                "' < /dev/null";
            const int status = std::system(command.c_str());
            const std::string answer = Read(output);
            Check(status != 0 &&
                      answer.find("\"error\":\"ValidationFailed\"") !=
                          std::string::npos &&
                      answer.find(says) != std::string::npos,
                  what + ": " + answer);
            const std::string printed = answer + Read(log);
            Check(printed.find(optionToken) == std::string::npos &&
                      printed.find(variableToken) == std::string::npos,
                  what + ", and nothing printed holds a token: " + printed);
        };
    refusedAtStart("", "--stdio --token " + optionToken, "it takes no --token",
                   "--stdio with --token is refused");
    refusedAtStart("", "--port 0 --token", "--token needs a value",
                   "--token without a value is refused");
    refusedAtStart("", "--token --port 0", "--token needs a value",
                   "--token followed by another option is refused");
    refusedAtStart("", "--port 0 --token=" + optionToken,
                   "--token takes its value as the next argument",
                   "--token=<t> is refused");
    refusedAtStart("", "--port 0 --token ''", "--token is empty",
                   "an empty --token is refused");
    refusedAtStart(
        "", "--port 0 --token '" + optionToken + " " + variableToken + "'",
        "--token holds a blank", "a --token with a blank is refused");
    refusedAtStart("CUTMACHINE_MCP_TOKEN=''", "--port 0",
                   "CUTMACHINE_MCP_TOKEN is empty",
                   "an empty CUTMACHINE_MCP_TOKEN is refused");
    refusedAtStart("", "--port 0 ./absent-project " + optionToken,
                   "takes one project path",
                   "a token typed without --token is refused");

    const std::string json = "Content-Type: application/json";
    const std::string list =
        R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})";
    const auto ask = [&](const ServedBinary& served,
                         std::vector<std::string> headers) {
        headers.insert(headers.begin(),
                       "Host: 127.0.0.1:" + std::to_string(served.Port()));
        return HttpSendRaw(served.Port(),
                           RawRequest("POST", "/mcp", headers, list));
    };
    // A failed connection throws: caught here, so that the server started
    // for the scenario is stopped on the way out instead of outliving the
    // suite.
    try {
        // --token, with the variable set to something else: the option wins.
        ServedBinary served(directory, "serve-option");
        Check(served.Start("CUTMACHINE_MCP_TOKEN='" + variableToken + "'",
                           "--port 0 --token '" + optionToken + "'"),
              "the binary serves HTTP with --token: " + served.Banner());
        if (served.Port() != 0) {
            const std::string none = ask(served, {json});
            Check(HttpStatus(none) == 401 &&
                      HttpHead(none).find("\r\nwww-authenticate: bearer\r\n") !=
                          std::string::npos,
                  "the binary given --token refuses a request without it: " +
                      none.substr(0, 200));
            Check(HttpStatus(ask(served, {json, "Authorization: Bearer " +
                                                    variableToken})) == 401,
                  "the option wins: the variable's token is refused");
            const std::string granted =
                ask(served, {json, "Authorization: Bearer " + optionToken});
            Check(
                HttpStatus(granted) == 200 &&
                    HttpBody(granted).find("\"tools\":[") != std::string::npos,
                "the option's token opens the server: " +
                    granted.substr(0, 200));
            const std::string listed = served.ListedArguments();
            Check(listed.find("--mcp-serve") == std::string::npos ||
                      listed.find(optionToken) == std::string::npos,
                  "the token is no longer in the process's argument list: " +
                      listed);
        }
        const std::string printed = served.Banner() + served.Output();
        Check(served.Banner().find("bearer token required (--token)") !=
                      std::string::npos &&
                  served.Output().empty() &&
                  printed.find(optionToken) == std::string::npos &&
                  printed.find(variableToken) == std::string::npos,
              "the banner says a token is asked and shows none: " + printed);
    } catch (const std::exception& failure) {
        Check(false,
              std::string("serving with --token threw: ") + failure.what());
    }
    try {
        ServedBinary served(directory, "serve-variable");
        Check(served.Start("CUTMACHINE_MCP_TOKEN='" + variableToken + "'",
                           "--port 0"),
              "the binary serves HTTP with CUTMACHINE_MCP_TOKEN: " +
                  served.Banner());
        if (served.Port() != 0) {
            Check(HttpStatus(ask(served, {json})) == 401,
                  "the binary given the variable refuses a request without "
                  "the token");
            Check(HttpStatus(ask(served, {json, "Authorization: Bearer " +
                                                    variableToken})) == 200,
                  "the variable's token opens the server");
        }
        Check(served.Banner().find(
                  "bearer token required (CUTMACHINE_MCP_TOKEN)") !=
                      std::string::npos &&
                  served.Banner().find(variableToken) == std::string::npos,
              "the banner names the variable and not its value: " +
                  served.Banner());
    } catch (const std::exception& failure) {
        Check(false, std::string("serving with the variable threw: ") +
                         failure.what());
    }
    try {
        // No token: the bare URL existing configurations point at.
        ServedBinary served(directory, "serve-open");
        Check(served.Start("", "--port 0"),
              "the binary serves HTTP without a token: " + served.Banner());
        if (served.Port() != 0) {
            const std::string open = ask(served, {json});
            Check(HttpStatus(open) == 200 &&
                      HttpBody(open).find("\"tools\":[") != std::string::npos,
                  "without a token a local client needs none: " +
                      open.substr(0, 200));
            const std::string hostile = HttpSendRaw(
                served.Port(),
                RawRequest("POST", "/mcp",
                           {"Origin: https://evil.example",
                            "Host: evil.example", "Content-Type: text/plain"},
                           list));
            Check(HttpStatus(hostile) == 403,
                  "the binary refuses the request of the audit: " +
                      hostile.substr(0, 200));
        }
        Check(
            served.Banner().find("token") == std::string::npos,
            "without a token the banner is the one it was: " + served.Banner());
    } catch (const std::exception& failure) {
        Check(false,
              std::string("serving without a token threw: ") + failure.what());
    }

    // Over stdio the variable is not read at all: exported for an HTTP
    // server, or left empty, it must not break the server a client spawns.
    const std::filesystem::path requests = directory / "prd13-stdio.jsonl";
    const std::filesystem::path answers = directory / "prd13-stdio-answers.txt";
    {
        std::ofstream out(requests);
        out << R"({"jsonrpc":"2.0","id":1,"method":"ping"})" << '\n';
    }
    for (const std::string& value : {variableToken, std::string()}) {
        const std::string command =
            "CUTMACHINE_MCP_TOKEN='" + value + "' '" + CUTMACHINE_EXECUTABLE +
            "' --mcp-serve --stdio < '" + requests.string() + "' > '" +
            answers.string() + "' 2> '" + log.string() + "'";
        Check(std::system(command.c_str()) == 0 &&
                  Read(answers).find("\"result\":{}") != std::string::npos,
              "--stdio serves with CUTMACHINE_MCP_TOKEN='" + value +
                  "' in its environment: " + Read(answers) + Read(log));
    }
}

// PRD13b -- ROADMAP.md. What the revisions the server grants ask of it,
// against real servers: a batch over HTTP and over stdio, the
// MCP-Protocol-Version header, a response posted by a client, the framing of
// a body, the method a 405 allows, and the port `--mcp-serve` reads. The
// rule on header fields alone is case by case in
// tests/http_request_policy_tests.cc.
void CheckProtocolPromisesOverServer(const std::filesystem::path& directory) {
    Project project = Project::FromDocument(Fixture(), "Protocol");
    std::string path;
    std::string error;
    Check(CreatePortableProject(
              (directory / "Protocol.cutmachine-project").string(), project,
              path, error),
          "protocol fixture package saves: " + error);
    McpProjectBackend backend(path);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "the protocol server starts: " + startError);
    const int port = server.Port();
    const std::string host = "Host: 127.0.0.1:" + std::to_string(port);
    const std::string json = "Content-Type: application/json";
    const auto post = [&](const std::string& body,
                          std::vector<std::string> headers) {
        headers.insert(headers.begin(), {host, json});
        return HttpSendRaw(port, RawRequest("POST", "/mcp", headers, body));
    };
    const auto parsed = [](const std::string& response) {
        mcp_json::Value value;
        std::string failure;
        mcp_json::Value::Parse(HttpBody(response), value, failure);
        return value;
    };
    const auto field = [](const mcp_json::Value& value, const char* key) {
        const mcp_json::Value* found = value.Find(key);
        return found != nullptr ? found->Dump() : std::string("(absent)");
    };
    const auto code = [&](const mcp_json::Value& value) {
        const mcp_json::Value* failure = value.Find("error");
        return failure != nullptr ? field(*failure, "code")
                                  : std::string("(none)");
    };
    try {
        // A batch: one answer per request, in order, none for a
        // notification.
        const std::string batched =
            post(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
                 R"({"jsonrpc":"2.0","method":"notifications/initialized"},)"
                 R"({"jsonrpc":"2.0","id":2,"method":"tools/list"}])",
                 {});
        const mcp_json::Value answers = parsed(batched);
        Check(HttpStatus(batched) == 200 && answers.IsArray() &&
                  answers.AsArray().size() == 2 &&
                  field(answers.AsArray()[0], "id") == "1" &&
                  answers.AsArray()[0].Find("result") != nullptr &&
                  field(answers.AsArray()[1], "id") == "2" &&
                  field(answers.AsArray()[1], "result").find("\"tools\":[") !=
                      std::string::npos,
              "a batch over HTTP is answered by an array, one answer per "
              "request: " +
                  batched.substr(0, 300));
        const std::string quiet =
            post(R"([{"jsonrpc":"2.0","method":"notifications/initialized"},)"
                 R"({"jsonrpc":"2.0","method":"notifications/cancelled",)"
                 R"("params":{}}])",
                 {});
        Check(HttpStatus(quiet) == 202 && HttpBody(quiet).empty(),
              "a batch of notifications is acknowledged, 202 without a "
              "body: " +
                  quiet.substr(0, 200));
        const mcp_json::Value empty = parsed(post("[]", {}));
        Check(empty.IsObject() && code(empty) == "-32600" &&
                  field(empty, "id") == "null",
              "an empty batch is one Invalid Request: " + empty.Dump());
        const mcp_json::Value invalid = parsed(
            post(R"([1,{"jsonrpc":"2.0","id":3,"method":"initialize","params":)"
                 R"({"protocolVersion":"2025-03-26"}}])",
                 {}));
        Check(invalid.IsArray() && invalid.AsArray().size() == 2 &&
                  field(invalid.AsArray()[0], "id") == "null" &&
                  code(invalid.AsArray()[0]) == "-32600" &&
                  field(invalid.AsArray()[1], "id") == "3" &&
                  code(invalid.AsArray()[1]) == "-32600" &&
                  invalid.AsArray()[1].Dump().find("initialize must not be "
                                                   "part of a batch") !=
                      std::string::npos,
              "a batch's invalid member is answered as invalid, and "
              "initialize is refused in a batch: " +
                  invalid.Dump());

        // MCP-Protocol-Version: one of the four, or none.
        const std::string ping = R"({"jsonrpc":"2.0","id":4,"method":"ping"})";
        for (const std::string& version : McpProtocolVersions()) {
            Check(HttpStatus(
                      post(ping, {"MCP-Protocol-Version: " + version})) == 200,
                  "MCP-Protocol-Version " + version + " is served");
        }
        Check(HttpStatus(post(ping, {})) == 200,
              "a request without MCP-Protocol-Version is served as before");
        const std::string unknownVersion =
            post(ping, {"MCP-Protocol-Version: 2026-07-28"});
        Check(HttpStatus(unknownVersion) == 400 &&
                  RefusalReason(unknownVersion).find("2024-11-05") !=
                      std::string::npos &&
                  RefusalReason(unknownVersion).find("2025-11-25") !=
                      std::string::npos,
              "a revision the server does not implement is refused, 400, "
              "naming those it does: " +
                  unknownVersion.substr(0, 400));

        // A JSON-RPC response from the client: nothing it could answer.
        const std::string response =
            post(R"({"jsonrpc":"2.0","id":5,"result":{}})", {});
        const mcp_json::Value responseBody = parsed(response);
        Check(HttpStatus(response) == 400 && code(responseBody) == "-32600" &&
                  field(responseBody, "id") == "null",
              "a response posted by a client is refused, 400, with a "
              "JSON-RPC error without an id: " +
                  response.substr(0, 300));
        Check(HttpStatus(post(R"([{"jsonrpc":"2.0","id":6,"error":)"
                              R"({"code":-1,"message":"x"}}])",
                              {})) == 400,
              "a batch of responses is refused too");
        const mcp_json::Value mixed =
            parsed(post(R"([{"jsonrpc":"2.0","id":6,"result":{}},)"
                        R"({"jsonrpc":"2.0","id":7,"method":"ping"}])",
                        {}));
        Check(mixed.IsArray() && mixed.AsArray().size() == 1 &&
                  field(mixed.AsArray()[0], "id") == "7",
              "a request beside a response is answered, the response passed "
              "over: " +
                  mixed.Dump());

        // The framing of the body.
        const std::string head =
            "POST /mcp HTTP/1.1\r\n" + host + "\r\n" + json + "\r\n";
        const std::string length = std::to_string(ping.size());
        const std::string twoLengths = HttpSendRaw(
            port, head + "Content-Length: " + length +
                      "\r\nContent-Length: " + std::to_string(ping.size() + 1) +
                      "\r\nConnection: close\r\n\r\n" + ping);
        Check(HttpStatus(twoLengths) == 400 &&
                  RefusalReason(twoLengths)
                          .find("Content-Length must be one "
                                "number") == 0,
              "two Content-Length that differ are refused, 400: " +
                  twoLengths.substr(0, 300));
        const std::string sameTwice =
            HttpSendRaw(port, head + "Content-Length: " + length +
                                  "\r\nContent-Length: " + length +
                                  "\r\nConnection: close\r\n\r\n" + ping);
        Check(HttpStatus(sameTwice) == 200,
              "the same Content-Length twice is that length: " +
                  sameTwice.substr(0, 200));
        std::ostringstream chunkSize;
        chunkSize << std::hex << ping.size();
        const std::string chunked = HttpSendRaw(
            port,
            head + "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n" +
                chunkSize.str() + "\r\n" + ping + "\r\n0\r\n\r\n");
        Check(HttpStatus(chunked) == 411 &&
                  HttpHead(chunked).find("411 length required") !=
                      std::string::npos &&
                  RefusalReason(chunked).find("Content-Length") !=
                      std::string::npos,
              "a chunked body is answered 411, Content-Length asked for: " +
                  chunked.substr(0, 300));

        // A GET, which asks for an event stream: none, and POST is allowed.
        const std::string get = HttpSendRaw(
            port, "GET /mcp HTTP/1.1\r\n" + host +
                      "\r\nAccept: text/event-stream\r\nConnection: "
                      "close\r\n\r\n");
        Check(
            HttpStatus(get) == 405 &&
                HttpHead(get).find("\r\nallow: post\r\n") != std::string::npos,
            "a 405 says which method is allowed: " + get.substr(0, 300));
    } catch (const std::exception& failure) {
        Check(false,
              std::string("the protocol checks threw: ") + failure.what());
    }
    server.Stop();

    // stdio, through the binary a client spawns: a batch is one line and is
    // answered on one line, a client's response is passed over, and the
    // next request is answered as ever.
    const std::filesystem::path requests = directory / "prd13b-stdio.jsonl";
    const std::filesystem::path answers = directory / "prd13b-answers.jsonl";
    const std::filesystem::path log = directory / "prd13b-stderr.txt";
    {
        std::ofstream out(requests);
        out << R"([{"jsonrpc":"2.0","id":1,"method":"ping"},)"
               R"({"jsonrpc":"2.0","id":2,"method":"prompts/list"}])"
            << '\n'
            << R"({"jsonrpc":"2.0","id":3,"result":{}})" << '\n'
            << R"({"jsonrpc":"2.0","id":4,"method":"ping"})" << '\n';
    }
    const std::string command = std::string("'") + CUTMACHINE_EXECUTABLE +
                                "' --mcp-serve --stdio < '" +
                                requests.string() + "' > '" + answers.string() +
                                "' 2> '" + log.string() + "'";
    Check(std::system(command.c_str()) == 0,
          "the stdio server serves a batch and exits 0: " + Read(log));
    std::vector<mcp_json::Value> lines;
    {
        std::istringstream input(Read(answers));
        std::string line;
        while (std::getline(input, line)) {
            mcp_json::Value value;
            std::string failure;
            mcp_json::Value::Parse(line, value, failure);
            lines.push_back(value);
        }
    }
    Check(lines.size() == 2 && lines[0].IsArray() &&
              lines[0].AsArray().size() == 2 &&
              field(lines[0].AsArray()[0], "id") == "1" &&
              field(lines[0].AsArray()[1], "id") == "2" &&
              field(lines[1], "id") == "4",
          "over stdio a batch is answered on one line, a client's response "
          "gets none: " +
              Read(answers).substr(0, 300));

    // The port: a number from 1 to 65535, or 0 for any free one; refused by
    // name otherwise, before anything is served.
    const std::filesystem::path output = directory / "prd13b-port.txt";
    const auto refusedPort = [&](const std::string& arguments,
                                 const std::string& says) {
        const std::string run = std::string("'") + CUTMACHINE_EXECUTABLE +
                                "' --mcp-serve " + arguments + " > '" +
                                output.string() + "' 2> '" + log.string() +
                                "' < /dev/null";
        const int status = std::system(run.c_str());
        const std::string answer = Read(output);
        Check(status != 0 &&
                  answer.find("\"error\":\"ValidationFailed\"") !=
                      std::string::npos &&
                  answer.find(says) != std::string::npos &&
                  answer.find("usage: cutmachine --mcp-serve") !=
                      std::string::npos,
              "--mcp-serve " + arguments + " is refused by name: " + answer);
    };
    const std::string range = "takes a number from 1 to 65535";
    refusedPort("--port abc", range +
                                  ", or 0 for a free port the system "
                                  "picks, not 'abc'");
    refusedPort("--port 70000", range);
    refusedPort("--port -1", range);
    refusedPort("--port 80x", range);
    refusedPort("--port", "--mcp-serve option '--port' needs a value");
    refusedPort("--port --stdio", "--mcp-serve option '--port' needs a value");
    refusedPort("--stdio --port 0", "it takes no --port");
}

int main() {
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-mcp-tests");
    std::filesystem::create_directory(directory);
    std::string error;
    // MB1 -- the motion library is a local setting (MotionTemplates.h): no
    // test here may list, or write to, the developer's own.
    ::setenv("CUTMACHINE_MOTION_LIBRARY",
             (directory / "motion-library").c_str(), 1);

    // Two independent copies of the same fixture: one the MCP server edits
    // over HTTP, one a reference edited directly through ApplyOperationCommand
    // (the exact function `--apply-op` calls).
    Project mcpProject = Project::FromDocument(Fixture(), "MCP fixture");
    const Ulid alternateTimelineId = "01K30000000000000000000006";
    DocumentSequence alternateTimeline = mcpProject.timelines.front();
    alternateTimeline.id = alternateTimelineId;
    alternateTimeline.name = "Alternate";
    alternateTimeline.tracks[0].id = "01K30000000000000000000007";
    alternateTimeline.tracks[0].clips[0].id = "01K30000000000000000000008";
    alternateTimeline.tracks[0].clips[1].id = "01K30000000000000000000009";
    mcpProject.timelines.push_back(alternateTimeline);
    std::string mcpPath;
    Check(CreatePortableProject((directory / "Mcp.cutmachine-project").string(),
                                mcpProject, mcpPath, error),
          "MCP fixture package saves: " + error);

    Project referenceProject =
        Project::FromDocument(Fixture(), "Reference fixture");
    DocumentSequence referenceAlternate = referenceProject.timelines.front();
    referenceAlternate.id = alternateTimelineId;
    referenceAlternate.name = "Alternate";
    referenceAlternate.tracks[0].id = "01K30000000000000000000007";
    referenceAlternate.tracks[0].clips[0].id = "01K30000000000000000000008";
    referenceAlternate.tracks[0].clips[1].id = "01K30000000000000000000009";
    referenceProject.timelines.push_back(referenceAlternate);
    std::string referencePath;
    Check(CreatePortableProject(
              (directory / "Reference.cutmachine-project").string(),
              referenceProject, referencePath, error),
          "reference fixture package saves: " + error);

    McpProjectBackend backend(mcpPath);
    McpServer server(backend);
    std::string startError;
    Check(server.Start(0, startError),
          "MCP server starts on an ephemeral port: " + startError);

    // ---- tools/list exposes the full catalog ----
    const std::string listResponse =
        HttpPostJson(server.Port(), "/mcp",
                     R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    mcp_json::Value listBody;
    std::string parseError;
    Check(mcp_json::Value::Parse(HttpBody(listResponse), listBody, parseError),
          "tools/list body is valid JSON: " + parseError);
    const mcp_json::Value* toolsField =
        listBody.Find("result") ? listBody.Find("result")->Find("tools")
                                : nullptr;
    Check(toolsField && toolsField->IsArray() &&
              toolsField->AsArray().size() > 30,
          "tools/list exposes the full catalog (>30 tools) over the real "
          "project-file backend");

    // ---- tools/call trim_clip vs. a direct --apply-op call ----
    const std::string trimRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":-1,"rate":25}}}})";
    const std::string trimResponse =
        HttpPostJson(server.Port(), "/mcp", trimRequest);
    Check(trimResponse.find("\"isError\":false") != std::string::npos,
          "tools/call trim_clip is not an error");

    const Operation referenceTrim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    std::string applyOpOutput;
    Check(
        ApplyOperationCommand(referencePath, SerializeOperation(referenceTrim),
                              applyOpOutput) == 0,
        "reference --apply-op trim succeeds: " + applyOpOutput);

    Project mcpAfterTrim;
    Project referenceAfterTrim;
    Check(Project::Load(mcpPath, mcpAfterTrim, error),
          "MCP-edited project reloads after trim: " + error);
    Check(Project::Load(referencePath, referenceAfterTrim, error),
          "reference project reloads after trim: " + error);
    Check(mcpAfterTrim.MakeActiveDocument().SaveToString() ==
              referenceAfterTrim.MakeActiveDocument().SaveToString(),
          "tools/call trim_clip changes the on-disk document exactly as a "
          "direct --apply-op call would");

    // ---- tools/call insert_clip vs. a direct --apply-op call ----
    // insert_clip's clip_id is engine-generated; discover the ID the MCP
    // call actually produced and replay it explicitly against the
    // reference project for a true byte-for-byte comparison (see
    // tests/mcp_tools_tests.cc for the same reasoning in more detail).
    std::vector<Ulid> clipIdsBeforeInsert;
    for (const DocumentTrack& track :
         mcpAfterTrim.MakeActiveDocument().sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            clipIdsBeforeInsert.push_back(clip.id);

    const std::string insertRequest =
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call",)"
        R"("params":{"name":"insert_clip","arguments":{)"
        R"("track_id":"01K30000000000000000000002",)"
        R"("source_id":"01K30000000000000000000001",)"
        R"("source_in":{"value":0,"rate":25},)"
        R"("duration":{"value":5,"rate":25},)"
        R"("timeline_in":{"value":0,"rate":25}}}})";
    const std::string insertResponse =
        HttpPostJson(server.Port(), "/mcp", insertRequest);
    Check(insertResponse.find("\"isError\":false") != std::string::npos,
          "tools/call insert_clip is not an error");

    Project mcpAfterInsert;
    Check(Project::Load(mcpPath, mcpAfterInsert, error),
          "MCP-edited project reloads after insert: " + error);
    Ulid insertedClipId;
    for (const DocumentTrack& track :
         mcpAfterInsert.MakeActiveDocument().sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (std::find(clipIdsBeforeInsert.begin(),
                          clipIdsBeforeInsert.end(),
                          clip.id) == clipIdsBeforeInsert.end())
                insertedClipId = clip.id;
        }
    }
    Check(!insertedClipId.empty(),
          "insert_clip created exactly one new, previously-unseen clip_id");

    const Operation referenceInsert =
        InsertClipOperation{"01K30000000000000000000002",
                            "01K30000000000000000000001",
                            {0, 25},
                            {5, 25},
                            {0, 25},
                            insertedClipId,
                            {}};
    Check(ApplyOperationCommand(referencePath,
                                SerializeOperation(referenceInsert),
                                applyOpOutput) == 0,
          "reference --apply-op insert succeeds: " + applyOpOutput);
    Project referenceAfterInsert;
    Check(Project::Load(referencePath, referenceAfterInsert, error),
          "reference project reloads after insert: " + error);
    Check(mcpAfterInsert.MakeActiveDocument().SaveToString() ==
              referenceAfterInsert.MakeActiveDocument().SaveToString(),
          "tools/call insert_clip changes the on-disk document exactly as a "
          "direct --apply-op call would");

    // ---- undo tool vs. a direct undo through the same timeline EditLog ----
    const std::string undoRequest =
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"undo","arguments":{}}})";
    const std::string undoResponse =
        HttpPostJson(server.Port(), "/mcp", undoRequest);
    Check(undoResponse.find("\"isError\":false") != std::string::npos,
          "tools/call undo is not an error");
    Check(UndoOperationCommand(referencePath, applyOpOutput) == 0,
          "reference undo-op succeeds: " + applyOpOutput);
    Project mcpAfterUndo;
    Project referenceAfterUndo;
    Check(Project::Load(mcpPath, mcpAfterUndo, error),
          "MCP-edited project reloads after undo: " + error);
    Check(Project::Load(referencePath, referenceAfterUndo, error),
          "reference project reloads after undo: " + error);
    Check(mcpAfterUndo.MakeActiveDocument().SaveToString() ==
              referenceAfterUndo.MakeActiveDocument().SaveToString(),
          "the undo tool matches a direct timeline undo");
    Check(mcpAfterUndo.MakeActiveDocument().SaveToString() ==
              mcpAfterTrim.MakeActiveDocument().SaveToString(),
          "undoing the insert restores the exact post-trim document");

    // ---- B9: an explicit non-active timeline is the sole edit target ----
    const std::string activeBeforeExplicit =
        mcpAfterUndo.MakeActiveDocument().SaveToString();
    const Document alternateBeforeExplicit =
        mcpAfterUndo.MakeDocument(alternateTimelineId);
    const std::string explicitTrimRequest =
        R"({"jsonrpc":"2.0","id":7,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"timeline_id":"01K30000000000000000000006",)"
        R"("clip_id":"01K30000000000000000000008","edge":"Tail",)"
        R"("delta":{"value":-1,"rate":25}}}})";
    const std::string explicitTrimResponse =
        HttpPostJson(server.Port(), "/mcp", explicitTrimRequest);
    Check(explicitTrimResponse.find("\"isError\":false") != std::string::npos,
          "an MCP edit accepts an explicit non-active timeline_id");
    Project afterExplicitTrim;
    Check(Project::Load(mcpPath, afterExplicitTrim, error),
          "project reloads after an explicit-timeline edit: " + error);
    Check(afterExplicitTrim.active_timeline_id ==
                  mcpAfterUndo.active_timeline_id &&
              afterExplicitTrim.MakeActiveDocument().SaveToString() ==
                  activeBeforeExplicit,
          "an explicit MCP edit preserves the active timeline byte-for-byte");
    const Document alternateAfterExplicit =
        afterExplicitTrim.MakeDocument(alternateTimelineId);
    const DocumentClip* alternateBeforeClip =
        alternateBeforeExplicit.FindClip("01K30000000000000000000008");
    const DocumentClip* alternateAfterClip =
        alternateAfterExplicit.FindClip("01K30000000000000000000008");
    Check(alternateBeforeClip && alternateAfterClip &&
              alternateAfterClip->duration == RationalTime{9, 25} &&
              alternateBeforeClip->duration == RationalTime{10, 25},
          "the explicit non-active timeline receives the edit");

    // ---- error path leaves the project file untouched ----
    const std::string beforeError = Read(mcpPath);
    const std::string badRequest =
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K39999999999999999999999","edge":"Tail",)"
        R"("delta":{"value":1,"rate":25}}}})";
    const std::string badResponse =
        HttpPostJson(server.Port(), "/mcp", badRequest);
    Check(badResponse.find("\"isError\":true") != std::string::npos,
          "an unknown clip_id is reported as isError, not a JSON-RPC "
          "protocol error");
    // An ID that resolves to nothing never reaches the engine: IdResolver
    // rejects it first, so the reported error is ValidationFailed naming the
    // offending argument, not the engine's UnknownClip. That is the more
    // precise of the two -- it tells the agent *which* argument was wrong --
    // so the resolver deliberately owns this case.
    Check(badResponse.find("ValidationFailed") != std::string::npos &&
              badResponse.find("trim_clip.clip_id") != std::string::npos,
          "an unresolvable id is refused by the resolver, naming the "
          "offending argument");
    Check(Read(mcpPath) == beforeError,
          "a refused tool call leaves the project file byte-identical");

    // ...but an operation the resolver accepts and the *engine* refuses
    // still reports the engine's own EditError name verbatim, which is what
    // keeps MCP error reporting indistinguishable from `--apply-op`. The
    // clip_id here is real, so resolution succeeds and the refusal comes
    // from ApplyOperation: trimming far more off the tail than the clip has
    // left would drive its duration to zero or below.
    const std::string badTrimRequest =
        R"({"jsonrpc":"2.0","id":6,"method":"tools/call",)"
        R"("params":{"name":"trim_clip","arguments":)"
        R"({"clip_id":"01K30000000000000000000003","edge":"Tail",)"
        R"("delta":{"value":-500,"rate":25}}}})";
    const std::string badTrimResponse =
        HttpPostJson(server.Port(), "/mcp", badTrimRequest);
    Check(badTrimResponse.find("InvalidDuration") != std::string::npos,
          "an engine-level refusal names the exact EditError, matching "
          "--apply-op");
    Check(Read(mcpPath) == beforeError,
          "an engine-refused tool call also leaves the project file "
          "byte-identical");

    // ---- the session cache never outlives an edit made through it ----
    // PERF-2026-09. McpProjectBackend keeps the project it loaded for the
    // life of the session, because one tool call used to reload the whole
    // package three or four times over. What makes that sound is that every
    // command drops the cache -- not the size/mtime stamp kept beside it,
    // which is only a second opinion for a package edited outside
    // CUTMACHINE. So this drives an edit the stamp cannot see: one that
    // leaves the project file exactly as long, with its modification time
    // put back. That is not a contrivance -- it is what an exFAT volume,
    // whose timestamps advance in two-second steps, produces on its own, and
    // the LaCie drives this project is edited from are exFAT.
    Project cacheProject = Project::FromDocument(Fixture(), "Cache fixture");
    std::string cachePath;
    Check(
        CreatePortableProject((directory / "Cache.cutmachine-project").string(),
                              cacheProject, cachePath, error),
        "cache fixture package saves: " + error);

    McpProjectBackend cacheBackend(cachePath);
    Document cachedBefore;
    std::string cacheMessage;
    Check(cacheBackend.SnapshotDocument(cachedBefore, cacheMessage),
          "the first snapshot loads and caches the project: " + cacheMessage);

    const std::filesystem::path cacheFile(cachePath);
    const auto stampedTime = std::filesystem::last_write_time(cacheFile);
    const auto stampedSize = std::filesystem::file_size(cacheFile);

    // timeline_in 5 -> 9: one digit for one digit, so the serialized project
    // keeps its exact length. The clip occupies [9,19), still clear of the
    // second clip at [20,30).
    std::string moveResult;
    std::string moveErrorName;
    Check(cacheBackend.ApplyOperation(
              MoveClipOperation{"01K30000000000000000000003",
                                "01K30000000000000000000002",
                                {9, 25},
                                {}},
              moveResult, moveErrorName, cacheMessage),
          "the edit through the cached backend applies: " + moveErrorName +
              " " + cacheMessage);
    Check(std::filesystem::file_size(cacheFile) == stampedSize,
          "the fixture edit leaves the project file exactly as long, so the "
          "size half of the stamp cannot detect it");
    std::filesystem::last_write_time(cacheFile, stampedTime);

    Document cachedAfter;
    Check(cacheBackend.SnapshotDocument(cachedAfter, cacheMessage),
          "the second snapshot succeeds: " + cacheMessage);
    const DocumentClip* movedClip =
        cachedAfter.FindClip("01K30000000000000000000003");
    Check(movedClip != nullptr && movedClip->timeline_in == RationalTime{9, 25},
          "a snapshot taken after an edit through the same backend sees the "
          "edit, with neither the file's size nor its timestamp to reveal it");

    // ---- IA7: a cold run, from a folder of rushes, without leaving MCP ----
    // The server starts with no project, creates one and ingests a rush,
    // edits and exports it. Throughout, it holds exactly one project lock,
    // and an ingest from outside is refused while it does.
    {
        const std::filesystem::path rushes = directory / "rushes";
        std::filesystem::create_directories(rushes);
        const std::filesystem::path rush = rushes / "C0001.mp4";
        const std::string generate =
            std::string("'") + FFMPEG_EXECUTABLE +
            "' -hide_banner -loglevel error "
            "-f lavfi -i 'color=c=gray:s=64x36:r=25:d=1' "
            "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
            "-c:v mpeg4 -c:a aac -shortest '" +
            rush.string() + "'";
        Check(std::system(generate.c_str()) == 0,
              "FFmpeg generates the cold-run rush");

        McpProjectBackend cold;
        McpServer coldServer(cold);
        std::string coldStartError;
        Check(coldServer.Start(0, coldStartError),
              "a server starts with no project: " + coldStartError);
        int requestId = 100;
        // Calls one tool; returns its payload and whether it was an error.
        const auto callTool = [&](const std::string& name,
                                  const std::string& arguments, bool& isError) {
            const std::string body =
                "{\"jsonrpc\":\"2.0\",\"id\":" + std::to_string(++requestId) +
                ",\"method\":\"tools/call\",\"params\":{\"name\":\"" + name +
                "\",\"arguments\":" + arguments + "}}";
            mcp_json::Value response;
            mcp_json::Value payload;
            std::string failure;
            isError = true;
            if (!mcp_json::Value::Parse(
                    HttpBody(HttpPostJson(coldServer.Port(), "/mcp", body)),
                    response, failure))
                return payload;
            const mcp_json::Value* result = response.Find("result");
            if (result == nullptr) return payload;
            const mcp_json::Value* error = result->Find("isError");
            isError = error == nullptr || error->AsBool();
            const mcp_json::Value* content = result->Find("content");
            if (content != nullptr && content->IsArray() &&
                !content->AsArray().empty())
                mcp_json::Value::Parse(
                    content->AsArray()[0].Find("text")->AsString(), payload,
                    failure);
            return payload;
        };

        bool isError = false;
        mcp_json::Value payload = callTool("describe", "{}", isError);
        const mcp_json::Value* refusal = payload.Find("error");
        Check(isError && refusal != nullptr &&
                  refusal->AsString() == "NoProjectOpen",
              "before a project exists, a tool is refused by name");

        const std::filesystem::path coldPackage =
            directory / "Cold.cutmachine-project";
        payload = callTool("create_project",
                           "{\"path\":\"" + coldPackage.string() +
                               "\",\"name\":\"Froid\",\"format\":"
                               "\"1920x1080@25\"}",
                           isError);
        const std::filesystem::path coldPath =
            coldPackage / "project.cutmachine.json";
        const std::filesystem::path coldLock =
            coldPackage / "project.cutmachine.json.lock";
        const mcp_json::Value* tracks = payload.Find("tracks");
        Check(!isError && tracks != nullptr && tracks->IsArray() &&
                  tracks->AsArray().size() == 2 &&
                  std::filesystem::exists(coldPath) &&
                  std::filesystem::exists(coldLock),
              "create_project creates the package, opens it and locks it: " +
                  payload.Dump());
        const std::string videoTrack =
            tracks != nullptr && tracks->AsArray().size() == 2
                ? tracks->AsArray()[0].Find("id")->AsString()
                : std::string();
        // PRD14 -- read now: `tracks` points into this answer.
        const auto aliasOf = [&](size_t index) {
            const mcp_json::Value* alias =
                tracks != nullptr && tracks->AsArray().size() > index
                    ? tracks->AsArray()[index].Find("alias")
                    : nullptr;
            return alias != nullptr && alias->IsString() ? alias->AsString()
                                                         : std::string();
        };
        const std::string videoAlias = aliasOf(0);
        const std::string audioAlias = aliasOf(1);
        const std::string timelineId =
            payload.Find("timeline_id") != nullptr
                ? payload.Find("timeline_id")->AsString()
                : std::string();

        const std::string beforeIngest = Read(coldPath);
        std::string outside;
        Check(LockedIngestCommand(coldPath.string(), rushes.string(), false,
                                  outside) != 0 &&
                  outside.find("\"error\":\"IoError\"") != std::string::npos &&
                  Read(coldPath) == beforeIngest,
              "an ingest from outside is refused while the server holds the "
              "project: " +
                  outside);

        payload = callTool("ingest_media",
                           "{\"path\":\"" + rushes.string() + "\"}", isError);
        const mcp_json::Value* added = payload.Find("added");
        int64_t addedCount = 0;
        Check(!isError && added != nullptr && added->AsInt64(addedCount) &&
                  addedCount == 1,
              "ingest_media adds the rush to the open project: " +
                  payload.Dump());
        Project ingested;
        Check(Project::Load(coldPath.string(), ingested, error) &&
                  ingested.rushes.size() == 1,
              "the ingest is committed: " + error);
        const std::string rushId =
            ingested.rushes.empty() ? std::string() : ingested.rushes[0].id;

        payload = callTool(
            "insert_clip",
            "{\"track_id\":\"" + videoTrack + "\",\"source_id\":\"" + rushId +
                "\",\"source_in\":{\"value\":0,\"rate\":25},\"duration\":"
                "{\"value\":20,\"rate\":25},\"timeline_in\":{\"value\":0,"
                "\"rate\":25}}",
            isError);
        Check(!isError,
              "the rush goes straight onto the created video track: " +
                  payload.Dump());

        const std::filesystem::path film = directory / "froid.mp4";
        payload = callTool("export_timeline",
                           "{\"timeline_id\":\"" + timelineId +
                               "\",\"output_path\":\"" + film.string() +
                               "\",\"software\":true}",
                           isError);
        Check(!isError && std::filesystem::exists(film),
              "the cold run exports without leaving MCP: " + payload.Dump());

        // PRD14 -- ROADMAP.md. The same first clip as an agent writes it
        // with no skill at hand, through the real server and the project
        // file: the rush as media_id and by its alias, the tracks by the
        // aliases create_project gave, the mode in lower case. Each of the
        // four used to be a refusal.
        Check(videoAlias == "T1" && audioAlias == "T2",
              "create_project names its tracks by alias as well as by ID");
        payload = callTool("edit_source_range",
                           R"({"media_id":"M1","mode":"insert"})", isError);
        const mcp_json::Value* detail = payload.Find("detail");
        Check(isError && detail != nullptr &&
                  detail->AsString().find("T1 (" + videoTrack + ")") !=
                      std::string::npos,
              "a Source edit with no track is refused with the track that "
              "would do: " +
                  payload.Dump());
        payload = callTool(
            "edit_source_range",
            R"({"media_id":"M1","track_id":"T1","audio_track_ids":["T2"],)"
            R"("mode":"overwrite","timeline_in":{"value":20,"rate":25}})",
            isError);
        Project laid;
        Check(
            !isError && Project::Load(coldPath.string(), laid, error) &&
                laid.timelines.front().tracks[0].clips.size() == 2 &&
                laid.timelines.front().tracks[1].clips.size() == 1 &&
                laid.timelines.front().tracks[0].clips[1].source_id == rushId &&
                laid.timelines.front().tracks[0].clips[1].timeline_in ==
                    RationalTime{20, 25},
            "the first clip is laid by aliases, in the caller's own "
            "words: " +
                payload.Dump() + " " + error);
        // PRD15 -- a track an edit creates comes back in the delta with the
        // alias describe gives it.
        payload = callTool("add_track", R"({"kind":"video"})", isError);
        const mcp_json::Value* delta = payload.Find("delta");
        const mcp_json::Value* createdTracks =
            delta != nullptr ? delta->Find("created_tracks") : nullptr;
        Check(!isError && createdTracks != nullptr &&
                  createdTracks->IsArray() &&
                  createdTracks->AsArray().size() == 1 &&
                  createdTracks->AsArray()[0].Find("alias") != nullptr &&
                  createdTracks->AsArray()[0].Find("alias")->AsString() == "T3",
              "add_track's delta names the new track T3: " + payload.Dump());

        // Another project, created outside, then opened: the lock moves.
        Project other("Autre");
        std::string otherPath;
        Check(CreatePortableProject(
                  (directory / "Other.cutmachine-project").string(), other,
                  otherPath, error),
              "a second project saves: " + error);
        payload = callTool(
            "open_project",
            "{\"path\":\"" + (directory / "Other.cutmachine-project").string() +
                "\"}",
            isError);
        Check(!isError && !std::filesystem::exists(coldLock) &&
                  std::filesystem::exists(otherPath + ".lock"),
              "open_project releases the lock it held and takes the new one: " +
                  payload.Dump());

        // A project someone else holds is refused, and the server keeps its
        // own.
        ProjectSessionLock resident;
        std::string owner;
        Check(resident.Acquire(coldPath.string(), owner, error),
              "another owner takes the first project: " + error);
        payload = callTool("open_project",
                           "{\"path\":\"" + coldPath.string() + "\"}", isError);
        Check(isError && std::filesystem::exists(otherPath + ".lock") &&
                  payload.Find("detail") != nullptr &&
                  payload.Find("detail")->AsString().find("still open") !=
                      std::string::npos,
              "a held project is refused and the previous one stays open: " +
                  payload.Dump());
        payload = callTool("describe", "{}", isError);
        Check(!isError, "the server still answers on the project it kept");
        resident.Release();

        coldServer.Stop();
    }
    Check(!std::filesystem::exists(directory / "Other.cutmachine-project" /
                                   "project.cutmachine.json.lock"),
          "the lock goes with the backend");

    server.Stop();
    CheckChatTranscriptionParity(directory);
    CheckChatAnalysisParity(directory);
    CheckChatIngestParity(directory);
    CheckChatResolveTimelineParity(directory);
    CheckBackgroundTasksAndReplays(directory);
    CheckBackgroundTaskSynonyms(directory);
    CheckBackgroundTasksOnSharedManager(directory);
    // PRD16 -- ROADMAP.md.
    CheckBackgroundExports(directory);
    // PRD17 -- ROADMAP.md.
    CheckSeriesOverOneServer(directory);
    CheckEditPlansOverServer(directory);
    CheckPlannedCaptions(directory);
    CheckMotionCompositionsOverMcp(directory);
    CheckMotionTemplatesOverMcp(directory);
    CheckMotionLibraryOverMcp(directory);
    CheckHumanizeOverMcp(directory);
    CheckEditCacheMatchesPackage(directory);
    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md.
    CheckProjectMotifs(directory);
    // BINS_SPEC.md -- a timeline filed in a bin by its tool.
    CheckTimelineBins(directory);
    CheckStdioTransport(directory);
    // PF1 -- docs/roadmap/PROFIL-2026-11.md.
    CheckProfileThroughBinary(directory);
    CheckProceduresOverStdio(directory);
    CheckLocalRequestsOverServer(directory);
    CheckProtocolVersionNegotiation();
    CheckHttpTokenResolution(directory);
    CheckHttpServeThroughBinary(directory);
    CheckProtocolPromisesOverServer(directory);
    std::filesystem::remove_all(directory);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All MCP integration tests passed\n";
    return 0;
}
