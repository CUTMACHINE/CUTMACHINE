#pragma once

// F2.1 -- ROADMAP.md: the single source of truth for the design system's
// palette, spacing scale and type scale. Plain C++, no AppKit -- everything
// here is a named constant or a pure function of one, so it is directly
// unit-testable (see tests/ui_theme_tests.cc) without a macOS toolchain.
//
// The named values are the C++ delivery from the 2026-08 ATELIER design
// pass. Keeping them here makes the AppKit chrome and Metal display lists
// projections of one palette instead of independent approximations.
//
// Cocoa-facing code converts these into NSColor/NSFont through
// UiThemeAppKit.h rather than re-deriving literals.

namespace ui::theme {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// ---- Surfaces (dark, cool industrial grays, darkest to lightest) ----
inline constexpr Color kSurfaceBase{0.047f, 0.051f, 0.055f, 1.0f};
inline constexpr Color kSurfacePanel{0.082f, 0.094f, 0.106f, 1.0f};
inline constexpr Color kSurfaceRaised{0.114f, 0.125f, 0.137f, 1.0f};
inline constexpr Color kSurfaceControl{0.149f, 0.161f, 0.176f, 1.0f};
inline constexpr Color kSurfaceControlHi{0.243f, 0.263f, 0.282f, 1.0f};
inline constexpr Color kSurfaceSunken{0.031f, 0.035f, 0.035f, 1.0f};
inline constexpr Color kSurfaceLane{0.039f, 0.043f, 0.047f, 1.0f};

// Alternating rows remain named because NSTableView consumes them directly.
// Their two-percent separation is deliberately quieter than a border.
inline constexpr Color kSurfaceRowEven = kSurfacePanel;
inline constexpr Color kSurfaceRowOdd{0.086f, 0.098f, 0.110f, 1.0f};
inline constexpr Color kSurfaceControlActive = kSurfaceControlHi;

// ---- Separation and relief ----
inline constexpr Color kSeparator{0.000f, 0.000f, 0.000f, 1.0f};
inline constexpr Color kBorderSubtle{0.133f, 0.149f, 0.165f, 1.0f};
inline constexpr Color kBorderStrong{0.200f, 0.216f, 0.231f, 1.0f};
inline constexpr Color kEdgeLight{0.290f, 0.310f, 0.329f, 1.0f};
inline constexpr Color kLaneGrid{0.078f, 0.094f, 0.106f, 1.0f};

// ---- Text ----
inline constexpr Color kTextPrimary{0.910f, 0.894f, 0.863f, 1.0f};
inline constexpr Color kTextSecondary{0.557f, 0.580f, 0.561f, 1.0f};
inline constexpr Color kTextTertiary{0.486f, 0.514f, 0.533f, 1.0f};
inline constexpr Color kTextOnAccent{0.047f, 0.051f, 0.055f, 1.0f};
inline constexpr Color kTextClip{0.957f, 0.945f, 0.918f, 1.0f};

// ---- One interaction accent ----
inline constexpr Color kAccent{1.000f, 0.353f, 0.122f, 1.0f};
inline constexpr Color kAccentHi{1.000f, 0.580f, 0.400f, 1.0f};
inline constexpr Color kAccentLo{0.902f, 0.290f, 0.071f, 1.0f};

// ---- Semantic colors (information, never generic interaction chrome) ----
inline constexpr Color kMarkIn{0.545f, 0.831f, 0.314f, 1.0f};
inline constexpr Color kMarkOut = kTextPrimary;
inline constexpr Color kRenderCached{0.180f, 0.431f, 0.322f, 1.0f};
inline constexpr Color kRenderStale{0.541f, 0.353f, 0.133f, 1.0f};
inline constexpr Color kError{0.769f, 0.212f, 0.165f, 1.0f};
// D2 -- docs/roadmap/DIALOGUE-2026-10.md. A marker carries its own colour in
// the document; this is the one it gets when that colour cannot be read, and
// it is the document's default (#f5c542) so both look the same.
inline constexpr Color kMarker{0.961f, 0.773f, 0.259f, 1.0f};

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. A selected edit point, as
// Resolve marks one: green while media is left beyond it, red once the
// media is used up (Resolve 21.1 manual, p. 1089). The In mark's green and
// the error red: no new hue.
inline constexpr Color kTrimPoint = kMarkIn;
inline constexpr Color kTrimPointAtLimit = kError;

// ---- Track families ----
inline constexpr Color kTrackVideo{0.200f, 0.220f, 0.239f, 1.0f};
inline constexpr Color kTrackVideoAlt{0.180f, 0.198f, 0.216f, 1.0f};
inline constexpr Color kTrackAudio{0.235f, 0.373f, 0.271f, 1.0f};
inline constexpr Color kTrackAudioAlt{0.200f, 0.310f, 0.259f, 1.0f};
// REMOTION-PARITY-2026-09 REM5 -- a motion clip on the timeline: the same
// dark weight as a shot, a violet cast that no shot takes, so that a title
// reads as a title before its name does.
inline constexpr Color kTrackMotion{0.290f, 0.235f, 0.380f, 1.0f};
inline constexpr Color kTrackMusic{0.243f, 0.251f, 0.220f, 1.0f};

// ---- Spacing scale (points) ----
inline constexpr double kSpaceXxs = 2.0;
inline constexpr double kSpaceXs = 4.0;
inline constexpr double kSpaceS = 8.0;
inline constexpr double kSpaceM = 12.0;
inline constexpr double kSpaceL = 16.0;
inline constexpr double kSpaceXl = 24.0;
inline constexpr double kSpaceXxl = 32.0;

// ---- Type scale (point sizes) ----
inline constexpr double kFontSizeCaption = 10.0;
inline constexpr double kFontSizeSmall = 11.0;
inline constexpr double kFontSizeBody = 12.0;
inline constexpr double kFontSizeSection = 13.0;
inline constexpr double kFontSizeTitle = 15.0;
inline constexpr double kFontSizeMonitorTimecode = 32.0;

// ACP8 -- docs/roadmap/ACP-2026-09.md. Conversation text needs room to
// read as prose, while the optional machine report stays monospaced.
inline constexpr double kChatBodyFontSize = kFontSizeSection;
inline constexpr double kChatAccentWidth = 3.0;
inline constexpr double kChatImageMaxHeight = 240.0;

// ---- Fixed component metrics ----
inline constexpr double kPanelHeaderHeight = 28.0;
inline constexpr double kControlRowHeight = 24.0;
inline constexpr double kTabStripHeight = 26.0;
inline constexpr double kCornerRadius = 4.0;
inline constexpr double kTimelineToolbarHeight = 26.0;
inline constexpr double kTimelineRulerHeight = 28.0;
inline constexpr double kTimelineRenderBandHeight = 5.0;
inline constexpr double kTimelineTrackHeight = 44.0;
inline constexpr double kTimelineTrackHeaderWidth = 96.0;
inline constexpr double kTimelineZoomBarHeight = 14.0;
inline constexpr double kTimelinePlayheadWidth = 1.0;
inline constexpr double kTimelineClipNameMinWidth = 40.0;

// GR4, GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The colour wheels and
// the curve editor of the Inspector. The channel colours name a channel
// wherever one is drawn (a curve, a field's underline); the hue ring of a
// wheel is not here: it is computed, a measurement and not a palette
// (GradeWheelModel.h).
inline constexpr Color kChannelRed{0.910f, 0.310f, 0.275f, 1.0f};
inline constexpr Color kChannelGreen{0.420f, 0.780f, 0.380f, 1.0f};
inline constexpr Color kChannelBlue{0.330f, 0.560f, 0.960f, 1.0f};
inline constexpr Color kChannelMaster = kTextPrimary;
inline constexpr double kGradeWheelDiameter = 104.0;
inline constexpr double kGradeWheelRingWidth = 5.0;
inline constexpr double kGradeWheelPuckRadius = 3.5;
inline constexpr double kGradeWheelTitleHeight = 18.0;
inline constexpr double kGradeWheelFieldHeight = 18.0;
inline constexpr double kGradeWheelMasterHeight = 10.0;
inline constexpr double kGradeCurveGraphHeight = 200.0;
inline constexpr double kGradeCurveBarHeight = 22.0;
inline constexpr double kGradeCurvePointRadius = 3.5;
// ON4 -- each half keeps room for the curve bar and a pair of wheels.
inline constexpr double kGradeWorkspaceColumnMinWidth = 320.0;
// GR8 -- the hue curves: shorter than the tone curve, whose y runs the whole
// range; six colour patches under the bar.
inline constexpr double kGradeHueCurveGraphHeight = 130.0;
inline constexpr double kGradeHuePatchSize = 14.0;

// PRD24 -- ROADMAP.md. What a clip carries, said on its rectangle
// (ClipBadges.h decides which badges and where): a dark plate and a
// caption-size label each, on one row between the top of the clip and its
// name. Calmer than the red of a sync drift on purpose -- a drift warns, a
// badge informs, and selection stays the only emphasis a clip gets.
inline constexpr Color kClipBadgeFill{0.000f, 0.000f, 0.000f, 0.600f};
inline constexpr Color kClipBadgeText = kTextClip;
// The veil over an angle a multicam group keeps off screen: the lane's own
// colour at half strength, so the clip sinks halfway towards the empty
// track -- it passes neither for an ordinary clip nor for no clip, and its
// name stays readable (tests/ui_theme_tests.cc holds all three).
inline constexpr Color kClipHiddenAngleVeil{kSurfaceLane.r, kSurfaceLane.g,
                                            kSurfaceLane.b, 0.500f};
// Out of lot -- the author, 29 October 2026: a disabled clip (ClipEnabled.h)
// "is greyed", as Resolve greys it. Its body loses the hue that tells a shot, a
// sound and a title apart and sinks below every ordinary body, its waveform
// turns the same grey and its name the secondary text, so that a clip taken
// out of the film reads as out of it before its name does
// (tests/ui_theme_tests.cc holds the bounds).
inline constexpr Color kClipDisabled{0.115f, 0.115f, 0.115f, 1.0f};
inline constexpr Color kClipDisabledWaveform{0.450f, 0.450f, 0.450f, 0.600f};
inline constexpr Color kClipDisabledText = kTextSecondary;
// Offsets from TimelineClipRect::y, the origin timelineRenderData draws a
// clip from: the body it fills, the strip its name is written on, and the
// badge row, which is the row of the sync badge and stays above that strip.
inline constexpr double kTimelineClipBodyTop = 2.0;
inline constexpr double kTimelineClipBodyHeight = 40.0;
inline constexpr double kTimelineClipNameBandTop = 28.0;
// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. The Retime controls a clip
// shows (Command-R): a band along the top of its body with its speed, whose
// two ends stretch it (Resolve 21.1 manual, p. 1218-1219). The badge's
// black, the interaction accent for the ends: no new hue.
inline constexpr double kTimelineRetimeBandHeight = 12.0;
inline constexpr Color kRetimeBand = kClipBadgeFill;
inline constexpr Color kRetimeHandle = kAccent;
// How far under the body the name's glyph cells reach: its descenders hang
// a point below, and the veil of a hidden angle has to dim them too (seen
// on screen: the tail of a "g" stayed bright under a dimmed name).
inline constexpr double kTimelineClipNameOverhang = 2.0;
inline constexpr double kTimelineClipBadgeTop = 7.0;
inline constexpr double kTimelineClipBadgeHeight = 11.0;
// Along the row: from the clip's left edge to the first plate (the inset of
// the clip's name), from the last plate to the clip's right edge, between
// two plates, and between a plate's edge and its label.
inline constexpr double kTimelineClipBadgeInset = 5.0;
inline constexpr double kTimelineClipBadgeEndInset = 2.0;
inline constexpr double kTimelineClipBadgeGap = 3.0;
inline constexpr double kTimelineClipBadgePadding = 2.0;
// The label is drawn in the atlas's monospaced bold face at
// kFontSizeCaption. Its advance is 6.18 points a glyph (measured with Core
// Text on macOS 26; the smoke test holds this constant to the real font).
// The atlas pads a glyph cell by one point on the left and puts that face's
// capitals 3.8 points under the cell's top, 7 points tall: the label's quad
// therefore starts one point right of the plate's edge and two points above
// it, which leaves the capitals in the middle of the plate both ways.
inline constexpr double kTimelineClipBadgeGlyphAdvance = 6.2;
inline constexpr double kTimelineClipBadgeTextInset = 1.0;
inline constexpr double kTimelineClipBadgeTextRise = 2.0;

// PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The agent's mark: the
// plate of the badge a clip carries while it is one the agent's last call
// created or changed, its label in kTextOnAccent, and the strip of the
// agent's lines in the Agent panel -- one colour for one agent, wherever it
// is said. A light aqua no other mark of the window takes: neither the
// accent (selection, interaction), nor the red of an error or of a sync
// drift, nor the yellow of a marker, nor a track's colour, nor the blue of a
// curve. Opaque, unlike the dark plates of the other badges, and light
// enough to stand out of the lightest clip body: this one asks to be looked
// at, until the person acts in the timeline (tests/ui_theme_tests.cc holds
// its contrasts).
inline constexpr Color kAgentMark{0.451f, 0.902f, 0.878f, 1.0f};

// PRD37 -- ROADMAP.md. A row of the lists the panels draw themselves (the
// bins, the media browser's list: CMIndustrialTableRowView in main.mm):
// selected, it takes this surface and its label the primary text. The bin
// list kept AppKit's blue selection under a label left in the secondary
// grey, a 2.05 contrast measured by the QA of 3 October 2026; both values
// hold 4.5:1 or more (tests/ui_theme_tests.cc).
inline constexpr Color kListRowSelected = kSurfaceControl;
constexpr Color ListRowLabel(bool selected) {
    return selected ? kTextPrimary : kTextSecondary;
}

// Returns `color` with its alpha replaced by `alpha`; RGB unchanged.
constexpr Color WithAlpha(Color color, float alpha) {
    return Color{color.r, color.g, color.b, alpha};
}

// The tint used to mark a track/clip/badge as video or audio. Centralizes
// the ternary that used to be repeated at every one of its call sites in
// -[AppDelegate timelineRenderData].
constexpr Color TrackTint(bool isVideoTrack) {
    return isVideoTrack ? kTrackVideo : kTrackAudio;
}

// Linear interpolation between two colors, componentwise. `t` is not
// clamped -- callers pass values already known to be in [0, 1].
constexpr Color Mix(Color from, Color to, float t) {
    return Color{
        from.r + (to.r - from.r) * t,
        from.g + (to.g - from.g) * t,
        from.b + (to.b - from.b) * t,
        from.a + (to.a - from.a) * t,
    };
}

}  // namespace ui::theme
