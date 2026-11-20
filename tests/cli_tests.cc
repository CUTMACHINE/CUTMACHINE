#include "BuildInfo.h"
#include "CaptionBreakModel.h"
#include "CaptionRender.h"
#include "Cli.h"
#include "CliCommands.h"
#include "Document.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Json.h"
#include "LookLut.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "MulticamCli.h"
#include "Operations.h"
#include "Procedures.h"
#include "ProcessLauncher.h"
#include "ProjectArchive.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveImport.h"
#include "TimelineTranscription.h"
#include "Transcription.h"
#include "Ulid.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <unistd.h>

#include "TestSupport.h"

namespace {

void CheckFailureEnvelope(const std::string& output, const std::string& label,
                          const std::string& expectedError) {
    mcp_json::Value envelope;
    std::string parseError;
    const bool parsed = mcp_json::Value::Parse(output, envelope, parseError) &&
                        envelope.IsObject();
    Check(parsed, label + " returns a JSON object: " + parseError);
    if (!parsed) return;
    const mcp_json::Value* ok = envelope.Find("ok");
    const mcp_json::Value* error = envelope.Find("error");
    const mcp_json::Value* detail = envelope.Find("detail");
    Check(ok != nullptr && ok->IsBool() && !ok->AsBool(),
          label + " returns ok:false");
    Check(error != nullptr && error->IsString() &&
              error->AsString() == expectedError,
          label + " preserves " + expectedError);
    Check(
        detail != nullptr && detail->IsString() && !detail->AsString().empty(),
        label + " returns a non-empty detail");
}

// PRD19 -- ROADMAP.md. The command line is read in main.mm, so what it
// answers is tested through the binary itself.
struct CliRun {
    // Ran to its end within the deadline; `status` is then its exit status.
    bool exited = false;
    int status = -1;
    std::string output;
};

// Runs a command line, each argument passed as it is: no shell. Its error
// output is dropped. A run that has not answered after thirty seconds is
// stopped and fails its test: a command line that reached the editor's
// window instead of an answer must not hold the screen.
CliRun RunCommand(const std::vector<std::string>& command) {
    process::Request request;
    request.arguments = command;
    request.standard_output = process::Stream::kPipe;
    request.standard_error = process::Stream::kDiscard;
    CliRun run;
    request.on_output = [&run](const char* bytes, size_t count) {
        run.output.append(bytes, count);
    };
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    request.cancelled = [deadline] {
        return std::chrono::steady_clock::now() > deadline;
    };
    const process::Result result = process::Run(request);
    run.exited = result.Started() && !result.cancelled && !result.Signalled();
    if (run.exited) run.status = result.ExitStatus();
    return run;
}

// The binary with these arguments.
CliRun RunCutmachine(const std::vector<std::string>& arguments) {
    std::vector<std::string> command = {CUTMACHINE_EXECUTABLE};
    command.insert(command.end(), arguments.begin(), arguments.end());
    return RunCommand(command);
}

// PRD21 -- the binary in an environment the test writes whole: `env -i`
// passes it nothing of the test's own -- no key, no PATH, no settings file
// -- but the `NAME=value` assignments given.
CliRun RunCutmachineIn(const std::vector<std::string>& environment,
                       const std::vector<std::string>& arguments) {
    std::vector<std::string> command = {"/usr/bin/env", "-i"};
    command.insert(command.end(), environment.begin(), environment.end());
    command.push_back(CUTMACHINE_EXECUTABLE);
    command.insert(command.end(), arguments.begin(), arguments.end());
    return RunCommand(command);
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// PRD20 -- the `detail` of a refusal: one JSON object with ok:false and
// this `error`. Empty, with the checks failed, for anything else.
std::string RefusalDetail(const std::string& output, const std::string& label,
                          const std::string& expectedError) {
    CheckFailureEnvelope(output, label, expectedError);
    mcp_json::Value envelope;
    std::string parseError;
    if (!mcp_json::Value::Parse(output, envelope, parseError) ||
        !envelope.IsObject()) {
        return std::string();
    }
    const mcp_json::Value* detail = envelope.Find("detail");
    return detail != nullptr && detail->IsString() ? detail->AsString()
                                                   : std::string();
}

// A refusal of the command line by the binary: status 2, ValidationFailed.
std::string UsageRefusal(const std::vector<std::string>& arguments) {
    std::string label = "cutmachine";
    for (const std::string& argument : arguments) label += " " + argument;
    const CliRun run = RunCutmachine(arguments);
    Check(run.exited && run.status == 2,
          label + " exits with status 2: " + std::to_string(run.status) + " " +
              run.output);
    return RefusalDetail(run.output, label, "ValidationFailed");
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string ProjectBatchJson(const std::vector<ProjectOperation>& operations) {
    std::string result = "[";
    for (size_t index = 0; index < operations.size(); ++index) {
        if (index) result += ',';
        result += SerializeProjectOperation(operations[index]);
    }
    return result + ']';
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
           {5, 25}},
          {"01K30000000000000000000004",
           "01K30000000000000000000001",
           {200, 25},
           {10, 25},
           {20, 25}}}},
    };
    document.sequence.markers = {
        {"01K30000000000000000000005",
         "Premier raccord",
         {15, 25},
         "#33AAFF",
         "Montage"},
    };
    return document;
}

// SYNCHRO-2026-10 SY1 -- a mono 16-bit WAV at 48 kHz, written as the
// format says: what an audio recorder hands the editor.
void WriteWav(const std::filesystem::path& path,
              const std::vector<float>& samples) {
    std::ofstream file(path, std::ios::binary);
    const auto put32 = [&file](uint32_t value) {
        for (int shift = 0; shift < 32; shift += 8) {
            file.put(static_cast<char>((value >> shift) & 0xff));
        }
    };
    const auto put16 = [&file](uint16_t value) {
        file.put(static_cast<char>(value & 0xff));
        file.put(static_cast<char>((value >> 8) & 0xff));
    };
    const uint32_t bytes = static_cast<uint32_t>(samples.size() * 2);
    file.write("RIFF", 4);
    put32(36 + bytes);
    file.write("WAVEfmt ", 8);
    put32(16);
    put16(1);
    put16(1);
    put32(48000);
    put32(96000);
    put16(2);
    put16(16);
    file.write("data", 4);
    put32(bytes);
    for (const float sample : samples) {
        const float clamped = std::max(-1.0f, std::min(1.0f, sample));
        put16(static_cast<uint16_t>(
            static_cast<int16_t>(std::lround(clamped * 32767.0f))));
    }
}

// Bursts and pauses with a faint hiss, like a voice: deterministic.
std::vector<float> VoiceLike(size_t count) {
    std::vector<float> samples(count);
    uint32_t state = 12345;
    const auto next = [&state]() {
        state = state * 1664525u + 1013904223u;
        return static_cast<float>(state >> 8) / 16777216.0f;
    };
    size_t index = 0;
    while (index < count) {
        const size_t loud = 4800 + static_cast<size_t>(next() * 14400);
        const float gain = 0.05f + 0.4f * next();
        for (size_t at = 0; at < loud && index < count; ++at, ++index) {
            samples[index] = gain * (2.0f * next() - 1.0f);
        }
        const size_t quiet = 2400 + static_cast<size_t>(next() * 12000);
        for (size_t at = 0; at < quiet && index < count; ++at, ++index) {
            samples[index] = 0.002f * (2.0f * next() - 1.0f);
        }
    }
    return samples;
}

}  // namespace

int main() {
    // LG2 -- the cartons generate_captions builds are measured by Core
    // Text, as in the cutmachine executable.
    InstallCaptionRenderer();
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-cli-tests");
    std::filesystem::create_directory(directory);

    // SYNCHRO-2026-10 SY1 -- the binary decodes two recordings of one
    // take through FFmpeg and finds where the second starts on the first,
    // to the sample, without writing to the project.
    {
        const std::filesystem::path package = directory / "sync-project";
        const Project project("Sync CLI");
        std::string projectPath;
        std::string message;
        Check(CreatePortableProject(package.string(), project, projectPath,
                                    message),
              "create audio offset fixture: " + message);
        const std::vector<float> take = VoiceLike(30 * 48000);
        WriteWav(package / "camera.wav",
                 {take.begin(), take.begin() + 20 * 48000});
        WriteWav(package / "recorder.wav",
                 {take.begin() + 36000, take.begin() + 36000 + 20 * 48000});
        for (const char* name : {"camera.wav", "recorder.wav"}) {
            const auto ingested = RunCutmachine(
                {"--ingest", projectPath, (package / name).string()});
            Check(ingested.status == 0,
                  "ingest " + std::string(name) + ": " + ingested.output);
        }
        std::string before;
        {
            std::ifstream stored(projectPath);
            before.assign(std::istreambuf_iterator<char>(stored), {});
        }
        Project loaded;
        Check(LoadStoredProject(projectPath, loaded, message) &&
                  loaded.rushes.size() == 2,
              "two rushes ingested: " + message);
        if (loaded.rushes.size() == 2) {
            const auto idOf = [&loaded](const std::string& file) {
                for (const LibraryMedia& media : loaded.rushes) {
                    if (media.filename == file) return media.id;
                }
                return Ulid();
            };
            const auto measured =
                RunCutmachine({"--measure-audio-offset", projectPath,
                               idOf("camera.wav"), idOf("recorder.wav")});
            Check(
                measured.status == 0 &&
                    Contains(measured.output,
                             R"("offset":{"value":36000,"rate":48000})"),
                "the recorder is found 36 000 samples in: " + measured.output);
            const auto itself =
                RunCutmachine({"--measure-audio-offset", projectPath,
                               idOf("camera.wav"), idOf("camera.wav")});
            Check(itself.status != 0 &&
                      Contains(itself.output, "\"ValidationFailed\""),
                  "a rush is not measured against itself: " + itself.output);
            // FC6 -- the ducking proposal decodes a rush the same way and
            // answers where it speaks and the curve a music would follow,
            // writing nothing; settings out of range are refused before
            // the decode.
            const auto ducked = RunCutmachine(
                {"--propose-ducking", projectPath, idOf("camera.wav")});
            Check(ducked.status == 0 &&
                      Contains(ducked.output,
                               R"("duration":{"value":960000,"rate":48000})") &&
                      Contains(ducked.output, R"("speech":[{"start":)") &&
                      Contains(ducked.output, R"("gain_curve":[{"time":)"),
                  "the ducking proposal answers ranges and a curve: " +
                      ducked.output);
            const auto refused =
                RunCutmachine({"--propose-ducking", projectPath,
                               idOf("camera.wav"), "--hysteresis-db", "10"});
            Check(refused.status != 0 &&
                      Contains(refused.output, "\"ValidationFailed\"") &&
                      Contains(refused.output, "out of range"),
                  "a hysteresis as large as the threshold is refused: " +
                      refused.output);
            std::ifstream stored(projectPath);
            const std::string after(std::istreambuf_iterator<char>(stored), {});
            Check(after == before, "measuring writes nothing to the project");
        }
    }

    // DB1 -- use the actual binary, archive a package and query it without
    // opening the editor or supplying a project to the read command.
    {
        const Project project("Archive CLI");
        std::string projectPath;
        std::string message;
        Check(CreatePortableProject((directory / "archive-project").string(),
                                    project, projectPath, message),
              "create archive CLI fixture: " + message);
        const std::string database = (directory / "projects.sqlite").string();
        const std::string context =
            "{\"snapshot_id\":\"" + GenerateUlid() +
            "\",\"run_id\":\"cli-run\",\"step\":\"selection\"}";
        const auto captured = RunCutmachine(
            {"--archive-project", (directory / "archive-project").string(),
             database, context});
        Check(captured.exited && captured.status == 0 &&
                  captured.output.find("\"snapshot_id\"") != std::string::npos,
              "binary archives package JSON: " + captured.output);
        const auto retry = RunCutmachine(
            {"--archive-project", projectPath, database, context});
        Check(retry.status == 0 &&
                  retry.output.find("\"replayed\":true") != std::string::npos,
              "a new CLI process finds the first capture");
        const auto queried =
            RunCutmachine({"--query-project-archive", database,
                           "SELECT json_extract(document_json,'$.name'),step "
                           "FROM project_snapshots"});
        Check(queried.exited && queried.status == 0 &&
                  queried.output.find("\"Archive CLI\",\"selection\"") !=
                      std::string::npos,
              "binary reads JSON through SQL: " + queried.output);
        const auto denied = RunCutmachine({"--query-project-archive", database,
                                           "DELETE FROM project_snapshots"});
        Check(denied.exited && denied.status != 0,
              "binary cannot delete archived data");
        const auto status =
            RunCutmachine({"--project-archive-status",
                           (directory / "archive-project").string()});
        Check(status.status == 0 && Contains(status.output, "\"pending\":0") &&
                  Contains(status.output, "\"backup\":{\"configured\":false}"),
              "binary inspects the automatic archive: " + status.output);
        const auto synced =
            RunCutmachine({"--sync-project-archive", projectPath});
        Check(
            synced.status == 0 && Contains(synced.output, "\"complete\":true"),
            "binary syncs the automatic archive: " + synced.output);
        const std::string importDatabase =
            (directory / "cli-import/projects.sqlite").string();
        const auto imported = RunCutmachine(
            {"--import-project-archive", importDatabase,
             (directory / "archive-project").string(), projectPath});
        Check(imported.status == 0 &&
                  Contains(imported.output, "\"imported\":2") &&
                  Contains(imported.output, "\"replayed\":true"),
              "binary imports package paths and project files idempotently: " +
                  imported.output);
        const auto partial = RunCutmachine(
            {"--import-project-archive", importDatabase, projectPath,
             (directory / "missing-import").string()});
        Check(partial.status == 1 &&
                  Contains(partial.output, "\"refused\":1") &&
                  Contains(partial.output, "\"imported\":1"),
              "binary reports partial imports with a failing exit status");

        const std::string automaticDatabase =
            (directory / "automatic/projects.sqlite").string();
        const auto automatic = RunCommand(
            {"/usr/bin/env", "CUTMACHINE_PROJECT_ARCHIVE=" + automaticDatabase,
             CUTMACHINE_EXECUTABLE, "--create-project",
             (directory / "automatic-project").string(), "Automatic CLI"});
        const auto automaticRows =
            RunCutmachine({"--query-project-archive", automaticDatabase,
                           "SELECT json_extract(document_json,'$.name'),step "
                           "FROM project_snapshots"});
        Check(automatic.status == 0 && automaticRows.status == 0 &&
                  Contains(automaticRows.output, "\"Automatic CLI\",\"save\""),
              "real CLI creation captures the saved project automatically");

        // DEC44 -- the archive's daily copy: a save makes the day's first
        // when a folder is set, the explicit command writes it again, and
        // without a folder the command refuses by name.
        const std::filesystem::path backups = directory / "OneDrive" / "Copies";
        std::filesystem::create_directories(backups);
        const std::string today =
            project_archive::BackupFileName(std::time(nullptr));
        const auto copiedBySave = RunCommand(
            {"/usr/bin/env", "CUTMACHINE_PROJECT_ARCHIVE=" + automaticDatabase,
             "CUTMACHINE_ARCHIVE_BACKUP_DIR=" + backups.string(),
             CUTMACHINE_EXECUTABLE, "--create-project",
             (directory / "automatic-copy").string(), "Copied CLI"});
        Check(copiedBySave.status == 0 &&
                  std::filesystem::exists(backups / today),
              "a save makes the day's copy of the archive: " +
                  copiedBySave.output);
        const auto explicitCopy = RunCommand(
            {"/usr/bin/env", "CUTMACHINE_PROJECT_ARCHIVE=" + automaticDatabase,
             "CUTMACHINE_ARCHIVE_BACKUP_DIR=" + backups.string(),
             CUTMACHINE_EXECUTABLE, "--backup-project-archive"});
        Check(
            explicitCopy.status == 0 &&
                Contains(explicitCopy.output, "\"performed\":true") &&
                Contains(explicitCopy.output, today) &&
                Contains(explicitCopy.output, "\"kept\":1"),
            "--backup-project-archive writes it again: " + explicitCopy.output);
        const auto copiedRows = RunCutmachine(
            {"--query-project-archive", (backups / today).string(),
             "SELECT json_extract(document_json,'$.name') FROM "
             "project_snapshots ORDER BY created_at"});
        Check(copiedRows.status == 0 &&
                  Contains(copiedRows.output, "\"Copied CLI\""),
              "the copy reads as an archive: " + copiedRows.output);
        const auto unset = RunCommand(
            {"/usr/bin/env", "CUTMACHINE_PROJECT_ARCHIVE=" + automaticDatabase,
             "CUTMACHINE_ARCHIVE_BACKUP_DIR=off", CUTMACHINE_EXECUTABLE,
             "--backup-project-archive"});
        Check(unset.status == 1 &&
                  !RefusalDetail(unset.output, "backup without a folder",
                                 "NotConfigured")
                       .empty(),
              "without a folder, no copy, by name: " + unset.output);
        Check(UsageRefusal({"--backup-project-archive", "now"})
                      .find("--backup-project-archive") != std::string::npos,
              "the copy takes no argument");
    }

    // CH4 -- docs/roadmap/CHARTE-2026-11.md. The charter tools from the
    // command line, with no project (the empty path of --call-tool): 0 on
    // an answer, 1 on a refusal, the library the one the child is given.
    {
        const std::filesystem::path library = directory / "charter-library";
        const std::string inLibrary =
            "CUTMACHINE_MOTION_LIBRARY=" + library.string();
        const auto callTool = [&](const std::string& tool,
                                  const std::string& arguments) {
            return RunCommand({"/usr/bin/env", inLibrary, CUTMACHINE_EXECUTABLE,
                               "--call-tool", "", tool, arguments});
        };
        const CliRun listed = callTool("read_motion_kit", "{}");
        Check(listed.exited && listed.status == 0 &&
                  Contains(listed.output, "\"kits\":[]") &&
                  Contains(listed.output, library.string()),
              "--call-tool \"\" read_motion_kit lists the charters with no "
              "project: " +
                  listed.output);
        const CliRun saved = callTool(
            "save_motion_kit",
            R"({"kit":"marque","expected_version":0,"content":)"
            R"({"format":"brand-kit.v1","description":"Marque.",)"
            R"("props":{"accent":"#1A5C8E","police":"Exemple Sans"}}})");
        Check(saved.exited && saved.status == 0 &&
                  Contains(saved.output, "\"version\":1"),
              "save_motion_kit creates a charter from the command line: " +
                  saved.output);
        const CliRun edited =
            callTool("edit_motion_kit",
                     R"({"kit":"marque","expected_version":1,"edits":)"
                     R"([{"path":"props.accent","value":"#2B6D9F"}]})");
        Check(edited.exited && edited.status == 0 &&
                  Contains(edited.output, "\"version\":2") &&
                  Contains(Read(library / "kits" / "marque.json"),
                           "\"accent\": \"#2B6D9F\""),
              "edit_motion_kit edits it, file included: " + edited.output);
        const CliRun stale =
            callTool("edit_motion_kit",
                     R"({"kit":"marque","expected_version":1,"edits":)"
                     R"([{"path":"props.accent","value":"#000000"}]})");
        Check(
            stale.exited && stale.status == 1 &&
                !RefusalDetail(stale.output, "a stale charter edit", "StaleKit")
                     .empty() &&
                Contains(Read(library / "kits" / "marque.json"),
                         "\"accent\": \"#2B6D9F\""),
            "a stale edit exits 1, by name, and writes nothing: " +
                stale.output);
    }

    // PRD19 -- ROADMAP.md. `--version` says what the binary is: the
    // project's one version number and the commit it was built at, with no
    // project and nothing set up. The engine's function and the binary give
    // the same answer, since both were linked from the same build.
    {
        const BuildInfo& build = CurrentBuild();
        std::string output;
        Check(VersionCommand(output) == 0 &&
                  output == "{\"ok\":true,\"version\":\"" + build.version +
                                "\",\"commit\":\"" + build.commit +
                                "\",\"dirty\":" +
                                (build.dirty ? "true" : "false") + "}\n",
              "the version command reports the build: " + output);
        mcp_json::Value answer;
        std::string parseError;
        Check(mcp_json::Value::Parse(output, answer, parseError) &&
                  answer.IsObject() && answer.Find("ok") &&
                  answer.Find("ok")->IsBool() && answer.Find("ok")->AsBool() &&
                  answer.Find("version") &&
                  answer.Find("version")->IsString() && answer.Find("commit") &&
                  answer.Find("commit")->IsString() && answer.Find("dirty") &&
                  answer.Find("dirty")->IsBool(),
              "the version answer is one JSON object: " + parseError);
        const CliRun run = RunCutmachine({"--version"});
        Check(run.exited && run.status == 0 && run.output == output,
              "the binary answers --version with the same build, status 0: " +
                  run.output);
        const CliRun help = RunCutmachine({"--help"});
        Check(help.exited && help.status == 0 &&
                  help.output.find("--version") != std::string::npos,
              "--help lists --version");
    }

    // RM3 -- docs/roadmap/REGLES-2026-10.md. The forge through the binary:
    // a corpus read, the rule and the model measured on held-out groups, no
    // file written while models are off (CUTMACHINE_MODELS=off, the suites'
    // setting), and a corpus of fewer groups than folds refused.
    {
        const std::filesystem::path corpus =
            std::filesystem::temp_directory_path() /
            ("cutmachine-cli-corpus-" + std::to_string(::getpid()) + ".jsonl");
        {
            std::ofstream file(corpus);
            for (const char* group : {"a", "b", "c"})
                file << "{\"group\": \"" << group
                     << "\", \"frame_rate\": {\"num\": 25, \"den\": 1}, "
                        "\"cards\": [{\"text\": \"Sur la colline,\", "
                        "\"start\": 0, \"duration\": 20}, {\"text\": \"tu as "
                        "vu l'orage\", \"start\": 20, \"duration\": 25}, "
                        "{\"text\": \"sans ouvrir la porte.\", \"start\": 45, "
                        "\"duration\": 25}]}\n";
        }
        const CliRun trained = RunCutmachine(
            {"--train-caption-breaks", corpus.string(), "--folds", "3"});
        mcp_json::Value answer;
        std::string parseError;
        mcp_json::Value::Parse(trained.output, answer, parseError);
        const mcp_json::Value* heldOut = answer.Find("held_out");
        const mcp_json::Value* installed = answer.Find("installed");
        Check(trained.exited && trained.status == 0 && heldOut != nullptr &&
                  heldOut->Find("rule") != nullptr &&
                  heldOut->Find("model") != nullptr && installed != nullptr &&
                  !installed->AsBool() &&
                  answer.Find("candidate_path") == nullptr &&
                  answer.Find("fingerprint") != nullptr,
              "--train-caption-breaks measures the rule and the model, and "
              "writes nothing while models are off: " +
                  trained.output.substr(0, 300));
        const CliRun unjudged =
            RunCutmachine({"--train-caption-breaks", corpus.string(), "--folds",
                           "3", "--install"});
        Check(
            unjudged.exited && unjudged.status == 1 &&
                unjudged.output.find("evaluation corpus") != std::string::npos,
            "installing without an evaluation corpus is refused: " +
                unjudged.output);
        const CliRun tooFew = RunCutmachine(
            {"--train-caption-breaks", corpus.string(), "--folds", "4"});
        Check(
            tooFew.exited && tooFew.status == 1 &&
                tooFew.output.find("fewer than the 4 folds") !=
                    std::string::npos,
            "a corpus of fewer groups than folds is refused: " + tooFew.output);
        std::filesystem::remove(corpus);
    }

    // PRD18 -- ROADMAP.md (ACP4). The procedures the binary carries, through
    // the binary: one JSON object per answer, the shape get_procedure gives.
    {
        const auto answerOf = [](const CliRun& run) {
            mcp_json::Value answer;
            std::string parseError;
            mcp_json::Value::Parse(run.output, answer, parseError);
            return answer;
        };
        const CliRun listed = RunCutmachine({"--procedure"});
        const mcp_json::Value listing = answerOf(listed);
        const mcp_json::Value* procedures = listing.Find("procedures");
        Check(listed.exited && listed.status == 0 &&
                  listing.Find("ok") != nullptr &&
                  listing.Find("ok")->AsBool() && procedures != nullptr &&
                  procedures->IsArray() &&
                  procedures->AsArray().size() == ListProcedures().size() &&
                  !procedures->AsArray().empty(),
              "--procedure lists the procedures: " +
                  listed.output.substr(0, 200));
        ProcedureText skill;
        std::string errorName;
        std::string message;
        ReadProcedureFile("monter-une-itw", "SKILL.md", skill, errorName,
                          message);
        const CliRun read = RunCutmachine({"--procedure", "monter-une-itw"});
        const mcp_json::Value answer = answerOf(read);
        Check(read.exited && read.status == 0 && answer.Find("ok") != nullptr &&
                  answer.Find("name") != nullptr &&
                  answer.Find("name")->AsString() == "monter-une-itw" &&
                  answer.Find("file") != nullptr &&
                  answer.Find("file")->AsString() == "SKILL.md" &&
                  answer.Find("files") != nullptr &&
                  answer.Find("files")->IsArray() &&
                  answer.Find("text") != nullptr && !skill.text.empty() &&
                  answer.Find("text")->AsString() == skill.text,
              "--procedure <name> gives its SKILL.md and its files");
        const CliRun cited = RunCutmachine(
            {"--procedure", "monter-une-itw", "mots-et-bornes.md"});
        Check(
            cited.exited && cited.status == 0 &&
                answerOf(cited).Find("file") != nullptr &&
                answerOf(cited).Find("file")->AsString() == "mots-et-bornes.md",
            "--procedure <name> <file> gives that file");
        const CliRun unknown = RunCutmachine({"--procedure", "monter-un-film"});
        Check(unknown.exited && unknown.status == 1 &&
                  RefusalDetail(unknown.output, "--procedure monter-un-film",
                                "UnknownProcedure")
                          .find("monter-une-itw") != std::string::npos,
              "an unknown procedure is refused with the known ones: " +
                  unknown.output);
        const std::string usage =
            "; " + CliUsageLine(*FindCliCommand("--procedure"));
        Check(UsageRefusal({"--procedure", "a", "b", "c"}) ==
                      "unexpected --procedure argument 'c'" + usage &&
                  UsageRefusal({"--procedure", "--frob"}) ==
                      "unknown --procedure option '--frob'" + usage,
              "a command line --procedure cannot read is refused by name, "
              "with the usage");
    }

    // PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. `--check-setup` through the
    // binary, in an environment the test writes whole: nothing of the
    // test's own reaches it -- not the real key, not the settings file
    // under $HOME.
    {
        const std::filesystem::path home = directory / "setup-home";
        std::filesystem::create_directories(home);
        // Not a key: as plain as the other suites' "sk-test-key", so that a
        // scan for secrets does not take it for one.
        const std::string madeUpKey = "sk-test-key-cli";
        const std::vector<std::string> machine = {
            "HOME=" + home.string(),
            "CUTMACHINE_ENV_FILE=" + (home / "absent.env").string(),
        };
        const auto with = [&machine](const std::vector<std::string>& more) {
            std::vector<std::string> environment = machine;
            environment.insert(environment.end(), more.begin(), more.end());
            return environment;
        };
        const auto parse = [](const CliRun& run) {
            mcp_json::Value answer;
            std::string parseError;
            mcp_json::Value::Parse(run.output, answer, parseError);
            return answer;
        };
        const auto text = [](const mcp_json::Value* value,
                             const std::string& key) {
            const mcp_json::Value* found =
                value != nullptr ? value->Find(key) : nullptr;
            return found != nullptr && found->IsString() ? found->AsString()
                                                         : std::string();
        };
        // The items by id, in the order the answer gives them.
        const auto items = [](const mcp_json::Value& answer,
                              std::vector<std::string>& order) {
            std::map<std::string, mcp_json::Value> byId;
            const mcp_json::Value* list = answer.Find("items");
            if (list == nullptr || !list->IsArray()) return byId;
            for (const mcp_json::Value& item : list->AsArray()) {
                const mcp_json::Value* id = item.Find("id");
                if (id == nullptr || !id->IsString()) continue;
                order.push_back(id->AsString());
                byId[id->AsString()] = item;
            }
            return byId;
        };

        const CliRun found = RunCutmachineIn(
            with({"PATH=/usr/bin:/bin",
                  std::string("CUTMACHINE_FFMPEG=") + FFMPEG_EXECUTABLE,
                  "CUTMACHINE_WHISPER_MODEL=/nope",
                  "CUTMACHINE_MFA_PYTHON=/nowhere/python",
                  "ANTHROPIC_API_KEY=" + madeUpKey}),
            {"--check-setup"});
        const mcp_json::Value answer = parse(found);
        std::vector<std::string> order;
        std::map<std::string, mcp_json::Value> byId = items(answer, order);
        Check(found.exited && found.status == 0 &&
                  answer.Find("ok") != nullptr && answer.Find("ok")->IsBool() &&
                  answer.Find("ok")->AsBool() &&
                  answer.Find("error") == nullptr,
              "--check-setup with FFmpeg answers ok:true, status 0: " +
                  found.output.substr(0, 400));
        Check(order == std::vector<std::string>{"ffmpeg", "whisper_model",
                                                "whisper_hesitations_model",
                                                "mfa", "agent_key",
                                                "archive_backup", "voice_over"},
              "--check-setup lists its items in order");
        const std::string ffmpegState = text(&byId["ffmpeg"], "state");
        // The machine's FFmpeg decides between the two: one without an
        // encoder of the table is incomplete, and still edits and exports.
        Check((ffmpegState == "ok" || ffmpegState == "incomplete") &&
                  byId["ffmpeg"].Find("required") != nullptr &&
                  byId["ffmpeg"].Find("required")->AsBool() &&
                  text(byId["ffmpeg"].Find("found"), "origin") ==
                      "CUTMACHINE_FFMPEG",
              "FFmpeg is found where CUTMACHINE_FFMPEG says: " +
                  byId["ffmpeg"].Dump());
        Check(text(&byId["whisper_model"], "state") == "missing" &&
                  Contains(text(&byId["whisper_model"], "detail"), "'/nope'"),
              "a Whisper model at /nope is missing: " +
                  byId["whisper_model"].Dump());
        Check(text(&byId["mfa"], "state") == "missing" &&
                  text(&byId["mfa"], "error") == "MfaNotFound" &&
                  Contains(text(&byId["mfa"], "detail"), "/nowhere/python"),
              "MFA's Python at /nowhere is missing, by name: " +
                  byId["mfa"].Dump());
        Check(text(&byId["agent_key"], "state") == "ok" &&
                  text(byId["agent_key"].Find("found"), "origin") ==
                      "environment",
              "the key is said to be set, from the environment: " +
                  byId["agent_key"].Dump());
        Check(!Contains(found.output, madeUpKey) &&
                  !Contains(found.output, "sk-test") &&
                  !Contains(found.output, "key-cli"),
              "the key's value appears nowhere in the answer");
        // DEC44 -- no folder for the archive's copy: said, and ok.
        Check(text(&byId["archive_backup"], "state") == "optional_absent" &&
                  text(&byId["archive_backup"], "error") == "NotConfigured",
              "no folder for the archive's copy is optional: " +
                  byId["archive_backup"].Dump());
        std::filesystem::create_directories(home / "Copies");
        const CliRun withFolder = RunCutmachineIn(
            with({"PATH=/usr/bin:/bin",
                  std::string("CUTMACHINE_FFMPEG=") + FFMPEG_EXECUTABLE,
                  "CUTMACHINE_ARCHIVE_BACKUP_DIR=" +
                      (home / "Copies").string()}),
            {"--check-setup"});
        std::vector<std::string> folderOrder;
        auto withFolderItems = items(parse(withFolder), folderOrder);
        Check(withFolder.status == 0 &&
                  text(&withFolderItems["archive_backup"], "state") == "ok",
              "a folder that exists: ok: " +
                  withFolderItems["archive_backup"].Dump());

        // The same key from the settings file only.
        const std::filesystem::path settings = home / "setup.env";
        Write(settings, "ANTHROPIC_API_KEY=" + madeUpKey + "\n");
        const CliRun fromFile = RunCutmachineIn(
            {"HOME=" + home.string(),
             "CUTMACHINE_ENV_FILE=" + settings.string(), "PATH=/usr/bin:/bin",
             std::string("CUTMACHINE_FFMPEG=") + FFMPEG_EXECUTABLE},
            {"--check-setup"});
        order.clear();
        byId = items(parse(fromFile), order);
        Check(text(byId["agent_key"].Find("found"), "origin") ==
                      "settings_file" &&
                  !Contains(fromFile.output, madeUpKey) &&
                  !Contains(fromFile.output, "sk-test"),
              "a key in the settings file is said to come from it, never "
              "shown: " +
                  byId["agent_key"].Dump());

        // No FFmpeg anywhere: the one required item, and the status.
        const CliRun lacking = RunCutmachineIn(
            with({"PATH=" + (home / "no-bin").string()}), {"--check-setup"});
        const mcp_json::Value refusal = parse(lacking);
        const size_t itemCount = order.size();
        order.clear();
        byId = items(refusal, order);
        Check(lacking.exited && lacking.status == 1 &&
                  refusal.Find("ok") != nullptr &&
                  !refusal.Find("ok")->AsBool() &&
                  text(&refusal, "error") == "FfmpegNotFound" &&
                  Contains(text(&refusal, "detail"), "FFmpeg not found: ") &&
                  order.size() == itemCount &&
                  text(&byId["ffmpeg"], "state") == "missing" &&
                  text(&byId["agent_key"], "state") == "optional_absent",
              "without FFmpeg, --check-setup answers ok:false with "
              "FfmpegNotFound and every item, status 1: " +
                  lacking.output.substr(0, 400));

        const CliRun help = RunCutmachine({"--help"});
        Check(help.exited && help.status == 0 &&
                  Contains(help.output, "--check-setup") &&
                  Contains(help.output, "--check-ffmpeg"),
              "--help lists --check-setup beside --check-ffmpeg");
        Check(UsageRefusal({"--check-setup", "now"}) ==
                  "unexpected --check-setup argument 'now'; " +
                      CliUsageLine(*FindCliCommand("--check-setup")),
              "--check-setup takes nothing more");
    }

    // ADOBE-CLI-2026-09 -- discovery and invocation reuse the MCP catalog,
    // including exact undo, named refusals and the resident project lock.
    {
        std::string output;
        mcp_json::Value catalog;
        std::string error;
        Check(ListToolsCommand(output) == 0 &&
                  mcp_json::Value::Parse(output, catalog, error),
              "CLI discovers the engine tool catalog");
        const auto* tools = catalog.Find("tools");
        const McpToolRegistry registry;
        Check((tools != nullptr) && tools->IsArray() &&
                  tools->AsArray().size() == registry.Tools().size(),
              "CLI advertises every MCP capability without a second catalog");
        if ((tools != nullptr) && tools->IsArray()) {
            for (size_t index = 0; index < tools->AsArray().size(); ++index) {
                const auto& tool = tools->AsArray()[index];
                Check((tool.Find("name") != nullptr) &&
                          tool.Find("name")->AsString() ==
                              registry.Tools()[index].name &&
                          (tool.Find("inputSchema") != nullptr) &&
                          tool.Find("inputSchema")->IsObject(),
                      "CLI publishes the original tool name and JSON schema");
            }
        }
        Project project = Project::FromDocument(Fixture(), "CLI tools");
        std::string projectPath;
        Check(CreatePortableProject((directory / "call-tools").string(),
                                    project, projectPath, error),
              "tool fixture saves as a native project package");
        const std::filesystem::path path(projectPath);
        const std::string before = Read(path);
        const std::string beforeDocument = project.SaveToString();
        const std::string arguments =
            "{\"timeline_id\":\"" + project.active_timeline_id +
            "\",\"clip_id\":\"01K30000000000000000000003\","
            "\"edge\":\"Tail\",\"delta\":{\"value\":-2,\"rate\":25}}";
        const int trimResult = CallToolCommand(path.string(), "trim_clip",
                                               arguments, output, true);
        Check(trimResult == 0,
              "CLI invokes a real timeline edit through the MCP dispatcher: " +
                  output);
        Check(output.find('\n') == output.size() - 1,
              "tool results remain one JSON record per stdout line");
        Project changed;
        Check(LoadStoredProject(path.string(), changed, error) &&
                  changed.MakeActiveDocument()
                          .FindClip("01K30000000000000000000003")
                          ->duration == RationalTime{8, 25},
              "tool invocation commits the expected exact trim");
        Check(CallToolCommand(
                  path.string(), "undo",
                  "{\"timeline_id\":\"" + project.active_timeline_id + "\"}",
                  output, true) == 0 &&
                  Read(path) == before &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.SaveToString() == beforeDocument,
              "CLI undo restores the exact original project bytes");
        // PRD35 -- ROADMAP.md. The timeline's journal, read from the package
        // by the project backend: what `--call-tool <project> describe
        // '{"only":["history"]}'` answers.
        Check(CallToolCommand(path.string(), "describe",
                              R"({"only":["history"]})", output) == 0 &&
                  output.find(R"("result":{"history":{"timeline_id":")" +
                              project.active_timeline_id +
                              R"(","total":1,"applied":0,"entries":[)"
                              R"({"rank":1,"type":"TrimClip","operations":1,)"
                              R"("undone":true}]}})") != std::string::npos,
              "the journal of a package reads as its history, the trim "
              "undone: " +
                  output);
        Check(CallToolCommand(path.string(), "trim_clip", "{}", output, true) !=
                  0,
              "CLI honors the local explicit-timeline policy");
        CheckFailureEnvelope(output, "call_tool timeline policy",
                             "TimelineRequired");
        Check(CallToolCommand(path.string(), "not_a_tool", "{}", output) != 0,
              "unknown CLI tools are refused");
        CheckFailureEnvelope(output, "call_tool unknown tool", "UnknownTool");
        Check(CallToolCommand(path.string(), "describe", "{", output) == 2,
              "malformed tool JSON is refused before any mutation");
        CheckFailureEnvelope(output, "call_tool malformed JSON", "ParseError");
        Check(CallToolCommand(path.string(), "clone_timeline",
                              "{\"source_timeline_id\":\"" +
                                  project.active_timeline_id +
                                  "\",\"name\":\"Event variant\"}",
                              output, true) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.timelines.size() == 2 &&
                  changed.active_timeline_id == project.active_timeline_id,
              "CLI clones and commits a complete timeline without stealing "
              "focus");
        const Ulid cloneId = changed.timelines.back().id;
        const Ulid clonedClipId =
            changed.timelines.back().tracks[0].clips[0].id;
        const std::string afterClone = changed.SaveToString();
        Check(CallToolCommand(path.string(), "trim_clip",
                              "{\"timeline_id\":\"" + cloneId +
                                  "\",\"clip_id\":\"" + clonedClipId +
                                  "\",\"edge\":\"Tail\",\"delta\":{\"value\":-"
                                  "2,\"rate\":25}}",
                              output, true) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.MakeDocument(cloneId)
                          .FindClip(clonedClipId)
                          ->duration == RationalTime{8, 25} &&
                  changed.MakeActiveDocument()
                          .FindClip("01K30000000000000000000003")
                          ->duration == RationalTime{10, 25},
              "an edit in the cloned timeline preserves the original clip");
        Check(CallToolCommand(path.string(), "undo",
                              "{\"timeline_id\":\"" + cloneId + "\"}", output,
                              true) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.SaveToString() == afterClone,
              "the clone has its own exact timeline undo journal");
        Check(UndoProjectOperationCommand(path.string(), output) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.SaveToString() == beforeDocument,
              "project undo removes the clone and restores canonical bytes");
        Check(RedoProjectOperationCommand(path.string(), output) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.SaveToString() == afterClone,
              "persisted project redo restores the same clone IDs");

        // R6 -- docs/roadmap/REEL-2026-09.md. The look of a sequence, laid
        // from the CLI through the same tool MCP calls: the .cube is copied
        // into the package under its digest and the timeline is given that
        // reference. Nothing of the file's numbers reaches the project.
        const std::filesystem::path lookCube = directory / "author-look.cube";
        {
            std::ofstream cube(lookCube, std::ios::binary | std::ios::trunc);
            cube << "TITLE \"lift\"\nLUT_3D_SIZE 2\n";
            for (int corner = 0; corner < 8; ++corner) {
                cube << (0.2 + 0.8 * (corner & 1)) << ' '
                     << (0.2 + 0.8 * ((corner >> 1) & 1)) << ' '
                     << (0.2 + 0.8 * ((corner >> 2) & 1)) << '\n';
            }
        }
        const std::string beforeLook = [&] {
            Project held;
            std::string held_error;
            LoadStoredProject(path.string(), held, held_error);
            return held.SaveToString();
        }();
        Check(
            CallToolCommand(path.string(), "set_sequence_lut",
                            "{\"timeline_id\":\"" + project.active_timeline_id +
                                "\",\"path\":\"" + lookCube.string() + "\"}",
                            output, true) == 0 &&
                LoadStoredProject(path.string(), changed, error) &&
                !changed.MakeActiveDocument().sequence.look_lut.digest.empty(),
            "the CLI lays a look on the selected timeline: " + output);
        const SequenceLut laid = changed.MakeActiveDocument().sequence.look_lut;
        Check(laid.name == "author-look.cube" && laid.space.empty() &&
                  IsLookLutDigest(laid.digest),
              "the look is stored as a reference, named and digested");
        Check(std::filesystem::exists(path.parent_path() /
                                      LookLutPackagePath(laid.digest)),
              "the .cube is copied into the package under its digest");
        Check(DescribeCommand(path.string(), output) == 0 &&
                  output.find("\"look_lut\"") != std::string::npos &&
                  output.find(laid.digest) != std::string::npos,
              "describe reads the look back");
        Check(CallToolCommand(
                  path.string(), "set_sequence_lut",
                  "{\"timeline_id\":\"" + project.active_timeline_id + "\"}",
                  output, true) == 0 &&
                  LoadStoredProject(path.string(), changed, error) &&
                  changed.SaveToString() == beforeLook,
              "clearing the look gives back the exact project bytes");
        // Guard rail 4: the author declares the space; a name this build has
        // no stage for is refused, and nothing is written.
        Check(
            CallToolCommand(path.string(), "set_sequence_lut",
                            "{\"timeline_id\":\"" + project.active_timeline_id +
                                "\",\"path\":\"" + lookCube.string() +
                                "\",\"space\":\"acescct\"}",
                            output, true) != 0 &&
                LoadStoredProject(path.string(), changed, error) &&
                changed.SaveToString() == beforeLook,
            "a space that does not exist without colour management is "
            "refused: " +
                output);
        Check(UndoProjectOperationCommand(path.string(), output) == 0,
              "restore the original project before checking rejected calls");
        ProjectSessionLock lock;
        std::string owner;
        Check(lock.Acquire(path.string(), owner, error), "resident lock held");
        Check(CallToolCommand(path.string(), "describe", "{}", output) != 0,
              "CLI refuses to race a resident project owner");
        CheckFailureEnvelope(output, "call_tool project lock", "IoError");
        // IA5 -- every other writer takes the same lock from main.mm.
        ProjectSessionLock writer;
        Check(!LockProjectForCliWriter(path.string(), writer, output) &&
                  !writer.Held(),
              "a CLI writer is refused while the project is held");
        CheckFailureEnvelope(output, "CLI writer project lock", "IoError");
        Check(CliCommandWritesProject({"--apply-op", "p", "{}"}) &&
                  CliCommandWritesProject({"--transcribe", "p", "m"}) &&
                  CliCommandWritesProject(
                      {"--align-transcripts", "p", "--write"}) &&
                  !CliCommandWritesProject({"--align-transcripts", "p"}) &&
                  !CliCommandWritesProject({"--describe", "p"}) &&
                  !CliCommandWritesProject({"--ingest", "p", "m"}) &&
                  !CliCommandWritesProject({"--call-tool", "p", "describe"}),
              "writers and readers are told apart");
        // A package folder stands for its project file; any other path is
        // left as given.
        const std::filesystem::path package = path.parent_path();
        Check(
            ProjectPathOfPackage(package.string()) == path.string() &&
                ProjectPathOfPackage(package.string() + "/") == path.string() &&
                ProjectPathOfPackage(path.string()) == path.string() &&
                ProjectPathOfPackage(package.parent_path().string()) ==
                    package.parent_path().string() &&
                ProjectPathOfPackage((package / "absent").string()) ==
                    (package / "absent").string(),
            "a package folder names its project file: " +
                ProjectPathOfPackage(package.string()));
        Check(Read(path) == before, "refused calls preserve the project bytes");
    }

    // Visibility -- every setting the catalog writes, describe reads back in
    // the shape its tool took, so an agent can check an edit, and send a
    // setting back unchanged, without guessing what the project holds.
    {
        Document document = Fixture();
        auto& video = document.sequence.tracks[0].clips;
        video[0].timeline_in = {0, 25};
        video[1].timeline_in = {10, 25};
        document.sequence.tracks.push_back({"01K30000000000000000000010",
                                            "audio",
                                            1,
                                            {{"01K30000000000000000000011",
                                              "01K30000000000000000000001",
                                              {100, 25},
                                              {10, 25},
                                              {0, 25}}}});
        DocumentClip caption;
        caption.id = "01K30000000000000000000021";
        caption.duration = {10, 25};
        caption.timeline_in = {0, 25};
        caption.include_audio = false;
        caption.caption_text = "Bonjour";
        document.sequence.tracks.push_back(
            {"01K30000000000000000000020", "caption", 2, {caption}});
        Project project = Project::FromDocument(document, "Describe");
        std::string error;
        std::string pathString;
        Check(CreatePortableProject(
                  (directory / "Describe.cutmachine-project").string(), project,
                  pathString, error),
              "describe fixture saves: " + error);
        std::string output;
        const auto call = [&](const std::string& tool,
                              const std::string& arguments) {
            Check(CallToolCommand(pathString, tool, arguments, output) == 0,
                  tool + " applies: " + output);
        };
        const auto described = [&]() {
            mcp_json::Value parsed;
            std::string parseError;
            Check(CallToolCommand(pathString, "describe", "{}", output) == 0 &&
                      mcp_json::Value::Parse(output, parsed, parseError) &&
                      parsed.Find("result"),
                  "describe reads the project: " + parseError);
            return parsed.Find("result") ? *parsed.Find("result")
                                         : mcp_json::Value::MakeObject();
        };
        const auto find = [](const mcp_json::Value& list, const Ulid& id) {
            for (const mcp_json::Value& item : list.AsArray())
                if (item.Find("id") && item.Find("id")->AsString() == id)
                    return item;
            return mcp_json::Value::MakeNull();
        };
        const auto clip = [&](const mcp_json::Value& description,
                              const Ulid& id) {
            for (const mcp_json::Value& track :
                 description.Find("timeline")->Find("tracks")->AsArray()) {
                const mcp_json::Value item = find(*track.Find("items"), id);
                if (!item.IsNull()) return item;
            }
            return mcp_json::Value::MakeNull();
        };
        const auto dumped = [](const mcp_json::Value* value) {
            return value ? value->Dump() : std::string("(absent)");
        };
        const auto list = [](const mcp_json::Value& description,
                             const char* key) {
            const mcp_json::Value* found = description.Find(key);
            Check(found && found->IsArray(),
                  std::string("describe has a '") + key + "' list");
            return found && found->IsArray() ? *found
                                             : mcp_json::Value::MakeArray();
        };

        const std::string audio =
            R"({"gain_db":{"num":-7,"den":2},"fade_in":{"value":1,"rate":3},)"
            R"("fade_out":{"value":5,"rate":25}})";
        const std::string opacity = R"({"num":3,"den":4})";
        const std::string effects =
            R"([{"type":"color.exposure","params":{"amount":{"num":1,"den":3}}}])";
        const std::string color =
            R"({"enabled":true,"input_gamut":"sony_sgamut3_cine",)"
            R"("input_transfer":"sony_slog3","input_ycbcr_matrix":"auto",)"
            R"("input_range":"limited","working_gamut":"acescct",)"
            R"("output_gamut":"rec709","output_transfer":"rec709"})";
        call("set_clip_audio",
             R"({"clip_id":"01K30000000000000000000011",)" + audio.substr(1));
        call("set_clip_opacity",
             R"({"clip_id":"01K30000000000000000000004","opacity":)" + opacity +
                 "}");
        call("set_clip_effects",
             R"({"clip_id":"01K30000000000000000000004","effects":)" + effects +
                 "}");
        call("add_transition",
             R"({"track_id":"01K30000000000000000000002",)"
             R"("left_clip_id":"01K30000000000000000000003",)"
             R"("right_clip_id":"01K30000000000000000000004",)"
             R"("type":"cross_dissolve","duration":{"value":6,"rate":25},)"
             R"("alignment":"StartAtCut"})");
        // R5 -- docs/roadmap/REEL-2026-09.md. The same tool, with one side
        // left out: a film that opens on a colour, over the first shot of
        // its track. The colour is read as the tool takes it.
        call("add_transition",
             R"({"track_id":"01K30000000000000000000002",)"
             R"("right_clip_id":"01K30000000000000000000003",)"
             R"("type":"dip_to_color","color":"#ffffff",)"
             R"("duration":{"value":6,"rate":25},)"
             R"("alignment":"StartAtCut"})");
        call("add_caption_style",
             R"({"font_family":"Inter","font_size":40,"color":"#ffcc00",)"
             R"("position":"top"})");
        call("set_track_sync_lock",
             R"({"track_id":"01K30000000000000000000010","sync_lock":false})");
        call("set_color_management", color);

        mcp_json::Value description = described();
        const mcp_json::Value styles = list(description, "caption_styles");
        Check(styles.AsArray().size() == 1 &&
                  dumped(styles.AsArray()[0].Find("font_family")) ==
                      "\"Inter\"" &&
                  dumped(styles.AsArray()[0].Find("font_size")) == "40" &&
                  dumped(styles.AsArray()[0].Find("color")) == "\"#ffcc00\"" &&
                  dumped(styles.AsArray()[0].Find("position")) == "\"top\"",
              "describe shows a caption style added through the catalog: " +
                  styles.Dump());
        const Ulid styleId = styles.AsArray().empty()
                                 ? Ulid()
                                 : styles.AsArray()[0].Find("id")->AsString();
        call("set_clip_caption",
             R"({"clip_id":"01K30000000000000000000021","caption_group_id":")" +
                 styleId + R"(","caption_text":"Bonjour"})");
        description = described();

        const mcp_json::Value sound =
            clip(description, "01K30000000000000000000011");
        Check(dumped(sound.Find("audio")) == audio,
              "describe shows gain and fades as set_clip_audio took them: " +
                  sound.Dump());
        const mcp_json::Value graded =
            clip(description, "01K30000000000000000000004");
        Check(dumped(graded.Find("opacity")) == opacity &&
                  dumped(graded.Find("effects")) == effects,
              "describe shows opacity and effects as their tools took them: " +
                  graded.Dump());
        const mcp_json::Value untouched =
            clip(description, "01K30000000000000000000003");
        Check(!untouched.Find("audio") && !untouched.Find("opacity") &&
                  !untouched.Find("effects"),
              "a clip with neutral settings carries no settings keys: " +
                  untouched.Dump());
        const mcp_json::Value captioned =
            clip(description, "01K30000000000000000000021");
        Check(dumped(captioned.Find("caption_group_id")) ==
                      "\"" + styleId + "\"" &&
                  dumped(captioned.Find("text")) == "\"Bonjour\"",
              "describe shows which caption style a caption uses: " +
                  captioned.Dump());
        const mcp_json::Value transitions = list(description, "transitions");
        Check(transitions.AsArray().size() == 2 &&
                  dumped(transitions.AsArray()[0].Find("left_clip_id")) ==
                      "\"01K30000000000000000000003\"" &&
                  dumped(transitions.AsArray()[0].Find("right_clip_id")) ==
                      "\"01K30000000000000000000004\"" &&
                  dumped(transitions.AsArray()[0].Find("type")) ==
                      "\"cross_dissolve\"" &&
                  dumped(transitions.AsArray()[0].Find("duration")) ==
                      R"({"value":6,"rate":25})" &&
                  dumped(transitions.AsArray()[0].Find("alignment")) ==
                      "\"StartAtCut\"" &&
                  transitions.AsArray()[0].Find("id"),
              "describe shows a transition with the ID update_transition "
              "needs: " +
                  transitions.Dump());
        // R5 -- and the edge dip beside it, with the side it has not and
        // the colour it dips through.
        const mcp_json::Value dip = transitions.AsArray().size() > 1
                                        ? transitions.AsArray()[1]
                                        : mcp_json::Value::MakeNull();
        Check(transitions.AsArray().size() == 2 &&
                  dumped(dip.Find("left_clip_id")) == "\"\"" &&
                  dumped(dip.Find("right_clip_id")) ==
                      "\"01K30000000000000000000003\"" &&
                  dumped(dip.Find("type")) == "\"dip_to_color\"" &&
                  dumped(dip.Find("color")) == "\"#ffffff\"",
              "describe shows an edge dip, its missing side and its "
              "colour: " +
                  transitions.Dump());
        const mcp_json::Value audioTrack =
            find(*description.Find("timeline")->Find("tracks"),
                 "01K30000000000000000000010");
        Check(dumped(audioTrack.Find("sync_lock")) == "false",
              "describe shows a track's sync lock: " + audioTrack.Dump());
        Check(dumped(description.Find("color_management")) == color,
              "describe shows color management as set_color_management "
              "took it");

        // What describe shows is accepted back verbatim, and changes nothing.
        Project before;
        Check(LoadStoredProject(pathString, before, error),
              "described project reloads: " + error);
        Check(sound.Find("audio") && !transitions.AsArray().empty(),
              "the settings to send back are described");
        if (!sound.Find("audio") || transitions.AsArray().empty()) return 1;
        call("set_clip_audio", R"({"clip_id":"01K30000000000000000000011",)" +
                                   sound.Find("audio")->Dump().substr(1));
        call("update_transition",
             R"({"transition_id":")" +
                 transitions.AsArray()[0].Find("id")->AsString() +
                 R"(","type":"cross_dissolve","duration":)" +
                 transitions.AsArray()[0].Find("duration")->Dump() +
                 R"(,"alignment":)" +
                 transitions.AsArray()[0].Find("alignment")->Dump() + "}");
        Project after;
        Check(LoadStoredProject(pathString, after, error) &&
                  after.MakeActiveDocument().SaveToString() ==
                      before.MakeActiveDocument().SaveToString(),
              "settings sent back as described leave the document unchanged");

        // F1.3 -- wheels and curves go through the same tool and come back
        // from describe as they were sent; a curve that goes back in x is
        // refused with its reason, and nothing is written.
        const std::string wheelsAndCurves =
            R"([{"type":"color.wheels","params":{"gain_b":{"num":-1,"den":5},)"
            R"("lift_r":{"num":1,"den":20}}},)"
            R"({"type":"color.curves","params":{"r_x0":{"num":0,"den":1},)"
            R"("r_x1":{"num":1,"den":1},"r_y0":{"num":1,"den":10},)"
            R"("r_y1":{"num":9,"den":10}}}])";
        call("set_clip_effects",
             R"({"clip_id":"01K30000000000000000000004","effects":)" +
                 wheelsAndCurves + "}");
        const mcp_json::Value wheeled =
            clip(described(), "01K30000000000000000000004");
        Check(dumped(wheeled.Find("effects")) == wheelsAndCurves,
              "describe shows wheels and curves as set_clip_effects took "
              "them: " +
                  wheeled.Dump());
        Project beforeRefusal;
        Check(LoadStoredProject(pathString, beforeRefusal, error),
              "graded project reloads: " + error);
        Check(CallToolCommand(
                  pathString, "set_clip_effects",
                  R"({"clip_id":"01K30000000000000000000004","effects":[)"
                  R"({"type":"color.curves","params":{)"
                  R"("g_x0":{"num":1,"den":2},"g_y0":{"num":0,"den":1},)"
                  R"("g_x1":{"num":1,"den":4},"g_y1":{"num":1,"den":1}}}]})",
                  output) != 0 &&
                  output.find("not increasing in x") != std::string::npos,
              "a curve that goes back in x is refused with its reason: " +
                  output);
        Project afterRefusal;
        Check(LoadStoredProject(pathString, afterRefusal, error) &&
                  afterRefusal.MakeActiveDocument().SaveToString() ==
                      beforeRefusal.MakeActiveDocument().SaveToString(),
              "the refused curve left the project unchanged");
    }

    // ADOBE-CLI-2026-09 -- a range preview is replayable on an inactive
    // timeline with the same IDs, and every call preserves project focus.
    {
        Project project = Project::FromDocument(Fixture(), "Source/Record CLI");
        DocumentSequence timeline = project.timelines.front();
        timeline.id = GenerateUlid();
        timeline.name = "Inactive range edits";
        for (auto& track : timeline.tracks) {
            track.id = GenerateUlid();
            for (auto& clip : track.clips) {
                clip.id = GenerateUlid();
            }
        }
        for (auto& marker : timeline.markers) {
            marker.id = GenerateUlid();
        }
        const Ulid timelineId = timeline.id;
        const Ulid trackId = timeline.tracks.front().id;
        project.timelines.push_back(std::move(timeline));
        std::string path;
        std::string error;
        std::string output;
        Check(CreatePortableProject((directory / "range-tools").string(),
                                    project, path, error),
              "range command fixture is a real two-timeline project");
        const std::string before = Read(path);
        const std::string arguments =
            "{\"timeline_id\":\"" + timelineId + "\",\"track_id\":\"" +
            trackId + "\",\"source_id\":\"" + project.sources.front().id +
            "\",\"source_in\":{\"value\":300,\"rate\":25},"
            "\"source_out\":{\"value\":310,\"rate\":25},"
            "\"timeline_in\":{\"value\":10,\"rate\":25},\"mode\":\"Insert\"";
        Check(CallToolCommand(path, "edit_source_range",
                              arguments + ",\"preview\":true}", output,
                              true) == 0 &&
                  Read(path) == before,
              "inactive Source range preview leaves canonical project bytes "
              "unchanged");
        mcp_json::Value preview;
        Check(mcp_json::Value::Parse(output, preview, error) &&
                  (preview.Find("result") != nullptr) &&
                  (preview.Find("result")->Find("operation") != nullptr),
              "CLI preview returns the canonical prepared operation");
        const auto* result = preview.Find("result");
        const auto* operation =
            (result != nullptr) ? result->Find("operation") : nullptr;
        if (operation != nullptr) {
            Operation parsed;
            EditError editError = EditError::None;
            Check(DeserializeOperation(operation->Dump(), parsed, editError,
                                       error),
                  "prepared Source range has the shared operation schema");
            Check(ApplyOperationCommand(path, operation->Dump(), output,
                                        timelineId) == 0,
                  "CLI replays the preview using its reserved clip IDs: " +
                      output);
            Project changed;
            Check(LoadStoredProject(path, changed, error),
                  "replay persists its exact edit");
            const Document document = changed.MakeDocument(timelineId);
            Check(document.sequence.tracks.front().clips.size() == 4 &&
                      document.sequence.tracks.front().clips[1].source_in ==
                          RationalTime{300, 25} &&
                      document.sequence.tracks.front().clips[1].duration ==
                          RationalTime{10, 25} &&
                      changed.active_timeline_id == project.active_timeline_id,
                  "Source insert splits the boundary and preserves project "
                  "focus");
            const std::string after = changed.SaveToString();
            const std::string undoArgs =
                "{\"timeline_id\":\"" + timelineId + "\"}";
            Check(
                CallToolCommand(path, "undo", undoArgs, output, true) == 0 &&
                    Read(path) == before &&
                    CallToolCommand(path, "redo", undoArgs, output, true) ==
                        0 &&
                    LoadStoredProject(path, changed, error) &&
                    changed.SaveToString() == after &&
                    CallToolCommand(path, "undo", undoArgs, output, true) == 0,
                "persisted Source undo/redo restores bytes and identical "
                "reserved IDs");
        }
        Check(CallToolCommand(path, "edit_source_range", arguments + "}",
                              output, true) == 0,
              "Source tool also applies directly through the file backend: " +
                  output);
        const std::string undoArgs = "{\"timeline_id\":\"" + timelineId + "\"}";
        Check(CallToolCommand(path, "undo", undoArgs, output, true) == 0 &&
                  Read(path) == before,
              "direct Source tool has one exact persisted undo");
        // PRD14 -- the same edit the way an agent writes it without the
        // skill: the rush as media_id, the mode in lower case, and the
        // track by its alias, which is read on the timeline named and not
        // on the active one.
        const std::string natural =
            "{\"timeline_id\":\"" + timelineId +
            "\",\"track_id\":\"T1\",\"media_id\":\"" +
            project.sources.front().id +
            "\",\"source_in\":{\"value\":300,\"rate\":25},"
            "\"source_out\":{\"value\":310,\"rate\":25},"
            "\"timeline_in\":{\"value\":10,\"rate\":25},\"mode\":\"insert\"}";
        Project laid;
        Check(CallToolCommand(path, "edit_source_range", natural, output,
                              true) == 0 &&
                  LoadStoredProject(path, laid, error) &&
                  laid.MakeDocument(timelineId)
                          .sequence.tracks.front()
                          .clips.size() == 4 &&
                  laid.MakeActiveDocument().SaveToString() ==
                      project.MakeActiveDocument().SaveToString(),
              "the Source tool takes media_id, a lower-case mode and the "
              "alias of the named timeline's track: " +
                  output);
        Check(CallToolCommand(path, "undo", undoArgs, output, true) == 0 &&
                  Read(path) == before,
              "and that edit undoes to the same bytes");
        const std::string rangeArgs = "{\"timeline_id\":\"" + timelineId +
                                      "\",\"track_ids\":[\"" + trackId +
                                      "\"],\"in\":{\"value\":8,\"rate\":25},"
                                      "\"out\":{\"value\":12,\"rate\":25}";
        Check(CallToolCommand(path, "delete_timeline_range",
                              rangeArgs + ",\"preview\":true}", output,
                              true) == 0 &&
                  Read(path) == before,
              "Record range preview writes no project changes");
        Check(
            CallToolCommand(path, "delete_timeline_range",
                            rangeArgs + ",\"ripple\":true}", output, true) == 0,
            "Record extraction applies through the file backend: " + output);
        Project changed;
        Check(LoadStoredProject(path, changed, error) &&
                  changed.MakeDocument(timelineId)
                          .sequence.tracks.front()
                          .clips.size() == 3 &&
                  changed.MakeDocument(timelineId)
                          .sequence.tracks.front()
                          .clips.back()
                          .timeline_in == RationalTime{16, 25} &&
                  changed.active_timeline_id == project.active_timeline_id,
              "Record extraction closes its exact duration without changing "
              "active timeline");
        Check(CallToolCommand(path, "undo", undoArgs, output, true) == 0 &&
                  Read(path) == before,
              "Record extraction undo restores the exact project bytes");
        Check(CallToolCommand(path, "delete_timeline_range", "{}", output,
                              true) != 0,
              "range tools obey the explicit-timeline policy");
        CheckFailureEnvelope(output, "range explicit-timeline policy",
                             "TimelineRequired");
    }

    // B2 -- exercise every headless command at its cheapest refusal point.
    // This catalog catches a new command that returns a non-zero status with
    // free text, a success-shaped object, or a missing stable code.
    // PRD20 -- that point is, for most, a project path where nothing exists:
    // each says so with the path, as IoError, where it used to give the
    // name of its own load failure and call the package unsupported.
    {
        const std::filesystem::path missingProject =
            directory / "absent.cutmachine.json";
        const std::filesystem::path missingManifest =
            directory / "absent-resolve.json";
        struct FailureCase {
            std::string name;
            std::string error;
            std::function<int(std::string&)> call;
        };
        const std::vector<FailureCase> cases = {
            {"create_project", "InvalidOperation",
             [&](std::string& output) {
                 return CreateProjectCommand(
                     (directory / "unused.cutmachine-project").string(), "",
                     output);
             }},
            {"transcribe_media", "IoError",
             [&](std::string& output) {
                 return TranscribeMediaCommand(missingProject.string(),
                                               {"missing-media"}, "", "auto",
                                               false, false, output);
             }},
            {"transcribe_timeline", "IoError",
             [&](std::string& output) {
                 return TranscribeTimelineCommand(missingProject.string(), "",
                                                  "auto", false, output);
             }},
            {"srt_from_media", "IoError",
             [&](std::string& output) {
                 return SrtFromMediaCommand(
                     (directory / "missing.wav").string(),
                     (directory / "unused.srt").string(), "auto", false,
                     output);
             }},
            {"analyze_shot_quality", "IoError",
             [&](std::string& output) {
                 return AnalyzeShotQualityCommand(missingProject.string(),
                                                  "missing-media", output);
             }},
            {"shot_quality_report", "IoError",
             [&](std::string& output) {
                 return ShotQualityReportCommand(missingProject.string(),
                                                 output);
             }},
            {"contact_sheet", "IoError",
             [&](std::string& output) {
                 return ContactSheetCommand(
                     missingProject.string(),
                     (directory / "unused-contact.jpg").string(), "", 24,
                     output);
             }},
            {"cut_sheet", "IoError",
             [&](std::string& output) {
                 return CutSheetCommand(
                     missingProject.string(),
                     (directory / "unused-cuts.jpg").string(), "", 24, output);
             }},
            {"align_transcripts", "IoError",
             [&](std::string& output) {
                 return AlignTranscriptsCommand(missingProject.string(), false,
                                                output);
             }},
            {"analyze_speech_onset", "IoError",
             [&](std::string& output) {
                 return AnalyzeSpeechOnsetCommand(missingProject.string(),
                                                  "missing-media", output);
             }},
            {"speech_onset_report", "IoError",
             [&](std::string& output) {
                 return SpeechOnsetReportCommand(missingProject.string(),
                                                 output);
             }},
            {"list_disfluencies", "IoError",
             [&](std::string& output) {
                 return ListDisfluenciesCommand(missingProject.string(),
                                                "missing-clip", output);
             }},
            {"remove_words", "IoError",
             [&](std::string& output) {
                 return RemoveWordsCommand(missingProject.string(),
                                           "missing-clip", "[]", output);
             }},
            {"tighten_pauses", "IoError",
             [&](std::string& output) {
                 return TightenPausesCommand(missingProject.string(),
                                             "missing-clip", 400, 6, output);
             }},
            {"trim_boundary_air", "IoError",
             [&](std::string& output) {
                 return TrimBoundaryAirCommand(missingProject.string(),
                                               "missing-clip", 3, 300, output);
             }},
            {"close_junction_air", "IoError",
             [&](std::string& output) {
                 return CloseJunctionAirCommand(missingProject.string(),
                                                "missing-left", "missing-right",
                                                3, 300, output);
             }},
            {"locate_source_frame", "IoError",
             [&](std::string& output) {
                 return LocateSourceFrameCommand(missingProject.string(),
                                                 "missing-media", 0, output);
             }},
            {"describe", "IoError",
             [&](std::string& output) {
                 return DescribeCommand(missingProject.string(), output);
             }},
            {"export_srt", "IoError",
             [&](std::string& output) {
                 return ExportSrtCommand(missingProject.string(),
                                         (directory / "unused.srt").string(),
                                         output);
             }},
            {"propose_sequence", "IoError",
             [&](std::string& output) {
                 return ProposeSequenceCommand(missingProject.string(), output);
             }},
            {"apply_operation", "ParseError",
             [&](std::string& output) {
                 return ApplyOperationCommand(missingProject.string(), "{}",
                                              output);
             }},
            {"apply_project_operation", "ParseError",
             [&](std::string& output) {
                 return ApplyProjectOperationCommand(missingProject.string(),
                                                     "{}", output);
             }},
            {"undo_project_operation", "IoError",
             [&](std::string& output) {
                 return UndoProjectOperationCommand(missingProject.string(),
                                                    output);
             }},
            {"redo_project_operation", "IoError",
             [&](std::string& output) {
                 return RedoProjectOperationCommand(missingProject.string(),
                                                    output);
             }},
            {"undo_operation", "IoError",
             [&](std::string& output) {
                 return UndoOperationCommand(missingProject.string(), output);
             }},
            {"redo_operation", "IoError",
             [&](std::string& output) {
                 return RedoOperationCommand(missingProject.string(), output);
             }},
            {"export", "IoError",
             [&](std::string& output) {
                 return ExportCommand(missingProject.string(), ExportSettings{},
                                      {}, nullptr, output);
             }},
            {"ingest", "IoError",
             [&](std::string& output) {
                 return IngestCommand(missingProject.string(),
                                      directory.string(), false, output);
             }},
            {"export_resolve_timeline", "IoError",
             [&](std::string& output) {
                 return ExportResolveTimelineCommand(missingProject.string(),
                                                     output);
             }},
            {"import_resolve", "IoError",
             [&](std::string& output) {
                 return ImportResolveCommand(missingProject.string(),
                                             missingManifest.string(), output);
             }},
        };
        // The cases refused for something else before the project is
        // looked at: no project argument, an operation or a manifest read
        // first.
        const std::set<std::string> refusedEarlier = {
            "create_project", "srt_from_media", "apply_operation",
            "apply_project_operation", "import_resolve"};
        for (const FailureCase& test : cases) {
            std::string output;
            Check(test.call(output) != 0, test.name + " is refused");
            const std::string detail =
                RefusalDetail(output, test.name, test.error);
            Check(refusedEarlier.count(test.name) != 0 ||
                      detail == MissingProjectMessage(missingProject.string()),
                  test.name +
                      " names the project path where nothing "
                      "exists: " +
                      detail);
        }
    }

    std::string error;
    std::string createdOutput;
    const std::filesystem::path createdPackage =
        directory / "Created.cutmachine-project";
    Check(CreateProjectCommand(createdPackage.string(), "Mission IA",
                               createdOutput) == 0,
          "headless project creation succeeds: " + createdOutput);
    const std::filesystem::path createdPath =
        createdPackage / "project.cutmachine.json";
    Project createdProject;
    Check(Project::Load(createdPath.string(), createdProject, error) &&
              createdProject.name == "Mission IA" &&
              createdProject.timelines.size() == 1,
          "headless project creation writes a valid package: " + error);
    // IA10 -- the created timeline can take a clip at once, and the result
    // names the tracks it starts with. PRD14 -- by their alias as well as
    // their ID, so the first insert copies no ULID.
    Check(createdProject.timelines.front().tracks.size() == 2 &&
              createdProject.timelines.front().tracks[0].kind == "video" &&
              createdProject.timelines.front().tracks[1].kind == "audio" &&
              createdOutput.find(
                  "\"tracks\":[{\"alias\":\"T1\",\"id\":\"" +
                  createdProject.timelines.front().tracks[0].id +
                  "\",\"kind\":\"video\",\"index\":0},{\"alias\":\"T2\","
                  "\"id\":\"" +
                  createdProject.timelines.front().tracks[1].id +
                  "\",\"kind\":\"audio\",\"index\":1}]") != std::string::npos,
          "the created timeline starts with a video and an audio track: " +
              createdOutput);
    // ITW7 -- a vertical project starts with a vertical timeline, and the
    // result names it so it can be addressed without list_timelines.
    const std::filesystem::path verticalPackage =
        directory / "Vertical.cutmachine-project";
    Check(CreateProjectCommand(verticalPackage.string(), "ITW", createdOutput,
                               "1080x1920@24000/1001") == 0,
          "headless project creation accepts a sequence format: " +
              createdOutput);
    Project verticalProject;
    Check(Project::Load((verticalPackage / "project.cutmachine.json").string(),
                        verticalProject, error) &&
              verticalProject.timelines.size() == 1 &&
              verticalProject.timelines[0].width == 1080 &&
              verticalProject.timelines[0].height == 1920 &&
              verticalProject.timelines[0].frame_rate.num == 24000 &&
              verticalProject.timelines[0].frame_rate.den == 1001 &&
              createdOutput.find("\"timeline_id\":\"" +
                                 verticalProject.timelines[0].id + "\"") !=
                  std::string::npos,
          "the only timeline has the requested format and is named: " +
              createdOutput);
    for (const std::string& invalid :
         {"1080x1920", "1080x1920@0", "-2x1920@25", "1080 x1920@25",
          "1080x1920@25/0", "1080x1920@25.5"}) {
        Check(
            CreateProjectCommand(
                (directory / "Invalid.cutmachine-project").string(), "X",
                createdOutput, invalid) == 1 &&
                !std::filesystem::exists(directory /
                                         "Invalid.cutmachine-project"),
            "an invalid project format is refused before writing: " + invalid);
    }
    const std::string createdBytes = Read(createdPath);
    Check(CreateProjectCommand(createdPackage.string(), "Replacement",
                               createdOutput) == 1,
          "headless project creation refuses an existing destination");
    Check(Read(createdPath) == createdBytes,
          "refused project creation preserves the existing package");
    Check(TranscribeMediaCommand(createdPath.string(),
                                 {"01K39999999999999999999999"}, "missing.bin",
                                 "fr", true, false, createdOutput) == 1 &&
              createdOutput.find("\"error\":\"UnknownMedia\"") !=
                  std::string::npos,
          "headless transcription rejects an unknown media identity");
    // QC-2026-09 A3 -- a batch is refused whole when one of its members is
    // not a media of this project: silently transcribing the rest would
    // leave a caller believing it had all of them.
    Check(TranscribeMediaCommand(createdPath.string(), {}, "missing.bin", "fr",
                                 true, false, createdOutput) == 1 &&
              createdOutput.find("at least one media_id") != std::string::npos,
          "headless transcription refuses an empty batch with a reason");
    Check(Read(createdPath) == createdBytes,
          "refused transcription leaves the project byte-identical");

    // AL4 -- the command line tells a profile it was not given from
    // `default` named, and the answer says which ran, on which weights, and
    // whether the engine chose it. No model runs: a rush measured mute is
    // skipped, and the timeline is served from its caches. The binary has
    // no test identity for the weights, so the hesitations setting names
    // other weights here, and an unnamed profile resolves to `default` on
    // the model (transcription_tests and mcp_tests cover the evaluated
    // weights).
    {
        const Ulid rush = "01K3A000000000000000000001";
        const Ulid timeline = "01K3A000000000000000000010";
        Document spoken;
        spoken.sequence.id = timeline;
        spoken.sources = {{rush, "voice.wav", {25, 1}, {1000, 25}}};
        LibraryMedia voice;
        voice.id = rush;
        voice.path = "voice.wav";
        voice.filename = "voice.wav";
        voice.codec = "pcm_s16le";
        voice.has_video = false;
        voice.orientation = "audio";
        voice.rate = {25, 1};
        voice.duration = {1000, 25};
        voice.has_audio = true;
        voice.audio_rate = 48000;
        voice.audio_channels = 1;
        voice.audio_level_measured = true;
        voice.audio_level = 200;
        spoken.library = {voice};
        spoken.sequence.tracks = {{"01K3A000000000000000000011",
                                   "audio",
                                   0,
                                   {{"01K3A000000000000000000012",
                                     rush,
                                     {0, 25},
                                     {100, 25},
                                     {0, 25}}}}};
        std::string spokenPath;
        Check(CreatePortableProject(
                  (directory / "Profile.cutmachine-project").string(),
                  Project::FromDocument(spoken, "Profile"), spokenPath, error),
              "profile package saves: " + error);
        const std::filesystem::path cache =
            std::filesystem::path(spokenPath).parent_path() / ".cutmachine";
        Project stored;
        TimelineAudioPlan plan;
        Check(Project::Load(spokenPath, stored, error) &&
                  BuildTimelineAudioPlan(stored.MakeDocument(timeline),
                                         spokenPath, plan, error),
              "profile timeline audio plan: " + error);
        for (const auto profile :
             {WhisperDecodingProfile::Default,
              WhisperDecodingProfile::FrenchHesitationsV1}) {
            Transcript reading;
            reading.media_id = plan.cache_identity;
            reading.whisper_model = "ggml-test.bin";
            reading.language = "fr";
            reading.verbatim = true;
            reading.decoding_profile = profile;
            reading.source_rate = {25, 1};
            reading.words = {{profile == WhisperDecodingProfile::Default
                                  ? "ordinaire"
                                  : "hésitant",
                              {0, 25},
                              {10, 25}}};
            Check(SaveAudioTranscript(TimelineTranscriptCachePath(
                                          cache, timeline, true, profile)
                                          .string(),
                                      reading, error),
                  "profile reading seeds: " + error);
        }
        const std::filesystem::path model = directory / "ggml-test.bin";
        Write(model, "not a model");
        const std::filesystem::path weights =
            directory / "ggml-hesitations.bin";
        Write(weights, "s075 stand-in");
        const std::vector<std::string> environment = {
            "CUTMACHINE_WHISPER_MODEL=" + model.string(),
            "CUTMACHINE_WHISPER_HESITATIONS_MODEL=" + weights.string(),
            "CUTMACHINE_ENV_FILE=" + (directory / "absent.env").string(),
            "CUTMACHINE_PROJECT_ARCHIVE=off", "CUTMACHINE_LOG=0"};
        const auto answer = [&](std::vector<std::string> arguments) {
            arguments.insert(arguments.begin() + 1, spokenPath);
            const CliRun run = RunCutmachineIn(environment, arguments);
            Check(run.exited && run.status == 0,
                  arguments[0] + " succeeds: " + run.output);
            return run.output;
        };
        const std::vector<std::string> frenchVerbatim = {"--language", "fr",
                                                         "--verbatim"};
        std::vector<std::string> timelineCall = {"--transcribe-timeline"};
        timelineCall.insert(timelineCall.end(), frenchVerbatim.begin(),
                            frenchVerbatim.end());
        std::string output = answer(timelineCall);
        Check(Contains(output,
                       "\"decoding_profile\":\"default\","
                       "\"decoding_profile_implicit\":true,"
                       "\"whisper_model\":\"ggml-test.bin\"") &&
                  Contains(output, "ordinaire"),
              "--transcribe-timeline without a profile lets the engine "
              "choose, on the model: " +
                  output);
        timelineCall.insert(timelineCall.end(),
                            {"--decoding-profile", "default"});
        output = answer(timelineCall);
        Check(Contains(output,
                       "\"decoding_profile\":\"default\","
                       "\"decoding_profile_implicit\":false") &&
                  Contains(output, "ordinaire"),
              "--decoding-profile default is a request: " + output);
        timelineCall.back() = "fr-hesitations-v1";
        output = answer(timelineCall);
        Check(Contains(output,
                       "\"decoding_profile\":\"fr-hesitations-v1\","
                       "\"decoding_profile_implicit\":false") &&
                  Contains(output, "hésitant"),
              "the profile named reads its own cache: " + output);
        output = answer({"--transcribe", rush, "", "fr", "--verbatim"});
        Check(Contains(output,
                       "\"decoding_profile\":\"default\","
                       "\"decoding_profile_implicit\":true") &&
                  Contains(output, "\"skipped_count\":1"),
              "--transcribe without a profile says so: " + output);
        output = answer({"--transcribe", rush, "", "fr", "--verbatim",
                         "--decoding-profile", "default"});
        Check(Contains(output,
                       "\"decoding_profile\":\"default\","
                       "\"decoding_profile_implicit\":false"),
              "--transcribe keeps default named: " + output);
        const CliRun setupRun = RunCutmachineIn(environment, {"--check-setup"});
        mcp_json::Value report;
        std::string parseError;
        const mcp_json::Value* hesitations = nullptr;
        if (mcp_json::Value::Parse(setupRun.output, report, parseError) &&
            report.Find("items") != nullptr &&
            report.Find("items")->IsArray()) {
            for (const mcp_json::Value& item :
                 report.Find("items")->AsArray()) {
                const mcp_json::Value* id = item.Find("id");
                if (id != nullptr && id->IsString() &&
                    id->AsString() == "whisper_hesitations_model") {
                    hesitations = &item;
                }
            }
        }
        Check(hesitations != nullptr &&
                  hesitations->Find("state")->AsString() == "incomplete" &&
                  !hesitations->Find("required")->AsBool() &&
                  Contains(hesitations->Dump(), "\"verified\":true") &&
                  Contains(hesitations->Dump(), "\"evaluated\":false"),
              "--check-setup says the hesitations weights are not the "
              "evaluated ones, without failing for it: " +
                  setupRun.output);
    }

    Project fixtureProject = Project::FromDocument(Fixture(), "CLI fixture");
    std::string fixturePath;
    Check(CreatePortableProject(
              (directory / "Fixture.cutmachine-project").string(),
              fixtureProject, fixturePath, error),
          "fixture package saves: " + error);
    const std::filesystem::path path = fixturePath;

    std::string firstDescription;
    std::string secondDescription;
    Check(DescribeCommand(path.string(), firstDescription) == 0,
          "describe succeeds");
    Check(DescribeCommand(path.string(), secondDescription) == 0,
          "second describe succeeds");
    Check(firstDescription == secondDescription, "describe is byte-stable");
    Check(firstDescription.find("\"alias\":\"A1\"") != std::string::npos &&
              firstDescription.find("\"alias\":\"A2\"") != std::string::npos,
          "describe emits stable per-track aliases");
    // PRD14 -- the track itself carries one, its rank in the listing: the
    // clips A1 and A2 are those of T1.
    Check(firstDescription.find(
              "\"tracks\":[{\"alias\":\"T1\",\"id\":"
              "\"01K30000000000000000000002\",\"kind\":\"video\"") !=
              std::string::npos,
          "describe names each track by an alias: " +
              firstDescription.substr(0, 400));
    Check(firstDescription.find("\"type\":\"gap\"") != std::string::npos,
          "describe emits holes");
    Check(firstDescription.find("\"frames\":") != std::string::npos &&
              firstDescription.find("\"seconds\":") != std::string::npos,
          "describe emits frames and decimal seconds");
    Check(firstDescription.find("\"markers\":[{\"alias\":\"K1\"") !=
                  std::string::npos &&
              firstDescription.find("\"name\":\"Premier raccord\"") !=
                  std::string::npos,
          "describe exposes stable marker IDs and aliases");
    Check(
        firstDescription.find("\"timelines\":[{\"id\":") != std::string::npos &&
            firstDescription.find("\"width\":1920,\"height\":1080,") !=
                std::string::npos &&
            firstDescription.find("\"frame_rate\":{\"num\":25,") !=
                std::string::npos &&
            firstDescription.find("\"active\":true") != std::string::npos,
        "describe exposes the active project's timeline summary");

    const std::string before = Read(path);
    const Operation trim = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    std::string result;
    Check(ApplyOperationCommand(path.string(), SerializeOperation(trim),
                                result) == 0,
          "valid apply-op succeeds: " + result);
    Check(result.find("{\"ok\":true,\"doc_hash\":\"") == 0,
          "valid apply-op returns a document hash");
    const std::string after = Read(path);
    Check(after != before, "valid apply-op changes the document bytes");

    EditLog log;
    EditError editError = EditError::None;
    std::string detail;
    Project appliedProject;
    Check(Project::Load(path.string(), appliedProject, detail),
          "applied project reloads: " + detail);
    const std::string activeTimelineLog = TimelineEditLogPathForProject(
        path.string(), appliedProject.active_timeline_id);
    Check(EditLog::Load(activeTimelineLog, log, editError, detail),
          "sidecar edit log loads: " + detail);
    Check(log.AppliedCount() == 1, "valid apply-op increments edit log");

    const Operation addBin =
        AddBinOperation{"01K30000000000000000000009", "Rushes CLI"};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(addBin),
                                result) == 0,
          "CLI creates a persistent bin through the same operation path");
    std::string withBinDescription;
    Check(DescribeCommand(path.string(), withBinDescription) == 0 &&
              withBinDescription.find("\"name\":\"Rushes CLI\"") !=
                  std::string::npos,
          "describe exposes bins created by apply-op");
    const std::string afterBin = Read(path);

    const std::string logBeforeRejection = Read(activeTimelineLog);
    const Operation rejected =
        RemoveClipOperation{"01K39999999999999999999999", {}};
    Check(ApplyOperationCommand(path.string(), SerializeOperation(rejected),
                                result) == 1,
          "refused apply-op returns status 1");
    Check(result.find("\"error\":\"UnknownClip\"") != std::string::npos,
          "refused apply-op returns the exact operation error name");
    Check(Read(path) == afterBin,
          "refused apply-op leaves document byte-identical");
    Check(Read(activeTimelineLog) == logBeforeRejection,
          "refused apply-op leaves edit log byte-identical");

    // DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. The stretch of Resolve's
    // Trim mode is the operation's own field, so --apply-op reaches it as
    // typed, before any tool: the first shot [5, 15) stretched to twenty
    // frames ripples the second from 20 to 30, where without the ripple it
    // would overlap it.
    {
        Project rippleProject = Project::FromDocument(Fixture(), "CLI ripple");
        std::string ripplePath;
        Check(CreatePortableProject(
                  (directory / "Ripple.cutmachine-project").string(),
                  rippleProject, ripplePath, error),
              "ripple fixture saves: " + error);
        const std::string plain =
            R"({"type":"RateStretch","clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","duration":{"value":20,"rate":25},)"
            R"("exact_tracks":[]})";
        Check(
            ApplyOperationCommand(ripplePath, plain, result) == 1 &&
                result.find("\"error\":\"Overlap\"") != std::string::npos,
            "--apply-op without the ripple overlaps the next shot: " + result);
        const std::string rippled =
            R"({"type":"RateStretch","clip_id":"01K30000000000000000000003",)"
            R"("edge":"Tail","duration":{"value":20,"rate":25},)"
            R"("ripple":true,"exact_tracks":[]})";
        Check(ApplyOperationCommand(ripplePath, rippled, result) == 0,
              "--apply-op with \"ripple\":true stretches: " + result);
        Project reloaded;
        Check(Project::Load(ripplePath, reloaded, error),
              "the rippled package reloads: " + error);
        const Document saved = reloaded.MakeActiveDocument();
        const DocumentClip* first =
            saved.FindClip("01K30000000000000000000003");
        const DocumentClip* second =
            saved.FindClip("01K30000000000000000000004");
        Check(first != nullptr && first->duration == RationalTime{20, 25} &&
                  first->speed.num == 1 && first->speed.den == 2 &&
                  second != nullptr &&
                  second->timeline_in == RationalTime{30, 25},
              "the first shot plays at 1/2 and the second moves ten frames");
    }

    // R1 -- docs/roadmap/REEL-2026-09.md. The engine first, the CLI next,
    // the MCP after: a framing posed by --apply-op is the same operation the
    // tool posts, and it reaches the saved package.
    {
        Document framedDocument = Fixture();
        framedDocument.sequence.width = 1080;
        framedDocument.sequence.height = 1920;
        LibraryMedia media;
        media.id = framedDocument.sources[0].id;
        media.path = framedDocument.sources[0].path;
        media.filename = "A.MP4";
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = 1920;
        media.height = 1080;
        media.rate = {25, 1};
        media.duration = {1000, 25};
        framedDocument.library.push_back(media);
        Project framedProject =
            Project::FromDocument(framedDocument, "CLI framing");
        std::string framedPath;
        Check(CreatePortableProject(
                  (directory / "Framing.cutmachine-project").string(),
                  framedProject, framedPath, error),
              "framing fixture saves: " + error);
        ClipFramingAssignment assignment;
        assignment.clip_id = "01K30000000000000000000003";
        assignment.framing.mode = FramingMode::Fill;
        assignment.framing.scale = {11, 10};
        assignment.framing.position_x = {-1, 8};
        const Operation framing = SetClipFramingOperation{{assignment}};
        Check(ApplyOperationCommand(framedPath, SerializeOperation(framing),
                                    result) == 0,
              "--apply-op poses a framing: " + result);
        Project reloaded;
        Check(Project::Load(framedPath, reloaded, error),
              "the framed package reloads: " + error);
        const Document saved = reloaded.MakeActiveDocument();
        const DocumentClip* clip = saved.FindClip("01K30000000000000000000003");
        Check(clip != nullptr && clip->framing.mode == FramingMode::Fill &&
                  clip->framing.scale.num == 11 &&
                  clip->framing.position_x.num == -1,
              "and the reopened project carries it");
        Check(!ClipHasFraming(*saved.FindClip("01K30000000000000000000004")),
              "while the clip nobody framed keeps the default");
        std::string framedDescription;
        Check(DescribeCommand(framedPath, framedDescription) == 0 &&
                  framedDescription.find("\"framing\":{\"mode\":\"fill\"") !=
                      std::string::npos,
              "describe reads the framing back as set_clip_framing takes it: " +
                  framedDescription);
        Check(ApplyOperationCommand(
                  framedPath,
                  R"({"type":"SetClipFraming","clips":[{"clip_id":)"
                  R"("01K30000000000000000000003","framing":{"mode":"fit",)"
                  R"("scale":{"num":1,"den":1},"position_x":{"num":0,"den":1},)"
                  R"("position_y":{"num":0,"den":1},)"
                  R"("crop_left":{"num":1,"den":2},)"
                  R"("crop_right":{"num":1,"den":2},)"
                  R"("crop_top":{"num":0,"den":1},)"
                  R"("crop_bottom":{"num":0,"den":1}}}]})",
                  result) == 1 &&
                  result.find("leave a picture") != std::string::npos,
              "and a framing that leaves no picture is refused by name: " +
                  result);

        // Out of lot -- the author, 29 October 2026. --apply-op takes the
        // operation the window's D and set_clip_enabled post, and the
        // reopened package carries the disabled clip. describe writes the
        // colour management's switch as "enabled" too: one more is the
        // clip's.
        const auto disabledKeys = [](const std::string& text) {
            size_t count = 0;
            for (size_t at = text.find("\"enabled\":false");
                 at != std::string::npos;
                 at = text.find("\"enabled\":false", at + 1))
                ++count;
            return count;
        };
        std::string enabledDescription;
        Check(DescribeCommand(framedPath, enabledDescription) == 0,
              "describe reads the package: " + enabledDescription);
        Check(ApplyOperationCommand(
                  framedPath,
                  R"({"type":"SetClipEnabled","clips":[{"clip_id":)"
                  R"("01K30000000000000000000004","enabled":false}]})",
                  result) == 0,
              "--apply-op disables a clip: " + result);
        Project disabledProject;
        Check(Project::Load(framedPath, disabledProject, error) &&
                  !disabledProject.MakeActiveDocument()
                       .FindClip("01K30000000000000000000004")
                       ->enabled,
              "and the reopened project carries it: " + error);
        std::string disabledDescription;
        Check(DescribeCommand(framedPath, disabledDescription) == 0 &&
                  disabledKeys(disabledDescription) ==
                      disabledKeys(enabledDescription) + 1,
              "describe says the clip is disabled: " + disabledDescription);
        Check(ApplyOperationCommand(
                  framedPath,
                  R"({"type":"SetClipEnabled","clips":[{"clip_id":)"
                  R"("01K30000000000000000000004","enabled":true}]})",
                  result) == 0,
              "--apply-op enables it again: " + result);

        // R2 -- docs/roadmap/REEL-2026-09.md. The engine first, the CLI
        // next, the MCP after: the keys posed by --apply-op are the same
        // operation the tool posts, they reach the saved package and
        // describe reads them back as set_clip_animation takes them.
        Check(ApplyOperationCommand(
                  framedPath,
                  R"({"type":"SetClipAnimation","clips":[{"clip_id":)"
                  R"("01K30000000000000000000003","animation":[{)"
                  R"("parameter":"framing.scale","keyframes":[)"
                  R"({"id":"01K30000000000000000000K01",)"
                  R"("time":{"value":0,"rate":25},)"
                  R"("value":{"num":1,"den":1},)"
                  R"("interpolation":"cubic-bezier",)"
                  R"("easing_x1":{"num":2,"den":5},)"
                  R"("easing_y1":{"num":0,"den":1},)"
                  R"("easing_x2":{"num":1,"den":5},)"
                  R"("easing_y2":{"num":1,"den":1}},)"
                  R"({"id":"01K30000000000000000000K02",)"
                  R"("time":{"value":12,"rate":25},)"
                  R"("value":{"num":23,"den":20},)"
                  R"("interpolation":"linear"}]}]}]})",
                  result) == 0,
              "--apply-op poses a zoom: " + result);
        Project animatedProject;
        Check(Project::Load(framedPath, animatedProject, error),
              "the animated package reloads: " + error);
        const Document animatedSaved = animatedProject.MakeActiveDocument();
        const DocumentClip* animatedClip =
            animatedSaved.FindClip("01K30000000000000000000003");
        Check(animatedClip != nullptr && ClipHasAnimation(*animatedClip) &&
                  animatedClip->animation[0].keyframes.size() == 2,
              "and the reopened project carries both keys");
        Check(!ClipHasAnimation(
                  *animatedSaved.FindClip("01K30000000000000000000004")),
              "while the clip nobody animated keeps none");
        std::string animatedDescription;
        Check(DescribeCommand(framedPath, animatedDescription) == 0 &&
                  animatedDescription.find(
                      "\"animation\":[{\"parameter\":\"framing.scale\"") !=
                      std::string::npos,
              "describe reads the keys back as set_clip_animation takes "
              "them: " +
                  animatedDescription);
        Check(ApplyOperationCommand(
                  framedPath,
                  R"({"type":"SetClipAnimation","clips":[{"clip_id":)"
                  R"("01K30000000000000000000003","animation":[{)"
                  R"("parameter":"framing.scale","keyframes":[)"
                  R"({"id":"01K30000000000000000000K03",)"
                  R"("time":{"value":0,"rate":25},)"
                  R"("value":{"num":1,"den":1},)"
                  R"("interpolation":"linear"}]}]}]})",
                  result) == 0,
              "and a single key is accepted (R2c): " + result);
    }

    // BENCH-2026-09 -- the journal on disk is read by scripts outside
    // CUTMACHINE. A two-line caption used to write a raw line feed there,
    // which a strict JSON reader (mcp_json, Python's json.loads) refuses.
    {
        Project captionProject =
            Project::FromDocument(Fixture(), "CLI captions");
        std::string captionPath;
        Check(CreatePortableProject(
                  (directory / "Captions.cutmachine-project").string(),
                  captionProject, captionPath, error),
              "caption fixture saves: " + error);
        AddCaptionStyleOperation style;
        style.style.id = "01K30000000000000000000021";
        const Operation caption = SetClipCaptionOperation{
            "01K30000000000000000000003", style.style.id,
            "Première ligne\nseconde ligne"};
        Check(ApplyOperationCommand(captionPath, SerializeOperation(style),
                                    result) == 0 &&
                  ApplyOperationCommand(
                      captionPath, SerializeOperation(caption), result) == 0,
              "a two-line caption applies through --apply-op: " + result);
        const std::string journal = Read(TimelineEditLogPathForProject(
            captionPath, captionProject.active_timeline_id));
        mcp_json::Value tree;
        std::string parseError;
        Check(journal.find("Première ligne\\nseconde ligne") !=
                      std::string::npos &&
                  mcp_json::Value::Parse(journal, tree, parseError),
              "the journal on disk is strict JSON: " + parseError);
    }

    // PL1 -- ROADMAP.md. An intervention is one undo step on disk too:
    // --apply-op takes a batch, and a client whose intervention spans
    // several tool calls names its start and its end.
    {
        Document interventionDocument = Fixture();
        interventionDocument.sequence.tracks.push_back(
            {"01K30000000000000000000030",
             "audio",
             1,
             {{"01K30000000000000000000031",
               "01K30000000000000000000001",
               {100, 25},
               {10, 25},
               {5, 25}}}});
        Project interventionProject =
            Project::FromDocument(interventionDocument, "CLI intervention");
        std::string interventionPath;
        Check(CreatePortableProject(
                  (directory / "Intervention.cutmachine-project").string(),
                  interventionProject, interventionPath, error),
              "intervention fixture saves: " + error);
        const Ulid timeline = interventionProject.active_timeline_id;
        const std::string journalPath =
            TimelineEditLogPathForProject(interventionPath, timeline);
        const std::string original = Read(interventionPath);
        const auto journalEntries = [&] {
            EditLog journal;
            EditError journalError = EditError::None;
            std::string journalMessage;
            if (!std::filesystem::exists(journalPath)) return size_t{0};
            EditLog::Load(journalPath, journal, journalError, journalMessage);
            return journal.AppliedCount();
        };

        const BatchOperation batch{
            "Resserrer la question 2",
            {InsertClipOperation{"01K30000000000000000000002",
                                 "01K30000000000000000000001",
                                 {400, 25},
                                 {10, 25},
                                 {40, 25}},
             TrimClipOperation{"01K30000000000000000000003",
                               TrimEdge::Tail,
                               {-2, 25},
                               std::nullopt},
             SetClipAudioOperation{
                 "01K30000000000000000000031", {-6, 1}, {0, 25}, {3, 25}}}};
        BatchOperation refused = batch;
        std::get<TrimClipOperation>(refused.operations[1]).clip_id =
            "01K39999999999999999999999";
        Check(
            ApplyOperationCommand(interventionPath, SerializeOperation(refused),
                                  result) == 1 &&
                result.find("\"error\":\"UnknownClip\"") != std::string::npos &&
                Read(interventionPath) == original && journalEntries() == 0,
            "a batch refused in its middle leaves project and journal "
            "untouched: " +
                result);
        Check(ApplyOperationCommand(interventionPath, SerializeOperation(batch),
                                    result) == 0 &&
                  journalEntries() == 1,
              "--apply-op applies a batch as one journal entry: " + result);
        const std::string batched = Read(interventionPath);
        mcp_json::Value tree;
        std::string parseError;
        Check(mcp_json::Value::Parse(Read(journalPath), tree, parseError),
              "the journal holding a batch is strict JSON: " + parseError);
        Check(CallToolCommand(interventionPath, "undo", "{}", result) == 0 &&
                  Read(interventionPath) == original,
              "one undo reverts the batch to the original bytes: " + result);
        Check(CallToolCommand(interventionPath, "redo", "{}", result) == 0 &&
                  Read(interventionPath) == batched,
              "one redo replays it");

        // Several tool calls, each its own process in real use.
        Check(CallToolCommand(interventionPath, "begin_intervention", "{}",
                              result) == 0,
              "begin_intervention answers: " + result);
        mcp_json::Value begun;
        const mcp_json::Value* resultObject =
            mcp_json::Value::Parse(result, begun, parseError)
                ? begun.Find("result")
                : nullptr;
        const mcp_json::Value* token =
            resultObject ? resultObject->Find("intervention") : nullptr;
        const std::string intervention =
            token && token->IsString() ? token->AsString() : std::string();
        Check(Read(interventionPath) == batched && journalEntries() == 1,
              "beginning an intervention writes nothing");
        const std::string trim =
            R"({"clip_id":"01K30000000000000000000004","edge":"Tail",)"
            R"("delta":{"value":-1,"rate":25}})";
        Check(
            CallToolCommand(interventionPath, "trim_clip", trim, result) == 0 &&
                CallToolCommand(interventionPath, "trim_clip", trim, result) ==
                    0 &&
                journalEntries() == 3,
            "two edits, two steps: " + result);
        const std::string edited = Read(interventionPath);
        mcp_json::Value endArguments = mcp_json::Value::MakeObject();
        endArguments.Set("intervention",
                         mcp_json::Value::MakeString(intervention));
        endArguments.Set("label", mcp_json::Value::MakeString("Raccourcir"));
        Check(CallToolCommand(interventionPath, "end_intervention",
                              endArguments.Dump(), result) == 0 &&
                  result.find("\"grouped\":2") != std::string::npos &&
                  journalEntries() == 2 && Read(interventionPath) == edited,
              "end_intervention folds the two steps, montage untouched: " +
                  result);
        Check(CallToolCommand(interventionPath, "undo", "{}", result) == 0 &&
                  Read(interventionPath) == batched,
              "one undo from the journal on disk reverts the intervention");
        Check(CallToolCommand(interventionPath, "undo", "{}", result) == 0 &&
                  Read(interventionPath) == original,
              "the next undo reverts the earlier batch");
        Check(CallToolCommand(interventionPath, "end_intervention",
                              endArguments.Dump(), result) == 1 &&
                  result.find("\"error\":\"InvalidOperation\"") !=
                      std::string::npos &&
                  journalEntries() == 0,
              "ending it again below its start is refused: " + result);

        // A project step in the middle -- here a rename -- changes the
        // document but not this timeline's history: it must not stop the
        // fold, and the fold must not undo it.
        Check(CallToolCommand(interventionPath, "begin_intervention", "{}",
                              result) == 0 &&
                  mcp_json::Value::Parse(result, begun, parseError),
              "a second intervention begins: " + result);
        resultObject = begun.Find("result");
        token = resultObject ? resultObject->Find("intervention") : nullptr;
        endArguments.Set("intervention", token && token->IsString()
                                             ? *token
                                             : mcp_json::Value::MakeString(""));
        const std::string rename = "{\"timeline_id\":\"" + timeline +
                                   "\",\"name\":\"Version courte\"}";
        Check(
            CallToolCommand(interventionPath, "trim_clip", trim, result) == 0 &&
                CallToolCommand(interventionPath, "rename_timeline", rename,
                                result) == 0 &&
                CallToolCommand(interventionPath, "trim_clip", trim, result) ==
                    0,
            "two trims around a rename: " + result);
        Check(CallToolCommand(interventionPath, "end_intervention",
                              endArguments.Dump(), result) == 0 &&
                  result.find("\"grouped\":2") != std::string::npos,
              "a rename during the intervention does not stop the fold: " +
                  result);
        Project renamed;
        Check(CallToolCommand(interventionPath, "undo", "{}", result) == 0 &&
                  LoadStoredProject(interventionPath, renamed, error) &&
                  renamed.MakeDocument(timeline)
                          .FindClip("01K30000000000000000000004")
                          ->duration == RationalTime{10, 25} &&
                  renamed.MakeDocument(timeline).sequence.name ==
                      "Version courte",
              "one undo reverts both trims and keeps the rename: " + result);
    }

    // PL2/PL3 -- ROADMAP.md. Edit plans from the CLI, on a timeline that is
    // not the active one, and the reads that prepare them. Nothing in the
    // package moves before a plan is applied.
    {
        Project plansProject = Project::FromDocument(Fixture(), "Plans");
        std::string plansPathString;
        Check(CreatePortableProject(
                  (directory / "Plans.cutmachine-project").string(),
                  plansProject, plansPathString, error),
              "plan fixture saves: " + error);
        const std::string plans = plansPathString;
        const std::filesystem::path package =
            std::filesystem::path(plans).parent_path();
        const auto packageBytes = [&] {
            std::vector<std::filesystem::path> paths;
            for (const auto& entry :
                 std::filesystem::recursive_directory_iterator(package)) {
                paths.push_back(entry.path());
            }
            std::sort(paths.begin(), paths.end());
            std::string all;
            for (const auto& path : paths) {
                all += path.lexically_relative(package).string() + '\n';
                if (std::filesystem::is_regular_file(path)) {
                    all += Read(path) + '\n';
                }
            }
            return all;
        };
        const auto member = [](const std::string& json,
                               std::initializer_list<const char*> path) {
            mcp_json::Value root;
            std::string parseError;
            if (!mcp_json::Value::Parse(json, root, parseError)) {
                return std::string();
            }
            const mcp_json::Value* value = &root;
            for (const char* key : path) {
                value = value->Find(key);
                if (value == nullptr) {
                    return std::string();
                }
            }
            return value->IsString() ? value->AsString() : value->Dump();
        };
        const auto timelineBytes = [&](const Ulid& timelineId) {
            Project loaded;
            std::string loadError;
            LoadStoredProject(plans, loaded, loadError);
            return loaded.MakeDocument(timelineId).SaveToString();
        };
        const auto activeTimeline = [&] {
            Project loaded;
            std::string loadError;
            LoadStoredProject(plans, loaded, loadError);
            return loaded.active_timeline_id;
        };
        const auto journalEntries = [&](const Ulid& timelineId) {
            const std::string path =
                plans + ".timeline-" + timelineId + ".editlog.json";
            EditLog journal;
            EditError journalError = EditError::None;
            std::string journalMessage;
            if (!std::filesystem::exists(path)) {
                return size_t{0};
            }
            EditLog::Load(path, journal, journalError, journalMessage);
            return journal.AppliedCount();
        };

        const Ulid active = plansProject.active_timeline_id;
        Check(CallToolCommand(plans, "clone_timeline",
                              R"({"source_timeline_id":")" + active +
                                  R"(","name":"Version B"})",
                              result) == 0,
              "a second timeline to plan on: " + result);
        mcp_json::Value cloned;
        std::string parseError;
        mcp_json::Value::Parse(result, cloned, parseError);
        const mcp_json::Value* createdIds =
            cloned.Find("result") != nullptr
                ? cloned.Find("result")->Find("created_timeline_ids")
                : nullptr;
        const Ulid other =
            createdIds != nullptr && !createdIds->AsArray().empty()
                ? createdIds->AsArray()[0].AsString()
                : Ulid();
        Check(!other.empty() && activeTimeline() == active,
              "the clone stays inactive");
        const std::string untouched = packageBytes();
        const std::string otherBefore = timelineBytes(other);

        // Reads of the inactive timeline.
        std::string read;
        Check(DescribeCommand(plans, read, other) == 0 &&
                  member(read, {"sequence", "id"}) == other,
              "--describe --timeline describes the timeline named: " +
                  read.substr(0, 200));
        Check(CallToolCommand(plans, "describe",
                              R"({"timeline_id":")" + other + R"("})",
                              read) == 0 &&
                  member(read, {"result", "sequence", "id"}) == other,
              "describe takes timeline_id: " + read.substr(0, 200));
        Check(TimelineStatsCommand(plans, read, other) == 0 &&
                  CallToolCommand(plans, "timeline_stats",
                                  R"({"timeline_id":")" + other + R"("})",
                                  read) == 0,
              "timeline_stats reads the timeline named: " + read);
        // OR4 -- docs/roadmap/DONNEES-2026-10.md. The check reads the
        // timeline named, as the MCP tool does, and judges it against the
        // default policy unless given one.
        {
            const int checked =
                CheckTimelineCommand(plans, other, "", "", read);
            Check((checked == 0 || checked == 1) &&
                      member(read, {"timeline_id"}) == other &&
                      member(read, {"policy", "name"}) == "default" &&
                      Contains(read, std::string("\"ok\":") +
                                         (checked == 0 ? "true" : "false")),
                  "--check-timeline judges the timeline named, ok set to "
                  "its verdict: " +
                      read);
            Check(CheckTimelineCommand(
                      plans, other,
                      R"({"name":"court","duration_max":{"value":0,"rate":1}})",
                      "", read) == 1 &&
                      member(read, {"policy", "name"}) == "court" &&
                      Contains(read, R"({"name":"duration","status":"failed")"),
                  "a policy given is the one judged against: " + read);
            CheckFailureEnvelope(
                [&] {
                    CheckTimelineCommand(plans, other, R"({"black":"no"})", "",
                                         read);
                    return read;
                }(),
                "--check-timeline with a policy it cannot read",
                "ValidationFailed");
            Check(Contains(read, "'policy' has unknown key 'black'"),
                  "the refusal names the key: " + read);
            CheckFailureEnvelope(
                [&] {
                    CheckTimelineCommand(plans, "01K3S999999999999999999999",
                                         "", "", read);
                    return read;
                }(),
                "--check-timeline of an unknown timeline", "UnknownSequence");
        }
        CheckFailureEnvelope(
            [&] {
                DescribeCommand(plans, read, "01K3S999999999999999999999");
                return read;
            }(),
            "--describe of an unknown timeline", "UnknownSequence");
        Check(packageBytes() == untouched && activeTimeline() == active,
              "reading another timeline changes no byte and activates "
              "nothing");

        // Prepare on the inactive timeline, aliases from one observation:
        // A2 is the second clip seen, even once A1 is gone and it rippled.
        const std::filesystem::path planFile = directory / "plan-b.json";
        const std::string planArguments =
            R"({"timeline_id":")" + other +
            R"(","label":"Version B resserrée","steps":[)"
            R"({"tool":"remove_clip","arguments":{"clip_id":"A1"}},)"
            R"({"tool":"split_clip","arguments":{"clip_id":"A2",)"
            R"("timeline_position":{"value":15,"rate":25}}}]})";
        std::string preview;
        Check(PrepareEditPlanCommand(plans, planArguments, planFile.string(),
                                     preview) == 0 &&
                  member(preview, {"status"}) == "prepared" &&
                  member(preview, {"timeline_id"}) == other &&
                  member(preview, {"plan_file"}) == planFile.string(),
              "--prepare-edit-plan prepares on the named timeline: " + preview);
        Check(packageBytes() == untouched && activeTimeline() == active &&
                  std::filesystem::exists(planFile),
              "preparing writes the plan file and no byte of the project");
        std::string createdInPreview;
        {
            mcp_json::Value parsed;
            mcp_json::Value::Parse(preview, parsed, parseError);
            const mcp_json::Value* delta = parsed.Find("delta");
            const mcp_json::Value* clips =
                delta != nullptr ? delta->Find("clips") : nullptr;
            if (clips != nullptr) {
                for (const mcp_json::Value& clip : clips->AsArray()) {
                    const mcp_json::Value* created = clip.Find("created");
                    if (created != nullptr && created->AsBool()) {
                        createdInPreview = member(clip.Dump(), {"clip_id"});
                    }
                }
            }
        }
        Check(!createdInPreview.empty(),
              "the preview names the clip the split creates: " + preview);
        const std::string planText = Read(planFile);
        std::string refusedOutput;
        Check(PrepareEditPlanCommand(plans, planArguments, planFile.string(),
                                     refusedOutput) != 0 &&
                  Read(planFile) == planText,
              "a plan file is never overwritten");
        CheckFailureEnvelope(refusedOutput, "a plan file that exists",
                             "ValidationFailed");
        Check(CallToolCommand(plans, "prepare_edit_plan", planArguments,
                              refusedOutput) != 0,
              "--call-tool keeps no plan between calls");
        CheckFailureEnvelope(refusedOutput, "prepare_edit_plan by --call-tool",
                             "UnsupportedOperation");

        std::string applied;
        Check(ApplyEditPlanCommand(plans, planFile.string(), applied) == 0 &&
                  member(applied, {"status"}) == "applied" &&
                  member(applied, {"doc_hash"}) ==
                      member(preview, {"result_doc_hash"}),
              "--apply-edit-plan lands where the preview said: " + applied);
        Project afterApply;
        LoadStoredProject(plans, afterApply, error);
        const Document planned = afterApply.MakeDocument(other);
        Check(planned.FindClip(createdInPreview) != nullptr &&
                  afterApply.active_timeline_id == active &&
                  journalEntries(other) == 1 && journalEntries(active) == 0,
              "the plan's identities, one journal entry on its timeline, "
              "and the active timeline left alone");
        Check(
            ApplyEditPlanCommand(plans, planFile.string(), refusedOutput) != 0,
            "an applied plan is stale for the timeline it moved");
        CheckFailureEnvelope(refusedOutput, "a plan applied twice",
                             "StaleRevision");
        Check(CallToolCommand(plans, "undo",
                              R"({"timeline_id":")" + other + R"("})",
                              read) == 0 &&
                  timelineBytes(other) == otherBefore,
              "one undo reverts the plan: " + read);

        // Stale: the timeline moves between preparation and application.
        const std::filesystem::path staleFile = directory / "plan-stale.json";
        Check(PrepareEditPlanCommand(
                  plans,
                  R"({"timeline_id":")" + other +
                      R"(","label":"Plus court","steps":[{"tool":"trim_clip",)"
                      R"("arguments":{"clip_id":"A2","edge":"Tail",)"
                      R"("delta":{"value":-2,"rate":25}}}]})",
                  staleFile.string(), read) == 0,
              "a second plan: " + read);
        Check(CallToolCommand(plans, "trim_clip",
                              R"({"timeline_id":")" + other +
                                  R"(","clip_id":"A1","edge":"Tail",)"
                                  R"("delta":{"value":-1,"rate":25}})",
                              read) == 0,
              "an edit lands meanwhile: " + read);
        const std::string moved = packageBytes();
        Check(ApplyEditPlanCommand(plans, staleFile.string(), refusedOutput) !=
                      0 &&
                  packageBytes() == moved,
              "a stale plan is refused and changes nothing");
        CheckFailureEnvelope(refusedOutput, "a stale plan", "StaleRevision");
        Check(member(refusedOutput, {"result", "base_doc_hash"}) ==
                      member(Read(staleFile), {"base_doc_hash"}) &&
                  !member(refusedOutput, {"result", "doc_hash"}).empty(),
              "the stale refusal names both revisions: " + refusedOutput);

        // Discard removes a plan file, and only a plan file.
        Check(DiscardEditPlanCommand(staleFile.string(), read) == 0 &&
                  member(read, {"status"}) == "discarded" &&
                  !std::filesystem::exists(staleFile),
              "--discard-edit-plan removes the plan: " + read);
        const std::filesystem::path notAPlan = directory / "not-a-plan.json";
        Write(notAPlan, "{\"ok\":true}");
        Check(DiscardEditPlanCommand(notAPlan.string(), refusedOutput) != 0 &&
                  std::filesystem::exists(notAPlan),
              "a file that is not a plan is never removed");
        CheckFailureEnvelope(refusedOutput, "discarding a stray file",
                             "ValidationFailed");
    }

    // QC-2026-09 A5 -- --apply-op is the CLI surface for synchronized ripple
    // removal; the undo command must restore every named track and the full
    // removed clip representation, not just its five timeline fields.
    Document syncFixture = Fixture();
    syncFixture.sequence.tracks.push_back({"01K30000000000000000000020",
                                           "audio",
                                           1,
                                           {{"01K30000000000000000000021",
                                             syncFixture.sources[0].id,
                                             {400, 25},
                                             {10, 25},
                                             {20, 25}}}});
    Project syncProject = Project::FromDocument(syncFixture, "CLI sync");
    std::string syncPathString;
    Check(
        CreatePortableProject((directory / "Sync.cutmachine-project").string(),
                              syncProject, syncPathString, error),
        "synchronized CLI fixture saves: " + error);
    const std::filesystem::path syncPath = syncPathString;
    const std::string syncBefore = Read(syncPath);
    const Operation synchronizedRemove = RemoveClipOperation{
        "01K30000000000000000000003", {"01K30000000000000000000020"}, {}};
    Check(ApplyOperationCommand(syncPath.string(),
                                SerializeOperation(synchronizedRemove),
                                result) == 0,
          "CLI synchronized remove applies: " + result);
    Project syncAfterProject;
    Check(Project::Load(syncPath.string(), syncAfterProject, error),
          "CLI synchronized result reloads: " + error);
    const Document syncAfter = syncAfterProject.MakeActiveDocument();
    Check(syncAfter.FindClip("01K30000000000000000000004")->timeline_in ==
                  RationalTime{10, 25} &&
              syncAfter.FindClip("01K30000000000000000000021")->timeline_in ==
                  RationalTime{10, 25},
          "CLI synchronized remove ripples the named track");
    Check(UndoOperationCommand(syncPath.string(), result) == 0 &&
              Read(syncPath) == syncBefore,
          "CLI synchronized undo restores canonical bytes");

    // IA9 -- ROADMAP.md. An edit result names the aligned words it took out
    // of the sound: --apply-op carries every one of them off, --remove-words
    // counts the words it was given as targeted.
    {
        const std::string rush = "01K30000000000000000000001";
        const std::string clipId = "01K30000000000000000000051";
        Document wordsFixture;
        wordsFixture.sources = {{rush, "folder/A.MP4", {25, 1}, {1000, 25}}};
        wordsFixture.sequence.tracks = {
            {"01K30000000000000000000050",
             "audio",
             0,
             {{clipId, rush, {0, 25}, {100, 25}, {0, 25}}}}};
        Project wordsProject = Project::FromDocument(wordsFixture, "CLI words");
        std::string wordsPath;
        Check(CreatePortableProject(
                  (directory / "Words.cutmachine-project").string(),
                  wordsProject, wordsPath, error),
              "word fixture saves: " + error);
        const std::filesystem::path cache =
            std::filesystem::path(wordsPath).parent_path() / ".cutmachine";
        Transcript whisper;
        whisper.media_id = rush;
        whisper.whisper_model = "ggml-large-v3.bin";
        whisper.language = "fr";
        whisper.source_rate = {25, 1};
        whisper.words = {{"Alors", {10, 25}, {20, 25}},
                         {"à", {30, 25}, {35, 25}},
                         {"demain", {36, 25}, {45, 25}},
                         {"matin", {50, 25}, {60, 25}}};
        std::filesystem::create_directories(cache / "transcripts");
        Check(SaveAudioTranscript(
                  (cache / "transcripts" / (rush + ".json")).string(), whisper,
                  error),
              "word fixture transcript saves: " + error);

        RemoveWordsOperation cut;
        cut.clip_id = clipId;
        cut.ranges = {{{30, 25}, {40, 25}}};
        Check(ApplyOperationCommand(wordsPath, SerializeOperation(cut),
                                    result) == 0 &&
                  result.find("\"cut_words\":{\"checked\":false") !=
                      std::string::npos &&
                  result.find("\"removed\":[{\"first_frame\":30,"
                              "\"end_frame\":40}]") != std::string::npos,
              "with Whisper words only, the cut is reported unchecked with "
              "its frames: " +
                  result);
        Check(UndoOperationCommand(wordsPath, result) == 0,
              "word fixture undo: " + result);

        std::filesystem::create_directories(cache / "word-alignments" / rush);
        std::ofstream manifest(cache / "word-alignments" / rush / "nemo.json",
                               std::ios::binary | std::ios::trunc);
        manifest
            << "{\"schema_version\":1,\"media_id\":\"" << rush
            << "\",\"source_rate\":{\"num\":25,\"den\":1},\"alignment\":"
               "{\"method\":\"nemo\",\"model\":\"stt_fr\"},\"processing\":"
               "{\"audio_duration_frames\":1000},\"words\":["
               "{\"text\":\"Alors\",\"frame_start\":10,\"frame_end\":20},"
               "{\"text\":\"à\",\"frame_start\":30,\"frame_end\":35},"
               "{\"text\":\"demain\",\"frame_start\":36,\"frame_end\":45},"
               "{\"text\":\"matin\",\"frame_start\":50,\"frame_end\":60}]}";
        manifest.close();

        Check(ApplyOperationCommand(wordsPath, SerializeOperation(cut),
                                    result) == 0 &&
                  result.find(
                      "\"word_timing\":\"nemo\",\"targeted\":[],\"carried\":["
                      "{\"word_index\":1,\"text\":\"à\",\"first_frame\":30,"
                      "\"end_frame\":35,\"whole\":true},{\"word_index\":2,"
                      "\"text\":\"demain\",\"first_frame\":36,\"end_frame\":45,"
                      "\"whole\":false}]") != std::string::npos,
              "--apply-op names a whole word and a word cut into as carried "
              "off: " +
                  result);
        Check(UndoOperationCommand(wordsPath, result) == 0,
              "aligned word fixture undo: " + result);

        Check(
            RemoveWordsCommand(wordsPath, clipId,
                               R"([{"start_word_index":1,"end_word_index":1}])",
                               result) == 0 &&
                result.find("\"targeted\":[{\"word_index\":1,\"text\":"
                            "\"à\"") != std::string::npos &&
                result.find("\"carried\":[]") != std::string::npos,
            "--remove-words counts the word it was given as targeted: " +
                result);

        const Operation bin =
            AddBinOperation{"01K30000000000000000000052", "Mots"};
        Check(ApplyOperationCommand(wordsPath, SerializeOperation(bin),
                                    result) == 0 &&
                  result.find("cut_words") == std::string::npos,
              "an edit that takes no sound reports no cut words: " + result);
    }

    // AX2 -- docs/roadmap/AX-2026-09.md. --align-words runs the MFA script
    // the environment names, with the rushes Whisper has read, and reports
    // what the engine reads afterwards. A stand-in script plays MFA here:
    // it checks its arguments and writes the report the real one writes.
    {
        const std::string spoken = "01K30000000000000000000061";
        const std::string silent = "01K30000000000000000000062";
        Document fixture;
        fixture.sources = {{spoken, "folder/Parole.MOV", {25, 1}, {1000, 25}},
                           {silent, "folder/Muet.MOV", {25, 1}, {1000, 25}}};
        fixture.sequence.tracks = {{"01K30000000000000000000063",
                                    "audio",
                                    0,
                                    {{"01K30000000000000000000064",
                                      spoken,
                                      {0, 25},
                                      {100, 25},
                                      {0, 25}}}}};
        Project alignProject = Project::FromDocument(fixture, "CLI NeMo");
        for (const DocumentSource& source : fixture.sources) {
            LibraryMedia media;
            media.id = source.id;
            media.path = source.path;
            media.filename =
                std::filesystem::path(source.path).filename().string();
            media.rate = {25, 1};
            media.duration = {1000, 25};
            // Probed as ingest leaves camera rushes: picture and sound for
            // the spoken one, picture alone for the silent one.
            media.has_audio = source.id == spoken;
            media.codec = "h264";
            media.width = 1080;
            media.height = 1920;
            media.orientation = "portrait";
            media.audio_rate = media.has_audio ? 48000 : 0;
            media.audio_channels = media.has_audio ? 2 : 0;
            alignProject.rushes.push_back(std::move(media));
        }
        std::string alignPath;
        Check(CreatePortableProject(
                  (directory / "Align.cutmachine-project").string(),
                  alignProject, alignPath, error),
              "NeMo fixture saves: " + error);
        const std::filesystem::path cache =
            std::filesystem::path(alignPath).parent_path() / ".cutmachine";
        Transcript whisper;
        whisper.media_id = spoken;
        whisper.whisper_model = "ggml-large-v3.bin";
        whisper.language = "fr";
        whisper.source_rate = {25, 1};
        whisper.words = {{"Bonjour", {10, 25}, {20, 25}}};
        std::filesystem::create_directories(cache / "transcripts");
        Check(SaveAudioTranscript(
                  (cache / "transcripts" / (spoken + ".json")).string(),
                  whisper, error),
              "MFA fixture transcript saves: " + error);

        const std::filesystem::path stub = directory / "mfa_stub.py";
        {
            std::ofstream script(stub);
            script << "import json, pathlib, sys\n"
                      "args = sys.argv[1:]\n"
                      "work = args.index('--work')\n"
                      "media = args[1:work]\n"
                      "if pathlib.Path(args[work + 1], '..', 'fail').exists() "
                      "or 'FAIL' in open(args[args.index('--text') + 1]).read()"
                      " if '--text' in args else False:\n"
                      "    sys.stderr.write('MFA stand-in failed on purpose')\n"
                      "    sys.exit(1)\n"
                      "report = {'aligned': [{'media_id': m, 'words': 1, "
                      "'path': ''} for m in media], 'skipped': [], "
                      "'uncovered_speech': [{'media_id': media[0], "
                      "'frames': [[30, 60]]}], 'mfa_ms': 7}\n"
                      "pathlib.Path(args[args.index('--report') + 1])"
                      ".write_text(json.dumps(report))\n";
        }
        ::setenv("CUTMACHINE_MFA_PYTHON", "/nowhere/python", 1);
        ::setenv("CUTMACHINE_MFA_ALIGNER", stub.c_str(), 1);
        ::setenv("CUTMACHINE_MFA_SCRIPT", stub.c_str(), 1);
        Check(AlignWordsCommand(alignPath, {}, "", result) != 0 &&
                  result.find("/nowhere/python") != std::string::npos &&
                  result.find("CUTMACHINE_MFA_PYTHON") != std::string::npos,
              "a missing MFA is named, with the variable to set: " + result);
        // PRD21 -- by its own name, as FfmpegNotFound is.
        Check(result.find("\"error\":\"MfaNotFound\"") != std::string::npos,
              "a missing MFA is the refusal MfaNotFound: " + result);

        ::setenv("CUTMACHINE_MFA_PYTHON", "/usr/bin/python3", 1);
        Check(AlignWordsCommand(alignPath, {}, "", result) == 0 &&
                  result.find("\"media_id\":\"" + spoken + "\"") !=
                      std::string::npos &&
                  result.find(silent) == std::string::npos &&
                  result.find("\"word_timing\":\"whisper\"") !=
                      std::string::npos &&
                  result.find("\"uncovered_speech\":[{") != std::string::npos,
              "without media_ids, the rushes Whisper read are aligned; the "
              "stand-in wrote no alignment, so the engine still reads "
              "Whisper's words and says so: " +
                  result);
        Check(AlignWordsCommand(alignPath, {silent}, "", result) == 0 &&
                  result.find("\"reason\":\"no_audio\"") != std::string::npos,
              "a rush with no sound is skipped, with its reason: " + result);
        Check(AlignWordsCommand(alignPath, {spoken},
                                "{\"" + spoken + "\":\"FAIL\"}", result) != 0 &&
                  result.find("failed on purpose") != std::string::npos,
              "the script's own error reaches the caller: " + result);
        Check(
            AlignWordsCommand(alignPath, {spoken},
                              "{\"" + silent + "\":\"texte\"}", result) != 0 &&
                result.find("ValidationFailed") != std::string::npos,
            "texts may only name a rush being aligned: " + result);
        ::unsetenv("CUTMACHINE_MFA_PYTHON");
        ::unsetenv("CUTMACHINE_MFA_ALIGNER");
        ::unsetenv("CUTMACHINE_MFA_SCRIPT");
    }

    // IA3 -- docs/roadmap/IA-2026-09.md. A selection by blocks, end to end
    // through --call-tool on a real package: the view and its revision, the
    // blocks, what a block leaves out, the timeline built from them, its
    // undo to the byte, and a selection read before an edit refused.
    {
        const std::string rush = "01K30000000000000000000071";
        const std::string clipId = "01K30000000000000000000073";
        std::string output;
        Document speech;
        speech.sources = {{rush, "folder/C.MP4", {25, 1}, {1000, 25}}};
        speech.sequence.tracks = {
            {"01K30000000000000000000072",
             "audio",
             0,
             {{clipId, rush, {0, 25}, {300, 25}, {0, 25}}}}};
        Project speechProject = Project::FromDocument(speech, "CLI blocks");
        std::string speechPath;
        Check(CreatePortableProject(
                  (directory / "Blocks.cutmachine-project").string(),
                  speechProject, speechPath, error),
              "block fixture saves: " + error);
        const std::filesystem::path cache =
            std::filesystem::path(speechPath).parent_path() / ".cutmachine";
        Transcript whisper;
        whisper.media_id = rush;
        whisper.whisper_model = "ggml-large-v3.bin";
        whisper.language = "fr";
        whisper.source_rate = {25, 1};
        // Three sentences; the third's "pareil" refers to the second.
        whisper.words = {{"Chaque", {10, 25}, {18, 25}},
                         {"matin,", {18, 25}, {28, 25}},
                         {"j'allume", {28, 25}, {38, 25}},
                         {"le", {40, 25}, {44, 25}},
                         {"four", {44, 25}, {60, 25}},
                         {"à", {60, 25}, {63, 25}},
                         {"bois", {63, 25}, {70, 25}},
                         {"dehors.", {70, 25}, {85, 25}},
                         {"Mon", {95, 25}, {102, 25}},
                         {"grand-père", {102, 25}, {112, 25}},
                         {"pétrissait", {112, 25}, {116, 25}},
                         {"toujours", {116, 25}, {119, 25}},
                         {"la", {119, 25}, {128, 25}},
                         {"pâte", {128, 25}, {134, 25}},
                         {"à", {134, 25}, {142, 25}},
                         {"la", {142, 25}, {146, 25}},
                         {"main.", {146, 25}, {160, 25}},
                         {"Alors", {166, 25}, {170, 25}},
                         {"nous,", {170, 25}, {178, 25}},
                         {"depuis", {178, 25}, {181, 25}},
                         {"le", {181, 25}, {186, 25}},
                         {"dernier", {186, 25}, {190, 25}},
                         {"hiver", {190, 25}, {202, 25}},
                         {"froid,", {202, 25}, {218, 25}},
                         {"on", {218, 25}, {224, 25}},
                         {"travaille", {224, 25}, {227, 25}},
                         {"pareil", {227, 25}, {231, 25}},
                         {"chaque", {231, 25}, {234, 25}},
                         {"semaine.", {234, 25}, {250, 25}}};
        std::filesystem::create_directories(cache / "transcripts");
        Check(SaveAudioTranscript(
                  (cache / "transcripts" / (rush + ".json")).string(), whisper,
                  error),
              "block fixture transcript saves: " + error);

        // The tool's own payload, out of the CLI's envelope.
        const auto parse = [&](const std::string& json) {
            mcp_json::Value value;
            std::string parseError;
            Check(mcp_json::Value::Parse(json, value, parseError),
                  "tool output parses: " + parseError);
            const mcp_json::Value* result = value.Find("result");
            return result != nullptr ? *result : mcp_json::Value::MakeNull();
        };
        const auto field = [](const mcp_json::Value& value, const char* key) {
            const mcp_json::Value* found = value.Find(key);
            return found != nullptr && found->IsString() ? found->AsString()
                                                         : std::string();
        };
        const auto time = [](const mcp_json::Value& value, const char* key) {
            const mcp_json::Value* found = value.Find(key);
            int64_t frames = -1;
            int64_t rate = 0;
            if (found != nullptr && found->Find("value") != nullptr &&
                found->Find("rate") != nullptr) {
                found->Find("value")->AsInt64(frames);
                found->Find("rate")->AsInt64(rate);
            }
            return RationalTime{frames, static_cast<int32_t>(rate)};
        };

        Check(CallToolCommand(speechPath, "get_timeline_transcript", "{}",
                              output) == 0,
              "the transcript view reads: " + output);
        const std::string revision =
            field(parse(output), "transcript_revision");
        Check(revision.size() == 16, "the view names its revision: " + output);

        Check(CallToolCommand(speechPath, "list_speech_blocks", "{}", output) ==
                  0,
              "the blocks read: " + output);
        const mcp_json::Value blockView = parse(output);
        Check(field(blockView, "transcript_revision") == revision,
              "blocks and spans share the revision");
        const mcp_json::Value* blocks = blockView.Find("blocks");
        Check(blocks != nullptr && blocks->IsArray() &&
                  blocks->AsArray().size() == 3,
              "three sentences, three blocks: " + output);
        if (blocks == nullptr || !blocks->IsArray() ||
            blocks->AsArray().size() != 3) {
            return 1;
        }
        const mcp_json::Value& second = blocks->AsArray()[1];
        const mcp_json::Value& third = blocks->AsArray()[2];
        Check(field(second, "text") ==
                      "Mon grand-père pétrissait toujours la pâte à la main." &&
                  field(third, "text").rfind("Alors nous,", 0) == 0,
              "each block is one whole sentence: " + output);
        Check(second.Find("opens_mid_sentence") == nullptr &&
                  third.Find("closes_mid_sentence") == nullptr,
              "whole sentences carry no mid-sentence flag");

        const std::string alone = R"({"transcript_revision":")" + revision +
                                  R"(","segments":[{"block_id":"B3"}]})";
        Check(CallToolCommand(speechPath, "inspect_speech_selection", alone,
                              output) == 0,
              "a selection is inspected: " + output);
        const mcp_json::Value inspected = parse(output);
        const mcp_json::Value* segments = inspected.Find("segments");
        const mcp_json::Value* before =
            segments != nullptr && segments->IsArray() &&
                    segments->AsArray().size() == 1
                ? segments->AsArray()[0].Find("before")
                : nullptr;
        const mcp_json::Value* kept =
            before != nullptr ? before->Find("kept") : nullptr;
        Check(
            before != nullptr && before->IsObject() &&
                field(*before, "text") == field(second, "text") &&
                kept != nullptr && kept->IsBool() && !kept->AsBool(),
            "B3 alone leaves the sentence its \"pareil\" refers to: " + output);

        const std::string beforeShort = Read(speechPath);
        Check(CallToolCommand(speechPath, "create_interview_short",
                              R"({"name":"Court","transcript_revision":")" +
                                  revision +
                                  R"(","segments":[{"block_id":"B2",)"
                                  R"("end_block_id":"B3"}]})",
                              output) == 0,
              "a timeline is built from a block run: " + output);
        Project built;
        Check(LoadStoredProject(speechPath, built, error) &&
                  built.timelines.size() == 2,
              "the short is a second timeline");
        const Document shortDocument =
            built.MakeDocument(built.timelines.back().id);
        const DocumentClip* shortClip = nullptr;
        for (const DocumentTrack& track : shortDocument.sequence.tracks) {
            if (track.kind == "audio" && !track.clips.empty()) {
                shortClip = &track.clips.front();
            }
        }
        const RationalTime secondIn = time(second, "source_in");
        const RationalTime thirdEnd =
            time(third, "source_in").add(time(third, "duration"));
        Check(shortClip != nullptr && shortClip->source_in == secondIn &&
                  shortClip->duration == thirdEnd.sub(secondIn),
              "the short plays exactly from B2's first word to B3's end");
        Check(UndoProjectOperationCommand(speechPath, output) == 0 &&
                  Read(speechPath) == beforeShort,
              "undoing the short restores the project to the byte: " + output);

        // Five frames off the head take no word (the first starts at frame
        // 10): the ids designate the same passages, the revision stands.
        const auto trimHead = [&](int64_t frames) {
            return CallToolCommand(speechPath, "trim_clip",
                                   R"({"clip_id":")" + clipId +
                                       R"(","edge":"Head","delta":{"value":)" +
                                       std::to_string(frames) +
                                       R"(,"rate":25}})",
                                   output) == 0;
        };
        Check(trimHead(5), "the head is trimmed: " + output);
        Check(CallToolCommand(speechPath, "get_timeline_transcript", "{}",
                              output) == 0 &&
                  field(parse(output), "transcript_revision") == revision,
              "an edit that moves no span leaves the revision alone");
        // Into the first sentence: B1 now opens on "j'allume", and a selection
        // read before names another view.
        Check(trimHead(20), "the head is trimmed into a word: " + output);
        Check(CallToolCommand(speechPath, "create_interview_short",
                              R"({"transcript_revision":")" + revision +
                                  R"(","segments":[{"block_id":"B2"}]})",
                              output) != 0,
              "a selection read before the trim is refused");
        CheckFailureEnvelope(output, "stale block selection", "StaleSelection");
        Check(LoadStoredProject(speechPath, built, error) &&
                  built.timelines.size() == 1,
              "and builds nothing");
    }

    // BENCH-2026-09 -- --trim-boundary-air and --close-junction-air resolve
    // their linked clips in Cli.cc. After a cut every fragment of the pair
    // keeps one link id; the whole group used to follow a trim on any one of
    // them, leaving holes. Only the fragment and its facing sound may move.
    {
        const std::string rush = "01K30000000000000000000061";
        const std::string videoTrack = "01K30000000000000000000062";
        const std::string audioTrack = "01K30000000000000000000063";
        const std::string pictureId = "01K30000000000000000000064";
        const std::string soundId = "01K30000000000000000000065";
        Document blockFixture;
        blockFixture.sources = {{rush, "folder/B.MP4", {25, 1}, {1000, 25}}};
        DocumentClip picture{pictureId, rush, {0, 25}, {200, 25}, {0, 25}};
        picture.link_group_id = "01K30000000000000000000066";
        DocumentClip sound = picture;
        sound.id = soundId;
        blockFixture.sequence.tracks = {{videoTrack, "video", 0, {picture}},
                                        {audioTrack, "audio", 1, {sound}}};
        Project blockProject = Project::FromDocument(blockFixture, "CLI block");
        std::string blockPath;
        Check(CreatePortableProject(
                  (directory / "Block.cutmachine-project").string(),
                  blockProject, blockPath, error),
              "block fixture saves: " + error);
        // Speech 0-40, room 40-60, speech 60-100, room 100-120, speech
        // 120-160, room 160-180, speech 180-200 (source frames).
        SpeechOnsetReport envelope;
        envelope.media_id = rush;
        envelope.windows_per_second = 50;
        envelope.decode_sample_rate = 16000;
        for (int phrase = 0; phrase < 3; ++phrase) {
            envelope.levels.insert(envelope.levels.end(), 80, 200000);
            envelope.levels.insert(envelope.levels.end(), 40, 20000);
        }
        envelope.levels.insert(envelope.levels.end(), 40, 200000);
        envelope.speech_level = SpeechLevelPercentile(envelope.levels, 90);
        envelope.noise_floor = SpeechLevelPercentile(envelope.levels, 5);
        Check(BuildSpeechGroups(envelope.levels, envelope.windows_per_second,
                                envelope.speech_level, envelope.noise_floor, {},
                                envelope.groups, error),
              "block envelope groups build: " + error);
        const std::filesystem::path onsetCache =
            std::filesystem::path(blockPath).parent_path() / ".cutmachine" /
            "speechonset";
        std::filesystem::create_directories(onsetCache);
        Write(onsetCache / (rush + ".json"), SerializeSpeechOnset(envelope));

        RemoveWordsOperation cut;
        cut.clip_id = pictureId;
        cut.ranges = {{{44, 25}, {52, 25}},
                      {{104, 25}, {112, 25}},
                      {{164, 25}, {172, 25}}};
        cut.linked_clip_ids = {soundId};
        Check(ApplyOperationCommand(blockPath, SerializeOperation(cut),
                                    result) == 0,
              "the block is cut into fragments: " + result);
        const auto fragmentAt =
            [&](const Document& document, const std::string& trackId,
                int64_t sourceFrame) -> const DocumentClip* {
            const DocumentTrack* track = document.FindTrack(trackId);
            if (track == nullptr) return nullptr;
            for (const DocumentClip& clip : track->clips)
                if (clip.source_in == RationalTime{sourceFrame, 25})
                    return &clip;
            return nullptr;
        };
        const auto plays = [&](const Document& document, int64_t sourceIn,
                               int64_t duration, int64_t timelineIn) {
            bool both = true;
            for (const std::string& trackId : {videoTrack, audioTrack}) {
                const DocumentClip* clip =
                    fragmentAt(document, trackId, sourceIn);
                both = both && clip != nullptr &&
                       clip->duration == RationalTime{duration, 25} &&
                       clip->timeline_in == RationalTime{timelineIn, 25};
            }
            return both;
        };
        Project fragmented;
        Check(LoadStoredProject(blockPath, fragmented, error),
              "the fragmented block loads: " + error);
        const Document fragments = fragmented.MakeActiveDocument();
        const DocumentClip* second = fragmentAt(fragments, videoTrack, 52);
        const DocumentClip* third = fragmentAt(fragments, videoTrack, 112);
        // Out of lot, 29 October 2026 -- each kept stretch is a pair of its
        // own, as a cut in the window leaves it; it used to keep the block's
        // group.
        Check(second != nullptr && third != nullptr &&
                  !second->link_group_id.empty() &&
                  !third->link_group_id.empty() &&
                  second->link_group_id != picture.link_group_id &&
                  third->link_group_id != picture.link_group_id &&
                  second->link_group_id != third->link_group_id,
              "each fragment is linked in a pair of its own");
        if (second != nullptr && third != nullptr) {
            const std::string secondId = second->id;
            const std::string thirdId = third->id;
            const std::string fragmentedBytes = Read(blockPath);

            Check(TrimBoundaryAirCommand(blockPath, secondId, 3, 300, result) ==
                          0 &&
                      result.find("\"applied\":true") != std::string::npos,
                  "--trim-boundary-air trims one fragment: " + result);
            Project trimmed;
            LoadStoredProject(blockPath, trimmed, error);
            const Document afterTrim = trimmed.MakeActiveDocument();
            Check(plays(afterTrim, 0, 44, 0) && plays(afterTrim, 57, 47, 44) &&
                      plays(afterTrim, 112, 52, 91) &&
                      plays(afterTrim, 172, 28, 143),
                  "--trim-boundary-air moves the fragment and its sound only");
            Check(UndoOperationCommand(blockPath, result) == 0 &&
                      Read(blockPath) == fragmentedBytes,
                  "--trim-boundary-air undoes byte for byte: " + result);

            Check(CloseJunctionAirCommand(blockPath, secondId, thirdId, 3, 300,
                                          result) == 0 &&
                      result.find("\"applied\":true") != std::string::npos,
                  "--close-junction-air closes one junction: " + result);
            Project closed;
            LoadStoredProject(blockPath, closed, error);
            const Document afterJunction = closed.MakeActiveDocument();
            Check(plays(afterJunction, 0, 44, 0) &&
                      plays(afterJunction, 52, 51, 44) &&
                      plays(afterJunction, 117, 47, 95) &&
                      plays(afterJunction, 172, 28, 142),
                  "--close-junction-air moves the two sides and their sound "
                  "only");
            Check(UndoOperationCommand(blockPath, result) == 0 &&
                      Read(blockPath) == fragmentedBytes,
                  "--close-junction-air undoes byte for byte: " + result);
        }
    }

    // BENCH-2026-09 (B3) -- a shot quality cache written by an older build
    // was reported as "no cached shot quality analysis". --shot-quality-report
    // and list_shot_quality (here through the project backend) now refuse,
    // name the cache, its version and the remedy, and re-analyse nothing; a
    // source with no cache at all is still merely "unanalyzed".
    {
        const std::string oldRush = "01K30000000000000000000071";
        const std::string newRush = "01K30000000000000000000072";
        Document qualityFixture;
        qualityFixture.sources = {
            {oldRush, "folder/C.MP4", {25, 1}, {1000, 25}},
            {newRush, "folder/D.MP4", {25, 1}, {1000, 25}}};
        for (const std::string& rush : {oldRush, newRush}) {
            LibraryMedia media;
            media.id = rush;
            media.path = rush == oldRush ? "folder/C.MP4" : "folder/D.MP4";
            media.filename = rush == oldRush ? "C.MP4" : "D.MP4";
            media.codec = "h264";
            media.width = 1920;
            media.height = 1080;
            media.orientation = "landscape";
            media.rate = {25, 1};
            media.duration = {1000, 25};
            qualityFixture.library.push_back(media);
        }
        qualityFixture.sequence.tracks = {{"01K30000000000000000000073",
                                           "video",
                                           0,
                                           {{"01K30000000000000000000074",
                                             oldRush,
                                             {0, 25},
                                             {50, 25},
                                             {0, 25}},
                                            {"01K30000000000000000000075",
                                             newRush,
                                             {0, 25},
                                             {50, 25},
                                             {50, 25}}}}};
        Project qualityProject =
            Project::FromDocument(qualityFixture, "CLI quality");
        std::string qualityPath;
        Check(CreatePortableProject(
                  (directory / "Quality.cutmachine-project").string(),
                  qualityProject, qualityPath, error),
              "quality fixture saves: " + error);
        const std::filesystem::path qualityCache =
            std::filesystem::path(qualityPath).parent_path() / ".cutmachine" /
            "shotquality";

        // Nothing analysed yet: a success that lists both clips as unknown.
        Check(ShotQualityReportCommand(qualityPath, result) == 0 &&
                  result.find("\"unanalyzed\":[{") != std::string::npos,
              "with no cache the report still succeeds: " + result);

        // The format an older build wrote: no histogram, version 2.
        std::filesystem::create_directories(qualityCache);
        const std::string versionTwo =
            "{\"version\":2,\"media_id\":\"" + oldRush +
            "\",\"samples_per_second\":4,\"analysis_width\":384,"
            "\"analysis_height\":216,\"median_sharpness\":4000,"
            "\"median_motion\":2000,\"sharpness\":[4000,4000],"
            "\"motion\":[0,2000]}";
        Write(qualityCache / (oldRush + ".json"), versionTwo);

        // The other rush analysed by this build, so the report has a source
        // to grade beside the one it cannot read.
        ShotQualityReport current;
        current.media_id = newRush;
        current.samples_per_second = 4;
        current.analysis_width = 384;
        current.analysis_height = 216;
        current.median_sharpness = 4000;
        current.median_motion = 2000;
        for (int64_t sample = 0; sample < 8; ++sample) {
            ShotQualitySample value;
            value.time = {sample, 4};
            value.sharpness = 4000;
            value.motion = sample == 0 ? 0 : 2000;
            current.samples.push_back(value);
        }
        Write(qualityCache / (newRush + ".json"),
              SerializeShotQuality(current));

        // The unreadable caches, as the report and the refusal both carry
        // them.
        const auto checkCaches = [&](const mcp_json::Value* caches,
                                     const std::string& label,
                                     const std::string& output) {
            Check(caches != nullptr && caches->IsArray() &&
                      caches->AsArray().size() == 1,
                  label + " lists the unreadable cache and only it: " + output);
            if (caches == nullptr || !caches->IsArray() ||
                caches->AsArray().size() != 1)
                return;
            const mcp_json::Value& cache = caches->AsArray()[0];
            int64_t found = 0;
            const mcp_json::Value* remedy = cache.Find("remedy");
            Check(cache.Find("source_id")->AsString() == oldRush &&
                      cache.Find("found_version")->AsInt64(found) &&
                      found == 2 &&
                      cache.Find("readable_versions")->Dump() == "[3,4]" &&
                      remedy != nullptr &&
                      remedy->Find("tool")->AsString() ==
                          "analyze_shot_quality" &&
                      remedy->Find("arguments")->Find("media_id")->AsString() ==
                          oldRush,
                  label + " is machine-readable: " + output);
        };
        // ROADMAP.md, "À trancher", B3, decided on 24 September 2026: the
        // timeline views answer. The unreadable cache is named apart, its
        // clip stays unknown with a reason that says why, and the source
        // this build reads is graded as usual.
        const auto checkReport = [&](const mcp_json::Value& report,
                                     const std::string& label,
                                     const std::string& output) {
            checkCaches(report.Find("unreadable_caches"), label, output);
            const std::string clips = report.Find("clips") != nullptr
                                          ? report.Find("clips")->Dump()
                                          : "";
            const std::string unknown = report.Find("unanalyzed") != nullptr
                                            ? report.Find("unanalyzed")->Dump()
                                            : "";
            Check(clips.find(newRush) != std::string::npos &&
                      clips.find(oldRush) == std::string::npos,
                  label + " grades the source it can read: " + output);
            Check(
                unknown.find(oldRush) != std::string::npos &&
                    unknown.find("cannot be read by this build") !=
                        std::string::npos &&
                    unknown.find("no cached shot quality") == std::string::npos,
                label +
                    " says the other clip's cache is unreadable, not "
                    "missing: " +
                    output);
        };
        Check(ShotQualityReportCommand(qualityPath, result) == 0,
              "--shot-quality-report answers despite an unreadable cache: " +
                  result);
        {
            mcp_json::Value report;
            std::string parseError;
            mcp_json::Value::Parse(result, report, parseError);
            checkReport(report, "--shot-quality-report", result);
        }
        Check(
            CallToolCommand(qualityPath, "list_shot_quality", "{}", result) ==
                0,
            "list_shot_quality answers despite an unreadable cache: " + result);
        {
            mcp_json::Value envelope;
            std::string parseError;
            mcp_json::Value::Parse(result, envelope, parseError);
            const mcp_json::Value* report = envelope.Find("result");
            Check(report != nullptr, "list_shot_quality has a result");
            if (report != nullptr) {
                checkReport(*report, "list_shot_quality", result);
            }
        }
        // One source asked about, one it cannot read: still a refusal.
        const auto checkRefusal = [&](const std::string& output,
                                      const std::string& label) {
            CheckFailureEnvelope(output, label, "UnreadableCache");
            mcp_json::Value envelope;
            std::string parseError;
            mcp_json::Value::Parse(output, envelope, parseError);
            const mcp_json::Value* detail = envelope.Find("detail");
            const mcp_json::Value* refusal = envelope.Find("result");
            Check(
                detail != nullptr &&
                    detail->AsString().find("version 2") != std::string::npos &&
                    detail->AsString().find("analyze_shot_quality") !=
                        std::string::npos,
                label +
                    " says which version it found and what to run: " + output);
            checkCaches(refusal != nullptr ? refusal->Find("unreadable_caches")
                                           : nullptr,
                        label, output);
        };
        Check(
            CallToolCommand(qualityPath, "list_stable_windows",
                            "{\"media_id\":\"" + oldRush + "\"}", result) != 0,
            "list_stable_windows refuses an unreadable cache");
        checkRefusal(result, "list_stable_windows");
        std::filesystem::remove(qualityCache / (newRush + ".json"));
        Check(CallToolCommand(qualityPath, "list_stable_windows",
                              "{\"media_id\":\"" + newRush + "\"}",
                              result) != 0 &&
                  result.find("\"error\":\"IoError\"") != std::string::npos &&
                  result.find("UnreadableCache") == std::string::npos,
              "a source never analysed is still reported as missing, not "
              "unreadable: " +
                  result);
        Check(Read(qualityCache / (oldRush + ".json")) == versionTwo &&
                  !std::filesystem::exists(qualityCache / (newRush + ".json")),
              "no analysis is re-run behind the caller's back");

        // 6 November 2026 -- a rush whose camera drifts at 75 px/s from end
        // to end: under a 60 px/s limit it has no window, and the answer
        // says how it moves instead of an empty list alone.
        ShotQualityReport drifting = current;
        drifting.has_global_motion = true;
        for (size_t sample = 1; sample < drifting.samples.size(); ++sample) {
            drifting.samples[sample].motion_blocks = 9;
            // 17 361 millionths of the width per 0.25 s is 75 px/s at 1080.
            drifting.samples[sample].translation_x = 17361;
        }
        Write(qualityCache / (newRush + ".json"),
              SerializeShotQuality(drifting));
        Check(
            CallToolCommand(
                qualityPath, "list_stable_windows",
                "{\"media_id\":\"" + newRush + "\",\"max_speed\":60}",
                result) == 0 &&
                result.find("\"windows\":[]") != std::string::npos &&
                result.find("\"median_speed\":75") != std::string::npos &&
                result.find("\"too_fast_percent\":100") != std::string::npos &&
                result.find("\"longest_still\":{\"value\":0") !=
                    std::string::npos &&
                result.find("\"measured_intervals\":7") != std::string::npos,
            "list_stable_windows describes a rush without a window: " + result);
    }

    // BENCH-2026-09 -- ROADMAP.md. generate_captions (through --call-tool,
    // the MCP dispatcher) and --export-srt name the word kept through an
    // inflection, the words the aligner put in a cut while the montage says
    // them, and the gap left between two cartons while a word is said.
    {
        // generate_captions measures widths with the ffmpeg on the PATH.
        const std::string ffmpegDirectory =
            std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string();
        const char* searchPath = std::getenv("PATH");
        setenv("PATH",
               (ffmpegDirectory + ':' +
                (searchPath != nullptr ? searchPath : "/usr/bin"))
                   .c_str(),
               1);

        const std::string rush = "01K30000000000000000000061";
        Document captions;
        captions.sequence.width = 1080;
        captions.sequence.height = 1920;
        captions.sequence.frame_rate = {25, 1};
        captions.sources = {{rush, "folder/B.MP4", {25, 1}, {1000, 25}}};
        // Rush [0, 100), [120, 160) and [170, 200): a hesitation and a pause
        // are cut out.
        captions.sequence.tracks = {
            {"01K30000000000000000000062",
             "audio",
             0,
             {{"01K30000000000000000000063", rush, {0, 25}, {100, 25}, {0, 25}},
              {"01K30000000000000000000064",
               rush,
               {120, 25},
               {40, 25},
               {100, 25}},
              {"01K30000000000000000000065",
               rush,
               {170, 25},
               {30, 25},
               {140, 25}}}}};
        Project captionProject =
            Project::FromDocument(captions, "CLI captions");
        std::string captionPath;
        Check(CreatePortableProject(
                  (directory / "HeardCaptions.cutmachine-project").string(),
                  captionProject, captionPath, error),
              "caption fixture saves: " + error);
        const std::filesystem::path cache =
            std::filesystem::path(captionPath).parent_path() / ".cutmachine";
        // Source words: Whisper's text, NeMo's frames. "tourne" lasts 0.8 s
        // so that it needs no silence: the gap after "Ensuite" stays a gap
        // (A32 grows a short carton back into the silence before it).
        const std::vector<std::tuple<std::string, int64_t, int64_t>> aligned = {
            {"Les", 0, 5},       {"couleurs", 6, 18},  {"sont", 19, 25},
            {"mélangé", 26, 47}, {"dans", 48, 55},     {"l'eau.", 56, 66},
            {"Ensuite", 70, 80}, {"vraiment", 81, 99}, {"euh", 101, 110},
            {"qui", 112, 118},   {"tourne", 121, 140}, {"doucement", 141, 158},
            {"à", 161, 169},     {"la", 170, 176},     {"fin.", 177, 195}};
        Transcript whisper;
        whisper.media_id = rush;
        whisper.whisper_model = "ggml-large-v3.bin";
        whisper.language = "fr";
        whisper.source_rate = {25, 1};
        std::string manifestWords;
        for (const auto& [text, start, end] : aligned) {
            // Whisper's own boundaries drift; the alignment corrects them.
            whisper.words.push_back({text, {start + 2, 25}, {end + 2, 25}});
            if (!manifestWords.empty()) {
                manifestWords += ',';
            }
            manifestWords += "{\"text\":\"" + text +
                             "\",\"frame_start\":" + std::to_string(start) +
                             ",\"frame_end\":" + std::to_string(end) + '}';
        }
        std::filesystem::create_directories(cache / "transcripts");
        Check(SaveAudioTranscript(
                  (cache / "transcripts" / (rush + ".json")).string(), whisper,
                  error),
              "caption fixture transcript saves: " + error);
        std::filesystem::create_directories(cache / "word-alignments" / rush);
        Write(cache / "word-alignments" / rush / "nemo.json",
              "{\"schema_version\":1,\"media_id\":\"" + rush +
                  "\",\"source_rate\":{\"num\":25,\"den\":1},\"alignment\":"
                  "{\"method\":\"nemo\",\"model\":\"stt_fr\"},\"processing\":"
                  "{\"audio_duration_frames\":1000},\"words\":[" +
                  manifestWords + "]}");

        // Both readings of the montage audio, at montage positions, as
        // transcribe_timeline caches them. Neither hears "vraiment"; both
        // write "mélangés" and hear "qui" and "à" at the junctions.
        Project stored;
        Check(Project::Load(captionPath, stored, error),
              "caption fixture reloads: " + error);
        const Document montage = stored.MakeActiveDocument();
        TimelineAudioPlan plan;
        Check(BuildTimelineAudioPlan(
                  montage, cache.parent_path() / "project.cutmachine.json",
                  plan, error),
              "caption fixture audio identity: " + error);
        std::filesystem::create_directories(cache / "timeline-transcripts");
        for (const bool verbatim : {false, true}) {
            Transcript reading;
            reading.media_id = plan.cache_identity;
            reading.whisper_model = "ggml-large-v3.bin";
            reading.language = "fr";
            reading.verbatim = verbatim;
            reading.source_rate = {25, 1};
            for (const auto& [text, frame] :
                 std::vector<std::pair<std::string, int64_t>>{
                     {"Les", 1},
                     {"couleurs", 7},
                     {"sont", 20},
                     {"mélangés", 27},
                     {"dans", 49},
                     {"l'eau.", 57},
                     {verbatim ? "Ensuite," : "Ensuite", 71},
                     {"qui", 99},
                     {"tourne", 102},
                     {"doucement", 122},
                     {"à", 139},
                     {"la", 141},
                     {"fin.", 148}}) {
                reading.words.push_back({text, {frame, 25}, {frame + 5, 25}});
            }
            Check(SaveAudioTranscript(TimelineTranscriptCachePath(
                                          cache, montage.sequence.id, verbatim)
                                          .string(),
                                      reading, error),
                  "caption fixture reading saves: " + error);
        }

        std::string generated;
        mcp_json::Value envelope;
        std::string parseError;
        Check(CallToolCommand(captionPath, "generate_captions",
                              "{\"preview\":true,\"max_characters\":20}",
                              generated) == 0 &&
                  mcp_json::Value::Parse(generated, envelope, parseError) &&
                  envelope.Find("result") != nullptr,
              "generate_captions previews the cartons: " + generated);
        const mcp_json::Value result = envelope.Find("result") != nullptr
                                           ? *envelope.Find("result")
                                           : mcp_json::Value::MakeObject();
        const mcp_json::Value* cartons = result.Find("cartons");
        const mcp_json::Value* heard = result.Find("word_check");
        const mcp_json::Value* gaps = result.Find("caption_gaps");
        std::string texts;
        int64_t alone = -1;
        if (cartons != nullptr && cartons->IsArray()) {
            for (size_t index = 0; index < cartons->AsArray().size(); ++index) {
                const std::string text =
                    cartons->AsArray()[index].Find("text")->AsString();
                if (text == "Ensuite") {
                    alone = static_cast<int64_t>(index);
                }
                texts += '[' + text + ']';
            }
        }
        Check(texts.find("mélangé") != std::string::npos &&
                  texts.find("vraiment") == std::string::npos &&
                  texts.find("qui") == std::string::npos,
              "the inflected word is captioned, the unheard word is not, "
              "and the word heard in a cut is not invented: " +
                  texts);
        Check(
            heard != nullptr && heard->Find("inflected_matches") != nullptr &&
                heard->Find("inflected_matches")
                        ->Dump()
                        .find(
                            "\"text\":\"mélangé\",\"heard_as\":\"mélangés\"") !=
                    std::string::npos,
            "the inflected match is reported: " + generated);
        const mcp_json::Value* cut =
            heard != nullptr ? heard->Find("cut_but_heard") : nullptr;
        Check(cut != nullptr && cut->IsArray() && cut->AsArray().size() == 2,
              "the two words aligned into a cut but heard are named, the "
              "cut hesitation is not: " +
                  generated);
        if (cut != nullptr && cut->IsArray() && cut->AsArray().size() == 2) {
            const mcp_json::Value& qui = cut->AsArray()[0];
            const mcp_json::Value& a = cut->AsArray()[1];
            int64_t quiCarton = 0;
            int64_t aCarton = -1;
            Check(qui.Find("text")->AsString() == "qui" &&
                      qui.Find("reason")->AsString() ==
                          "aligned_in_removed_range" &&
                      qui.Find("carton")->AsInt64(quiCarton) &&
                      quiCarton == -1 &&
                      qui.Find("heard_as")->Dump() == "[\"qui\",\"qui\"]",
                  "a word heard at a junction between two cartons has no "
                  "carton: " +
                      qui.Dump());
            Check(a.Find("text")->AsString() == "à" &&
                      a.Find("carton")->AsInt64(aCarton) && aCarton >= 0,
                  "a word heard at a junction inside a carton names it: " +
                      a.Dump());
        }
        Check(gaps != nullptr && gaps->IsArray() && gaps->AsArray().size() == 1,
              "one gap falls while a word is said: " + generated);
        if (gaps != nullptr && gaps->IsArray() && gaps->AsArray().size() == 1) {
            const mcp_json::Value& gap = gaps->AsArray()[0];
            int64_t after = -2;
            // AGENT-2026-09 A32 -- the lone carton, ten frames long, grew
            // into the silence after it to 0.8 s: the gap starts at 90.
            Check(gap.Find("after_carton")->AsInt64(after) && after == alone &&
                      gap.Find("timeline_in")->Dump() ==
                          "{\"value\":90,\"rate\":25}" &&
                      gap.Find("words")->Dump().find(
                          "\"text\":\"vraiment\",\"timeline_in\":{\"value\":"
                          "81,\"rate\":25},\"reason\":\"unheard\"") !=
                          std::string::npos &&
                      gap.Find("words")->Dump().find(
                          "\"text\":\"qui\",\"timeline_in\":{\"value\":100,"
                          "\"rate\":25},\"reason\":"
                          "\"aligned_in_removed_range\"") != std::string::npos,
                  "the gap after the lone carton names the unheard word and "
                  "the word heard at the junction: " +
                      gap.Dump());
        }

        std::string exported;
        Check(
            ExportSrtCommand(captionPath, (directory / "captions.srt").string(),
                             exported) == 0 &&
                exported.find("\"inflected_matches\":[{\"text\":"
                              "\"mélangé\"") != std::string::npos &&
                exported.find("\"cut_but_heard\":[{\"text\":\"qui\"") !=
                    std::string::npos &&
                exported.find("\"caption_gaps\":[{\"after_carton\":") !=
                    std::string::npos &&
                exported.find("\"reason\":\"unheard\"") != std::string::npos,
            "--export-srt carries the same report: " + exported);

        // RM3 -- docs/roadmap/REGLES-2026-10.md. The suites run with no
        // model (CUTMACHINE_MODELS=off): the rule cut the cartons above, and
        // says so. An installed model cuts them in its place, named by its
        // fingerprint; break_model "rule" still asks for the rule alone; an
        // installed file that does not read is said, and the rule decides.
        {
            const mcp_json::Value* cutBy = result.Find("break_model");
            Check(cutBy != nullptr && cutBy->Find("source") != nullptr &&
                      cutBy->Find("source")->AsString() == "rule",
                  "without a model, the rule cut the cartons: " + generated);
            const auto preview = [&](const std::string& arguments,
                                     std::string& source,
                                     std::string& cartonTexts) {
                std::string answer;
                mcp_json::Value parsed;
                std::string ignored;
                source.clear();
                cartonTexts.clear();
                if (CallToolCommand(captionPath, "generate_captions", arguments,
                                    answer) != 0 ||
                    !mcp_json::Value::Parse(answer, parsed, ignored) ||
                    parsed.Find("result") == nullptr)
                    return answer;
                const mcp_json::Value& body = *parsed.Find("result");
                if (const mcp_json::Value* by = body.Find("break_model"))
                    source = by->Dump();
                if (const mcp_json::Value* list = body.Find("cartons"))
                    for (const mcp_json::Value& carton : list->AsArray())
                        cartonTexts +=
                            '[' + carton.Find("text")->AsString() + ']';
                return answer;
            };
            const std::filesystem::path models = directory / "models";
            std::filesystem::create_directories(models);
            CaptionBreakModel model;
            // Every boundary far dearer than the rule's: fewer, longer
            // cartons. (Far cheaper would cut one word per carton, and the
            // short ones would join back into the rule's, A32.)
            model.weights["bias"] = 2000;
            CaptionBreakTrainingReport admitted;
            admitted.beats_rule = true;
            {
                std::ofstream file(models / "caption_breaks.json");
                file << SerializeCaptionBreakModel(model, admitted, {});
            }
            setenv("CUTMACHINE_MODELS", models.c_str(), 1);
            std::string source;
            std::string learnedTexts;
            const std::string learned =
                preview("{\"preview\":true,\"max_characters\":20}", source,
                        learnedTexts);
            Check(source.find("\"source\":\"model\"") != std::string::npos &&
                      source.find(CaptionBreakModelFingerprint(model)) !=
                          std::string::npos &&
                      !learnedTexts.empty() && learnedTexts != texts,
                  "an installed model cuts other cartons and is named by its "
                  "fingerprint: " +
                      learned + " / rule: " + texts);
            std::string ruleTexts;
            preview(
                "{\"preview\":true,\"max_characters\":20,"
                "\"break_model\":\"rule\"}",
                source, ruleTexts);
            Check(source.find("\"source\":\"rule\"") != std::string::npos &&
                      ruleTexts == texts,
                  "break_model rule asks for the rule alone: " + source);
            {
                std::ofstream file(models / "caption_breaks.json");
                file << "{not a model";
            }
            std::string brokenTexts;
            const std::string broken =
                preview("{\"preview\":true,\"max_characters\":20}", source,
                        brokenTexts);
            Check(source.find("\"source\":\"rule\"") != std::string::npos &&
                      source.find("model_error") != std::string::npos &&
                      brokenTexts == texts,
                  "an installed model that does not read is said, and the "
                  "rule decides: " +
                      broken);
            setenv("CUTMACHINE_MODELS", "off", 1);

            // DEC4 -- docs/roadmap/REGLES-2026-10.md. Without
            // max_characters, the cartons are the 20-character ones above
            // (caption_generation_tests shows 42 grouping more words).
            std::string defaultTexts;
            const std::string byDefault =
                preview("{\"preview\":true}", source, defaultTexts);
            Check(source.find("\"source\":\"rule\"") != std::string::npos &&
                      !defaultTexts.empty() && defaultTexts == texts,
                  "generate_captions cuts at 20 characters by default: " +
                      byDefault + " / at 20: " + texts);

            // Cartons grouped by the caller: kept as given, a short one
            // never joined to its neighbour, the source named; a word that
            // differs, or a pause of 0.7 s inside a carton, refused.
            std::string givenTexts;
            const std::string given = preview(
                "{\"preview\":true,\"max_characters\":20,\"cartons\":["
                "\"les couleurs sont\",\"mélangé dans l'eau.\","
                "\"ensuite\",\"tourne doucement\",\"la fin.\"]}",
                source, givenTexts);
            Check(source.find("\"source\":\"cartons\"") != std::string::npos &&
                      givenTexts ==
                          "[Les couleurs sont][mélangé dans l'eau.][Ensuite]"
                          "[tourne doucement][la fin.]",
                  "given cartons are kept as grouped, with the engine's "
                  "capitals: " +
                      given);
            std::string ignored;
            const std::string differs =
                preview("{\"preview\":true,\"cartons\":[\"les couleur sont\"]}",
                        source, ignored);
            Check(differs.find("'couleur' where the montage says 'couleurs' "
                               "(word 1)") != std::string::npos,
                  "a carton word that differs is refused by its index: " +
                      differs);
            const std::string paused = preview(
                "{\"preview\":true,\"cartons\":[\"les couleurs sont\","
                "\"mélangé dans l'eau.\",\"ensuite tourne\","
                "\"doucement la fin.\"]}",
                source, ignored);
            Check(paused.find("pause of 0.7 s") != std::string::npos,
                  "a carton across a long pause is refused: " + paused);

            // DEC4 -- applied without max_characters, the track is cut at
            // 20 characters, and the rule's motif in the journal says so.
            std::string applied;
            std::string motifs;
            Check(CallToolCommand(captionPath, "generate_captions", "{}",
                                  applied) == 0 &&
                      applied.find("\"track_id\"") != std::string::npos &&
                      CallToolCommand(captionPath, "list_motifs",
                                      "{\"rule\":\"generate_captions\"}",
                                      motifs) == 0 &&
                      motifs.find("\"max_characters\":20") != std::string::npos,
                  "the caption track applied by default records 20 "
                  "characters: " +
                      applied + " / " + motifs);

            // On a landscape timeline the default line is 32 characters.
            std::string resized;
            std::string wide;
            Check(CallToolCommand(captionPath, "update_sequence",
                                  "{\"width\":1920,\"height\":1080}",
                                  resized) == 0 &&
                      CallToolCommand(captionPath, "generate_captions", "{}",
                                      wide) == 0 &&
                      CallToolCommand(captionPath, "list_motifs",
                                      "{\"rule\":\"generate_captions\"}",
                                      motifs) == 0 &&
                      motifs.find("\"max_characters\":32") != std::string::npos,
                  "on a landscape timeline the caption track records 32 "
                  "characters by default: " +
                      resized + " / " + wide + " / " + motifs);
        }
    }

    Check(ApplyOperationCommand(path.string(), "{not json", result) == 1,
          "malformed operation returns status 1");
    Check(result.find("\"error\":\"ParseError\"") != std::string::npos,
          "malformed operation returns ParseError");
    Check(Read(path) == afterBin,
          "malformed operation leaves document byte-identical");

    Project project = Project::FromDocument(Fixture(), "CLI project");
    std::string projectPathString;
    Check(CreatePortableProject(
              (directory / "Project.cutmachine-project").string(), project,
              projectPathString, error),
          "project fixture saves: " + error);
    const std::filesystem::path projectPath = projectPathString;
    const std::string projectBefore = Read(projectPath);
    const ProjectOperation addTimeline =
        AddProjectTimelineOperation{"CLI vertical",
                                    1080,
                                    1920,
                                    {25, 1},
                                    "01K30000000000000000000010",
                                    "01K30000000000000000000011",
                                    "01K30000000000000000000012"};
    Check(ApplyProjectOperationCommand(projectPath.string(),
                                       SerializeProjectOperation(addTimeline),
                                       result) == 0,
          "valid apply-project-op succeeds: " + result);
    Check(result.find("{\"ok\":true,\"project_hash\":\"") == 0,
          "apply-project-op returns a project hash");
    const std::string projectAfter = Read(projectPath);
    Check(projectAfter != projectBefore,
          "apply-project-op changes project bytes");

    ProjectEditLog projectLog;
    Check(
        ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath.string()),
                             projectLog, editError, detail),
        "project edit log loads: " + detail);
    Check(projectLog.AppliedCount() == 1,
          "apply-project-op increments the project log");
    Check(UndoProjectOperationCommand(projectPath.string(), result) == 0,
          "undo-project-op succeeds: " + result);
    Check(Read(projectPath) == projectBefore,
          "CLI project undo restores byte-identical JSON");
    Check(RedoProjectOperationCommand(projectPath.string(), result) == 0,
          "redo-project-op succeeds: " + result);
    Check(Read(projectPath) == projectAfter,
          "CLI project redo restores deterministic JSON");

    const std::string projectLogBeforeRejection =
        Read(ProjectEditLogPathForProject(projectPath.string()));
    const ProjectOperation rejectedProject =
        RemoveProjectTimelineOperation{"01K39999999999999999999999"};
    Check(ApplyProjectOperationCommand(
              projectPath.string(), SerializeProjectOperation(rejectedProject),
              result) == 1,
          "refused project operation returns status 1");
    Check(result.find("\"error\":\"UnknownSequence\"") != std::string::npos,
          "refused project operation returns its exact error");
    Check(Read(projectPath) == projectAfter,
          "refused project operation leaves project byte-identical");
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeRejection,
          "refused project operation leaves project log byte-identical");

    // B10 -- ROADMAP.md, on the project journal. A timeline edit and a
    // project edit keep separate histories, and committing one must not
    // publish an empty version of the other: --apply-op and --undo used to
    // hand CommitStoredProjectAndLogs a default-constructed ProjectEditLog,
    // which erased every project undo step on each clip the agent moved.
    const std::string projectLogBeforeTimelineEdit =
        Read(ProjectEditLogPathForProject(projectPath.string()));
    const Operation timelineEditOnProject = TrimClipOperation{
        "01K30000000000000000000003", TrimEdge::Tail, {-1, 25}, std::nullopt};
    Check(ApplyOperationCommand(projectPath.string(),
                                SerializeOperation(timelineEditOnProject),
                                result) == 0,
          "timeline edit on a project with history succeeds: " + result);
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeTimelineEdit,
          "a timeline edit leaves the project journal byte-identical");
    Check(UndoOperationCommand(projectPath.string(), result) == 0,
          "undoing that timeline edit succeeds: " + result);
    Check(Read(ProjectEditLogPathForProject(projectPath.string())) ==
              projectLogBeforeTimelineEdit,
          "undoing a timeline edit leaves the project journal byte-identical");
    ProjectEditLog survivingProjectLog;
    Check(
        ProjectEditLog::Load(ProjectEditLogPathForProject(projectPath.string()),
                             survivingProjectLog, editError, detail) &&
            survivingProjectLog.AppliedCount() == 1,
        "project history survives timeline edits: " + detail);
    Check(UndoProjectOperationCommand(projectPath.string(), result) == 0,
          "project undo still reaches the step taken before the timeline "
          "edit: " +
              result);
    Check(RedoProjectOperationCommand(projectPath.string(), result) == 0,
          "project redo replays it: " + result);

    // B10 -- reproduce the measured project cardinality and the exact cleanup
    // shape. Corrupt journals are stronger than a timing-only assertion: an
    // eager reader would reject them, while the lazy project path must leave
    // every surviving byte untouched. Ten seconds is the product criterion,
    // not a microbenchmark threshold, and leaves ample room for loaded CI.
    Project largeProject("B10 performance fixture");
    for (int index = 1; index < 24; ++index) {
        DocumentSequence timeline;
        timeline.name = "Timeline " + std::to_string(index + 1);
        largeProject.timelines.push_back(std::move(timeline));
    }
    for (int index = 0; index < 210; ++index) {
        const Ulid mediaId = GenerateUlid();
        const std::string filename = "rush-" + std::to_string(index) + ".mov";
        LibraryMedia media;
        media.id = mediaId;
        media.path = filename;
        media.filename = filename;
        media.rate = {25, 1};
        media.duration = {250, 25};
        media.metadata_complete = false;
        largeProject.rushes.push_back(std::move(media));
        largeProject.sources.push_back({mediaId, filename, {25, 1}, {250, 25}});
    }
    std::string largeProjectPathString;
    Check(CreatePortableProject((directory / "B10.cutmachine-project").string(),
                                largeProject, largeProjectPathString, error),
          "B10 fixture saves: " + error);
    const std::filesystem::path largeProjectPath = largeProjectPathString;
    const std::string beforeLargeBatch = Read(largeProjectPath);

    std::map<Ulid, std::string> journalSentinels;
    for (const DocumentSequence& timeline : largeProject.timelines) {
        const std::string sentinel =
            "not a journal; must stay unread: " + timeline.id + "\n";
        journalSentinels.emplace(timeline.id, sentinel);
        Write(TimelineEditLogPathForProject(largeProjectPath.string(),
                                            timeline.id),
              sentinel);
    }
    std::vector<ProjectOperation> removals;
    for (size_t index = 9; index < largeProject.timelines.size(); ++index) {
        removals.push_back(
            RemoveProjectTimelineOperation{largeProject.timelines[index].id});
    }
    Check(removals.size() == 15,
          "B10 fixture contains exactly fifteen removals");
    const auto cleanupStart = std::chrono::steady_clock::now();
    Check(ApplyProjectOperationCommand(largeProjectPath.string(),
                                       ProjectBatchJson(removals), result) == 0,
          "fifteen project removals apply as one batch: " + result);
    const auto cleanupElapsed = std::chrono::steady_clock::now() - cleanupStart;
    Check(cleanupElapsed < std::chrono::seconds(10),
          "fifteen timeline removals stay below ten seconds");

    Project cleanedProject;
    Check(LoadStoredProject(largeProjectPath.string(), cleanedProject, error) &&
              cleanedProject.timelines.size() == 9,
          "B10 batch removes exactly fifteen timelines: " + error);
    ProjectEditLog batchProjectLog;
    Check(ProjectEditLog::Load(
              ProjectEditLogPathForProject(largeProjectPath.string()),
              batchProjectLog, editError, detail) &&
              batchProjectLog.AppliedCount() == 1,
          "B10 batch occupies one project undo entry: " + detail);
    for (const DocumentSequence& timeline : cleanedProject.timelines) {
        Check(Read(TimelineEditLogPathForProject(largeProjectPath.string(),
                                                 timeline.id)) ==
                  journalSentinels[timeline.id],
              "an untouched timeline journal remains byte-identical");
    }
    Check(UndoProjectOperationCommand(largeProjectPath.string(), result) == 0 &&
              Read(largeProjectPath) == beforeLargeBatch,
          "one project undo restores the entire fifteen-removal batch: " +
              result);
    Check(RedoProjectOperationCommand(largeProjectPath.string(), result) == 0,
          "one project redo reapplies the entire batch: " + result);

    const std::string beforeAtomicRefusal = Read(largeProjectPath);
    const std::string logBeforeAtomicRefusal =
        Read(ProjectEditLogPathForProject(largeProjectPath.string()));
    const Ulid survivingTimeline = cleanedProject.timelines.front().id;
    const std::vector<ProjectOperation> rejectedBatch = {
        RenameProjectItemOperation{survivingTimeline, "Must roll back"},
        RemoveProjectTimelineOperation{"01K39999999999999999999999"}};
    Check(ApplyProjectOperationCommand(largeProjectPath.string(),
                                       ProjectBatchJson(rejectedBatch),
                                       result) == 1,
          "a refused project batch returns status 1");
    CheckFailureEnvelope(result, "refused project batch", "UnknownSequence");
    Check(Read(largeProjectPath) == beforeAtomicRefusal &&
              Read(ProjectEditLogPathForProject(largeProjectPath.string())) ==
                  logBeforeAtomicRefusal,
          "a refused project batch is atomic on disk");

    Check(ApplyProjectOperationCommand(largeProjectPath.string(), "[]",
                                       result) == 1,
          "an empty project batch is refused");
    CheckFailureEnvelope(result, "empty project batch", "InvalidOperation");

    // F1.5 -- ROADMAP.md. Multicam from the command line: ids by prefix,
    // created ids in the result, one reversible step each, refusals named.
    {
        Document multicamFixture = Fixture();
        const Ulid wide = "01K30000000000000000000003";
        const Ulid close = "01K3M000000000000000000021";
        multicamFixture.sequence.tracks.push_back(
            {"01K3T000000000000000000020",
             "video",
             1,
             {{close,
               "01K30000000000000000000001",
               {300, 25},
               {10, 25},
               {5, 25}}}});
        Project multicamProject =
            Project::FromDocument(multicamFixture, "CLI multicam");
        std::string packagePath;
        Check(CreatePortableProject(
                  (directory / "Multicam.cutmachine-project").string(),
                  multicamProject, packagePath, error),
              "multicam package saves: " + error);
        const std::string multicamPath = packagePath;
        const std::string beforeGroup = Read(multicamPath);
        const auto run = [&](const std::string& command,
                             const std::vector<std::string>& arguments,
                             mcp_json::Value& parsed) {
            std::string output;
            const int status = MulticamCommand(command, arguments, output);
            std::string parseError;
            Check(mcp_json::Value::Parse(output, parsed, parseError) &&
                      parsed.IsObject(),
                  command + " prints one JSON object: " + output);
            return std::make_pair(status, output);
        };
        const auto hidden = [](const mcp_json::Value& result) {
            std::vector<std::string> ids;
            if (const mcp_json::Value* list = result.Find("hidden_clip_ids")) {
                for (const mcp_json::Value& id : list->AsArray()) {
                    ids.push_back(id.AsString());
                }
            }
            return ids;
        };

        mcp_json::Value added;
        const auto [addStatus, addOutput] =
            run("--add-multicam-group",
                {multicamPath, wide + ",01K3M", "--name", "Interview",
                 "--angle-name", "Large", "--angle-name", "Serré", "--active",
                 "01K3M"},
                added);
        const mcp_json::Value* groupField = added.Find("group_id");
        const mcp_json::Value* angleIds = added.Find("angle_ids");
        Check(addStatus == 0 && (added.Find("doc_hash") != nullptr) &&
                  (groupField != nullptr) && (angleIds != nullptr) &&
                  angleIds->AsArray().size() == 2 &&
                  added.Find("active_angle_id")->AsString() ==
                      angleIds->AsArray()[1].AsString() &&
                  hidden(added) == std::vector<std::string>{wide},
              "--add-multicam-group resolves a prefix and returns the ids it "
              "created: " +
                  addOutput);
        const std::string groupId =
            (groupField != nullptr) ? groupField->AsString() : "";

        mcp_json::Value listed;
        const auto [listStatus, listOutput] =
            run("--list-multicam-groups", {multicamPath}, listed);
        const mcp_json::Value* groups = listed.Find("groups");
        Check(
            listStatus == 0 && (groups != nullptr) &&
                groups->AsArray().size() == 1 &&
                groups->AsArray()[0].Find("name")->AsString() == "Interview" &&
                hidden(listed) == std::vector<std::string>{wide},
            "--list-multicam-groups shows the group and what it hides: " +
                listOutput);

        mcp_json::Value switched;
        const auto [setStatus, setOutput] =
            run("--set-multicam-angle", {multicamPath, wide}, switched);
        Check(
            setStatus == 0 &&
                hidden(switched) == std::vector<std::string>{close},
            "--set-multicam-angle finds the angle from its clip: " + setOutput);

        mcp_json::Value refused;
        const auto [noGroupStatus, noGroupOutput] =
            run("--set-multicam-angle", {multicamPath, "none"}, refused);
        Check(noGroupStatus == 1, "clearing without --group is refused");
        CheckFailureEnvelope(noGroupOutput, "clearing without --group",
                             "InvalidOperation");
        const auto [ambiguousStatus, ambiguousOutput] =
            run("--set-multicam-angle", {multicamPath, "01K3"}, refused);
        Check(ambiguousStatus == 1, "an ambiguous prefix is refused");
        CheckFailureEnvelope(ambiguousOutput, "ambiguous angle prefix",
                             "ValidationFailed");
        const auto [singleStatus, singleOutput] =
            run("--add-multicam-group", {multicamPath, wide}, refused);
        Check(singleStatus == 1, "a group of one clip is refused");
        CheckFailureEnvelope(singleOutput, "group of one clip",
                             "InvalidOperation");
        const auto [usageStatus, usageOutput] =
            run("--set-multicam-angle", {multicamPath, wide, "--active", wide},
                refused);
        Check(usageStatus == 2,
              "an option of another command is a usage error");
        CheckFailureEnvelope(usageOutput, "misplaced option",
                             "ValidationFailed");

        mcp_json::Value cleared;
        const auto [clearStatus, clearOutput] =
            run("--set-multicam-angle",
                {multicamPath, "none", "--group", groupId}, cleared);
        Check(clearStatus == 0 && hidden(cleared).empty() &&
                  cleared.Find("active_angle_id")->AsString().empty(),
              "none with --group clears the active angle: " + clearOutput);

        mcp_json::Value removed;
        const auto [removeStatus, removeOutput] =
            run("--remove-multicam-group", {multicamPath, groupId}, removed);
        Check(removeStatus == 0 && (removed.Find("group") != nullptr) &&
                  removed.Find("group")->IsNull(),
              "--remove-multicam-group dissolves the group: " + removeOutput);
        Check(Read(multicamPath) == beforeGroup,
              "dissolving leaves the project as it was before the group");
        std::string undone;
        for (int step = 0; step < 4; ++step) {
            Check(UndoOperationCommand(multicamPath, undone) == 0,
                  "multicam undo step: " + undone);
        }
        Check(Read(multicamPath) == beforeGroup,
              "each multicam command is one undo step of the timeline log");
    }

    // PRD20 -- ROADMAP.md. The table of commands (CliCommands.h): what the
    // text of --help, the help of one command and every refusal of a command
    // line are built from.
    {
        std::set<std::string> names;
        for (const CliCommand& command : CliCommands()) {
            const std::string name = command.name;
            Check(name.rfind("--", 0) == 0 && names.insert(name).second,
                  "each command is named once, with its two dashes: " + name);
            Check(std::string(command.summary).size() > 3,
                  name + " says what it does");
            Check(FindCliCommand(name) == &command &&
                      FindCliCommand(name.substr(2)) == &command,
                  name + " is found with or without its dashes");
            const std::string usage = CliUsageLine(command);
            Check(usage.rfind("usage: cutmachine " + name, 0) == 0 &&
                      !Contains(usage, "\n") && !Contains(usage, "  "),
                  name + " has a usage of one line: " + usage);
            Check(CliCommandHelpText(command).rfind(usage + "\n", 0) == 0,
                  "the help of " + name + " starts with its usage");
            Check(Contains(CliHelpText(), "\n  " + name),
                  "--help lists " + name);
        }
        Check(names.count("--version") == 1 && names.count("--help") == 1 &&
                  names.count("--describe") == 1 &&
                  names.count("--media-sheet") == 1 &&
                  names.count("--render-motion-frame") == 1 &&
                  names.count("--add-multicam-group") == 1,
              "the table holds the commands --help used not to list");
        Check(FindCliCommand("-h") == FindCliCommand("--help") &&
                  FindCliCommand("--frobnicate") == nullptr &&
                  FindCliCommand("") == nullptr &&
                  FindCliCommand("--") == nullptr,
              "-h is --help, and a name the table lacks is not found");
        Check(Contains(CliHelpText(), "docs/reference/commandes-headless.md") &&
                  !Contains(CliHelpText(), "Ligne de commande"),
              "--help names the reference file that exists");

        const CliCommand& exportCommand = *FindCliCommand("--export");
        const CliCommand& noArgument = *FindCliCommand("--check-setup");
        const CliCommand& describe = *FindCliCommand("--describe");
        const CliCommand& isolated = *FindCliCommand("--isolated-frames");
        const CliCommand& serve = *FindCliCommand("--mcp-serve");
        const CliCommand& cdl = *FindCliCommand("--apply-cdl");
        Check(
            CliRequiredArguments(exportCommand) ==
                    std::vector<std::string>{"<project>", "<file>"} &&
                CliRequiredArguments(noArgument).empty() &&
                CliRequiredArguments(serve).empty() &&
                CliRequiredArguments(*FindCliCommand("--apply-op")) ==
                    std::vector<std::string>{"<project>", "'<operation-json>'"},
            "the required arguments are the ones the usage starts with");

        Check(SuggestCliCommand("--decsribe") == "--describe" &&
                  SuggestCliCommand("--exprot") == "--export" &&
                  SuggestCliCommand("-describe") == "--describe" &&
                  SuggestCliCommand("--create_project") == "--create-project" &&
                  SuggestCliCommand("--Export") == "--export" &&
                  SuggestCliCommand("--transcibe-timline") ==
                      "--transcribe-timeline",
              "a slip of one letter, of dashes or of case is put right, and "
              "two letters in a long name");
        Check(SuggestCliCommand("--frobnicate").empty() &&
                  SuggestCliCommand("--export-xml").empty() &&
                  SuggestCliCommand("--xdo-project-op").empty() &&
                  SuggestCliCommand("--x").empty(),
              "another word, two letters of a short name or two commands "
              "as near as each other get no suggestion");

        Check(CliArgumentsMismatch(exportCommand, {}) ==
                      "--export is missing <project> <file>" &&
                  CliArgumentsMismatch(exportCommand, {"p"}) ==
                      "--export is missing <file>",
              "a missing argument is named as the usage names it");
        Check(CliArgumentsMismatch(describe, {"p", "--frob", "x"}) ==
                      "unknown --describe option '--frob'" &&
                  CliArgumentsMismatch(exportCommand, {"p", "f", "--frob"}) ==
                      "unknown --export option '--frob'",
              "an option the usage does not have is named");
        Check(
            CliArgumentsMismatch(describe, {"p", "--timeline"}) ==
                    "--describe option '--timeline' needs a value" &&
                CliArgumentsMismatch(exportCommand,
                                     {"p", "f", "--software", "--codec"}) ==
                    "--export option '--codec' needs a value" &&
                CliArgumentsMismatch(isolated, {"p", "--analyze", "--media"}) ==
                    "--isolated-frames option '--media' needs a value" &&
                CliArgumentsMismatch(serve, {"--stdio", "--port"}) ==
                    "--mcp-serve option '--port' needs a value" &&
                CliArgumentsMismatch(cdl, {"p", "c", "--preview", "--slope"}) ==
                    "--apply-cdl option '--slope' needs a value",
            "an option left without the value its usage gives it is "
            "named, and one that takes none is not asked for one");
        // PROFIL-2026-11 PF1 -- and when the usage admits nothing past its
        // required arguments but options with their values, as --tools now
        // does: "--describe p x" used to be told only that its arguments did
        // not fit.
        Check(
            CliArgumentsMismatch(noArgument, {"x"}) ==
                    "unexpected --check-setup argument 'x'" &&
                CliArgumentsMismatch(describe, {"p", "x"}) ==
                    "unexpected --describe argument 'x'" &&
                CliArgumentsMismatch(describe, {"p", "--timeline", "--frob"}) ==
                    "these arguments do not fit --describe",
            "an argument too many is named when the command takes a fixed "
            "number or options alone, and a value is never read as an option");

        // PRD39 -- ROADMAP.md. An option given twice, or left without its
        // value, is said to be so; the commands that read their own options
        // called both "unknown".
        Check(CliArgumentsMismatch(
                  describe, {"p", "--timeline", "a", "--timeline", "b"}) ==
                      "--describe option '--timeline' is given twice" &&
                  CliArgumentsMismatch(isolated, {"p", "--media", "a",
                                                  "--media", "b", "--frob"}) ==
                      "unknown --isolated-frames option '--frob'",
              "an option given twice is named, and one whose usage repeats "
              "it (\"...\") may be given again");
        const std::vector<std::string> exportArguments = {
            "p",       "out.mp4", "--codec", "h264",       "--software",
            "--codec", "hevc",    "--frob",  "--timeline", "--preset"};
        Check(CliOptionRefusal(exportCommand, exportArguments, 5) ==
                      "--export option '--codec' is given twice" &&
                  CliOptionRefusal(exportCommand, exportArguments, 7) ==
                      "unknown --export option '--frob'" &&
                  CliOptionRefusal(exportCommand, exportArguments, 9) ==
                      "--export option '--preset' needs a value" &&
                  CliOptionRefusal(exportCommand, exportArguments, 2) ==
                      "--export option '--codec' does not take 'h264'" &&
                  CliOptionRefusal(exportCommand, {"p", "out.mp4", "extra"},
                                   2) == "unexpected --export argument 'extra'",
              "a refused option is named for what it is: given twice, "
              "unknown, without its value, with a value it does not take; a "
              "word where an option goes is an argument too many");
        Check(CliOptionRefusal(
                  isolated, {"p", "--media", "a", "--media", "--analyze"}, 3) ==
                  "--isolated-frames option '--media' does not take "
                  "'--analyze'",
              "a repeatable option given again is not refused for it");

        // PRD39 -- "ok" in front of a report, and nothing else moved.
        Check(WithCliOk("{\"timeline\":{}}\n") ==
                      "{\"ok\":true,\"timeline\":{}}\n" &&
                  WithCliOk("{}") == "{\"ok\":true}" &&
                  WithCliOk("{\"ok\":false,\"error\":\"IoError\"}\n") ==
                      "{\"ok\":false,\"error\":\"IoError\"}\n" &&
                  WithCliOk("{\"ok\":true}") == "{\"ok\":true}" &&
                  WithCliOk("[1]") == "[1]" && WithCliOk("") == "",
              "WithCliOk puts ok:true in front of an object without one, and "
              "leaves anything else as it is");

        const auto preflight = [](const std::vector<std::string>& arguments) {
            return PreflightCommandLine(arguments);
        };
        using Action = CliPreflight::Action;
        Check(
            preflight({}).action == Action::kRun &&
                preflight({}).command == nullptr &&
                preflight({"Film.cutmachine-project"}).action == Action::kRun &&
                preflight({"Film.cutmachine-project"}).command == nullptr &&
                preflight({""}).action == Action::kRun,
            "no argument, or a project path, opens the editor");
        Check(preflight({"Film", "extra"}).action == Action::kRefuse &&
                  Contains(preflight({"Film", "extra"}).text,
                           "unexpected argument 'extra' after the project "
                           "path 'Film'"),
              "nothing follows a project path");
        Check(preflight({"--describe", "p"}).action == Action::kRun &&
                  preflight({"--describe", "p"}).command == &describe &&
                  preflight({"--describe", "p", "--frob"}).action ==
                      Action::kRun &&
                  preflight({"--mcp-serve"}).action == Action::kRun,
              "a command with what it requires is left to run: its options "
              "are its own to read");
        Check(preflight({"-describe", "p"}).action == Action::kRefuse &&
                  preflight({"describe", "p"}).action == Action::kRefuse &&
                  Contains(preflight({"-describe", "p"}).text,
                           "did you mean '--describe'?") &&
                  Contains(preflight({"describe", "p"}).text,
                           "unknown command 'describe', did you mean "
                           "'--describe'?"),
              "a command is typed as the table spells it");
        // A command typed without its dashes reads as a project path: the
        // editor opened its window on a project that is not there. Where
        // nothing bears that name it is refused as the command it meant;
        // a project that does bear it still opens. Told through `exists`,
        // so no test starts the editor to check it.
        const auto nothing = [](const std::string&) { return false; };
        const auto something = [](const std::string&) { return true; };
        Check(preflight({"version"}).action == Action::kRun &&
                  preflight({"version"}).command == nullptr,
              "a lone word is a project path as far as the table can tell");
        const CliPreflight bare = PreflightCommandLine({"version"}, nothing);
        Check(bare.action == Action::kRefuse &&
                  bare.text ==
                      "unknown command 'version', did you mean "
                      "'--version'? (cutmachine --help lists the "
                      "commands)" &&
                  PreflightCommandLine({"help"}, nothing).action ==
                      Action::kRefuse,
              "a command without its dashes, where nothing bears that name, "
              "is the command mistyped: " +
                  bare.text);
        Check(PreflightCommandLine({"version"}, something).action ==
                      Action::kRun &&
                  PreflightCommandLine({"Film.cutmachine-project"}, nothing)
                          .action == Action::kRun &&
                  PreflightCommandLine({"--version"}, nothing).action ==
                      Action::kRun,
              "a project that bears a command's name still opens, and so "
              "does a path that is no command's name");
        Check(preflight({"--mcp-serve", "--help"}).action == Action::kPrint &&
                  preflight({"--mcp-serve", "--help"}).text ==
                      CliCommandHelpText(serve) &&
                  preflight({"--mcp-serve", "p", "--stdio", "-h"}).action ==
                      Action::kPrint,
              "--help among a command's arguments prints its help instead "
              "of starting it");
    }

    // PRD20 -- the same, through the binary: main.mm reads the command line.
    {
        // Every command that requires an argument, started with none: each
        // answers with its own name and its own usage, never with a general
        // sentence. The commands that require nothing are valid as they
        // stand and are not started here (--check-setup, --mcp-serve).
        size_t started = 0;
        for (const CliCommand& command : CliCommands()) {
            const std::vector<std::string> required =
                CliRequiredArguments(command);
            if (required.empty()) {
                continue;
            }
            ++started;
            const std::string name = command.name;
            const std::string detail = UsageRefusal({name});
            std::string missing;
            for (const std::string& argument : required) {
                missing += " " + argument;
            }
            Check(detail == name + " is missing" + missing + "; " +
                                CliUsageLine(command),
                  name +
                      " without an argument names itself, what it lacks "
                      "and its usage: " +
                      detail);
        }
        Check(started > 45,
              "the table's commands were started: " + std::to_string(started));

        Check(UsageRefusal({"--frobnicate"}) ==
                  "unknown command '--frobnicate' (cutmachine --help lists "
                  "the commands)",
              "an unknown command is named");
        Check(UsageRefusal({"--decsribe", "p"}) ==
                  "unknown command '--decsribe', did you mean "
                  "'--describe'? (cutmachine --help lists the commands)",
              "a mistyped command is given the one it meant");
        Check(Contains(UsageRefusal({"Film.cutmachine-project", "--describe"}),
                       "unexpected argument '--describe' after the project "
                       "path 'Film.cutmachine-project'"),
              "an argument after a project path is refused, and the editor "
              "does not open");
        const auto usageOf = [](const char* name) {
            return "; " + CliUsageLine(*FindCliCommand(name));
        };
        // Refused by the table before anything runs.
        Check(UsageRefusal({"--tools", "extra"}) ==
                  "unexpected --tools argument 'extra'" + usageOf("--tools"),
              "an argument too many names it");
        Check(
            UsageRefusal({"--version", "extra"}) ==
                "unexpected --version argument 'extra'" + usageOf("--version"),
            "--version takes nothing");
        // Refused by the command's own reading of its options in main.mm.
        // PRD39 -- in the words of the table, which name the command as it
        // is typed: "unknown export option" until then.
        Check(UsageRefusal({"--export", "p", "out.mp4", "--frob"}) ==
                  "unknown --export option '--frob'" + usageOf("--export"),
              "an option main.mm refuses is named with its command and "
              "gains the usage");
        Check(UsageRefusal({"--check-timeline", "p", "--frob"}) ==
                  "unknown --check-timeline option '--frob'" +
                      usageOf("--check-timeline"),
              "--check-timeline names an option it does not take");
        Check(UsageRefusal({"--export", "p", "out.mp4", "--codec", "vp9"}) ==
                  "export codec must be h264 or hevc" + usageOf("--export"),
              "a value main.mm refuses gains the usage too");
        Check(UsageRefusal({"--transcribe-timeline", "p", "--frob"}) ==
                  "unknown --transcribe-timeline option '--frob'" +
                      usageOf("--transcribe-timeline"),
              "another command's refusal gains its own usage");
        Check(UsageRefusal({"--transcribe-timeline", "p", "--decoding-profile",
                            "typo"}) ==
                  "unknown Whisper decoding_profile 'typo'" +
                      usageOf("--transcribe-timeline"),
              "an unknown decoding profile is a CLI validation error");
        Check(UsageRefusal({"--transcribe", "p", "m", "--decoding-profile"}) ==
                  "--transcribe option '--decoding-profile' needs a value" +
                      usageOf("--transcribe"),
              "a missing profile cannot become a model path");
        Check(
            UsageRefusal({"--srt-from-media", "m.wav", "out.srt",
                          "--decoding-profile", "default", "--decoding-profile",
                          "default"}) ==
                "--srt-from-media option '--decoding-profile' is given twice" +
                    usageOf("--srt-from-media"),
            "duplicate profile options are refused on direct SRT too");

        // Refused where main.mm had no branch for the arguments: the end of
        // its chain, which used to answer "invalid command line".
        Check(UsageRefusal({"--describe", "p", "--frob", "x"}) ==
                  "unknown --describe option '--frob'" + usageOf("--describe"),
              "an option no branch takes is named");
        Check(UsageRefusal({"--create-project", "a", "b", "--format"}) ==
                  "--create-project option '--format' needs a value" +
                      usageOf("--create-project"),
              "a value left out is named");
        Check(UsageRefusal({"--tighten-pauses", "p", "c", "400"}) ==
                  "these arguments do not fit --tighten-pauses" +
                      usageOf("--tighten-pauses"),
              "arguments that fit no form of the command get its usage");
        Check(UsageRefusal({"--isolated-frames", "p", "--frob"}) ==
                  "unknown --isolated-frames option '--frob'" +
                      usageOf("--isolated-frames"),
              "--isolated-frames no longer falls through in silence");
        // Refused by a command that reads its arguments outside main.mm.
        Check(UsageRefusal({"--add-multicam-group", "p", "a,b", "--frob"}) ==
                  "unknown --add-multicam-group option '--frob'" +
                      usageOf("--add-multicam-group"),
              "a multicam refusal gains the usage");
        const std::string motion = UsageRefusal(
            {"--render-motion-video", "c.json", "out.mp4", "--frob"});
        Check(Contains(motion, "--frob") && Contains(motion, "usage: ") &&
                  motion.find("usage: ") == motion.rfind("usage: "),
              "a motion refusal gives a usage, once: " + motion);

        // PRD39 -- ROADMAP.md. An option given twice or left without its
        // value is said to be so, by every command that reads its own
        // options; and a motion command refuses its command line with
        // status 2, as every other command does, in the words and with the
        // usage of the table. Each of these exited 1, or was "unknown".
        const std::vector<std::pair<std::vector<std::string>, std::string>>
            refusals = {
                {{"--contact-sheet", "p", "out.jpg", "--timeline", "A",
                  "--timeline", "B"},
                 "--contact-sheet option '--timeline' is given twice"},
                {{"--cut-sheet", "p", "out.jpg", "--timeline"},
                 "--cut-sheet option '--timeline' needs a value"},
                {{"--media-sheet", "p", "out.jpg", "--media", "M",
                  "--max-images"},
                 "--media-sheet option '--max-images' needs a value"},
                {{"--media-sheet", "p", "out.jpg", "extra"},
                 "unexpected --media-sheet argument 'extra'"},
                {{"--export", "p", "out.mp4", "--codec", "h264", "--codec",
                  "hevc"},
                 "--export option '--codec' is given twice"},
                {{"--export", "p", "out.mp4", "--preset"},
                 "--export option '--preset' needs a value"},
                {{"--export", "p", "out.mp4", "--timeline", "A", "--timeline",
                  "B"},
                 "--export option '--timeline' is given twice"},
                {{"--transcribe-timeline", "p", "--timeline", "A", "--timeline",
                  "B"},
                 "--transcribe-timeline option '--timeline' is given twice"},
                {{"--isolated-frames", "p", "--timeline", "A", "--timeline",
                  "B"},
                 "--isolated-frames option '--timeline' is given twice"},
                {{"--describe", "p", "--timeline", "A", "--timeline", "B"},
                 "--describe option '--timeline' is given twice"},
                // OR4 -- docs/roadmap/DONNEES-2026-10.md.
                {{"--check-timeline", "p", "--policy", "{}", "--policy", "{}"},
                 "--check-timeline option '--policy' is given twice"},
                {{"--check-timeline", "p", "--file"},
                 "--check-timeline option '--file' needs a value"},
                {{"--export-cdl", "p", "c", "--timeline", "A", "--timeline",
                  "B"},
                 "--export-cdl option '--timeline' is given twice"},
                {{"--export-resolve-xml", "p", "out.xml", "--resolve-scaling",
                  "zoom"},
                 "--export-resolve-xml option '--resolve-scaling' does not "
                 "take 'zoom'"},
                {{"--align-words", "p", "--texts"},
                 "--align-words option '--texts' needs a value"},
                {{"--describe-motion-frame", "c.json", "z"},
                 "frame: frame must be an integer, got 'z'"},
                {{"--describe-motion-frame", "c.json", "0", "--speed", "2"},
                 "unknown --describe-motion-frame option '--speed'"},
                {{"--render-motion-frame", "c.json", "0", "out.png",
                  "--composition", "A", "--composition", "B"},
                 "--render-motion-frame option '--composition' is given "
                 "twice"},
                {{"--describe-motion-frame", "p", "0", "--clip", "X",
                  "--composition", "Y"},
                 "--clip: --clip names its composition and its props: leave "
                 "out --composition, --props, --variant and --format"},
                {{"--render-motion-video", "c.json", "out.mov", "--variants",
                  "a", "--variants", "b"},
                 "--render-motion-video option '--variants' is given twice"},
                {{"--export-motion-after", "c.json", "d.json", "--props"},
                 "--export-motion-after option '--props' needs a value"},
                {{"--humanize-motion", "c.json", "out.json", "--frob", "x"},
                 "unknown --humanize-motion option '--frob'"},
                {{"--describe-compositions", "p", "c", "extra"},
                 "unexpected --describe-compositions argument 'extra'"},
                {{"--list-fonts", "--frob", "x"},
                 "unknown --list-fonts option '--frob'"},
                {{"--list-fonts", "--query", "a", "--query", "b"},
                 "--list-fonts option '--query' is given twice"},
            };
        for (const auto& [arguments, detail] : refusals) {
            Check(UsageRefusal(arguments) ==
                      detail + usageOf(arguments[0].c_str()),
                  arguments[0] +
                      " says why it refuses its command line, with "
                      "status 2: " +
                      detail);
        }

        // FI15 -- docs/roadmap/FIABILITE-2026-10.md. An output naming the
        // file a motion command reads -- here through another spelling of
        // its path -- is refused, and that file keeps its bytes.
        {
            const std::filesystem::path directory =
                std::filesystem::temp_directory_path() /
                ("cutmachine-cli-over-source-" + std::to_string(::getpid()));
            std::filesystem::create_directories(directory);
            const std::filesystem::path composition = directory / "motion.json";
            const std::string source =
                "{\"width\": 32, \"height\": 32, \"fps\": 25, \"duration\": 2, "
                "\"root\": {\"type\": \"fill\", \"style\": "
                "{\"backgroundColor\": \"#ff0000\"}}}";
            for (const std::string name :
                 {"--export-motion-after", "--humanize-motion"}) {
                std::ofstream(composition, std::ios::binary) << source;
                const CliRun run =
                    RunCutmachine({name, composition.string(),
                                   (directory / "." / "motion.json").string()});
                Check(
                    run.exited && run.status == 1 &&
                        Contains(
                            RefusalDetail(run.output, name, "OutputExists"),
                            "write over the source") &&
                        Read(composition) == source,
                    name + " refuses to write over its source: " + run.output);
            }
            std::filesystem::remove_all(directory);
        }

        // AX-2026-09 AX6 -- the font families motion can name, "ok" first,
        // narrowed by part of a name without case.
        const CliRun fonts =
            RunCutmachine({"--list-fonts", "--query", "helvetica NEUE"});
        Check(fonts.exited && fonts.status == 0 &&
                  fonts.output.rfind("{\"ok\":true,", 0) == 0 &&
                  Contains(fonts.output, "\"Helvetica Neue\"") &&
                  !Contains(fonts.output, "\"Avenir\""),
              "--list-fonts lists the families a query names: " + fonts.output);

        // Help: plain text on standard output, status 0.
        const CliRun help = RunCutmachine({"--help"});
        Check(help.exited && help.status == 0 && help.output == CliHelpText(),
              "--help prints the list built from the table");
        const CliRun shortHelp = RunCutmachine({"-h"});
        Check(shortHelp.exited && shortHelp.status == 0 &&
                  shortHelp.output == help.output,
              "-h is --help");
        const std::string exportHelp =
            CliCommandHelpText(*FindCliCommand("--export"));
        for (const std::vector<std::string>& arguments :
             std::vector<std::vector<std::string>>{
                 {"--help", "--export"},
                 {"--help", "export"},
                 {"-h", "export"},
                 {"--export", "--help"},
                 {"--export", "-h"},
                 {"--export", "p", "out.mp4", "--help"}}) {
            const CliRun run = RunCutmachine(arguments);
            Check(run.exited && run.status == 0 && run.output == exportHelp,
                  "the help of one command, asked as '" + arguments[0] + " " +
                      arguments[1] + "': " + run.output);
        }
        Check(Contains(exportHelp, "--codec h264|hevc") &&
                  Contains(exportHelp, "render the timeline"),
              "the help of a command gives its arguments and what it does");
        Check(UsageRefusal({"--help", "frobnicate"}) ==
                  "unknown command 'frobnicate' (cutmachine --help lists the "
                  "commands)",
              "--help of an unknown command is refused");
        Check(UsageRefusal({"--help", "exprot"}) ==
                  "unknown command 'exprot', did you mean '--export'? "
                  "(cutmachine --help lists the commands)",
              "--help of a mistyped command suggests the one it meant");
    }

    // PRD20 -- a project path where nothing exists says so, with the path,
    // as IoError: on every command that takes a project, and for
    // open_project. A folder that exists and is not a package keeps the
    // refusal it had.
    {
        const std::filesystem::path absent =
            directory / "nowhere" / "Absent.cutmachine-project";
        const std::string absentMessage = "no project at '" + absent.string() +
                                          "': nothing exists at this path";
        Check(IsMissingProjectPath(absent.string()) &&
                  IsMissingProjectPath(
                      (absent / "project.cutmachine.json").string()) &&
                  IsMissingProjectPath(
                      (directory / "Absent.cutmachine-project").string()) &&
                  !IsMissingProjectPath(directory.string()) &&
                  !IsMissingProjectPath(
                      (directory / "project.cutmachine.json").string()),
              "a path is missing when neither it nor its package folder "
              "exists");
        Check(
            MissingProjectMessage(absent.string()) == absentMessage &&
                IsMissingProjectMessage("cannot open 'x': " + absentMessage) &&
                !IsMissingProjectMessage(
                    "unsupported project package (CUTMACHINE package v2 "
                    "required)"),
            "the sentence names the path and is recognised when wrapped");

        Project loaded;
        std::string loadError;
        Check(!LoadStoredProject(absent.string(), loaded, loadError) &&
                  loadError == absentMessage,
              "the engine refuses a missing path as missing: " + loadError);
        Check(!LoadStoredProject(directory.string(), loaded, loadError) &&
                  loadError ==
                      "unsupported project package (CUTMACHINE package v2 "
                      "required)",
              "a folder that is not a package keeps its refusal: " + loadError);

        ProjectSessionLock lock;
        std::string owner;
        std::string lockError;
        Check(!lock.Acquire(absent.string(), owner, lockError) &&
                  !lock.Held() && lockError == absentMessage,
              "a lock that cannot be made beside nothing says the project "
              "is missing: " +
                  lockError);

        std::string output;
        Check(DescribeCommand(absent.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a missing project",
                                "IoError") == absentMessage,
              "describe names the missing path: " + output);
        Check(DescribeCommand(directory.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a plain folder",
                                "ParseError") ==
                      "unsupported project package (CUTMACHINE package v2 "
                      "required)",
              "describe of a folder that is not a package is unchanged: " +
                  output);
        Check(LockedIngestCommand(absent.string(), directory.string(), false,
                                  output) == 1 &&
                  RefusalDetail(output, "ingest into a missing project",
                                "IoError") == absentMessage,
              "ingest names the missing path before any lock: " + output);
        Check(CallToolCommand(absent.string(), "describe", "{}", output) == 1 &&
                  Contains(RefusalDetail(output,
                                         "call-tool on a missing "
                                         "project",
                                         "IoError"),
                           absentMessage),
              "call-tool names the missing path: " + output);

        // open_project goes through the same load: IoError by name, not the
        // ParseError of a package that would not parse, whether or not the
        // folder the path sits in exists.
        for (const std::filesystem::path& path :
             {absent, directory / "Absent.cutmachine-project"}) {
            McpProjectBackend backend({}, false);
            std::string result;
            std::string errorName;
            std::string message;
            Check(!backend.OpenProject(path.string(), result, errorName,
                                       message) &&
                      errorName == "IoError" &&
                      Contains(message, "no project at '" + path.string() +
                                            "': nothing exists at this path") &&
                      !backend.HasOpenProject(),
                  "open_project names the missing path as IoError: " +
                      errorName + " " + message);
            Check(!std::filesystem::exists(path.string() + ".lock"),
                  "and leaves no lock beside nothing");
        }
        McpProjectBackend backend({}, false);
        std::string result;
        std::string errorName;
        std::string message;
        Check(!backend.OpenProject(directory.string(), result, errorName,
                                   message) &&
                  errorName == "ParseError" &&
                  Contains(message, "unsupported project package"),
              "open_project of a folder that is not a package is "
              "unchanged: " +
                  errorName + " " + message);

        for (const std::vector<std::string>& arguments :
             std::vector<std::vector<std::string>>{
                 {"--describe", absent.string()},
                 {"--timeline-stats", absent.string()},
                 {"--undo-project-op", absent.string()},
                 {"--export", absent.string(), "out.mp4"},
                 {"--ingest", absent.string(), directory.string()},
                 {"--call-tool", absent.string(), "describe"},
                 {"--list-multicam-groups", absent.string()},
                 {"--describe-compositions", absent.string()}}) {
            const CliRun run = RunCutmachine(arguments);
            Check(run.exited && run.status == 1 &&
                      Contains(
                          RefusalDetail(run.output,
                                        arguments[0] + " on a missing project",
                                        "IoError"),
                          absentMessage),
                  "cutmachine " + arguments[0] +
                      " names the missing path, status 1: " + run.output);
        }
        const CliRun folder = RunCutmachine({"--describe", directory.string()});
        Check(folder.exited && folder.status == 1 &&
                  RefusalDetail(folder.output, "--describe of a plain folder",
                                "ParseError") ==
                      "unsupported project package (CUTMACHINE package v2 "
                      "required)",
              "cutmachine --describe of a folder that is not a package is "
              "unchanged: " +
                  folder.output);
    }

    // PRD20 -- a project written by a more recent CUTMACHINE says so: the
    // version found and the newest this build reads, for the document, the
    // project file and the package. The error names do not change.
    {
        const int32_t newestDocument = DocumentFormatVersion(
            true, true, true, true, true, true, true, true, true, true, true,
            true, true, true, true, true, true, true, true, true);
        const std::string written =
            "): the project was written by a more recent CUTMACHINE";
        const std::string newerDocument =
            "document version 999 is newer than this build reads (up to " +
            std::to_string(newestDocument) + written;

        std::string json = Fixture().SaveToString();
        const std::string versionKey = "\"version\": 7";
        Check(Contains(json, versionKey), "the fixture declares version 7");
        Document document;
        std::string error;
        std::string newest = json;
        newest.replace(newest.find(versionKey), versionKey.size(),
                       "\"version\": " + std::to_string(newestDocument));
        Check(
            Document::LoadFromString(newest, document, error),
            "the newest document version this build writes is read: " + error);
        std::string next = json;
        next.replace(next.find(versionKey), versionKey.size(),
                     "\"version\": " + std::to_string(newestDocument + 1));
        Check(!Document::LoadFromString(next, document, error) &&
                  error == "document version " +
                               std::to_string(newestDocument + 1) +
                               " is newer than this build reads (up to " +
                               std::to_string(newestDocument) + written,
              "the version after it is refused as newer, and the refusal "
              "names the newest: " +
                  error);
        json.replace(json.find(versionKey), versionKey.size(),
                     "\"version\": 3");
        Check(!Document::LoadFromString(json, document, error) &&
                  error == "unsupported document version 3",
              "a version too old keeps its refusal: " + error);

        std::string createOutput;
        const std::filesystem::path package =
            directory / "Newer.cutmachine-project";
        Check(
            CreateProjectCommand(package.string(), "Newer", createOutput) == 0,
            "the fixture project is created: " + createOutput);
        const std::filesystem::path projectFile =
            package / "project.cutmachine.json";
        const std::string projectJson = Read(projectFile);
        const std::string manifestJson = Read(package / "manifest.json");
        Project created;
        Check(LoadStoredProject(projectFile.string(), created, error),
              "the fixture project loads: " + error);
        const std::filesystem::path timelineFile =
            package / "Timelines" / (created.active_timeline_id + ".json");
        const std::string timelineJson = Read(timelineFile);
        const auto replaced = [](std::string text, const std::string& from,
                                 const std::string& to) {
            const size_t at = text.find(from);
            Check(at != std::string::npos, "the fixture holds " + from);
            return at == std::string::npos ? text
                                           : text.replace(at, from.size(), to);
        };

        // The timeline file of the package: the case the audit met.
        Write(timelineFile,
              replaced(timelineJson, "\"version\": 7", "\"version\": 999"));
        std::string output;
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a newer timeline",
                                "ParseError") ==
                      "invalid portable timeline '" +
                          created.active_timeline_id + "': " + newerDocument,
              "a newer timeline document is said to be newer: " + output);
        const CliRun run = RunCutmachine({"--describe", package.string()});
        Check(run.exited && run.status == 1 &&
                  Contains(RefusalDetail(run.output,
                                         "--describe of a newer timeline",
                                         "ParseError"),
                           newerDocument),
              "and by the binary: " + run.output);
        Write(timelineFile, timelineJson);

        // The project file: its own version, then its documents'.
        Write(projectFile, replaced(projectJson, "\"project_version\":3",
                                    "\"project_version\":4"));
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a newer project file",
                                "ParseError") ==
                      "project version 4 is newer than this build reads "
                      "(up to 3" +
                          written,
              "a newer project file is said to be newer: " + output);
        Write(projectFile, replaced(projectJson, "\"document_version\":7",
                                    "\"document_version\":999"));
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of newer project documents",
                                "ParseError") ==
                      "invalid shared project data: " + newerDocument,
              "newer documents in a project file are said to be newer: " +
                  output);
        Write(projectFile, replaced(projectJson, "\"project_version\":3",
                                    "\"project_version\":1"));
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of an older project file",
                                "ParseError") == "unsupported project version",
              "a project version too old keeps its refusal: " + output);
        Write(projectFile, projectJson);

        // The package: its manifest.
        const std::string newerPackage =
            "project package version 3 is newer than this build reads (up "
            "to 2" +
            written;
        Write(package / "manifest.json",
              replaced(manifestJson, "\"version\":2", "\"version\":3"));
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a newer package",
                                "ParseError") == newerPackage,
              "a newer package is said to be newer: " + output);
        CollectionIntegrityReport report;
        Check(!VerifyPortableProject(projectFile.string(), report, error) &&
                  error == newerPackage,
              "and so it is when the window verifies it: " + error);
        Write(package / "manifest.json",
              replaced(manifestJson, "\"format\":\"cutmachine-collection\"",
                       "\"format\":\"another-collection\""));
        Check(DescribeCommand(projectFile.string(), output) == 1 &&
                  RefusalDetail(output, "describe of a foreign package",
                                "ParseError") ==
                      "unsupported project package (CUTMACHINE package v2 "
                      "required)",
              "a manifest of another format is not called newer: " + output);
        Write(package / "manifest.json", manifestJson);
        Check(DescribeCommand(projectFile.string(), output) == 0,
              "the fixture project reads again once restored: " + output);
    }

    // PRD20 -- an ingest that read nothing is a refusal, with the status of
    // one, on the command line and for ingest_media alike. The media side
    // of the rule is in ingest_tests; a file that is not media needs none.
    {
        std::string output;
        const std::filesystem::path package =
            directory / "Ingest.cutmachine-project";
        Check(CreateProjectCommand(package.string(), "Ingest", output) == 0,
              "the ingest fixture is created: " + output);
        const std::string projectFile =
            (package / "project.cutmachine.json").string();
        const std::string before = Read(projectFile);
        const std::filesystem::path text = directory / "a.txt";
        Write(text, "not media\n");

        const auto checkRefusal = [&](const std::string& answer,
                                      const std::string& label) {
            const std::string detail = RefusalDetail(answer, label, "IoError");
            Check(detail.rfind("nothing was ingested: 'a.txt' could not be "
                               "read (",
                               0) == 0,
                  label + " says nothing was ingested, and why: " + detail);
            mcp_json::Value envelope;
            std::string parseError;
            const mcp_json::Value* report =
                mcp_json::Value::Parse(answer, envelope, parseError)
                    ? envelope.Find("result")
                    : nullptr;
            const mcp_json::Value* errors =
                report != nullptr ? report->Find("errors") : nullptr;
            Check(
                errors != nullptr && errors->IsArray() &&
                    errors->AsArray().size() == 1 &&
                    errors->AsArray()[0].Find("file") != nullptr &&
                    errors->AsArray()[0].Find("file")->AsString() == "a.txt" &&
                    errors->AsArray()[0].Find("reason") != nullptr &&
                    report->Find("added") != nullptr &&
                    report->Find("added")->Dump() == "0" &&
                    report->Find("skipped") != nullptr &&
                    report->Find("skipped")->Dump() == "1",
                label + " keeps the errors, under result: " + answer);
        };

        Check(
            LockedIngestCommand(projectFile, text.string(), false, output) == 1,
            "an ingest that read nothing fails: " + output);
        checkRefusal(output, "ingest of a text file");
        const CliRun run =
            RunCutmachine({"--ingest", package.string(), text.string()});
        Check(run.exited && run.status == 1,
              "cutmachine --ingest of a text file exits with status 1: " +
                  std::to_string(run.status));
        checkRefusal(run.output, "--ingest of a text file");
        // ingest_media, the same refusal: what the tool's dispatcher gives
        // any client of the catalog.
        Check(CallToolCommand(projectFile, "ingest_media",
                              "{\"path\":\"" + text.string() + "\"}",
                              output) == 1,
              "ingest_media of a text file fails: " + output);
        checkRefusal(output, "ingest_media of a text file");
        Check(Read(projectFile) == before,
              "an ingest that read nothing leaves the project as it was");

        // A folder without a single file is not a failure: nothing was
        // refused.
        const std::filesystem::path empty = directory / "empty-rushes";
        std::filesystem::create_directory(empty);
        Check(
            LockedIngestCommand(projectFile, empty.string(), false, output) ==
                    0 &&
                Contains(output, "\"ok\":true") &&
                Contains(output, "\"added\":0") &&
                Contains(output, "\"errors\":[]"),
            "an empty folder is ingested as nothing, successfully: " + output);
    }

    // PRD16 -- ROADMAP.md. --export no longer chooses an encoder of its own:
    // no codec and no preset is the engine's default for the timeline, the
    // file export_timeline writes without codec. --codec keeps the file it
    // always made (HEVC Main10 was the CLI's default until then), --preset
    // names the app's presets, and a preset that contradicts the codec is
    // refused by name.
    {
        const std::string ffmpegDirectory =
            std::filesystem::path(FFMPEG_EXECUTABLE).parent_path().string();
        const char* inheritedPath = std::getenv("PATH");
        const std::string savedPath =
            inheritedPath != nullptr ? inheritedPath : "/usr/bin";
        setenv("PATH", (ffmpegDirectory + ':' + savedPath).c_str(), 1);

        const Ulid rush = "01K30000000000000000000071";
        LibraryMedia media;
        media.id = rush;
        media.path = "rush.mp4";
        media.filename = "rush.mp4";
        media.codec = "h264";
        media.width = 320;
        media.height = 180;
        media.rate = {25, 1};
        media.duration = {25, 25};
        media.orientation = "landscape";
        media.has_audio = true;
        media.audio_rate = 48000;
        media.audio_channels = 1;
        Document delivery;
        delivery.sequence.id = "01K30000000000000000000072";
        delivery.sequence.name = "Livraison";
        delivery.sequence.width = 320;
        delivery.sequence.height = 180;
        delivery.sequence.frame_rate = {25, 1};
        delivery.library = {media};
        delivery.sources = {{rush, "rush.mp4", {25, 1}, {25, 25}}};
        delivery.sequence.tracks = {{"01K30000000000000000000073",
                                     "video",
                                     0,
                                     {{"01K30000000000000000000074",
                                       rush,
                                       {0, 25},
                                       {25, 25},
                                       {0, 25}}}},
                                    {"01K30000000000000000000075",
                                     "audio",
                                     1,
                                     {{"01K30000000000000000000076",
                                       rush,
                                       {0, 25},
                                       {25, 25},
                                       {0, 25}}}}};
        std::string deliveryPath;
        Check(CreatePortableProject(
                  (directory / "Delivery.cutmachine-project").string(),
                  Project::FromDocument(delivery, "Livraison"), deliveryPath,
                  error),
              "delivery fixture saves: " + error);
        const std::filesystem::path package =
            std::filesystem::path(deliveryPath).parent_path();
        const std::string generate =
            std::string("'") + FFMPEG_EXECUTABLE +
            "' -hide_banner -loglevel error "
            "-f lavfi -i 'testsrc2=size=320x180:rate=25:duration=1' "
            "-f lavfi -i 'sine=frequency=440:sample_rate=48000:duration=1' "
            "-c:v libx264 -pix_fmt yuv420p -c:a aac -ac 1 -shortest -y '" +
            (package / "rush.mp4").string() + "'";
        Check(std::system(generate.c_str()) == 0,
              "FFmpeg generates the delivery rush");
        const auto exported = [&](const std::string& name,
                                  const ExportCommandChoice& choice,
                                  std::string& output) {
            return ExportCommand(deliveryPath, (directory / name).string(),
                                 choice, {}, nullptr, output);
        };

        std::string output;
        ExportCommandChoice choice;
        choice.software = true;
        Check(exported("default.mp4", choice, output) == 0 &&
                  output.find("\"codec\":\"h264\",\"profile\":\"high\"") !=
                      std::string::npos &&
                  output.find("\"width\":320,\"height\":180") !=
                      std::string::npos,
              "--export without codec writes the engine's SDR default, H.264 "
              "High at the timeline's format: " +
                  output);
        std::string tool;
        Check(CallToolCommand(deliveryPath, "export_timeline",
                              "{\"timeline_id\":\"" + delivery.sequence.id +
                                  "\",\"output_path\":\"" +
                                  (directory / "default-tool.mp4").string() +
                                  "\",\"software\":true}",
                              tool) == 0 &&
                  tool.find("\"codec\":\"h264\",\"profile\":\"high\"") !=
                      std::string::npos,
              "export_timeline without codec writes the same: " + tool);

        choice.codec = "hevc";
        Check(exported("hevc.mp4", choice, output) == 0 &&
                  output.find("\"codec\":\"hevc\",\"profile\":\"main10\"") !=
                      std::string::npos,
              "--codec hevc gives the file --export wrote before: " + output);

        choice.codec.clear();
        choice.preset = "hevc_web_1080";
        Check(exported("web.mp4", choice, output) == 0 &&
                  output.find("\"codec\":\"hevc\",\"profile\":\"main10\"") !=
                      std::string::npos &&
                  output.find("\"width\":1920,\"height\":1080") !=
                      std::string::npos,
              "--preset names one of the app's presets: " + output);

        choice.codec = "hevc";
        choice.preset = "h264";
        Check(
            exported("conflict.mp4", choice, output) != 0 &&
                output.find("\"error\":\"ValidationFailed\"") !=
                    std::string::npos &&
                output.find("contradicts codec 'hevc'") != std::string::npos &&
                !std::filesystem::exists(directory / "conflict.mp4"),
            "a preset that contradicts the codec is refused by name: " +
                output);
        choice.codec.clear();
        choice.preset = "prores";
        Check(
            exported("unknown.mp4", choice, output) != 0 &&
                output.find(Exporter::PresetKeys()) != std::string::npos,
            "an unknown preset is refused with every key there is: " + output);

        // export_timeline keeps its timeline_id: a refusal names the
        // timelines, name and ID, for an agent to correct in one turn.
        Check(
            CallToolCommand(deliveryPath, "export_timeline",
                            "{\"output_path\":\"" +
                                (directory / "unnamed.mp4").string() + "\"}",
                            tool) != 0 &&
                tool.find("'Livraison' (" + delivery.sequence.id + ")") !=
                    std::string::npos,
            "export_timeline without timeline_id names the timelines: " + tool);
        Check(
            CallToolCommand(deliveryPath, "export_timeline",
                            "{\"timeline_id\":\"\",\"output_path\":\"" +
                                (directory / "empty-id.mp4").string() + "\"}",
                            tool) != 0 &&
                tool.find("'Livraison' (" + delivery.sequence.id + ")") !=
                    std::string::npos,
            "an empty timeline_id is answered with the timelines too: " + tool);
        Check(CallToolCommand(
                  deliveryPath, "export_timeline",
                  "{\"timeline_id\":\"01K3000000000000000000ZZZZ\","
                  "\"output_path\":\"" +
                      (directory / "unknown-timeline.mp4").string() + "\"}",
                  tool) != 0 &&
                  tool.find("UnknownSequence") != std::string::npos &&
                  tool.find("'Livraison' (" + delivery.sequence.id + ")") !=
                      std::string::npos,
              "an unknown timeline_id is refused with the timelines there "
              "are: " +
                  tool);
        setenv("PATH", savedPath.c_str(), 1);
    }

    // PRD39 -- ROADMAP.md. What the command line answers, through the
    // binary, on a project of one rush of two seconds laid once: the four
    // commands that printed their report bare answer "ok" in front of it;
    // a timeline without a cut has nothing to show, which is no IoError;
    // and a sheet of one rush has the engine's one default, 24 cells, where
    // --media-sheet gave 12.
    {
        const char* inheritedFfmpeg = std::getenv("CUTMACHINE_FFMPEG");
        const bool hadFfmpeg = inheritedFfmpeg != nullptr;
        const std::string savedFfmpeg = hadFfmpeg ? inheritedFfmpeg : "";
        setenv("CUTMACHINE_FFMPEG", FFMPEG_EXECUTABLE, 1);
        const Ulid rush = "01K39000000000000000000001";
        const Ulid other = "01K39000000000000000000002";
        Document document;
        document.sequence.id = "01K39000000000000000000003";
        document.sequence.name = "Un plan";
        document.sequence.width = 160;
        document.sequence.height = 90;
        document.sequence.frame_rate = {25, 1};
        for (const Ulid& id : {rush, other}) {
            LibraryMedia media;
            media.id = id;
            media.path = "rush.mp4";
            media.filename = id == rush ? "rush.mp4" : "copie.mp4";
            media.codec = "h264";
            media.width = 160;
            media.height = 90;
            media.rate = {25, 1};
            media.duration = {50, 25};
            media.orientation = "landscape";
            document.library.push_back(media);
            document.sources.push_back({id, "rush.mp4", {25, 1}, {50, 25}});
        }
        document.sequence.tracks = {{"01K39000000000000000000004",
                                     "video",
                                     0,
                                     {{"01K39000000000000000000005",
                                       rush,
                                       {0, 25},
                                       {25, 25},
                                       {0, 25}}}}};
        std::string path;
        Check(CreatePortableProject(
                  (directory / "UnPlan.cutmachine-project").string(),
                  Project::FromDocument(document, "Un plan"), path, error),
              "PRD39 fixture saves: " + error);
        const std::string generate =
            std::string("'") + FFMPEG_EXECUTABLE +
            "' -hide_banner -loglevel error "
            "-f lavfi -i 'testsrc2=size=160x90:rate=25:duration=2' "
            "-c:v libx264 -pix_fmt yuv420p -y '" +
            (std::filesystem::path(path).parent_path() / "rush.mp4").string() +
            "'";
        Check(std::system(generate.c_str()) == 0,
              "FFmpeg generates the PRD39 rush");

        // Status 0, one JSON object, "ok":true as its first key.
        const auto answer = [](const CliRun& run, mcp_json::Value& value) {
            std::string parseError;
            if (!run.exited || run.status != 0 ||
                !mcp_json::Value::Parse(run.output, value, parseError) ||
                !value.IsObject()) {
                return false;
            }
            const mcp_json::Value* ok = value.Find("ok");
            return ok != nullptr && ok->IsBool() && ok->AsBool() &&
                   run.output.rfind("{\"ok\":true,", 0) == 0;
        };
        mcp_json::Value value;
        const CliRun described = RunCutmachine({"--describe", path});
        Check(answer(described, value) && value.Find("timeline") != nullptr &&
                  value.Find("library") != nullptr &&
                  value.Find("timelines") != nullptr,
              "--describe answers ok in front of the whole snapshot: " +
                  described.output.substr(0, 200));
        const CliRun quality = RunCutmachine({"--shot-quality-report", path});
        Check(answer(quality, value) && value.Find("unanalyzed") != nullptr,
              "--shot-quality-report answers ok in front of its report: " +
                  quality.output.substr(0, 200));
        const CliRun isolated = RunCutmachine({"--isolated-frames", path});
        Check(answer(isolated, value) && value.Find("unscanned") != nullptr,
              "--isolated-frames answers ok in front of its report: " +
                  isolated.output.substr(0, 200));
        const CliRun located =
            RunCutmachine({"--locate-source-frame", path, rush, "10"});
        Check(answer(located, value) && value.Find("matches") != nullptr,
              "--locate-source-frame answers ok in front of its matches: " +
                  located.output.substr(0, 200));
        std::string direct;
        Check(LocateSourceFrameCommand(path, rush, 10, direct) == 0 &&
                  direct == located.output,
              "the engine's command and the binary answer the same");

        const CliRun uncut = RunCutmachine(
            {"--cut-sheet", path, (directory / "cuts.jpg").string()});
        Check(uncut.exited && uncut.status == 1 &&
                  RefusalDetail(uncut.output, "--cut-sheet without a cut",
                                "ValidationFailed")
                          .rfind("nothing to show: the timeline has no "
                                 "visible cut",
                                 0) == 0 &&
                  !std::filesystem::exists(directory / "cuts.jpg"),
              "a cut sheet of one shot has nothing to show, and says why: " +
                  uncut.output);

        const CliRun one =
            RunCutmachine({"--media-sheet", path,
                           (directory / "one.jpg").string(), "--media", rush});
        Check(one.exited && one.status == 0 &&
                  Contains(one.output, "\"total_candidates\":50") &&
                  Contains(one.output, "\"selected_images\":24") &&
                  std::filesystem::exists(directory / "one.jpg"),
              "a sheet of one rush has 24 cells by default, as media_sheet: " +
                  one.output.substr(0, 300));
        const CliRun two = RunCutmachine({"--media-sheet", path,
                                          (directory / "two.jpg").string(),
                                          "--media", rush, "--media", other});
        Check(two.exited && two.status == 0 &&
                  Contains(two.output, "\"selected_images\":2"),
              "a sheet of two rushes has one cell each: " +
                  two.output.substr(0, 300));
        if (hadFfmpeg) {
            setenv("CUTMACHINE_FFMPEG", savedFfmpeg.c_str(), 1);
        } else {
            unsetenv("CUTMACHINE_FFMPEG");
        }
    }

    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All CLI tests passed\n";
    return 0;
}
