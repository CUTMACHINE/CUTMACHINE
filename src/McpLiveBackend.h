#pragma once

// Third McpBackend implementation (ROADMAP.md F2.4), alongside
// McpProjectBackend (McpProjectBackend.h, production MCP server -- a
// project *file path*, reloaded per call) and the in-memory test backend
// (tests/mcp_tools_tests.cc, a Document the test itself owns). This one
// wraps the app's already-open, in-memory Document and EditLog *by
// reference* -- the exact objects main.mm's mouse-driven edit handlers
// call `self.state->editLog.Apply(self.state->document, ...)` against.
//
// That reference-not-copy choice is the whole point: the chat panel and a
// mouse gesture must be editing the *same* document instance, sharing one
// undo/redo stack, never a stale reload of what was last saved to disk.
// Routing the chat panel through McpProjectBackend instead (reload from the
// project file, apply, save back) would silently fork the in-memory
// document from whatever the chat tool call just wrote to disk -- exactly
// the kind of hidden second mutation path PHILOSOPHY.md principle 2 and
// this ticket's hard constraints rule out.
//
// PHILOSOPHY.md principle 3 stays satisfied the same way McpProjectBackend
// already satisfies it: ApplyOperation below calls EditLog::Apply, the
// identical function every other edit path in this project calls, and nothing
// here constructs an Operation from scratch -- McpTools.cc's dispatch
// functions still own that.
//
// AppKit-only in practice (only ChatPanelView.mm constructs one), but the
// class itself is plain C++ -- Document.h/EditLog.h/Operations.h only, no
// AppKit -- so nothing stops a future test from linking it directly.
// Describe() does pull in Cli.h's DescribeDocument, whose translation unit
// (Cli.cc) also defines ProjectStorage-dependent commands this class never
// calls; that only matters at *link* time and only for whoever links this
// class in, which today is only the macOS app target.

#include "Document.h"
#include "EditLog.h"
#include "McpBackend.h"
#include "MediaTaskManager.h"
#include "ResolveImport.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

class McpLiveBackend : public McpBackend {
public:
    using RunStepCommitCallback = std::function<bool(
        const RunStepRequest&, std::string&, std::string&, std::string&)>;
    void SetRunStepCommitCallback(RunStepCommitCallback callback) {
        commit_run_step_ = std::move(callback);
    }
    void SetRunStepMatchCallback(
        std::function<bool(const RunStepReceipt&)> callback) {
        match_run_step_ = std::move(callback);
    }
    // OR2 -- validate a worker's stored answer against the window without
    // doing file I/O on its thread. Unsaved edits also stop reconciliation.
    bool ValidateRunStepState(std::string& resultJson, std::string& message);
    bool CommitRunStep(const RunStepRequest& request, std::string& resultJson,
                       std::string& errorName, std::string& message) override;
    bool ReadRunStep(const Ulid& stepId, std::string& resultJson,
                     std::string& errorName, std::string& message) override;
    bool ProjectArchiveStatus(bool retry, std::string& resultJson,
                              std::string& errorName,
                              std::string& message) override;
    bool ImportProjectArchive(const std::string& database,
                              const std::vector<std::string>& projectPaths,
                              std::string& resultJson, std::string& errorName,
                              std::string& message) override;
    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The window journals the
    // project edit with the call's motifs (ProjectEditLog::Apply).
    using ProjectApplyCallback =
        std::function<bool(ProjectOperation, std::vector<EntryMotif>,
                           std::string&, std::string&, std::string&)>;
    using ProjectDescribeCallback =
        std::function<bool(std::string&, std::string&)>;
    using TranscriptCallback = std::function<bool(std::string&, std::string&)>;
    // Word-level editing needs one clip's own source transcript, which the
    // window resolves against the open project's cache directory -- the same
    // place the transcribe action writes it.
    using SourceTranscriptCallback =
        std::function<bool(const Ulid&, Transcript&, std::string&)>;
    // Picture-quality reports live beside the transcripts, in the open
    // project's cache directory, and are resolved by the window for the same
    // reason: this backend holds a Document, not a project path.
    //
    // CHAT-ANALYSES -- F2.4. Producing an analysis, and rendering a frame or
    // a timeline sheet, used to be callbacks too, which main.mm ran on the
    // main thread. This backend has since been given the project path
    // (SetProjectPath), and does them itself through the split described
    // below FreezeCacheTarget(). The readers stay callbacks: a cache read is
    // cheap enough for the main thread.
    using SourceShotQualityCallback =
        std::function<ShotQualityCacheRead(const Ulid&, ShotQualityReport&)>;
    using SourceSpeechOnsetCallback =
        std::function<bool(const Ulid&, SpeechOnsetReport&, std::string&)>;
    using TimelineSelectCallback =
        std::function<bool(const Ulid&, std::string&)>;
    // Neither reference is owned; both must outlive this backend. `onApplied`
    // (optional) is invoked after every successful ApplyOperation/Undo/Redo,
    // once the edit is committed (SetEditCommitCallback) and the two
    // references hold it, before the call returns -- main.mm wires it to the
    // -refreshTimelineAfterEditFromPosition/-rebuildMediaList sequence every
    // other in-app edit handler runs after an edit is saved, so a chat-driven
    // edit is reflected on screen exactly like a mouse-driven one. Runs on
    // whatever thread calls ApplyOperation/Undo/Redo -- see ChatPanelView.mm
    // for why that is always the main thread.
    McpLiveBackend(Document& document, EditLog& editLog,
                   std::function<void()> onApplied = nullptr,
                   ProjectApplyCallback applyProject = nullptr,
                   ProjectDescribeCallback describeProject = nullptr,
                   TranscriptCallback readTranscript = nullptr,
                   SourceTranscriptCallback readSourceTranscript = nullptr,
                   SourceShotQualityCallback readSourceShotQuality = nullptr,
                   SourceSpeechOnsetCallback readSourceSpeechOnset = nullptr,
                   TimelineSelectCallback selectTimeline = nullptr,
                   bool requireExplicitTimeline = false);

    // SAVING_ROADMAP.md, a write refused under the editor (S3) -- 24 October
    // 2026. How the window makes an edit of this backend durable: the
    // document and journal the edit produced, staged on copies, are committed
    // to its package before the two references above take them. False, with
    // the storage's reason, when the write was refused: the references are
    // left exactly as they were and the call is refused by name (IoError, as
    // `--apply-op` and the MCP server name the same failure). The edit used
    // to be applied in place, the callback undid it when the commit failed,
    // and the caller -- an agent relayed to the window, the Agent panel --
    // was answered `ok` with the delta of an edit the window no longer held.
    // Without a callback (tests, a backend over a bare document), an edit is
    // taken as soon as it applies.
    using EditCommitCallback = std::function<bool(
        const Document&, const EditLog&, std::string& message)>;
    void SetEditCommitCallback(EditCommitCallback callback) {
        commit_edit_ = std::move(callback);
    }

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override;
    // PRD17 -- ROADMAP.md. The CMRequireExplicitTimeline preference, which
    // the window reads when it makes this backend.
    bool RequiresExplicitTimeline() const override {
        return require_explicit_timeline_;
    }
    void SetRequireExplicitTimeline(bool required) override {
        require_explicit_timeline_ = required;
    }
    void SetProjectPath(std::string path) { project_path_ = std::move(path); }
    std::string ProjectPath() const override { return project_path_; }
    // AX1 -- docs/roadmap/AX-2026-09.md. An agent relayed to the app
    // (McpEndpoint.h) opens the project it means to edit, which can only be
    // the one this window holds: answered as the MCP server answers it,
    // `app: true` added. Any other path is refused, the window keeping its
    // project for its whole life.
    bool OpenProject(const std::string& requestedPath, std::string& resultJson,
                     std::string& errorName, std::string& message) override;
    // F2.4 -- ROADMAP.md. Nor does an agent of the window create one: the
    // window holds its project, its lock and its journals until it closes
    // (IA7). Refused for that reason, and with the person's way to another
    // project, rather than with McpBackend's "this backend cannot create a
    // project", which left the panel's agent nothing to say.
    bool CreateProject(const std::string& packagePath, const std::string& name,
                       const std::string& format, std::string& resultJson,
                       std::string& errorName, std::string& message) override;

    // F2.4 -- ROADMAP.md. ingest_media into the window's project. It was
    // refused from the panel because the window imported through a probe
    // and a commit of its own, which the tool did not reach; both are the
    // engine's since PRD7 (Ingest.h). This runs IngestCommand's steps on the
    // window's document and answers what IngestCommand answers.
    //
    // Split for the adapter (MainThreadBackend.h), like CacheTarget below:
    // the target is read on the thread that owns the document; the files
    // are read on any other -- the sound of each is decoded, on the runner
    // when there is one, so that the task bar shows the import and cancels
    // it; the commit runs back on the owner, through the window's own import
    // (SetIngestCommitCallback), so that a rush reaches the media browser,
    // with its thumbnail and waveform, as one imported from the File menu.
    struct IngestTarget {
        std::string project_path;
        // CompleteLibraryPaths of the window's document: not read again.
        std::set<std::string> complete;
        MediaTaskRunner runner;
    };
    struct ProbedIngest {
        std::vector<ProbedIngestFile> files;
        size_t ignored = 0;
        // DEC32 (PRD7) -- the bin ingest_media names, for the commit.
        std::optional<Ulid> target_bin;
    };
    IngestTarget FreezeIngestTarget() const;
    static bool RunProbeIngest(const IngestTarget& target,
                               const std::string& mediaPath, bool recursive,
                               ProbedIngest& probed, std::string& errorName,
                               std::string& message);
    bool CommitIngest(const ProbedIngest& probed, std::string& resultJson,
                      std::string& errorName, std::string& message);
    // The three in a row, on the thread that owns the document.
    bool IngestMedia(const std::string& mediaPath, bool recursive,
                     const std::optional<Ulid>& targetBin,
                     std::string& resultJson, std::string& errorName,
                     std::string& message) override;
    // What the window does with the files read: the ingest's plan
    // (PlanProbedMedia) on its document, into the bin ingest_media named
    // (none: as for IngestCommand), its operation in the window's journal,
    // its validation, its write, and what a new rush needs on screen.
    // Without it the ingest is refused: this backend has no package of its
    // own to write.
    using IngestCommitCallback = std::function<bool(
        const std::vector<ProbedIngestFile>&, const std::optional<Ulid>&,
        IngestOutcome&, std::string& errorName, std::string& message)>;
    void SetIngestCommitCallback(IngestCommitCallback callback) {
        commit_ingest_ = std::move(callback);
    }

    // F2.4 (R10) -- ROADMAP.md. import_resolve_timeline into the window's
    // timeline. It was refused from the panel and from an agent relayed to
    // the window ("this backend cannot read a Resolve timeline"): the
    // headless command loads and commits the package itself, and the window
    // already holds that project, its lock and its journals. This runs the
    // command's steps (ResolveImport.h) on the window's document and journal
    // and answers what the command answers, DEC15's grades included.
    //
    // Split for the adapter like the ingest above: the manifest, and the
    // copy of a Resolve database of several megabytes, are read on any
    // thread (RunReadResolveTimeline); the plan and the operations run on
    // the thread that owns the document (CommitResolveTimeline), applied to
    // copies swapped in only once the whole import holds and the window has
    // saved them (SetEditCommitCallback), then shown once through
    // `onApplied`.
    struct ResolveTimelineFiles {
        ResolveTimelineImportInput input;
        // The command's status, and its refusal when not 0.
        int status = 0;
        std::string output;
    };
    // False only for a path that is not absolute; what the files say is in
    // `files`, refusals included, for CommitResolveTimeline to answer.
    static bool RunReadResolveTimeline(const std::string& manifestPath,
                                       const std::string& resolveDatabase,
                                       ResolveTimelineFiles& files,
                                       std::string& errorName,
                                       std::string& message);
    bool CommitResolveTimeline(const ResolveTimelineFiles& files,
                               std::string& resultJson, std::string& errorName,
                               std::string& message);
    // The two in a row, on the thread that owns the document.
    bool ImportResolveTimeline(const std::string& manifestPath,
                               const std::string& resolveDatabase,
                               std::string& resultJson, std::string& errorName,
                               std::string& message) override;

    // AX1 -- a background task (`async: true`) runs on the endpoint server's
    // worker over a read-only attachment to the same package, the way the
    // MCP server forks one: it only installs analysis caches, never edits.
    std::unique_ptr<McpBackend> ForkForTask(MediaTaskRunner runner,
                                            std::string& message) override;
    // PRD16 -- ROADMAP.md. The folder of the window's package, where the
    // relay resolves a relative output path (McpSession.h).
    std::string ProjectFolder() const override;
    // GENERATIF-2026-10 GN3 -- the folder of the window's project file.
    std::string PackageDirectory() const override;
    bool SnapshotDocument(Document& document, std::string& message) override;
    bool SnapshotProject(Project& project, std::string& message) override;
    void SetProjectSnapshotCallback(
        std::function<bool(Project&, std::string&)> callback) {
        snapshot_project_ = std::move(callback);
    }
    // AX5 -- the window fills in what it shows; read on the thread that owns
    // the document, like every other read of this backend.
    void SetEditorViewCallback(
        std::function<bool(EditorView&, std::string&)> callback) {
        read_editor_view_ = std::move(callback);
    }
    bool ReadEditorView(EditorView& view, std::string& message) override;

    using VideoOutputCallback =
        std::function<bool(const std::optional<std::string>&, VideoOutputState&,
                           std::string&, std::string&)>;
    void SetVideoOutputCallback(VideoOutputCallback callback) {
        video_output_ = std::move(callback);
    }
    bool AccessVideoOutput(const std::optional<std::string>& screen,
                           VideoOutputState& state, std::string& errorName,
                           std::string& message) override;
    // VQA5 -- and draws it, on the same thread (WindowCapture.h).
    using EditorCaptureCallback =
        std::function<bool(const EditorCaptureRequest&, EditorCapture&,
                           std::string& errorName, std::string& message)>;
    void SetEditorCaptureCallback(EditorCaptureCallback callback) {
        capture_editor_window_ = std::move(callback);
    }
    bool CaptureEditorWindow(const EditorCaptureRequest& request,
                             EditorCapture& capture, std::string& errorName,
                             std::string& message) override;

    // CHAT-PARITE -- ROADMAP.md F2.4 and P7. transcribe_media,
    // transcribe_timeline and align_transcript failed from the chat panel
    // while the same tools worked over MCP. They produce cache files from
    // the committed project, and do it here through the very Cli.cc commands
    // McpProjectBackend runs, with the open project's path: one
    // implementation for the three surfaces, and none of it in this class.
    //
    // Of the live state they read only that path and the timeline on screen,
    // which is what an empty timeline_id means here as for every other live
    // tool -- the one the project file calls active may be another.
    // FreezeCacheTarget() reads both on the thread that owns the document;
    // the Run* functions do the rest from any thread. The chat adapter
    // (ChatPanelView.mm) relies on that split to keep minutes of Whisper off
    // the main thread.
    struct CacheTarget {
        std::string project_path;
        Ulid timeline_id;
        MediaTaskRunner runner;
    };
    CacheTarget FreezeCacheTarget() const;
    static bool RunTranscribeSources(
        const CacheTarget& target, const std::vector<Ulid>& sourceIds,
        const std::string& language, bool verbatim, bool includeSilent,
        bool overwrite, std::string& resultJson, std::string& message,
        WhisperDecodingProfile profile = WhisperDecodingProfile::Default);
    static bool RunTranscribeTimeline(
        const CacheTarget& target, const std::string& timelineId,
        const std::string& language, bool verbatim, std::string& resultJson,
        std::string& message,
        WhisperDecodingProfile profile = WhisperDecodingProfile::Default);
    static bool RunAlignSourceTranscripts(const CacheTarget& target, bool apply,
                                          std::string& resultJson,
                                          std::string& message);
    // AX2 -- MFA, through the editor's task manager: the task bar shows it
    // and cancels it.
    static bool RunAlignSourceWords(const CacheTarget& target,
                                    const std::vector<Ulid>& mediaIds,
                                    const std::string& textsJson,
                                    std::string& resultJson,
                                    std::string& message);

    // CHAT-ANALYSES -- ROADMAP.md F2.4. analyze_shot_quality and
    // analyze_speech_onset decoded the whole rush on the main thread when the
    // chat asked for them: the adapter handed them to callbacks of main.mm
    // under dispatch_sync, and the editor froze for the whole analysis. They
    // take the transcription's split now -- the same Cli.cc commands as
    // McpProjectBackend, from the chat worker, their decoding on the runner
    // -- and so does analyze_audio_beats, which already ran on the worker
    // but on a task manager nobody could see or cancel.
    static bool RunAnalyzeSourceShotQuality(const CacheTarget& target,
                                            const Ulid& sourceId,
                                            std::string& resultJson,
                                            std::string& message);
    static bool RunAnalyzeSourceSpeechOnset(const CacheTarget& target,
                                            const Ulid& sourceId,
                                            const SpeechOnsetSettings& settings,
                                            std::string& resultJson,
                                            std::string& message);
    // SYNCHRO-2026-10 SY1 -- the same split: the project path on the main
    // thread, the decode and the measure on the chat worker.
    static bool RunMeasureAudioOffset(
        const CacheTarget& target, const Ulid& referenceId, const Ulid& otherId,
        const AudioOffsetChannels& channels, std::string& resultJson,
        std::string& errorName, std::string& message);
    // FC6 -- FILMCRAFT-2026-10. The same split as the audio offset.
    static bool RunProposeDucking(const CacheTarget& target,
                                  const Ulid& mediaId,
                                  AudioOffsetChannel channel,
                                  const audio_dsp::DuckingSettings& settings,
                                  std::string& resultJson,
                                  std::string& errorName, std::string& message);
    // R8 -- docs/roadmap/REEL-2026-09.md. Same split: the project path is
    // read on the main thread, the pass runs on the chat worker and decodes
    // on the runner, because it decodes every frame of the ranges the
    // timeline uses.
    static bool RunScanIsolatedFrames(const CacheTarget& target, bool analyze,
                                      const std::vector<Ulid>& mediaIds,
                                      std::string& resultJson,
                                      std::string& message);
    // PA3 -- docs/roadmap/PARITE-2026-11.md. The frozen project path.
    static bool RunAnalyzeSourceSpeakers(const CacheTarget& target,
                                         const Ulid& sourceId, int32_t speakers,
                                         std::string& resultJson,
                                         std::string& message);
    static bool RunAnalyzeSourceAudioBeats(const CacheTarget& target,
                                           const Ulid& sourceId,
                                           std::string& resultJson,
                                           std::string& message);

    // read_frame, contact_sheet and cut_sheet ran FFmpeg on the main thread
    // the same way. They render from the document itself -- its media paths
    // and its colour pipeline -- so what they freeze is a copy of it. No
    // runner: neither FrameCapture.h nor TimelineSheets.h can report progress
    // or be interrupted, so a task bar entry could not be cancelled.
    struct PictureTarget {
        Document document;
        std::string project_path;
    };
    PictureTarget FreezePictureTarget() const;
    static bool RunCaptureSourceFrame(const PictureTarget& target,
                                      const Ulid& sourceId, const Ulid& clipId,
                                      const RationalTime& time,
                                      std::string& jpegBytes,
                                      std::string& message);
    // G7, G3 -- docs/roadmap/GRADE-2026-09.md. The frozen document and path.
    static bool RunReadMediaFileColor(const PictureTarget& target,
                                      const Ulid& mediaId,
                                      MediaFileColor& color,
                                      std::string& message);
    // G2 -- docs/roadmap/GRADE-2026-09.md. The frozen document and path.
    static bool RunMatchClipColor(const PictureTarget& target,
                                  const ColorMatchPlan& plan,
                                  ColorMatchResult& result,
                                  std::string& message);
    // CA3 -- docs/roadmap/CADRAGE-2026-10.md. The frozen document and path.
    static bool RunSampleClipFaces(const PictureTarget& target,
                                   const Ulid& clipId, int64_t everyFrames,
                                   std::vector<SubjectSample>& samples,
                                   std::string& message);
    // PA7 -- docs/roadmap/PARITE-2026-11.md. The frozen document and path.
    static bool RunSampleClipMotion(const PictureTarget& target,
                                    const Ulid& clipId,
                                    std::vector<StabilizationSample>& motion,
                                    std::string& message);
    // AGENT-2026-09 A20 -- the frozen document and path.
    static bool RunProposeClipGrades(const PictureTarget& target,
                                     const GradeProposalPlan& plan,
                                     std::vector<GradeProposal>& proposals,
                                     std::string& message);
    // G1b -- docs/roadmap/GRADE-2026-09.md. The frozen document and path.
    static bool RunCaptureGradedClipFrame(const PictureTarget& target,
                                          const Ulid& clipId,
                                          const RationalTime& time,
                                          std::string& jpegBytes,
                                          std::string& message);
    // G1 -- docs/roadmap/GRADE-2026-09.md. The frozen document and path.
    static bool RunMeasureClipColors(const PictureTarget& target,
                                     const ColorMeasurePlan& plan,
                                     std::vector<ColorClipMeasurement>& results,
                                     std::string& message);
    // IA4 -- the frozen document and path, and where a reading runs.
    static bool RunInspectCut(const PictureTarget& target,
                              const MediaTaskRunner& runner,
                              const CutInspectionPlan& plan,
                              const CutInspectionRenderSettings& settings,
                              CutInspectionResult& result,
                              std::string& message);
    static bool RunCaptureTimelineSheet(const PictureTarget& target,
                                        const TimelineSheetPlan& plan,
                                        const TimelineSheetSettings& settings,
                                        std::string& jpegBytes,
                                        std::string& message);
    // Where the Whisper work runs. main.mm hands the editor's own
    // MediaTaskManager, the one the Media menu's transcription uses, so the
    // task bar shows and cancels a transcription the chat asked for. Empty,
    // the command waits on a manager of its own, as the CLI does.
    void SetMediaTaskRunner(MediaTaskRunner runner) {
        media_task_runner_ = std::move(runner);
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
    // R9 -- docs/roadmap/REEL-2026-09.md. Writes the montage the window is
    // showing, unsaved edits included, rather than the package on disk: the
    // chat panel asks for the timeline in front of it.
    bool ExportResolveXml(const std::string& xmlPath,
                          const std::string& resolveScaling,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) override;
    // R6 -- docs/roadmap/REEL-2026-09.md.
    bool ImportSequenceLut(const std::string& cubePath,
                           const std::string& space, std::string& resultJson,
                           std::string& errorName,
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
    bool ReadTimelineTranscript(std::string& json,
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
    bool TranscribeSources(const std::vector<Ulid>& sourceIds,
                           const std::string& language, bool verbatim,
                           bool includeSilent, bool overwrite,
                           std::string& resultJson, std::string& message,
                           WhisperDecodingProfile profile =
                               WhisperDecodingProfile::Default) override;
    bool TranscribeTimeline(const std::string& timelineId,
                            const std::string& language, bool verbatim,
                            std::string& resultJson, std::string& message,
                            WhisperDecodingProfile profile =
                                WhisperDecodingProfile::Default) override;
    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string& message) override;
    bool AlignSourceWords(const std::vector<Ulid>& mediaIds,
                          const std::string& textsJson, std::string& resultJson,
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
    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
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

private:
    // PL1 -- ROADMAP.md. A person keeps editing while the chat agent works:
    // the panel does not lock the timeline during a turn. Folding the turn
    // must not take a mouse edit with it, or one Undo would revert the
    // person's own work along with the agent's. This backend therefore
    // remembers the entries it appended itself, by the hash of their text,
    // and refuses to fold a history holding any other.
    void RememberAgentEntry();
    std::set<std::string> agent_entries_;
    // S3 -- the staged state, committed (commit_edit_) and then taken by
    // the two references; IoError and nothing taken when the commit fails.
    bool CommitAndTake(Document staged, EditLog stagedLog,
                       std::string& errorName, std::string& message);
    EditCommitCallback commit_edit_;

    std::string project_path_;
    RunStepCommitCallback commit_run_step_;
    std::function<bool(const RunStepReceipt&)> match_run_step_;
    MediaTaskRunner media_task_runner_;
    Document& document_;
    EditLog& edit_log_;
    std::function<void()> on_applied_;
    ProjectApplyCallback apply_project_;
    ProjectDescribeCallback describe_project_;
    std::function<bool(Project&, std::string&)> snapshot_project_;
    std::function<bool(EditorView&, std::string&)> read_editor_view_;
    VideoOutputCallback video_output_;
    EditorCaptureCallback capture_editor_window_;
    IngestCommitCallback commit_ingest_;
    TranscriptCallback read_transcript_;
    SourceTranscriptCallback read_source_transcript_;
    SourceShotQualityCallback read_source_shot_quality_;
    SourceSpeechOnsetCallback read_source_speech_onset_;
    TimelineSelectCallback select_timeline_;
    bool require_explicit_timeline_ = false;
};
