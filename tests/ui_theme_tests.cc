// Pure-C++ test for the design system's token file (ROADMAP.md F2.1).
// UiTheme.h has no AppKit dependency, so this builds and runs on a plain
// Linux host, the same way tests/fft_tests.cc does.

#include "UiTheme.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <string>
#include <vector>

namespace {

using ui::theme::Color;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) std::cout << "PASS: " << name << '\n';
}

bool InUnitRange(float value) { return value >= 0.0f && value <= 1.0f; }

bool IsValidColor(const Color& color) {
    return InUnitRange(color.r) && InUnitRange(color.g) &&
           InUnitRange(color.b) && InUnitRange(color.a);
}

// `over` laid on `under` at its own alpha, as the display list blends a
// rectangle: on the encoded values.
Color Over(const Color& over, const Color& under) {
    return ui::theme::Mix(Color{under.r, under.g, under.b, 1.0f},
                          Color{over.r, over.g, over.b, 1.0f}, over.a);
}

// The luminance of a colour as the timeline shows it, and WCAG's contrast
// of two. The Metal layers are tagged BT.709 at a pure 2.4 gamma
// (Renderer.mm), not sRGB: a dark token comes out darker on screen than its
// sRGB reading says, which is what decides how far a veil may go.
double Luminance(const Color& color) {
    const auto linear = [](float channel) { return std::pow(channel, 2.4); };
    return 0.2126 * linear(color.r) + 0.7152 * linear(color.g) +
           0.0722 * linear(color.b);
}

double Contrast(const Color& left, const Color& right) {
    const double lighter = std::max(Luminance(left), Luminance(right));
    const double darker = std::min(Luminance(left), Luminance(right));
    return (lighter + 0.05) / (darker + 0.05);
}

// A clip's body as timelineRenderData fills it: the track colour, up to
// 0.11 lighter at the top of its gradient and 0.08 darker at the bottom.
Color Shifted(const Color& color, float by) {
    const auto clamp = [](float value) {
        return std::min(1.0f, std::max(0.0f, value));
    };
    return {clamp(color.r + by), clamp(color.g + by), clamp(color.b + by),
            1.0f};
}

}  // namespace

int main() {
    Test("every named surface/border/text/accent token is a valid color", [] {
        const std::vector<Color> tokens{
            ui::theme::kSurfaceBase,      ui::theme::kSurfacePanel,
            ui::theme::kSurfaceRaised,    ui::theme::kSurfaceRowEven,
            ui::theme::kSurfaceRowOdd,    ui::theme::kSurfaceControl,
            ui::theme::kSurfaceControlHi, ui::theme::kSurfaceControlActive,
            ui::theme::kSurfaceSunken,    ui::theme::kSurfaceLane,
            ui::theme::kSeparator,        ui::theme::kBorderSubtle,
            ui::theme::kBorderStrong,     ui::theme::kEdgeLight,
            ui::theme::kLaneGrid,         ui::theme::kTextPrimary,
            ui::theme::kTextSecondary,    ui::theme::kTextTertiary,
            ui::theme::kTextOnAccent,     ui::theme::kTextClip,
            ui::theme::kAccent,           ui::theme::kAccentHi,
            ui::theme::kAccentLo,         ui::theme::kMarkIn,
            ui::theme::kMarkOut,          ui::theme::kRenderCached,
            ui::theme::kRenderStale,      ui::theme::kError,
            ui::theme::kTrackVideo,       ui::theme::kTrackVideoAlt,
            ui::theme::kTrackAudio,       ui::theme::kTrackAudioAlt,
            ui::theme::kTrackMusic,       ui::theme::kMarker,
            ui::theme::kTrackMotion,      ui::theme::kClipBadgeFill,
            ui::theme::kClipBadgeText,    ui::theme::kClipHiddenAngleVeil,
            ui::theme::kListRowSelected,  ui::theme::kAgentMark,
            ui::theme::kClipDisabled,     ui::theme::kClipDisabledWaveform,
            ui::theme::kClipDisabledText,
        };
        for (const Color& token : tokens)
            Check(IsValidColor(token), "token channel outside [0, 1]");
    });

    Test("surfaces get progressively lighter, darkest to lightest", [] {
        const auto luma = [](const Color& color) {
            return color.r + color.g + color.b;
        };
        Check(luma(ui::theme::kSurfaceBase) < luma(ui::theme::kSurfacePanel),
              "panel should be lighter than the base window surface");
        Check(luma(ui::theme::kSurfacePanel) < luma(ui::theme::kSurfaceRaised),
              "raised surfaces (toolbars/rulers) should read above panels");
        Check(
            luma(ui::theme::kSurfaceRaised) < luma(ui::theme::kSurfaceControl),
            "interactive control wells should stand out from raised chrome");
    });

    Test("text hierarchy goes primary > secondary > tertiary in luminance", [] {
        const auto luma = [](const Color& color) {
            return color.r + color.g + color.b;
        };
        Check(luma(ui::theme::kTextSecondary) < luma(ui::theme::kTextPrimary),
              "secondary text should be dimmer than primary");
        Check(luma(ui::theme::kTextTertiary) < luma(ui::theme::kTextSecondary),
              "tertiary text should be dimmer than secondary");
    });

    Test("WithAlpha changes only the alpha channel", [] {
        const Color faded = ui::theme::WithAlpha(ui::theme::kAccent, 0.3f);
        Check(faded.r == ui::theme::kAccent.r &&
                  faded.g == ui::theme::kAccent.g &&
                  faded.b == ui::theme::kAccent.b,
              "RGB must be unchanged");
        Check(faded.a == 0.3f, "alpha must be replaced");
    });

    Test("TrackTint distinguishes video from audio", [] {
        const Color video = ui::theme::TrackTint(true);
        const Color audio = ui::theme::TrackTint(false);
        Check(video.r == ui::theme::kTrackVideo.r &&
                  video.g == ui::theme::kTrackVideo.g &&
                  video.b == ui::theme::kTrackVideo.b,
              "video tint should match kTrackVideo");
        Check(audio.r == ui::theme::kTrackAudio.r &&
                  audio.g == ui::theme::kTrackAudio.g &&
                  audio.b == ui::theme::kTrackAudio.b,
              "audio tint should match kTrackAudio");
        Check(!(video.r == audio.r && video.g == audio.g && video.b == audio.b),
              "video and audio tints must be visually distinguishable");
    });

    Test("Mix interpolates and the endpoints match", [] {
        const auto nearlyEqual = [](float a, float b) {
            return std::abs(a - b) < 1e-6f;
        };
        const Color start = ui::theme::kSurfaceBase;
        const Color end = ui::theme::kAccent;
        const Color atStart = ui::theme::Mix(start, end, 0.0f);
        const Color atEnd = ui::theme::Mix(start, end, 1.0f);
        Check(atStart.r == start.r && atStart.g == start.g &&
                  atStart.b == start.b,
              "t=0 should return the start color exactly");
        Check(nearlyEqual(atEnd.r, end.r) && nearlyEqual(atEnd.g, end.g) &&
                  nearlyEqual(atEnd.b, end.b),
              "t=1 should return the end color, within float rounding");
        const Color mid = ui::theme::Mix(start, end, 0.5f);
        Check(mid.r > std::min(start.r, end.r) &&
                  mid.r < std::max(start.r, end.r),
              "t=0.5 should land strictly between the endpoints");
    });

    Test("the spacing scale is strictly increasing", [] {
        const std::vector<double> spacing{
            ui::theme::kSpaceXxs, ui::theme::kSpaceXs, ui::theme::kSpaceS,
            ui::theme::kSpaceM,   ui::theme::kSpaceL,  ui::theme::kSpaceXl,
            ui::theme::kSpaceXxl,
        };
        for (size_t index = 1; index < spacing.size(); ++index)
            Check(spacing[index - 1] < spacing[index],
                  "spacing scale must be strictly increasing");
    });

    Test("the type scale is strictly increasing", [] {
        const std::vector<double> sizes{
            ui::theme::kFontSizeCaption, ui::theme::kFontSizeSmall,
            ui::theme::kFontSizeBody,    ui::theme::kFontSizeSection,
            ui::theme::kFontSizeTitle,
        };
        for (size_t index = 1; index < sizes.size(); ++index)
            Check(sizes[index - 1] < sizes[index],
                  "type scale must be strictly increasing");
    });

    Test("ATELIER timeline geometry stays on the delivered point grid", [] {
        Check(ui::theme::kTimelineToolbarHeight == 26.0,
              "toolbar height should match the delivered geometry");
        Check(ui::theme::kTimelineRulerHeight == 28.0,
              "ruler height should match the delivered geometry");
        Check(ui::theme::kTimelineTrackHeight == 44.0,
              "track height should match the delivered geometry");
        Check(ui::theme::kTimelineTrackHeaderWidth == 96.0,
              "track header width should match the delivered geometry");
        Check(ui::theme::kTimelineZoomBarHeight == 14.0,
              "zoom bar height should match the delivered geometry");
        Check(ui::theme::kTimelinePlayheadWidth == 1.0,
              "the playhead must remain a single unshadowed point line");
    });

    // PRD24 -- ROADMAP.md. The badges a clip carries on the timeline.
    Test("a clip badge reads on every kind of clip", [] {
        // The lightest a body gets behind the badge row.
        for (const Color& track :
             {ui::theme::kTrackVideo, ui::theme::kTrackAudio,
              ui::theme::kTrackMotion, ui::theme::kTrackMusic}) {
            const Color plate =
                Over(ui::theme::kClipBadgeFill, Shifted(track, 0.11f));
            Check(Contrast(ui::theme::kClipBadgeText, plate) >= 7.0,
                  "a badge's label needs a 7:1 contrast on its plate");
        }
        Check(ui::theme::kClipBadgeFill.a > 0.0f &&
                  ui::theme::kClipBadgeFill.a < 1.0f,
              "the plate lets the clip's own colour through");
    });

    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The agent's mark.
    Test("the agent's mark reads, and reads as no other mark", [] {
        const Color mark = ui::theme::kAgentMark;
        Check(mark.a == 1.0f, "an opaque plate: it asks to be looked at");
        Check(Contrast(ui::theme::kTextOnAccent, mark) >= 7.0,
              "its label needs a 7:1 contrast on its plate");
        for (const Color& track :
             {ui::theme::kTrackVideo, ui::theme::kTrackAudio,
              ui::theme::kTrackMotion, ui::theme::kTrackMusic})
            Check(Contrast(mark, Shifted(track, 0.11f)) >= 3.0,
                  "the plate stands out of every kind of clip");
        Check(Contrast(mark, ui::theme::kSurfacePanel) >= 3.0,
              "and the panel's strip out of the panel");
        // Told apart from what the window already marks in colour: hue, not
        // only lightness, separates them.
        const auto distance = [](const Color& left, const Color& right) {
            return std::abs(left.r - right.r) + std::abs(left.g - right.g) +
                   std::abs(left.b - right.b);
        };
        for (const Color& other :
             {ui::theme::kAccent, ui::theme::kAccentHi, ui::theme::kError,
              ui::theme::kMarker, ui::theme::kMarkIn, ui::theme::kChannelBlue,
              ui::theme::kRenderCached})
            Check(distance(mark, other) >= 0.4,
                  "the agent's colour is no other mark's");
    });

    Test("a hidden angle is dimmed, neither ordinary nor gone", [] {
        const Color veil = ui::theme::kClipHiddenAngleVeil;
        Check(veil.a > 0.0f && veil.a < 1.0f, "a veil, not a cover");
        const Color body = Shifted(ui::theme::kTrackVideo, 0.08f);
        const Color dimmed = Over(veil, body);
        // Clip bodies are dark to begin with -- 2.1:1 against the lane at
        // their lightest -- so the two bounds below leave the veil a narrow
        // band (a strength of 0.26 to 0.57). Being told from an ordinary
        // clip comes first: that is the mistake the screen showed.
        Check(Contrast(body, dimmed) >= 1.3,
              "a dimmed clip must not pass for an ordinary one");
        Check(Contrast(dimmed, ui::theme::kSurfaceLane) >= 1.2,
              "a dimmed clip must not pass for an empty lane");
        // Its name, on the darkened strip timelineRenderData writes it on
        // (black at 0.40 over the foot of the body).
        const Color strip = Over(Color{0.0f, 0.0f, 0.0f, 0.40f},
                                 Shifted(ui::theme::kTrackVideo, -0.05f));
        Check(Contrast(Over(veil, ui::theme::kTextClip), Over(veil, strip)) >=
                  3.0,
              "a dimmed clip's name stays readable");
    });

    // Hors lot -- l'auteur, 29 octobre 2026 : un plan désactivé est grisé.
    Test("a disabled clip is greyed, neither ordinary nor gone", [] {
        const Color grey = ui::theme::kClipDisabled;
        Check(grey.r == grey.g && grey.g == grey.b,
              "a disabled clip has no hue: shot, sound and title alike");
        // The lightest shade of each body, as the hidden angle's test reads
        // it: the top of timelineRenderData's gradient. A shot or a sound
        // is its track's colour moved by up to 0.03 per source (ClipColor
        // in main.mm): the darkest of them is the one to tell apart.
        const Color disabled = Shifted(grey, 0.08f);
        for (const Color& track :
             {Shifted(ui::theme::kTrackVideo, -0.03f),
              Shifted(ui::theme::kTrackAudio, -0.03f), ui::theme::kTrackMotion})
            Check(Contrast(Shifted(track, 0.08f), disabled) >= 1.3,
                  "a disabled clip must not pass for an ordinary one");
        Check(Contrast(disabled, ui::theme::kSurfaceLane) >= 1.2,
              "a disabled clip must not pass for an empty lane");
        const Color strip =
            Over(Color{0.0f, 0.0f, 0.0f, 0.40f}, Shifted(grey, -0.05f));
        Check(Contrast(ui::theme::kClipDisabledText, strip) >= 3.0,
              "a disabled clip's name stays readable");
        const Color wave = ui::theme::kClipDisabledWaveform;
        Check(wave.r == wave.g && wave.g == wave.b && wave.a < 1.0f,
              "its waveform is greyed with it");
        Check(Contrast(Over(wave, disabled), disabled) >= 1.5,
              "its waveform stays visible on it");
    });

    Test("a clip is drawn where it was before it had tokens", [] {
        // timelineRenderData spelled these three as literals; the tokens
        // name them so that the badge row and the veil can be held to them.
        // Another value here moves every clip of every timeline.
        Check(ui::theme::kTimelineClipBodyTop == 2.0 &&
                  ui::theme::kTimelineClipBodyHeight == 40.0 &&
                  ui::theme::kTimelineClipNameBandTop == 28.0,
              "the clip's body and the strip of its name keep their place");
    });

    Test("the badge row lies inside the clip's body, above its name", [] {
        Check(ui::theme::kTimelineClipBodyTop +
                      ui::theme::kTimelineClipBodyHeight <=
                  ui::theme::kTimelineTrackHeight,
              "a clip's body fits its track");
        Check(
            ui::theme::kTimelineClipBadgeTop >= ui::theme::kTimelineClipBodyTop,
            "the row starts inside the body");
        Check(ui::theme::kTimelineClipBadgeTop +
                      ui::theme::kTimelineClipBadgeHeight <=
                  ui::theme::kTimelineClipNameBandTop,
              "the row ends above the name's strip");
        Check(ui::theme::kTimelineClipNameBandTop <
                  ui::theme::kTimelineClipBodyTop +
                      ui::theme::kTimelineClipBodyHeight,
              "the name's strip is inside the body");
        // The veil of a hidden angle runs that much under the body: enough
        // for a descender, short of a clip's body on the track below.
        Check(ui::theme::kTimelineClipNameOverhang >= 1.0 &&
                  ui::theme::kTimelineClipBodyTop +
                          ui::theme::kTimelineClipBodyHeight +
                          ui::theme::kTimelineClipNameOverhang <
                      ui::theme::kTimelineTrackHeight +
                          ui::theme::kTimelineClipBodyTop,
              "the name's overhang stays between two tracks' clips");
    });

    Test("a badge's label fits its plate", [] {
        // The atlas pads a glyph cell by one point (MetalUiAtlas.mm): the
        // first glyph then starts one padding inside the plate, as the last
        // one ends.
        Check(ui::theme::kTimelineClipBadgeTextInset + 1.0 ==
                  ui::theme::kTimelineClipBadgePadding,
              "the label is centred across its plate");
        // A monospaced face advances by about six tenths of its size.
        Check(ui::theme::kTimelineClipBadgeGlyphAdvance >=
                      0.55 * ui::theme::kFontSizeCaption &&
                  ui::theme::kTimelineClipBadgeGlyphAdvance <=
                      0.70 * ui::theme::kFontSizeCaption,
              "the advance is that of the caption face");
        // Its capitals are about seven tenths of its size tall.
        Check(ui::theme::kTimelineClipBadgeHeight >=
                  0.7 * ui::theme::kFontSizeCaption + 2.0,
              "the plate clears the capitals by a point above and below");
        Check(ui::theme::kTimelineClipBadgeTextRise > 0.0 &&
                  ui::theme::kTimelineClipBadgeTextRise <
                      ui::theme::kTimelineClipBadgeTop,
              "the label's quad starts above the plate, inside the clip");
    });

    // PRD37 -- ROADMAP.md. The QA of 3 October 2026 measured the selected
    // bin's label at 2.05:1 on AppKit's blue selection.
    Test("a list row's label reads, selected or not", [] {
        Check(Contrast(ui::theme::ListRowLabel(true),
                       ui::theme::kListRowSelected) >= 4.5,
              "a selected row's label needs 4.5:1 on the selection");
        Check(Contrast(ui::theme::ListRowLabel(false),
                       ui::theme::kSurfacePanel) >= 4.5,
              "and an unselected one on the panel");
        Check(Luminance(ui::theme::ListRowLabel(true)) >
                  Luminance(ui::theme::ListRowLabel(false)),
              "selection brightens the label: the row says it twice");
        Check(Luminance(ui::theme::kListRowSelected) >
                  Luminance(ui::theme::kSurfacePanel),
              "the selected row stands out of the panel");
    });

    Test("a clip wide enough for its name has room for its speed", [] {
        Check(ui::theme::kTimelineClipBadgeInset > 3.0,
              "the row clears the three-point highlight of a clip's head");
        Check(ui::theme::kTimelineClipBadgeGap > 0.0 &&
                  ui::theme::kTimelineClipBadgePadding > 0.0 &&
                  ui::theme::kTimelineClipBadgeEndInset > 0.0,
              "plates neither touch each other nor the clip's edge");
        // "50%", "25%", "2x": three glyphs at most for the usual speeds.
        const double speed = ui::theme::kTimelineClipBadgeInset +
                             3.0 * ui::theme::kTimelineClipBadgeGlyphAdvance +
                             2.0 * ui::theme::kTimelineClipBadgePadding +
                             ui::theme::kTimelineClipBadgeEndInset;
        Check(speed <= ui::theme::kTimelineClipNameMinWidth,
              "a three-glyph speed fits the narrowest clip that is named");
    });

    return failures == 0 ? 0 : 1;
}
