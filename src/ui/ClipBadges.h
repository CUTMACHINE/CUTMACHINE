#pragma once

// PRD24 -- ROADMAP.md. What a clip carries that its rectangle on the timeline
// did not say.
//
// An agent does most of the editing here, through the engine, and the window
// is where its work is read back. The timeline drew a shot slowed to 1/2, a
// reframed shot, an animated one, a graded one and the angle a multicam group
// keeps off screen exactly like a shot nothing was done to: telling them
// apart meant selecting each clip in turn and reading the Inspector.
//
// This file decides what a clip's rectangle says -- which badges, in which
// order, what each one reads -- and where they sit in it, so that
// -[AppDelegate timelineRenderData] only draws. It is portable C++ on
// purpose: the display list is reached by the window test alone, while this
// is tested on any host (tests/clip_badges_tests.cc).
//
// Derived, never stored. Every answer is read from the document as it
// stands, through the predicates the engine already has (ClipSpeed.h,
// Document.h, Multicam.h): nothing is written to the document or to the
// preferences, and an undo takes a badge away because it took the speed
// away. PRD25 -- one badge is the exception, and is not stored either: the
// agent's mark, which the window's journal of the agent's calls hands in
// (AgentJournal.h), in memory, until the person acts in the timeline.
//
// Not clickable. A badge lies inside its clip's rectangle, and a click on it
// is a click on the clip: TimelineView.cc's hit test never hears of badges.

#include "Document.h"
#include "UiTheme.h"

#include <cstdint>
#include <string>
#include <vector>

// In priority order: when a clip is too narrow for all it carries, the last
// of this list goes first and the speed stays longest. The speed changes
// what the shot lasts and is the only badge that states a value; a motion
// clip is another kind of clip altogether; an animation moves over time; a
// framing holds still; and in a graded film every shot is graded, so that
// badge tells the least.
//
// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The agent's mark comes
// before all of them: it is what the person is asked to look at now, and it
// goes as soon as they act in the timeline, giving the row back to the rest.
//
// DEC36 -- docs/roadmap/GENERATIF-2026-10.md. A shot whose picture a
// generation service made comes next, before the speed: it is the one fact
// about a shot that changes what the film claims to have filmed, so a
// narrow clip gives up its speed before it gives up this. Derived like the
// others, from the media's provenance (ClipPlaysGeneratedMedia): an undo of
// the provenance takes the badge away, and nothing else can (EditLog.cc).
//
// Out of lot -- the author, 18 November 2026 (GitHub #330). An audio clip's
// gain comes after the speed: the other badge that states a value, the one a
// person reviewing an agent's mix looks for first ("-18 dB" on the music),
// and nothing else said it on the timeline. Its fades are drawn on the
// body instead (TimelineView.h, VisibleTimelineAudioFades).
enum class ClipBadgeKind : std::uint8_t {
    Agent,
    Generated,
    Speed,
    Gain,
    Motion,
    Animation,
    Framing,
    Grade
};

struct ClipBadge {
    ClipBadgeKind kind = ClipBadgeKind::Speed;
    // What the badge reads, in glyphs the Metal atlas holds (Latin-1,
    // MetalUiAtlas.mm) and encoded as UTF-8.
    std::string label;
};

struct ClipSignage {
    // In the order of ClipBadgeKind, each kind at most once.
    std::vector<ClipBadge> badges;
    // F1.5 -- the clip is an angle a multicam group takes off screen
    // (Multicam.h). A state of the whole clip rather than a badge: the
    // timeline draws it dimmed, as it never reaches the picture.
    bool hidden_angle = false;
    // Out of lot -- the author, 29 October 2026. The clip is disabled
    // (ClipEnabled.h): a state of the whole clip too, drawn greyed, its
    // waveform and name with it. Its badges stay: what it carries comes
    // back with it.
    bool disabled = false;
};

// What `clip` carries, read from `document`; with `agentMarked`, the agent's
// last call created or changed it (AgentJournal::MarkedClips).
ClipSignage ClipSignageFor(const Document& document, const DocumentClip& clip,
                           bool agentMarked = false);

// The exact text of a speed, from its fraction and with no floating point:
// a whole multiple reads "2x", a whole percentage "50%" (1/2), "40%" (2/5),
// "150%" (3/2), and any other speed its reduced fraction, "4/3x". Empty for
// the unit speed, and for a fraction a document cannot carry (a zero or
// negative term), which is never divided by.
std::string ClipSpeedBadgeLabel(const EffectParamValue& speed);

// GitHub #330 -- the exact text of a clip's gain, from its fraction: whole
// decibels "-18 dB", "+3 dB", any other value to the nearest tenth, "-4.5 dB"
// (half away from zero). Empty for no gain, and for a fraction a document
// cannot carry (a zero or negative denominator).
std::string ClipGainBadgeLabel(const EffectParamValue& gainDb);

// R3, criterion 4 -- a ramp's badge: its slowest and its fastest key, each
// spelled as above, the unit speed as "100%" and a stop as "0%" -- a ramp
// from 1 down to 1/4 and back reads "25%-100%". Empty without a ramp.
std::string ClipSpeedRampBadgeLabel(const DocumentClip& clip);

// What the badges of the other kinds read; empty for a speed and a gain,
// whose text depends on their value.
std::string ClipBadgeLabel(ClipBadgeKind kind);

// PRD25 -- the colours a badge of `kind` is drawn in: its plate, and its
// label. A dark plate and the clip's text for every badge but the agent's
// mark, which is the agent's colour under dark text (UiTheme.h).
ui::theme::Color ClipBadgeFillColor(ClipBadgeKind kind);
ui::theme::Color ClipBadgeTextColor(ClipBadgeKind kind);

// The number of glyphs the atlas draws for a UTF-8 label: its code points,
// not its bytes.
int ClipBadgeGlyphCount(const std::string& label);

// The width of a badge's plate for `label`.
double ClipBadgeWidth(const std::string& label);

// One badge as it is drawn: a plate, and the label handed to the atlas. `x`
// is measured from the left edge of what shows of the clip and `y` from
// TimelineClipRect::y -- the two origins timelineRenderData draws a clip
// from, the ones its name and its sync badge already use.
struct PlacedClipBadge {
    ClipBadgeKind kind = ClipBadgeKind::Speed;
    std::string label;
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
    // Where the label's quad starts, and the width the atlas may fill.
    double text_x = 0.0;
    double text_y = 0.0;
    double text_width = 0.0;
};

// PRD25 -- one plate of the badge row for `label`, its left edge `x` points
// from the left edge of what shows of the clip: its size, and where its
// label's quad starts, the capitals then in the middle of the plate both
// ways. The badges are placed with it, and so is the sync badge of a drifted
// sound, at the row's inset. That one used to be drawn with figures of its
// own, which put its label's quad two points under the plate's top instead
// of two above (kTimelineClipBadgeTextRise): the capitals ran over the
// plate's lower edge onto the waveform. `kind` is left at its default.
PlacedClipBadge PlaceClipPlate(const std::string& label, double x);

// The badges that fit in a clip `clipWidth` points wide, left to right in
// the order given. They sit on one row above the clip's name, which they
// can therefore never cover (UiTheme.h holds both offsets).
//
// The row keeps a strict prefix of `badges`: the first one that does not fit
// ends it, even when a shorter one behind it would have fitted. A reader can
// then trust what is absent -- a badge shown means every badge of a higher
// priority the clip carries is shown too. A clip too narrow for its first
// badge shows none.
//
// `leadingWidth` is what already stands at the start of that row, measured
// from the row's own left inset: the sync badge of a drifted audio clip
// ("+3f"). The row starts after it. Zero when nothing is there.
std::vector<PlacedClipBadge> PlaceClipBadges(
    const std::vector<ClipBadge>& badges, double clipWidth,
    double leadingWidth = 0.0);
