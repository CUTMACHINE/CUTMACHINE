#pragma once

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. Trim mode as portable policy:
// which edit points are selected and on which side, the nudge by one or five
// frames, the extension to the playhead, the J/K/L dynamic trim and what a
// Trim Monitor shows. Adapted from FilmCraft crates/engine/src/trim.rs and
// crates/ui-egui/src/panels/trim_monitor.rs (MIT OR Apache-2.0, commit
// 09aaf9b).
//
// Why a session and not an operation: a selected edit point is interface
// state, like the clip selection. It never goes into the document and is
// never journaled; rerunning a project elsewhere must give the same edit
// without it (AGENTS.md, "L'état d'interface est une préférence locale").
// What it produces is ordinary operations of Operations.h -- RippleTrim,
// RollEdit, TrimClip, TrimLinkedClips -- which the caller applies through
// EditLog::Apply like any other edit. No new operation is introduced, so
// the surfaces that already apply those (CLI, MCP) need nothing new.
//
// Where FilmCraft is not followed:
// - it reads its clock in floating seconds and keeps the shuttle speed as a
//   float. Here the clock is an exact RationalTime the caller injects on
//   every call, the speed an integer (1, 2, 4, 8), and the offset a whole
//   number of frames computed by integer floor division (PHILOSOPHY.md
//   principle 4). No real clock is ever read in this file.
// - its live dynamic trim rewrites the project on every tick and truncates
//   the history afterwards. Here the document is never touched before K:
//   the offset lives in the session, PreviewDocument shows it on a copy, and
//   K hands back the one operation that commits it.
// - an edit point is addressed by the ULIDs of the two clips that meet at it
//   (PHILOSOPHY.md principle 5), not by "clip plus out flag": a roll needs
//   both, and the Trim Monitor shows both.

#include "Document.h"
#include "EditLog.h"
#include "Operations.h"
#include "RationalTime.h"
#include "TimelineView.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace trim {

// Which side of an edit point moves.
//   A    -- the tail of the outgoing clip (the clip before the cut);
//   B    -- the head of the incoming clip (the clip after it);
//   Roll -- both: the cut moves, the total duration does not.
enum class TrimSide { A, B, Roll };

// What an A or B side does to the rest of the track. A roll ignores it.
//   Ripple -- the material after the edit follows, as RippleTrimOperation;
//   Trim   -- nothing else moves: a gap opens, or the trim is blocked by the
//             neighbour, as TrimClipOperation.
enum class TrimStyle { Ripple, Trim };

// The refusals of the trim mode, by name. Every planning call answers one;
// None means the plan holds an operation that applies.
enum class TrimRefusal {
    None,
    // Nothing is selected, or no edit point lies on the targeted tracks.
    NoEditPoint,
    // A selected point names a clip the document no longer holds (an undo
    // took it away, another surface removed it).
    StaleEditPoint,
    // A roll, or an A or B side, asked at an edge that has no clip on that
    // side: a free head, a free tail, or two clips no longer adjacent.
    NoJunction,
    // The source has no frames left beyond the edit (the handle is used up).
    MediaLimit,
    // The trim would leave a clip with no duration.
    ClipTooShort,
    // A plain trim runs into the clip next to it, or a head before 0.
    BlockedByNeighbour,
    // The edit, or what follows it, sits on a locked track.
    LockedTrack,
    // The sequence frame rate is not a positive fraction.
    InvalidFrameRate,
    // Every selected point is already where it was asked to go.
    NoMovement,
    // The operation refused for another reason; `detail` carries the
    // engine's message.
    Rejected,
};

// Stable English identifier, for logs and tests ("MediaLimit").
const char* TrimRefusalName(TrimRefusal refusal);
// What the window shows, in French.
const char* TrimRefusalDescription(TrimRefusal refusal);

// One selected edit point. `outgoing_clip_id` is the clip whose tail is the
// cut, empty at a free head; `incoming_clip_id` the clip whose head is the
// cut, empty at a free tail. Both are on one track.
//
// The shifts count how far the outgoing Out point and the incoming In point
// moved since the point was selected, as the Trim Monitor's counters do in
// Premiere and Resolve; RecordApplied adds to them.
struct TrimPoint {
    Ulid outgoing_clip_id;
    Ulid incoming_clip_id;
    TrimSide side = TrimSide::Roll;
    RationalTime out_shift{0, 1};
    RationalTime in_shift{0, 1};
};

// A planned trim: one operation (a BatchOperation when several points move,
// so that the gesture is one undo step), or a refusal. `movements` is, per
// selected point and in the same order, how far its edit point is asked to
// move (later is positive); a point folded into another one's operation
// (its clip is that operation's linked partner) still has its movement.
struct TrimEdit {
    TrimRefusal refusal = TrimRefusal::None;
    std::string detail;
    // The clip the refusal is about, when there is one.
    Ulid refused_clip_id;
    std::optional<Operation> operation;
    std::vector<RationalTime> movements;

    bool ok() const {
        return refusal == TrimRefusal::None && operation.has_value();
    }
};

// The J/K/L state, readable by the window and the Trim Monitor.
struct DynamicTrimState {
    bool active = false;
    // Frames per second multiplier: -8, -4, -2, -1, 0 (held at a limit), 1,
    // 2, 4, 8. Negative plays backward.
    int speed = 0;
    // Whole frames the edit points have moved since J or L was first
    // pressed.
    int64_t offset_frames = 0;
    // The trim ran into a limit (handle, neighbour, length) and holds there;
    // K still commits what was reached.
    bool at_limit = false;
    // The clock and the offset the current speed is counted from.
    RationalTime anchor_clock{0, 1};
    int64_t anchor_offset_frames = 0;
};

// The largest shuttle speed, as in FilmCraft and the J/L keys of Resolve.
inline constexpr int kMaxShuttleSpeed = 8;
// The "many frames" nudge. Premiere's default Large Trim Offset is 5, and
// Resolve's fast nudge is 5 frames by default.
inline constexpr int64_t kLargeTrimFrames = 5;

class TrimSession {
public:
    const std::vector<TrimPoint>& Points() const { return points_; }
    bool Active() const { return !points_.empty(); }
    // FC4 (window) -- every selected point still has the clips its side
    // needs in `document`. False once an edit elsewhere (an undo, another
    // surface) took one away or pulled a cut apart: the window then leaves
    // trim mode rather than plan against clips that moved.
    bool StillHolds(const Document& document) const;
    // Leaves trim mode: no point, no dynamic trim. Nothing is written.
    void Clear();

    TrimStyle Style() const { return style_; }
    void SetStyle(TrimStyle style) { style_ = style; }
    // Linked A/V partners follow the edge they share (LinkedEdgePartnerIds),
    // as the linked selection does for a drag. On by default, as the MCP
    // ripple_trim default.
    bool FollowLinks() const { return follow_links_; }
    void SetFollowLinks(bool follow) { follow_links_ = follow; }

    // Replaces the selection with, on each targeted track, the edit point
    // nearest `playhead` that has the asked side: an outgoing clip for A,
    // an incoming clip for B, both for a roll. An empty `targetedTrackIds`
    // means every track. Locked tracks are skipped: a trim there would be
    // refused. A tie goes to the earlier edit. The shifts restart at zero.
    TrimRefusal SelectNearest(const Document& document,
                              const RationalTime& playhead,
                              const std::vector<Ulid>& targetedTrackIds,
                              TrimSide side);

    // FC4 (window) -- Resolve's V: "selects the edit point closest to the
    // playhead, and moves the playhead there" (Resolve 21.1 manual, p.
    // 1069). SelectNearest takes the nearest point of each targeted track,
    // however far: with every track targeted, as the window starts, a music
    // cut a minute away was selected with the shot's cut under the playhead,
    // and a nudge moved both. Here the one edit time nearest `playhead`
    // among the targeted tracks is found first (a tie goes to the earlier),
    // then every targeted track's point at exactly that time is selected,
    // with the asked side; `editTime` is that time, for the playhead.
    // Locked tracks are skipped. An empty `targetedTrackIds` targets none:
    // the window always names its targets.
    TrimRefusal SelectNearestEdit(const Document& document,
                                  const RationalTime& playhead,
                                  const std::vector<Ulid>& targetedTrackIds,
                                  TrimSide side, RationalTime& editTime);

    // The edit point at `edge` of `clipId`, on the asked side. With `add`
    // false it replaces the selection; with `add` true it is added, or
    // removed when that point (the same two clips) is already selected, as
    // a Command-click toggles a clip.
    TrimRefusal SelectEdge(const Document& document, const Ulid& clipId,
                           TrimEdge edge, TrimSide side, bool add);

    // Every selected point goes to its next side, A -> Roll -> B -> A,
    // skipping the sides its edge does not have (a free tail stays on A).
    // Refused while a dynamic trim runs.
    TrimRefusal CycleSide(const Document& document);
    // ON8 -- docs/roadmap/ONGLETS-2026-10.md. A side chosen outright, as
    // the trim bar's A, Roll and B buttons choose it: every point takes it,
    // or none does (NoJunction) when one of them has no clip on that side.
    TrimRefusal SetSide(const Document& document, TrimSide side);
    // Whether every selected point has a clip on `side`.
    bool CanSetSide(const Document& document, TrimSide side) const;

    // Moves every selected point by `frames` sequence frames (later is
    // positive): +-1 for "trim forward/backward", +-kLargeTrimFrames for
    // "many". Pure: the document is not touched.
    TrimEdit PlanNudge(const Document& document, int64_t frames) const;

    // Moves every selected point to `playhead`: each by playhead minus its
    // own edit time. A point already there does not move.
    TrimEdit PlanExtendToPlayhead(const Document& document,
                                  const RationalTime& playhead) const;

    // Adds an applied edit's movements to the points' shifts. The window
    // calls it after its own EditLog::Apply succeeded.
    void RecordApplied(const TrimEdit& edit);

    // Applies `edit` through `log` and records it: the path a test, or a
    // surface that owns its EditLog, takes. False, with the log's error,
    // when the edit is a refusal or the log refuses it.
    bool Apply(const TrimEdit& edit, Document& document, EditLog& log,
               EditError& error, std::string& message);

    // J (direction -1) or L (+1) in trim mode. The first press starts a
    // dynamic trim at speed 1 from `now`; pressing the same direction again
    // doubles the speed up to kMaxShuttleSpeed; the other direction starts
    // again at speed 1 the other way. The offset reached so far is kept.
    TrimRefusal Shuttle(const Document& document, int direction,
                        const RationalTime& now);

    // Brings the offset up to `now`: anchor offset plus the whole frames the
    // elapsed time covers at the current speed, truncated toward the
    // anchor. When that offset would be refused, the offset holds at the
    // last one that applies, the speed drops to 0 and at_limit is set.
    void Tick(const Document& document, const RationalTime& now);

    // K: brings the offset up to `now`, ends the dynamic trim and plans the
    // offset reached as ONE edit (a NoMovement refusal when it is zero).
    // The caller applies it and calls RecordApplied.
    TrimEdit StopDynamic(const Document& document, const RationalTime& now);

    // Escape: drops the dynamic trim; nothing is planned, nothing written.
    // False when no dynamic trim was running (the window then leaves trim
    // mode with Clear).
    bool CancelDynamic();

    const DynamicTrimState& Dynamic() const { return dynamic_; }

    // `document` with the dynamic offset applied, for the window to draw
    // while J/K/L runs. A copy of `document` when no dynamic trim runs or
    // its offset is zero. False only when the offset no longer applies
    // (the document changed under the session).
    bool PreviewDocument(const Document& document, Document& preview) const;

private:
    TrimEdit PlanUniform(const Document& document,
                         const RationalTime& movement) const;
    bool OffsetApplies(const Document& document, int64_t frames) const;

    std::vector<TrimPoint> points_;
    TrimStyle style_ = TrimStyle::Ripple;
    bool follow_links_ = true;
    DynamicTrimState dynamic_;
};

// Plans moving each point by its own movement, against `document`: the
// engine of the three gestures above, exposed for tests and for a surface
// that wants an arbitrary trim of a selection. `movements` is index-aligned
// with `points`.
TrimEdit PlanTrim(const Document& document,
                  const std::vector<TrimPoint>& points,
                  const std::vector<RationalTime>& movements, TrimStyle style,
                  bool followLinks);

// One sequence frame as an exact time; false for a rate that is not a
// positive fraction.
bool SequenceFrame(const Document& document, RationalTime& frame);

// The time of the edge that moves, on the timeline: the incoming clip's
// start for a B side, the outgoing clip's end otherwise (else the incoming
// start, at a free head). They differ only once a plain trim has opened a
// gap between the two. False when the point names no clip the document
// holds.
bool EditPointTime(const Document& document, const TrimPoint& point,
                   RationalTime& time);

// FC4 (window) -- the side a click on a clip's edge selects: a roll when
// the press rolls (Command, as a Command-drag on an edge rolls in this
// window), else the outgoing side for a tail and the incoming one for a
// head.
TrimSide TrimSideOfEdge(TrimEdge edge, bool roll);

// FC4 (window) -- the edges the selected points hold, for the timeline to
// mark: a tail for an A side, a head for a B side, both for a roll. Each
// says whether its media is used up (the handle the trim would take is
// zero), as Resolve marks it red. Points the document no longer holds give
// nothing.
std::vector<TimelineEdgeMark> TrimSessionEdgeMarks(const Document& document,
                                                   const TrimSession& session);

// One side of the Trim Monitor.
struct TrimMonitorSide {
    bool present = false;
    Ulid clip_id;
    Ulid track_id;
    // The frame shown, on the timeline: the outgoing clip's last frame, the
    // incoming clip's first.
    RationalTime timeline_time{0, 1};
    // What the clip reads there, through THE RULE of ClipSpeed.h (speed and
    // ramp included). `has_source` is false for a caption clip, which reads
    // none, and when the time has no exact spelling.
    bool has_source = false;
    RationalTime source_time{0, 1};
    // The source frame at the source's own rate; -1 when the source has no
    // rate.
    int64_t source_frame = -1;
    // How much media is left beyond the edit on this side, in source time:
    // after the outgoing clip's Out, before the incoming clip's In.
    RationalTime handle{0, 1};
    // What a trim can still take, in whole sequence frames: the handle
    // through the clip's constant speed (35 frames for 70 of source at 2x).
    // -1 for a ramp, whose extension has no one speed, and a freeze.
    int64_t handle_frames = -1;
};

// What a Trim Monitor shows for one selected point.
struct TrimMonitorView {
    bool active = false;
    TrimSide side = TrimSide::Roll;
    TrimStyle style = TrimStyle::Ripple;
    size_t point_count = 0;
    RationalTime edit_time{0, 1};
    TrimMonitorSide outgoing;
    TrimMonitorSide incoming;
    // The counters, the dynamic offset included: whole sequence frames
    // (floor) and the exact time.
    RationalTime out_shift{0, 1};
    RationalTime in_shift{0, 1};
    int64_t out_shift_frames = 0;
    int64_t in_shift_frames = 0;
    DynamicTrimState dynamic;
};

// Pure: `shown` is the document the window draws -- PreviewDocument's
// while a dynamic trim runs, so that the last frame of A follows the moving
// edit. `index` picks the point; 0 is the primary one the monitor shows.
TrimMonitorView DescribeTrimMonitor(const Document& shown,
                                    const TrimSession& session,
                                    size_t index = 0);

// FC4 (window) -- where the Trim Monitor draws its two frames in the
// sequence frame, in sequence pixels: the last frame of A in the left half,
// the first of B in the right half, each picture fitted whole in its half
// (as Resolve's two-up, manual p. 1065-1067) and centred. Integer pixels:
// the renderer is handed the rectangles (Renderer.h, MonitorFraming) and
// decides nothing. A side without a picture (no clip, a caption, a size
// unknown) gets an empty pane.
struct TrimMonitorPane {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;

    bool empty() const { return width <= 0 || height <= 0; }
};

struct TrimMonitorLayout {
    TrimMonitorPane outgoing;
    TrimMonitorPane incoming;
};

// `outgoingWidth` and the others: the displayed size of each side's
// picture, rotation applied; zero or less for a side with none.
TrimMonitorLayout LayoutTrimMonitor(
    int32_t sequenceWidth, int32_t sequenceHeight, int64_t outgoingWidth,
    int64_t outgoingHeight, int64_t incomingWidth, int64_t incomingHeight);

// What the Trim Monitor writes, in French: over each half the side and the
// counter of frames trimmed since the point was selected (the numbers under
// each frame in Resolve's Trim Editor, manual p. 1068), signed; under it,
// what the trim can still take ("poignée"). Latin-1 only: the timeline's
// atlas draws no other glyph (MetalUiAtlas.h). The status line says the style,
// the side, how many points move together, and the dynamic trim's speed.
struct TrimMonitorText {
    std::string outgoing_title;
    std::string incoming_title;
    std::string outgoing_shift;
    std::string incoming_shift;
    std::string outgoing_handle;
    std::string incoming_handle;
    std::string status;
};

TrimMonitorText DescribeTrimMonitorText(const TrimMonitorView& view);

// A signed frame count as the counters write it: "+3", "-12", "0".
std::string SignedFrames(int64_t frames);

}  // namespace trim
