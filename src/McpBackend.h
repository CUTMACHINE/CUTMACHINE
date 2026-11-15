#pragma once

#include "WhisperDecodingProfile.h"

// Backend seam between the transport (McpServer/HttpServer) and tool
// dispatch (McpTools) on one side, and the actual edit path on the other.
//
// There are two implementations:
//  - McpProjectBackend (McpProjectBackend.h): the production backend. It
//    reuses ApplyOperationCommand/UndoOperationCommand/RedoOperationCommand/
//    DescribeCommand from Cli.cc verbatim -- the exact same load/apply/
//    commit path a `--apply-op` CLI invocation takes. It depends on
//    ProjectStorage, so (like Cli.cc's project-facing commands) it only
//    links where CommonCrypto is available (macOS).
//  - A pure in-memory backend used by tests (tests/mcp_tools_tests.cc),
//    which calls EditLog::Apply/Undo/Redo directly against a Document held
//    in memory. That is still the exact same apply path -- EditLog::Apply
//    is what ApplyOperationCommand calls -- just without the project-file
//    round trip, so it builds and runs without FFmpeg/CommonCrypto/AppKit
//    for local iteration and CI on non-macOS hosts.
//
// Either way, McpTools.cc never mutates a Document itself: every tool
// dispatch function ends by constructing an Operation and handing it to
// this interface. Errors are reported by name (the same strings
// EditErrorName() in Operations.h produces) rather than as an EditError
// enum, so both implementations -- one working from a JSON string returned
// by a Cli.cc command, the other from a live EditError -- report through
// one shape without a reverse enum<->string mapping.

#include "AudioDspDucking.h"
#include "AudioOffset.h"
#include "BeatDetection.h"
#include "ColorMeasure.h"
#include "CutInspectionRender.h"
#include "Document.h"
#include "EditLog.h"
#include "Export.h"
#include "FrameCapture.h"
#include "Hesitations.h"
#include "Ingest.h"
#include "InterviewShort.h"
#include "Operations.h"
#include "RunStep.h"
#include "ShotQuality.h"
#include "SpeakerDiarization.h"
#include "SpeechOnset.h"
#include "Stabilization.h"
#include "SubjectTracking.h"
#include "TimelineAudioReport.h"
#include "TimelineSheets.h"
#include "Transcription.h"
#include "VideoOutput.h"

#include "MediaTaskManager.h"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// AX5 -- docs/roadmap/AX-2026-09.md. What the editor is looking at, in the
// app an agent is relayed to (McpEndpoint.h): the timeline on screen, where
// the playhead stands, which clips are selected. Interface state, read and
// never written: nothing of it reaches the document.
struct EditorView {
    Ulid timeline_id;
    RationalTime playhead{0, 1};
    std::vector<Ulid> selected_clip_ids;
    // D5 -- docs/roadmap/DIALOGUE-2026-10.md. The In and Out the editor set
    // on the timeline on screen (I, O), each absent until set: the range
    // delete_timeline_range takes as `in` and `out`, Out excluded.
    std::optional<RationalTime> mark_in;
    std::optional<RationalTime> mark_out;
};

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. The same window as pixels,
// drawn by the app itself without the screen (WindowCapture.h): what an
// agent asked about the interface could not see, since a session has no
// screen-recording permission. Interface state, read and never written.
struct EditorCaptureRequest {
    // "window", title bar included, or one Metal surface: "timeline",
    // "record", "source".
    std::string region = "window";
    // Pixels per point: 1, or 2 for the backing pixels of a Retina screen.
    int32_t scale = 1;
};

struct EditorCaptureRegion {
    std::string name;
    // In the image's pixels, from its top-left corner.
    int64_t x = 0;
    int64_t y = 0;
    int64_t width = 0;
    int64_t height = 0;
};

struct EditorCapture {
    std::string jpeg;
    int64_t width = 0;
    int64_t height = 0;
    // The Metal surfaces the image shows, where it shows them.
    std::vector<EditorCaptureRegion> regions;
};

class McpBackend {
public:
    virtual ~McpBackend() = default;

    // DB3, OR2 -- durable evidence belongs to the same edit/commit boundary
    // as the montage. Memory-only backends cannot promise restart replay.
    virtual bool CommitRunStep(const RunStepRequest&, std::string&,
                               std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend has no durable run step committer";
        return false;
    }
    virtual bool ReadRunStep(const Ulid&, std::string&, std::string& errorName,
                             std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend has no stored run receipts";
        return false;
    }

    // DB2 -- storage maintenance stays behind the backend so tool dispatch
    // and in-memory tests do not depend on the package's platform I/O.
    virtual bool ProjectArchiveStatus(bool, std::string&,
                                      std::string& errorName,
                                      std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend has no stored project package";
        return false;
    }
    virtual bool ImportProjectArchive(const std::string&,
                                      const std::vector<std::string>&,
                                      std::string&, std::string& errorName,
                                      std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot import stored projects";
        return false;
    }

    // IA7 -- ROADMAP.md. A run that starts from a folder of rushes has no
    // project yet, so a server can be started with none, then create or
    // open one and hold that project's session lock -- only ever one. Every
    // other tool is refused until then. Backends bound to one document (the
    // app, tests) always have it open and keep the unsupported defaults.
    virtual bool HasOpenProject() const { return true; }
    virtual bool CreateProject(const std::string&, const std::string&,
                               const std::string&, std::string&,
                               std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot create a project";
        return false;
    }
    virtual bool OpenProject(const std::string&, std::string&,
                             std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot open another project";
        return false;
    }
    // The same scan and audio measurement `--ingest` runs, on the open
    // project, under the lock this backend already holds. DEC32 (PRD7) --
    // `targetBin` as IngestCommand takes it: unset, known rushes stay where
    // they are; set ("" is the root), what is ingested is filed there.
    virtual bool IngestMedia(const std::string&, bool,
                             const std::optional<Ulid>& /*targetBin*/,
                             std::string&, std::string& errorName,
                             std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot ingest media";
        return false;
    }
    // R10 -- docs/roadmap/REEL-2026-09.md. Reads a Resolve timeline manifest
    // into the open project's active timeline, the same plan/refuse/commit
    // path `--import-resolve-timeline` takes. DEC15 (GR15) -- the second
    // argument, when not empty, is the Resolve project's Project.db, whose
    // grades the answer carries (ResolveImport.h).
    virtual bool ImportResolveTimeline(const std::string&, const std::string&,
                                       std::string&, std::string& errorName,
                                       std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot read a Resolve timeline";
        return false;
    }

    // R6 -- docs/roadmap/REEL-2026-09.md. Copies a .cube into the open
    // project's package, under its digest, and lays it on the selected
    // timeline through SetSequenceLutOperation. The copy is why this is a
    // backend capability and not a plain tool dispatch: the operation
    // carries a reference, and something has to put the bytes where every
    // machine that opens the package will find them.
    virtual bool ImportSequenceLut(const std::string&, const std::string&,
                                   std::string&, std::string& errorName,
                                   std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot import a look LUT";
        return false;
    }

    // R9 -- docs/roadmap/REEL-2026-09.md. Writes the selected timeline as a
    // FCP7 XML (xmeml v5) file, the return trip that carries a speed and a
    // dissolve where AppendToTimeline carries neither. Read-only on the
    // project: it produces a file and changes no document.
    // `resolveScaling` is "fit", "fill" or empty (ResolveScaling).
    virtual bool ExportResolveXml(const std::string&, const std::string&,
                                  std::string&, std::string& errorName,
                                  std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot write a Resolve XML";
        return false;
    }

    // Read-only snapshot of the current document, used only to resolve
    // short IDs (IdResolver) before an operation is constructed.
    virtual bool SnapshotDocument(Document& document, std::string& message) = 0;

    // ADOBE-CLI-2026-09 -- batch previews need the complete project, including
    // inactive templates, while keeping the live document untouched.
    virtual bool SnapshotProject(Project&, std::string& message) {
        message = "this backend cannot snapshot project timelines";
        return false;
    }

    // ADOBE-CLI-2026-09 -- prepared range edits contain exact track states.
    // Refuse a stale snapshot rather than overwriting a concurrent edit.
    // Live adapters must keep this check and ApplyOperation on one thread.
    virtual bool ApplyPreparedOperation(Operation operation,
                                        const std::string& expectedDocument,
                                        std::string& resultJson,
                                        std::string& errorName,
                                        std::string& message) {
        Document current;
        if (!SnapshotDocument(current, message)) {
            errorName = "IoError";
            return false;
        }
        if (current.SaveToString() != expectedDocument) {
            errorName = "StaleDocument";
            message = "the timeline changed while preparing the range edit";
            return false;
        }
        return ApplyOperation(std::move(operation), resultJson, errorName,
                              message);
    }

    virtual bool ExportProjectTimelines(
        const std::vector<std::pair<Ulid, ExportSettings>>&, bool, std::string&,
        std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend has no batch timeline exporter";
        return false;
    }

    // B9 -- scopes one timeline-editing tool call. The default backend only
    // owns one document, so it accepts that document's id and rejects any
    // other explicit timeline. Project/live backends override this to select
    // another timeline without changing persisted active-project state.
    virtual bool SelectTimelineForEdit(const std::string& timelineId,
                                       std::string& errorName,
                                       std::string& message) {
        if (timelineId.empty()) return true;
        Document document;
        if (!SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        if (document.sequence.id == timelineId) return true;
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + timelineId + "'";
        return false;
    }
    virtual void EndTimelineEdit() {}

    // PL3 -- ROADMAP.md. Scopes one call that reads a timeline -- or prepares
    // a plan on a copy of it -- without making it the active one: nothing
    // navigates, nothing is written, strict mode does not apply to a read.
    // EndTimelineEdit ends the scope as it does an edit's. The default holds
    // one document and reads only that one; a backend that switches
    // timelines to edit them (the app) keeps this default rather than
    // switching to read. McpProjectBackend reads any timeline of its project.
    virtual bool SelectTimelineForRead(const std::string& timelineId,
                                       std::string& errorName,
                                       std::string& message) {
        if (timelineId.empty()) return true;
        Document document;
        if (!SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        if (document.sequence.id == timelineId) return true;
        errorName = "UnknownSequence";
        message = "timeline_id '" + timelineId +
                  "' is not the timeline open here ('" + document.sequence.id +
                  "'), and reading it would mean switching to it; read it "
                  "through the project (--mcp-serve, --call-tool) instead";
        return false;
    }

    // PRD17 -- ROADMAP.md. Strict mode: every call that edits or reads one
    // timeline must name it (timeline_id). In a series -- N agents editing
    // N timelines of one project through one server -- the active timeline
    // is nobody's: an agent that forgets its timeline_id edits, or reads,
    // another agent's film. Set when the backend is made (the
    // CMRequireExplicitTimeline preference, `--mcp-serve
    // --require-timeline`), and for the session by open_project and
    // create_project (`require_timeline`). McpToolRegistry::Call refuses by
    // name (TimelineRequired, with the timelines there are). A backend that
    // keeps no mode answers false and ignores the request.
    virtual bool RequiresExplicitTimeline() const { return false; }
    virtual void SetRequireExplicitTimeline(bool) {}

    // MOTIF2 -- docs/roadmap/MOTIF-2026-10.md. Why the edits of the call
    // under way are made: the `motif` its caller gave, which
    // McpToolRegistry::Call sets for the call and clears after it, and which
    // a tool whose rule decides the cut replaces with its own for the
    // operation it applies (McpTools.cc). `members` are motifs already
    // placed on a batch's members (a plan's steps), the call's motif
    // covering the rest. Every journaling path of a backend --
    // ApplyOperation, ApplyPreparedOperation, EndIntervention -- records
    // EntryMotifsFor(operation, members, motif) on the entry it writes.
    struct EditMotifs {
        std::optional<EditMotif> motif;
        std::vector<EntryMotif> members;
    };
    virtual void SetEditMotifs(EditMotifs motifs) {
        edit_motifs_ = std::move(motifs);
    }
    virtual EditMotifs CurrentEditMotifs() const { return edit_motifs_; }

    // The package this backend edits, empty when it holds none.
    virtual std::string ProjectPath() const { return {}; }

    // Applies a freshly constructed (not-yet-applied) operation through the
    // same path as `--apply-op`. On success, resultJson holds the tool
    // result payload to report back over MCP. On failure, errorName is one
    // of the EditErrorName() strings from Operations.h.
    virtual bool ApplyOperation(Operation operation, std::string& resultJson,
                                std::string& errorName,
                                std::string& message) = 0;

    // Project-level edit used by agent-created timelines. Backends that only
    // expose a standalone Document may leave this unsupported. MOTIF6 --
    // every backend that journals one records ProjectEntryMotifsForCall()
    // on the project journal's entry.
    virtual bool ApplyProjectEdit(ProjectOperation, std::string&,
                                  std::string& errorName,
                                  std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot create project timelines";
        return false;
    }

    // ADOBE-CLI-2026-09 -- export consumes the selected snapshot without
    // changing the document or its edit journal.
    virtual bool ExportTimeline(const ExportSettings&, std::string&,
                                std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend has no timeline exporter";
        return false;
    }

    // ITW1 -- ROADMAP.md. Compares a delivered file with the selected
    // timeline, frame by frame (ExportVerification.h). Behind the backend
    // because relative media paths resolve against the open project, exactly
    // as they do for the export being checked.
    virtual bool VerifyExportedFile(const std::string&, std::string&,
                                    std::string& errorName,
                                    std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot verify an exported file";
        return false;
    }

    // Read-only cached transcript view. Kept behind the backend because the
    // cache directory belongs to the open project, not the Document JSON.
    virtual bool ReadTimelineTranscript(std::string&, std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // ITW2 -- ROADMAP.md. The per-rush hesitation analysis (Hesitations.h):
    // produced from the signal and placed against the aligned words, then
    // read back by list_disfluencies. Behind the backend for the media path
    // and the cache, which belong to the open project.
    virtual bool AnalyzeSourceHesitations(const Ulid&,
                                          const HesitationSettings&,
                                          HesitationReport&,
                                          std::string& message) {
        message = "this backend cannot analyse hesitations";
        return false;
    }
    virtual bool ReadSourceHesitations(const Ulid&, HesitationReport&,
                                       std::string& message) {
        message = "this backend has no hesitation cache";
        return false;
    }

    // ITW8 -- ROADMAP.md. Measures the selected timeline's assembled sound
    // (TimelineAudioReport.h). Behind the backend because the media paths
    // and the speech onset caches that give each rush its floor belong to
    // the open project.
    virtual bool AnalyzeTimelineAudio(const TimelineAudioSettings&,
                                      std::string&, std::string& message) {
        message = "this backend cannot measure timeline audio";
        return false;
    }

    // ITW4 -- ROADMAP.md. The selected timeline's words, one span per word,
    // as the SRT export and the caption generator read them: aligned
    // boundaries, words no montage reading contains left out and reported.
    virtual bool ReadTimelineWordSpans(std::vector<TimelineTranscriptSpan>&,
                                       TimelineWordCheck&,
                                       std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // OR3 -- ROADMAP.md. A prepared caption edit must project cached words
    // onto its draft, including earlier steps, never onto the live timeline.
    // The cache reader also checks montage readings against that draft's
    // audio identity, so a reading of the pre-edit mix cannot filter words.
    virtual bool ReadDocumentWordSpans(const Document&,
                                       std::vector<TimelineTranscriptSpan>&,
                                       TimelineWordCheck&,
                                       std::string& message) {
        message = "this backend cannot project words onto a document copy";
        return false;
    }

    // One mounted source's cached transcript, keyed on the media identity
    // DocumentSource::id and LibraryMedia::id share. Separate from
    // ReadTimelineTranscript because word-level editing addresses a single
    // clip's own source, not the flattened view of the audible timeline.
    virtual bool ReadSourceTranscript(const Ulid&, Transcript&,
                                      std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // The same view, structured. Editorial selection addresses spans by id
    // and never by time, so the tool that builds a short needs the spans as
    // objects rather than as the rendered JSON a model reads. The default
    // goes through ReadTimelineTranscript and parses its own output, so a
    // backend that can only produce the rendered view still answers this
    // correctly instead of having to grow a second cache path.
    virtual bool ReadTimelineTranscriptSpans(
        std::vector<TimelineTranscriptSpan>& spans, std::string& message) {
        std::string json;
        if (!ReadTimelineTranscript(json, message)) return false;
        return ParseTimelineTranscriptSpans(json, spans, message);
    }

    // Renders one frame of a mounted source, as JPEG bytes, at an exact
    // position in that source's own time domain. Behind the backend because
    // resolving a media id to a file on disk is the open project's job.
    // See FrameCapture.h for why looking is a capability at all.
    // R1 -- `clipId` is the clip the frame was asked for, empty when a
    // media was asked for directly. A framed clip comes back composed into
    // the sequence frame (ClipFraming.h), because an agent that reframes a
    // shot has to see what the sequence shows, not the whole rush.
    virtual bool CaptureSourceFrame(const Ulid&, const Ulid&,
                                    const RationalTime&, std::string&,
                                    std::string& message) {
        message = "this backend cannot render a frame";
        return false;
    }

    // QC-2026-09 (A6) -- unlike read_frame, a timeline sheet depends on the
    // mounted media paths and on the document colour pipeline. The engine
    // has already selected every exact position in `plan`; the backend only
    // resolves those files and renders the read-only result.
    virtual bool CaptureTimelineSheet(const TimelineSheetPlan&,
                                      const TimelineSheetSettings&,
                                      std::string&, std::string& message) {
        message = "this backend cannot render a timeline sheet";
        return false;
    }

    // IA4 -- docs/roadmap/IA-2026-09.md. Renders an inspection window
    // through the export's graph and reads it back (CutInspectionRender.h).
    // The engine has chosen every frame in `plan`; the backend resolves the
    // project's files and says where a reading of the mix runs.
    virtual bool InspectCut(const CutInspectionPlan&,
                            const CutInspectionRenderSettings&,
                            CutInspectionResult&, std::string& message) {
        message = "this backend cannot render an inspection";
        return false;
    }

    // G1b -- docs/roadmap/GRADE-2026-09.md. read_frame with `graded`: the
    // clip's frame at `time`, graded, with the sequence look
    // (CaptureGradedClipFrame, ColorMeasure.h).
    virtual bool CaptureGradedClipFrame(const Ulid&, const RationalTime&,
                                        std::string&, std::string& message) {
        message = "this backend cannot render a graded frame";
        return false;
    }

    // G7, G3 -- docs/roadmap/GRADE-2026-09.md. What ingest reads of a
    // media's file colour (ReadFileColorFacts): its camera's white balance
    // and the input it declares. Behind the backend because resolving a
    // media to its file is the project's.
    virtual bool ReadMediaFileColor(const Ulid&, MediaFileColor&,
                                    std::string& message) {
        message = "this backend cannot read a media's file";
        return false;
    }

    // G2 -- docs/roadmap/GRADE-2026-09.md. Reads two clips and solves the
    // correction matching one to the other (RenderColorMatch). Changes
    // nothing.
    virtual bool MatchClipColor(const ColorMatchPlan&, ColorMatchResult&,
                                std::string& message) {
        message = "this backend cannot match clips";
        return false;
    }

    // AGENT-2026-09 A20 -- docs/roadmap/AGENT-2026-09.md. Reads every clip
    // of the plan on its face or on a patch and proposes its exposure and
    // balance (RenderGradeProposals). Changes nothing.
    virtual bool ProposeClipGrades(const GradeProposalPlan&,
                                   std::vector<GradeProposal>&,
                                   std::string& message) {
        message = "this backend cannot propose grades";
        return false;
    }

    // CA3 -- docs/roadmap/CADRAGE-2026-10.md. The faces of one clip, every
    // `everyFrames`-th frame, through the face finder the executable
    // installed (SampleClipFaces, SubjectTracking.h). Changes nothing.
    virtual bool SampleClipFaces(const Ulid&, int64_t,
                                 std::vector<SubjectSample>&,
                                 std::string& message) {
        message = "this backend cannot read faces";
        return false;
    }

    // PA7 -- docs/roadmap/PARITE-2026-11.md. The movement between every
    // frame of one clip and the next, read from its rush (SampleClipMotion,
    // Stabilization.h). Changes nothing.
    virtual bool SampleClipMotion(const Ulid&,
                                  std::vector<StabilizationSample>&,
                                  std::string& message) {
        message = "this backend cannot read a clip's movement";
        return false;
    }

    // G1 -- docs/roadmap/GRADE-2026-09.md. Decodes, grades and measures
    // one frame of each clip in `plan` (ColorMeasure.h). Behind the backend
    // for read_frame's reason: the media paths and the look's package
    // belong to the open project.
    virtual bool MeasureClipColors(const ColorMeasurePlan&,
                                   std::vector<ColorClipMeasurement>&,
                                   std::string& message) {
        message = "this backend cannot measure a frame";
        return false;
    }

    // Produces the picture-quality analysis this backend can otherwise only
    // read. Without it the tool catalog could report grades and never make
    // one, so an agent depended on a human running the CLI first -- the
    // engine-first rule broken at the last step (PHILOSOPHY.md principle 3).
    // Long-running by nature: it decodes the whole source.
    virtual bool AnalyzeSourceShotQuality(const Ulid&, std::string&,
                                          std::string& message) {
        message = "this backend cannot run a shot quality analysis";
        return false;
    }

    // Produces the transcript this backend can otherwise only read. Same
    // asymmetry AnalyzeSourceShotQuality closed for picture quality, and the
    // more costly one: every word-level tool in this catalog
    // (list_disfluencies, remove_words, the interview short) needs a
    // transcript, so without this the agent's whole editorial path depended
    // on a human running `--transcribe` first. The Whisper model is not an
    // argument -- it is a local setting the engine resolves
    // (Transcription.h) -- because an agent has no way to know where a human
    // keeps a ggml file. Long-running by nature: it decodes and infers over
    // the whole audio.
    //
    // QC-2026-09 A3 -- takes a list because loading the model costs about 8 s
    // and used to be paid once per media. `includeSilent` overrides the skip
    // of media the document records as mute. PRD40 -- `overwrite` installs
    // each new transcript even where the cached one looks better
    // (TranscribeMediaCommand).
    virtual bool TranscribeSources(
        const std::vector<Ulid>&, const std::string&, bool, bool, bool,
        std::string&, std::string& message,
        WhisperDecodingProfile = WhisperDecodingProfile::Default) {
        message = "this backend cannot run a transcription";
        return false;
    }

    // B7 -- the timeline is not a source media: its audible PCM is assembled
    // from clips and may be cached only against that exact arrangement.
    virtual bool TranscribeTimeline(
        const std::string&, const std::string&, bool, std::string&,
        std::string& message,
        WhisperDecodingProfile = WhisperDecodingProfile::Default) {
        message = "this backend cannot transcribe a timeline";
        return false;
    }

    // Read-only cached picture-quality report for one mounted source, keyed
    // on the same media identity DocumentSource::id and LibraryMedia::id
    // share. Kept behind the backend for the same reason the transcript
    // readers are: the analysis is a cache artifact belonging to the open
    // project, never document state.
    //
    // BENCH-2026-09 (B3) -- answers with ReadShotQualityCache's state rather
    // than a bool, so a cache written by an older build is refused as
    // unreadable instead of being listed as never analysed.
    virtual ShotQualityCacheRead ReadSourceShotQuality(const Ulid&,
                                                       ShotQualityReport&) {
        return {ShotQualityCacheState::Absent, 0,
                "this backend has no project shot quality cache"};
    }

    // R8 -- docs/roadmap/REEL-2026-09.md. The isolated-frame pass over the
    // source ranges the active timeline reads. One entry point for both
    // tools: `analyze` decodes and caches what no cache covers yet, and
    // without it the call only reports what is already measured. Behind the
    // backend for the same reason the shot quality pair is -- the scan is a
    // cache artifact of the open project, never document state -- and not
    // split in two because the ranges are a property of the edit, so a
    // reader and a writer that planned them separately could disagree.
    // `mediaIds`, when not empty, scans those media whole (the derush of a
    // shoot) instead of the ranges the timeline reads.
    virtual bool ScanIsolatedFrames(bool, const std::vector<Ulid>&,
                                    std::string&, std::string& message) {
        message = "this backend cannot scan for isolated frames";
        return false;
    }

    // F1.6 -- beat analysis is a derived cache artifact keyed by source ULID.
    // Reading or producing it never changes the canonical project document.
    virtual bool ReadSourceAudioBeats(const Ulid&, BeatDetectionResult&,
                                      std::string& message) {
        message = "this backend has no project audio beat cache";
        return false;
    }

    virtual bool AnalyzeSourceAudioBeats(const Ulid&, std::string&,
                                         std::string& message) {
        message = "this backend cannot run an audio beat analysis";
        return false;
    }

    // PA3 -- docs/roadmap/PARITE-2026-11.md. The same pair for a rush's
    // voices (SpeakerDiarization.h): the cache at
    // `.cutmachine/speakers/<media id>.json`, and the analysis that writes
    // it for the count of voices the caller gives.
    virtual bool ReadSourceSpeakers(const Ulid&, SpeakerDiarization&,
                                    std::string& message) {
        message = "this backend has no project speakers cache";
        return false;
    }

    virtual bool AnalyzeSourceSpeakers(const Ulid&, int32_t, std::string&,
                                       std::string& message) {
        message = "this backend cannot tell a rush's voices apart";
        return false;
    }

    // ONSET-2026-08 -- the same pair for the speech envelope. It exists
    // because the transcript is not a reliable answer to "where does this
    // clip start speaking": Whisper puts the first words of a segment on
    // silence, so an agent trusting them opens clips on dead air. Reading a
    // measurement it cannot produce would leave the same half-workflow
    // AnalyzeSourceShotQuality was added to close.
    virtual bool ReadSourceSpeechOnset(const Ulid&, SpeechOnsetReport&,
                                       std::string& message) {
        message = "this backend has no project speech onset cache";
        return false;
    }

    virtual bool AnalyzeSourceSpeechOnset(const Ulid&,
                                          const SpeechOnsetSettings&,
                                          std::string&, std::string& message) {
        message = "this backend cannot run a speech onset analysis";
        return false;
    }

    // SYNCHRO-2026-10 SY1 -- how far one rush's sound sits from another's.
    // Read only; a refusal keeps the measure's name in `errorName`.
    virtual bool MeasureAudioOffset(const Ulid&, const Ulid&,
                                    const AudioOffsetChannels&, std::string&,
                                    std::string& errorName,
                                    std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot measure an audio offset";
        return false;
    }

    // FC6 -- FILMCRAFT-2026-10. Where one rush's voice speaks and the
    // ducking a music under it would follow. Read only; nothing is written.
    virtual bool ProposeDucking(const Ulid&, AudioOffsetChannel,
                                const audio_dsp::DuckingSettings&, std::string&,
                                std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot propose a ducking";
        return false;
    }

    // QC-2026-09 (A1) -- pulls every cached transcript's word boundaries onto
    // the speech envelope, and optionally writes the corrections back. Not a
    // per-source call like the pair above: the point of the pass is that the
    // agent stops choosing which rush to doubt, because it cannot tell from
    // the words alone. Behind the backend because both artifacts are project
    // cache files, and the write installs one.
    virtual bool AlignSourceTranscripts(bool, std::string&,
                                        std::string& message) {
        message = "this backend has no project transcript cache";
        return false;
    }

    // AX2 -- docs/roadmap/AX-2026-09.md. MFA's word boundaries for these
    // rushes (every rush Whisper has read when empty), `textsJson` an
    // optional {media id: text} object (AlignWordsCommand). Minutes of work:
    // a server runs it as a background task.
    virtual bool AlignSourceWords(const std::vector<Ulid>&, const std::string&,
                                  std::string&, std::string& message) {
        message = "this backend has no project to align words in";
        return false;
    }

    // AFTER-COMP -- external host automation is an output-side effect, not a
    // Document mutation. Keeping it behind the backend prevents McpTools from
    // acquiring process-launch or Apple Events knowledge, just as media cache
    // and render effects already stay behind this seam.
    virtual bool RunAfterRequest(const std::string&, std::string&,
                                 std::string& errorName, std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend cannot automate After Effects";
        return false;
    }

    virtual bool Undo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;
    virtual bool Redo(std::string& resultJson, std::string& errorName,
                      std::string& message) = 0;

    // PL1 -- ROADMAP.md. An agent's intervention as one undo step. Begin
    // answers with a token marking where the selected timeline's history
    // stands (InterventionMark, EditLog.h) and writes nothing; End folds
    // what that history gained since into one labelled entry
    // (EditLog::GroupSince). The answers are InterventionBeganJson and
    // InterventionEndedJson on every backend.
    virtual bool BeginIntervention(std::string&, std::string& errorName,
                                   std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no edit history to group";
        return false;
    }
    virtual bool EndIntervention(const std::string&, const std::string&,
                                 std::string&, std::string& errorName,
                                 std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no edit history to group";
        return false;
    }

    // PRD35 -- ROADMAP.md. The selected timeline's journal as describe's
    // `history` lists it (SummarizeHistory, EditLog.h): read, never written.
    virtual bool ReadTimelineHistory(TimelineHistory&, std::string& errorName,
                                     std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no edit history to read";
        return false;
    }

    // Same JSON shape as `--describe`.
    virtual bool Describe(std::string& json, std::string& message) = 0;

    // AX5 -- only a backend behind an editor window has a view to share.
    virtual bool ReadEditorView(EditorView&, std::string& message) {
        message =
            "this backend has no editor window: the project is edited "
            "headless; open it in CUTMACHINE, and open_project again, to "
            "see what the editor is looking at";
        return false;
    }

    // CF1 -- local display state, outside the document and its edit log.
    // An absent screen ID reads; an empty one disables the clean feed.
    virtual bool AccessVideoOutput(const std::optional<std::string>&,
                                   VideoOutputState&, std::string& errorName,
                                   std::string& message) {
        errorName = "UnsupportedOperation";
        message =
            "video output needs the CUTMACHINE editor window; open "
            "the project in the application and open_project again";
        return false;
    }

    // VQA5 -- likewise. `errorName` tells a backend without a window
    // (UnsupportedOperation) from a window that could not be drawn.
    virtual bool CaptureEditorWindow(const EditorCaptureRequest&,
                                     EditorCapture&, std::string& errorName,
                                     std::string& message) {
        errorName = "UnsupportedOperation";
        message =
            "this backend has no editor window: the project is edited "
            "headless; open it in CUTMACHINE, and open_project again, to "
            "see the window";
        return false;
    }

    // IA5 -- ROADMAP.md. The state a caller names in `expected_revision` and
    // every server answer reports as `revision`: CanonicalHash of the whole
    // project, the value project edits already return as `project_hash`, or
    // of the document for a backend that holds no project. Defined in
    // McpSession.cc.
    virtual bool Revision(std::string& revision, std::string& message);

    // IA5 -- a second backend on the same open project, for one background
    // task: it runs on the task's worker thread, never on the caller's, so it
    // shares no mutable state with this one, and it runs its media work
    // through `runner` so the task can cancel it. It never edits the project:
    // a task only installs analysis caches. Only a server session calls it.
    virtual std::unique_ptr<McpBackend> ForkForTask(MediaTaskRunner,
                                                    std::string& message) {
        message = "this backend cannot run a tool in the background";
        return nullptr;
    }

    // PRD16 -- ROADMAP.md. A background export works on the project as it
    // stood when it was asked for, while the montage goes on: the session
    // snapshots the project on the calling thread and pins it on the task's
    // fork, whose every read then answers from that copy -- never from a
    // package an edit is rewriting meanwhile -- until it is unpinned
    // (nullptr) for the session to tell whether the montage moved since
    // (`stale`). Only a fork is asked. One that cannot hold a project says
    // so, and the export is refused in the background.
    virtual bool PinTaskProject(const Project*, std::string& message) {
        message = "this backend cannot hold a project for a background export";
        return false;
    }

    // PRD16 -- the runner of the background task this backend was forked for
    // (ForkForTask); empty for every other backend. A tool that writes its
    // file with code it calls itself rather than through the backend
    // (render_motion_video) hands it on, so that its progress is the task's
    // and cancel_task reaches it.
    virtual MediaTaskRunner BackgroundTaskRunner() const { return {}; }

    // PRD16 -- ROADMAP.md. The folder that holds the open project's package
    // (the parent of X.cutmachine-project): where a running server resolves
    // a relative path its caller names for a file (McpSession.h), since an
    // agent cannot know the server's working directory. Empty while no
    // project is open.
    virtual std::string ProjectFolder() const { return {}; }

    // MOTION-2026-09 M3 -- where a composition's relative image sources
    // resolve when render_motion_frame draws it: the open project's package,
    // as for its media. Empty where the backend knows no directory; they
    // then resolve against the process's own.
    virtual std::string MotionMediaDirectory() const { return {}; }

    // PA9 -- docs/roadmap/PARITE-2026-11.md. What the selected timeline
    // became since the state `sinceHash` names (ReadTimelineChanges,
    // EditLog.h), as list_changes answers it: read, never written.
    virtual bool ReadTimelineChangesJson(const std::string&, std::string&,
                                         std::string& errorName,
                                         std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no edit history to read";
        return false;
    }

    // MOTIF4 -- docs/roadmap/MOTIF-2026-10.md. The selected timeline's
    // journal as list_motifs reads it (ListMotifsJson, EditMotif.h): read,
    // never written.
    virtual bool ReadTimelineMotifs(const std::optional<MotifReason>&,
                                    const std::string&, std::string&,
                                    std::string& errorName,
                                    std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no edit history to read";
        return false;
    }
    // MOTIF6 -- the project's journal as list_motifs reads it with
    // `journal: "project"` (ListProjectMotifsJson): read, never written.
    virtual bool ReadProjectMotifs(const std::optional<MotifReason>&,
                                   const std::string&, std::string&,
                                   std::string& errorName,
                                   std::string& message) {
        errorName = "UnsupportedOperation";
        message = "this backend keeps no project journal to read";
        return false;
    }
    // GENERATIF-2026-10 GN3 -- docs/roadmap/GENERATIF-2026-10.md. The open
    // project's package directory (the folder of project.cutmachine.json),
    // absolute: where a generated file is written, so that the project
    // carries it and replays without the service, and what a library
    // media's relative path is relative to. Empty while no project is open,
    // and generate_media then refuses.
    virtual std::string PackageDirectory() const { return {}; }

protected:
    // The entry motifs of an operation this backend is about to journal.
    std::vector<EntryMotif> EntryMotifsForCall(const Operation& operation) {
        const EditMotifs current = CurrentEditMotifs();
        return EntryMotifsFor(operation, current.members, current.motif);
    }
    // MOTIF6 -- the entry motifs of a project edit this backend is about to
    // journal: the call's motif, a project entry being one member.
    std::vector<EntryMotif> ProjectEntryMotifsForCall() {
        return ProjectEntryMotifsFor(CurrentEditMotifs().motif);
    }

private:
    EditMotifs edit_motifs_;
};
