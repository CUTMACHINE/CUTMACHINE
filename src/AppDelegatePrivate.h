#pragma once

// PRD8 -- docs/roadmap/PRODUIT-2026-10.md. What main.mm shares with its
// window test harness, UiSmokeHarness.mm, which only
// cutmachine_ui_smoke_tests compiles: the includes, the window's state
// (AppState and the types it holds), the interfaces of the views the
// harness drives, and AppDelegate's with the private methods it calls.
// Each piece was in main.mm, which includes this header first; nothing
// else includes it. Declared here, defined in main.mm.

#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#if defined(CUTMACHINE_UI_SMOKE_TEST)
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <objc/runtime.h>
// PRD25 -- the run speaks to the window's endpoint as an agent does.
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach.h>
#include <os/signpost.h>
#include <iostream>

extern "C" {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
// ScreenCaptureKit imports AVFoundation, whose Objective-C AVMediaType typedef
// collides with FFmpeg's C enum tag. This translation unit does not name that
// enum; changing only its local tag leaves every C symbol and ABI untouched.
#define AVMediaType CutmachineFfmpegAVMediaType
#endif
#include <libavutil/frame.h>
#include <libavutil/parseutils.h>
#if defined(CUTMACHINE_UI_SMOKE_TEST)
#undef AVMediaType
#endif
}

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AfterBridge.h"
#include "AgentJournal.h"
#include "AgentWindow.h"
#include "AudioPlayback.h"
#include "BoundaryAir.h"
#include "BuildInfo.h"
#include "CanonicalHash.h"
#include "CaptionGeneration.h"
#include "CaptionOverlayLayout.h"
#include "CaptionRenderer.h"
#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipBadges.h"
#include "ClipEnabled.h"
#include "ClipFraming.h"
#include "ClipGradeCopy.h"
#include "ColorEffects.h"
#include "DecodeWorker.h"
#include "DecoderPool.h"
#include "Document.h"
#include "EditLog.h"
#include "EngineLog.h"
#include "Export.h"
#include "FaceFinder.h"
#include "FrameCache.h"
#include "FrameCapture.h"
#include "GradeCurveModel.h"
#include "GradeWheelModel.h"
#include "Ingest.h"
#include "InspectorGradeControls.h"
#include "InspectorKeys.h"
#include "InterviewShort.h"
#include "Json.h"
#include "KeyboardShortcuts.h"
#include "LookLutStorage.h"
#include "McpEndpoint.h"
#include "McpLiveBackend.h"
#include "McpProjectBackend.h"
#include "McpServer.h"
#include "McpTools.h"
#include "MediaPanelModel.h"
#include "MediaTaskManager.h"
#include "MonitorOcclusion.h"
#include "MotionCanvas.h"
#include "MotionCli.h"
#include "MotionClip.h"
#include "MotionClipPreview.h"
#include "MotionClipPreviewRender.h"
#include "MotionDocument.h"
#include "MotionOutline.h"
#include "MotionRender.h"
#include "MotionTemplates.h"
#include "Multicam.h"
#include "MulticamCli.h"
#include "PanelLayout.h"
#include "PauseTightening.h"
#include "PerformanceMetrics.h"
#include "PersonMatte.h"
#include "PlaybackPresentation.h"
#include "PlaybackReader.h"
#include "PlaybackReads.h"
#include "Project.h"
#include "ProjectArchive.h"
#include "ProjectRecovery.h"
#include "ProjectStorage.h"
#include "Proxy.h"
#include "Relink.h"
#include "RenderCostMeter.h"
#include "Renderer.h"
#include "ResolveExport.h"
#include "ResolveImport.h"
#include "ResolveXmlExport.h"
#include "RunStepStorage.h"
#include "SeparateSoundGesture.h"
#include "SetupCheck.h"
#include "SetupNotice.h"
#include "SmokeReport.h"
#include "Subtitles.h"
#include "Thumbnail.h"
#include "Timecode.h"
#include "Timeline.h"
#include "TimelineSheets.h"
#include "TimelineView.h"
#include "Transcription.h"
#include "TransitionRender.h"
#include "TransportBar.h"
#include "TrimSession.h"
#include "UiAccessibility.h"
#include "UiPreferences.h"
#include "UiTheme.h"
#include "Waveform.h"
#include "WhisperModelSetting.h"
#include "WindowCapture.h"
#include "WordAlignment.h"

// F2.1 -- ROADMAP.md design system: reusable AppKit chrome and the fixed
// right-hand panel host that F2.2 (Inspector) and F2.4 (Chat) install their
// content into (see PanelHostView.h). ChatPanelView.h is F2.4's actual
// content view -- the chat transcript + input this file installs into the
// dock's Chat slot below. Objective-C++ declarations, so #import rather
// than #include, matching AppKit/Foundation above.
#import "ChatPanelView.h"
#include "EffectsLibrary.h"
#import "EffectsPanelView.h"
#import "FormPanel.h"
#import "GradeCurveView.h"
#import "GradeHueCurveView.h"
#import "GradeWheelView.h"
#import "InspectorView.h"
#include "MainThreadBackend.h"
#import "MotionCanvasView.h"
#import "MotionInspectorView.h"
#import "PanelHostView.h"
#import "TrimBarView.h"
#import "TrimCursors.h"
#import "UiComponents.h"
#import "UiThemeAppKit.h"
#import "WorkspaceBar.h"

namespace cutmachine_app {

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The fifth tool was Slip; it is
// Resolve's Trim Edit mode now (TimelineView.h, TimelinePointerTool).
enum class TimelineTool { Select, Hand, Zoom, Cut, Trim };

enum class HistoryDomain { Timeline, Project };

// REMOTION-PARITY-2026-09 REM5 -- the visual editor of the motion clip
// the Record monitor shows at the playhead: which clip and composition,
// the frame of it evaluated there (what a click selects in, MotionCanvas.h),
// where its pixels sit in the monitor, the node selected and hovered, and
// the gesture in progress. The gesture's preview stays here; only its end
// commits an operation.
struct MotionCanvasState {
    Ulid clip_id;
    Ulid composition_id;
    int64_t composition_frame = 0;
    bool has_frame = false;
    MotionFrame frame;
    MotionMonitorMapping mapping;
    std::string selected;
    std::string hovered;
    std::vector<MotionField> fields;
    // MA6 -- the composition's layers as the Inspector lists them, built
    // again only when the composition or its props change (one evaluation
    // per scene): `outline_key` is what they were built from.
    std::string outline_key;
    std::vector<MotionOutlineRow> outline;
    // 0: none, 1: moving, 2: resizing by `handle`.
    int gesture = 0;
    int handle = -1;
    double start_x = 0.0;
    double start_y = 0.0;
    double delta_x = 0.0;
    double delta_y = 0.0;
};

struct ResolvedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
    float opacity = 1.0f;
    // The DocumentClip currently occupying this slot. Used only to look up
    // that clip's color.* effects stack for F1.3 grading; see
    // presentNearestFrameAtDeadline.
    Ulid clipId;
};

struct RenderedSlot {
    bool active = false;
    Ulid sourceId;
    int64_t frame = -1;
    float opacity = 1.0f;
    Ulid clipId;

    bool operator==(const RenderedSlot& other) const {
        return active == other.active && sourceId == other.sourceId &&
               frame == other.frame && opacity == other.opacity &&
               clipId == other.clipId;
    }
};

// PRD7 -- docs/roadmap/PRODUIT-2026-10.md. What the background task read of
// each file, as the engine's ingest leaves it (ProbeIngestFile, Ingest.h):
// the window no longer has a probe result of its own.
struct ProbedImportBatch {
    std::filesystem::path document_path;
    std::string target_bin;
    std::vector<ProbedIngestFile> files;
};

struct PendingProxy {
    Ulid media_id;
    std::filesystem::path absolute_path;
    std::string stored_path;
};

struct PendingWaveform {
    Ulid media_id;
    std::filesystem::path absolute_path;
};

struct PendingThumbnail {
    Ulid media_id;
    std::filesystem::path absolute_path;
};

struct PendingTranscription {
    Ulid media_id;
    Ulid timeline_id;
    Ulid batch_id;
    std::filesystem::path absolute_path;
    // AL4 -- « Transcrire les hésitations (français)… »: a failure is shown
    // as the refusal it is.
    bool hesitations = false;
};

struct PendingTimelineTranscription {
    Ulid timeline_id;
    std::set<Ulid> task_ids;
    std::map<Ulid, std::filesystem::path> transcript_paths;
    std::string error;
    bool hesitations = false;
};

struct RelinkProbeResult {
    LibraryMedia media;
    std::string error;
};

struct PendingRelink {
    Ulid media_id;
    std::filesystem::path absolute_path;
    std::string stored_path;
    std::shared_ptr<RelinkProbeResult> result;
};

struct BatchRelinkResult {
    std::vector<RelinkReplacement> replacements;
    size_t unmatched = 0;
    size_t ambiguous = 0;
    size_t incompatible = 0;
    std::string last_error;
};

struct PendingBatchRelink {
    std::shared_ptr<BatchRelinkResult> result;
};

// PERF-2026-09. Opening a project probes every rush and then opens a decoder
// for every rush: two avformat_open_input plus avformat_find_stream_info per
// source, one after the other, on the thread AppKit draws from. They are
// independent per source and spend most of their time waiting on the drive,
// which is why an interview project's launch was seconds of frozen window on
// an external volume. Nothing here shares FFmpeg state between sources --
// each iteration opens, reads and closes its own context, exactly as the
// media task workers already do concurrently for thumbnails and waveforms.
//
// The body writes only into its own slot; every mutation of the app's state
// stays on the calling thread, in source order, in the pass that follows.
// The cap is there because past a handful of concurrent reads the drive, not
// the CPU, is the queue.
struct PendingDecodeOpen {
    Ulid source_id;
    std::string selected;
    std::string original;
    bool using_proxy = false;
};

struct TimelineClipboardClip {
    DocumentClip clip;
    Ulid track_id;
    std::string track_kind;
    size_t kind_ordinal = 0;
};

// Out of lot -- the author, 29 October 2026: "there is a delay when moving
// clips, it needs an optimistic update". A sample of the window showed each
// drop spend ~240 ms of the main thread saving (autosave ~130 ms, package ~45,
// archive ~30, archive status ~20) against 1 ms for the edit itself. A
// gesture's save is now a job run on the window's commit queue; this is
// everything it needs, copied, so the window keeps editing while it runs.
struct ProjectCommitJob {
    std::string path;
    Project project;
    std::map<std::string, EditLog> timeline_logs;
    ProjectEditLog project_log;
};

struct AppState {
    std::unique_ptr<ProjectSessionLock> projectLock;
    // The latest gesture save the commit queue has not taken yet: a burst of
    // gestures writes only its last state, which holds all of them.
    std::mutex pendingCommitMutex;
    std::shared_ptr<ProjectCommitJob> pendingCommit;
    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). What an agent relayed
    // to this window did (AgentJournal.h): listed in the Agent panel, its
    // last call's clips marked on the timeline. In memory, gone with the
    // window.
    AgentJournal agentJournal;
    Project project;
    ProjectEditLog projectEditLog;
    Ulid activeTimelineId;
    HistoryDomain lastHistoryDomain = HistoryDomain::Timeline;
    Document document;
    EditLog editLog;
    std::map<Ulid, EditLog> timelineEditLogs;
    std::map<Ulid, RationalTime> timelinePositions;
    std::map<Ulid, std::set<Ulid>> timelineTargetTracks;
    std::set<Ulid> targetedTrackIds;
    TimelineViewport viewport;
    std::unique_ptr<TimelineInteraction> interaction;
    std::unique_ptr<Timeline> timeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<Renderer> sourceRenderer;
    std::unique_ptr<Renderer> programRenderer;
    // CF1 -- another surface of the same decoded frames, no extra reader.
    std::unique_ptr<Renderer> videoOutputRenderer;
    bool videoOutputNeedsPresent = false;
    std::unique_ptr<FrameCache> frameCache;
    // MOTION-2026-09 M4 -- rendered motion frames, as the monitor uploads
    // them (MotionClipPreview.h).
    std::unique_ptr<MotionPreviewCache> motionPreviews =
        std::make_unique<MotionPreviewCache>(kMotionPreviewCacheBytes);
    // Visual QA of 19 September 2026 -- a motion frame that failed to render
    // (a missing font) was retried on every display tick, and each attempt
    // wrote the same line: 9 812 of them in one session. A failed frame is
    // not retried, and its composition reported once. The preview key
    // changes with the composition or its props, which retries.
    std::set<std::pair<std::string, int64_t>> failedMotionFrames;
    std::set<std::string> reportedMotionKeys;
    // REM5 -- the visual editor of motion (MotionCanvasState above).
    MotionCanvasState motionCanvas;
    // R6 -- docs/roadmap/REEL-2026-09.md. The sequence's look LUT, read
    // once from the project package and kept until the document names
    // another digest. `reportedLookLut` holds the digests whose read
    // already failed, so a missing or altered .cube is reported once and
    // not on every display tick (the same lesson as the motion frames
    // above).
    LoadedLookLut lookLut;
    std::set<std::string> reportedLookLuts;
    // PA15 -- docs/roadmap/PARITE-2026-11.md. The mattes of the media whose
    // clips are cut out along their people, open for Record to read the
    // frame each slot shows (PersonMatte.h); reopened when the file is
    // computed again. `matteFrames` holds each slot's frame for the render
    // that reads it. `reportedMattes` keeps a missing matte said once.
    struct OpenPersonMatte {
        std::string path;
        std::filesystem::file_time_type written;
        std::unique_ptr<person_matte::Reader> reader;
    };
    std::map<Ulid, OpenPersonMatte> personMattes;
    std::vector<std::vector<uint8_t>> matteFrames;
    std::set<std::string> reportedMattes;
    std::unique_ptr<PerformanceMetrics> performanceMetrics;
    std::unique_ptr<AudioPlayback> audioPlayback;
    // Out of lot -- the author, 29 October 2026: the source monitor plays,
    // picture and sound. Its direction (J/K/L's -4..4, 0 stopped), where and
    // when it started (AdvanceSourcePlayback, TransportBar.h), and a player of
    // its own that holds the shown rush alone, so the timeline's mix is never
    // rebuilt for it.
    int sourcePlaybackDirection = 0;
    RationalTime sourcePlaybackAnchor;
    std::chrono::steady_clock::time_point sourcePlaybackStarted;
    std::unique_ptr<AudioPlayback> sourceAudioPlayback;
    Ulid sourceAudioMediaId;
    std::unique_ptr<MediaTaskManager> mediaTasks =
        std::make_unique<MediaTaskManager>(2);
    std::map<Ulid, std::shared_ptr<ProbedImportBatch>> pendingImports;
    std::map<Ulid, PendingProxy> pendingProxies;
    std::map<Ulid, PendingWaveform> pendingWaveforms;
    std::map<Ulid, PendingThumbnail> pendingThumbnails;
    std::map<Ulid, PendingTranscription> pendingTranscriptions;
    std::map<Ulid, PendingTimelineTranscription> pendingTimelineTranscriptions;
    std::map<Ulid, PendingRelink> pendingRelinks;
    std::map<Ulid, PendingBatchRelink> pendingBatchRelinks;
    std::map<Ulid, AudioWaveform> waveforms;
    bool proxiesEnabled = true;
    bool automaticProxiesEnabled = false;
    std::map<Ulid, std::unique_ptr<DecodeWorker>> workers;
    // PERF10 -- DecoderPool.h. Decoders are opened when the monitor needs
    // their source, in the background, and at most decoderCapacity stay
    // open. These are the opens in flight, and the workers being torn down
    // off the main thread: a source is not reopened until its old worker is
    // gone, since ~DecodeWorker unregisters the source from the frame cache
    // and would otherwise cancel the new worker's registration.
    std::set<Ulid> openingDecoders;
    std::set<Ulid> closingDecoders;
    decoder_pool::Usage decoderUsage;
    size_t decoderCapacity = decoder_pool::kDefaultCapacity;
    // The sources the next second of playback crosses into, as last warmed:
    // needed, so never the decoder the pool closes.
    std::set<Ulid> upcomingSourceIds;
    // PERF14 -- the sources playback asked for at the last position: on
    // screen, the next shot, a cutaway's end. FrameCache shares its window
    // among them (FrameCache::SetReadSources).
    std::set<Ulid> readSourceIds;
    // PERF15 -- PlaybackReader.h. In forward playback, the one reader of the
    // bytes of the pictures about to be shown; the decoders then find them
    // in the system's cache.
    playback_reader::Reader playbackReader;
    // PERF12 -- DecoderPool.h, PlanOpens. What playback shows next, as last
    // planned, held like what is on screen; when each open in flight
    // started, so that one that hangs stops holding the slot; and whether
    // the project's media are known to be there, before which nothing is
    // opened ahead: a missing file is found by a stat, not by an open.
    std::set<Ulid> plannedSourceIds;
    std::map<Ulid, std::chrono::steady_clock::time_point> openingSince;
    bool sourceAvailabilityKnown = false;
    // True from the moment the project-open availability check is
    // dispatched until its results are installed; a reload in between
    // bumps the generation, and the check's stale results are dropped.
    bool checkingSourceAvailability = false;
    uint64_t sourceAvailabilityGeneration = 0;
    // Asked for while their previous worker was still closing.
    std::set<Ulid> reopenAfterClose;
    // Sources remain part of the edit even when their files are unavailable.
    // Keeping this separate from the document makes reconnecting media a
    // runtime concern rather than a destructive edit.
    std::set<Ulid> offlineSourceIds;
    // Runtime probe cache. UI metadata never mutates the edit document.
    std::map<Ulid, LibraryMedia> mediaMetadata;
    std::vector<Ulid> videoTrackIds;
    std::vector<ResolvedSlot> requested;
    // PERF13 -- MonitorOcclusion.h. In playback, the tracks an opaque shot
    // above hides at the position last requested, and their media layers:
    // neither composited nor asked for, yet on screen, so their decoders
    // are kept (-trimDecoderPool) and asked ahead for the frame they are
    // shown again at (-warmUpcomingDecoders:).
    std::set<Ulid> hiddenTrackIds;
    std::vector<ResolvedSlot> hiddenSlots;
    std::vector<RenderedSlot> rendered;
    // PERF-2026-09. Whether the Record monitor holds a previously composited
    // image, and for how many ticks it has been holding it. Runtime playback
    // state, never persisted; the policy lives in PlaybackPresentation.h.
    bool programPresented = false;
    int programHeldTicks = 0;
    RenderedSlot sourceRendered;
    RationalTime duration{0, 1};
    RationalTime requestedPosition{0, 1};
    std::optional<RationalTime> timelineIn;
    std::optional<RationalTime> timelineOut;
    PlayheadResolution playheadResolution = PlayheadResolution::Frame;
    bool overlayDirty = true;
    // Whether the timeline has been scaled to a real, laid-out viewport
    // width yet. Not made until then, so the first resize that brings a
    // usable width does the fit the launch path could not. PRD37 -- and
    // what that fit gave: until a zoom or a scroll moves it, a resize fits
    // again (TimelineView.h, TimelineFitFollowsResize).
    TimelineFit timelineFit;
    // PRD37 -- the view each timeline was left with, for this session.
    std::map<Ulid, TimelineViewMemory> timelineViews;
    TimelineTool tool = TimelineTool::Select;
    bool spaceHand = false;
    bool navigationDragging = false;
    std::optional<TimelineZoomBarDrag> zoomBarDrag;
    bool scrubDragging = false;
    bool editDragging = false;
    bool duplicateDragging = false;
    bool lassoCandidate = false;
    bool lassoDragging = false;
    // Visual QA of 19 September 2026 -- where the playhead was before the
    // press that may start a lasso: the press moves it, a lasso puts it back.
    RationalTime lassoPlayhead{0, 1};
    bool linkedSelection = true;
    bool linkedSelectionGesture = true;
    std::vector<TimelineClipboardClip> timelineClipboard;
    // GR11, DEC30 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The stack
    // Couleur › Copier l'étalonnage took, held until it is pasted:
    // transient window state, never the document's.
    std::optional<std::vector<ClipEffect>> gradeClipboard;
    std::vector<TimelineClipboardClip> duplicateDragClipboard;
    double lassoStartX = 0.0;
    double lassoStartY = 0.0;
    double lassoCurrentX = 0.0;
    double lassoCurrentY = 0.0;
    bool spaceUsedForPan = false;
    double navigationLastX = 0.0;
    int playbackDirection = 0;
    RationalTime playbackAnchor{0, 1};
    std::chrono::steady_clock::time_point playbackStarted;
    std::optional<double> cutPreviewX;
    std::optional<double> cutPreviewY;
    int hoveredTimelineTool = -1;
    Ulid hoveredTrackId;
    int hoveredTrackControl = -1;
    // D2 -- docs/roadmap/DIALOGUE-2026-10.md. The marker whose flag the
    // pointer is on: its note is shown under the ruler.
    Ulid hoveredMarkerId;
    // Visual QA of 1 October 2026 -- what the last zoom at the pointer held
    // still, kept while the pointer stays put (TimelineView.h).
    std::optional<TimelineZoomAnchor> zoomAnchor;
    // D6 -- docs/roadmap/DIALOGUE-2026-10.md. A marker being stretched into
    // a span with Option held: the end that stays, and the operation the
    // pointer has reached so far. Nothing is in the document until the
    // button is released.
    struct MarkerStretch {
        Ulid marker_id;
        RationalTime anchor;
        std::optional<UpdateMarkerOperation> reached;
    };
    std::optional<MarkerStretch> markerStretch;
    // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode: the edit
    // points selected, their side, the J/K/L dynamic trim (TrimSession.h).
    // Interface state, like the clip selection: never in the document.
    trim::TrimSession trimSession;
    // W -- Resolve's Dynamic Trim mode: with points selected, J, K and L
    // trim them instead of playing.
    bool dynamicTrimMode = false;
    // What the timeline draws while J or L runs: the document with the
    // offset reached so far (TrimSession::PreviewDocument). Nothing is in
    // the document itself until K.
    std::optional<Document> trimPreview;
    // The target the last press on the timeline took, and whether Shift
    // was held: a click on an edge that moved nothing selects its point.
    std::optional<TimelinePointerTarget> pressTarget;
    bool pressShift = false;
    // FC4 -- the Trim Monitor, while the trim mode holds points and nothing
    // plays: what it shows (DescribeTrimMonitor) and, parallel to
    // `requested`, the pane of the sequence frame each of its two frames is
    // drawn in (LayoutTrimMonitor). Empty: Record shows the program.
    std::vector<trim::TrimMonitorPane> trimMonitorPanes;
    trim::TrimMonitorView trimMonitorView;
    // The two panes by side, A then B, an empty one for a side with no
    // picture: where the counters are written.
    trim::TrimMonitorLayout trimMonitorLayout;
    // FC3 -- the clips whose Retime controls are shown (Command-R), whose
    // ends stretch them (TimelineView.h, VisibleTimelineRetimeBands).
    // Interface state, of this timeline only.
    std::vector<Ulid> retimeClipIds;
    bool sourceMonitor = false;
    Ulid sourceMonitorId;
    RationalTime sourceMonitorPosition{0, 1};
    std::optional<RationalTime> sourceIn;
    std::optional<RationalTime> sourceOut;
    bool sourceMonitorActive = false;
    double sourceMonitorZoom = 0.0;
    double programMonitorZoom = 0.0;
    // Visual QA of 19 September 2026 -- the grade an Inspector slider shows
    // while it is dragged, drawn in place of the clip's own until the commit
    // or the next Inspector reload. Never written to the document.
    std::optional<ui::inspector::GradePreview> gradePreview;
    // The zoom or position an Inspector slider is dragging, shown on Record
    // until its release commits it (InspectorGradeControls.h).
    std::optional<ui::inspector::FramingPreview> framingPreview;
    // EF1 -- the cut an effect being dragged would land on, drawn on the
    // timeline until the drop or the drag's exit.
    std::optional<ui::effects::DropTarget> effectDropTarget;
    // D7 -- the same for the words of a carton while the Inspector's field
    // is typed in (MediaPanelModel.h, DisplayedCartonText).
    std::optional<ui::media_panel::CartonTextPreview> cartonPreview;
    VideoScopeMode programVideoScope = VideoScopeMode::Off;
    // G6 -- docs/roadmap/GRADE-2026-09.md. The Record monitor's comparison
    // (MonitorCompare.h), its reference still, and a grab the next Record
    // render makes. Local preferences, never the document.
    MonitorCompare monitorCompare;
    std::shared_ptr<const ReferenceStill> referenceStill;
    bool grabReferenceRequested = false;
    bool sourceMonitorVisible = true;
    double sourceMonitorPanelWidth = 0.0;
    Ulid contextClipId;
    Ulid contextTrackId;
    RationalTime contextTime{0, 1};
    std::optional<TimelineGapSelection> contextGap;
    // SYNCHRO-2026-10 SY2 -- an Auto Align is measuring the two sounds off
    // the main thread: a second one waits for it.
    bool soundAlignmentRunning = false;
    std::atomic_bool exportCancel{false};
    bool exportRunning = false;
};

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. The setup report and
// the two settings only the window keeps (SetupNotice.h), gathered together
// off the main thread: the report probes FFmpeg, may read the Keychain, and
// every file it looks at may sit on a slow disk.
struct SetupNoticeResult {
    setup::Report report;
    setup_notice::WindowFacts facts;
};

// Defined in main.mm; the harness calls them too.
NSString* CMStringFromUtf8(const std::string& text);
NSString* ShortcutSignature(NSString* spec, NSString** error);
bool DecodeWorkerMatchesSource(const DecodeWorker& worker,
                               const DocumentSource& source);
std::optional<PendingDecodeOpen> DecodeOpenFor(
    const Document& document, const DocumentSource& source,
    const std::filesystem::path& base, bool proxiesEnabled);
std::string TimelineDrawnClipName(const Document& document,
                                  const Project& project,
                                  const DocumentClip* clip,
                                  const Ulid& sourceId);
NSString* TimelineTimecode(const RationalTime& time, MediaRate rate);
extern NSPasteboardType const kCutmachineMediaPasteboardType;
extern NSPasteboardType const kCutmachineBinPasteboardType;
extern NSPasteboardType const kCutmachineTimelinePasteboardType;
extern NSString* const kProgramVideoScopeDefaultsKey;
extern int64_t gRenderCostOriginNs;
// A media drag can copy into the timeline or move into a bin. AppKit
// intersects this source mask with the destination's requested operation;
// advertising Copy alone makes every bin drop resolve to None.
constexpr NSDragOperation kMediaLocalDragOperations =
    NSDragOperationCopy | NSDragOperationMove;

#if defined(CUTMACHINE_UI_SMOKE_TEST)
// Defined in UiSmokeHarness.mm; main.mm's test hooks read or set them.
extern bool gUiSmokeTesting;
extern int gUiSmokeFailures;
extern bool gUiSmokeIconMouseDown;
extern bool gUiSmokeIconDragSession;
extern bool gUiSmokeIconDragEnded;
extern NSUInteger gUiSmokeMediaDragCount;
extern int gUiSmokeDecodeReloads;
extern int gUiSmokeDecodeOpens;
extern size_t gUiSmokeWorkersAtWindowShow;
extern std::string gUiPlayProbeProject;
extern double gUiPlayProbeSeconds;
extern int gUiPlayProbeOpens;
extern int gUiPlayProbeReopens;
extern std::set<Ulid> gUiPlayProbeOpened;
extern int gUiPlayProbeTicks;
extern int gUiPlayProbeTopDrops;
extern bool gUiPlayProbeHide;
extern bool gUiPlayProbeReadAhead;
extern int64_t gUiPlayProbeByteSeconds;
extern int64_t gUiPlayProbeReappearSeconds;
extern int64_t gUiPlayProbeCutSeconds;
extern bool gUiPlayProbeReader;
extern bool gUiPlayProbeNextPiece;
extern bool gUiSmokeClipSignageOff;
extern std::string gUiSmokeAgentJournalTime;
extern int gUiSmokeAgentEvents;
extern std::filesystem::path gUiSmokeRoot;
extern std::string gUiSmokeProjectPath;
extern std::filesystem::path gUiSmokeSaveAnswer;
extern ui_smoke::NotRun gUiSmokeNotRun;
bool PrepareUiSmokeProject(std::string& error);
#endif

}  // namespace cutmachine_app

using namespace cutmachine_app;

@class TimelineMetalView;
@class VideoFullscreenWindow;

@protocol TimelineEventTarget <NSObject>
- (void)timelineMouseDown:(NSEvent*)event;
- (void)timelineMouseDragged:(NSEvent*)event;
- (void)timelineMouseUp:(NSEvent*)event;
- (void)timelineMouseMoved:(NSEvent*)event;
- (void)timelineScroll:(NSEvent*)event;
- (void)timelineMagnify:(NSEvent*)event;
- (BOOL)timelineKeyDown:(NSEvent*)event;
- (void)timelineKeyUp:(NSEvent*)event;
- (BOOL)timelineDropMediaIds:(NSArray<NSString*>*)mediaIds
                 atViewPoint:(NSPoint)point;
// EF1 -- an effect from the « Effets » tab, over the timeline and dropped.
- (NSDragOperation)timelineDragEffectId:(NSString*)effectId
                            atViewPoint:(NSPoint)point;
- (void)timelineEffectDragEnded;
- (BOOL)timelineDropEffectId:(NSString*)effectId atViewPoint:(NSPoint)point;
- (NSMenu*)timelineMenuForEvent:(NSEvent*)event;
@end

@protocol TimelineMetalViewResizeTarget <NSObject>
- (void)timelineMetalViewDidResize:(TimelineMetalView*)view;
@end

@interface TimelineMetalView : NSView
@property(nonatomic, weak) id<TimelineEventTarget> eventTarget;
@property(nonatomic, weak) id<TimelineMetalViewResizeTarget> resizeTarget;
// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. What VoiceOver reads as this
// view's value, asked each time it reads it: a value set once would be
// stale by the next frame. Nil: the view has none.
@property(nonatomic, copy) NSString* (^accessibilityValueProvider)(void);
// EF1 -- the timeline takes an effect dropped from the « Effets » tab; the
// monitors, the same class, do not.
@property(nonatomic) BOOL acceptsEffects;
@end

@interface CMIndustrialTableRowView : NSTableRowView
@end

@interface ContextCollectionView : NSCollectionView <NSDraggingSource>
@property(nonatomic, weak) id doubleClickTarget;
@property(nonatomic) SEL doubleClickAction;
@property(nonatomic, strong) NSIndexPath* forwardedMouseDownPath;
@property(nonatomic, strong) NSIndexPath* rangeSelectionAnchor;
@end

@interface BrowserRenameTextField : NSTextField
@end

@interface MediaIconView : NSView
@property(nonatomic, strong) NSIndexPath* indexPath;
@end

// LG5 -- docs/roadmap/LEGENDES-2026-10.md. The cartons over the Record
// monitor: the caption renderer's picture of the frame (CaptionRenderer.h),
// the one the export lays on the file, drawn at the size the frame is shown
// and laid over it. It hosts its own layer, whose contents are that
// picture; clicks go through it to the monitor.
@interface CMCaptionView : NSView
// The cartons' text as shown, one per line: what VoiceOver reads and what
// the window test compares.
@property(nonatomic, copy) NSString* captionText;
// The cartons, styles and size the picture was drawn from: the same key
// draws nothing again.
@property(nonatomic, copy) NSString* drawnKey;
// Where the picture is painted -- letters, outline, shadow, box -- as a
// share of the picture, origin top-left. The view covers the whole frame;
// this is the part of it a carton hides.
@property(nonatomic) CGRect inkShare;
@end

@interface AppDelegate : NSObject <NSApplicationDelegate,
                                   NSWindowDelegate,
                                   NSSplitViewDelegate,
                                   TimelineEventTarget,
                                   TimelineMetalViewResizeTarget,
                                   NSOutlineViewDataSource,
                                   NSOutlineViewDelegate,
                                   NSTableViewDataSource,
                                   NSTableViewDelegate,
                                   NSCollectionViewDataSource,
                                   NSCollectionViewDelegate,
                                   NSTextFieldDelegate,
                                   NSMenuItemValidation,
                                   CMInspectorViewDelegate,
                                   CMWorkspaceBarDelegate,
                                   CMTrimBarDelegate,
                                   CMMotionInspectorViewDelegate,
                                   CMEffectsPanelDelegate>
@property(nonatomic, strong) NSWindow* window;
@property(nonatomic, strong) NSWindow* startupWindow;
@property(nonatomic, strong) NSPopUpButton* recentProjectsPopup;
@property(nonatomic, assign) NSInteger startupChoice;
@property(nonatomic, copy) NSString* startupSelectedPath;
// PRD21 (window) -- the welcome screen's setup line (SetupNotice.h), hidden
// until the report comes back from its thread; `startupSetupNoticeChecked`
// says it has. The diagnosis dialog's text to copy, while it is open.
@property(nonatomic, strong) NSView* startupSetupNotice;
@property(nonatomic, strong) NSTextField* startupSetupNoticeLabel;
@property(nonatomic, assign) BOOL startupSetupNoticeChecked;
@property(nonatomic, assign) BOOL setupDiagnosticPending;
// PA16 -- a send to Resolve is running on its thread; a second one waits.
@property(nonatomic, assign) BOOL resolveSendPending;
@property(nonatomic, copy) NSString* setupDiagnosticClipboard;
@property(nonatomic, strong) TimelineMetalView* metalView;
@property(nonatomic, strong) TimelineMetalView* sourceMonitorView;
@property(nonatomic, strong) TimelineMetalView* programMonitorView;
@property(nonatomic, strong) NSView* sourceMonitorPanel;
@property(nonatomic, strong) NSView* programMonitorPanel;
@property(nonatomic, strong) VideoFullscreenWindow* videoFullscreenWindow;
@property(nonatomic, assign) BOOL videoFullscreenActive;
@property(nonatomic, strong) NSWindow* videoOutputWindow;
@property(nonatomic, strong) TimelineMetalView* videoOutputView;
@property(nonatomic, strong) CMCaptionView* videoOutputCaptionView;
@property(nonatomic, strong) NSMenu* videoOutputMenu;
@property(nonatomic, copy) NSString* videoOutputSelection;
@property(nonatomic, copy) NSString* videoOutputActiveScreen;
@property(nonatomic, strong) NSView* mediaPanel;
@property(nonatomic, strong) NSSplitView* workspaceSplitView;
@property(nonatomic, strong) NSSplitView* editorSplitView;
@property(nonatomic, strong) NSSplitView* monitorSplitView;
// ON5, ON7 -- page-local presentation; no project state is kept here.
@property(nonatomic, strong) NSScrollView* binSidebarScroll;
@property(nonatomic, strong) NSScrollView* workspaceActivityScroll;
@property(nonatomic, strong) NSTextView* workspaceActivityText;
@property(nonatomic) CGFloat workspaceMonitorHeight;
// F2.1 design system: fixed right-hand dock hosting the Inspector (F2.2) and
// Chat (F2.4) tabs. See PanelHostView.h -- the set of tabs and their order
// are fixed at construction time, never user-rearrangeable.
@property(nonatomic, strong) CMPanelHostView* rightDockPanel;
// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. The bar of pages at the
// bottom, the page shown and the panels open on it. `rememberWorkspace` is
// NO while the smoke run lays the window out its own way: a page chosen by
// the test is not written into the editor's preferences.
@property(nonatomic, strong) CMWorkspaceBarView* workspaceBar;
// ON8 -- the trim bar, in the timeline's tool row (TrimBarView.h).
@property(nonatomic, strong) CMTrimBarView* trimBarView;
// ON9 -- the playback « Lire autour » started: from `playAroundStart`, it
// stops at `playAroundEnd` and takes the playhead back to `playAroundReturn`
// -- or, with `loopPlayback`, starts again. Any other playback command ends
// it (-setPlaybackDirection:).
@property(nonatomic) BOOL playAroundActive;
@property(nonatomic) RationalTime playAroundStart;
@property(nonatomic) RationalTime playAroundEnd;
@property(nonatomic) RationalTime playAroundReturn;
@property(nonatomic) BOOL loopPlayback;
// ON9 -- a drag on the Trim Monitor: the side its press took, where it
// began, and the frames its preview holds.
@property(nonatomic) BOOL trimMonitorDragging;
@property(nonatomic) CGFloat trimMonitorDragStartX;
@property(nonatomic) int64_t trimMonitorDragFrames;
- (BOOL)trimMonitorPressAt:(NSPoint)point;
- (void)playAroundSelection;
- (TimelinePointerTarget)timelineTargetAt:(NSPoint)point event:(NSEvent*)event;
- (void)checkPlayAround;
- (void)trimMonitorDragTo:(NSPoint)point;
- (void)trimMonitorRelease;
@property(nonatomic) ui::workspace::Page workspacePage;
@property(nonatomic) ui::workspace::PanelSet workspacePanels;
@property(nonatomic) BOOL applyingWorkspace;
@property(nonatomic) BOOL rememberWorkspace;
// The smoke run's own layout keeps the panels' widths from before pages
// (320 and 301 points), so that every check aiming at a place of the
// timeline aims where it always has. NO outside the run.
@property(nonatomic) BOOL workspaceWidthsFixed;
@property(nonatomic) ui::workspace::PanelWidths workspaceFixedWidths;
// F2.2 -- Inspector content installed into rightDockPanel's Inspector slot
// (see -applicationDidFinishLaunching below).
@property(nonatomic, strong) CMInspectorView* inspectorView;
// REM5 -- the selection over Record and the Inspector of a motion node.
@property(nonatomic, strong) CMMotionCanvasView* motionCanvasView;
@property(nonatomic, strong) CMMotionInspectorView* motionInspectorView;
@property(nonatomic) BOOL motionInspectorShown;
// F2.4 -- the Chat tab's actual content (ChatPanelView.h), installed into
// rightDockPanel's PanelSlot::Chat once at startup (see
// -applicationDidFinishLaunching:). `chatBackend` wraps
// `self.state->document`/`self.state->editLog`
// by reference (McpLiveBackend.h) -- allocated once, alongside the view,
// and never freed or reassigned for the life of the window, the same
// "assign, never explicitly torn down" convention `state` below already
// uses.
@property(nonatomic, strong) CMChatPanelView* chatPanelView;
// ON3 -- docs/roadmap/ONGLETS-2026-10.md. The Agent's own floating window,
// which holds chatPanelView (AgentWindow.h). `agentShown`: shown by the
// person, though the application hides it while another is active;
// `agentUnread`: a row added while it was hidden; `agentPlaced`: placed once
// this session, its frame then the one to keep; `agentPlacing`: a frame set
// by the window, not by the hand.
@property(nonatomic, strong) NSPanel* agentWindow;
@property(nonatomic) BOOL agentShown;
@property(nonatomic) BOOL agentUnread;
@property(nonatomic) BOOL agentPlaced;
@property(nonatomic) BOOL agentPlacing;
@property(nonatomic, assign) McpLiveBackend* chatBackend;
// AX1 -- docs/roadmap/AX-2026-09.md. The endpoint an external agent reaches
// the open project through, over the same live backend as the chat panel.
// Allocated once the project is open and never freed: it may still have a
// connection waiting on the main thread when the app quits (Abandon).
@property(nonatomic, assign) MainThreadBackend* agentEndpointBackend;
@property(nonatomic, assign) McpEndpoint* agentEndpoint;
@property(nonatomic, strong) NSPopUpButton* binPopup;
@property(nonatomic, strong) NSPopUpButton* mediaPopup;
@property(nonatomic, strong) NSTextField* binSummaryLabel;
@property(nonatomic, strong) NSTextField* mediaTaskLabel;
@property(nonatomic, strong) NSProgressIndicator* mediaTaskProgress;
@property(nonatomic, strong) NSButton* mediaTaskCancelButton;
@property(nonatomic, copy) NSString* displayedMediaTaskId;
@property(nonatomic, strong) NSButton* assignMediaButton;
@property(nonatomic, strong) NSOutlineView* binOutline;
@property(nonatomic, strong) NSTableView* mediaTable;
@property(nonatomic, strong) NSSearchField* mediaSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleMediaIds;
@property(nonatomic, copy) NSString* selectedBinId;
@property(nonatomic, assign) BOOL updatingBinControls;
@property(nonatomic, strong) NSCollectionView* mediaCollection;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSImage*>* mediaThumbnails;
@property(nonatomic, strong) NSScrollView* mediaListScroll;
@property(nonatomic, strong) NSScrollView* mediaIconScroll;
@property(nonatomic, strong) NSSegmentedControl* mediaViewToggle;
@property(nonatomic, strong) NSButton* sourceMonitorButton;
// F2.3 -- the Media panel's three tabs (see -applicationDidFinishLaunching).
@property(nonatomic, strong) CMTabStripView* mediaTabStrip;
@property(nonatomic, strong) NSView* mediaTabContentMedia;
@property(nonatomic, strong) NSView* mediaTabContentAudio;
@property(nonatomic, strong) NSView* mediaTabContentCaptions;
// EF1 -- docs/roadmap/EFFETS-2026-10.md. The « Effets » tab.
@property(nonatomic, strong) CMEffectsPanelView* effectsPanelView;
@property(nonatomic, strong) NSTableView* audioTable;
@property(nonatomic, strong) NSScrollView* audioScroll;
@property(nonatomic, strong) NSSearchField* audioSearchField;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleAudioIds;
@property(nonatomic, strong) NSTextField* audioSummaryLabel;
@property(nonatomic, strong) NSTableView* captionStyleTable;
@property(nonatomic, strong) NSScrollView* captionStyleScroll;
@property(nonatomic, strong) NSMutableArray<NSString*>* visibleCaptionStyleIds;
@property(nonatomic, strong) NSTextField* captionSelectionLabel;
@property(nonatomic, strong) NSButton* addCaptionStyleButton;
@property(nonatomic, strong) NSButton* removeCaptionStyleButton;
@property(nonatomic, strong) NSButton* applyCaptionStyleButton;
@property(nonatomic, strong) NSButton* clearCaptionButton;
@property(nonatomic, strong) NSPopUpButton* captionFontPopup;
@property(nonatomic, strong) NSTextField* captionSizeField;
@property(nonatomic, strong) NSStepper* captionSizeStepper;
@property(nonatomic, strong) NSColorWell* captionColorWell;
@property(nonatomic, strong) NSTextField* infoLabel;
@property(nonatomic, strong) NSTextField* offlineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceOfflineMediaLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorTitleLabel;
@property(nonatomic, strong) NSTextField* sourceMonitorZoneLabel;
@property(nonatomic, strong) NSTextField* programMonitorTitleLabel;
// LG5 -- docs/roadmap/LEGENDES-2026-10.md. The cartons over Record, as the
// export draws them (CMCaptionView).
@property(nonatomic, strong) CMCaptionView* programCaptionView;
@property(nonatomic, strong) NSPopUpButton* sourceMonitorZoomPopup;
@property(nonatomic, strong) NSPopUpButton* programMonitorZoomPopup;
@property(nonatomic, strong) NSPopUpButton* programVideoScopePopup;
// G6 -- what Record shows when it is not the plain graded picture.
@property(nonatomic, strong) NSTextField* programCompareBadge;
@property(nonatomic, strong) NSButton* sourceMonitorToggleButton;
@property(nonatomic, strong) NSTextField* timelineTimecodeLabel;
@property(nonatomic, strong) NSMutableArray<NSTextField*>* trackHeaderLabels;
@property(nonatomic, strong) NSMutableArray<NSImageView*>* timelineToolIcons;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSString*>* shortcutBindings;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSMenuItem*>* shortcutMenuItems;
@property(nonatomic, strong)
    NSMutableDictionary<NSString*, NSTextField*>* keyboardEditingFields;
@property(nonatomic, strong) NSPopUpButton* keyboardCommandPopup;
@property(nonatomic, strong) NSButton* keyboardCommandModifier;
@property(nonatomic, strong) NSButton* keyboardOptionModifier;
@property(nonatomic, strong) NSButton* keyboardControlModifier;
@property(nonatomic, strong) NSButton* keyboardShiftModifier;
@property(nonatomic, strong) NSTimer* displayTimer;
// Timeline shortcuts remain global editing gestures while an Inspector
// control owns first responder. AppKit otherwise sends plain keys such as C
// only to the slider, making a graded clip appear impossible to cut.
@property(nonatomic, strong) id timelineShortcutMonitor;
@property(nonatomic, copy) NSString* documentPath;
// DEC39 -- the project the editor reopens on as it quits, chosen in the
// Fichier menu; nil otherwise.
@property(nonatomic, copy) NSString* pendingProjectSwitchPath;
@property(nonatomic, copy) NSString* lastProjectLoadError;
// DB2 -- warn once per period of pending delivery, without failing a save.
@property(nonatomic, assign) BOOL projectArchiveWarningShown;
// Out of lot, 29 October 2026 -- every project commit goes through this
// serial queue, in order: a gesture's is enqueued, any other waits for the
// queue to drain first, so an older state never lands after a newer one.
@property(nonatomic, strong) dispatch_queue_t projectCommitQueue;
// When the pending-archive status was last read: once a minute, not once
// per edit (it read the archive database on the main thread every time).
@property(nonatomic, assign) NSTimeInterval lastProjectArchiveCheck;
// Set while a background save has failed and none has succeeded since.
@property(nonatomic, copy) NSString* backgroundCommitError;
@property(nonatomic, assign) AppState* state;
- (BOOL)importMediaURLs:(NSArray<NSURL*>*)urls intoBin:(NSString*)binId;
// Out of lot, 29 October 2026 -- waits until every gesture save already on
// the commit queue is on disk (or has failed); anything that reads the
// package from disk calls it first.
- (void)drainProjectCommits;
- (BOOL)chooseStartupProject;
- (BOOL)createStartupProject;
- (CMFormPanel*)newProjectNamePanel;
- (BOOL)openStartupProject;
- (NSString*)resolvedProjectPath:(NSString*)selection;
// DEC39 -- docs/roadmap/PHASES-2026-08.md. « Fichier › Nouveau projet… » and
// « Ouvrir un projet… » in the editor (main.mm, ProjectSwitch.h).
- (NSSavePanel*)newProjectSavePanelNamed:(NSString*)projectName;
- (NSString*)createProjectNamed:(NSString*)projectName
                             at:(NSURL*)destination
                          error:(NSString**)failure;
- (NSOpenPanel*)projectOpenPanel;
- (void)newProjectFromEditor:(id)sender;
- (void)openProjectFromEditor:(id)sender;
- (BOOL)projectSwitchAllowed;
- (BOOL)switchToProjectAtPath:(NSString*)chosen;
- (void)collectPortableProject:(id)sender;
- (NSString*)selectedBrowserObjectId;
- (NSArray<NSString*>*)selectedMediaIds;
- (void)beginEditingBin:(NSString*)binId;
- (void)dismissAgentMarks;
// ON3 -- the Agent's window, from the toggle, the menu or its key.
- (void)toggleAgentWindow:(id)sender;
- (void)showAgentWindow:(BOOL)shown focus:(BOOL)focus;
- (void)refreshTimelineChrome;
- (void)layOutMediaBrowser;
- (void)layOutPageContents;
- (void)refreshWorkspaceActivity;
- (BOOL)hasValidTimelineRange;
- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple;
- (void)menuCopyTimelineClips:(id)sender;
- (void)menuPasteTimelineClips:(id)sender;
- (void)menuAddMarker:(id)sender;
- (void)menuAnnotateMarker:(id)sender;
- (void)menuRemoveMarker:(id)sender;
- (void)menuNextMarker:(id)sender;
- (void)menuPreviousMarker:(id)sender;
- (void)editMarkerNote:(const Ulid&)markerId;
- (void)goToAdjacentMarkerForward:(BOOL)forward;
- (void)menuEditCartonText:(id)sender;
- (BOOL)textEditKeyDown:(NSEvent*)event inTextView:(NSTextView*)text;
- (void)editCartonText:(const Ulid&)clipId;
- (BOOL)setCartonText:(const std::string&)typed forClip:(const Ulid&)clipId;
- (void)menuAddCrossDissolve:(id)sender;
- (void)menuRemoveCrossDissolve:(id)sender;
- (void)menuActivateMulticamAngle:(id)sender;
- (void)menuClearMulticamAngle:(id)sender;
- (void)menuAutoAlignByWaveform:(id)sender;
- (BOOL)alignSelectedSoundByWaveform;
- (void)finishSoundAlignment:(ui::separate_sound::SoundPair)pair
                    measured:(const std::string&)output;
- (NSMenuItem*)autoAlignMenuItem;
- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label;
- (BOOL)persistStagedDocument:(const Document&)document
                      editLog:(const EditLog&)editLog
                      message:(std::string&)message;
- (BOOL)applyAndPersistTimelineOperation:(Operation)operation
                                   error:(EditError&)error
                                 message:(std::string&)message;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                       message:(std::string&)message;
- (BOOL)commitProjectCandidate:(const Project&)project
                       editLog:(const EditLog&)editLog
                    projectLog:(const ProjectEditLog&)projectLog
                       message:(std::string&)message;
- (BOOL)activateTimelineIdentifier:(NSString*)identifier;
- (void)createTimelinePressed:(id)sender;
- (void)createTimelineFromSelectedMedia:(id)sender;
- (void)saveProjectPressed:(id)sender;
- (void)transcribeSelectedMedia:(id)sender;
- (void)transcribeSelectedMediaHesitations:(id)sender;
- (void)transcribeSelectedMediaFrenchVerbatim:(BOOL)frenchVerbatim;
- (BOOL)hesitationsWeightsAllowed;
- (void)showHesitationsRefusal:(const std::string&)detail;
- (void)transcribeTimeline:(id)sender;
- (void)transcribeTimelineHesitations:(id)sender;
- (void)transcribeTimelineFrenchVerbatim:(BOOL)frenchVerbatim;
- (void)chooseWhisperModel:(id)sender;
- (NSString*)requestWhisperModelPath;
- (NSString*)transcriptionModelPath;
- (void)migrateWhisperModelPreference;
- (void)buildStartupWindow;
- (void)gatherSetupNotice:(void (^)(const SetupNoticeResult& result))completion;
- (void)showSetupDiagnostic:(id)sender;
- (CMFormPanel*)setupDiagnosticPanel:(const SetupNoticeResult&)result;
- (BOOL)writeSetupDiagnosticToPasteboard:(NSPasteboard*)pasteboard;
- (void)openIconBin:(NSClickGestureRecognizer*)recognizer;
- (void)openIconItem:(id)sender;
- (void)requestSourcePosition:(RationalTime)position;
- (BOOL)performSourceEditInsert:(BOOL)insert;
- (void)updateSourceZoneLabel;
- (void)refreshMediaTaskStatus;
- (void)processCompletedMediaImports;
- (void)commitMediaImportBatch:(ProbedImportBatch*)batch;
- (BOOL)commitProbedMedia:(const std::vector<ProbedIngestFile>&)files
             documentPath:(const std::filesystem::path&)documentPath
                targetBin:(const std::optional<Ulid>&)targetBin
                  outcome:(IngestOutcome&)outcome
                errorName:(std::string&)errorName
                  message:(std::string&)message;
- (void)processCompletedMediaProxies;
- (void)processCompletedMediaWaveforms;
- (void)processCompletedMediaThumbnails;
- (void)processCompletedMediaTranscriptions;
- (void)processCompletedMediaRelinks;
- (void)processCompletedBatchRelinks;
- (void)reloadDecodeWorkers;
- (void)syncPlaybackReader;
- (void)checkSourceAvailabilityInBackground;
- (void)requestFrame:(int64_t)frame ofSource:(const Ulid&)sourceId;
- (void)requestFrame:(int64_t)frame
            ofSource:(const Ulid&)sourceId
                plan:(const DecodeWorker::ReadPlan&)plan;
- (DecodeWorker::ReadPlan)readPlanForClip:(const Ulid&)clipId
                                  onTrack:(const Ulid&)trackId
                                       at:(RationalTime)position
                                 onScreen:(BOOL)onScreen;
- (void)refreshAfterProjectMutation;
- (void)refreshAfterProjectRename;
- (void)enqueueProxyForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)enqueueWaveformForMediaIdentifier:(NSString*)identifier;
- (void)loadOrEnqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (BOOL)stageThumbnailForMediaIdentifier:(NSString*)identifier;
- (void)enqueueThumbnailForMediaIdentifier:(NSString*)identifier;
- (Ulid)enqueueTranscriptionForMediaIdentifier:(NSString*)identifier
                                      settings:(const WhisperSettings&)settings
                                       batchId:(const Ulid&)batchId
                                    reuseCache:(BOOL)reuseCache
                                   hesitations:(BOOL)hesitations;
@end

// The icon under a point of the media browser, which the harness aims at;
// main.mm implements it.
@interface ContextCollectionView ()
- (NSIndexPath*)iconIndexPathAtPoint:(NSPoint)point;
@end

// The private methods of AppDelegate the harness calls; main.mm
// implements them.
@interface AppDelegate ()
- (NSArray<NSDictionary<NSString*, NSString*>*>*)shortcutDefinitions;
- (void)editKeyboardShortcuts:(id)sender;
- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key;
- (void)copySetupDiagnostic:(id)sender;
- (void)toggleFullscreen:(id)sender;
- (void)configureSequence:(id)sender;
- (void)exportFinalVideo:(id)sender;
- (CMFormPanel*)exportProgressPanel:(BOOL)h264;
- (CMFormPanel*)collectProgressPanel;
- (void)configureColorManagement:(id)sender;
- (void)requestResolvedPosition:(RationalTime)position;
- (void)refreshBinControlsSelecting:(NSString*)selectedBinId;
- (BOOL)bin:(NSString*)binId canMoveInto:(NSString*)parentId;
- (void)audioSearchChanged:(id)sender;
- (NSInteger)collectionView:(NSCollectionView*)collectionView
     numberOfItemsInSection:(NSInteger)section;
- (NSCollectionViewItem*)collectionView:(NSCollectionView*)collectionView
    itemForRepresentedObjectAtIndexPath:(NSIndexPath*)indexPath;
- (id<NSPasteboardWriting>)collectionView:(NSCollectionView*)collectionView
       pasteboardWriterForItemAtIndexPath:(NSIndexPath*)indexPath;
- (BOOL)collectionView:(NSCollectionView*)collectionView
    canDragItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths
                   withEvent:(NSEvent*)event;
- (NSDragOperation)collectionView:(NSCollectionView*)collectionView
                     validateDrop:(id<NSDraggingInfo>)draggingInfo
                proposedIndexPath:(NSIndexPath**)proposedDropIndexPath
                    dropOperation:
                        (NSCollectionViewDropOperation*)proposedDropOperation;
- (BOOL)collectionView:(NSCollectionView*)collectionView
            acceptDrop:(id<NSDraggingInfo>)draggingInfo
             indexPath:(NSIndexPath*)indexPath
         dropOperation:(NSCollectionViewDropOperation)dropOperation;
- (BOOL)tableView:(NSTableView*)tableView
    writeRowsWithIndexes:(NSIndexSet*)rowIndexes
            toPasteboard:(NSPasteboard*)pasteboard;
- (BOOL)validateMenuItem:(NSMenuItem*)item;
- (void)openMediaIdentifierInSourceMonitor:(NSString*)identifier;
- (NSRect)programScopePanelFrame;
- (void)saveMonitorCompare;
- (BOOL)referenceStillFits;
- (void)toggleGradeBypass:(id)sender;
- (void)grabReferenceStill:(id)sender;
- (void)forgetReferenceStill:(id)sender;
- (NSTableRowView*)tableView:(NSTableView*)tableView
               rowViewForRow:(NSInteger)row;
- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row;
- (void)collectionView:(NSCollectionView*)collectionView
    didSelectItemsAtIndexPaths:(NSSet<NSIndexPath*>*)indexPaths;
- (void)controlTextDidEndEditing:(NSNotification*)notification;
- (void)moveMediaIdsToBin:(NSArray<NSString*>*)mediaIdentifiers;
- (MediaRate)playheadFrameRate;
- (double)videoHeight;
- (TimelineTool)effectiveTool;
- (void)setTimelineTool:(TimelineTool)tool;
- (void)requestTimelinePosition:(RationalTime)position;
- (BOOL)timelineDropMediaIds:(NSArray<NSString*>*)mediaIds
                 atViewPoint:(NSPoint)point;
- (void)setPlaybackDirection:(int)direction;
- (void)updateSelectionInfo;
- (BOOL)motionCanvasPrepare;
- (void)motionCanvasRedraw;
- (void)motionInspector:(CMMotionInspectorView*)view selectNode:(NSString*)uid;
- (void)motionCanvasSelect:(const std::string&)uid;
- (void)motionCanvasMouseUp:(NSEvent*)event;
- (void)motionInspector:(CMMotionInspectorView*)view
            commitField:(NSString*)property
                   text:(NSString*)text;
- (void)motionInspector:(CMMotionInspectorView*)view
              toggleKey:(NSString*)property;
- (void)motionInspector:(CMMotionInspectorView*)view
                 easing:(NSString*)easing
            forProperty:(NSString*)property;
- (void)motionInspector:(CMMotionInspectorView*)view
                 spring:(NSString*)property
                     on:(BOOL)on
                   from:(double)from
                     to:(double)to
                  delay:(NSInteger)delay
                damping:(double)damping;
- (void)refreshTimelineAfterEditFromPosition:(RationalTime)position;
- (void)menuUndo:(id)sender;
- (void)menuRedo:(id)sender;
- (void)askMarkerNote:(NSString*)note
                 time:(RationalTime)time
             duration:(RationalTime)duration
            removable:(BOOL)removable
               answer:
                   (void (^)(NSModalResponse response, NSString* typed))answer;
- (std::filesystem::path)decodeBasePath;
- (TimelineRenderData)timelineRenderData;
- (void)presentNearestFrameAtDeadline:(BOOL)isDisplayDeadline;
- (void)showCaptions:(const std::vector<CaptionCarton>&)cartons
                text:(NSString*)text
                view:(CMCaptionView*)view
             monitor:(NSView*)monitor
                zoom:(double)zoom;
- (NSBitmapImageRep*)drawEditorWindowOffscreen:(std::string&)error;
- (void)fitTimelineToViewportWidth;
- (void)startAgentEndpoint;
// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode.
- (void)trimSelectNearest;
- (void)trimCycleSide;
- (void)trimNudge:(int64_t)frames;
- (void)trimExtendToPlayhead;
- (void)toggleDynamicTrim;
- (BOOL)dynamicTrimTransport:(int)direction;
- (void)refreshDynamicTrim;
- (void)leaveTrimMode;
- (void)menuTrimNudgeBackMany:(id)sender;
- (void)menuTrimNudgeForwardMany:(id)sender;
- (void)menuStop:(id)sender;
- (void)layOutTrimMonitor:(TimelineRenderData&)programData;
- (void)toggleRetimeControls;
@end

// CF1 -- screen/window wiring only; the policy and agent contract live in
// VideoOutput.h/McpTools.cc, and the composite remains Renderer.mm's.
@interface AppDelegate (VideoOutput)
- (void)startVideoOutput;
- (void)stopVideoOutput;
- (void)refreshVideoOutputMenu;
- (void)selectVideoOutput:(NSMenuItem*)sender;
- (BOOL)showVideoOutputOnScreen:(NSScreen* _Nullable)screen
                        message:(std::string&)message;
- (BOOL)accessVideoOutput:(const std::optional<std::string>&)screen
                    state:(VideoOutputState&)state
                errorName:(std::string&)errorName
                  message:(std::string&)message;
@end

#if defined(CUTMACHINE_UI_SMOKE_TEST)
// Implemented by UiSmokeHarness.mm; main.mm starts them.
@interface AppDelegate (UiSmoke)
- (void)runUiSmokeTests;
// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md.
- (void)applyWorkspacePage:(ui::workspace::Page)page
                    panels:(const ui::workspace::PanelSet&)panels
                  remember:(BOOL)remember;
- (void)showWorkspacePage:(ui::workspace::Page)page;
- (void)toggleWorkspacePanel:(ui::workspace::Side)side;
- (void)runUiSmokeAgentJournalScenario;
- (void)finishUiSmokeTests;
- (void)runPlayProbe;
@end
#endif
