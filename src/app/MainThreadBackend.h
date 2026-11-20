#pragma once

// ObjC++ only: the methods below hand their work to the main queue with
// dispatch_sync and a block.
//
// AX1 -- docs/roadmap/AX-2026-09.md. Moved out of ChatPanelView.mm, where
// the chat panel was its only user: the endpoint an external agent reaches
// (McpEndpoint.h) runs the same catalog over the same live backend from its
// connection threads, and needs exactly this seam. One adapter, so that a
// capability added to the live backend reaches the chat panel and the
// external agent together (tests/architecture_tests.py checks that none is
// dropped here).

#include "AfterBridge.h"
#include "Cli.h"
#include "Document.h"
#include "Export.h"
#include "ExportVerification.h"
#include "LookLutStorage.h"
#include "McpLiveBackend.h"
#include "McpProjectBackend.h"
#include "McpTools.h"
#include "ResolveXmlExport.h"

#include <dispatch/dispatch.h>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------
// MainThreadBackend: wraps the live McpLiveBackend so every actual document
// read/mutation runs on the main thread, serialized with every mouse-driven
// edit and with rendering -- while the chat turn that triggers it (network
// I/O to the LLM, potentially several round trips) runs entirely on a
// background queue (see -sendPressed: below). ChatSession itself is
// thread-agnostic; this is the one seam that makes "run the slow part off
// the main thread, the document-touching part on it" true without
// ChatSession knowing anything about threads.
// ---------------------------------------------------------------------
// Runs `function` on the main queue and waits for its answer: what every
// method below does when it reads or edits the window's document. It was
// spelled out once per method, a __block variable per result and a
// dispatch_sync around one call, fifty-four times; a method whose body
// needs more than one call on the main queue keeps its own block.
// Never call it from the main queue: dispatch_sync would wait on itself.
template <typename Function>
auto OnMainQueue(Function&& function) -> decltype(function()) {
    using Result = decltype(function());
    auto* call = &function;
    if constexpr (std::is_void_v<Result>) {
        dispatch_sync(dispatch_get_main_queue(), ^{
          (*call)();
        });
    } else {
        std::optional<Result> result;
        auto* slot = &result;
        dispatch_sync(dispatch_get_main_queue(), ^{
          slot->emplace((*call)());
        });
        return std::move(*result);
    }
}

class MainThreadBackend : public McpBackend {
public:
    explicit MainThreadBackend(McpLiveBackend& live) : live_(live) {}

    bool CommitRunStep(const RunStepRequest& request, std::string& resultJson,
                       std::string& errorName, std::string& message) override {
        return OnMainQueue([&] {
            return live_.CommitRunStep(request, resultJson, errorName, message);
        });
    }

    bool ReadRunStep(const Ulid& stepId, std::string& resultJson,
                     std::string& errorName, std::string& message) override {
        std::string path = OnMainQueue([&] { return live_.ProjectPath(); });
        if (path.empty()) {
            errorName = "UnsupportedOperation";
            message = "save the project before reading its run receipts";
            return false;
        }
        // OR2 -- ProjectFolder is the package's parent for caller outputs,
        // not the package that contains receipts. Keep the saved path exact.
        McpProjectBackend attached(path);
        if (!attached.ReadRunStep(stepId, resultJson, errorName, message)) {
            return false;
        }
        std::string answer = resultJson;
        std::string detail;
        bool valid = OnMainQueue(
            [&] { return live_.ValidateRunStepState(answer, detail); });
        resultJson = std::move(answer);
        if (!valid) {
            errorName = "ValidationFailed";
            message = detail;
        }
        return valid;
    }

    bool ProjectArchiveStatus(bool retry, std::string& resultJson,
                              std::string& errorName,
                              std::string& message) override {
        // Freeze the window's path on the main thread; SQL delivery itself
        // only touches storage and belongs on the calling agent's worker.
        std::string path = OnMainQueue([&] { return live_.ProjectPath(); });
        if (path.empty()) {
            errorName = "UnsupportedOperation";
            message = "this backend has no stored project package";
            return false;
        }
        McpProjectBackend attached(path);
        return attached.ProjectArchiveStatus(retry, resultJson, errorName,
                                             message);
    }

    bool ImportProjectArchive(const std::string& database,
                              const std::vector<std::string>& projectPaths,
                              std::string& resultJson, std::string& errorName,
                              std::string& message) override {
        McpProjectBackend attached;
        return attached.ImportProjectArchive(database, projectPaths, resultJson,
                                             errorName, message);
    }

    // AX1 -- the project a relayed agent opens is the one the window holds;
    // the answer reads it from the window's state.
    bool OpenProject(const std::string& requestedPath, std::string& resultJson,
                     std::string& errorName, std::string& message) override {
        __block bool opened = false;
        __block std::string answer;
        __block std::string name;
        __block std::string detail;
        dispatch_sync(dispatch_get_main_queue(), ^{
          opened = live_.OpenProject(requestedPath, answer, name, detail);
        });
        resultJson = answer;
        errorName = name;
        message = detail;
        return opened;
    }

    // F2.4 -- ROADMAP.md. Refused, naming the window's project, which only
    // the main thread reads.
    bool CreateProject(const std::string& packagePath, const std::string& name,
                       const std::string& format, std::string& resultJson,
                       std::string& errorName, std::string& message) override {
        __block bool created = false;
        __block std::string answer;
        __block std::string refusal;
        __block std::string detail;
        dispatch_sync(dispatch_get_main_queue(), ^{
          created = live_.CreateProject(packagePath, name, format, answer,
                                        refusal, detail);
        });
        resultJson = answer;
        errorName = refusal;
        message = detail;
        return created;
    }

    // F2.4 -- the window's library on the main thread; the files read on
    // this worker, their sound decoded on the editor's task manager, whose
    // bar shows the import and cancels it; what was read added on the main
    // thread by the window's own import.
    bool IngestMedia(const std::string& mediaPath, bool recursive,
                     const std::optional<Ulid>& targetBin,
                     std::string& resultJson, std::string& errorName,
                     std::string& message) override {
        McpLiveBackend::ProbedIngest probed;
        if (!McpLiveBackend::RunProbeIngest(FreezeIngestTarget(), mediaPath,
                                            recursive, probed, errorName,
                                            message)) {
            return false;
        }
        probed.target_bin = targetBin;
        return CommitIngest(probed, resultJson, errorName, message);
    }

    // F2.4 (R10) -- the manifest and the copy of the Resolve database read on
    // this worker; the plan and the operations on the main thread, against
    // the window's document and journal, where every mouse edit runs.
    bool ImportResolveTimeline(const std::string& manifestPath,
                               const std::string& resolveDatabase,
                               std::string& resultJson, std::string& errorName,
                               std::string& message) override {
        McpLiveBackend::ResolveTimelineFiles files;
        if (!McpLiveBackend::RunReadResolveTimeline(
                manifestPath, resolveDatabase, files, errorName, message)) {
            return false;
        }
        return CommitResolveTimeline(files, resultJson, errorName, message);
    }

    // AX5 -- what the window shows belongs to the main thread.
    bool ReadEditorView(EditorView& view, std::string& message) override {
        __block bool read = false;
        __block EditorView shown;
        __block std::string detail;
        dispatch_sync(dispatch_get_main_queue(), ^{
          read = live_.ReadEditorView(shown, detail);
        });
        view = shown;
        message = detail;
        return read;
    }

    // CF1 -- display enumeration and window changes belong to AppKit.
    bool AccessVideoOutput(const std::optional<std::string>& screen,
                           VideoOutputState& state, std::string& errorName,
                           std::string& message) override {
        __block bool ok = false;
        __block VideoOutputState shown;
        __block std::string name;
        __block std::string detail;
        const auto& asked = screen;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.AccessVideoOutput(asked, shown, name, detail);
        });
        state = std::move(shown);
        errorName = name;
        message = detail;
        return ok;
    }

    // VQA5 -- and so does drawing it.
    bool CaptureEditorWindow(const EditorCaptureRequest& request,
                             EditorCapture& capture, std::string& errorName,
                             std::string& message) override {
        __block bool drawn = false;
        __block EditorCapture image;
        __block std::string name;
        __block std::string detail;
        const EditorCaptureRequest asked = request;
        dispatch_sync(dispatch_get_main_queue(), ^{
          drawn = live_.CaptureEditorWindow(asked, image, name, detail);
        });
        capture = image;
        errorName = name;
        message = detail;
        return drawn;
    }

    // AX1 -- the fork reads the package path on the main thread and then
    // lives on the task's worker, sharing nothing with the window.
    std::unique_ptr<McpBackend> ForkForTask(MediaTaskRunner runner,
                                            std::string& message) override {
        // A raw pointer across the block: a __block variable must be
        // copyable, and the fork is owned by one unique_ptr at a time.
        __block McpBackend* fork = nullptr;
        __block std::string detail;
        dispatch_sync(dispatch_get_main_queue(), ^{
          fork = live_.ForkForTask(runner, detail).release();
        });
        message = detail;
        return std::unique_ptr<McpBackend>(fork);
    }

    // PRD17 -- ROADMAP.md. A session's strict timeline mode, set by an agent
    // relayed to the window (open_project's require_timeline), is this
    // adapter's own: the relay and the chat panel each have one, and a
    // series started through the relay does not change the panel's. Until a
    // session sets it, the window's preference.
    bool RequiresExplicitTimeline() const override {
        if (require_explicit_timeline_set_) return require_explicit_timeline_;
        return OnMainQueue([&] { return live_.RequiresExplicitTimeline(); });
    }
    void SetRequireExplicitTimeline(bool required) override {
        require_explicit_timeline_ = required;
        require_explicit_timeline_set_ = true;
    }

    // PRD16 -- ROADMAP.md. Read on the main thread, like every state of the
    // window; a background export then pins the window's project the same
    // way (SnapshotProject below) and encodes on the task's worker.
    std::string ProjectFolder() const override {
        std::string folder = OnMainQueue([&] { return live_.ProjectFolder(); });
        return folder;
    }
    // GENERATIF-2026-10 GN3 -- read on the main thread, as the folder is.
    std::string PackageDirectory() const override {
        std::string directory =
            OnMainQueue([&] { return live_.PackageDirectory(); });
        return directory;
    }

    // AFTER-COMP -- the bridge reads and writes no Document and blocks until
    // After Effects answers, a rendered frame included: it stays on this
    // worker, never on the main queue.
    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
                         std::string& message) override {
        return RunAfterBridgeRequest(requestJson, resultJson, errorName,
                                     message);
    }

    bool SnapshotDocument(Document& document, std::string& message) override {
        return OnMainQueue(
            [&] { return live_.SnapshotDocument(document, message); });
    }

    // ADOBE-CLI-2026-09 -- the worker must preserve the same project and
    // timeline capabilities as the live backend, including inactive scopes.
    bool SnapshotProject(Project& project, std::string& message) override {
        return OnMainQueue(
            [&] { return live_.SnapshotProject(project, message); });
    }

    bool SelectTimelineForEdit(const std::string& timelineId,
                               std::string& errorName,
                               std::string& message) override {
        return OnMainQueue([&] {
            return live_.SelectTimelineForEdit(timelineId, errorName, message);
        });
    }

    void EndTimelineEdit() override {
        OnMainQueue([&] { live_.EndTimelineEdit(); });
    }

    bool ExportTimeline(const ExportSettings& settings, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        __block Document document;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
        });
        if (!ok) {
            errorName = "IoError";
            return false;
        }
        if (path.empty()) {
            errorName = "InvalidDocument";
            message = "save the project before exporting relative media paths";
            return false;
        }
        // Only snapshot access touches the main thread. Encoding the frozen
        // plan stays on the chat worker, so the editor remains responsive.
        ExportPlan plan;
        if (!Exporter::BuildPlan(document, path, settings, plan, message)) {
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

    // ITW2 -- the path and library on the main thread; decoding and
    // measuring the rush on the chat worker.
    bool AnalyzeSourceHesitations(const Ulid& sourceId,
                                  const HesitationSettings& settings,
                                  HesitationReport& report,
                                  std::string& message) override {
        __block Document document;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
        });
        if (!ok) return false;
        if (path.empty()) {
            message = "save the project before analysing its rushes";
            return false;
        }
        return AnalyzeProjectSourceHesitations(path, document.library, sourceId,
                                               settings, report, message);
    }

    bool ReadSourceHesitations(const Ulid& sourceId, HesitationReport& report,
                               std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadSourceHesitations(sourceId, report, message);
        });
    }

    // ITW8 -- the snapshot, path and floors on the main thread; decoding and
    // measuring the mix on the chat worker.
    bool AnalyzeTimelineAudio(const TimelineAudioSettings& settings,
                              std::string& resultJson,
                              std::string& message) override {
        __block Document document;
        __block std::string path;
        __block std::map<Ulid, SpeechOnsetReport> floors;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
          for (const DocumentTrack& track : document.sequence.tracks) {
              if (track.kind != "audio") continue;
              for (const DocumentClip& clip : track.clips) {
                  if (floors.count(clip.source_id)) continue;
                  SpeechOnsetReport onset;
                  std::string ignored;
                  if (live_.ReadSourceSpeechOnset(clip.source_id, onset,
                                                  ignored))
                      floors.emplace(clip.source_id, std::move(onset));
              }
          }
        });
        if (!ok) return false;
        if (path.empty()) {
            message = "save the project before measuring its audio";
            return false;
        }
        TimelineAudioReport report;
        if (!::AnalyzeTimelineAudio(document, path, floors, settings, report,
                                    message))
            return false;
        resultJson =
            SerializeTimelineAudioReport(report, document.sequence.frame_rate);
        return true;
    }

    bool ReadTimelineWordSpans(std::vector<TimelineTranscriptSpan>& spans,
                               TimelineWordCheck& check,
                               std::string& message) override {
        return OnMainQueue(
            [&] { return live_.ReadTimelineWordSpans(spans, check, message); });
    }

    bool ReadDocumentWordSpans(const Document& document,
                               std::vector<TimelineTranscriptSpan>& spans,
                               TimelineWordCheck& check,
                               std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadDocumentWordSpans(document, spans, check, message);
        });
    }

    // ITW1 -- same split as export: the snapshot on the main thread, the
    // decoding of the file and its sources on the chat worker.
    bool VerifyExportedFile(const std::string& exportPath,
                            std::string& resultJson, std::string& errorName,
                            std::string& message) override {
        __block Document document;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
        });
        if (!ok) {
            errorName = "IoError";
            return false;
        }
        if (path.empty()) {
            errorName = "InvalidDocument";
            message = "save the project before verifying relative media paths";
            return false;
        }
        ExportVerificationReport report;
        if (!VerifyExport(document,
                          std::filesystem::absolute(path).parent_path(),
                          exportPath, {}, report, message)) {
            errorName = "IoError";
            return false;
        }
        resultJson = SerializeExportVerification(report);
        return true;
    }

    // R9 -- docs/roadmap/REEL-2026-09.md. Same split: the snapshot of the
    // montage on the main thread, the writing of the file on the chat worker.
    bool ExportResolveXml(const std::string& xmlPath,
                          const std::string& resolveScaling,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) override {
        ResolveScaling scaling = ResolveScaling::Undeclared;
        if (!resolveScaling.empty() &&
            !ParseResolveScaling(resolveScaling, scaling)) {
            errorName = "ValidationFailed";
            message = "resolve_scaling must be 'fit' or 'fill'";
            return false;
        }
        __block Document document;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
        });
        if (!ok) {
            errorName = "IoError";
            return false;
        }
        if (path.empty()) {
            errorName = "InvalidDocument";
            message = "save the project before writing a Resolve XML: the rush "
                      "paths it names are stored relative to the package";
            return false;
        }
        if (!std::filesystem::path(xmlPath).is_absolute()) {
            errorName = "ValidationFailed";
            message = "xml path must be absolute";
            return false;
        }
        return BuildAndWriteResolveXml(
            std::move(document),
            std::filesystem::absolute(path).parent_path().string(), xmlPath,
            resultJson, errorName, message, scaling);
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. The .cube is read, digested and
    // copied on the worker; only the snapshot and the operation cross to
    // the main queue, where the window's project lives.
    bool ImportSequenceLut(const std::string& cubePath,
                           const std::string& space, std::string& resultJson,
                           std::string& errorName,
                           std::string& message) override {
        __block Document document;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotDocument(document, message);
          path = live_.ProjectPath();
        });
        if (!ok) {
            errorName = "IoError";
            return false;
        }
        if (path.empty()) {
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
        SequenceLut lut;
        if (!ImportLookLutIntoPackage(path, cubePath, space,
                                      document.color_management, lut,
                                      message)) {
            errorName = "ValidationFailed";
            return false;
        }
        return ApplyOperation(SetSequenceLutOperation{lut}, resultJson,
                              errorName, message);
    }

    bool ExportProjectTimelines(
        const std::vector<std::pair<Ulid, ExportSettings>>& items, bool preview,
        std::string& resultJson, std::string& errorName,
        std::string& message) override {
        __block Project project;
        __block std::string path;
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.SnapshotProject(project, message);
          path = live_.ProjectPath();
        });
        if (!ok) {
            errorName = "IoError";
            return false;
        }
        if (path.empty()) {
            errorName = "InvalidDocument";
            message = "save the project before exporting relative media paths";
            return false;
        }
        return Exporter::ExportBatch(project, path, items, preview, resultJson,
                                     errorName, message);
    }

    // CHAT-ANALYSES -- ROADMAP.md F2.4. A sheet runs FFmpeg over up to 64
    // positions: a copy of the document on the main thread, the rendering on
    // the chat worker. read_frame below takes the same split.
    bool CaptureTimelineSheet(const TimelineSheetPlan& plan,
                              const TimelineSheetSettings& settings,
                              std::string& jpegBytes,
                              std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunCaptureTimelineSheet(target, plan, settings,
                                                       jpegBytes, message);
    }

    // IA4 -- an inspection renders a window through the export's graph and
    // may run Whisper on it: a copy of the document and the runner taken on
    // the main thread, the work on the chat worker.
    bool InspectCut(const CutInspectionPlan& plan,
                    const CutInspectionRenderSettings& settings,
                    CutInspectionResult& result,
                    std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        const McpLiveBackend::CacheTarget cache = FreezeCacheTarget();
        return McpLiveBackend::RunInspectCut(target, cache.runner, plan,
                                             settings, result, message);
    }

    // G7, G3 -- a media's file probed on the chat worker.
    bool ReadMediaFileColor(const Ulid& mediaId, MediaFileColor& color,
                            std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunReadMediaFileColor(target, mediaId, color,
                                                     message);
    }

    // G2 -- two clips read and matched on the chat worker, from a copy of
    // the document taken on the main thread.
    bool MatchClipColor(const ColorMatchPlan& plan, ColorMatchResult& result,
                        std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunMatchClipColor(target, plan, result, message);
    }

    // AGENT-2026-09 A20 -- every clip read on its face, on the chat worker.
    bool ProposeClipGrades(const GradeProposalPlan& plan,
                           std::vector<GradeProposal>& proposals,
                           std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunProposeClipGrades(target, plan, proposals,
                                                    message);
    }

    // CA3 -- docs/roadmap/CADRAGE-2026-10.md. The faces of one clip, read
    // on the chat worker from a copy of the document taken on the main
    // thread, as propose_grade reads its frames.
    bool SampleClipFaces(const Ulid& clipId, int64_t everyFrames,
                         std::vector<SubjectSample>& samples,
                         std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunSampleClipFaces(target, clipId, everyFrames,
                                                  samples, message);
    }

    // PA7 -- docs/roadmap/PARITE-2026-11.md. A clip's every frame is
    // decoded: on the chat worker, from a copy taken on the main thread.
    bool SampleClipMotion(const Ulid& clipId,
                          std::vector<StabilizationSample>& motion,
                          std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunSampleClipMotion(target, clipId, motion,
                                                   message);
    }

    // G1b -- the graded frame, frozen and rendered as read_frame is.
    bool CaptureGradedClipFrame(const Ulid& clipId, const RationalTime& time,
                                std::string& jpegBytes,
                                std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunCaptureGradedClipFrame(target, clipId, time,
                                                         jpegBytes, message);
    }

    // G1 -- docs/roadmap/GRADE-2026-09.md. Decoding and measuring take a
    // moment per clip: a copy of the document taken on the main thread,
    // the work on the chat worker, as for read_frame.
    bool MeasureClipColors(const ColorMeasurePlan& plan,
                           std::vector<ColorClipMeasurement>& results,
                           std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunMeasureClipColors(target, plan, results,
                                                    message);
    }

    bool ApplyPreparedOperation(Operation operation,
                                const std::string& expectedDocument,
                                std::string& resultJson, std::string& errorName,
                                std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ApplyPreparedOperation(operation, expectedDocument,
                                            resultJson, errorName, message);
        });
        return ok;
    }

    bool ApplyOperation(Operation operation, std::string& resultJson,
                        std::string& errorName, std::string& message) override {
        return OnMainQueue([&] {
            return live_.ApplyOperation(operation, resultJson, errorName,
                                        message);
        });
    }

    bool ApplyProjectEdit(ProjectOperation operation, std::string& resultJson,
                          std::string& errorName,
                          std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok =
              live_.ApplyProjectEdit(operation, resultJson, errorName, message);
        });
        return ok;
    }

    bool ReadTimelineTranscript(std::string& json,
                                std::string& message) override {
        return OnMainQueue(
            [&] { return live_.ReadTimelineTranscript(json, message); });
    }

    // Forwarded like everything else. Leaving these to McpBackend's
    // "this backend has no project cache" defaults silently disabled every
    // transcript- and picture-driven tool from the chat panel while the same
    // tools worked over MCP -- the exact kind of surface-specific behaviour
    // PHILOSOPHY.md principle 3 rules out.
    bool ReadSourceTranscript(const Ulid& sourceId, Transcript& transcript,
                              std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadSourceTranscript(sourceId, transcript, message);
        });
    }

    ShotQualityCacheRead ReadSourceShotQuality(
        const Ulid& sourceId, ShotQualityReport& report) override {
        ShotQualityCacheRead read = OnMainQueue(
            [&] { return live_.ReadSourceShotQuality(sourceId, report); });
        return read;
    }

    bool ReadSourceAudioBeats(const Ulid& sourceId, BeatDetectionResult& result,
                              std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadSourceAudioBeats(sourceId, result, message);
        });
    }

    // PA3 -- docs/roadmap/PARITE-2026-11.md. The cache is read on the main
    // thread, as the beats'; the analysis freezes only the project path and
    // runs on the chat worker, in the task bar.
    bool ReadSourceSpeakers(const Ulid& sourceId, SpeakerDiarization& speakers,
                            std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadSourceSpeakers(sourceId, speakers, message);
        });
    }

    bool AnalyzeSourceSpeakers(const Ulid& sourceId, int32_t speakers,
                               std::string& resultJson,
                               std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAnalyzeSourceSpeakers(
            target, sourceId, speakers, resultJson, message);
    }

    // F1.6 -- freeze only the project path on the UI thread. FFmpeg's whole
    // source decode stays on the chat worker instead of freezing the editor.
    // CHAT-ANALYSES -- and runs as a task of the editor's manager, like the
    // two analyses below (McpLiveBackend.h).
    bool AnalyzeSourceAudioBeats(const Ulid& sourceId, std::string& resultJson,
                                 std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAnalyzeSourceAudioBeats(target, sourceId,
                                                          resultJson, message);
    }

    // CHAT-ANALYSES -- ROADMAP.md F2.4. Both decode the whole rush. They ran
    // under dispatch_sync, so the editor froze for the whole analysis; now
    // the transcription's split: the project path on the main thread, the
    // command on the chat worker, the decoding in the task bar.
    bool AnalyzeSourceShotQuality(const Ulid& sourceId, std::string& resultJson,
                                  std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAnalyzeSourceShotQuality(target, sourceId,
                                                           resultJson, message);
    }

    // R8 -- docs/roadmap/REEL-2026-09.md. Decodes every frame of the ranges
    // the timeline uses, so it takes the same split as the analyses above.
    bool ScanIsolatedFrames(bool analyze, const std::vector<Ulid>& mediaIds,
                            std::string& resultJson,
                            std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunScanIsolatedFrames(target, analyze, mediaIds,
                                                     resultJson, message);
    }

    bool ReadSourceSpeechOnset(const Ulid& sourceId, SpeechOnsetReport& report,
                               std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadSourceSpeechOnset(sourceId, report, message);
        });
    }

    bool AnalyzeSourceSpeechOnset(const Ulid& sourceId,
                                  const SpeechOnsetSettings& settings,
                                  std::string& resultJson,
                                  std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAnalyzeSourceSpeechOnset(
            target, sourceId, settings, resultJson, message);
    }

    // SYNCHRO-2026-10 SY1 -- the project path on the main thread, the
    // decode and the measure on the chat worker.
    bool MeasureAudioOffset(const Ulid& referenceId, const Ulid& otherId,
                            const AudioOffsetChannels& channels,
                            std::string& resultJson, std::string& errorName,
                            std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunMeasureAudioOffset(
            target, referenceId, otherId, channels, resultJson, errorName,
            message);
    }

    // FC6 -- FILMCRAFT-2026-10. The same split: the project path on the
    // main thread, the decode and the analysis on the chat worker.
    bool ProposeDucking(const Ulid& mediaId, AudioOffsetChannel channel,
                        const audio_dsp::DuckingSettings& settings,
                        std::string& resultJson, std::string& errorName,
                        std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunProposeDucking(
            target, mediaId, channel, settings, resultJson, errorName, message);
    }

    bool CaptureSourceFrame(const Ulid& sourceId, const Ulid& clipId,
                            const RationalTime& time, std::string& jpegBytes,
                            std::string& message) override {
        const McpLiveBackend::PictureTarget target = FreezePictureTarget();
        return McpLiveBackend::RunCaptureSourceFrame(target, sourceId, clipId,
                                                     time, jpegBytes, message);
    }

    // CHAT-PARITE -- ROADMAP.md F2.4 and P7. A transcription lasts minutes.
    // Only the project path and the timeline on screen are read on the main
    // thread; the command runs from the chat worker, and its Whisper work
    // runs on the editor's own task manager, where the task bar shows it and
    // can cancel it (McpLiveBackend.h). The alignment pass has no Whisper in
    // it but reads and rewrites every cached transcript: same split.
    bool TranscribeSources(
        const std::vector<Ulid>& sourceIds, const std::string& language,
        bool verbatim, bool includeSilent, bool overwrite,
        std::string& resultJson, std::string& message,
        const WhisperDecodingRequest& profile = std::nullopt) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunTranscribeSources(
            target, sourceIds, language, verbatim, includeSilent, overwrite,
            resultJson, message, profile);
    }

    bool TranscribeTimeline(
        const std::string& timelineId, const std::string& language,
        bool verbatim, std::string& resultJson, std::string& message,
        const WhisperDecodingRequest& profile = std::nullopt) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunTranscribeTimeline(
            target, timelineId, language, verbatim, resultJson, message,
            profile);
    }

    bool AlignSourceTranscripts(bool apply, std::string& resultJson,
                                std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAlignSourceTranscripts(target, apply,
                                                         resultJson, message);
    }

    // AX2 -- MFA runs for minutes: from this worker, never the main queue.
    bool AlignSourceWords(const std::vector<Ulid>& mediaIds,
                          const std::string& textsJson, std::string& resultJson,
                          std::string& message) override {
        const McpLiveBackend::CacheTarget target = FreezeCacheTarget();
        return McpLiveBackend::RunAlignSourceWords(target, mediaIds, textsJson,
                                                   resultJson, message);
    }

    bool Undo(std::string& resultJson, std::string& errorName,
              std::string& message) override {
        return OnMainQueue(
            [&] { return live_.Undo(resultJson, errorName, message); });
    }

    bool Redo(std::string& resultJson, std::string& errorName,
              std::string& message) override {
        return OnMainQueue(
            [&] { return live_.Redo(resultJson, errorName, message); });
    }

    // PL1 -- ROADMAP.md. ChatSession marks and folds each turn; the journal
    // belongs to the main thread like every other edit.
    bool BeginIntervention(std::string& resultJson, std::string& errorName,
                           std::string& message) override {
        return OnMainQueue([&] {
            return live_.BeginIntervention(resultJson, errorName, message);
        });
    }

    bool EndIntervention(const std::string& intervention,
                         const std::string& label, std::string& resultJson,
                         std::string& errorName,
                         std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.EndIntervention(intervention, label, resultJson, errorName,
                                     message);
        });
        return ok;
    }

    // MOTIF2 -- the motif of a call is the live backend's, which applies
    // on the main thread; set there like every other state of a call.
    void SetEditMotifs(EditMotifs motifs) override {
        OnMainQueue([&] { live_.SetEditMotifs(std::move(motifs)); });
    }
    EditMotifs CurrentEditMotifs() const override {
        EditMotifs current =
            OnMainQueue([&] { return live_.CurrentEditMotifs(); });
        return current;
    }

    // MOTIF4 -- read on the main thread, like the history.
    bool ReadTimelineMotifs(const std::optional<MotifReason>& reason,
                            const std::string& rule, std::string& json,
                            std::string& errorName,
                            std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadTimelineMotifs(reason, rule, json, errorName,
                                            message);
        });
    }
    // MOTIF6 -- read on the main thread, where the window commits.
    bool ReadProjectMotifs(const std::optional<MotifReason>& reason,
                           const std::string& rule, std::string& json,
                           std::string& errorName,
                           std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadProjectMotifs(reason, rule, json, errorName,
                                           message);
        });
    }

    // PA9 -- read against the journal, on its thread.
    bool ReadTimelineChangesJson(const std::string& sinceHash,
                                 std::string& json, std::string& errorName,
                                 std::string& message) override {
        __block bool ok = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          ok = live_.ReadTimelineChangesJson(sinceHash, json, errorName,
                                             message);
        });
        return ok;
    }

    // PRD35 -- the journal belongs to the main thread like every edit.
    bool ReadTimelineHistory(TimelineHistory& history, std::string& errorName,
                             std::string& message) override {
        return OnMainQueue([&] {
            return live_.ReadTimelineHistory(history, errorName, message);
        });
    }

    bool Describe(std::string& json, std::string& message) override {
        return OnMainQueue([&] { return live_.Describe(json, message); });
    }

private:
    McpLiveBackend::CacheTarget FreezeCacheTarget() {
        McpLiveBackend::CacheTarget target =
            OnMainQueue([&] { return live_.FreezeCacheTarget(); });
        return target;
    }

    McpLiveBackend::PictureTarget FreezePictureTarget() {
        McpLiveBackend::PictureTarget target =
            OnMainQueue([&] { return live_.FreezePictureTarget(); });
        return target;
    }

    McpLiveBackend::IngestTarget FreezeIngestTarget() {
        McpLiveBackend::IngestTarget target =
            OnMainQueue([&] { return live_.FreezeIngestTarget(); });
        return target;
    }

    bool CommitIngest(const McpLiveBackend::ProbedIngest& probed,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
        __block bool committed = false;
        __block std::string answer;
        __block std::string refusal;
        __block std::string detail;
        const McpLiveBackend::ProbedIngest* read = &probed;
        dispatch_sync(dispatch_get_main_queue(), ^{
          committed = live_.CommitIngest(*read, answer, refusal, detail);
        });
        resultJson = answer;
        errorName = refusal;
        message = detail;
        return committed;
    }

    bool CommitResolveTimeline(
        const McpLiveBackend::ResolveTimelineFiles& files,
        std::string& resultJson, std::string& errorName, std::string& message) {
        __block bool committed = false;
        __block std::string answer;
        __block std::string refusal;
        __block std::string detail;
        const McpLiveBackend::ResolveTimelineFiles* read = &files;
        dispatch_sync(dispatch_get_main_queue(), ^{
          committed =
              live_.CommitResolveTimeline(*read, answer, refusal, detail);
        });
        resultJson = answer;
        errorName = refusal;
        message = detail;
        return committed;
    }

    McpLiveBackend& live_;
    // Read and written on the session's own thread, one call at a time.
    bool require_explicit_timeline_ = false;
    bool require_explicit_timeline_set_ = false;
};
