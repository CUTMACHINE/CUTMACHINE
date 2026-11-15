#pragma once

// Production McpBackend: a thin adapter over Cli.cc's headless commands, the
// same entry points `--apply-op`/`--describe` use. Depends on ProjectStorage
// (CommonCrypto), so -- like the rest of the project-facing CLI surface --
// this only builds where that is available (macOS). Keep this file free of
// HTTP/JSON-RPC concerns; McpServer.cc/McpTools.cc own that layer and only
// see it through McpBackend.

#include "McpBackend.h"
#include "Project.h"
#include "ProjectStorage.h"

#include <filesystem>
#include <string>

// The project file a path names, the way open_project reads it: absolute
// (never relative to wherever a server was started), a package directory
// meaning the project.cutmachine.json inside it, lexically normal. AX1 --
// docs/roadmap/AX-2026-09.md: the relay to the app and the app's own
// open_project resolve it the same way, so they find the same lock.
bool ResolveMcpProjectPath(const std::string& requestedPath,
                           std::string& projectPath, std::string& message);

// PRD16 -- ROADMAP.md. The folder that holds the package of the project file
// `projectPath` names (McpBackend::ProjectFolder): the package is the folder
// of project.cutmachine.json, whatever its name, since a project file
// outside one is refused when it is loaded. Absolute; empty for an empty
// path. The live backend answers with it too.
std::string McpProjectFolder(const std::string& projectPath);

class McpProjectBackend : public McpBackend {
public:
    bool CommitRunStep(const RunStepRequest& request, std::string& resultJson,
                       std::string& errorName, std::string& message) override;
    bool ReadRunStep(const Ulid& stepId, std::string& resultJson,
                     std::string& errorName, std::string& message) override;
    // Attaches to `projectPath` without taking its session lock, for a
    // caller that holds it. An empty path starts with no project open:
    // OpenProject and CreateProject then lock the one they attach to.
    explicit McpProjectBackend(std::string projectPath = {},
                               bool requireExplicitTimeline = false);
    bool ProjectArchiveStatus(bool retry, std::string& resultJson,
                              std::string& errorName,
                              std::string& message) override;
    bool ImportProjectArchive(const std::string& database,
                              const std::vector<std::string>& projectPaths,
                              std::string& resultJson, std::string& errorName,
                              std::string& message) override;

    // IA7 -- ROADMAP.md. The lock moves with the project: the one held is
    // released before the next is taken, so this backend never holds two,
    // and a refused open reattaches the project it had.
    bool HasOpenProject() const override { return !project_path_.empty(); }
    bool CreateProject(const std::string& packagePath, const std::string& name,
                       const std::string& format, std::string& resultJson,
                       std::string& errorName, std::string& message) override;
    bool OpenProject(const std::string& requestedPath, std::string& resultJson,
                     std::string& errorName, std::string& message) override;
    bool IngestMedia(const std::string& mediaPath, bool recursive,
                     const std::optional<Ulid>& targetBin,
                     std::string& resultJson, std::string& errorName,
                     std::string& message) override;
    bool ImportResolveTimeline(const std::string& manifestPath,
                               const std::string& resolveDatabase,
                               std::string& resultJson, std::string& errorName,
                               std::string& message) override;
    bool ExportResolveXml(const std::string& xmlPath,
                          const std::string& resolveScaling,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) override;
    // R6 -- docs/roadmap/REEL-2026-09.md.
    bool ImportSequenceLut(const std::string& cubePath,
                           const std::string& space, std::string& resultJson,
                           std::string& errorName,
                           std::string& message) override;

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override;
    bool SelectTimelineForRead(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override;
    void EndTimelineEdit() override { selected_timeline_id_.clear(); }
    // PRD17 -- ROADMAP.md.
    bool RequiresExplicitTimeline() const override {
        return require_explicit_timeline_;
    }
    void SetRequireExplicitTimeline(bool required) override {
        require_explicit_timeline_ = required;
    }
    std::string ProjectPath() const override { return project_path_; }
    bool SnapshotDocument(Document& document, std::string& message) override;
    bool SnapshotProject(Project& project, std::string& message) override {
        return LoadProject(project, message);
    }
    bool ExportProjectTimelines(
        const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
        std::string& resultJson, std::string& errorName,
        std::string& message) override;
    bool ExportTimeline(const ExportSettings& settings, std::string& resultJson,
                        std::string& errorName, std::string& message) override;
    bool VerifyExportedFile(const std::string& exportPath,
                            std::string& resultJson, std::string& errorName,
                            std::string& message) override;
    bool ReadTimelineWordSpans(std::vector<TimelineTranscriptSpan>& spans,
                               TimelineWordCheck& check,
                               std::string& message) override;
    bool ReadDocumentWordSpans(const Document& document,
                               std::vector<TimelineTranscriptSpan>& spans,
                               TimelineWordCheck& check,
                               std::string& message) override;
    bool AnalyzeTimelineAudio(const TimelineAudioSettings& settings,
                              std::string& resultJson,
                              std::string& message) override;
    bool AnalyzeSourceHesitations(const Ulid& sourceId,
                                  const HesitationSettings& settings,
                                  HesitationReport& report,
                                  std::string& message) override;
    bool ReadSourceHesitations(const Ulid& sourceId, HesitationReport& report,
                               std::string& message) override;
    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override;
    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override;
    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override;
    ShotQualityCacheRead ReadSourceShotQuality(
        const Ulid& sourceId, ShotQualityReport& report) override;
    bool ReadSourceAudioBeats(const Ulid& sourceId, BeatDetectionResult& result,
                              std::string& message) override;
    bool ReadSourceSpeakers(const Ulid& sourceId, SpeakerDiarization& speakers,
                            std::string& message) override;
    bool AnalyzeSourceSpeakers(const Ulid& sourceId, int32_t speakers,
                               std::string& resultJson,
                               std::string& message) override;
    bool AnalyzeSourceAudioBeats(const Ulid& sourceId, std::string& resultJson,
                                 std::string& message) override;
    bool AnalyzeSourceShotQuality(const Ulid& sourceId, std::string& resultJson,
                                  std::string& message) override;
    bool ScanIsolatedFrames(bool analyze, const std::vector<Ulid>& mediaIds,
                            std::string& resultJson,
                            std::string& message) override;
    bool ReadSourceSpeechOnset(const Ulid& sourceId, SpeechOnsetReport& report,
                               std::string& message) override;
    bool AnalyzeSourceSpeechOnset(const Ulid& sourceId,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string& message) override;
    bool MeasureAudioOffset(const Ulid& referenceId, const Ulid& otherId,
                            const AudioOffsetChannels& channels,
                            std::string& resultJson, std::string& errorName,
                            std::string& message) override;
    bool ProposeDucking(const Ulid& mediaId, AudioOffsetChannel channel,
                        const audio_dsp::DuckingSettings& settings,
                        std::string& resultJson, std::string& errorName,
                        std::string& message) override;
    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string& message) override;
    bool AlignSourceWords(const std::vector<Ulid>& mediaIds,
                          const std::string& textsJson, std::string& resultJson,
                          std::string& message) override;
    bool TranscribeSources(
        const std::vector<Ulid>& sourceIds, const std::string& language,
        bool verbatim, bool includeSilent, bool overwrite,
        std::string& resultJson, std::string& message,
        const WhisperDecodingRequest& profile = std::nullopt) override;
    bool TranscribeTimeline(
        const std::string& timelineId, const std::string& language,
        bool verbatim, std::string& resultJson, std::string& message,
        const WhisperDecodingRequest& profile = std::nullopt) override;
    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
                         std::string& message) override;
    bool CaptureSourceFrame(const Ulid& sourceId, const Ulid& clipId,
                            const RationalTime& time, std::string& jpegBytes,
                            std::string& message) override;
    bool InspectCut(const CutInspectionPlan& plan,
                    const CutInspectionRenderSettings& settings,
                    CutInspectionResult& result, std::string& message) override;
    bool MeasureClipColors(const ColorMeasurePlan& plan,
                           std::vector<ColorClipMeasurement>& results,
                           std::string& message) override;
    bool CaptureGradedClipFrame(const Ulid& clipId, const RationalTime& time,
                                std::string& jpegBytes,
                                std::string& message) override;
    bool MatchClipColor(const ColorMatchPlan& plan, ColorMatchResult& result,
                        std::string& message) override;
    bool ProposeClipGrades(const GradeProposalPlan& plan,
                           std::vector<GradeProposal>& proposals,
                           std::string& message) override;
    bool SampleClipFaces(const Ulid& clipId, int64_t everyFrames,
                         std::vector<SubjectSample>& samples,
                         std::string& message) override;
    bool SampleClipMotion(const Ulid& clipId,
                          std::vector<StabilizationSample>& motion,
                          std::string& message) override;
    bool ReadMediaFileColor(const Ulid& mediaId, MediaFileColor& color,
                            std::string& message) override;
    bool CaptureTimelineSheet(const TimelineSheetPlan& plan,
                              const TimelineSheetSettings& settings,
                              std::string& jpegBytes,
                              std::string& message) override;
    bool ReadTimelineTranscript(std::string& json,
                                std::string& message) override;
    bool Undo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override;
    bool BeginIntervention(std::string& resultJson, std::string& errorName,
                           std::string& message) override;
    bool EndIntervention(const std::string& intervention,
                         const std::string& label, std::string& resultJson,
                         std::string& errorName, std::string& message) override;
    bool ReadTimelineHistory(TimelineHistory& history, std::string& errorName,
                             std::string& message) override;
    bool ReadTimelineMotifs(const std::optional<MotifReason>& reason,
                            const std::string& rule, std::string& json,
                            std::string& errorName,
                            std::string& message) override;
    bool ReadTimelineChangesJson(const std::string& sinceHash,
                                 std::string& json, std::string& errorName,
                                 std::string& message) override;
    bool ReadProjectMotifs(const std::optional<MotifReason>& reason,
                           const std::string& rule, std::string& json,
                           std::string& errorName,
                           std::string& message) override;
    bool Describe(std::string& json, std::string& message) override;
    std::string MotionMediaDirectory() const override;
    // GENERATIF-2026-10 GN3 -- the same folder: the package's.
    std::string PackageDirectory() const override {
        return MotionMediaDirectory();
    }

    // IA5 -- ROADMAP.md. Revision from the session cache: hashed once per
    // load, not once per call.
    bool Revision(std::string& revision, std::string& message) override;
    // A backend attached to the same project without its lock (this one holds
    // it), whose media work runs through `runner` and which refuses every
    // edit: a background task only installs analysis caches, so the project
    // this backend caches never moves under it.
    std::unique_ptr<McpBackend> ForkForTask(MediaTaskRunner runner,
                                            std::string& message) override;
    // PRD16 -- ROADMAP.md. On a fork, every read answers from `project`
    // while it is pinned (LoadProject); nullptr goes back to the package.
    bool PinTaskProject(const Project* project, std::string& message) override;
    MediaTaskRunner BackgroundTaskRunner() const override {
        return analysis_only_ ? media_task_runner_ : MediaTaskRunner{};
    }
    std::string ProjectFolder() const override {
        return McpProjectFolder(project_path_);
    }

private:
    // Refuses an edit on a task's fork. Returns true when the call may go on.
    bool MayEdit(std::string& errorName, std::string& message) const;

    MediaTaskRunner media_task_runner_;
    bool analysis_only_ = false;
    // PRD16 -- the project a background export was asked on (PinTaskProject).
    std::shared_ptr<const Project> pinned_project_;
    std::string cached_revision_;
    // PERF-2026-09. One MCP tool call used to reload the whole package from
    // disk three or four times over: once to resolve the timeline, once for
    // the snapshot the dispatcher resolves aliases against, usually once
    // more inside the dispatcher itself, and once more inside the CLI
    // command that applies the edit. Each of those reads the project file
    // plus every Timelines/*.json beside it, parses them and revalidates the
    // result. The project's session lock is held for as long as it is open
    // -- by this backend when it opened the project itself (IA7), by the
    // caller otherwise -- so nothing outside this process can move those
    // bytes: the reloads were repetition, not re-reads of something that
    // had changed.
    //
    // What keeps the cache the package is the commit generation storage
    // counts (StoredProjectCommitGeneration): read before each parse, it
    // moves whenever anything in this process commits a package, so a
    // command that writes -- an ingest, an intervention, a command that
    // becomes a writer later -- drops the cache without this adapter having
    // to know which commands write; that stays Cli.cc's business. It
    // replaces a scope drawn at "runs a command", which dropped the cache
    // after every analysis and undo too, and cost a parse on the next call.
    bool LoadProject(Project& project, std::string& message);
    void InvalidateCache();
    // PERF5 -- an edit applied here, on a copy of the cache, commits a
    // project this backend then holds: it becomes the cache, stamped with
    // the file just written, instead of being dropped and parsed back. Only
    // when the edit's commit is the one commit since `generationBefore`.
    void AdoptCommittedProject(Project project, uint64_t generationBefore);

    std::string project_path_;
    // Held only for a project this backend opened itself. PRD17 -- behind a
    // pointer, so that OpenProject can take the next project's lock before
    // it lets this one go.
    std::unique_ptr<ProjectSessionLock> session_lock_ =
        std::make_unique<ProjectSessionLock>();
    std::string selected_timeline_id_;
    bool require_explicit_timeline_ = false;
    Project cached_project_;
    bool cache_valid_ = false;
    uint64_t cached_generation_ = 0;
    std::uintmax_t cached_size_ = 0;
    std::filesystem::file_time_type cached_write_time_{};
};
