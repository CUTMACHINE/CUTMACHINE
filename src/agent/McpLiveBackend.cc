#include "McpLiveBackend.h"
#include "RunStepStorage.h"

#include "AfterBridge.h"
#include "CanonicalHash.h"
#include "Cli.h"
#include "DocumentDelta.h"
#include "ExportVerification.h"
#include "FrameCapture.h"
#include "Ingest.h"
#include "LookLutStorage.h"
#include "McpProjectBackend.h"
#include "Operations.h"
#include "ResolveImport.h"
#include "ResolveXmlExport.h"
#include "TimelineSheets.h"

#include <filesystem>

McpLiveBackend::McpLiveBackend(Document& document, EditLog& editLog,
                               std::function<void()> onApplied,
                               ProjectApplyCallback applyProject,
                               ProjectDescribeCallback describeProject,
                               TranscriptCallback readTranscript,
                               SourceTranscriptCallback readSourceTranscript,
                               SourceShotQualityCallback readSourceShotQuality,
                               SourceSpeechOnsetCallback readSourceSpeechOnset,
                               TimelineSelectCallback selectTimeline,
                               bool requireExplicitTimeline)
    : document_(document),
      edit_log_(editLog),
      on_applied_(std::move(onApplied)),
      apply_project_(std::move(applyProject)),
      describe_project_(std::move(describeProject)),
      read_transcript_(std::move(readTranscript)),
      read_source_transcript_(std::move(readSourceTranscript)),
      read_source_shot_quality_(std::move(readSourceShotQuality)),
      read_source_speech_onset_(std::move(readSourceSpeechOnset)),
      select_timeline_(std::move(selectTimeline)),
      require_explicit_timeline_(requireExplicitTimeline) {}

bool McpLiveBackend::SelectTimelineForEdit(const std::string& timelineId,
                                           std::string& errorName,
                                           std::string& message) {
    if (require_explicit_timeline_ && timelineId.empty()) {
        errorName = "TimelineRequired";
        message = "strict timeline editing requires an explicit timeline_id";
        return false;
    }
    if (timelineId.empty() || timelineId == document_.sequence.id) return true;
    if (!select_timeline_) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    if (!select_timeline_(timelineId, message)) {
        errorName = "UnknownSequence";
        return false;
    }
    return true;
}

bool McpLiveBackend::SnapshotDocument(Document& document, std::string&) {
    document = document_;
    return true;
}

namespace {

// F2.4 -- ROADMAP.md. Why an agent of the window stays on its project.
std::string WindowHoldsProject(const std::string& projectPath) {
    return "CUTMACHINE's window holds " +
           (projectPath.empty() ? std::string("an unsaved project")
                                : "'" + projectPath + "'") +
           " for as long as it is open, and the agents it serves -- its "
           "Agent panel, an agent relayed to it -- work on that project "
           "alone";
}

}  // namespace

bool McpLiveBackend::OpenProject(const std::string& requestedPath,
                                 std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    std::string projectPath;
    if (!ResolveMcpProjectPath(requestedPath, projectPath, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    std::error_code sameError;
    if (project_path_.empty() ||
        !std::filesystem::equivalent(projectPath, project_path_, sameError)) {
        errorName = "UnsupportedOperation";
        // F2.4 -- the editor holds its project, and the panel's agent with
        // it. DEC39 -- docs/roadmap/PHASES-2026-08.md. The way to another
        // project is the Fichier menu, which reopens the editor on it; it
        // was the welcome window, after quitting.
        message = WindowHoldsProject(project_path_) + ": to open '" +
                  projectPath +
                  "', the person chooses it with « Ouvrir un projet… » in "
                  "the Fichier menu of CUTMACHINE's window, which closes "
                  "this project and reopens on that one; an agent outside "
                  "the app opens it with open_project once CUTMACHINE is "
                  "closed";
        return false;
    }
    Project project;
    if (!SnapshotProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    mcp_json::Value root = mcp_json::Value::MakeObject();
    root.Set("path", mcp_json::Value::MakeString(project_path_));
    root.Set("project_id", mcp_json::Value::MakeString(project.id));
    root.Set("name", mcp_json::Value::MakeString(project.name));
    root.Set("active_timeline_id",
             mcp_json::Value::MakeString(project.active_timeline_id));
    root.Set("timelines", mcp_json::Value::MakeInt(
                              static_cast<int64_t>(project.timelines.size())));
    root.Set("rushes", mcp_json::Value::MakeInt(
                           static_cast<int64_t>(project.rushes.size())));
    root.Set("app", mcp_json::Value::MakeBool(true));
    resultJson = root.Dump();
    return true;
}

bool McpLiveBackend::CreateProject(const std::string&, const std::string&,
                                   const std::string&, std::string&,
                                   std::string& errorName,
                                   std::string& message) {
    errorName = "UnsupportedOperation";
    // DEC39 -- docs/roadmap/PHASES-2026-08.md. Refused still, as decided:
    // the panel's agent works on the window's project alone. The way is
    // the Fichier menu now, not the welcome window after quitting.
    message = WindowHoldsProject(project_path_) +
              ": to start another, the person chooses « Nouveau projet… » "
              "in the Fichier menu of CUTMACHINE's window, which reopens on "
              "it; an agent outside the app creates one with create_project "
              "once CUTMACHINE is closed. Rushes join this project with "
              "ingest_media";
    return false;
}

McpLiveBackend::IngestTarget McpLiveBackend::FreezeIngestTarget() const {
    IngestTarget target;
    target.project_path = project_path_;
    target.runner = media_task_runner_;
    if (!project_path_.empty()) {
        std::error_code error;
        const std::filesystem::path projectDirectory =
            std::filesystem::absolute(project_path_, error)
                .lexically_normal()
                .parent_path();
        if (!error) {
            target.complete = CompleteLibraryPaths(document_, projectDirectory);
        }
    }
    return target;
}

bool McpLiveBackend::RunProbeIngest(const IngestTarget& target,
                                    const std::string& mediaPath,
                                    bool recursive, ProbedIngest& probed,
                                    std::string& errorName,
                                    std::string& message) {
    if (target.project_path.empty()) {
        errorName = "InvalidDocument";
        message =
            "save the project before importing rushes: their paths are "
            "stored relative to its package";
        return false;
    }
    // As McpProjectBackend::IngestMedia: a relative path would be read
    // against the app's working directory, which is no one's.
    if (!std::filesystem::path(mediaPath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "media path must be absolute";
        return false;
    }
    probed = ProbedIngest{};
    std::vector<std::filesystem::path> candidates;
    if (!CollectIngestFiles(mediaPath, recursive, candidates, probed.ignored,
                            message)) {
        errorName = "IoError";
        return false;
    }
    if (!target.runner) {
        ProbeIngestFiles(candidates, target.complete, probed.files);
        return true;
    }
    // The task's work keeps copies: the manager holds it past the wait.
    auto files = std::make_shared<std::vector<ProbedIngestFile>>();
    const std::set<std::string> complete = target.complete;
    const MediaTaskSnapshot task = target.runner(
        MediaTaskKind::Probe, IngestTaskLabel(candidates.size()),
        [candidates, complete, files](MediaTaskContext& context,
                                      std::string& error) {
            if (ProbeIngestFiles(candidates, complete, *files, &context)) {
                return true;
            }
            error = "import cancelled";
            return false;
        });
    if (task.state != MediaTaskState::Succeeded) {
        // As the transcriptions say it (Cli.cc): a cancel from the task bar
        // carries no error of its own, the wait limit does ("timed out").
        errorName = "IoError";
        message =
            task.state == MediaTaskState::Failed
                ? task.error
                : "import " + (task.error.empty() ? std::string("cancelled")
                                                  : task.error);
        return false;
    }
    probed.files = std::move(*files);
    return true;
}

bool McpLiveBackend::CommitIngest(const ProbedIngest& probed,
                                  std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    if (!commit_ingest_) {
        errorName = "UnsupportedOperation";
        message = "this live backend has no window to import rushes into";
        return false;
    }
    IngestOutcome outcome;
    if (!commit_ingest_(probed.files, probed.target_bin, outcome, errorName,
                        message)) {
        if (errorName.empty()) {
            errorName = "IoError";
        }
        return false;
    }
    std::string output;
    const int status = IngestCommandAnswer(outcome, probed.ignored, output);
    return ReadCliCommandResult(output, status == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::IngestMedia(const std::string& mediaPath, bool recursive,
                                 const std::optional<Ulid>& targetBin,
                                 std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    ProbedIngest probed;
    if (!RunProbeIngest(FreezeIngestTarget(), mediaPath, recursive, probed,
                        errorName, message)) {
        return false;
    }
    probed.target_bin = targetBin;
    return CommitIngest(probed, resultJson, errorName, message);
}

bool McpLiveBackend::RunReadResolveTimeline(const std::string& manifestPath,
                                            const std::string& resolveDatabase,
                                            ResolveTimelineFiles& files,
                                            std::string& errorName,
                                            std::string& message) {
    // As McpProjectBackend::ImportResolveTimeline: a relative path would be
    // read against the app's working directory, which is no one's.
    if (!std::filesystem::path(manifestPath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "manifest path must be absolute";
        return false;
    }
    if (!resolveDatabase.empty() &&
        !std::filesystem::path(resolveDatabase).is_absolute()) {
        errorName = "ValidationFailed";
        message = "resolve_database must be absolute";
        return false;
    }
    files = ResolveTimelineFiles{};
    files.status = ReadResolveTimelineImportInput(manifestPath, resolveDatabase,
                                                  files.input, files.output);
    return true;
}

bool McpLiveBackend::CommitResolveTimeline(const ResolveTimelineFiles& files,
                                           std::string& resultJson,
                                           std::string& errorName,
                                           std::string& message) {
    if (files.status != 0) {
        return ReadCliCommandResult(files.output, false, resultJson, errorName,
                                    message);
    }
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message =
            "save the project before reading a Resolve timeline: the rushes "
            "it links to are found by their paths, stored relative to the "
            "package";
        return false;
    }
    std::error_code pathError;
    const std::string projectRoot =
        std::filesystem::absolute(project_path_, pathError)
            .lexically_normal()
            .parent_path()
            .string();
    std::string output;
    ResolveTimelineImportStaged staged;
    int status = PlanResolveTimelineImportStep(document_, projectRoot,
                                               files.input, staged, output);
    if (status == 0) {
        // On copies: a refusal midway leaves the window as it was, as the
        // command leaves the package uncommitted.
        Document document = document_;
        EditLog log = edit_log_;
        const size_t applied = log.AppliedCount();
        status = ApplyResolveTimelineImportStep(document, log, files.input,
                                                staged, output);
        if (status == 0) {
            // SAVING_ROADMAP.md, a write refused under the editor (S3) --
            // committed before the window takes it, as every other edit of
            // this backend: a refused write leaves the window as it was and
            // is refused by name (IoError), never answered with the import.
            if (!CommitAndTake(std::move(document), std::move(log), errorName,
                               message)) {
                return false;
            }
            // PL1 -- each entry is the agent's, so that end_intervention
            // folds the import with the rest of its turn.
            const std::vector<Entry>& entries = edit_log_.AppliedEntries();
            for (size_t index = applied; index < entries.size(); ++index)
                agent_entries_.insert(
                    CanonicalHash(SerializeOperation(entries[index].op)));
            // Shown once, as one edit of the mouse would be.
            if (on_applied_) on_applied_();
        }
    }
    return ReadCliCommandResult(output, status == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::ImportResolveTimeline(const std::string& manifestPath,
                                           const std::string& resolveDatabase,
                                           std::string& resultJson,
                                           std::string& errorName,
                                           std::string& message) {
    ResolveTimelineFiles files;
    return RunReadResolveTimeline(manifestPath, resolveDatabase, files,
                                  errorName, message) &&
           CommitResolveTimeline(files, resultJson, errorName, message);
}

std::unique_ptr<McpBackend> McpLiveBackend::ForkForTask(MediaTaskRunner runner,
                                                        std::string& message) {
    if (project_path_.empty()) {
        message = "no project is open";
        return nullptr;
    }
    McpProjectBackend attached(project_path_, require_explicit_timeline_);
    return attached.ForkForTask(std::move(runner), message);
}

std::string McpLiveBackend::ProjectFolder() const {
    return McpProjectFolder(project_path_);
}

std::string McpLiveBackend::PackageDirectory() const {
    if (project_path_.empty()) return {};
    return std::filesystem::absolute(project_path_)
        .parent_path()
        .lexically_normal()
        .string();
}

bool McpLiveBackend::CommitRunStep(const RunStepRequest& request,
                                   std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    if (!commit_run_step_) {
        errorName = "UnsupportedOperation";
        message = "this live backend has no durable run step committer";
        return false;
    }
    if (!commit_run_step_(request, resultJson, errorName, message)) {
        return false;
    }
    mcp_json::Value answer;
    std::string parseError;
    if (!request.plan_json.empty() &&
        request.timeline_id == document_.sequence.id &&
        mcp_json::Value::Parse(resultJson, answer, parseError) &&
        answer.Find("replayed") != nullptr &&
        !answer.Find("replayed")->AsBool()) {
        RememberAgentEntry();
    }
    return true;
}

bool McpLiveBackend::ReadRunStep(const Ulid& stepId, std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    if (!ReadStoredRunStep(project_path_, stepId, resultJson, errorName,
                           message)) {
        return false;
    }
    if (!ValidateRunStepState(resultJson, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    return true;
}

bool McpLiveBackend::ValidateRunStepState(std::string& resultJson,
                                          std::string& message) {
    mcp_json::Value answer;
    if (!mcp_json::Value::Parse(resultJson, answer, message)) {
        return false;
    }
    const auto* found = answer.Find("found");
    if (found == nullptr || !found->IsBool()) {
        return false;
    }
    if (!found->AsBool()) {
        return true;
    }
    const auto* bytes = answer.Find("receipt_json");
    const auto* storedMatches = answer.Find("matches_current");
    RunStepReceipt receipt;
    if (bytes == nullptr || !bytes->IsString() || storedMatches == nullptr ||
        !storedMatches->IsBool() ||
        !ParseRunStepReceipt(bytes->AsString(), receipt, message)) {
        return false;
    }
    answer.Set(
        "matches_current",
        mcp_json::Value::MakeBool(storedMatches->AsBool() && match_run_step_ &&
                                  match_run_step_(receipt)));
    resultJson = answer.Dump();
    return true;
}

bool McpLiveBackend::ProjectArchiveStatus(bool retry, std::string& resultJson,
                                          std::string& errorName,
                                          std::string& message) {
    if (project_path_.empty()) {
        errorName = "UnsupportedOperation";
        message = "this backend has no stored project package";
        return false;
    }
    McpProjectBackend attached(project_path_);
    return attached.ProjectArchiveStatus(retry, resultJson, errorName, message);
}

bool McpLiveBackend::ImportProjectArchive(
    const std::string& database, const std::vector<std::string>& projectPaths,
    std::string& resultJson, std::string& errorName, std::string& message) {
    McpProjectBackend attached;
    return attached.ImportProjectArchive(database, projectPaths, resultJson,
                                         errorName, message);
}

bool McpLiveBackend::ReadEditorView(EditorView& view, std::string& message) {
    if (!read_editor_view_) return McpBackend::ReadEditorView(view, message);
    return read_editor_view_(view, message);
}

bool McpLiveBackend::AccessVideoOutput(const std::optional<std::string>& screen,
                                       VideoOutputState& state,
                                       std::string& errorName,
                                       std::string& message) {
    if (!video_output_) {
        return McpBackend::AccessVideoOutput(screen, state, errorName, message);
    }
    return video_output_(screen, state, errorName, message);
}

bool McpLiveBackend::CaptureEditorWindow(const EditorCaptureRequest& request,
                                         EditorCapture& capture,
                                         std::string& errorName,
                                         std::string& message) {
    if (!capture_editor_window_)
        return McpBackend::CaptureEditorWindow(request, capture, errorName,
                                               message);
    return capture_editor_window_(request, capture, errorName, message);
}

bool McpLiveBackend::SnapshotProject(Project& project, std::string& message) {
    if (snapshot_project_) {
        return snapshot_project_(project, message);
    }
    project = Project::FromDocument(document_);
    return true;
}

bool McpLiveBackend::ExportProjectTimelines(
    const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
    std::string& resultJson, std::string& errorName, std::string& message) {
    Project project;
    if (!SnapshotProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message = "save the project before exporting relative media paths";
        return false;
    }
    return Exporter::ExportBatch(project, project_path_, items, preview,
                                 resultJson, errorName, message);
}

bool McpLiveBackend::ExportTimeline(const ExportSettings& settings,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message = "save the project before exporting relative media paths";
        return false;
    }
    ExportPlan plan;
    if (!Exporter::BuildPlan(document_, project_path_, settings, plan,
                             message)) {
        errorName = "InvalidExport";
        return false;
    }
    if (!Exporter::Run(plan, {}, nullptr, message)) {
        errorName = "ExportFailed";
        return false;
    }
    resultJson = Exporter::ResultJson(plan);
    return true;
}

bool McpLiveBackend::VerifyExportedFile(const std::string& exportPath,
                                        std::string& resultJson,
                                        std::string& errorName,
                                        std::string& message) {
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message = "save the project before verifying relative media paths";
        return false;
    }
    Document document;
    if (!SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    ExportVerificationReport report;
    if (!VerifyExport(document,
                      std::filesystem::absolute(project_path_).parent_path(),
                      exportPath, {}, report, message)) {
        errorName = "IoError";
        return false;
    }
    resultJson = SerializeExportVerification(report);
    return true;
}

bool McpLiveBackend::ExportResolveXml(const std::string& xmlPath,
                                      const std::string& resolveScaling,
                                      std::string& resultJson,
                                      std::string& errorName,
                                      std::string& message) {
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message =
            "save the project before writing a Resolve XML: the rush paths "
            "it names are stored relative to the package";
        return false;
    }
    if (!std::filesystem::path(xmlPath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "xml path must be absolute";
        return false;
    }
    ResolveScaling scaling = ResolveScaling::Undeclared;
    if (!resolveScaling.empty() &&
        !ParseResolveScaling(resolveScaling, scaling)) {
        errorName = "ValidationFailed";
        message = "resolve_scaling must be 'fit' or 'fill'";
        return false;
    }
    Document document;
    if (!SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return BuildAndWriteResolveXml(
        std::move(document),
        std::filesystem::absolute(project_path_).parent_path().string(),
        xmlPath, resultJson, errorName, message, scaling);
}

// R6 -- docs/roadmap/REEL-2026-09.md. The same two steps the headless
// backend takes, in the same order: the bytes into the package, then the
// reference into the document through the ordinary operation path.
bool McpLiveBackend::ImportSequenceLut(const std::string& cubePath,
                                       const std::string& space,
                                       std::string& resultJson,
                                       std::string& errorName,
                                       std::string& message) {
    if (project_path_.empty()) {
        errorName = "InvalidDocument";
        message =
            "save the project before importing a look LUT: the .cube is "
            "copied into the package beside it";
        return false;
    }
    if (!std::filesystem::path(cubePath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "LUT path must be absolute";
        return false;
    }
    Document document;
    if (!SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    SequenceLut lut;
    if (!ImportLookLutIntoPackage(project_path_, cubePath, space,
                                  document.color_management, lut, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    return ApplyOperation(SetSequenceLutOperation{lut}, resultJson, errorName,
                          message);
}

bool McpLiveBackend::AnalyzeTimelineAudio(const TimelineAudioSettings& settings,
                                          std::string& resultJson,
                                          std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before measuring its audio";
        return false;
    }
    Document document;
    if (!SnapshotDocument(document, message)) return false;
    std::map<Ulid, SpeechOnsetReport> floors;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips) {
            if (floors.count(clip.source_id)) continue;
            SpeechOnsetReport onset;
            std::string ignored;
            if (ReadSourceSpeechOnset(clip.source_id, onset, ignored))
                floors.emplace(clip.source_id, std::move(onset));
        }
    }
    TimelineAudioReport report;
    if (!::AnalyzeTimelineAudio(document, project_path_, floors, settings,
                                report, message))
        return false;
    resultJson =
        SerializeTimelineAudioReport(report, document.sequence.frame_rate);
    return true;
}

bool McpLiveBackend::AnalyzeSourceHesitations(
    const Ulid& sourceId, const HesitationSettings& settings,
    HesitationReport& report, std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before analysing its rushes";
        return false;
    }
    return AnalyzeProjectSourceHesitations(project_path_, document_.library,
                                           sourceId, settings, report, message);
}

bool McpLiveBackend::ReadSourceHesitations(const Ulid& sourceId,
                                           HesitationReport& report,
                                           std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before reading its caches";
        return false;
    }
    return ReadProjectSourceHesitations(project_path_, sourceId, report,
                                        message);
}

bool McpLiveBackend::ReadTimelineWordSpans(
    std::vector<TimelineTranscriptSpan>& spans, TimelineWordCheck& check,
    std::string& message) {
    Document document;
    if (!SnapshotDocument(document, message)) {
        return false;
    }
    return ReadDocumentWordSpans(document, spans, check, message);
}

bool McpLiveBackend::ReadDocumentWordSpans(
    const Document& document, std::vector<TimelineTranscriptSpan>& spans,
    TimelineWordCheck& check, std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before reading its transcripts";
        return false;
    }
    return BuildTimelineTranscriptWordSpans(
        document,
        std::filesystem::absolute(project_path_).parent_path() / ".cutmachine" /
            "transcripts",
        spans, message, &check);
}

bool McpLiveBackend::ApplyOperation(Operation operation,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    EditError error = EditError::None;
    // S3 -- applied to copies, taken once committed (CommitAndTake).
    Document staged = document_;
    EditLog stagedLog = edit_log_;
    // MOTIF2 -- the call's motif rides on the entry.
    std::vector<EntryMotif> motifs = EntryMotifsForCall(operation);
    if (!stagedLog.Apply(staged, std::move(operation), error, message,
                         std::move(motifs))) {
        errorName = EditErrorName(error);
        return false;
    }
    // ITW7 -- the chat agent reads the same delta as a CLI or MCP caller,
    // created identities included; it used to receive {"ok":true} alone.
    DocumentDelta delta;
    ComputeDocumentDelta(document_, staged, delta);
    if (!CommitAndTake(std::move(staged), std::move(stagedLog), errorName,
                       message)) {
        return false;
    }
    RememberAgentEntry();
    resultJson =
        "{\"ok\":true,\"delta\":" + SerializeDocumentDelta(delta) + '}';
    if (on_applied_) on_applied_();
    return true;
}

bool McpLiveBackend::CommitAndTake(Document staged, EditLog stagedLog,
                                   std::string& errorName,
                                   std::string& message) {
    std::string refused;
    if (commit_edit_ && !commit_edit_(staged, stagedLog, refused)) {
        errorName = "IoError";
        message =
            "the edit was not saved, so the window did not take it: " + refused;
        return false;
    }
    document_ = std::move(staged);
    edit_log_ = std::move(stagedLog);
    return true;
}

bool McpLiveBackend::ApplyProjectEdit(ProjectOperation operation,
                                      std::string& resultJson,
                                      std::string& errorName,
                                      std::string& message) {
    if (!apply_project_) {
        errorName = "UnsupportedOperation";
        message = "the live backend cannot create project timelines";
        return false;
    }
    return apply_project_(std::move(operation), ProjectEntryMotifsForCall(),
                          resultJson, errorName, message);
}

bool McpLiveBackend::ReadTimelineTranscript(std::string& json,
                                            std::string& message) {
    if (!read_transcript_) {
        message = "the live backend has no transcript reader";
        return false;
    }
    return read_transcript_(json, message);
}

bool McpLiveBackend::ReadSourceTranscript(const Ulid& sourceId,
                                          Transcript& transcript,
                                          std::string& message) {
    if (!read_source_transcript_) {
        message = "the live backend has no transcript reader";
        return false;
    }
    return read_source_transcript_(sourceId, transcript, message);
}

ShotQualityCacheRead McpLiveBackend::ReadSourceShotQuality(
    const Ulid& sourceId, ShotQualityReport& report) {
    if (!read_source_shot_quality_) {
        return {ShotQualityCacheState::Absent, 0,
                "the live backend has no shot quality reader"};
    }
    return read_source_shot_quality_(sourceId, report);
}

bool McpLiveBackend::ReadSourceAudioBeats(const Ulid& sourceId,
                                          BeatDetectionResult& result,
                                          std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before reading audio beat caches";
        return false;
    }
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "beats" /
                                       (sourceId + ".beats");
    return LoadAudioBeats(path.string(), result, message);
}

// PA3 -- docs/roadmap/PARITE-2026-11.md.
bool McpLiveBackend::ReadSourceSpeakers(const Ulid& sourceId,
                                        SpeakerDiarization& speakers,
                                        std::string& message) {
    if (project_path_.empty()) {
        message = "save the project before reading speakers caches";
        return false;
    }
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "speakers" /
                                       (sourceId + ".json");
    return LoadSpeakerDiarization(path.string(), speakers, message);
}

bool McpLiveBackend::AnalyzeSourceSpeakers(const Ulid& sourceId,
                                           int32_t speakers,
                                           std::string& resultJson,
                                           std::string& message) {
    return RunAnalyzeSourceSpeakers(FreezeCacheTarget(), sourceId, speakers,
                                    resultJson, message);
}

bool McpLiveBackend::RunAnalyzeSourceSpeakers(const CacheTarget& target,
                                              const Ulid& sourceId,
                                              int32_t speakers,
                                              std::string& resultJson,
                                              std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before telling its voices apart";
        return false;
    }
    std::string output;
    const int result = AnalyzeSpeakersCommand(target.project_path, sourceId,
                                              speakers, output, target.runner);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::AnalyzeSourceAudioBeats(const Ulid& sourceId,
                                             std::string& resultJson,
                                             std::string& message) {
    return RunAnalyzeSourceAudioBeats(FreezeCacheTarget(), sourceId, resultJson,
                                      message);
}

bool McpLiveBackend::AnalyzeSourceShotQuality(const Ulid& sourceId,
                                              std::string& resultJson,
                                              std::string& message) {
    return RunAnalyzeSourceShotQuality(FreezeCacheTarget(), sourceId,
                                       resultJson, message);
}

bool McpLiveBackend::ScanIsolatedFrames(bool analyze,
                                        const std::vector<Ulid>& mediaIds,
                                        std::string& resultJson,
                                        std::string& message) {
    return RunScanIsolatedFrames(FreezeCacheTarget(), analyze, mediaIds,
                                 resultJson, message);
}

bool McpLiveBackend::ReadSourceSpeechOnset(const Ulid& sourceId,
                                           SpeechOnsetReport& report,
                                           std::string& message) {
    if (!read_source_speech_onset_) {
        message = "the live backend has no speech onset reader";
        return false;
    }
    return read_source_speech_onset_(sourceId, report, message);
}

bool McpLiveBackend::AnalyzeSourceSpeechOnset(
    const Ulid& sourceId, const SpeechOnsetSettings& settings,
    std::string& resultJson, std::string& message) {
    return RunAnalyzeSourceSpeechOnset(FreezeCacheTarget(), sourceId, settings,
                                       resultJson, message);
}

bool McpLiveBackend::MeasureAudioOffset(const Ulid& referenceId,
                                        const Ulid& otherId,
                                        const AudioOffsetChannels& channels,
                                        std::string& resultJson,
                                        std::string& errorName,
                                        std::string& message) {
    return RunMeasureAudioOffset(FreezeCacheTarget(), referenceId, otherId,
                                 channels, resultJson, errorName, message);
}

bool McpLiveBackend::RunMeasureAudioOffset(
    const CacheTarget& target, const Ulid& referenceId, const Ulid& otherId,
    const AudioOffsetChannels& channels, std::string& resultJson,
    std::string& errorName, std::string& message) {
    if (target.project_path.empty()) {
        errorName = "InvalidDocument";
        message = "save the project before measuring its rushes";
        return false;
    }
    std::string output;
    const int result =
        MeasureAudioOffsetCommand(target.project_path, referenceId, otherId,
                                  channels, output, target.runner);
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::ProposeDucking(const Ulid& mediaId,
                                    AudioOffsetChannel channel,
                                    const audio_dsp::DuckingSettings& settings,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    return RunProposeDucking(FreezeCacheTarget(), mediaId, channel, settings,
                             resultJson, errorName, message);
}

bool McpLiveBackend::RunProposeDucking(
    const CacheTarget& target, const Ulid& mediaId, AudioOffsetChannel channel,
    const audio_dsp::DuckingSettings& settings, std::string& resultJson,
    std::string& errorName, std::string& message) {
    if (target.project_path.empty()) {
        errorName = "InvalidDocument";
        message = "save the project before analysing its rushes";
        return false;
    }
    std::string output;
    const int result = ProposeDuckingCommand(
        target.project_path, mediaId, channel, settings, output, target.runner);
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

McpLiveBackend::CacheTarget McpLiveBackend::FreezeCacheTarget() const {
    return {project_path_, document_.sequence.id, media_task_runner_};
}

bool McpLiveBackend::RunTranscribeSources(
    const CacheTarget& target, const std::vector<Ulid>& sourceIds,
    const std::string& language, bool verbatim, bool includeSilent,
    bool overwrite, std::string& resultJson, std::string& message,
    const WhisperDecodingRequest& profile) {
    if (target.project_path.empty()) {
        message = "save the project before transcribing its rushes";
        return false;
    }
    // The empty model path resolves the locally configured model (LocalEnv.h)
    // exactly as McpProjectBackend does: the app's own model preference
    // belongs to its Media menu, not to the tool catalog.
    std::string output;
    const int result = TranscribeMediaCommand(
        target.project_path, sourceIds, "", language, verbatim, includeSilent,
        output, target.runner, nullptr, overwrite, profile);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunTranscribeTimeline(
    const CacheTarget& target, const std::string& timelineId,
    const std::string& language, bool verbatim, std::string& resultJson,
    std::string& message, const WhisperDecodingRequest& profile) {
    if (target.project_path.empty()) {
        message = "save the project before transcribing a timeline";
        return false;
    }
    std::string output;
    const int result = TranscribeTimelineCommand(
        target.project_path,
        timelineId.empty() ? target.timeline_id : timelineId, language,
        verbatim, output, target.runner, nullptr, profile);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunAlignSourceTranscripts(const CacheTarget& target,
                                               bool apply,
                                               std::string& resultJson,
                                               std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before aligning its transcripts";
        return false;
    }
    std::string output;
    const int result =
        AlignTranscriptsCommand(target.project_path, apply, output);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunAlignSourceWords(const CacheTarget& target,
                                         const std::vector<Ulid>& mediaIds,
                                         const std::string& textsJson,
                                         std::string& resultJson,
                                         std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before aligning its words";
        return false;
    }
    std::string output;
    const int result = AlignWordsCommand(target.project_path, mediaIds,
                                         textsJson, output, target.runner);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunAnalyzeSourceShotQuality(const CacheTarget& target,
                                                 const Ulid& sourceId,
                                                 std::string& resultJson,
                                                 std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before analysing its rushes";
        return false;
    }
    // Same command `--shot-quality` runs, so a grade produced from the chat
    // panel is the same artifact the CLI and the MCP server write.
    std::string output;
    const int result = AnalyzeShotQualityCommand(target.project_path, sourceId,
                                                 output, target.runner);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunScanIsolatedFrames(const CacheTarget& target,
                                           bool analyze,
                                           const std::vector<Ulid>& mediaIds,
                                           std::string& resultJson,
                                           std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before scanning it for isolated frames";
        return false;
    }
    // Same command `--isolated-frames` runs, so a scan produced from the
    // chat panel is the same artifact the CLI and the MCP server write.
    std::string output;
    const int result =
        IsolatedFramesCommand(target.project_path, target.timeline_id, analyze,
                              output, target.runner, mediaIds);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunAnalyzeSourceSpeechOnset(
    const CacheTarget& target, const Ulid& sourceId,
    const SpeechOnsetSettings& settings, std::string& resultJson,
    std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before analysing its rushes";
        return false;
    }
    std::string output;
    const int result = AnalyzeSpeechOnsetCommand(
        target.project_path, sourceId, output, settings, target.runner);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

bool McpLiveBackend::RunAnalyzeSourceAudioBeats(const CacheTarget& target,
                                                const Ulid& sourceId,
                                                std::string& resultJson,
                                                std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before analyzing audio beats";
        return false;
    }
    // A refusal reads its `detail`, as from McpProjectBackend: this used to
    // hand the model the whole JSON envelope as the message.
    std::string output;
    const int result = AnalyzeAudioBeatsCommand(target.project_path, sourceId,
                                                output, target.runner);
    std::string errorName;
    return ReadCliCommandResult(output, result == 0, resultJson, errorName,
                                message);
}

McpLiveBackend::PictureTarget McpLiveBackend::FreezePictureTarget() const {
    return {document_, project_path_};
}

bool McpLiveBackend::RunCaptureSourceFrame(
    const PictureTarget& target, const Ulid& sourceId, const Ulid& clipId,
    const RationalTime& time, std::string& jpegBytes, std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before reading its frames";
        return false;
    }
    const LibraryMedia* media = target.document.FindLibraryMedia(sourceId);
    if (media == nullptr) {
        message = "unknown media_id '" + sourceId + "'";
        return false;
    }
    if (!media->has_video) {
        message = "media_id '" + sourceId +
                  "' is audio-only; read_frame requires a video stream";
        return false;
    }
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path =
            std::filesystem::absolute(target.project_path).parent_path() / path;
    }
    FrameCaptureSettings settings;
    // G3 -- the media read in its own declared input.
    settings.color_management = target.document.ColorManagementFor(sourceId);
    // PA15 -- where a clip cut out along its people finds its matte.
    settings.package_directory =
        std::filesystem::absolute(target.project_path).parent_path().string();
    if (!ApplyClipFramingToCapture(target.document, clipId, time, settings,
                                   message))
        return false;
    return ::CaptureSourceFrame(path.lexically_normal().string(), time,
                                settings, jpegBytes, message);
}

bool McpLiveBackend::RunCaptureTimelineSheet(
    const PictureTarget& target, const TimelineSheetPlan& plan,
    const TimelineSheetSettings& settings, std::string& jpegBytes,
    std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before rendering a timeline sheet";
        return false;
    }
    return RenderTimelineSheet(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path(), plan,
        settings, jpegBytes, message);
}

bool McpLiveBackend::RunReadMediaFileColor(const PictureTarget& target,
                                           const Ulid& mediaId,
                                           MediaFileColor& color,
                                           std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before reading its media";
        return false;
    }
    const LibraryMedia* media = target.document.FindLibraryMedia(mediaId);
    if (media == nullptr) {
        message = "unknown media_id '" + mediaId + "'";
        return false;
    }
    std::filesystem::path path(media->path);
    if (path.is_relative()) {
        path =
            std::filesystem::absolute(target.project_path).parent_path() / path;
    }
    return ReadFileColorFacts(path.lexically_normal().string(), color, message);
}

bool McpLiveBackend::ReadMediaFileColor(const Ulid& mediaId,
                                        MediaFileColor& color,
                                        std::string& message) {
    return RunReadMediaFileColor(FreezePictureTarget(), mediaId, color,
                                 message);
}

bool McpLiveBackend::RunMatchClipColor(const PictureTarget& target,
                                       const ColorMatchPlan& plan,
                                       ColorMatchResult& result,
                                       std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before measuring its frames";
        return false;
    }
    return RenderColorMatch(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        plan, ColorMeasureSettings{}, result, message);
}

bool McpLiveBackend::MatchClipColor(const ColorMatchPlan& plan,
                                    ColorMatchResult& result,
                                    std::string& message) {
    return RunMatchClipColor(FreezePictureTarget(), plan, result, message);
}

bool McpLiveBackend::RunProposeClipGrades(const PictureTarget& target,
                                          const GradeProposalPlan& plan,
                                          std::vector<GradeProposal>& proposals,
                                          std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before measuring its frames";
        return false;
    }
    return RenderGradeProposals(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        plan, ColorMeasureSettings{}, proposals, message);
}

// CA3 -- docs/roadmap/CADRAGE-2026-10.md. The faces of one clip of the
// frozen document, as propose_grade reads its frames.
bool McpLiveBackend::RunSampleClipFaces(const PictureTarget& target,
                                        const Ulid& clipId, int64_t everyFrames,
                                        std::vector<SubjectSample>& samples,
                                        std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before reading its frames";
        return false;
    }
    return ::SampleClipFaces(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        clipId, everyFrames, 512, "", samples, message);
}

bool McpLiveBackend::SampleClipFaces(const Ulid& clipId, int64_t everyFrames,
                                     std::vector<SubjectSample>& samples,
                                     std::string& message) {
    return RunSampleClipFaces(FreezePictureTarget(), clipId, everyFrames,
                              samples, message);
}

// PA7 -- docs/roadmap/PARITE-2026-11.md. The movement between the frames
// of one clip of the frozen document.
bool McpLiveBackend::RunSampleClipMotion(
    const PictureTarget& target, const Ulid& clipId,
    std::vector<StabilizationSample>& motion, std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before reading its frames";
        return false;
    }
    return ::SampleClipMotion(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        clipId, "", motion, message);
}

bool McpLiveBackend::SampleClipMotion(const Ulid& clipId,
                                      std::vector<StabilizationSample>& motion,
                                      std::string& message) {
    return RunSampleClipMotion(FreezePictureTarget(), clipId, motion, message);
}

bool McpLiveBackend::ProposeClipGrades(const GradeProposalPlan& plan,
                                       std::vector<GradeProposal>& proposals,
                                       std::string& message) {
    return RunProposeClipGrades(FreezePictureTarget(), plan, proposals,
                                message);
}

bool McpLiveBackend::RunCaptureGradedClipFrame(const PictureTarget& target,
                                               const Ulid& clipId,
                                               const RationalTime& time,
                                               std::string& jpegBytes,
                                               std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before reading its frames";
        return false;
    }
    return ::CaptureGradedClipFrame(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        clipId, time, FrameCaptureSettings{}, jpegBytes, message);
}

bool McpLiveBackend::RunMeasureClipColors(
    const PictureTarget& target, const ColorMeasurePlan& plan,
    std::vector<ColorClipMeasurement>& results, std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before measuring its frames";
        return false;
    }
    return RenderColorMeasurement(
        target.document,
        std::filesystem::absolute(target.project_path).parent_path().string(),
        plan, ColorMeasureSettings{}, results, message);
}

bool McpLiveBackend::RunInspectCut(const PictureTarget& target,
                                   const MediaTaskRunner& runner,
                                   const CutInspectionPlan& plan,
                                   const CutInspectionRenderSettings& settings,
                                   CutInspectionResult& result,
                                   std::string& message) {
    if (target.project_path.empty()) {
        message = "save the project before inspecting a cut";
        return false;
    }
    CutInspectionRenderSettings withRunner = settings;
    withRunner.runner = runner;
    return RenderCutInspection(target.document,
                               std::filesystem::absolute(target.project_path),
                               plan, withRunner, result, message);
}

bool McpLiveBackend::InspectCut(const CutInspectionPlan& plan,
                                const CutInspectionRenderSettings& settings,
                                CutInspectionResult& result,
                                std::string& message) {
    return RunInspectCut(FreezePictureTarget(), media_task_runner_, plan,
                         settings, result, message);
}

bool McpLiveBackend::CaptureGradedClipFrame(const Ulid& clipId,
                                            const RationalTime& time,
                                            std::string& jpegBytes,
                                            std::string& message) {
    return RunCaptureGradedClipFrame(FreezePictureTarget(), clipId, time,
                                     jpegBytes, message);
}

bool McpLiveBackend::MeasureClipColors(
    const ColorMeasurePlan& plan, std::vector<ColorClipMeasurement>& results,
    std::string& message) {
    return RunMeasureClipColors(FreezePictureTarget(), plan, results, message);
}

bool McpLiveBackend::TranscribeSources(const std::vector<Ulid>& sourceIds,
                                       const std::string& language,
                                       bool verbatim, bool includeSilent,
                                       bool overwrite, std::string& resultJson,
                                       std::string& message,
                                       const WhisperDecodingRequest& profile) {
    return RunTranscribeSources(FreezeCacheTarget(), sourceIds, language,
                                verbatim, includeSilent, overwrite, resultJson,
                                message, profile);
}

bool McpLiveBackend::TranscribeTimeline(const std::string& timelineId,
                                        const std::string& language,
                                        bool verbatim, std::string& resultJson,
                                        std::string& message,
                                        const WhisperDecodingRequest& profile) {
    return RunTranscribeTimeline(FreezeCacheTarget(), timelineId, language,
                                 verbatim, resultJson, message, profile);
}

bool McpLiveBackend::AlignSourceTranscripts(bool apply, std::string& resultJson,
                                            std::string& message) {
    return RunAlignSourceTranscripts(FreezeCacheTarget(), apply, resultJson,
                                     message);
}

bool McpLiveBackend::AlignSourceWords(const std::vector<Ulid>& mediaIds,
                                      const std::string& textsJson,
                                      std::string& resultJson,
                                      std::string& message) {
    return RunAlignSourceWords(FreezeCacheTarget(), mediaIds, textsJson,
                               resultJson, message);
}

bool McpLiveBackend::CaptureSourceFrame(const Ulid& sourceId,
                                        const Ulid& clipId,
                                        const RationalTime& time,
                                        std::string& jpegBytes,
                                        std::string& message) {
    return RunCaptureSourceFrame(FreezePictureTarget(), sourceId, clipId, time,
                                 jpegBytes, message);
}

bool McpLiveBackend::CaptureTimelineSheet(const TimelineSheetPlan& plan,
                                          const TimelineSheetSettings& settings,
                                          std::string& jpegBytes,
                                          std::string& message) {
    return RunCaptureTimelineSheet(FreezePictureTarget(), plan, settings,
                                   jpegBytes, message);
}

bool McpLiveBackend::RunAfterRequest(const std::string& requestJson,
                                     std::string& resultJson,
                                     std::string& errorName,
                                     std::string& message) {
    return RunAfterBridgeRequest(requestJson, resultJson, errorName, message);
}

bool McpLiveBackend::Undo(std::string& resultJson, std::string& errorName,
                          std::string& message) {
    EditError error = EditError::None;
    // AX1 -- an agent relayed to the app learns what a reversal put back the
    // way the MCP server tells it, by the delta; {"ok":true} alone left its
    // model of the timeline stale after every undo.
    // S3 -- on copies, taken once committed, as ApplyOperation.
    Document staged = document_;
    EditLog stagedLog = edit_log_;
    if (!stagedLog.Undo(staged, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(document_, staged, delta);
    if (!CommitAndTake(std::move(staged), std::move(stagedLog), errorName,
                       message)) {
        return false;
    }
    resultJson =
        "{\"ok\":true,\"delta\":" + SerializeDocumentDelta(delta) + '}';
    if (on_applied_) on_applied_();
    return true;
}

bool McpLiveBackend::Redo(std::string& resultJson, std::string& errorName,
                          std::string& message) {
    EditError error = EditError::None;
    Document staged = document_;
    EditLog stagedLog = edit_log_;
    if (!stagedLog.Redo(staged, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    DocumentDelta delta;
    ComputeDocumentDelta(document_, staged, delta);
    if (!CommitAndTake(std::move(staged), std::move(stagedLog), errorName,
                       message)) {
        return false;
    }
    RememberAgentEntry();
    resultJson =
        "{\"ok\":true,\"delta\":" + SerializeDocumentDelta(delta) + '}';
    if (on_applied_) on_applied_();
    return true;
}

void McpLiveBackend::RememberAgentEntry() {
    if (edit_log_.AppliedCount() == 0) {
        return;
    }
    agent_entries_.insert(CanonicalHash(
        SerializeOperation(edit_log_.AppliedEntries().back().op)));
}

bool McpLiveBackend::BeginIntervention(std::string& resultJson, std::string&,
                                       std::string&) {
    resultJson = InterventionBeganJson(
        MarkIntervention(document_.sequence.id, edit_log_), document_);
    return true;
}

bool McpLiveBackend::EndIntervention(const std::string& intervention,
                                     const std::string& label,
                                     std::string& resultJson,
                                     std::string& errorName,
                                     std::string& message) {
    InterventionMark mark;
    if (!ParseInterventionMark(intervention, mark, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    // The references this backend holds follow the active timeline, so an
    // intervention that began on another one cannot be reached from here.
    if (mark.timeline_id != document_.sequence.id) {
        errorName = "UnknownSequence";
        message = "the intervention began on timeline '" + mark.timeline_id +
                  "', which is no longer the one being edited; its history "
                  "is left as it was";
        return false;
    }
    const std::vector<Entry>& entries = edit_log_.AppliedEntries();
    for (size_t index = mark.history_depth; index < entries.size(); ++index) {
        if (agent_entries_.count(
                CanonicalHash(SerializeOperation(entries[index].op))) == 0) {
            errorName = "InvalidOperation";
            message =
                "the timeline was also edited by hand during the "
                "intervention; its history is left as it was, so that one "
                "Undo never reverts a person's own edit";
            return false;
        }
    }
    size_t grouped = 0;
    EditError error = EditError::None;
    // S3 -- the fold rewrites the journal: on copies, taken once committed.
    Document staged = document_;
    EditLog stagedLog = edit_log_;
    if (!stagedLog.GroupSince(staged, mark, label, grouped, error, message,
                              CurrentEditMotifs().motif)) {
        errorName = EditErrorName(error);
        return false;
    }
    // PRD40 -- a single step is folded too, under its label.
    if (grouped > 0) {
        // Commits the journal: the document itself did not change.
        if (!CommitAndTake(std::move(staged), std::move(stagedLog), errorName,
                           message)) {
            return false;
        }
        RememberAgentEntry();
        if (on_applied_) {
            on_applied_();
        }
    }
    resultJson = InterventionEndedJson(grouped, edit_log_, document_);
    return true;
}

// PRD35 -- ROADMAP.md. The journal the window edits, in memory: the one
// every mouse gesture and every call of the panel append to.
bool McpLiveBackend::ReadTimelineHistory(TimelineHistory& history, std::string&,
                                         std::string&) {
    history = SummarizeHistory(document_.sequence.id, edit_log_);
    return true;
}

// PA9 -- the same journal, as what changed since a state.
bool McpLiveBackend::ReadTimelineChangesJson(const std::string& sinceHash,
                                             std::string& json,
                                             std::string& errorName,
                                             std::string& message) {
    TimelineChanges changes;
    if (!ReadTimelineChanges(document_, edit_log_, sinceHash, changes,
                             message)) {
        errorName = "InvalidDocument";
        return false;
    }
    json = TimelineChangesJson(changes);
    return true;
}

// MOTIF4 -- the same journal, grouped by motif.
bool McpLiveBackend::ReadTimelineMotifs(
    const std::optional<MotifReason>& reason, const std::string& rule,
    std::string& json, std::string&, std::string&) {
    json = ListMotifsJson(document_.sequence.id, edit_log_, reason, rule);
    return true;
}

// MOTIF6 -- the project's journal, as the window last committed it: each
// project edit, the agent's (apply_project_) and the window's own, is
// written to the package before it answers. A window whose project was
// never saved keeps no journal on disk to read.
bool McpLiveBackend::ReadProjectMotifs(const std::optional<MotifReason>& reason,
                                       const std::string& rule,
                                       std::string& json,
                                       std::string& errorName,
                                       std::string& message) {
    if (project_path_.empty()) {
        errorName = "UnsupportedOperation";
        message = "save the project before reading its project journal";
        return false;
    }
    Project project;
    if (!SnapshotProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    ProjectEditLog log;
    EditError error = EditError::None;
    if (!LoadProjectEditLogIfAny(project_path_, log, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    json = ListProjectMotifsJson(project, log, reason, rule);
    return true;
}

bool McpLiveBackend::Describe(std::string& json, std::string& message) {
    if (describe_project_) return describe_project_(json, message);
    json = DescribeDocument(document_);
    return true;
}
