// R5 -- docs/roadmap/REEL-2026-09.md. Where a transition falls and what it
// is worth at every frame it covers.
//
// Portable on purpose (guard rail 1 of the lot): the monitor, the export,
// Document::Validate, verify_export and the two Resolve bridges all reach
// this arithmetic. Before this ticket the split around the cut was written
// out in four files and the fade curve in two, and nothing compared them.
// Everything here runs without AppKit, Metal or FFmpeg.

#include "Document.h"
#include "Timeline.h"
#include "TransitionRender.h"

#include <iostream>
#include <string>
#include <vector>

namespace {

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
    try {
        function();
        if (failures == before) std::cout << "PASS: " << name << '\n';
    } catch (const std::exception& exception) {
        ++failures;
        std::cerr << "FAIL: " << name << ": " << exception.what() << '\n';
    }
}

const Ulid kSource = "01K90000000000000000000001";
const Ulid kTrack = "01K90000000000000000000002";
const Ulid kFirst = "01K90000000000000000000003";
const Ulid kSecond = "01K90000000000000000000004";
const Ulid kTransition = "01K90000000000000000000005";
const Ulid kSequence = "01K90000000000000000000006";

// Two 100-frame shots abutting at frame 100, with 50 frames of handle on
// either side of the cut: enough for a dissolve, and irrelevant to a dip.
Document TwoShots() {
    Document document;
    document.sequence.id = kSequence;
    document.sequence.frame_rate = MediaRate{25, 1};
    DocumentSource source;
    source.id = kSource;
    source.path = "shot.mov";
    source.rate = MediaRate{25, 1};
    source.duration = RationalTime(400, 25);
    document.sources.push_back(source);
    DocumentTrack track;
    track.id = kTrack;
    track.kind = "video";
    track.index = 0;
    DocumentClip first;
    first.id = kFirst;
    first.source_id = kSource;
    first.source_in = RationalTime(50, 25);
    first.duration = RationalTime(100, 25);
    first.timeline_in = RationalTime(0, 25);
    DocumentClip second = first;
    second.id = kSecond;
    second.source_in = RationalTime(200, 25);
    second.timeline_in = RationalTime(100, 25);
    track.clips = {first, second};
    document.sequence.tracks.push_back(track);
    return document;
}

DocumentTransition Dip(const Ulid& left, const Ulid& right,
                       TransitionAlignment alignment, int64_t frames,
                       const std::string& color = "#000000") {
    DocumentTransition transition;
    transition.id = kTransition;
    transition.track_id = kTrack;
    transition.left_clip_id = left;
    transition.right_clip_id = right;
    transition.type = kTransitionDipToColor;
    transition.color = color;
    transition.duration = RationalTime(frames, 25);
    transition.alignment = alignment;
    return transition;
}

std::string Fraction(const EffectParamValue& value) {
    return std::to_string(value.num) + "/" + std::to_string(value.den);
}

// The alpha of every frame of a span, as fractions, so that a curve that
// moved shows as a list and not as one number.
std::vector<std::string> Curve(const TransitionSpan& span) {
    std::vector<std::string> values;
    for (int64_t index = 0; index < span.frames(); ++index)
        values.push_back(Fraction(ColorVeilAlpha(span, index)));
    return values;
}

bool Same(const std::vector<std::string>& left,
          const std::vector<std::string>& right) {
    return left == right;
}

}  // namespace

int main() {
    Test("a colour is #rrggbb and nothing else", [] {
        TransitionColor color;
        std::string error;
        Check(ParseTransitionColor("#000000", color, error) &&
                  color == TransitionColor{0, 0, 0},
              "black");
        Check(ParseTransitionColor("#FFFFFF", color, error) &&
                  color == TransitionColor{255, 255, 255},
              "white, upper case");
        Check(ParseTransitionColor("#bfbfbf", color, error) &&
                  color == TransitionColor{191, 191, 191},
              "the graphic white of an HLG delivery");
        Check(TransitionColorHex({191, 191, 191}) == "bfbfbf",
              "spelled back without its hash");
        for (const std::string& rejected : {"", "#fff", "ffffff", "#gggggg",
                                            "#ffffff ", "white", "#ffffffff"}) {
            Check(!ParseTransitionColor(rejected, color, error),
                  "refused: '" + rejected + "'");
        }
    });

    Test("the span of a centred dissolve splits floor then ceil", [] {
        Document document = TwoShots();
        DocumentTransition transition =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 11);
        transition.type = kTransitionCrossDissolve;
        transition.color = "#000000";
        TransitionSpan span;
        std::string error;
        Check(ResolveTransitionSpan(document.sequence, transition, span, error),
              "resolved: " + error);
        Check(span.cut_frame == 100, "cut at 100");
        Check(span.pre_frames == 5 && span.post_frames == 6,
              "11 frames split 5 then 6");
        Check(span.first_frame() == 95 && span.end_frame() == 106,
              "95 to 106 exclusive");
    });

    Test("a dissolve's curve is k over n, exact", [] {
        Check(Fraction(CrossDissolveProgress(0, 12)) == "0/1",
              "the first frame shows none of the incoming shot");
        Check(Fraction(CrossDissolveProgress(6, 12)) == "1/2", "half way");
        Check(Fraction(CrossDissolveProgress(11, 12)) == "11/12",
              "the last frame is not yet the incoming shot");
        Check(Fraction(CrossDissolveProgress(12, 12)) == "1/1",
              "the frame past the span is the incoming shot");
    });

    Test("a dip peaks at exactly one on the frame of its cut", [] {
        Document document = TwoShots();
        const DocumentTransition transition =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 4);
        TransitionSpan span;
        std::string error;
        Check(ResolveTransitionSpan(document.sequence, transition, span, error),
              "resolved: " + error);
        Check(span.pre_frames == 2 && span.post_frames == 2, "2 and 2");
        // 98, 99 on the outgoing shot; 100 is the cut and fully the
        // colour; 101 already half back.
        Check(Same(Curve(span), {"0/1", "1/2", "1/1", "1/2"}),
              "the tent of a four-frame dip");
        Check(Fraction(ColorVeilAlpha(span, -1)) == "0/1" &&
                  Fraction(ColorVeilAlpha(span, 4)) == "0/1",
              "nothing outside the span");
    });

    Test("an opening fade is full colour on the first frame of the film", [] {
        Document document = TwoShots();
        const DocumentTransition transition =
            Dip("", kFirst, TransitionAlignment::StartAtCut, 4);
        TransitionSpan span;
        std::string error;
        Check(ResolveTransitionSpan(document.sequence, transition, span, error),
              "resolved: " + error);
        Check(span.cut_frame == 0 && span.pre_frames == 0 &&
                  span.post_frames == 4,
              "four frames, all after the cut");
        Check(Same(Curve(span), {"1/1", "3/4", "1/2", "1/4"}),
              "black, then out of it; frame 4 is the picture");
    });

    Test("a closing fade reaches the colour on the frame past the film", [] {
        Document document = TwoShots();
        const DocumentTransition transition =
            Dip(kSecond, "", TransitionAlignment::EndAtCut, 4);
        TransitionSpan span;
        std::string error;
        Check(ResolveTransitionSpan(document.sequence, transition, span, error),
              "resolved: " + error);
        Check(span.cut_frame == 200 && span.pre_frames == 4 &&
                  span.post_frames == 0,
              "four frames, all before the end");
        Check(Same(Curve(span), {"0/1", "1/4", "1/2", "3/4"}),
              "the last delivered frame is 3/4 of the way to black");
    });

    Test("the film measured on 19 September: 40 in, 50 out", [] {
        Document document = TwoShots();
        document.sequence.transitions = {
            Dip("", kFirst, TransitionAlignment::StartAtCut, 40),
            Dip(kSecond, "", TransitionAlignment::EndAtCut, 50)};
        document.sequence.transitions[1].id = "01K90000000000000000000007";
        std::string error;
        Check(document.Validate(error),
              "a film that opens and closes on black validates: " + error);
        const std::optional<ResolvedColorVeil> opening =
            ResolveTrackColorVeil(document.sequence, kTrack, 20);
        Check(opening && Fraction(opening->alpha) == "1/2",
              "half way through the 40-frame opening");
        const std::optional<ResolvedColorVeil> closing =
            ResolveTrackColorVeil(document.sequence, kTrack, 175);
        Check(closing && Fraction(closing->alpha) == "1/2",
              "half way through the 50-frame closing");
        Check(!ResolveTrackColorVeil(document.sequence, kTrack, 40),
              "the frame after the opening carries no veil");
        Check(!ResolveTrackColorVeil(document.sequence, kTrack, 149),
              "nor does the middle of the film");
        Check(ResolveTrackColorVeil(document.sequence, kTrack, 150).has_value(),
              "the closing starts 50 frames before the end");
    });

    Test("the monitor reads the same veil as the document", [] {
        Document document = TwoShots();
        document.sequence.transitions = {
            Dip("", kFirst, TransitionAlignment::StartAtCut, 40)};
        std::string error;
        Check(document.Validate(error), "validated: " + error);
        const Timeline timeline(document);
        const std::optional<ResolvedColorVeil> veil =
            timeline.ResolveTrackColorVeil(kTrack, RationalTime(10, 25));
        Check(veil && Fraction(veil->alpha) == "3/4",
              "frame 10 of a 40-frame opening is 3/4 black");
        // A dip moves no clip: the layers under the veil are the layers
        // without it, which is what lets it sit where nothing can dissolve.
        const std::vector<ResolvedLayer> layers =
            timeline.ResolveTrackLayers(kTrack, RationalTime(10, 25));
        Check(layers.size() == 1 && layers.front().frame.clip_id == kFirst &&
                  layers.front().frame.source_frame == 60,
              "one layer, the shot's own frame");
    });

    // R2c -- docs/roadmap/REEL-2026-09.md. A layer's opacity is the frame's:
    // a key replaces the clip's constant, as in the export. The monitor used
    // to multiply the two, so a translucent clip with keys -- or with only
    // its framing keyed -- was shown darker than it was delivered.
    Test("a layer's opacity is the one its frame shows", [] {
        const auto key = [](const char* id, int64_t frame, int32_t num,
                            int32_t den) {
            ClipKeyframe keyframe;
            keyframe.id = id;
            keyframe.time = RationalTime(frame, 25);
            keyframe.value = EffectParamValue{num, den};
            return keyframe;
        };
        Document document = TwoShots();
        document.sequence.width = 1920;
        document.sequence.height = 1080;
        // A framing key needs the picture's size: the shot's media.
        LibraryMedia media;
        media.id = kSource;
        media.path = "shot.mov";
        media.filename = "shot.mov";
        media.codec = "h264";
        media.orientation = "landscape";
        media.has_video = true;
        media.width = 1920;
        media.height = 1080;
        media.rate = MediaRate{25, 1};
        media.duration = RationalTime(400, 25);
        document.library = {media};
        DocumentClip& first = document.sequence.tracks[0].clips[0];
        first.opacity = {1, 2};
        ClipParameterAnimation zoom;
        zoom.parameter = ClipAnimatedParameter::FramingScale;
        zoom.keyframes = {key("01K90000000000000000000K01", 0, 1, 1),
                          key("01K90000000000000000000K02", 20, 6, 5)};
        first.animation = {zoom};
        std::string error;
        Check(document.Validate(error), "validated: " + error);
        std::vector<ResolvedLayer> layers =
            Timeline(document).ResolveTrackLayers(kTrack, RationalTime(10, 25));
        Check(layers.size() == 1 && layers.front().opacity == 0.5f,
              "a zoom keyed on a clip at 1/2 leaves it at 1/2");

        ClipParameterAnimation fade;
        fade.parameter = ClipAnimatedParameter::Opacity;
        fade.keyframes = {key("01K90000000000000000000K03", 0, 1, 1),
                          key("01K90000000000000000000K04", 20, 0, 1)};
        first.animation = {fade};
        first.opacity = {1, 4};
        layers =
            Timeline(document).ResolveTrackLayers(kTrack, RationalTime(10, 25));
        Check(layers.size() == 1 && layers.front().opacity == 0.5f,
              "half way down a fade from 1 to 0, 1/2 -- not the clip's 1/4");

        // In a dissolve, the incoming side is its progress times its key.
        first.animation.clear();
        first.opacity = {1, 1};
        DocumentClip& second = document.sequence.tracks[0].clips[1];
        ClipParameterAnimation half;
        half.parameter = ClipAnimatedParameter::Opacity;
        half.keyframes = {key("01K90000000000000000000K05", 0, 1, 2)};
        second.animation = {half};
        DocumentTransition dissolve;
        dissolve.id = kTransition;
        dissolve.track_id = kTrack;
        dissolve.left_clip_id = kFirst;
        dissolve.right_clip_id = kSecond;
        dissolve.type = "cross_dissolve";
        dissolve.duration = RationalTime(10, 25);
        dissolve.alignment = TransitionAlignment::Center;
        document.sequence.transitions = {dissolve};
        Check(document.Validate(error), "the dissolve validates: " + error);
        layers = Timeline(document).ResolveTrackLayers(kTrack,
                                                       RationalTime(100, 25));
        const float progress =
            TransitionAlphaFloat(CrossDissolveProgress(5, 10));
        Check(layers.size() == 2 && layers[0].opacity == 1.0f &&
                  layers[1].opacity == progress * 0.5f,
              "the incoming shot is its progress times its single key");
    });

    Test("the exact fraction becomes a number in exactly two places", [] {
        Check(TransitionAlphaCode({0, 1}) == 0 &&
                  TransitionAlphaCode({1, 1}) == 255,
              "the ends of an alpha plane");
        Check(TransitionAlphaCode({1, 2}) == 128, "a half rounds away from 0");
        Check(TransitionAlphaCode({1, 3}) == 85, "a third");
        Check(TransitionAlphaFloat({1, 4}) == 0.25f, "a quarter as a float");
        Check(TransitionAlphaFloat({5, 4}) == 1.0f, "clamped above");
    });

    Test("Validate refuses what cannot be rendered", [] {
        const auto refused = [](const DocumentTransition& transition,
                                const std::string& expected) {
            Document document = TwoShots();
            document.sequence.transitions = {transition};
            std::string error;
            const bool ok = document.Validate(error);
            Check(!ok, "should have been refused: " + expected);
            Check(ok || error.find(expected) != std::string::npos,
                  "expected '" + expected + "', got '" + error + "'");
        };
        DocumentTransition unknown =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 4);
        unknown.type = "flash";
        refused(unknown, "unsupported type");

        DocumentTransition edgeDissolve =
            Dip("", kFirst, TransitionAlignment::StartAtCut, 4);
        edgeDissolve.type = kTransitionCrossDissolve;
        edgeDissolve.color = "#000000";
        refused(edgeDissolve, "only a 'dip_to_color' sits at the edge");

        DocumentTransition coloured =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 4, "#ffffff");
        coloured.type = kTransitionCrossDissolve;
        refused(coloured, "carries a colour but is not a 'dip_to_color'");

        refused(Dip(kFirst, kSecond, TransitionAlignment::Center, 4, "rouge"),
                "must be spelled #rrggbb");

        refused(Dip("", kFirst, TransitionAlignment::Center, 4),
                "must be aligned 'start_at_cut'");
        refused(Dip(kSecond, "", TransitionAlignment::Center, 4),
                "must be aligned 'end_at_cut'");

        // A veil longer than the shot it covers would paint the clip
        // before it, which belongs to another transition or to nothing.
        refused(Dip("", kFirst, TransitionAlignment::StartAtCut, 101),
                "reaches past the end of the clip it covers");
        refused(Dip(kFirst, "", TransitionAlignment::EndAtCut, 101),
                "reaches back past the start of the clip it covers");
    });

    Test("two transitions of one track cannot claim the same frame", [] {
        Document document = TwoShots();
        DocumentTransition dissolve =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 12);
        dissolve.type = kTransitionCrossDissolve;
        dissolve.color = "#000000";
        DocumentTransition dip =
            Dip(kFirst, kSecond, TransitionAlignment::EndAtCut, 4);
        dip.id = "01K90000000000000000000008";
        document.sequence.transitions = {dissolve, dip};
        std::string error;
        Check(!document.Validate(error) &&
                  error.find("overlaps transition") != std::string::npos,
              "a dip inside a dissolve is refused: " + error);
        // Moved clear of it, both stand.
        document.sequence.transitions[1] =
            Dip(kFirst, "", TransitionAlignment::EndAtCut, 4);
        document.sequence.transitions[1].id = "01K90000000000000000000008";
        document.sequence.transitions[1].duration = RationalTime(4, 25);
        document.sequence.transitions[0].duration = RationalTime(4, 25);
        Check(!document.Validate(error), "still overlapping at the same cut");
    });

    Test("a dip is allowed on a shot a dissolve could not touch", [] {
        // R3 refuses a dissolve on a retimed shot because it would have to
        // name a handle frame. A dip reads no handle, so it stands.
        Document document = TwoShots();
        document.sequence.tracks[0].clips[0].speed = EffectParamValue{1, 2};
        document.sequence.tracks[0].clips[0].source_in = RationalTime(50, 25);
        document.sequence.transitions = {
            Dip(kFirst, "", TransitionAlignment::EndAtCut, 4)};
        std::string error;
        Check(document.Validate(error),
              "a closing dip over a slowed shot: " + error);
        document.sequence.transitions[0].type = kTransitionCrossDissolve;
        document.sequence.transitions[0].left_clip_id = kFirst;
        document.sequence.transitions[0].right_clip_id = kSecond;
        document.sequence.transitions[0].alignment =
            TransitionAlignment::Center;
        Check(!document.Validate(error) &&
                  error.find("speed") != std::string::npos,
              "the dissolve is still refused: " + error);
    });

    Test("the version follows what the transitions spell", [] {
        Document plain = TwoShots();
        DocumentTransition dissolve =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 12);
        dissolve.type = kTransitionCrossDissolve;
        dissolve.color = "#000000";
        plain.sequence.transitions = {dissolve};
        std::string error;
        Check(plain.Validate(error), "a plain dissolve validates: " + error);
        Check(!HoldsColorTransition(plain.sequence),
              "a plain dissolve is not a version-13 fact");
        Check(plain.SaveToString().find("\"version\": 7") != std::string::npos,
              "and keeps version 7");

        Document dipped = TwoShots();
        dipped.sequence.transitions = {
            Dip("", kFirst, TransitionAlignment::StartAtCut, 4)};
        Check(dipped.Validate(error), "an opening dip validates: " + error);
        Check(HoldsColorTransition(dipped.sequence), "which is version 13");
        Check(
            dipped.SaveToString().find("\"version\": 13") != std::string::npos,
            "and declares it");
        // Black is the default and is not written; a white dip is.
        Check(dipped.SaveToString().find("\"color\"") == std::string::npos,
              "a black dip writes no colour key");
        dipped.sequence.transitions[0].color = "#ffffff";
        Check(dipped.SaveToString().find("\"color\":\"#ffffff\"") !=
                  std::string::npos,
              "a white one does");

        // What must NOT move the version, because the fixtures written
        // before this ticket already spell it: a transition type no build
        // knows, and a transition with one side missing. Both are refused
        // by Validate, in every build, so no file is ever read wrong for
        // want of a version.
        Document odd = TwoShots();
        DocumentTransition unknown =
            Dip(kFirst, kSecond, TransitionAlignment::Center, 4);
        unknown.type = "wipe";
        unknown.color = "#000000";
        odd.sequence.transitions = {unknown};
        Check(!HoldsColorTransition(odd.sequence),
              "an unknown type is not a version-13 fact");
        odd.sequence.transitions[0].type = kTransitionCrossDissolve;
        odd.sequence.transitions[0].left_clip_id.clear();
        Check(!HoldsColorTransition(odd.sequence),
              "nor is a missing side on its own");
    });

    if (failures > 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all transition render tests passed\n";
    return 0;
}
