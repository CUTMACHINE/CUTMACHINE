// PA3 -- docs/roadmap/PARITE-2026-11.md. Who speaks when: the report the
// script writes read and refused, the cache written and read back, the
// stretches of speech taken from a transcript's words, the voice of a word,
// where the three files are looked for, and the whole path through the
// tools on a stored project -- with a script that stands for the model, so
// that the suite runs where the model is not installed. What the model is
// worth is measured elsewhere (scripts/diarize.py says the figures).

#include "Cli.h"
#include "Ingest.h"
#include "Json.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "SpeakerDiarization.h"
#include "Ulid.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

std::string Quote(const std::string& text) { return "'" + text + "'"; }

RationalTime Seconds(int64_t value, int32_t rate = 1) { return {value, rate}; }

// Two voices over four seconds: 0 for a second and a half, 1 for a second,
// a silence, 0 again.
const char* kReport =
    R"({"sample_rate":16000,"samples":64000,"speakers":2,"speakers_heard":2,)"
    R"("segments":[{"start":0,"end":24000,"speaker":0},)"
    R"({"start":24000,"end":40000,"speaker":1},)"
    R"({"start":48000,"end":64000,"speaker":0}],)"
    R"("similarity":[[0,1,512]],"engine":"sherpa-onnx",)"
    R"("engine_version":"2.0.1","embedding":"model.onnx",)"
    R"("speech_from":"words"})";

}  // namespace

int main() {
    Test("the script's report is read, and a wrong one refused", [] {
        SpeakerDiarization diarization;
        std::string error;
        Check(ReadDiarizationReport(kReport, "M1", diarization, error),
              "the report reads: " + error);
        Check(diarization.media_id == "M1" && diarization.speakers == 2 &&
                  diarization.speakers_heard == 2 &&
                  diarization.segments.size() == 3 &&
                  diarization.segments[1].start == Seconds(3, 2) &&
                  diarization.segments[1].end == Seconds(5, 2) &&
                  diarization.segments[1].speaker == 1 &&
                  diarization.similarity.size() == 1 &&
                  diarization.similarity[0].per_mille == 512 &&
                  diarization.speech_from == "words" &&
                  diarization.engine_version == "2.0.1",
              "its segments are exact times and its voices numbers");
        const auto refused = [&](std::string report, const std::string& why) {
            SpeakerDiarization none;
            std::string refusal;
            Check(!ReadDiarizationReport(report, "M1", none, refusal) &&
                      refusal.find(why) != std::string::npos &&
                      none.segments.empty(),
                  "refused (" + why + "): " + refusal);
        };
        std::string swapped = kReport;
        swapped.replace(swapped.find("\"start\":24000"), 13, "\"start\":20000");
        refused(swapped, "not in order");
        std::string stranger = kReport;
        stranger.replace(stranger.find("\"speaker\":1"), 11, "\"speaker\":2");
        refused(stranger, "voice 2 of 2");
        std::string past = kReport;
        past.replace(past.find("\"end\":64000"), 11, "\"end\":64001");
        refused(past, "inside the sound");
        refused(R"({"sample_rate":44100,"samples":10,"speakers":2,)"
                R"("speakers_heard":2,"segments":[]})",
                "sample_rate");
        refused(R"({"sample_rate":16000,"samples":64000,"speakers":2,)"
                R"("speakers_heard":1,"segments":[]})",
                "no stretch of speech");
        refused("[]", "not a JSON object");
    });

    Test("the cache is written and read back the same", [] {
        SpeakerDiarization diarization;
        std::string error;
        ReadDiarizationReport(kReport, "01KPA300000000000000000M01",
                              diarization, error);
        const std::string cache = SerializeSpeakerDiarization(diarization);
        SpeakerDiarization read;
        Check(DeserializeSpeakerDiarization(cache, read, error) &&
                  SerializeSpeakerDiarization(read) == cache &&
                  read.media_id == diarization.media_id &&
                  read.segments.size() == 3 &&
                  read.segments[2].start == Seconds(3) &&
                  read.similarity[0].per_mille == 512,
              "a cache round-trips: " + error);
        std::string other = cache;
        other.replace(other.find("\"version\":1"), 11, "\"version\":9");
        Check(!DeserializeSpeakerDiarization(other, read, error) &&
                  error.find("version") != std::string::npos,
              "a cache of another version is not read: " + error);
        Check(!LoadSpeakerDiarization("/nowhere/speakers.json", read, error) &&
                  error.find("no speakers cache") != std::string::npos,
              "an absent cache says so: " + error);
    });

    Test("the stretches of speech are the words, joined across breaths", [] {
        const std::vector<TranscriptWord> words = {
            {"Bonjour", {0, 100}, {40, 100}},
            {"à", {45, 100}, {50, 100}},
            {"tous", {52, 100}, {90, 100}},
            // A pause of 0.6 s: another stretch.
            {"Merci", {150, 100}, {200, 100}},
            // A word with no length is not speech.
            {"", {210, 100}, {210, 100}},
        };
        const std::vector<SpeechStretch> stretches =
            SpeechStretchesOfWords(words, {3, 10});
        Check(stretches.size() == 2 && stretches[0].start == Seconds(0) &&
                  stretches[0].end == Seconds(9, 10) &&
                  stretches[1].start == Seconds(3, 2) &&
                  stretches[1].end == Seconds(2),
              "two stretches, cut at the pause");
        Check(SpeechStretchesJson(stretches) == "[[0,14400],[24000,32000]]\n",
              "spelled in whole samples: " + SpeechStretchesJson(stretches));
        // Rounded outwards: a stretch never loses the edge of a word.
        Check(SpeechStretchesJson({{{1, 3}, {2, 3}}}) == "[[5333,10667]]\n",
              "an edge inside a sample is rounded outwards");
    });

    Test("a word takes the voice that speaks over most of it", [] {
        SpeakerDiarization diarization;
        std::string error;
        ReadDiarizationReport(kReport, "M1", diarization, error);
        Check(SpeakerOfSpan(diarization, Seconds(1, 2), Seconds(1)) == 0,
              "inside the first voice");
        Check(SpeakerOfSpan(diarization, Seconds(13, 10), Seconds(18, 10)) == 1,
              "0.2 s of voice 0 and 0.3 s of voice 1: voice 1");
        Check(SpeakerOfSpan(diarization, Seconds(14, 10), Seconds(16, 10)) == 0,
              "as much of each: the earlier voice");
        Check(
            SpeakerOfSpan(diarization, Seconds(26, 10), Seconds(29, 10)) == -1,
            "in the silence: no voice");
        Check(SpeakerOfSpan(diarization, Seconds(2), Seconds(2)) == -1,
              "a span with no length: no voice");
        const std::vector<RationalTime> durations =
            SpeakerDurations(diarization);
        Check(durations.size() == 2 && durations[0] == Seconds(5, 2) &&
                  durations[1] == Seconds(1),
              "voice 0 speaks 2.5 s and voice 1 one second");
    });

    Test("the three files are looked for, and named when missing", [] {
        DiarizationSearch search;
        search.python_variable = "/env/bin/python";
        search.script_variable = "/src/scripts/diarize.py";
        search.executable_directory = "/app/build";
        search.settings_file = "/home/settings.env";
        std::vector<std::string> present = {
            "/env/bin/python", "/src/scripts/diarize.py",
            std::string("/app/build/diarization/models/") +
                kSpeakerEmbeddingModel};
        search.is_file = [&present](const std::string& path) {
            for (const std::string& file : present)
                if (file == path) return true;
            return false;
        };
        const DiarizationLocation found = LocateDiarization(search);
        Check(found.Found() && found.python.path == "/env/bin/python" &&
                  found.model.path == present[2] &&
                  found.model.origin == tool_files::Origin::kExecutable,
              "the variables first, then beside the executable: " +
                  found.refusal);
        const DiarizationSettings settings = found.Settings();
        DiarizationRequest request;
        request.audio_path = "/tmp/sound.wav";
        request.report_path = "/tmp/report.json";
        request.speakers = 2;
        const std::vector<std::string> plain =
            DiarizationArguments(settings, request);
        Check(plain ==
                  std::vector<std::string>(
                      {"/env/bin/python", "/src/scripts/diarize.py", "--audio",
                       "/tmp/sound.wav", "--embedding", present[2],
                       "--speakers", "2", "--report", "/tmp/report.json"}),
              "the script is started on the sound, the model and the count");
        request.speech_path = "/tmp/speech.json";
        const std::vector<std::string> worded =
            DiarizationArguments(settings, request);
        Check(worded.size() == plain.size() + 2 &&
                  worded[worded.size() - 2] == "--speech",
              "and on the stretches of speech when there are words");

        present.pop_back();
        const DiarizationLocation missing = LocateDiarization(search);
        Check(!missing.Found() &&
                  missing.refusal.find("diarization model not found: ") == 0 &&
                  missing.refusal.find(kDiarizationModelVariable) !=
                      std::string::npos &&
                  missing.refusal.find("/home/settings.env") !=
                      std::string::npos &&
                  DiarizationRefusalName(missing.refusal) ==
                      kDiarizationNotFoundErrorName,
              "a model found nowhere is a named refusal: " + missing.refusal);
        Check(DiarizationRefusalName("anything else").empty(),
              "another sentence is not that refusal");
    });

    Test("the tools read a rush through a script standing for the model", [] {
        const std::filesystem::path root =
            std::filesystem::temp_directory_path() /
            (GenerateUlid() + "-speakers");
        std::filesystem::create_directories(root);
        const std::filesystem::path audio = root / "voices.wav";
        const std::string generate =
            Quote(FFMPEG_EXECUTABLE) +
            " -hide_banner -loglevel error -f lavfi -i "
            "sine=frequency=220:duration=4 -c:a pcm_s16le -ar 48000 -y " +
            Quote(audio.string());
        Check(std::system(generate.c_str()) == 0, "the rush is generated");
        // The stand-in: a shell script run by /bin/sh in the Python's
        // place. It checks what the engine hands it -- a 16 kHz mono WAV
        // and a count -- and writes the report above.
        const std::filesystem::path script = root / "diarize.sh";
        {
            std::ofstream file(script);
            file << "audio=; report=; speakers=\n"
                    "while [ $# -gt 0 ]; do\n"
                    "  case \"$1\" in\n"
                    "    --audio) audio=\"$2\"; shift;;\n"
                    "    --report) report=\"$2\"; shift;;\n"
                    "    --speakers) speakers=\"$2\"; shift;;\n"
                    "  esac\n"
                    "  shift\n"
                    "done\n"
                    "[ -s \"$audio\" ] || { echo 'no sound' >&2; exit 1; }\n"
                    "# 4 s at 16 kHz, 16 bits, mono: 128000 bytes and a "
                    "header.\n"
                    "size=$(wc -c < \"$audio\")\n"
                    "[ \"$size\" -ge 128000 ] && [ \"$size\" -le 128200 ] || "
                    "{ echo \"sound of $size bytes\" >&2; exit 1; }\n"
                    "[ \"$speakers\" = 2 ] || { echo 'the model cannot' >&2; "
                    "exit 1; }\n"
                    "echo 'progress 0 1' >&2\n"
                    "cat > \"$report\" <<'REPORT'\n"
                 << kReport << "\nREPORT\n";
        }
        const std::filesystem::path model = root / "model.onnx";
        std::ofstream(model) << "weights";
        setenv(kDiarizationPythonVariable, "/bin/sh", 1);
        setenv(kDiarizationScriptVariable, script.c_str(), 1);
        setenv(kDiarizationModelVariable, model.c_str(), 1);

        std::string path;
        std::string error;
        std::string output;
        Check(CreatePortableProject(
                  (root / "project").string(),
                  Project::FromDocument(Document{}, "Speakers"), path, error) &&
                  IngestCommand(path, audio.string(), false, output) == 0,
              "the project holds the rush: " + error + output);
        Project project;
        Check(LoadStoredProject(path, project, error) &&
                  project.rushes.size() == 1,
              "one rush: " + error);
        if (project.rushes.size() != 1) return;
        const Ulid mediaId = project.rushes.front().id;
        std::ifstream stored(path, std::ios::binary);
        const std::string before{std::istreambuf_iterator<char>(stored),
                                 std::istreambuf_iterator<char>()};
        const std::string media = "{\"media_id\":\"" + mediaId + "\"";

        Check(
            CallToolCommand(path, "list_speakers", media + "}", output) != 0 &&
                output.find("run analyze_speakers first") != std::string::npos,
            "before the analysis there is nothing to list: " + output);
        Check(
            CallToolCommand(path, "analyze_speakers", media + "}", output) != 0,
            "the count of voices is required: " + output);
        Check(CallToolCommand(path, "analyze_speakers",
                              media + ",\"speakers\":9}", output) != 0 &&
                  output.find("[1, 8]") != std::string::npos,
              "nine voices are refused: " + output);
        Check(CallToolCommand(path, "analyze_speakers",
                              media + ",\"speakers\":3}", output) != 0 &&
                  output.find("the model cannot") != std::string::npos &&
                  output.find("progress") == std::string::npos,
              "the script's own failure is relayed, without its progress "
              "lines: " +
                  output);
        Check(CallToolCommand(path, "analyze_speakers",
                              media + ",\"speakers\":2}", output) == 0 &&
                  output.find("\"speakers\":2") != std::string::npos &&
                  output.find("\"segments\":3") != std::string::npos,
              "two voices are told apart: " + output);
        const std::filesystem::path cache =
            std::filesystem::path(path).parent_path() / ".cutmachine" /
            "speakers" / (mediaId + ".json");
        SpeakerDiarization cached;
        Check(LoadSpeakerDiarization(cache.string(), cached, error) &&
                  cached.media_id == mediaId && cached.segments.size() == 3,
              "the cache is beside the transcripts: " + error);

        Check(CallToolCommand(path, "list_speakers", media + "}", output) == 0,
              "the cache is listed: " + output);
        mcp_json::Value listed;
        Check(mcp_json::Value::Parse(output, listed, error), "it is JSON");
        const mcp_json::Value* result = listed.Find("result");
        const mcp_json::Value& body = result != nullptr ? *result : listed;
        const mcp_json::Value* turns = body.Find("turns");
        const mcp_json::Value* voices = body.Find("voices");
        Check(turns != nullptr && turns->IsArray() &&
                  turns->AsArray().size() == 3 && voices != nullptr &&
                  voices->AsArray().size() == 2 &&
                  output.find("\"per_mille\":512") != std::string::npos &&
                  output.find("\"turn_count\":3") != std::string::npos,
              "three turns of two voices, and how alike they are: " + output);
        Check(CallToolCommand(path, "list_speakers",
                              media + ",\"max_turns\":1}", output) == 0 &&
                  output.find("\"next_from_turn\":1") != std::string::npos,
              "a long list is paged by turn: " + output);

        std::ifstream after(path, std::ios::binary);
        const std::string unchanged{std::istreambuf_iterator<char>(after),
                                    std::istreambuf_iterator<char>()};
        Check(unchanged == before,
              "the analysis leaves the project's bytes alone");

        // The model gone: a refusal by name, and nothing started.
        std::filesystem::remove(model);
        Check(CallToolCommand(path, "analyze_speakers",
                              media + ",\"speakers\":2}", output) != 0 &&
                  output.find(kDiarizationNotFoundErrorName) !=
                      std::string::npos &&
                  output.find(kDiarizationModelVariable) != std::string::npos,
              "without its model the tool says what is missing: " + output);
        unsetenv(kDiarizationPythonVariable);
        unsetenv(kDiarizationScriptVariable);
        unsetenv(kDiarizationModelVariable);
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
    });

    return failures == 0 ? 0 : 1;
}
