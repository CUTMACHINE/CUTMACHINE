#include "McpProjectBackend.h"
#include "RunStepStorage.h"

#include "AfterBridge.h"
#include "CanonicalHash.h"
#include "Cli.h"
#include "ExportVerification.h"
#include "FrameCapture.h"
#include "Ingest.h"
#include "InterviewShort.h"
#include "Json.h"
#include "LookLutStorage.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "ResolveImport.h"
#include "ResolveXmlExport.h"
#include "WordAlignment.h"

#include <algorithm>

namespace {

// Cli.cc's headless commands each return `{"ok":true,...}` or
// `{"ok":false,"error":"<EditErrorName>","detail":"..."}`. Unpack that
// shared shape once instead of duplicating the parse in every method below;
// the live backend reads it through the same function (Cli.h).
bool ParseCommandResult(const std::string& output, bool commandSucceeded,
                        std::string& resultJson, std::string& errorName,
                        std::string& message) {
    return ReadCliCommandResult(output, commandSucceeded, resultJson, errorName,
                                message);
}

}  // namespace

McpProjectBackend::McpProjectBackend(std::string projectPath,
                                     bool requireExplicitTimeline)
    : project_path_(std::move(projectPath)),
      require_explicit_timeline_(requireExplicitTimeline) {}

bool McpProjectBackend::CommitRunStep(const RunStepRequest& request,
                                      std::string& resultJson,
                                      std::string& errorName,
                                      std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    Project committed;
    EditLog log;
    const bool ok = CommitStoredRunStep(project_path_, request, committed, log,
                                        resultJson, errorName, message);
    InvalidateCache();
    return ok;
}

bool McpProjectBackend::ReadRunStep(const Ulid& stepId, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    return ReadStoredRunStep(project_path_, stepId, resultJson, errorName,
                             message);
}

bool McpProjectBackend::ProjectArchiveStatus(bool retry,
                                             std::string& resultJson,
                                             std::string& errorName,
                                             std::string& message) {
    // DEC44 -- a tool's answer says where the archive's copies stand.
    resultJson = StoredProjectArchiveReportJson(
                     StoredProjectArchiveStatus(project_path_, retry, true))
                     .Dump();
    errorName.clear();
    message.clear();
    return true;
}

bool McpProjectBackend::ImportProjectArchive(
    const std::string& database, const std::vector<std::string>& projectPaths,
    std::string& resultJson, std::string& errorName, std::string& message) {
    mcp_json::Value result;
    if (!ImportStoredProjectsToArchive(database, projectPaths, result,
                                       errorName, message)) {
        return false;
    }
    resultJson = result.Dump();
    return true;
}

bool McpProjectBackend::CreateProject(const std::string& packagePath,
                                      const std::string& name,
                                      const std::string& format,
                                      std::string& resultJson,
                                      std::string& errorName,
                                      std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    if (!std::filesystem::path(packagePath).is_absolute()) {
        errorName = "ValidationFailed";
        message =
            "project path must be absolute, not relative to wherever "
            "the server was started";
        return false;
    }
    // Same command `--create-project` runs: same format grammar, same refusal
    // of an existing destination, same initial timeline.
    std::string output;
    const int result = CreateProjectCommand(packagePath, name, output, format);
    if (!ParseCommandResult(output, result == 0, resultJson, errorName,
                            message))
        return false;
    mcp_json::Value created;
    std::string parseError;
    const mcp_json::Value* path =
        mcp_json::Value::Parse(resultJson, created, parseError)
            ? created.Find("path")
            : nullptr;
    if (path == nullptr || !path->IsString()) {
        errorName = "IoError";
        message = "project creation did not report its path";
        return false;
    }
    std::string opened;
    if (!OpenProject(path->AsString(), opened, errorName, message)) {
        message = "project created at '" + path->AsString() +
                  "' but not opened: " + message;
        return false;
    }
    return true;
}

bool ResolveMcpProjectPath(const std::string& requestedPath,
                           std::string& projectPath, std::string& message) {
    std::filesystem::path path(requestedPath);
    if (!path.is_absolute()) {
        message =
            "project path must be absolute, not relative to wherever "
            "the server was started";
        return false;
    }
    std::error_code directoryError;
    if (std::filesystem::is_directory(path, directoryError))
        path /= "project.cutmachine.json";
    projectPath = path.lexically_normal().string();
    return true;
}

std::string McpProjectFolder(const std::string& projectPath) {
    if (projectPath.empty()) {
        return {};
    }
    return std::filesystem::absolute(projectPath)
        .lexically_normal()
        .parent_path()
        .parent_path()
        .string();
}

bool McpProjectBackend::OpenProject(const std::string& requestedPath,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    std::string projectPath;
    if (!ResolveMcpProjectPath(requestedPath, projectPath, message)) {
        errorName = "ValidationFailed";
        return false;
    }

    const std::string previousPath = project_path_;
    const bool previousLocked = session_lock_->Held();
    Project project;
    if (projectPath == previousPath && previousLocked) {
        if (!LoadProject(project, message)) {
            errorName = "ParseError";
            return false;
        }
    } else {
        // PRD17 -- ROADMAP.md. The next project is locked and read first,
        // and the one held is let go only once it is: a wrong path, a
        // project held elsewhere or a package that does not parse used to
        // leave a series' server -- every agent sharing it -- with no
        // project at all, the previous lock released before the attempt.
        // The lock is a try-lock, so holding two for that moment cannot
        // deadlock against a server opening the other way.
        auto next = std::make_unique<ProjectSessionLock>();
        std::string owner;
        const bool locked = next->Acquire(projectPath, owner, message);
        const uint64_t generation = StoredProjectCommitGeneration();
        std::error_code sizeError;
        const std::uintmax_t size =
            std::filesystem::file_size(projectPath, sizeError);
        std::error_code timeError;
        const std::filesystem::file_time_type writeTime =
            std::filesystem::last_write_time(projectPath, timeError);
        if (!locked || !LoadStoredProject(projectPath, project, message)) {
            // PRD20 -- a path where nothing exists is not a package that
            // would not parse (ProjectStorage.h).
            errorName = locked && !IsMissingProjectMessage(message)
                            ? "ParseError"
                            : "IoError";
            next->Release();
            message = "cannot open '" + projectPath + "': " + message +
                      (previousPath.empty()
                           ? ""
                           : "; '" + previousPath + "' is still open");
            return false;
        }
        session_lock_->Release();
        session_lock_ = std::move(next);
        project_path_ = projectPath;
        selected_timeline_id_.clear();
        InvalidateCache();
        // DEC24 -- SAVING_ROADMAP.md. A journal written before S6 is
        // migrated under the lock just taken, as the editor does at its
        // opening. Only the journal is written, and nothing of it decides
        // whether the project opens: a journal that does not read fails
        // the project operation that needs it, as before.
        {
            ProjectEditLogMigration ignoredMigration;
            std::string ignoredError;
            MigrateProjectEditLog(projectPath, ignoredMigration, ignoredError);
        }
        // The read that proved the project opens is the first snapshot
        // every later call reuses, stamped as LoadProject stamps one.
        if (!sizeError && !timeError) {
            cached_project_ = project;
            cached_generation_ = generation;
            cached_size_ = size;
            cached_write_time_ = writeTime;
            cache_valid_ = true;
        }
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
    resultJson = root.Dump();
    return true;
}

bool McpProjectBackend::IngestMedia(const std::string& mediaPath,
                                    bool recursive,
                                    const std::optional<Ulid>& targetBin,
                                    std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    if (!std::filesystem::path(mediaPath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "media path must be absolute";
        return false;
    }
    std::string output;
    const int result =
        IngestCommand(project_path_, mediaPath, recursive, output, targetBin);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// R10 -- docs/roadmap/REEL-2026-09.md. Same shape as IngestMedia: an absolute
// path in, the headless command's own refusal out, under the lock this
// backend already holds.
bool McpProjectBackend::ImportResolveTimeline(
    const std::string& manifestPath, const std::string& resolveDatabase,
    std::string& resultJson, std::string& errorName, std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    if (!std::filesystem::path(manifestPath).is_absolute()) {
        errorName = "ValidationFailed";
        message = "manifest path must be absolute";
        return false;
    }
    // DEC15 (GR15) -- as the manifest: a relative path would be read
    // against the server's working directory.
    if (!resolveDatabase.empty() &&
        !std::filesystem::path(resolveDatabase).is_absolute()) {
        errorName = "ValidationFailed";
        message = "resolve_database must be absolute";
        return false;
    }
    std::string output;
    // PRD17 -- into the timeline the call named (SelectTimelineForEdit).
    const int result =
        ImportResolveTimelineCommand(project_path_, manifestPath, output,
                                     selected_timeline_id_, resolveDatabase);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// R9 -- docs/roadmap/REEL-2026-09.md. Goes through the same headless
// command `--export-resolve-xml` takes, so the file an agent asks for and
// the file the CLI writes cannot drift apart. It reads the project and
// writes one file outside it: no edit rights, no cache to drop.
bool McpProjectBackend::ExportResolveXml(const std::string& xmlPath,
                                         const std::string& resolveScaling,
                                         std::string& resultJson,
                                         std::string& errorName,
                                         std::string& message) {
    if (!HasOpenProject()) {
        errorName = "UnsupportedOperation";
        message = "no project is open";
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
    std::string output;
    const int result = ExportResolveXmlCommand(project_path_, xmlPath, output,
                                               selected_timeline_id_, scaling);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// R6 -- docs/roadmap/REEL-2026-09.md. The bytes go into the package first
// and the reference into the document second, so a document can never name
// a LUT the package does not hold. The declared space is checked before
// anything is written (ImportLookLutIntoPackage), by the same function
// Document::Validate calls.
bool McpProjectBackend::ImportSequenceLut(const std::string& cubePath,
                                          const std::string& space,
                                          std::string& resultJson,
                                          std::string& errorName,
                                          std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    if (!HasOpenProject()) {
        errorName = "UnsupportedOperation";
        message = "no project is open";
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

// See McpProjectBackend.h for why a session-lifetime cache is sound here.
// The size/mtime comparison is only a second opinion, for a package edited
// outside CUTMACHINE altogether: it cannot stand on its own, because two
// consecutive edits can leave the project file the same length and, on
// exFAT's two-second timestamp resolution, the same mtime. What makes the
// cache correct is the commit generation, read before the parse: a commit
// that lands during it moves the generation past the one the cache keeps.
bool McpProjectBackend::LoadProject(Project& project, std::string& message) {
    // PRD16 -- a background export reads the project it was asked on, and
    // nothing an edit commits meanwhile.
    if (pinned_project_ != nullptr) {
        project = *pinned_project_;
        return true;
    }
    const uint64_t generation = StoredProjectCommitGeneration();
    const std::filesystem::path path(project_path_);
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime =
        std::filesystem::last_write_time(path, timeError);
    const bool stamped = !sizeError && !timeError;
    if (cache_valid_ && generation == cached_generation_ && stamped &&
        size == cached_size_ && writeTime == cached_write_time_) {
        project = cached_project_;
        return true;
    }
    InvalidateCache();
    if (!LoadStoredProject(project_path_, project, message)) return false;
    if (stamped) {
        cached_project_ = project;
        cached_generation_ = generation;
        cached_size_ = size;
        cached_write_time_ = writeTime;
        cache_valid_ = true;
    }
    return true;
}

void McpProjectBackend::AdoptCommittedProject(Project project,
                                              uint64_t generationBefore) {
    InvalidateCache();
    // Another commit in between, by another thread of this process, and the
    // project held is no longer known to be the package.
    const uint64_t generation = StoredProjectCommitGeneration();
    if (generation != generationBefore + 1) {
        return;
    }
    const std::filesystem::path path(project_path_);
    std::error_code sizeError;
    const std::uintmax_t size = std::filesystem::file_size(path, sizeError);
    std::error_code timeError;
    const std::filesystem::file_time_type writeTime =
        std::filesystem::last_write_time(path, timeError);
    if (sizeError || timeError) {
        return;
    }
    cached_project_ = std::move(project);
    cached_generation_ = generation;
    cached_size_ = size;
    cached_write_time_ = writeTime;
    cache_valid_ = true;
}

void McpProjectBackend::InvalidateCache() {
    cache_valid_ = false;
    cached_revision_.clear();
    cached_project_ = Project{};
    cached_generation_ = 0;
    cached_size_ = 0;
    cached_write_time_ = {};
}

bool McpProjectBackend::SelectTimelineForEdit(const std::string& timelineId,
                                              std::string& errorName,
                                              std::string& message) {
    if (require_explicit_timeline_ && timelineId.empty()) {
        errorName = "TimelineRequired";
        message = "strict timeline editing requires an explicit timeline_id";
        return false;
    }
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    selected_timeline_id_ =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected_timeline_id_)) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + selected_timeline_id_ + "'";
        selected_timeline_id_.clear();
        return false;
    }
    return true;
}

// PL3 -- ROADMAP.md. Selecting a timeline here never activated it: the
// project file keeps its active timeline, only this call's scope moves. A
// read therefore needs no more than an edit does, minus strict mode, which
// guards edits that forget to name their timeline.
bool McpProjectBackend::SelectTimelineForRead(const std::string& timelineId,
                                              std::string& errorName,
                                              std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    selected_timeline_id_ =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(selected_timeline_id_) == nullptr) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + selected_timeline_id_ + "'";
        selected_timeline_id_.clear();
        return false;
    }
    return true;
}

bool McpProjectBackend::SnapshotDocument(Document& document,
                                         std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    document = selected_timeline_id_.empty()
                   ? project.MakeActiveDocument()
                   : project.MakeDocument(selected_timeline_id_);
    return true;
}

bool McpProjectBackend::ApplyOperation(Operation operation,
                                       std::string& resultJson,
                                       std::string& errorName,
                                       std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    // PERF5 -- the edit starts from the cache and leaves the committed
    // project in it: under the session lock nothing else moves the package,
    // so parsing it before (the CLI command) and after (the revision stamp)
    // read back what this process already held. The cache is replaced on
    // success and dropped on failure, explicitly.
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    const uint64_t generation = StoredProjectCommitGeneration();
    const std::string operationJson = SerializeOperation(operation);
    std::string output;
    const int result = ApplyOperationToLoadedProject(
        project_path_, project, operationJson, output, selected_timeline_id_,
        {}, EntryMotifsForCall(operation));
    if (result == 0) {
        AdoptCommittedProject(std::move(project), generation);
    } else {
        InvalidateCache();
    }
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// PRD16 -- ROADMAP.md. From the project this backend holds -- its cache, or
// on a background export's fork the project pinned when the export was asked
// for -- rather than through ExportCommand, which read the package again
// mid-call: a task must not export the edits made after it started. The
// settings arrive whole (the catalog resolves them, Exporter::DefaultSettings
// included), as the live backend takes them. Same error names as
// `--export`. The file is encoded by RunPlan: with a task's runner, followed
// and cancellable.
bool McpProjectBackend::ExportTimeline(const ExportSettings& settings,
                                       std::string& resultJson,
                                       std::string& errorName,
                                       std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "InvalidDocument";
        return false;
    }
    const Ulid timelineId = selected_timeline_id_.empty()
                                ? project.active_timeline_id
                                : selected_timeline_id_;
    if (project.FindTimeline(timelineId) == nullptr) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    ExportPlan plan;
    if (!Exporter::BuildPlan(project.MakeDocument(timelineId), project_path_,
                             settings, plan, message)) {
        errorName = "InvalidExport";
        return false;
    }
    if (!Exporter::RunPlan(plan, BackgroundTaskRunner(), {}, message)) {
        errorName = "ExportFailed";
        return false;
    }
    resultJson = Exporter::ResultJson(plan);
    return true;
}

bool McpProjectBackend::VerifyExportedFile(const std::string& exportPath,
                                           std::string& resultJson,
                                           std::string& errorName,
                                           std::string& message) {
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

bool McpProjectBackend::ApplyProjectEdit(ProjectOperation operation,
                                         std::string& resultJson,
                                         std::string& errorName,
                                         std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    // PERF5 -- as ApplyOperation: from the cache, and the committed project
    // back into it.
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    const uint64_t generation = StoredProjectCommitGeneration();
    std::string output;
    // MOTIF6 -- the call's motif rides on the project journal's entry.
    const int result = ApplyProjectOperationToLoadedProject(
        project_path_, project, SerializeProjectOperation(operation), output,
        ProjectEntryMotifsForCall());
    if (result == 0) {
        AdoptCommittedProject(std::move(project), generation);
    } else {
        InvalidateCache();
    }
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::ExportProjectTimelines(
    const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
    std::string& resultJson, std::string& errorName, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    // PRD16 -- on a background task's fork, each file is followed and
    // cancellable (Exporter::RunPlan).
    return Exporter::ExportBatch(project, project_path_, items, preview,
                                 resultJson, errorName, message,
                                 BackgroundTaskRunner());
}

bool McpProjectBackend::ReadTimelineWordSpans(
    std::vector<TimelineTranscriptSpan>& spans, TimelineWordCheck& check,
    std::string& message) {
    Document document;
    if (!SnapshotDocument(document, message)) return false;
    return ReadDocumentWordSpans(document, spans, check, message);
}

bool McpProjectBackend::ReadDocumentWordSpans(
    const Document& document, std::vector<TimelineTranscriptSpan>& spans,
    TimelineWordCheck& check, std::string& message) {
    return BuildTimelineTranscriptWordSpans(
        document,
        std::filesystem::absolute(project_path_).parent_path() / ".cutmachine" /
            "transcripts",
        spans, message, &check);
}

bool McpProjectBackend::AnalyzeTimelineAudio(
    const TimelineAudioSettings& settings, std::string& resultJson,
    std::string& message) {
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

bool McpProjectBackend::AnalyzeSourceHesitations(
    const Ulid& sourceId, const HesitationSettings& settings,
    HesitationReport& report, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    return AnalyzeProjectSourceHesitations(project_path_, project.rushes,
                                           sourceId, settings, report, message);
}

bool McpProjectBackend::ReadSourceHesitations(const Ulid& sourceId,
                                              HesitationReport& report,
                                              std::string& message) {
    return ReadProjectSourceHesitations(project_path_, sourceId, report,
                                        message);
}

bool McpProjectBackend::ReadTimelineTranscript(std::string& json,
                                               std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    return DescribeTimelineTranscriptForAgent(
        selected_timeline_id_.empty()
            ? project.MakeActiveDocument()
            : project.MakeDocument(selected_timeline_id_),
        projectPath.parent_path() / ".cutmachine" / "transcripts", json,
        message);
}

bool McpProjectBackend::ReadSourceTranscript(const Ulid& sourceId,
                                             Transcript& transcript,
                                             std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    return LoadSourceWords(projectPath.parent_path() / ".cutmachine", sourceId,
                           transcript, message);
}

ShotQualityCacheRead McpProjectBackend::ReadSourceShotQuality(
    const Ulid& sourceId, ShotQualityReport& report) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "shotquality" /
                                       (sourceId + ".json");
    return ReadShotQualityCache(path.string(), report);
}

bool McpProjectBackend::ReadSourceAudioBeats(const Ulid& sourceId,
                                             BeatDetectionResult& result,
                                             std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "beats" /
                                       (sourceId + ".beats");
    return LoadAudioBeats(path.string(), result, message);
}

// PA3 -- docs/roadmap/PARITE-2026-11.md.
bool McpProjectBackend::ReadSourceSpeakers(const Ulid& sourceId,
                                           SpeakerDiarization& speakers,
                                           std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "speakers" /
                                       (sourceId + ".json");
    return LoadSpeakerDiarization(path.string(), speakers, message);
}

bool McpProjectBackend::AnalyzeSourceSpeakers(const Ulid& sourceId,
                                              int32_t speakers,
                                              std::string& resultJson,
                                              std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result =
        AnalyzeSpeakersCommand(project_path_, sourceId, speakers, output,
                               media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AnalyzeSourceAudioBeats(const Ulid& sourceId,
                                                std::string& resultJson,
                                                std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result = AnalyzeAudioBeatsCommand(project_path_, sourceId, output,
                                                media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::ReadSourceSpeechOnset(const Ulid& sourceId,
                                              SpeechOnsetReport& report,
                                              std::string& message) {
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    const std::filesystem::path path = projectPath.parent_path() /
                                       ".cutmachine" / "speechonset" /
                                       (sourceId + ".json");
    return LoadSpeechOnset(path.string(), report, message);
}

bool McpProjectBackend::AnalyzeSourceSpeechOnset(
    const Ulid& sourceId, const SpeechOnsetSettings& settings,
    std::string& resultJson, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result =
        AnalyzeSpeechOnsetCommand(project_path_, sourceId, output, settings,
                                  media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// SYNCHRO-2026-10 SY1 -- read only, on the project as stored.
bool McpProjectBackend::MeasureAudioOffset(const Ulid& referenceId,
                                           const Ulid& otherId,
                                           const AudioOffsetChannels& channels,
                                           std::string& resultJson,
                                           std::string& errorName,
                                           std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    std::string output;
    const int result =
        MeasureAudioOffsetCommand(project_path_, referenceId, otherId, channels,
                                  output, media_task_runner_, &project);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// FC6 -- read only, on the project as stored.
bool McpProjectBackend::ProposeDucking(
    const Ulid& mediaId, AudioOffsetChannel channel,
    const audio_dsp::DuckingSettings& settings, std::string& resultJson,
    std::string& errorName, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    std::string output;
    const int result =
        ProposeDuckingCommand(project_path_, mediaId, channel, settings, output,
                              media_task_runner_, &project);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AlignSourceTranscripts(bool apply,
                                               std::string& resultJson,
                                               std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result =
        AlignTranscriptsCommand(project_path_, apply, output, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AlignSourceWords(const std::vector<Ulid>& mediaIds,
                                         const std::string& textsJson,
                                         std::string& resultJson,
                                         std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result = AlignWordsCommand(project_path_, mediaIds, textsJson,
                                         output, media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::AnalyzeSourceShotQuality(const Ulid& sourceId,
                                                 std::string& resultJson,
                                                 std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    // Same command `--shot-quality` runs, for the same reason ApplyOperation
    // reuses ApplyOperationCommand: one analysis path, not two.
    std::string output;
    const int result = AnalyzeShotQualityCommand(
        project_path_, sourceId, output, media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::ScanIsolatedFrames(bool analyze,
                                           const std::vector<Ulid>& mediaIds,
                                           std::string& resultJson,
                                           std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    // Same command `--isolated-frames` runs, for the reason ApplyOperation
    // reuses ApplyOperationCommand: one pass, not two that drift.
    std::string output;
    const int result =
        IsolatedFramesCommand(project_path_, selected_timeline_id_, analyze,
                              output, media_task_runner_, mediaIds, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::TranscribeSources(const std::vector<Ulid>& sourceIds,
                                          const std::string& language,
                                          bool verbatim, bool includeSilent,
                                          bool overwrite,
                                          std::string& resultJson,
                                          std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    // Same command `--transcribe` runs. The empty model path is deliberate:
    // it resolves the locally configured model, which is the only form a
    // caller that is not a human typing a path can use.
    std::string output;
    const int result = TranscribeMediaCommand(
        project_path_, sourceIds, "", language, verbatim, includeSilent, output,
        media_task_runner_, &project, overwrite);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::TranscribeTimeline(const std::string& timelineId,
                                           const std::string& language,
                                           bool verbatim,
                                           std::string& resultJson,
                                           std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    std::string output;
    const int result =
        TranscribeTimelineCommand(project_path_, timelineId, language, verbatim,
                                  output, media_task_runner_, &project);
    std::string errorName;
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::RunAfterRequest(const std::string& requestJson,
                                        std::string& resultJson,
                                        std::string& errorName,
                                        std::string& message) {
    return RunAfterBridgeRequest(requestJson, resultJson, errorName, message);
}

bool McpProjectBackend::CaptureSourceFrame(const Ulid& sourceId,
                                           const Ulid& clipId,
                                           const RationalTime& time,
                                           std::string& jpegBytes,
                                           std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == sourceId; });
    if (media == project.rushes.end()) {
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
        path = std::filesystem::absolute(project_path_).parent_path() / path;
    }
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    FrameCaptureSettings settings;
    // G3 -- the media read in its own declared input.
    settings.color_management = document.ColorManagementFor(sourceId);
    if (!ApplyClipFramingToCapture(document, clipId, time, settings, message))
        return false;
    return ::CaptureSourceFrame(path.lexically_normal().string(), time,
                                settings, jpegBytes, message);
}

bool McpProjectBackend::CaptureTimelineSheet(
    const TimelineSheetPlan& plan, const TimelineSheetSettings& settings,
    std::string& jpegBytes, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    const std::filesystem::path projectPath =
        std::filesystem::absolute(project_path_);
    return RenderTimelineSheet(document, projectPath.parent_path(), plan,
                               settings, jpegBytes, message);
}

// IA4 -- the committed project, as the sheets read it; a reading of the
// mix runs where this backend's media work runs.
bool McpProjectBackend::InspectCut(const CutInspectionPlan& plan,
                                   const CutInspectionRenderSettings& settings,
                                   CutInspectionResult& result,
                                   std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    CutInspectionRenderSettings withRunner = settings;
    withRunner.runner = media_task_runner_;
    return RenderCutInspection(document,
                               std::filesystem::absolute(project_path_), plan,
                               withRunner, result, message);
}

// G1 -- the committed project, as read_frame reads it.
bool McpProjectBackend::MeasureClipColors(
    const ColorMeasurePlan& plan, std::vector<ColorClipMeasurement>& results,
    std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return RenderColorMeasurement(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), plan,
        ColorMeasureSettings{}, results, message);
}

// G7, G3 -- the committed project's media, as read_frame resolves it.
bool McpProjectBackend::ReadMediaFileColor(const Ulid& mediaId,
                                           MediaFileColor& color,
                                           std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const auto media = std::find_if(
        project.rushes.begin(), project.rushes.end(),
        [&](const LibraryMedia& item) { return item.id == mediaId; });
    if (media == project.rushes.end()) {
        message = "unknown media_id '" + mediaId + "'";
        return false;
    }
    std::filesystem::path path(media->path);
    if (path.is_relative())
        path = std::filesystem::absolute(project_path_).parent_path() / path;
    return ReadFileColorFacts(path.lexically_normal().string(), color, message);
}

// G2 -- the committed project, as measure_color reads it.
bool McpProjectBackend::MatchClipColor(const ColorMatchPlan& plan,
                                       ColorMatchResult& result,
                                       std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return RenderColorMatch(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), plan,
        ColorMeasureSettings{}, result, message);
}

// AGENT-2026-09 A20 -- the committed project, read as match_color reads it.
bool McpProjectBackend::ProposeClipGrades(const GradeProposalPlan& plan,
                                          std::vector<GradeProposal>& proposals,
                                          std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return RenderGradeProposals(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), plan,
        ColorMeasureSettings{}, proposals, message);
}

// CA3 -- docs/roadmap/CADRAGE-2026-10.md. The committed project, read as
// propose_grade reads it. In the build without a window no finder is
// installed, and SampleClipFaces refuses by name.
bool McpProjectBackend::SampleClipFaces(const Ulid& clipId, int64_t everyFrames,
                                        std::vector<SubjectSample>& samples,
                                        std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return ::SampleClipFaces(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), clipId,
        everyFrames, 512, "", samples, message);
}

// PA7 -- docs/roadmap/PARITE-2026-11.md. The committed project, read as
// SampleClipFaces reads it.
bool McpProjectBackend::SampleClipMotion(
    const Ulid& clipId, std::vector<StabilizationSample>& motion,
    std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return ::SampleClipMotion(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), clipId,
        "", motion, message);
}

// G1b -- the committed project, as read_frame reads it.
bool McpProjectBackend::CaptureGradedClipFrame(const Ulid& clipId,
                                               const RationalTime& time,
                                               std::string& jpegBytes,
                                               std::string& message) {
    Project project;
    if (!LoadProject(project, message)) return false;
    const Document document = selected_timeline_id_.empty()
                                  ? project.MakeActiveDocument()
                                  : project.MakeDocument(selected_timeline_id_);
    return ::CaptureGradedClipFrame(
        document,
        std::filesystem::absolute(project_path_).parent_path().string(), clipId,
        time, FrameCaptureSettings{}, jpegBytes, message);
}

bool McpProjectBackend::Undo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    // PERF5 -- as ApplyOperation.
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    const uint64_t generation = StoredProjectCommitGeneration();
    std::string output;
    const int result = UndoOperationOnLoadedProject(
        project_path_, project, output, selected_timeline_id_);
    if (result == 0) {
        AdoptCommittedProject(std::move(project), generation);
    } else {
        InvalidateCache();
    }
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::Redo(std::string& resultJson, std::string& errorName,
                             std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    // PERF5 -- as ApplyOperation.
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    const uint64_t generation = StoredProjectCommitGeneration();
    std::string output;
    const int result = RedoOperationOnLoadedProject(
        project_path_, project, output, selected_timeline_id_);
    if (result == 0) {
        AdoptCommittedProject(std::move(project), generation);
    } else {
        InvalidateCache();
    }
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// PL1 -- ROADMAP.md. Begin only reads; End rewrites the timeline's journal
// and nothing else, under the lock this backend holds.
bool McpProjectBackend::BeginIntervention(std::string& resultJson,
                                          std::string& errorName,
                                          std::string& message) {
    std::string output;
    const int result =
        BeginInterventionCommand(project_path_, output, selected_timeline_id_);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

bool McpProjectBackend::EndIntervention(const std::string& intervention,
                                        const std::string& label,
                                        std::string& resultJson,
                                        std::string& errorName,
                                        std::string& message) {
    if (!MayEdit(errorName, message)) {
        return false;
    }
    std::string output;
    const int result = EndInterventionCommand(
        project_path_, intervention, label, output, CurrentEditMotifs().motif);
    return ParseCommandResult(output, result == 0, resultJson, errorName,
                              message);
}

// PRD35 -- ROADMAP.md. The journal as it is on disk, the selected timeline's
// (SelectTimelineForRead) or the active one's; a timeline never edited has
// none, and an empty history.
namespace {

// The journal of the timeline a read selected, or of the active one; a
// timeline never edited has none, and an empty one.
bool LoadSelectedTimelineLog(const std::string& projectPath,
                             const Project& project, const Ulid& selected,
                             Ulid& timelineId, EditLog& log,
                             std::string& errorName, std::string& message) {
    timelineId = selected.empty() ? project.active_timeline_id : selected;
    if (project.FindTimeline(timelineId) == nullptr) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    const std::string path =
        TimelineEditLogPathForProject(projectPath, timelineId);
    std::error_code existsError;
    if (std::filesystem::exists(path, existsError)) {
        EditError error = EditError::None;
        if (!EditLog::Load(path, log, error, message)) {
            errorName = EditErrorName(error);
            return false;
        }
    } else if (existsError) {
        errorName = "IoError";
        message = "unable to inspect edit log '" + path +
                  "': " + existsError.message();
        return false;
    }
    return true;
}

}  // namespace

// MOTIF4 -- docs/roadmap/MOTIF-2026-10.md.
bool McpProjectBackend::ReadTimelineMotifs(
    const std::optional<MotifReason>& reason, const std::string& rule,
    std::string& json, std::string& errorName, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    Ulid timelineId;
    EditLog log;
    if (!LoadSelectedTimelineLog(project_path_, project, selected_timeline_id_,
                                 timelineId, log, errorName, message)) {
        return false;
    }
    json = ListMotifsJson(timelineId, log, reason, rule);
    return true;
}

// PA9 -- docs/roadmap/PARITE-2026-11.md. The selected timeline as the
// package holds it, against its journal.
bool McpProjectBackend::ReadTimelineChangesJson(const std::string& sinceHash,
                                                std::string& json,
                                                std::string& errorName,
                                                std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    Ulid timelineId;
    EditLog log;
    if (!LoadSelectedTimelineLog(project_path_, project, selected_timeline_id_,
                                 timelineId, log, errorName, message)) {
        return false;
    }
    TimelineChanges changes;
    if (!ReadTimelineChanges(project.MakeDocument(timelineId), log, sinceHash,
                             changes, message)) {
        errorName = "InvalidDocument";
        return false;
    }
    json = TimelineChangesJson(changes);
    return true;
}

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The project's journal, as the
// package stores it, named with the project this backend holds.
bool McpProjectBackend::ReadProjectMotifs(
    const std::optional<MotifReason>& reason, const std::string& rule,
    std::string& json, std::string& errorName, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
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

bool McpProjectBackend::ReadTimelineHistory(TimelineHistory& history,
                                            std::string& errorName,
                                            std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    Ulid timelineId;
    EditLog log;
    if (!LoadSelectedTimelineLog(project_path_, project, selected_timeline_id_,
                                 timelineId, log, errorName, message)) {
        return false;
    }
    history = SummarizeHistory(timelineId, log);
    return true;
}

std::string McpProjectBackend::MotionMediaDirectory() const {
    if (project_path_.empty()) {
        return {};
    }
    return std::filesystem::absolute(project_path_).parent_path().string();
}

// PERF5 -- a read, answered from the cache like every other: it used to run
// the CLI command, which parsed the package, and then drop the cache for
// the revision stamp to parse it again. Same text as --describe
// (DescribeProject), same refusals; the command line alone puts "ok" in
// front of it (PRD39).
bool McpProjectBackend::Describe(std::string& json, std::string& message) {
    Project project;
    if (!LoadProject(project, message)) {
        message =
            std::string(EditErrorName(EditError::ParseError)) + ": " + message;
        return false;
    }
    // PL3 -- a describe scoped to a timeline (SelectTimelineForRead) shows
    // that one; unscoped, the active one, as before.
    if (!selected_timeline_id_.empty() &&
        project.FindTimeline(selected_timeline_id_) == nullptr) {
        message = std::string(EditErrorName(EditError::UnknownSequence)) +
                  ": unknown timeline_id '" + selected_timeline_id_ + "'";
        return false;
    }
    try {
        json = DescribeProject(project, selected_timeline_id_);
        return true;
    } catch (const std::exception& exception) {
        message = std::string(EditErrorName(EditError::ArithmeticError)) +
                  ": " + exception.what();
        return false;
    }
}

bool McpProjectBackend::MayEdit(std::string& errorName,
                                std::string& message) const {
    if (!analysis_only_) {
        return true;
    }
    errorName = "UnsupportedOperation";
    message =
        "a background task installs analysis caches and never edits "
        "the project";
    return false;
}

bool McpProjectBackend::Revision(std::string& revision, std::string& message) {
    // PRD16 -- a pinned project's own revision, never one the cache kept.
    if (pinned_project_ != nullptr) {
        revision = CanonicalHash(pinned_project_->SaveToString());
        return true;
    }
    // Through LoadProject, so the size/mtime second opinion still applies;
    // what the cache saves is serializing and hashing the project again.
    Project project;
    if (!LoadProject(project, message)) {
        return false;
    }
    if (!cached_revision_.empty()) {
        revision = cached_revision_;
        return true;
    }
    revision = CanonicalHash(project.SaveToString());
    if (cache_valid_) {
        cached_revision_ = revision;
    }
    return true;
}

std::unique_ptr<McpBackend> McpProjectBackend::ForkForTask(
    MediaTaskRunner runner, std::string& message) {
    if (project_path_.empty()) {
        message = "no project is open";
        return nullptr;
    }
    auto fork = std::make_unique<McpProjectBackend>(project_path_,
                                                    require_explicit_timeline_);
    fork->media_task_runner_ = std::move(runner);
    fork->analysis_only_ = true;
    // PERF5 -- the task starts from this session's cache, copied here on the
    // calling thread; the commit generation drops the copy if an edit lands
    // before the task reads it.
    if (cache_valid_) {
        fork->cached_project_ = cached_project_;
        fork->cached_generation_ = cached_generation_;
        fork->cached_size_ = cached_size_;
        fork->cached_write_time_ = cached_write_time_;
        fork->cache_valid_ = true;
    }
    return fork;
}

bool McpProjectBackend::PinTaskProject(const Project* project,
                                       std::string& message) {
    if (!analysis_only_) {
        message = "only a background task's backend holds a pinned project";
        return false;
    }
    pinned_project_ = project != nullptr
                          ? std::make_shared<const Project>(*project)
                          : nullptr;
    return true;
}
