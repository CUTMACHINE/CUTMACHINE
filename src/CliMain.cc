// HEADLESS-2026-10 H1 -- docs/roadmap/HEADLESS-2026-10.md. The command
// line of every cutmachine executable (CliMain.h). Moved out of main.mm as
// it stood, branch for branch; what came from AppKit or Core Text arrives
// through CliHost.
#include "CliMain.h"

#include "AfterBridge.h"
#include "AudioDspDucking.h"
#include "AudioOffset.h"
#include "BoundaryAir.h"
#include "Cli.h"
#include "CliCommands.h"
#include "CutReview.h"
#include "Edl.h"
#include "EngineLog.h"
#include "Interchange.h"
#include "Json.h"
#include "McpEndpoint.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "MotionFrameRenderer.h"
#include "MulticamCli.h"
#include "PauseTightening.h"
#include "ProcessLauncher.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveImport.h"
#include "ResolveSend.h"
#include "ResolveXmlExport.h"
#include "TimelineSheets.h"
#include "Ulid.h"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

std::optional<int> RunCliCommand(int argc, char* argv[], const CliHost& host) {
    // PRD20 -- ROADMAP.md. What the command line asks for is settled first,
    // from the table of commands (CliCommands.h): help is printed, and a
    // command that does not exist, lacks a required argument or has one too
    // many is refused by name, with its usage. Every mistake used to get the
    // one sentence that ended this function. Nothing is set up for those,
    // and each branch below is only reached by a command the table knows --
    // so a command dispatched here without an entry there does not run.
    const std::vector<std::string> commandLine(argv + 1, argv + argc);
    // What exists is asked for one case only: a command typed without its
    // dashes ("version"), which reads as a project path and opened the
    // editor's window on a project that is not there. A path that cannot be
    // inspected is not known to be empty.
    const auto pathExists = [](const std::string& path) {
        std::error_code error;
        return std::filesystem::exists(path, error) || error;
    };
    const CliPreflight preflight =
        PreflightCommandLine(commandLine, pathExists);
    if (preflight.action == CliPreflight::Action::kPrint) {
        std::fwrite(preflight.text.data(), 1, preflight.text.size(), stdout);
        return 0;
    }
    if (preflight.action == CliPreflight::Action::kRefuse) {
        std::string output;
        const int result =
            FailCliCommand("ValidationFailed", preflight.text, output, 2);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    const CliCommand* const cliCommand = preflight.command;
    const std::string cliUsage =
        cliCommand != nullptr ? CliUsageLine(*cliCommand) : std::string();
    // PRD19 -- ROADMAP.md. What this binary is, answered before anything is
    // set up: no project, no AppKit, no FFmpeg, nothing started.
    if (argc == 2 && std::string(argv[1]) == "--version") {
        std::string output;
        const int result = VersionCommand(output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PRD18 -- ROADMAP.md (ACP4). The procedures this binary carries, read
    // before anything is set up either: they are compiled in (Procedures.h).
    if (argc >= 2 && std::string(argv[1]) == "--procedure") {
        std::string output;
        const int result = ProcedureCommand(
            std::vector<std::string>(argv + 2, argv + argc), output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // DB3, OR2 -- durable run steps need neither UI nor model setup.
    if (argc >= 2 && (std::string(argv[1]) == "--commit-run-step" ||
                      std::string(argv[1]) == "--get-run-step")) {
        std::string output;
        const int result =
            RunStepCommand(std::vector<std::string>(argv + 2, argv + argc),
                           std::string(argv[1]) == "--commit-run-step", output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // DB1 -- SQL archive commands need no UI or model setup.
    if (argc >= 2 && (std::string(argv[1]) == "--archive-project" ||
                      std::string(argv[1]) == "--query-project-archive")) {
        std::string output;
        const int result = ProjectArchiveCommand(
            std::vector<std::string>(argv + 2, argv + argc),
            std::string(argv[1]) == "--query-project-archive", output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // RM3 -- the forge reads corpus files, never a project.
    if (argc >= 2 && std::string(argv[1]) == "--train-caption-breaks") {
        std::string output;
        const int result = TrainCaptionBreaksCommand(
            std::vector<std::string>(argv + 2, argv + argc), output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // Stop before static transport state is destroyed, including short CLI
    // runs.
    if (argc >= 2 && (std::string(argv[1]) == "--project-archive-status" ||
                      std::string(argv[1]) == "--sync-project-archive" ||
                      std::string(argv[1]) == "--import-project-archive" ||
                      std::string(argv[1]) == "--backup-project-archive")) {
        std::string tool = std::string(argv[1]).substr(2);
        std::replace(tool.begin(), tool.end(), '-', '_');
        std::string output;
        const int result = ProjectArchiveMaintenanceCommand(
            std::vector<std::string>(argv + 2, argv + argc), tool, output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    const auto preferredStrictTimeline = [&host] {
        return host.require_explicit_timeline &&
               host.require_explicit_timeline();
    };
    // PRD20 -- a refusal of the command line (ValidationFailed, status 2)
    // says how its command is called, whichever branch below wrote it.
    const auto failCli = [&cliUsage](const std::string& errorCode,
                                     const std::string& detail,
                                     int exitStatus) {
        std::string output;
        const int result =
            FailCliCommand(errorCode, detail, output, exitStatus);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    };
    // PRD39 -- ROADMAP.md. The option at argv[index] that a branch below
    // does not take, refused for what it is (CliOptionRefusal): one its
    // command does not have, one left without its value, one given twice.
    // The branches called all three "unknown".
    const auto refuseOption = [&](int index) {
        const std::string option = index < argc ? argv[index] : "";
        return failCli("ValidationFailed",
                       cliCommand != nullptr
                           ? CliOptionRefusal(*cliCommand,
                                              std::vector<std::string>(
                                                  commandLine.begin() + 1,
                                                  commandLine.end()),
                                              static_cast<size_t>(index - 2))
                           : "unknown option '" + option + "'",
                       2);
    };
    // A package folder stands for its project file (ProjectPathOfPackage).
    // --create-project names a package to create, not one to open.
    std::string packageProject;
    if (argc >= 3 && std::string(argv[1]).rfind("--", 0) == 0 &&
        std::string(argv[1]) != "--create-project") {
        packageProject = ProjectPathOfPackage(argv[2]);
        argv[2] = packageProject.data();
    }
    // AFTER-COMP -- the same strict request envelope used by MCP is available
    // headlessly, so host automation is testable without entering AppKit.
    if (argc == 3 && std::string(argv[1]) == "--after-request") {
        std::string output;
        std::string errorName;
        std::string message;
        if (RunAfterBridgeRequest(argv[2], output, errorName, message)) {
            std::fwrite(output.data(), 1, output.size(), stdout);
            return 0;
        }
        return failCli(errorName, message, 1);
    }
    // MOTION-2026-09 M3 -- every surface of the model that renders a motion
    // frame (the MCP tool, --call-tool, the app's agent) reaches Core Text
    // and Core Graphics through this renderer (MotionFrameRenderer.h).
    // AGENT-2026-09 A20 -- a grade read on the face needs a face: Vision,
    // installed for every surface of the model as the renderer is. Both
    // come from the executable (CliHost).
    if (host.install_renderers) {
        host.install_renderers();
    }
    // MOTION-2026-09 M2 -- a motion frame is described and rendered headlessly
    // before any MCP or UI surface exposes it; M3 reads a project's
    // compositions the same way.
    // HEADLESS-2026-10 H1 -- the motion commands are the table's section
    // (CliCommands.h): an executable without Core Text refuses them by name.
    if (cliCommand != nullptr && cliCommand->section == CliSection::kMotion) {
        if (!host.motion_command) {
            return failCli("UnsupportedOperation",
                           "'" + std::string(argv[1]) +
                               "' needs the motion renderer (Core Text and "
                               "Core Graphics): " +
                               kMotionRendererHolder,
                           1);
        }
        const std::vector<std::string> arguments(argv + 2, argv + argc);
        std::string output;
        const int result = host.motion_command(argv[1], arguments, output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // F1.5 -- ROADMAP.md. Multicam groups: create, switch, remove, list.
    if (argc >= 2 && IsMulticamCommand(argv[1])) {
        const std::vector<std::string> arguments(argv + 2, argv + argc);
        std::string output;
        const int result = MulticamCommand(argv[1], arguments, output);
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // IA5 -- ROADMAP.md. A server or the app keeps the project it holds
    // loaded and assumes nobody else writes it (PERF5): every command that
    // writes the package or its analysis caches takes the session lock for
    // its whole run, as --ingest and --call-tool already did (IA7).
    ProjectSessionLock cliWriterLock;
    if (const std::vector<std::string> cliArguments(argv + 1, argv + argc);
        CliCommandWritesProject(cliArguments)) {
        std::string refusal;
        if (!LockProjectForCliWriter(cliArguments[1], cliWriterLock, refusal)) {
            std::fwrite(refusal.data(), 1, refusal.size(), stdout);
            return 1;
        }
    }
    // AGENT-2026-09 A6 -- ROADMAP.md. Four agents out of four asked for
    // `--help`, were refused with "invalid command line", and went on to
    // read main.mm and Cli.cc to learn the syntax; one gave up. PRD20 --
    // the list is printed above, from the table every refusal reads too,
    // and `--help <command>` gives one command's arguments.
    if (argc == 2 && std::string(argv[1]) == "--tools") {
        std::string output;
        const int result = ListToolsCommand(output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PROFIL-2026-11 PF1 -- what a server started with `--profile` lists,
    // to weigh it before handing it to a client.
    if (argc == 4 && std::string(argv[1]) == "--tools" &&
        std::string(argv[2]) == "--profile") {
        const McpToolProfile* profile = FindMcpToolProfile(argv[3]);
        if (profile == nullptr) {
            return failCli("ValidationFailed",
                           "--tools option '--profile' takes one of: " +
                               McpToolProfileNames() + "; '" +
                               std::string(argv[3]) + "' is none",
                           2);
        }
        std::string output;
        const int result = ListToolsCommand(output, *profile);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PRD2 -- docs/roadmap/PRODUIT-2026-10.md.
    if (argc == 2 && std::string(argv[1]) == "--check-ffmpeg") {
        std::string output;
        const int result = CheckFfmpegCommand(output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. Everything this machine
    // lacks, not only FFmpeg: no project, no window.
    if (argc == 2 && std::string(argv[1]) == "--check-setup") {
        std::string output;
        const int result = CheckSetupCommand(output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 4 || argc == 5) && std::string(argv[1]) == "--call-tool") {
        std::string output;
        const bool requireExplicitTimeline = preferredStrictTimeline();
        const int result =
            CallToolCommand(argv[2], argv[3], argc == 5 ? argv[4] : "{}",
                            output, requireExplicitTimeline);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PL3 -- ROADMAP.md. `--timeline <id>` describes another timeline
    // without activating it.
    if ((argc == 3 || (argc == 5 && std::string(argv[3]) == "--timeline")) &&
        std::string(argv[1]) == "--describe") {
        std::string output;
        const int result =
            DescribeCommand(argv[2], output, argc == 5 ? argv[4] : "");
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PL3 -- ROADMAP.md. Edit plans kept in a file the caller names: each
    // CLI invocation is a session of its own, and a plan must outlive the
    // one that prepared it. Same arguments as the MCP prepare_edit_plan.
    if (argc == 5 && std::string(argv[1]) == "--prepare-edit-plan") {
        std::string output;
        const int result = PrepareEditPlanCommand(
            argv[2], argv[3], argv[4], output, preferredStrictTimeline());
        output = WithCliUsage(output, result, cliUsage);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-edit-plan") {
        std::string output;
        const int result = ApplyEditPlanCommand(argv[2], argv[3], output,
                                                preferredStrictTimeline());
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--discard-edit-plan") {
        std::string output;
        const int result = DiscardEditPlanCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 4 || (argc == 6 && std::string(argv[4]) == "--format")) &&
        std::string(argv[1]) == "--create-project") {
        std::string output;
        const int result = CreateProjectCommand(argv[2], argv[3], output,
                                                argc == 6 ? argv[5] : "");
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // B7 -- the timeline's assembled PCM goes straight to Whisper. Unlike
    // --transcribe this command has no media id: its optional timeline id is
    // resolved by the project and its model is the local configured model.
    if (argc >= 3 && std::string(argv[1]) == "--transcribe-timeline") {
        std::string timelineId;
        std::string language = "auto";
        bool verbatim = false;
        WhisperDecodingProfile profile = WhisperDecodingProfile::Default;
        bool profileSeen = false;
        for (int index = 3; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--timeline" && index + 1 < argc &&
                timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--language" && index + 1 < argc) {
                language = argv[++index];
            } else if (option == "--decoding-profile" && index + 1 < argc &&
                       !profileSeen) {
                std::string error;
                if (!ParseWhisperDecodingProfile(argv[++index], profile, error))
                    return failCli("ValidationFailed", error, 2);
                profileSeen = true;
            } else if (option == "--verbatim") {
                verbatim = true;
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            TranscribeTimelineCommand(argv[2], timelineId, language, verbatim,
                                      output, {}, nullptr, profile);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // B7 -- direct SRT from a delivered media file, with no disposable
    // project. The local Whisper model remains configured exactly as for the
    // other transcription surfaces.
    if (argc >= 4 && std::string(argv[1]) == "--srt-from-media") {
        std::string language = "auto";
        bool verbatim = false;
        WhisperDecodingProfile profile = WhisperDecodingProfile::Default;
        bool profileSeen = false;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--language" && index + 1 < argc) {
                language = argv[++index];
            } else if (option == "--decoding-profile" && index + 1 < argc &&
                       !profileSeen) {
                std::string error;
                if (!ParseWhisperDecodingProfile(argv[++index], profile, error))
                    return failCli("ValidationFailed", error, 2);
                profileSeen = true;
            } else if (option == "--verbatim") {
                verbatim = true;
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result = SrtFromMediaCommand(argv[2], argv[3], language,
                                               verbatim, output, profile);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // ALPHA-2026-08 -- the model path became optional the day it became a
    // local setting (Transcription.h). Omitting it entirely, or passing it
    // empty to reach the arguments behind it, resolves the configured model.
    //
    // QC-2026-09 A3 -- several media ids may be given, comma-separated, so
    // one call pays the model load once. The grammar is otherwise unchanged:
    // `--transcribe <projet> <media-ids> [modèle] [langue] [--verbatim]`.
    if (argc >= 4 && std::string(argv[1]) == "--transcribe") {
        std::vector<std::string> mediaIds;
        {
            const std::string joined(argv[3]);
            size_t start = 0;
            while (start <= joined.size()) {
                const size_t comma = joined.find(',', start);
                const std::string piece = joined.substr(
                    start, comma == std::string::npos ? std::string::npos
                                                      : comma - start);
                if (!piece.empty()) mediaIds.push_back(piece);
                if (comma == std::string::npos) break;
                start = comma + 1;
            }
        }
        std::string language = "auto";
        bool verbatim = false;
        WhisperDecodingProfile profile = WhisperDecodingProfile::Default;
        bool profileSeen = false;
        bool includeSilent = false;
        // PRD40 -- install the new transcript even where it looks worse than
        // the cached one.
        bool overwrite = false;
        std::string modelPath;
        int positional = 0;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--decoding-profile" && index + 1 < argc &&
                !profileSeen) {
                std::string error;
                if (!ParseWhisperDecodingProfile(argv[++index], profile, error))
                    return failCli("ValidationFailed", error, 2);
                profileSeen = true;
            } else if (option == "--verbatim") {
                verbatim = true;
            } else if (option == "--include-silent") {
                includeSilent = true;
            } else if (option == "--overwrite") {
                overwrite = true;
            } else if (option.rfind("--", 0) == 0) {
                return refuseOption(index);
            } else if (positional == 0) {
                modelPath = option;
                ++positional;
            } else if (positional == 1) {
                language = option;
                ++positional;
            } else {
                return failCli("ValidationFailed",
                               "unexpected transcription argument '" +
                                   std::string(argv[index]) + "'",
                               2);
            }
        }
        std::string output;
        const int result = TranscribeMediaCommand(
            argv[2], mediaIds, modelPath, language, verbatim, includeSilent,
            output, {}, nullptr, overwrite, profile);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--shot-quality") {
        std::string output;
        const int result = AnalyzeShotQualityCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--shot-quality-report") {
        std::string output;
        const int result = ShotQualityReportCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // R8 -- docs/roadmap/REEL-2026-09.md. Without --analyze it reports what
    // the caches hold and names what they do not; with it, it decodes the
    // ranges the timeline reads and caches them first.
    // --media, repeated, scans those rushes whole: the derush of a shoot.
    if (argc >= 3 && std::string(argv[1]) == "--isolated-frames") {
        std::string timelineId;
        std::vector<Ulid> mediaIds;
        bool analyze = false;
        for (int index = 3; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--analyze") {
                analyze = true;
            } else if (option == "--timeline" && index + 1 < argc &&
                       timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--media" && index + 1 < argc) {
                mediaIds.push_back(argv[++index]);
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result = IsolatedFramesCommand(argv[2], timelineId, analyze,
                                                 output, {}, mediaIds);
        // PRD39 -- ROADMAP.md. The pass is the MCP tools' too, which answer
        // its report as they did; the command line answers it with "ok".
        output = WithCliOk(output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // OR4 -- docs/roadmap/DONNEES-2026-10.md.
    if (argc >= 3 && std::string(argv[1]) == "--check-timeline") {
        std::string timelineId;
        std::string policy;
        std::string file;
        bool policyGiven = false;
        bool fileGiven = false;
        for (int index = 3; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--timeline" && index + 1 < argc &&
                timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--policy" && index + 1 < argc &&
                       !policyGiven) {
                policy = argv[++index];
                policyGiven = true;
            } else if (option == "--file" && index + 1 < argc && !fileGiven) {
                file = argv[++index];
                fileGiven = true;
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            CheckTimelineCommand(argv[2], timelineId, policy, file, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 3 || (argc == 5 && std::string(argv[3]) == "--timeline")) &&
        std::string(argv[1]) == "--timeline-stats") {
        std::string output;
        const int result =
            TimelineStatsCommand(argv[2], output, argc == 5 ? argv[4] : "");
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 4 && (std::string(argv[1]) == "--contact-sheet" ||
                      std::string(argv[1]) == "--cut-sheet")) {
        std::string timelineId;
        // PRD39 -- the engine's default, which the tools read too.
        int32_t maximumImages = TimelineSheetSettings{}.maximum_images;
        // DEC29 -- docs/roadmap/GRADE-2026-09.md. As the tools' graded.
        bool graded = false;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--timeline" && index + 1 < argc &&
                timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--max-images" && index + 1 < argc) {
                maximumImages = std::atoi(argv[++index]);
            } else if (option == "--graded" && !graded) {
                graded = true;
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            std::string(argv[1]) == "--contact-sheet"
                ? ContactSheetCommand(argv[2], argv[3], timelineId,
                                      maximumImages, output, graded)
                : CutSheetCommand(argv[2], argv[3], timelineId, maximumImages,
                                  output, graded);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // RC1 -- docs/roadmap/RACCORDS-2026-10.md. --frame, repeated, names the
    // junctions; none reviews them all.
    if (argc >= 4 && std::string(argv[1]) == "--review-cuts") {
        std::string timelineId;
        std::vector<int64_t> frames;
        int32_t maximumBands = CutReviewRequest{}.max_bands;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--timeline" && index + 1 < argc &&
                timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--frame" && index + 1 < argc) {
                frames.push_back(std::atoll(argv[++index]));
            } else if (option == "--max-bands" && index + 1 < argc) {
                maximumBands = std::atoi(argv[++index]);
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result = ReviewCutsCommand(argv[2], argv[3], timelineId,
                                             frames, maximumBands, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // The derush sheet: --media, repeated, names the rushes.
    if (argc >= 4 && std::string(argv[1]) == "--media-sheet") {
        std::vector<Ulid> mediaIds;
        // PRD39 -- the engine's default, as media_sheet reads it: it was 12
        // here and 24 there.
        int32_t maximumImages = TimelineSheetSettings{}.maximum_images;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--media" && index + 1 < argc) {
                mediaIds.push_back(argv[++index]);
            } else if (option == "--max-images" && index + 1 < argc) {
                maximumImages = std::atoi(argv[++index]);
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result = MediaSheetCommand(argv[2], argv[3], mediaIds,
                                             maximumImages, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // G1 -- docs/roadmap/GRADE-2026-09.md. cutmachine --measure-color
    // <project> <clip_id>[,<clip_id>...] [--timeline <id>] [--position
    // Start|Middle|End] [--region x,y,width,height]
    if (argc >= 4 && std::string(argv[1]) == "--measure-color") {
        std::string timelineId;
        std::string position = "Middle";
        std::string region;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--timeline" && index + 1 < argc &&
                timelineId.empty()) {
                timelineId = argv[++index];
            } else if (option == "--position" && index + 1 < argc) {
                position = argv[++index];
            } else if (option == "--region" && index + 1 < argc) {
                region = argv[++index];
            } else {
                return refuseOption(index);
            }
        }
        std::vector<Ulid> clipIds;
        const std::string ids(argv[3]);
        for (size_t start = 0; start <= ids.size();) {
            const size_t comma = std::min(ids.find(',', start), ids.size());
            if (comma > start)
                clipIds.push_back(ids.substr(start, comma - start));
            start = comma + 1;
        }
        std::string output;
        const int result = MeasureColorCommand(argv[2], timelineId, clipIds,
                                               position, region, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // G2 -- cutmachine --match-color <project> <clip_id> <reference_clip_id>
    // [--timeline <id>] [--position P] [--reference-position P]
    // [--region x,y,w,h] [--reference-region x,y,w,h] [--match a,b]
    // G4 -- docs/roadmap/GRADE-2026-09.md. cutmachine --export-cdl <project>
    // <clip_id>[,<clip_id>...] [--file F.cc|.ccc|.cdl] [--timeline <id>]
    // [--resolve-node N]; cutmachine --apply-cdl <project> <clip_id>[,...]
    // --space S (--file F [--id X] | --slope "r g b" --offset "r g b"
    // --power "r g b" [--saturation s]) [--timeline <id>] [--preview].
    if (argc >= 4 && (std::string(argv[1]) == "--export-cdl" ||
                      std::string(argv[1]) == "--apply-cdl")) {
        const std::string command(argv[1]);
        std::vector<Ulid> clipIds;
        const std::string ids(argv[3]);
        for (size_t start = 0; start <= ids.size();) {
            const size_t comma = std::min(ids.find(',', start), ids.size());
            if (comma > start)
                clipIds.push_back(ids.substr(start, comma - start));
            start = comma + 1;
        }
        std::string timelineId;
        std::string file;
        std::string id;
        std::string space;
        int node = 1;
        bool preview = false;
        ResolveCdlMap resolve;
        resolve.saturation = "1";
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--preview" && command == "--apply-cdl") {
                preview = true;
                continue;
            }
            if (index + 1 >= argc)
                return failCli(
                    "ValidationFailed",
                    command + " option '" + option + "' needs a value", 2);
            const std::string value(argv[++index]);
            if (option == "--timeline" && timelineId.empty()) {
                timelineId = value;
            } else if (option == "--file") {
                file = value;
            } else if (option == "--resolve-node" &&
                       command == "--export-cdl") {
                node = std::atoi(value.c_str());
            } else if (command == "--apply-cdl" && option == "--space") {
                space = value;
            } else if (command == "--apply-cdl" && option == "--id") {
                id = value;
            } else if (command == "--apply-cdl" && option == "--slope") {
                resolve.slope = value;
            } else if (command == "--apply-cdl" && option == "--offset") {
                resolve.offset = value;
            } else if (command == "--apply-cdl" && option == "--power") {
                resolve.power = value;
            } else if (command == "--apply-cdl" && option == "--saturation") {
                resolve.saturation = value;
            } else {
                // The value is read: the option sits just before it.
                return refuseOption(index - 1);
            }
        }
        std::string output;
        const int result =
            command == "--export-cdl"
                ? ExportCdlCommand(argv[2], timelineId, clipIds, file,
                                   std::max(node, 1), output)
                : ApplyCdlCommand(argv[2], timelineId, clipIds, space, file, id,
                                  resolve, preview, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 5 && std::string(argv[1]) == "--match-color") {
        std::string timelineId;
        std::string position = "Middle";
        std::string referencePosition = "Middle";
        std::string region;
        std::string referenceRegion;
        std::string match;
        for (int index = 5; index < argc; ++index) {
            const std::string option(argv[index]);
            if (index + 1 >= argc) {
                return failCli(
                    "ValidationFailed",
                    "--match-color option '" + option + "' needs a value", 2);
            }
            const std::string value(argv[++index]);
            if (option == "--timeline" && timelineId.empty()) {
                timelineId = value;
            } else if (option == "--position") {
                position = value;
            } else if (option == "--reference-position") {
                referencePosition = value;
            } else if (option == "--region") {
                region = value;
            } else if (option == "--reference-region") {
                referenceRegion = value;
            } else if (option == "--match") {
                match = value;
            } else {
                // The value is read: the option sits just before it.
                return refuseOption(index - 1);
            }
        }
        std::string output;
        const int result = MatchColorCommand(
            argv[2], timelineId, argv[3], argv[4], position, referencePosition,
            region, referenceRegion, match, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 5 && std::string(argv[1]) == "--locate-source-frame") {
        std::string output;
        const int result = LocateSourceFrameCommand(
            argv[2], argv[3], std::atoll(argv[4]), output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--disfluencies") {
        std::string output;
        const int result = ListDisfluenciesCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // QC-2026-09 A2 -- min-gap and keep default in the engine, so the short
    // form is the one an editor types and the long form is the one a script
    // pins.
    if ((argc == 4 || argc == 6) &&
        std::string(argv[1]) == "--tighten-pauses") {
        PauseTighteningSettings defaults;
        int64_t minimumGapMs = defaults.minimum_gap_milliseconds;
        int64_t keepFrames = defaults.keep_frames;
        if (argc == 6) {
            minimumGapMs = std::atoll(argv[4]);
            keepFrames = std::atoll(argv[5]);
        }
        std::string output;
        const int result = TightenPausesCommand(argv[2], argv[3], minimumGapMs,
                                                keepFrames, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // B4 -- boundary air uses the B1 speech groups, not transcript words.
    // Short forms use the engine's three-frame / 300 ms safety defaults;
    // scripts can pin both values positionally after the clip ids.
    if ((argc == 4 || argc == 6) &&
        std::string(argv[1]) == "--trim-boundary-air") {
        BoundaryAirSettings defaults;
        int64_t keepFrames = defaults.keep_frames;
        int64_t minimumAirMs = defaults.minimum_air_milliseconds;
        if (argc == 6) {
            keepFrames = std::atoll(argv[4]);
            minimumAirMs = std::atoll(argv[5]);
        }
        std::string output;
        const int result = TrimBoundaryAirCommand(argv[2], argv[3], keepFrames,
                                                  minimumAirMs, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if ((argc == 5 || argc == 7) &&
        std::string(argv[1]) == "--close-junction-air") {
        BoundaryAirSettings defaults;
        int64_t keepFrames = defaults.keep_frames;
        int64_t minimumAirMs = defaults.minimum_air_milliseconds;
        if (argc == 7) {
            keepFrames = std::atoll(argv[5]);
            minimumAirMs = std::atoll(argv[6]);
        }
        std::string output;
        const int result = CloseJunctionAirCommand(
            argv[2], argv[3], argv[4], keepFrames, minimumAirMs, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 5 && std::string(argv[1]) == "--remove-words") {
        std::string output;
        const int result =
            RemoveWordsCommand(argv[2], argv[3], argv[4], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-op") {
        std::string output;
        const int result = ApplyOperationCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. The same edit with why it is
    // made, the `motif` object an edit tool takes, kept on its journal
    // entry.
    if (argc == 5 && std::string(argv[1]) == "--apply-op") {
        std::string output;
        mcp_json::Value given;
        std::string message;
        EditMotif motif;
        Operation operation = RemoveClipOperation{};
        EditError error = EditError::None;
        if (!mcp_json::Value::Parse(argv[4], given, message) ||
            !ParseMotifArgument(given, "--apply-op motif", motif, message) ||
            !DeserializeOperation(argv[3], operation, error, message)) {
            output =
                "{\"ok\":false,\"error\":\"ValidationFailed\",\"detail\":\"" +
                mcp_json::EscapeJsonString(message) + "\"}\n";
            std::fwrite(output.data(), 1, output.size(), stdout);
            return 1;
        }
        const int result =
            ApplyOperationCommand(argv[2], argv[3], output, {}, {},
                                  EntryMotifsFor(operation, {}, motif));
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--apply-project-op") {
        std::string output;
        const int result =
            ApplyProjectOperationCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. A project edit with why it
    // is made, kept on the project journal's entry, as --apply-op does for
    // a timeline's.
    if (argc == 5 && std::string(argv[1]) == "--apply-project-op") {
        std::string output;
        mcp_json::Value given;
        std::string message;
        EditMotif motif;
        if (!mcp_json::Value::Parse(argv[4], given, message) ||
            !ParseMotifArgument(given, "--apply-project-op motif", motif,
                                message)) {
            output =
                "{\"ok\":false,\"error\":\"ValidationFailed\",\"detail\":\"" +
                mcp_json::EscapeJsonString(message) + "\"}\n";
            std::fwrite(output.data(), 1, output.size(), stdout);
            return 1;
        }
        const int result = ApplyProjectOperationCommand(
            argv[2], argv[3], output, ProjectEntryMotifsFor(motif));
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--undo-project-op") {
        std::string output;
        const int result = UndoProjectOperationCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--redo-project-op") {
        std::string output;
        const int result = RedoProjectOperationCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 2 && argc <= 10 && std::string(argv[1]) == "--mcp-serve") {
        // IA7 -- ROADMAP.md. The project is optional: a server started
        // without one answers create_project and open_project, so a run can
        // begin from a folder of rushes without leaving MCP.
        int port = 0;
        bool portGiven = false;
        bool stdio = false;
        // PRD13 -- ROADMAP.md. `--token <t>` is the bearer token every HTTP
        // request must then carry (HttpRequestPolicy.h). Its value is
        // printed nowhere: not in a refusal below, not in the banner.
        bool tokenGiven = false;
        std::string tokenOption;
        std::string projectArgument;
        // PRD17 -- ROADMAP.md. Strict timeline mode from the start, for a
        // server a series of agents shares (McpBackend.h); open_project's
        // require_timeline sets it per session too.
        bool requireTimeline = false;
        // PROFIL-2026-11 PF1 -- docs/roadmap/PROFIL-2026-11.md. The tools the
        // server serves (McpToolProfiles.h): the whole catalog by default.
        const McpToolProfile* profile = &FullMcpToolProfile();
        for (int index = 2; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--profile") {
                if (index + 1 >= argc ||
                    std::string(argv[index + 1]).rfind("--", 0) == 0) {
                    return failCli("ValidationFailed",
                                   "--mcp-serve option '--profile' needs a "
                                   "value: one of " +
                                       McpToolProfileNames(),
                                   2);
                }
                const std::string name(argv[++index]);
                profile = FindMcpToolProfile(name);
                if (profile == nullptr) {
                    return failCli("ValidationFailed",
                                   "--mcp-serve option '--profile' takes one "
                                   "of: " +
                                       McpToolProfileNames() + "; '" + name +
                                       "' is none",
                                   2);
                }
            } else if (option == "--port") {
                // PRD13b -- ROADMAP.md. Read whole (ParseMcpPort): "abc" was
                // port 0, a free port nobody asked for, and a --port left
                // without its value was "unknown". An option where the value
                // should be is a value left out, as for --token; the sentence
                // is the one the table of commands gives (CliCommands.h).
                if (index + 1 >= argc ||
                    std::string(argv[index + 1]).rfind("--", 0) == 0) {
                    return failCli("ValidationFailed",
                                   "--mcp-serve option '--port' needs a value",
                                   2);
                }
                std::string portError;
                if (!ParseMcpPort(argv[++index], port, portError)) {
                    return failCli("ValidationFailed", portError, 2);
                }
                portGiven = true;
            } else if (option == "--stdio") {
                stdio = true;
            } else if (option == "--require-timeline") {
                requireTimeline = true;
            } else if (option == "--token") {
                // An option where the value should be is a value left out,
                // not a token: taking `--token --stdio` at its word would
                // start an HTTP server whose token is "--stdio".
                if (index + 1 >= argc ||
                    std::string(argv[index + 1]).rfind("--", 0) == 0) {
                    return failCli("ValidationFailed",
                                   "--token needs a value, given as the next "
                                   "argument (a token that begins with -- "
                                   "goes in CUTMACHINE_MCP_TOKEN)",
                                   2);
                }
                tokenGiven = true;
                tokenOption = argv[++index];
                // The arguments of a process can be listed by every user of
                // the machine (ps), the very clients a token is there to
                // keep out. The value is overwritten where it sits once
                // read: that shortens the exposure to the start-up, it does
                // not remove it -- CUTMACHINE_MCP_TOKEN never creates it.
                std::memset(argv[index], 'x', std::strlen(argv[index]));
            } else if (option.rfind("--token", 0) == 0) {
                // `--token=<t>`: refused without repeating it.
                return failCli("ValidationFailed",
                               "--token takes its value as the next "
                               "argument: --token <t>",
                               2);
            } else if (option.rfind("--", 0) != 0) {
                if (!projectArgument.empty()) {
                    // Not repeated either: a second bare argument may be a
                    // token typed without its option.
                    return failCli("ValidationFailed",
                                   "--mcp-serve takes one project path, and "
                                   "a second argument without an option was "
                                   "given",
                                   2);
                }
                projectArgument = option;
            } else {
                return failCli("ValidationFailed",
                               "unknown --mcp-serve option '" + option + "'",
                               2);
            }
        }
        if (stdio && portGiven) {
            return failCli("ValidationFailed",
                           "--stdio serves the client that spawned this "
                           "process; it takes no --port",
                           2);
        }
        if (stdio && tokenGiven) {
            return failCli("ValidationFailed",
                           "--stdio serves the client that spawned this "
                           "process; it takes no --token",
                           2);
        }
        // Over stdio the variable is not read at all: exported for an HTTP
        // server, it must not break the one a client spawns.
        McpHttpToken httpToken;
        if (!stdio) {
            std::string tokenError;
            if (!ResolveMcpHttpToken(tokenGiven ? &tokenOption : nullptr,
                                     httpToken, tokenError)) {
                return failCli("ValidationFailed", tokenError, 2);
            }
        }
        // Purely local, loopback-only HTTP + JSON-RPC MCP server
        // (ROADMAP.md F1.1). McpProjectBackend reuses ApplyOperationCommand/
        // UndoOperationCommand/RedoOperationCommand/DescribeCommand -- the
        // exact functions --apply-op/--describe already call -- so every
        // tool call takes the same load/apply/commit path a human editing
        // through the app or the CLI does. No AppKit/Metal/media decoding is
        // initialized for this path. The backend holds the project's session
        // lock for as long as it has the project open.
        const bool requireExplicitTimeline =
            requireTimeline || preferredStrictTimeline();
        McpProjectBackend backend({}, requireExplicitTimeline);
        // AX1 -- docs/roadmap/AX-2026-09.md. A project the app holds is
        // edited in the app: the stdio server relays to its endpoint rather
        // than refusing the lock it holds.
        std::string relayedProject;
        if (stdio && !projectArgument.empty()) {
            std::string resolved;
            std::string socketPath;
            std::string ignored;
            if (ResolveMcpProjectPath(
                    std::filesystem::absolute(projectArgument).string(),
                    resolved, ignored) &&
                FindMcpAppEndpoint(resolved, socketPath))
                relayedProject = resolved;
        }
        if (!projectArgument.empty() && relayedProject.empty()) {
            std::string opened;
            std::string openErrorName;
            std::string openError;
            if (!backend.OpenProject(std::filesystem::absolute(projectArgument)
                                         .lexically_normal()
                                         .string(),
                                     opened, openErrorName, openError)) {
                return failCli(
                    openErrorName,
                    "mcp-serve cannot open the project: " + openError, 1);
            }
        }
        McpServer server(backend, *profile);
        // PF1 -- said once on stderr, which a client keeps in its log.
        const std::string profileNotice =
            profile->everything
                ? std::string()
                : "Profile '" + profile->name +
                      "': " + std::to_string(server.Tools().Tools().size()) +
                      " of " + std::to_string(server.Tools().CatalogSize()) +
                      " tools\n";
        if (stdio) {
            // AGENT-2026-09 A1 -- ROADMAP.md. The protocol owns stdout: the
            // answers go to the descriptor the client gave us, and stdout
            // itself is pointed at stderr first, so anything a library or a
            // child prints on it lands in the client's log instead of
            // corrupting the stream.
            const int protocolFd = ::dup(STDOUT_FILENO);
            if (protocolFd < 0 || ::dup2(STDERR_FILENO, STDOUT_FILENO) < 0) {
                return failCli("IoError",
                               "mcp-serve --stdio cannot take over stdout: " +
                                   std::string(std::strerror(errno)),
                               1);
            }
            std::FILE* protocol = ::fdopen(protocolFd, "w");
            if (protocol == nullptr) {
                return failCli("IoError",
                               "mcp-serve --stdio cannot open its output: " +
                                   std::string(std::strerror(errno)),
                               1);
            }
            std::fprintf(stderr,
                         !relayedProject.empty()
                             ? "MCP server on stdio relaying to the "
                               "CUTMACHINE window that holds '%s'\n"
                         : projectArgument.empty()
                             ? "MCP server on stdio with no project open: "
                               "call create_project or open_project\n"
                             : "MCP server on stdio for '%s'\n",
                         projectArgument.c_str());
            std::fputs(profileNotice.c_str(), stderr);
            // A client that goes away closes our stdin; SIGPIPE on a closed
            // stdout would kill us before ServeStdio could report it.
            std::signal(SIGPIPE, SIG_IGN);
            // PRD35 -- ROADMAP.md. A server keeps the engine log.
            engine_log::StartProcessLog("mcp-stdio", true);
            const int served =
                ServeMcpStdio(server, stdin, protocol, relayedProject);
            engine_log::StopProcessLog();
            std::fclose(protocol);
            return served;
        }
        server.SetBearerToken(httpToken.token);
        std::string startError;
        if (!server.Start(port, startError)) {
            return failCli("IoError",
                           "mcp-serve failed to start: " + startError, 1);
        }
        engine_log::StartProcessLog("mcp-http", true);  // PRD35
        // PRD13 -- the banner says that a token is asked and where it was
        // given, so a 401 can be traced to a forgotten variable; never what
        // the token is.
        const std::string tokenNotice =
            httpToken.token.empty()
                ? std::string()
                : ", bearer token required (" + httpToken.source + ")";
        if (projectArgument.empty()) {
            std::fprintf(stderr,
                         "MCP server listening on http://127.0.0.1:%d/mcp "
                         "with no project open%s: call create_project or "
                         "open_project (Ctrl-C to stop)\n",
                         server.Port(), tokenNotice.c_str());
        } else {
            std::fprintf(stderr,
                         "MCP server listening on http://127.0.0.1:%d/mcp for "
                         "'%s'%s (Ctrl-C to stop)\n",
                         server.Port(), projectArgument.c_str(),
                         tokenNotice.c_str());
        }
        std::fputs(profileNotice.c_str(), stderr);
        static std::atomic_bool stopRequested{false};
        std::signal(SIGINT, [](int) { stopRequested.store(true); });
        std::signal(SIGTERM, [](int) { stopRequested.store(true); });
        while (!stopRequested.load() && server.IsRunning())
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        // FI9 -- docs/roadmap/FIABILITE-2026-10.md. The programs a task
        // started lead their own process groups: stopped with the server.
        process::SignalRunningGroups(SIGTERM);
        server.Stop();
        engine_log::StopProcessLog();
        return 0;
    }
    // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. `--bin <id>` files
    // what the ingest brings in, as the window's import does ("" is the
    // root); the options in either order, each once.
    if (argc >= 4 && argc <= 7 && std::string(argv[1]) == "--ingest") {
        bool recursive = false;
        std::optional<Ulid> targetBin;
        bool understood = true;
        for (int index = 4; index < argc && understood; ++index) {
            const std::string option = argv[index];
            if (option == "--recursive" && !recursive) {
                recursive = true;
            } else if (option == "--bin" && !targetBin && index + 1 < argc) {
                targetBin = std::string(argv[++index]);
            } else {
                understood = false;
            }
        }
        if (understood) {
            std::string output;
            const int result = LockedIngestCommand(argv[2], argv[3], recursive,
                                                   output, targetBin);
            std::fwrite(output.data(), 1, output.size(), stdout);
            return result;
        }
    }
    // SYNCHRO-2026-10 SY2 -- an edit, under the writer lock taken above.
    // --sync-audio-clip (SY2, the window) takes the recorder's sound already
    // on the timeline instead of its media and a track, and the same
    // options.
    const bool syncClip =
        argc >= 5 && std::string(argv[1]) == "--sync-audio-clip";
    if (syncClip || (argc >= 6 && std::string(argv[1]) == "--sync-audio")) {
        SyncAudioOptions options;
        std::string output;
        std::string refusal;
        const std::string command = argv[1];
        for (int index = syncClip ? 5 : 6; index < argc && refusal.empty();
             ++index) {
            const std::string option = argv[index];
            const bool hasValue = index + 1 < argc;
            if (option == "--keep-camera-sound") {
                options.mute_camera_sound = false;
            } else if (option == "--picture-channel" && hasValue &&
                       ParseAudioOffsetChannel(argv[index + 1],
                                               options.channels.reference)) {
                ++index;
            } else if (option == "--recorder-channel" && hasValue &&
                       ParseAudioOffsetChannel(argv[index + 1],
                                               options.channels.other)) {
                ++index;
            } else if (option == "--offset" && hasValue) {
                const std::string text = argv[++index];
                const size_t slash = text.find('/');
                char* valueEnd = nullptr;
                char* rateEnd = nullptr;
                const long long value =
                    std::strtoll(text.c_str(), &valueEnd, 10);
                const long long rate =
                    slash == std::string::npos
                        ? 0
                        : std::strtoll(text.c_str() + slash + 1, &rateEnd, 10);
                if (slash == std::string::npos ||
                    valueEnd != text.c_str() + slash || rateEnd == nullptr ||
                    *rateEnd != '\0' || rate <= 0 || rate > INT32_MAX) {
                    refusal =
                        "--offset takes <value>/<rate>, as "
                        "measure_audio_offset gives it: '" +
                        text + "'";
                } else {
                    options.has_offset = true;
                    options.offset = {value, static_cast<int32_t>(rate)};
                }
            } else {
                refusal = command;
                refusal += " does not take '" + option + "'";
            }
        }
        if (!refusal.empty()) {
            const int status =
                FailCliCommand("ValidationFailed", refusal, output, 2);
            std::fwrite(output.data(), 1, output.size(), stdout);
            return status;
        }
        const int result = syncClip
                               ? SyncAudioClipCommand(argv[2], argv[3], argv[4],
                                                      options, output)
                               : SyncAudioCommand(argv[2], argv[3], argv[4],
                                                  argv[5], options, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // SYNCHRO-2026-10 SY1 -- read only: no lock, nothing written.
    if (argc >= 5 && std::string(argv[1]) == "--measure-audio-offset") {
        AudioOffsetChannels channels;
        std::string output;
        for (int index = 5; index < argc; index += 2) {
            const std::string option = argv[index];
            AudioOffsetChannel* channel = nullptr;
            if (option == "--reference-channel") {
                channel = &channels.reference;
            } else if (option == "--other-channel") {
                channel = &channels.other;
            }
            if (channel == nullptr || index + 1 >= argc ||
                !ParseAudioOffsetChannel(argv[index + 1], *channel)) {
                const int status = FailCliCommand(
                    "ValidationFailed",
                    "--measure-audio-offset takes --reference-channel and "
                    "--other-channel, each mix, left or right; got '" +
                        option + "'",
                    output, 2);
                std::fwrite(output.data(), 1, output.size(), stdout);
                return status;
            }
        }
        const int result = MeasureAudioOffsetCommand(argv[2], argv[3], argv[4],
                                                     channels, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // FC6 -- FILMCRAFT-2026-10. Read only: no lock, nothing written.
    if (argc >= 4 && std::string(argv[1]) == "--propose-ducking") {
        AudioOffsetChannel channel = AudioOffsetChannel::Mix;
        audio_dsp::DuckingSettings settings;
        const std::vector<std::pair<std::string, int64_t*>> numbers = {
            {"--threshold-db", &settings.threshold_db},
            {"--hysteresis-db", &settings.hysteresis_db},
            {"--floor-percentile", &settings.floor_percentile},
            {"--minimum-speech-ms", &settings.minimum_speech_milliseconds},
            {"--bridge-ms", &settings.bridge_milliseconds},
            {"--depth-db", &settings.depth_db},
            {"--attack-ms", &settings.attack_milliseconds},
            {"--release-ms", &settings.release_milliseconds}};
        std::string output;
        std::string refusal;
        for (int index = 4; index < argc && refusal.empty(); index += 2) {
            const std::string option = argv[index];
            if (index + 1 >= argc) {
                refusal = option + " takes a value";
                break;
            }
            const std::string value = argv[index + 1];
            if (option == "--channel") {
                if (!ParseAudioOffsetChannel(value, channel)) {
                    refusal =
                        "--channel is mix, left or right: '" + value + "'";
                }
                continue;
            }
            int64_t* target = nullptr;
            for (const auto& [name, field] : numbers) {
                if (option == name) {
                    target = field;
                }
            }
            char* end = nullptr;
            const long long parsed = std::strtoll(value.c_str(), &end, 10);
            if (target == nullptr) {
                refusal = "--propose-ducking does not take '" + option + "'";
            } else if (value.empty() || end == nullptr || *end != '\0') {
                refusal = option + " takes a whole number: '" + value + "'";
            } else {
                *target = parsed;
            }
        }
        if (!refusal.empty()) {
            const int status =
                FailCliCommand("ValidationFailed", refusal, output, 2);
            std::fwrite(output.data(), 1, output.size(), stdout);
            return status;
        }
        const int result =
            ProposeDuckingCommand(argv[2], argv[3], channel, settings, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--speech-onset") {
        std::string output;
        const int result = AnalyzeSpeechOnsetCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // AX2 -- docs/roadmap/AX-2026-09.md. MFA's boundaries, under the writer
    // lock taken above; `--texts '<json>'` replaces a rush's Whisper text.
    if (argc >= 3 && std::string(argv[1]) == "--align-words") {
        std::vector<std::string> mediaIds;
        std::string textsJson;
        for (int index = 3; index < argc; ++index) {
            const std::string argument(argv[index]);
            if (argument == "--texts" && index + 1 < argc) {
                textsJson = argv[++index];
            } else if (argument.rfind("--", 0) == 0) {
                return refuseOption(index);
            } else {
                mediaIds.push_back(argument);
            }
        }
        std::string output;
        const int result =
            AlignWordsCommand(argv[2], mediaIds, textsJson, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // QC-2026-09 (A1) -- `--write` is what makes the alignment reach the tools
    // that cut on words; without it this only reports what it would correct.
    if ((argc == 3 || argc == 4) &&
        std::string(argv[1]) == "--align-transcripts") {
        if (argc == 4 && std::string(argv[3]) != "--write") {
            return failCli("ValidationFailed",
                           "unknown --align-transcripts option '" +
                               std::string(argv[3]) + "'",
                           2);
        }
        std::string output;
        const int result = AlignTranscriptsCommand(argv[2], argc == 4, output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--speech-onset-report") {
        std::string output;
        const int result = SpeechOnsetReportCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 4 && std::string(argv[1]) == "--export-srt") {
        std::string timelineId;
        bool wordByWord = false;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--word-by-word") {
                wordByWord = true;
            } else if (option == "--timeline" && index + 1 < argc) {
                timelineId = argv[++index];
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            ExportSrtCommand(argv[2], argv[3], output, timelineId, wordByWord);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The same cues as
    // --export-srt, written as WebVTT.
    if (argc >= 4 && std::string(argv[1]) == "--export-vtt") {
        std::string timelineId;
        bool wordByWord = false;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--word-by-word") {
                wordByWord = true;
            } else if (option == "--timeline" && index + 1 < argc) {
                timelineId = argv[++index];
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            ExportVttCommand(argv[2], argv[3], output, timelineId, wordByWord);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // FC7 -- the CMX 3600 lists of a timeline, its markers out and in
    // (Edl.h). Each option takes one value, given once.
    if (argc >= 4 && argc % 2 == 0 &&
        (std::string(argv[1]) == "--export-edl" ||
         std::string(argv[1]) == "--export-edl-markers" ||
         std::string(argv[1]) == "--import-edl-markers")) {
        const std::string command(argv[1]);
        std::string timelineId;
        std::string startTimecode;
        std::string styleName;
        EdlMarkerStyle style = EdlMarkerStyle::Loc;
        for (int index = 4; index + 1 < argc; index += 2) {
            const std::string option(argv[index]);
            const std::string value(argv[index + 1]);
            if (option == "--timeline" && timelineId.empty()) {
                timelineId = value;
            } else if (option == "--start-timecode" && startTimecode.empty() &&
                       !value.empty()) {
                startTimecode = value;
            } else if (option == "--style" &&
                       command == "--export-edl-markers" && styleName.empty() &&
                       ParseEdlMarkerStyle(value, style)) {
                styleName = value;
            } else {
                return refuseOption(index);
            }
        }
        if (startTimecode.empty()) startTimecode = kEdlDefaultStartTimecode;
        std::string output;
        const int result =
            command == "--export-edl"
                ? ExportEdlCommand(argv[2], argv[3], output, timelineId,
                                   startTimecode)
            : command == "--export-edl-markers"
                ? ExportEdlMarkersCommand(argv[2], argv[3], output, timelineId,
                                          style, startTimecode)
                : ImportEdlMarkersCommand(argv[2], argv[3], output, timelineId,
                                          startTimecode);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PA1 -- docs/roadmap/PARITE-2026-11.md. The timeline as an OTIO or a
    // Final Cut Pro XML file (Interchange.h).
    if (argc >= 4 && argc % 2 == 0 &&
        (std::string(argv[1]) == "--export-otio" ||
         std::string(argv[1]) == "--export-fcpxml")) {
        std::string timelineId;
        for (int index = 4; index + 1 < argc; index += 2) {
            if (std::string(argv[index]) == "--timeline" &&
                timelineId.empty()) {
                timelineId = argv[index + 1];
            } else {
                return refuseOption(index);
            }
        }
        std::string output;
        const int result =
            std::string(argv[1]) == "--export-otio"
                ? ExportOtioCommand(argv[2], argv[3], output, timelineId)
                : ExportFcpxmlCommand(argv[2], argv[3], output, timelineId);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // PA16 -- docs/roadmap/PARITE-2026-11.md. The descriptor of the one
    // send (ResolveExport.h), and the send itself (ResolveSend.h).
    if (argc >= 3 && (std::string(argv[1]) == "--export-resolve-timeline" ||
                      std::string(argv[1]) == "--send-resolve")) {
        const bool send = std::string(argv[1]) == "--send-resolve";
        std::string timelineId;
        ResolveSendRequest request;
        bool gradeGiven = false;
        bool nodeGiven = false;
        for (int index = 3; index < argc; ++index) {
            const std::string option(argv[index]);
            if (send && option == "--dry-run" && !request.dry_run) {
                request.dry_run = true;
                continue;
            }
            if (index + 1 >= argc) return refuseOption(index);
            const std::string value(argv[index + 1]);
            if (option == "--timeline" && timelineId.empty()) {
                timelineId = value;
            } else if (option == "--grade" && !gradeGiven &&
                       (value == "cdl" || value == "none")) {
                request.options.grade_cdl = value == "cdl";
                gradeGiven = true;
            } else if (option == "--cdl-node" && !nodeGiven && !value.empty() &&
                       value.size() < 4 &&
                       value.find_first_not_of("0123456789") ==
                           std::string::npos &&
                       std::stoi(value) >= 1) {
                request.options.cdl_node = std::stoi(value);
                nodeGiven = true;
            } else if (send && option == "--name" && request.name.empty() &&
                       !value.empty()) {
                request.name = value;
            } else {
                return refuseOption(index);
            }
            ++index;
        }
        std::string output;
        const int result =
            send ? SendResolveCommand(argv[2], timelineId, request, output)
                 : ExportResolveTimelineCommand(argv[2], output, timelineId,
                                                request.options);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // R9 -- docs/roadmap/REEL-2026-09.md. The return trip that carries a
    // speed and a dissolve, as a file Resolve imports (ResolveXmlExport.h).
    // R9 -- `--resolve-scaling fit|fill` declares how the Resolve project
    // places a picture of another size; a framed clip needs it.
    if (argc >= 4 && argc % 2 == 0 &&
        std::string(argv[1]) == "--export-resolve-xml") {
        std::string timelineId;
        ResolveScaling scaling = ResolveScaling::Undeclared;
        for (int index = 4; index + 1 < argc; index += 2) {
            const std::string option(argv[index]);
            if (option == "--timeline" && timelineId.empty()) {
                timelineId = argv[index + 1];
            } else if (option == "--resolve-scaling" &&
                       scaling == ResolveScaling::Undeclared &&
                       ParseResolveScaling(argv[index + 1], scaling)) {
            } else {
                // PRD39 -- a value it refuses, or an option given twice, is
                // no longer called unknown; the usage lists both options.
                return refuseOption(index);
            }
        }
        std::string output;
        const int result = ExportResolveXmlCommand(argv[2], argv[3], output,
                                                   timelineId, scaling);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 3 && std::string(argv[1]) == "--propose-sequence") {
        std::string output;
        const int result = ProposeSequenceCommand(argv[2], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc == 4 && std::string(argv[1]) == "--import-resolve") {
        std::string output;
        const int result = ImportResolveCommand(argv[2], argv[3], output);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    // R10 -- docs/roadmap/REEL-2026-09.md. The timeline beside the Media
    // Pool: same manifest-in, same headless shape.
    // DEC15 (GR15) -- `--resolve-database <Project.db>`: the grades of the
    // timeline, read from a copy of the Resolve project's database.
    if ((argc == 4 || argc == 6) &&
        std::string(argv[1]) == "--import-resolve-timeline") {
        if (argc == 6 && std::string(argv[4]) != "--resolve-database") {
            return refuseOption(4);
        }
        std::string output;
        const int result = ImportResolveTimelineCommand(
            argv[2], argv[3], output, "", argc == 6 ? argv[5] : "");
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc >= 4 && std::string(argv[1]) == "--export") {
        // PRD16 -- ROADMAP.md. No --codec and no --preset: the engine's
        // default for the timeline exported (Exporter::DefaultSettings),
        // chosen once the project is read (Cli.h).
        std::string codec;
        std::string preset;
        bool software = false;
        bool overwrite = false;
        bool rec709a = false;
        bool codecSpecified = false;
        std::string timelineId;
        // PA2 -- `--loudness <LUFS>`, whole, from -40 to -5.
        ExportLoudnessTarget loudness;
        for (int index = 4; index < argc; ++index) {
            const std::string option(argv[index]);
            if (option == "--loudness" && index + 1 < argc &&
                !loudness.requested) {
                char* end = nullptr;
                const long lufs = std::strtol(argv[++index], &end, 10);
                if (end == argv[index] || *end != '\0' ||
                    lufs * 1000 < kMinimumLoudnessTargetMillilufs ||
                    lufs * 1000 > kMaximumLoudnessTargetMillilufs) {
                    return failCli("ValidationFailed",
                                   "--loudness takes a loudness in whole "
                                   "LUFS, from -40 to -5",
                                   2);
                }
                loudness.requested = true;
                loudness.integrated_millilufs =
                    static_cast<int32_t>(lufs * 1000);
            } else if (option == "--software")
                software = true;
            else if (option == "--rec709a")
                rec709a = true;
            else if (option == "--overwrite")
                overwrite = true;
            else if (option == "--entire") {
                // Entire timeline is the engine's default export range.
            } else if (option == "--codec" && index + 1 < argc &&
                       !codecSpecified) {
                codec = argv[++index];
                codecSpecified = true;
                if (codec != "h264" && codec != "hevc") {
                    return failCli("ValidationFailed",
                                   "export codec must be h264 or hevc", 2);
                }
            } else if (option == "--preset" && index + 1 < argc &&
                       preset.empty()) {
                preset = argv[++index];
            } else if (option == "--timeline" && index + 1 < argc &&
                       timelineId.empty()) {
                timelineId = argv[++index];
            } else {
                return refuseOption(index);
            }
        }
        const ExportCommandChoice choice{codec,     preset,  software,
                                         overwrite, rec709a, loudness};
        std::string output;
        int lastPercent = -1;
        const int result = ExportCommand(
            argv[2], argv[3], choice,
            [&](const ExportProgress& progress) {
                const int percent = static_cast<int>(progress.fraction * 100.0);
                if (percent != lastPercent) {
                    lastPercent = percent;
                    std::fprintf(
                        stderr, "Export%s: %d%% (%lld/%lld images)\r",
                        codec.empty()     ? ""
                        : codec == "h264" ? " H.264"
                                          : " HEVC",
                        percent,
                        static_cast<long long>(progress.rendered_frames),
                        static_cast<long long>(progress.total_frames));
                    std::fflush(stderr);
                }
            },
            nullptr, output, timelineId);
        if (lastPercent >= 0) std::fputc('\n', stderr);
        std::fwrite(output.data(), 1, output.size(), stdout);
        return result;
    }
    if (argc > 2 || (argc == 2 && argv[1][0] == '-')) {
        // PRD20 -- a command the table knows, whose arguments none of the
        // branches above took. What is wrong is named as far as the usage
        // tells (CliArgumentsMismatch), and failCli adds the usage itself.
        // Without a command (the preflight refuses those first), the first
        // argument is named.
        return failCli(
            "ValidationFailed",
            cliCommand != nullptr
                ? CliArgumentsMismatch(*cliCommand, std::vector<std::string>(
                                                        commandLine.begin() + 1,
                                                        commandLine.end()))
                : "these arguments do not fit '" + std::string(argv[1]) + "'",
            2);
    }
    return std::nullopt;
}
