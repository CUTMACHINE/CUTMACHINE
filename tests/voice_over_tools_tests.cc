// GN6b -- docs/roadmap/GENERATIF-2026-10.md. design_voice and
// generate_voice_over, on a project package.
//
// /bin/sh stands in for the model's Python (as in voice_over_tests.cc): it
// records what the engine handed it, copies a real WAV where it is told
// and writes the report. What is checked is the engine's part: the
// reference phrase it fixes, the voice it hands back to clone, the WAV in
// the package's Sound folder, ingested and marked, the journal, undo, and
// DEC49 -- nothing but a voice the engine made is ever a reference.
//
// No AppKit, no model, no network; FFmpeg reads the WAV at ingest.

#include "Document.h"
#include "GeneratedVoice.h"
#include "Generation.h"
#include "Json.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "VoiceOver.h"

#include <sys/stat.h>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;
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
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

Value Parse(const std::string& text) {
    Value value;
    std::string error;
    if (!Value::Parse(text, value, error)) {
        std::cerr << "unparsable test JSON: " << text << " (" << error << ")\n";
        std::abort();
    }
    return value;
}

std::string At(const Value& root, std::initializer_list<const char*> keys) {
    const Value* current = &root;
    for (const char* key : keys) {
        current = current->Find(key);
        if (current == nullptr) return {};
    }
    return current->IsString() ? current->AsString() : current->Dump();
}

std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in),
            std::istreambuf_iterator<char>()};
}

void PutLittleEndian(std::string& out, uint32_t value, int bytes) {
    for (int index = 0; index < bytes; ++index) {
        out.push_back(static_cast<char>((value >> (8 * index)) & 0xff));
    }
}

// Half a second of a 220 Hz tone, PCM 16 bits, mono, 24 kHz: what the
// model's WAV is, short.
void WriteWav(const fs::path& path) {
    constexpr uint32_t kRate = 24000;
    constexpr uint32_t kSamples = kRate / 2;
    std::string data;
    for (uint32_t index = 0; index < kSamples; ++index) {
        const double phase = 2.0 * M_PI * 220.0 * index / kRate;
        const auto sample = static_cast<int16_t>(8000.0 * std::sin(phase));
        PutLittleEndian(data, static_cast<uint16_t>(sample), 2);
    }
    std::string wav = "RIFF";
    PutLittleEndian(wav, 36 + static_cast<uint32_t>(data.size()), 4);
    wav += "WAVEfmt ";
    PutLittleEndian(wav, 16, 4);
    PutLittleEndian(wav, 1, 2);  // PCM
    PutLittleEndian(wav, 1, 2);  // mono
    PutLittleEndian(wav, kRate, 4);
    PutLittleEndian(wav, kRate * 2, 4);
    PutLittleEndian(wav, 2, 2);
    PutLittleEndian(wav, 16, 2);
    wav += "data";
    PutLittleEndian(wav, static_cast<uint32_t>(data.size()), 4);
    wav += data;
    std::ofstream(path, std::ios::binary) << wav;
}

void WriteScript(const fs::path& path, const std::string& body) {
    std::ofstream(path) << "#!/bin/sh\n" << body << '\n';
    chmod(path.c_str(), 0755);
}

// A models folder the search accepts: both model folders, every file the
// script needs, empty.
void WriteModels(const fs::path& models) {
    for (const char* model : {kVoiceDesignModel, kVoiceCloneModel}) {
        for (const std::string& file : VoiceModelFiles()) {
            const fs::path target = models / model / file;
            fs::create_directories(target.parent_path());
            std::ofstream(target) << "";
        }
    }
}

// Each run's arguments, one per line, the runs separated by "----".
std::vector<std::string> Runs(const fs::path& received) {
    std::vector<std::string> runs;
    std::string current;
    std::istringstream lines(ReadText(received));
    std::string line;
    while (std::getline(lines, line)) {
        if (line == "----") {
            runs.push_back(current);
            current.clear();
        } else {
            current += line + "\n";
        }
    }
    return runs;
}

struct Fixture {
    fs::path root;
    fs::path received;
    std::string project_path;
};

Fixture MakeFixture() {
    Fixture fixture;
    fixture.root = fs::temp_directory_path() /
                   ("cutmachine-voice-tools-" + GenerateUlid());
    fs::create_directories(fixture.root);
    fixture.received = fixture.root / "received.txt";
    const fs::path source = fixture.root / "model-output.wav";
    WriteWav(source);
    const fs::path script = fixture.root / "voice_over.sh";
    WriteScript(script,
                "mode=\"$1\"\n"
                "{ for argument in \"$@\"; do printf '%s\\n' \"$argument\"; "
                "done; printf -- '----\\n'; } >> '" +
                    fixture.received.string() +
                    "'\n"
                    "while [ $# -gt 0 ]; do\n"
                    "  case \"$1\" in\n"
                    "    --out) out=\"$2\"; shift;;\n"
                    "    --report) report=\"$2\"; shift;;\n"
                    "    --seed) seed=\"$2\"; shift;;\n"
                    "  esac\n"
                    "  shift\n"
                    "done\n"
                    "cp '" +
                    source.string() +
                    "' \"$out\"\n"
                    "if [ \"$mode\" = design ]; then "
                    "model=Qwen3-TTS-12Hz-1.7B-VoiceDesign; else "
                    "model=Qwen3-TTS-12Hz-1.7B-Base; fi\n"
                    "printf '{\"sample_rate\":24000,\"samples\":12000,"
                    "\"model\":\"%s\",\"revision\":\"abc123\","
                    "\"device\":\"cpu\",\"seed\":%s}' \"$model\" \"$seed\" "
                    "> \"$report\"");
    const fs::path models = fixture.root / "models";
    WriteModels(models);
    setenv(kVoicePythonVariable, "/bin/sh", 1);
    setenv(kVoiceScriptVariable, script.c_str(), 1);
    setenv(kVoiceModelsVariable, models.c_str(), 1);
    std::string detail;
    Check(CreatePortableProject((fixture.root / "film").string(),
                                Project("Film"), fixture.project_path, detail),
          "project: " + detail);
    return fixture;
}

Value Arguments(const std::string& json) { return Parse(json); }

std::string Quoted(const std::string& text) {
    return "\"" + mcp_json::EscapeJsonString(text) + "\"";
}

}  // namespace

int main() {
    Test("every language of the models has a reference phrase", [] {
        for (const std::string& code : VoiceOverLanguageCodes()) {
            Check(!VoiceReferenceText(code).empty(),
                  "a reference phrase in '" + code + "'");
        }
        Check(
            VoiceReferenceText("xx").empty() && VoiceReferenceText("").empty(),
            "none in a language the models do not say");
        Check(VoiceReferenceText("fr") ==
                  "Bonjour. Je vais vous raconter une histoire. Prenez le "
                  "temps d'écouter, et laissez-vous porter par les images.",
              "the French one is the bench's, heard on 7 November");
    });

    Test("DEC48: free, so neither tool is a spending one nor runs alone", [] {
        Check(!ToolSpendsOnPersonsAccount("design_voice") &&
                  !ToolSpendsOnPersonsAccount("generate_voice_over"),
              "no permission prompt for a generation that spends nothing");
        const McpToolRegistry registry;
        int found = 0;
        for (const McpTool& tool : registry.Tools()) {
            if (tool.name != "design_voice" &&
                tool.name != "generate_voice_over")
                continue;
            ++found;
            Check(!tool.background,
                  tool.name +
                      " never runs as a background task: a task "
                      "cannot ingest or mark a media");
            Check(Contains(tool.description, "asked"),
                  tool.name + " says it is made only when asked");
        }
        Check(found == 2, "both tools are in the catalog");
    });

    Fixture fixture = MakeFixture();
    const fs::path package = fs::path(fixture.project_path).parent_path();
    McpProjectBackend backend(fixture.project_path);
    McpToolRegistry registry;
    Ulid voiceId;
    fs::path voicePath;

    Test("design_voice: a voice in Sound/, ingested and marked", [&] {
        const auto made = registry.Call(
            backend, "design_voice",
            Arguments(R"({"description":"a warm, calm voice","seed":7})"));
        Check(made.ok,
              "the voice is made: " + made.error_name + " " + made.message);
        if (!made.ok) return;
        const Value answer = Parse(made.result_json);
        voiceId = At(answer, {"media_id"});
        voicePath = At(answer, {"path"});
        Check(
            voicePath.parent_path().filename() == "Sound" &&
                voicePath.parent_path().parent_path() == package &&
                voicePath.filename().string().rfind("voix-", 0) == 0 &&
                fs::exists(voicePath),
            "the WAV is in the package's Sound folder: " + voicePath.string());
        // 12 000 samples at 24 kHz, as the report says.
        Check(std::abs(std::stod("0" + At(answer, {"duration_s"})) - 0.5) <
                      1e-9 &&
                  At(answer, {"device"}) == "cpu",
              "the report's length and device: " + made.result_json);

        const std::vector<std::string> runs = Runs(fixture.received);
        Check(runs.size() == 1, "the script ran once");
        const std::string run = runs.empty() ? "" : runs[0];
        Check(run.rfind("design\n", 0) == 0 &&
                  Contains(run, std::string(kVoiceDesignModel) + "\n") &&
                  Contains(run, "--text\n" + VoiceReferenceText("fr") + "\n") &&
                  Contains(run, "--description\na warm, calm voice\n") &&
                  Contains(run, "--language\nFrench\n") &&
                  Contains(run, "--seed\n7\n"),
              "VoiceDesign says the French reference phrase in the voice "
              "described: " +
                  run);

        Document document;
        std::string message;
        backend.SnapshotDocument(document, message);
        const LibraryMedia* voice = document.FindLibraryMedia(voiceId);
        Check(voice != nullptr && document.library.size() == 1,
              "one media in the library");
        if (voice == nullptr) return;
        const MediaGeneration& generation = voice->generation;
        Check(generation.service == "local" &&
                  generation.kind == "voice_design" &&
                  generation.model ==
                      "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign@abc123" &&
                  generation.prompt == "a warm, calm voice" &&
                  generation.seed == "7" &&
                  generation.reference_text == VoiceReferenceText("fr") &&
                  generation.source_media_id.empty() &&
                  generation.announced_cost.empty() &&
                  generation.created_at.size() == 20,
              "marked: local, voice_design, its description and its words");
        Check(At(answer, {"generation", "reference_text"}) ==
                  VoiceReferenceText("fr"),
              "the answer says the words the voice says");

        const auto described =
            registry.Call(backend, "describe", Value::MakeObject());
        Check(
            described.ok &&
                Contains(described.result_json, "\"kind\":\"voice_design\"") &&
                Contains(
                    described.result_json,
                    "\"reference_text\":" + Quoted(VoiceReferenceText("fr"))),
            "describe shows the voice's provenance and its words");
        const std::string projectText = ReadText(fixture.project_path);
        Check(Contains(projectText, "\"reference_text\""),
              "the project file keeps the words");
        Check(Contains(projectText, "\"document_version\":25"),
              "and declares version 25, which an older binary refuses "
              "rather than drop them");
    });

    Test("generate_voice_over: the voice cloned, the take marked", [&] {
        const std::string text = "Le 21 mars, à 10 h — « on vous attend ».";
        const auto said = registry.Call(
            backend, "generate_voice_over",
            Arguments(R"({"voice_media_id":")" + voiceId + R"(","text":)" +
                      Quoted(text) + R"(,"seed":9})"));
        Check(said.ok, "the voice-over is made: " + said.error_name + " " +
                           said.message);
        if (!said.ok) return;
        const Value answer = Parse(said.result_json);
        const Ulid takeId = At(answer, {"media_id"});
        const fs::path takePath = At(answer, {"path"});
        Check(takePath.parent_path() == voicePath.parent_path() &&
                  takePath.filename().string().rfind("voix-off-", 0) == 0,
              "the take is in Sound/ too: " + takePath.string());

        const std::vector<std::string> runs = Runs(fixture.received);
        const std::string run = runs.size() == 2 ? runs[1] : "";
        Check(run.rfind("clone\n", 0) == 0 &&
                  Contains(run, std::string(kVoiceCloneModel) + "\n") &&
                  Contains(run, "--reference\n" +
                                    voicePath.lexically_normal().string() +
                                    "\n") &&
                  Contains(run, "--reference-text\n" +
                                    VoiceReferenceText("fr") + "\n") &&
                  Contains(run, "--text\n" + text + "\n") &&
                  Contains(run, "--seed\n9\n"),
              "Base clones the voice's WAV with its words, and says the text "
              "as written: " +
                  run);

        Document document;
        std::string message;
        backend.SnapshotDocument(document, message);
        const LibraryMedia* take = document.FindLibraryMedia(takeId);
        Check(take != nullptr && take->generation.service == "local" &&
                  take->generation.kind == "voice_over" &&
                  take->generation.model ==
                      "Qwen/Qwen3-TTS-12Hz-1.7B-Base@abc123" &&
                  take->generation.prompt == text &&
                  take->generation.seed == "9" &&
                  take->generation.source_media_id == voiceId &&
                  take->generation.reference_text.empty(),
              "marked: local, voice_over, its text, from the voice");
    });

    Test("DEC49: nothing but a voice the engine made is cloned", [&] {
        const fs::path rush = fixture.root / "interview.wav";
        WriteWav(rush);
        const auto ingested = registry.Call(
            backend, "ingest_media",
            Arguments(R"({"path":)" + Quoted(rush.string()) + "}"));
        Check(ingested.ok, "a recording is ingested: " + ingested.message);
        Document document;
        std::string message;
        backend.SnapshotDocument(document, message);
        Ulid recordingId;
        Ulid takeId;
        for (const LibraryMedia& media : document.library) {
            if (media.filename == "interview.wav") recordingId = media.id;
            if (media.generation.kind == "voice_over") takeId = media.id;
        }
        const size_t runsBefore = Runs(fixture.received).size();
        const size_t mediaBefore = document.library.size();
        for (const auto& [id, what] :
             {std::pair<Ulid, std::string>{recordingId, "a recording"},
              std::pair<Ulid, std::string>{takeId, "a voice-over"}}) {
            const auto refused =
                registry.Call(backend, "generate_voice_over",
                              Arguments(R"({"voice_media_id":")" + id +
                                        R"(","text":"Bonjour."})"));
            Check(!refused.ok && refused.error_name == "NotAnEngineVoice" &&
                      Contains(refused.message, "DEC49") &&
                      Contains(refused.message, "design_voice"),
                  what + " is refused as a reference: " + refused.error_name +
                      " " + refused.message);
        }
        const auto unknown = registry.Call(
            backend, "generate_voice_over",
            Arguments(
                R"({"voice_media_id":"01K3ZZZZZZZZZZZZZZZZZZZZZZ","text":"Bonjour."})"));
        Check(!unknown.ok, "an unknown media is refused");
        Document after;
        backend.SnapshotDocument(after, message);
        Check(Runs(fixture.received).size() == runsBefore &&
                  after.library.size() == mediaBefore,
              "the model never ran, nothing entered the library");
    });

    Test("arguments are checked before anything runs", [&] {
        const size_t runsBefore = Runs(fixture.received).size();
        const std::pair<const char*, const char*> cases[] = {
            {"design_voice", R"({"description":"a voice","language":"xx"})"},
            {"design_voice", R"({"description":""})"},
            {"design_voice", R"({"description":"a voice","seed":-1})"},
            {"design_voice", R"({"description":"a voice","seed":4294967296})"},
            {"design_voice", R"({"description":"a voice","text":"Bonjour"})"},
            {"generate_voice_over", R"({"text":"Bonjour."})"},
        };
        for (const auto& [tool, json] : cases) {
            const auto refused = registry.Call(backend, tool, Arguments(json));
            Check(!refused.ok && refused.error_name == "ValidationFailed",
                  std::string(tool) + " " + json + " is refused: " +
                      refused.error_name + " " + refused.message);
        }
        Check(Runs(fixture.received).size() == runsBefore,
              "the model never ran");
    });

    Test("without the models, the refusal names what is missing", [&] {
        const fs::path empty = fixture.root / "no-models";
        fs::create_directories(empty);
        setenv(kVoiceModelsVariable, empty.c_str(), 1);
        Document before;
        std::string message;
        backend.SnapshotDocument(before, message);
        const auto refused = registry.Call(
            backend, "design_voice", Arguments(R"({"description":"a voice"})"));
        Check(!refused.ok && refused.error_name == "VoiceOverNotFound" &&
                  Contains(refused.message, kVoiceDesignModel),
              "VoiceOverNotFound, naming the model: " + refused.error_name +
                  " " + refused.message);
        Document after;
        backend.SnapshotDocument(after, message);
        Check(after.library.size() == before.library.size(),
              "nothing entered the library");
        setenv(kVoiceModelsVariable, (fixture.root / "models").c_str(), 1);
    });

    Test("undo takes back the mark, then the media", [&] {
        const auto made = registry.Call(
            backend, "design_voice",
            Arguments(R"({"description":"a bright voice","language":"en"})"));
        Check(made.ok, "an English voice is made: " + made.message);
        const Ulid id =
            made.ok ? At(Parse(made.result_json), {"media_id"}) : Ulid();
        Check(Contains(made.ok ? Runs(fixture.received).back() : "",
                       "--text\n" + VoiceReferenceText("en") + "\n"),
              "VoiceDesign says the English phrase");
        Check(Contains(ReadText(fixture.project_path),
                       mcp_json::EscapeJsonString(VoiceReferenceText("en"))),
              "the project file keeps its English words");
        Document document;
        std::string message;
        Check(registry.Call(backend, "undo", Value::MakeObject()).ok,
              "the mark is undone");
        backend.SnapshotDocument(document, message);
        const LibraryMedia* voice = document.FindLibraryMedia(id);
        Check(voice != nullptr && !IsGenerated(voice->generation),
              "first the provenance goes");
        Check(registry.Call(backend, "redo", Value::MakeObject()).ok,
              "and comes back from the journal");
        backend.SnapshotDocument(document, message);
        voice = document.FindLibraryMedia(id);
        Check(voice != nullptr &&
                  voice->generation.reference_text == VoiceReferenceText("en"),
              "with its words, replayed from the journal");
        Check(registry.Call(backend, "undo", Value::MakeObject()).ok &&
                  registry.Call(backend, "undo", Value::MakeObject()).ok,
              "the mark, then the ingest, are undone");
        backend.SnapshotDocument(document, message);
        Check(document.FindLibraryMedia(id) == nullptr, "then the media goes");
    });

    unsetenv(kVoicePythonVariable);
    unsetenv(kVoiceScriptVariable);
    unsetenv(kVoiceModelsVariable);
    std::error_code ignored;
    fs::remove_all(fixture.root, ignored);
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
