#pragma once

#include "Document.h"
#include "EditLog.h"

#include <optional>
#include <string>
#include <vector>

// The only boundary between exact timeline time and display coordinates.
// The graduation spacing the ruler falls back to when nothing is known of
// its labels.
inline constexpr double kTimelineMinTickSpacing = 56.0;

// Visual QA of 1 October 2026 -- the rate the view itself moves at. The app
// scrolled and zoomed at the sequence's frame rate: zoomed in to 400 points a
// second a frame is 16 points wide, so a slow two-finger pan (3 points an
// event) rounded back to where it was and the timeline did not move at all,
// and the time a zoom is meant to hold under the pointer slid by up to 13
// points. Where the view starts is not an edit: it has no reason to sit on a
// frame. At this rate a tick is under a tenth of a point at the closest zoom.
constexpr int32_t kTimelineViewRate = 48000;

// Coordinates are in logical points; backing-scale conversion belongs to the
// renderer and never affects edit rounding.
struct TimelineViewport {
    RationalTime view_start{0, 1};
    double pixels_per_second = 100.0;
    double track_height = 44.0;
    double header_width = 96.0;

    double TimeToX(RationalTime time) const;
    RationalTime XToTime(double x, int32_t rate) const;

    void ScrollByPixels(double delta_x, int32_t rate);
    void ZoomAroundX(double x, double factor, int32_t rate);
    void FitDuration(RationalTime duration, double width);
    // Ruler graduations at least `minimum_spacing` points apart. The caller
    // passes the width of its widest label: at 56 points, the old fixed
    // floor, 74-point timecodes ran into each other once the timeline was
    // fitted to the window (agentic QA of 23 September 2026).
    std::vector<double> TickXs(
        double width, double minimum_spacing = kTimelineMinTickSpacing) const;
};

// UI-2026-08 -- a practical lower bound must still allow long-form projects
// to fit. Four pixels per second capped an ordinary editor pane at roughly
// three minutes, making the zoom bar and FitDuration misleadingly stop early.
// Numerical safety floor only: at this scale an ordinary editor pane spans
// decades, so no fixed scale limits the zoom out. The limit an editor meets
// is relative to the program (TimelineZoomOutFloor, below).
constexpr double kTimelineMinPixelsPerSecond = 0.000001;
constexpr double kTimelineMaxPixelsPerSecond = 4000.0;

// UI-2026-08 -- Keep device-specific NSEvent details out of the viewport.
// AppKit only reports whether deltas are precise and which modifiers are
// active; this portable policy decides whether the same gesture pans or
// zooms, so mouse and trackpad regressions are covered by unit tests.
enum class TimelineScrollAction { Pan, Zoom };

struct TimelineScrollIntent {
    TimelineScrollAction action = TimelineScrollAction::Pan;
    double delta = 0.0;
};

// `zoomModifier` (Option in the app) makes any scroll a zoom. Without it a
// trackpad, a Magic Mouse or a wheel with smooth scrolling could only pan:
// they report precise deltas, and their only zoom was the pinch.
//
// A zoom's delta is negative to zoom in. `naturalScrolling` says the system
// already inverted the deltas (NSEvent's isDirectionInvertedFromDevice): the
// zoom follows the hand, not that setting. A wheel pushed away zooms in, as
// it does in every editor; with natural scrolling off it zoomed out.
TimelineScrollIntent ResolveTimelineScrollIntent(double deltaX, double deltaY,
                                                 bool preciseDeltas,
                                                 bool shiftPressed,
                                                 bool zoomModifier = false,
                                                 bool naturalScrolling = true);

// Visual QA of 1 October 2026 -- the scale change one scroll event asks for.
// A wheel's delta is not a count of notches: macOS accelerates it, from about
// 0.1 for a notch turned slowly to 10 and more for one flicked. Used as it
// came, ten slow notches zoomed by 13 % in all and ten fast ones by 197
// times. A notch is now worth the same step at any speed, a flick at most
// twice that; precise deltas are points and scale with the distance.
double TimelineScrollZoomFactor(double delta, bool preciseDeltas);

// Visual QA of 1 October 2026 -- what a zoom at the pointer holds still.
// Each wheel event used to anchor on whatever was under the pointer at that
// moment. Zooming out pins the view's start at zero, the picture slides
// under a pointer that has not moved, and the next notch in anchors on a
// time the editor never pointed at: traced on a 57 s film, a zoom out and
// back in from 7 s landed on 54 s -- on the empty time after the film when
// the pointer sat in the middle of the window. The anchor is now taken once
// and kept for as long as the pointer stays put and nothing else moves the
// view, so zooming out and back in returns where it left; and it is never
// taken beyond the program's end.
constexpr double kTimelineZoomAnchorSlop = 4.0;

struct TimelineZoomAnchor {
    double x = 0.0;
    RationalTime time{0, 1};
    // The view the last zoom on this anchor left. A view that differs has
    // been moved by something else since, and the anchor is taken afresh.
    double pixels_per_second = 0.0;
    RationalTime view_start{0, 1};
};

// Visual QA of 1 October 2026 -- how far out a zoom goes. Nothing stopped
// it: traced, a few flicks of the wheel took a 57 s film down to 1.4 points
// wide. Every pointer position is then past the film's end, and zooming back
// in lands on that end whatever was meant. The program now keeps at least
// half the width of the view, which still leaves half of it empty after the
// end to drop a shot in. An empty timeline has no floor but the numerical
// one (kTimelineMinPixelsPerSecond).
double TimelineZoomOutFloor(const TimelineViewport& viewport,
                            RationalTime duration, double width);

// Zooms by `factor` at the pointer, on `held` when it still applies and on
// the time under `x` otherwise, no further out than TimelineZoomOutFloor.
// Returns the anchor to hand back next time.
TimelineZoomAnchor ZoomTimelineAtPointer(
    TimelineViewport& viewport, double x, double factor, RationalTime duration,
    double width, const std::optional<TimelineZoomAnchor>& held);

// Visual QA of 1 October 2026 -- the zoom the editor asked for, the one of
// the program his timelines go to: around the playhead, wherever the pointer
// is. The playhead keeps its place on screen while the scale changes; out of
// sight, it is brought to the middle of the view, since a zoom is a way of
// looking closer at where one is. `held` is kept as above -- while the
// playhead has not moved and nothing else moved the view -- so that a zoom
// out pinned at the program's start and back in puts the playhead back where
// it was. The wheel, the pinch and the +/- keys zoom this way; the Zoom tool
// still zooms where it is clicked.
TimelineZoomAnchor ZoomTimelineAtPlayhead(
    TimelineViewport& viewport, const RationalTime& playhead, double factor,
    RationalTime duration, double width,
    const std::optional<TimelineZoomAnchor>& held);

enum class TimelineZoomBarPart { None, LeftHandle, Body, RightHandle };

struct TimelineZoomBarGeometry {
    double rail_x = 0.0;
    double rail_width = 0.0;
    double thumb_x = 0.0;
    double thumb_width = 0.0;
    double handle_width = 4.0;
    // The time the whole rail stands for, in seconds: the program, or the
    // view when the view reaches past the program's end.
    double ruler_seconds = 0.0;

    TimelineZoomBarPart HitTest(double x) const;
};

// Visual QA of 1 October 2026 -- the thumb is the visible span within the
// program. A program shown whole used to get a thumb half the rail wide, "to
// stay movable": the bar then said half the film was out of sight on a
// timeline just fitted to the window, and its width doubled the moment a
// handle crossed that point -- the handle was drawn 592 points away from the
// pointer holding it. A program shown whole now fills the rail.
//
// `heldRulerSeconds`, when positive, is the `ruler_seconds` a drag started
// with: while the drag lasts the rail keeps standing for that span, so the
// handle stays under the pointer even as the view grows past the program.
TimelineZoomBarGeometry CalculateTimelineZoomBarGeometry(
    const TimelineViewport& viewport, RationalTime duration, double width,
    double heldRulerSeconds = 0.0);

struct TimelineZoomBarDrag {
    TimelineZoomBarPart part = TimelineZoomBarPart::None;
    double pointer_x = 0.0;
    TimelineViewport initial_viewport;
    TimelineZoomBarGeometry initial_geometry;
};

void UpdateTimelineZoomBarDrag(TimelineViewport& viewport,
                               const TimelineZoomBarDrag& drag, double pointerX,
                               RationalTime duration, double width,
                               int32_t rate);

// PRD37 -- ROADMAP.md. A timeline fitted to its view stays fitted while
// nothing moves it. The QA of 3 October 2026 saw the end of the film out of
// view at opening: the fit was made while the window was being laid out,
// for a view 971 points wide, and the view settled at 917.5 without another
// fit. What a fit gave is recorded, as TimelineZoomAnchor records what a
// zoom gave: a resize fits again while the view is still exactly that, at
// another width. Once a zoom or a scroll has moved it, the scale is the
// editor's and a resize keeps it.
struct TimelineFit {
    bool made = false;
    double width = 0.0;
    RationalTime view_start{0, 1};
    double pixels_per_second = 0.0;
};

// Fits `viewport` to `duration` in `width` (FitDuration, which refuses a
// width within the track headers) and records what it gave.
TimelineFit FitTimeline(TimelineViewport& viewport, RationalTime duration,
                        double width);

// Whether a view now `width` wide is fitted again: it never was, or it shows
// the last fit untouched, made at another width. Never for a width within
// the track headers: a view not laid out yet waits for its real width.
bool TimelineFitFollowsResize(const TimelineFit& fit,
                              const TimelineViewport& viewport, double width);

// PRD37 -- each timeline keeps its own view. The QA saw a 9:16 timeline
// whose shots fit in its first eight seconds open at 39 s, empty: the view of
// the timeline left behind. What a timeline is left with is kept for the
// session, as an interface state: never in the document.
struct TimelineViewMemory {
    RationalTime view_start{0, 1};
    double pixels_per_second = 100.0;
    TimelineFit fit;
};

TimelineViewMemory RememberTimelineView(const TimelineViewport& viewport,
                                        const TimelineFit& fit);

// The view a timeline opens on: the one it was left with, while that view
// still starts within it -- fitted again if it was an untouched fit made at
// another width; a fit of the whole timeline the first time, or once an edit
// left the remembered view past its end. Returns what `viewport` is now as
// a fit (TimelineFit::made false when it is not one, or when the view is not
// laid out yet and the fit waits for its resize).
TimelineFit OpenTimelineView(TimelineViewport& viewport,
                             const TimelineViewMemory* left,
                             RationalTime duration, double width);

constexpr double kTimelineToolbarHeight = 26.0;
constexpr double kTimelineScaleHeight = 28.0;
constexpr double kTimelineRenderBandHeight = 5.0;
// Compatibility name used by the interaction engine for "everything above
// the first track". It now includes the integrated toolbar and render band;
// the actual ruler remains kTimelineScaleHeight.
constexpr double kTimelineRulerHeight =
    kTimelineToolbarHeight + kTimelineScaleHeight + kTimelineRenderBandHeight;
constexpr double kTimelineZoomBarHeight = 14.0;
constexpr double kTimelineEdgeHitWidth = 6.0;
// ON9 -- docs/roadmap/ONGLETS-2026-10.md. Where two clips meet, the cut
// itself, this far on either side, is the edit point taken as a roll, as
// Resolve rolls the centre of an edit point dragged in Selection or Trim
// mode (manual 21.1, p. 1065). The rest of each edge zone trims its side.
constexpr double kTimelineRollHitHalfWidth = 2.0;
// Cases 96 and 107b of VISUAL_QA_CHECKLIST.md -- how many sequence frames
// short of the pointer a refused trim is tried again one by one, before the
// bisection of the clip's limits: enough for a slowed clip's head to reach
// a frame of its rush at the usual speeds (every fifth frame at 2/5, every
// eighth at 1/8), bounded because each try applies the operation to a copy
// of the document while the pointer moves.
constexpr int kTimelineTrimAlignmentScan = 12;
constexpr double kTimelineSnapDistance = 8.0;

enum class PlayheadResolution { Frame, Sample };

RationalTime QuantizePlayheadPosition(RationalTime position,
                                      PlayheadResolution resolution,
                                      MediaRate frameRate,
                                      int32_t sampleRate = 48000);

enum class TimelineHitEdge { None, Head, Tail };
// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. Slide moves a clip between its
// neighbours (SlideEditOperation), RateStretch changes its length and so its
// speed (RateStretchOperation): the Trim tool's name bar and the ends of a
// clip's Retime controls.
//
// DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. RippleRateStretch is the same
// stretch with the Trim tool, where it ripples as in Resolve's Trim mode
// (RateStretchOperation::ripple): the clip keeps its head and what follows
// moves by the change of length, on its tracks and on the sync-locked ones.
enum class TimelineTrimMode {
    Normal,
    Ripple,
    Roll,
    Slip,
    Slide,
    RateStretch,
    RippleRateStretch
};

// Either stretch: what the window's cursor, info line and edit-point
// selection ask, where the two only differ by what follows the clip.
inline bool TimelineTrimModeStretches(TimelineTrimMode mode) {
    return mode == TimelineTrimMode::RateStretch ||
           mode == TimelineTrimMode::RippleRateStretch;
}

struct TimelineHit {
    Ulid clip_id;
    Ulid track_id;
    TimelineHitEdge edge = TimelineHitEdge::None;
};

struct TimelineGapSelection {
    Ulid track_id;
    RationalTime start;
    RationalTime duration;
};

struct TimelineClipRect {
    Ulid clip_id;
    Ulid source_id;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    bool selected = false;
    bool preview = false;
    bool valid = true;
    bool moving = false;
    bool audio = false;
    std::optional<RationalTime> sync_drift;
    // Out of lot -- the author, 29 October 2026: "when I trim a shot the waveform
    // is not cut, it is compressed". The source range this rectangle shows,
    // speed included: what the waveform drawn under it reads. While a trim
    // is held it follows the trim preview, so the waveform is cut at the
    // edge being dragged; timelineRenderData read the committed source_in
    // and duration, and squeezed the whole committed range into the
    // previewed width. A zero span (a ramp whose integral has no
    // RationalTime spelling) draws no waveform rather than a wrong one.
    RationalTime source_in;
    RationalTime source_span;
};

std::vector<const DocumentTrack*> TimelineTracksInDisplayOrder(
    const Document& document);

// Visual QA of 19 September 2026 -- the name a track goes by on screen: V1
// for the lowest video track up to the top one, A1, S1 for the first audio
// and caption tracks down the timeline. The timeline's headers and the
// Inspector read it here; the Inspector used to show the raw index ("piste
// 0"). Empty for a track the document does not hold.
std::string TrackDisplayName(const Document& document, const Ulid& trackId);

std::optional<TimelineHit> HitTestTimeline(const Document& document,
                                           const TimelineViewport& viewport,
                                           double x, double y, double width);
std::optional<TimelineGapSelection> HitTestTimelineGap(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, int32_t timeRate);
std::vector<Ulid> LassoHitTestTimeline(const Document& document,
                                       const TimelineViewport& viewport,
                                       double startX, double startY,
                                       double endX, double endY, double width);
// Out of lot -- the author, 29 October 2026: "Alt+Y to select everything to the
// right on the timeline", Resolve's Select All Clips Forward
// (editSelectClipsSelectAllAfter). Every clip of every unlocked track that
// still plays after `time`: the clip under the playhead is in, the one that
// ends exactly there is not.
std::vector<Ulid> TimelineClipsAfter(const Document& document,
                                     const RationalTime& time);
std::vector<Ulid> ExpandLinkedClipSelection(const Document& document,
                                            const std::vector<Ulid>& clipIds);

// Exact edit points shared by keyboard navigation and temporary playhead
// snapping. Results are sorted and deduplicated across every track.
std::vector<RationalTime> TimelineEditPoints(const Document& document);
std::optional<RationalTime> AdjacentTimelineEdit(const Document& document,
                                                 const RationalTime& position,
                                                 bool forward);
std::optional<RationalTime> SnapTimelineTimeToEdit(
    const Document& document, const TimelineViewport& viewport,
    const RationalTime& candidate);

// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A sequence marker is how an editor
// and an agent point at a moment for each other. The engine had them
// (Operations.h, add_marker) and the timeline neither drew nor hit one: a
// note left by an agent was invisible in the window, and the editor's notes
// went through another program. The flag's rectangle is worked out once,
// here, and both the Metal display list and the pointer read it, so the
// drawn flag and the clickable one cannot drift apart.
//
// Visual QA of 1 October 2026 -- seen on screen, the first marker was a
// square with a line down through the tracks: a second playhead. It is now
// what the editor's other program draws, a filled tag on the ruler whose
// point marks the time, and nothing in the tracks.
constexpr double kTimelineMarkerFlagWidth = 9.0;
constexpr double kTimelineMarkerFlagHeight = 10.0;
constexpr double kTimelineMarkerHitSlop = 3.0;

// The colour of a marker dropped in the window, as the document spells one.
// Blue, as in that other program: the playhead is orange, and the document's
// own default (#f5c542, a yellow) sits too close to it.
inline constexpr char kTimelineMarkerEditorColor[] = "#4da3ff";

// DIALOGUE-2026-10 D4 -- decided by the author on 8 October 2026: an agent points
// at a moment ("look here") with a marker of a category kept for it, drawn
// in a colour kept for it, and no new field in the document. The editor
// answers in its note (Maj+M) or resolves it by removing it; the agent reads
// either back. Orange, apart from the editor's blue and the document's
// yellow. A marker the editor files in this category reads as the agent's,
// which was accepted.
inline constexpr char kTimelineMarkerAgentCategory[] = "question";
inline constexpr char kTimelineMarkerAgentColor[] = "#ff8a3d";

// The colour a marker of `category` keeps, given the one asked (empty when
// none was): the agent's question takes its colour, and no other category
// may take that colour; another category asked none keeps `color` as it
// is, the document's default. False with the reason otherwise.
bool ResolveMarkerColor(const std::string& category, const std::string& asked,
                        std::string& color, std::string& error);

struct TimelineMarkerFlag {
    Ulid marker_id;
    // Where the marker's exact time falls: the tip of the tag.
    double x = 0.0;
    // The tag's bounding box, centred on that time and sitting on the
    // ruler's baseline; y is measured from the top of the timeline, like
    // every pointer coordinate in this file.
    double flag_x = 0.0;
    double flag_y = 0.0;
    // D6 -- where a span ends: the tag's body runs on to there. Equal to
    // `x` for a point.
    double end_x = 0.0;
};

// D6 -- docs/roadmap/DIALOGUE-2026-10.md. A marker being stretched, as the
// gesture has it so far and the document does not yet: what is drawn, and
// hit, while the gesture lasts.
struct TimelineMarkerPreview {
    Ulid marker_id;
    RationalTime time;
    RationalTime duration{0, 1};
};

// The tag itself, in the only shape primitive the timeline has: rectangles.
// A body the width of the box, then rows two points narrower each, down to
// a single point on the marker's time.
struct TimelineMarkerFlagRow {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};
std::vector<TimelineMarkerFlagRow> TimelineMarkerFlagShape(
    const TimelineMarkerFlag& flag);

// Markers any part of which falls in the visible span, in document order:
// the last one is drawn on top. `preview` replaces the time and duration of
// the marker it names.
std::vector<TimelineMarkerFlag> VisibleTimelineMarkers(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::optional<TimelineMarkerPreview>& preview = std::nullopt);
// The marker whose flag is under the pointer. When several are, the one an
// end of which is nearest: a point inside a span can still be picked.
std::optional<Ulid> HitTestTimelineMarker(const Document& document,
                                          const TimelineViewport& viewport,
                                          double x, double y, double width);
// A marker need not sit on a frame: an agent puts one on a word boundary.
// "The marker under the playhead" is therefore the one the playhead lands on
// when sent to it -- the same picture, whatever the sub-frame remainder.
// The first in document order when several share that picture.
const DocumentMarker* TimelineMarkerAtPlayhead(const Document& document,
                                               const RationalTime& playhead,
                                               MediaRate frameRate);
// D6 -- the marker whose note the playhead reads: the one it stands on, as
// above, or else the first span it is inside of.
const DocumentMarker* TimelineMarkerCoveringPlayhead(
    const Document& document, const RationalTime& playhead,
    MediaRate frameRate);
// D6 -- stretching a marker into a span, the gesture the editor's other
// program makes with Option held: the marker runs from `anchor` to where
// the pointer is, whichever side that is. A stretch back onto the anchor is
// a point again, spelled as the document spells none (0/1).
UpdateMarkerOperation TimelineMarkerSpanUpdate(const DocumentMarker& marker,
                                               const RationalTime& anchor,
                                               const RationalTime& pointer);
// The end of `marker` that stays put when a stretch begins at `x`: its time
// for a point, and for a span the end the pointer is further from.
RationalTime TimelineMarkerSpanAnchor(const DocumentMarker& marker,
                                      const TimelineViewport& viewport,
                                      double x);
// The exact time of the nearest marker strictly before or after the picture
// the playhead shows.
std::optional<RationalTime> AdjacentTimelineMarker(const Document& document,
                                                   const RationalTime& playhead,
                                                   MediaRate frameRate,
                                                   bool forward);
// What a marker dropped without a word is called, and the name a typed note
// becomes: Document::Validate takes neither an empty name, nor a control
// character (a pasted line break), nor more than 4096 bytes, and a note
// refused after it was typed is a note lost.
inline constexpr char kTimelineMarkerDefaultName[] = "Marqueur";
std::string TimelineMarkerNameFromNote(const std::string& typed);

// Builds the canonical blade operation. With linked selection enabled, every
// member of the same A/V group containing the cut is split atomically.
Operation TimelineCutOperationForClip(const Document& document,
                                      const DocumentClip& clip,
                                      const RationalTime& position,
                                      bool linkedSelection);

struct TimelineTrimPreview {
    Ulid clip_id;
    ExactClipTimes times;
    bool valid = false;
    Ulid link_group_id;
    std::vector<std::pair<Ulid, ExactClipTimes>> linked_times;
};

// Out of lot -- the author, 29 October 2026: "when trimming, the frame counter
// must show, as in Resolve". What a held trim has moved its edit by, in
// frames of the sequence, signed as the edge goes on the timeline (right is
// +): a head trimmed in or a tail pulled out reads +, ripple or not. A slip
// reads the source it moved, a slide the place. `edge` is where the moved
// edge now sits, for the window to draw the counter beside it. A display,
// so the frame count is rounded; the edit itself stays exact.
struct TimelineTrimReadout {
    Ulid clip_id;
    RationalTime edge;
    int64_t frames = 0;
    std::string text;
};
std::optional<TimelineTrimReadout> TimelineTrimReadoutFor(
    const Document& document, const std::optional<TimelineTrimPreview>& preview,
    const MediaRate& rate);
// "+12" under a second, "-1:05" (seconds:frames) from one on.
std::string TrimReadoutText(int64_t frames, const MediaRate& rate);

struct TimelineMovePreview {
    Ulid clip_id;
    Ulid target_track_id;
    RationalTime timeline_in;
    bool valid = false;
    Ulid link_group_id;
    std::vector<LinkedClipMove> linked_moves;
};

std::optional<RationalTime> ClipSyncDrift(const Document& document,
                                          const Ulid& clipId);

class TimelineInteraction {
public:
    TimelineInteraction(Document& document, EditLog& editLog,
                        TimelineViewport& viewport);

    void PointerDown(double x, double y, double width, int32_t timeRate);
    void PointerDrag(double x, double y, double width);
    bool PointerUp(EditError& error, std::string& message);
    void CancelDrag();

    // Empty unless *exactly one* clip is selected. Callers that must act on
    // an unambiguous single target rely on that.
    const Ulid& SelectedClipId() const;
    // The clip the selection is anchored on -- the one actually clicked --
    // even when selecting it pulled in others. Non-empty whenever anything
    // is selected. Surfaces that describe or edit one clip at a time (the
    // Inspector) want this, not SelectedClipId(): linking a video clip to
    // its audio is the norm for imported footage, so requiring a selection
    // of exactly one would leave them permanently blank.
    const Ulid& PrimarySelectedClipId() const;
    const std::vector<Ulid>& SelectedClipIds() const;
    void SelectClip(const Ulid& clipId);
    void SelectClips(const std::vector<Ulid>& clipIds);
    const std::optional<TimelineGapSelection>& SelectedGap() const;
    void ClearGapSelection();
    const std::optional<RationalTime>& RequestedPlayhead() const;
    void ClearRequestedPlayhead();
    const std::optional<TimelineTrimPreview>& TrimPreview() const;
    const std::optional<TimelineMovePreview>& MovePreview() const;
    bool HasActiveDrag() const;
    void SetSnappingEnabled(bool enabled);
    bool SnappingEnabled() const;
    void SetPlayheadSnapTarget(std::optional<RationalTime> target);
    // Out of lot -- the author, 29 October 2026: "the magnet must also hold the
    // playhead when trimming". The playhead as the window has it: a target
    // of the magnet whenever the magnet is on, as in Resolve. It was one only
    // while Command was held (SetPlayheadSnapTarget above, which still makes
    // it one with the magnet off).
    void SetPlayhead(std::optional<RationalTime> playhead);
    // FC1 -- docs/roadmap/FILMCRAFT-2026-10.md. The timeline's In and Out
    // points are targets of the magnet, as in FilmCraft. The window keeps
    // them (an interface state, never the document's), so it hands them over
    // here before a gesture; either may be absent.
    void SetInOutSnapTargets(std::optional<RationalTime> in,
                             std::optional<RationalTime> out);
    const std::optional<RationalTime>& SnapGuideTime() const;
    void SetLinkedSelectionEnabled(bool enabled);
    void SetTrimMode(TimelineTrimMode mode);
    TimelineTrimMode TrimMode() const;
    // Visual QA of 19 September 2026 -- a timeline full of clips left no
    // empty space to start a lasso from, and nothing else selected more than
    // one clip. When set, the next PointerDown on a clip's body adds it to
    // the selection, or takes it out if it is already in (its linked
    // partners with it), and a drag then moves the whole selection. On an
    // edge it changes nothing: Shift there is the ripple trim.
    void SetSelectionToggle(bool toggle);
    // DEC40 -- docs/roadmap/SYNCHRO-2026-10.md. Command-click extends a
    // selection of clips, as in Resolve, and Shift-click stays. Command on
    // a body was not free: held from the press, it makes the playhead a
    // target of the magnet for the drag that follows (main.mm), the one way
    // to drop a selection on the playhead with the magnet off. So the press
    // adds a clip that is not selected and the drag moves the whole
    // selection, as with Shift; on a clip already selected it keeps the
    // selection and drags it, and only a release that moved nothing takes
    // the clip out (its linked partners with it). On a locked track, where
    // nothing is dragged, the press takes it out at once.
    void SetSelectionExtend(bool extend);
    std::optional<Operation> PendingOperation() const;

private:
    void UpdatePreview(double x, double y, double width);
    // DEC40 -- takes deselect_on_release_ out of the selection.
    void DeselectPendingClips();
    // The trim of `clip`'s `edge` by `delta`, stopped at what the clip
    // can give: the rush on that side, one frame of length, and -- for a
    // trim that leaves the rest of the track where it is
    // (`holdsNeighbours`) -- its neighbour and the timeline's start. A
    // ripple and a roll move what is around the edge, so they keep only the
    // first two.
    RationalTime ConstrainTrimDelta(const DocumentClip& clip, TrimEdge edge,
                                    RationalTime delta,
                                    bool holdsNeighbours = true) const;
    // `excludedClipId`'s edges are never targets, nor `partnerClipId`'s: the
    // other side of a cut a roll moves along with it.
    //
    // FC1 -- docs/roadmap/FILMCRAFT-2026-10.md. The magnet used to see only
    // `track`: a cut on V1 did not hold a clip dragged on V2, nor did a
    // marker or the In point. Targets, in the order that wins a tie: 0, the
    // playhead (magnet on, or Command held), the sync anchor, the edges of
    // `track`, the
    // edges of every other track, the markers (start, then end of a span),
    // In, Out. The nearest within kTimelineSnapDistance wins. Never targets:
    // the clips the gesture carries along -- the selection, which a move
    // moves and a linked trim trims, and, for a roll, the partner's linked
    // clips -- since their edges sit where the gesture started.
    std::optional<RationalTime> FindSnapTime(
        RationalTime candidate, const DocumentTrack& track,
        const Ulid& excludedClipId, bool includeSyncTarget = false,
        const Ulid& partnerClipId = {}) const;

    Document& document_;
    EditLog& edit_log_;
    TimelineViewport& viewport_;
    Ulid selected_clip_id_;
    Ulid primary_clip_id_;
    std::vector<Ulid> selected_clip_ids_;
    std::optional<TimelineGapSelection> selected_gap_;
    std::optional<RationalTime> requested_playhead_;
    std::optional<TimelineTrimPreview> preview_;
    std::optional<TimelineMovePreview> move_preview_;
    std::optional<Operation> trim_operation_;
    std::optional<TimelineHit> drag_hit_;
    std::optional<RationalTime> drag_start_time_;
    int32_t drag_rate_ = 1;
    double drag_x_ = 0.0;
    bool snapping_enabled_ = true;
    std::optional<RationalTime> playhead_snap_target_;
    std::optional<RationalTime> playhead_;
    std::optional<RationalTime> in_snap_target_;
    std::optional<RationalTime> out_snap_target_;
    bool linked_selection_enabled_ = true;
    TimelineTrimMode trim_mode_ = TimelineTrimMode::Normal;
    bool selection_toggle_ = false;
    bool selection_extend_ = false;
    // DEC40 -- what a Command-press on a selected clip takes out of the
    // selection if its release moved nothing.
    std::vector<Ulid> deselect_on_release_;
    std::optional<RationalTime> snap_guide_time_;
};

std::vector<TimelineClipRect> VisibleTimelineClips(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& selectedClipIds,
    const std::optional<TimelineTrimPreview>& preview,
    const std::optional<TimelineMovePreview>& movePreview = std::nullopt);

// FC2 -- docs/roadmap/FILMCRAFT-2026-10.md. What is under the pointer, and
// what a press there takes hold of, decided once. The hover (cursor, info
// line), the click and an agent asked "what is at this point" each read the
// geometry for themselves in main.mm, and could drift apart; FilmCraft
// answers all three from one function (grab_at). This one reproduces the
// click as it stands -- nothing in it is a new rule.
//
// The tools of the timeline's toolbar, as the window names them.
//
// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The fifth tool was Slip, under
// T, Resolve's key for its Trim Edit mode; it is now that mode (Resolve
// 21.1 manual, p. 1061-1066): a clip's edge ripples, Command-drag rolls as
// it does with Select, the upper part of its body slips and its name bar
// slides it (FC3).
enum class TimelinePointerTool { Select, Hand, Zoom, Cut, Trim };

struct TimelinePointerModifiers {
    bool shift = false;
    bool option = false;
    bool command = false;
};

// The trim mode a press of `tool` with `modifiers` sets: Command rolls; the
// Trim tool ripples otherwise, the others ripple with Shift. The interaction
// is set to it whatever is under the pointer; it acts on an edge. On a body
// the Trim tool slips or slides instead, and the ends of Retime controls
// stretch (TimelineHitAt says so); the others move a body.
TimelineTrimMode TimelineTrimModeFor(TimelinePointerTool tool,
                                     TimelinePointerModifiers modifiers);

enum class TimelinePointerTargetKind {
    // Nothing a press takes hold of: the ruler, a gap, an empty row, the
    // space under the tracks -- or any point for the Hand and Zoom tools,
    // which act on the view. A press there moves the playhead (and, with
    // Select, may start a lasso).
    None,
    // The zoom strip along the bottom. Its part may be None: a press there
    // still belongs to the strip, and does nothing.
    ZoomBar,
    // A marker's flag, whatever the tool.
    Marker,
    ClipBody,
    // Head or tail, within kTimelineEdgeHitWidth: Select and Trim. The blade
    // cuts wherever it falls.
    ClipEdge,
};

struct TimelinePointerTarget {
    TimelinePointerTargetKind kind = TimelinePointerTargetKind::None;
    TimelineZoomBarPart zoom_bar_part = TimelineZoomBarPart::None;
    Ulid marker_id;
    // Option on a marker: it is taken hold of to be stretched into a span,
    // and the playhead stays where it is.
    bool marker_stretch = false;
    Ulid clip_id;
    // The clip's track; for None, the track whose row is under the pointer,
    // if any (where a gap is selected, where a lasso may start).
    Ulid track_id;
    TimelineHitEdge edge = TimelineHitEdge::None;
    // TimelineTrimModeFor(tool, modifiers), for every kind.
    TimelineTrimMode trim_mode = TimelineTrimMode::Normal;
    // Select tool, a clip's body, Option: a drag duplicates.
    bool duplicate = false;
    // Select tool, a clip's body, Shift: the press adds the clip to the
    // selection or takes it out (SetSelectionToggle).
    bool selection_toggle = false;
    // DEC40 -- Select tool, a clip's body, Command without Shift or Option:
    // the press extends the selection (SetSelectionExtend). Option-Command
    // stays a duplicate snapped to the playhead.
    bool selection_extend = false;
    // A clip on a locked track is selected by a press, never dragged.
    bool track_locked = false;
    // The transition drawn over this point, if any. The click acts on the
    // clip under it, as it always has: a transition is not taken hold of
    // by the pointer yet.
    Ulid transition_id;
};

// `x` and `y` are timeline coordinates (TimelineHit's), `width` and `height`
// the timeline's, `duration` the program's (the zoom strip's geometry,
// CalculateTimelineZoomBarGeometry). The toolbar, the track header controls and
// the add-track row are the window's own chrome, laid out in main.mm, and are
// tested there first; none of them overlaps what this function answers.
//
// FC3 -- `retimeClipIds`: the clips whose Retime controls are shown, an
// interface state the window keeps. With Select or Trim, an end of one
// (VisibleTimelineRetimeBands) is a ClipEdge whose trim mode is
// RateStretch, whatever the modifiers; the Trim tool's body is Slide on the
// clip's name bar (from kTimelineClipNameBandTop down) and Slip above it.
TimelinePointerTarget TimelineHitAt(
    const Document& document, const TimelineViewport& viewport, double x,
    double y, double width, double height, RationalTime duration,
    TimelinePointerTool tool, TimelinePointerModifiers modifiers,
    const std::vector<Ulid>& retimeClipIds = {});

// FC2 -- a transition as the timeline draws it: across its span, inset four
// points in its track's row. TimelineHitAt reads it; the display list can
// draw from it, so that the drawn rectangle and the pointed one stay one.
struct TimelineTransitionRect {
    Ulid transition_id;
    Ulid track_id;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    // Where the cut falls inside it.
    double cut_x = 0.0;
};

std::vector<TimelineTransitionRect> VisibleTimelineTransitions(
    const Document& document, const TimelineViewport& viewport, double width);

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. An edge of a clip the trim mode
// holds (TrimSession.h, TrimSessionEdgeMarks): drawn where it is pressed,
// so the mark and the press cannot drift apart. `at_limit`: no media left
// beyond it, which Resolve draws red and an edge with handles green (Resolve
// 21.1 manual, p. 1089).
struct TimelineEdgeMark {
    Ulid clip_id;
    TrimEdge edge = TrimEdge::Tail;
    bool at_limit = false;
};

struct TimelineEdgeMarkRect {
    Ulid clip_id;
    TrimEdge edge = TrimEdge::Tail;
    bool at_limit = false;
    // The zone HitTestTimeline gives the edge, kTimelineEdgeHitWidth inside
    // the clip, clipped to the view; `y` and `height` are its track's row.
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

std::vector<TimelineEdgeMarkRect> VisibleTimelineEdgeMarks(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<TimelineEdgeMark>& marks);

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. A clip's Retime controls as the
// timeline draws them and the pointer finds them: the band along the top
// of its body, clipped to the view, and its two ends, the edge zones of the
// clip within the band (kTimelineEdgeHitWidth inside, never past the
// middle). An end out of view has a zero width. y is from the top of the
// timeline, like every pointer coordinate here.
struct TimelineRetimeBand {
    Ulid clip_id;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    double head_x = 0.0;
    double head_width = 0.0;
    double tail_x = 0.0;
    double tail_width = 0.0;
    // The clip's speed as its badge spells it (ClipBadges.h), "100%" at
    // its own speed, as Resolve writes it.
    std::string label;
};

std::vector<TimelineRetimeBand> VisibleTimelineRetimeBands(
    const Document& document, const TimelineViewport& viewport, double width,
    const std::vector<Ulid>& retimeClipIds);

// Builds one exact, undoable edit over every unlocked track. Clear preserves
// the range as a gap; ripple closes it. Clips crossing a boundary are split
// while retaining exact source time.
std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple);

// How a ripple range delete treats sync-locked tracks it does not target.
// Cut removes the same range from them. Continuous maps their clips through
// the same time change -- before the range unchanged, inside it collapsed to
// its in point, after it moved back -- but keeps each clip's pictures
// continuous from its head, shortening it at its tail instead.
//
// ITW7 -- ROADMAP.md. With Cut, a cutaway spanning a cut in the interview
// lost frames in the middle of the shot, a visible jump. Cutting twelve
// passages without that meant removing the cutaway track, cutting, and
// placing fourteen shots back by hand. A cutaway illustrates the words under
// it: when those words end earlier, so should the cutaway, whole.
enum class RangeDeleteFollowers { Cut, Continuous };

// Targeted tracks are edited directly. During ripple edits, other unlocked
// tracks participate only when their sync lock is enabled. An empty target
// list intentionally means that no track is edited.
std::optional<DeleteGapOperation> TimelineRangeDeleteOperation(
    const Document& document, const RationalTime& rangeIn,
    const RationalTime& rangeOut, bool ripple,
    const std::vector<Ulid>& targetedTrackIds,
    RangeDeleteFollowers followers = RangeDeleteFollowers::Cut);

// Builds the exact range edit used by UI ripple-delete for a clip selection.
// Linked selections may span tracks; downstream sync-locked tracks follow the
// same range through TimelineRangeDeleteOperation.
std::optional<DeleteGapOperation> BuildTimelineRippleDeleteSelection(
    const Document& document, const std::vector<Ulid>& clipIds);

// Builds one atomic source-to-record edit. Insert opens time on the target and
// every unlocked sync-locked follower; overwrite replaces only the target
// interval. The returned exact-track operation is deterministic and undoable.
// The target is a video track, or -- out of lot, 29 October 2026 -- an audio
// track alone, for a sound-only edit (music, Shift on a drop).
std::optional<DeleteGapOperation> TimelineSourceEditOperation(
    const Document& document, const Ulid& sourceId,
    const RationalTime& sourceIn, const RationalTime& sourceOut,
    const RationalTime& timelineIn, const Ulid& targetTrackId, bool insert,
    const std::vector<Ulid>& audioTargetTrackIds = {});

// Out of lot -- the author, 29 October 2026: "quand je monte un plan ça doit
// automatiquement créer une nouvelle piste audio ou vidéo, on peut du coup
// virer les + en bas des pistes". The tracks a media dropped on the timeline
// goes to, as in DaVinci Resolve 21.1 (manual, p. 891): the track under the
// pointer, the media's other part on the track of the same number (V2 and
// A2), and a new track where there is none to take it -- the blank below
// the last track, a caption track, a number the other kind does not reach, a
// locked partner. The drop used to need a video track under the pointer,
// refused a sound-only media there (music had no way in), and sent the sound
// of a cutaway dropped on V2 to the first free audio track, the interview's.
struct TimelineDropTracks {
    // An empty id with `create_*`: a new track of that kind, above the other
    // video tracks or below the other audio tracks. Empty without it: the
    // media's part of that kind is not edited.
    Ulid video_track_id;
    bool create_video_track = false;
    Ulid audio_track_id;
    bool create_audio_track = false;
};

// `rowY`: the pointer in timeline coordinates (TimelineHit's). `video` and
// `audio`: what the dropped media carry, Resolve's modifiers applied (Option
// keeps the picture, Shift the sound). Nullopt above the tracks, without a
// part to edit, or on a locked track under the pointer that would take one;
// `refusal` then says why, in the interface's words (empty above the tracks).
std::optional<TimelineDropTracks> TimelineDropTracksAt(
    const Document& document, const TimelineViewport& viewport, double rowY,
    bool video, bool audio, std::string& refusal);

// ADOBE-CLI-2026-09 -- three-point arithmetic belongs to the engine, so a
// script and the Source/Record monitors resolve the same exact interval.
// Out points are exclusive; missing source bounds use the full rush.
struct SourceRecordEditPoints {
    std::optional<RationalTime> source_in;
    std::optional<RationalTime> source_out;
    std::optional<RationalTime> timeline_in;
    std::optional<RationalTime> timeline_out;
};

struct SourceRecordEditRange {
    RationalTime source_in;
    RationalTime source_out;
    RationalTime timeline_in;
};

bool ResolveSourceRecordEditRange(const RationalTime& sourceDuration,
                                  const SourceRecordEditPoints& points,
                                  SourceRecordEditRange& range,
                                  std::string& error);
