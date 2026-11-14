// PERF13 -- MonitorOcclusion.h. Which layers of the Record monitor an
// opaque shot above hides, which playback then neither decodes nor asks
// for. Plain C++ with no AppKit, so this runs on a plain Linux host.
//
// What it guards: a plain cutaway hides the interview under it, and each
// clause of the rule, broken alone, leaves the interview shown -- the rule
// declares a layer hidden only when it cannot be seen, and errs the other
// way. And the frame a hidden track is shown again at, which playback asks
// its decoder for ahead.

#include "MonitorOcclusion.h"

#include "Document.h"
#include "Json.h"
#include "Timeline.h"

#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using monitor_occlusion::Picture;
using monitor_occlusion::PictureLookup;

const Ulid kInterview = "01KP0000000000000000000001";
const Ulid kCutaway = "01KP0000000000000000000002";
const Ulid kOther = "01KP0000000000000000000003";
const Ulid kV1 = "01KP0000000000000000000010";
const Ulid kV2 = "01KP0000000000000000000011";
const Ulid kV3 = "01KP0000000000000000000012";
const Ulid kInterviewClip = "01KP0000000000000000000020";
const Ulid kCutawayClip = "01KP0000000000000000000021";
const Ulid kSecondCutawayClip = "01KP0000000000000000000022";
const Ulid kTitleClip = "01KP0000000000000000000023";
const Ulid kComposition = "01KP0000000000000000000030";

RationalTime Frame(int64_t frame) { return {frame, 25}; }

LibraryMedia Media(const Ulid& id, int32_t width, int32_t height,
                   int32_t rotation = 0) {
    LibraryMedia media;
    media.id = id;
    media.path = id + ".mov";
    media.filename = media.path;
    media.metadata_complete = true;
    media.has_video = true;
    media.width = width;
    media.height = height;
    media.rotation_degrees = rotation;
    media.pixel_format = "yuv422p10le";
    media.rate = {25, 1};
    media.duration = {2500, 25};
    return media;
}

DocumentClip Shot(const Ulid& id, const Ulid& source, int64_t in,
                  int64_t frames, int64_t sourceIn = 0) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.timeline_in = Frame(in);
    clip.duration = Frame(frames);
    clip.source_in = Frame(sourceIn);
    return clip;
}

DocumentTrack Track(const Ulid& id, int32_t index,
                    std::vector<DocumentClip> clips) {
    DocumentTrack track;
    track.id = id;
    track.kind = "video";
    track.index = index;
    track.clips = std::move(clips);
    return track;
}

// An interview of ten seconds on V1, a cutaway from 4 s to 8 s on V2, in a
// 1920x1080 sequence at 25 i/s; every rush 16:9.
Document Interview() {
    Document document;
    document.sequence.width = 1920;
    document.sequence.height = 1080;
    document.sequence.frame_rate = {25, 1};
    for (const Ulid& id : {kInterview, kCutaway, kOther}) {
        LibraryMedia media = Media(id, 1920, 1080);
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        document.library.push_back(std::move(media));
    }
    document.sequence.tracks = {
        Track(kV1, 0, {Shot(kInterviewClip, kInterview, 0, 250)}),
        Track(kV2, 1, {Shot(kCutawayClip, kCutaway, 100, 100)})};
    return document;
}

// The frame cache as main.mm reads it: a picture for each source listed.
PictureLookup Cached(const std::map<Ulid, Picture>& pictures) {
    return [pictures](const Ulid& source, int64_t) -> std::optional<Picture> {
        const auto found = pictures.find(source);
        if (found == pictures.end()) {
            return std::nullopt;
        }
        return found->second;
    };
}

PictureLookup AllCached(int64_t width = 1920, int64_t height = 1080) {
    return Cached({{kInterview, {width, height, false}},
                   {kCutaway, {width, height, false}},
                   {kOther, {width, height, false}}});
}

std::set<Ulid> Hidden(const Document& document, int64_t frame,
                      const PictureLookup& picture = AllCached(),
                      bool zoomed = false) {
    const Timeline timeline(document);
    return monitor_occlusion::HiddenTracks(document, timeline, Frame(frame),
                                           picture, zoomed);
}

const std::set<Ulid> kNone;
const std::set<Ulid> kInterviewHidden = {kV1};

DocumentClip& Cutaway(Document& document) {
    return document.sequence.tracks[1].clips.front();
}

mcp_json::Value Json(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    if (!mcp_json::Value::Parse(text, value, error)) {
        throw std::runtime_error("test JSON: " + error);
    }
    return value;
}

void PlainCutaway() {
    const Document document = Interview();
    Check(Hidden(document, 150) == kInterviewHidden,
          "a cutaway, opaque and full frame, hides the interview under it");
    Check(Hidden(document, 100) == kInterviewHidden, "from its first frame");
    Check(Hidden(document, 99) == kNone && Hidden(document, 200) == kNone,
          "and nothing is hidden before or after it");
    Check(Hidden(document, 150, AllCached(), true) == kNone,
          "a zoomed monitor hides nothing: an unframed picture is drawn at "
          "its own size there");
}

void EachClauseAlone() {
    {
        Document document = Interview();
        Cutaway(document).opacity = {1, 2};
        Check(Hidden(document, 150) == kNone,
              "a cutaway at half opacity hides nothing");
    }
    {
        const Document document = Interview();
        Check(Hidden(document, 150,
                     Cached({{kInterview, {1920, 1080, false}}})) == kNone,
              "a cutaway with no picture to draw yet, or offline, hides "
              "nothing: once dropped, the interview must be there");
        Check(Hidden(document, 150,
                     Cached({{kInterview, {1920, 1080, false}},
                             {kCutaway, {1920, 1080, true}}})) == kNone,
              "a cutaway whose picture carries an alpha channel hides "
              "nothing");
        Check(Hidden(document, 150,
                     Cached({{kInterview, {1920, 1080, false}},
                             {kCutaway, {1440, 1080, false}}})) == kNone,
              "a 4:3 cutaway fitted into 16:9 leaves bars, and hides nothing");
        Check(Hidden(document, 150,
                     Cached({{kInterview, {1920, 1080, false}},
                             {kCutaway, {1920, 1082, false}}})) == kNone,
              "nor does a picture two lines off the frame's aspect");
        Check(
            Hidden(document, 150,
                   Cached({{kInterview, {1920, 1080, false}},
                           {kCutaway, {960, 540, false}}})) == kInterviewHidden,
            "a proxy at the frame's exact aspect covers it");
    }
    {
        Document document = Interview();
        document.sequence.tracks[1].visible = false;
        Check(Hidden(document, 150) == kNone,
              "a hidden track's cutaway is not composited, and hides nothing");
    }
    {
        // A motion card, even one that paints the whole frame: its alpha
        // is only known once drawn.
        Document document = Interview();
        document.compositions = {
            {kComposition, "Carton",
             Json(R"({"width":1920,"height":1080,"fps":25,"duration":100,)"
                  R"("root":{"type":"fill","uid":"01KP0000000000000000000031",)"
                  R"("style":{"backgroundColor":"#FFFFFF"}}})")}};
        DocumentClip card = Shot(kTitleClip, "", 100, 100);
        card.composition_id = kComposition;
        document.sequence.tracks[1].clips = {card};
        // Resolved like any layer, so the refusal below is the rule's and
        // not a document the timeline cannot read.
        const Timeline timeline(document);
        Check(timeline.ResolveTrackLayers(kV2, Frame(150)).size() == 1,
              "the motion card is composited at 6 s");
        Check(Hidden(document, 150) == kNone,
              "a motion clip hides nothing, whatever it paints");
    }
}

void Transitions() {
    // Two cutaways meeting at 6 s with a ten-frame dissolve centred on the
    // cut: the dissolve spans frames 145 to 154.
    Document document = Interview();
    document.sequence.tracks[1].clips = {
        Shot(kCutawayClip, kCutaway, 100, 50),
        Shot(kSecondCutawayClip, kOther, 150, 50, 50)};
    DocumentTransition dissolve;
    dissolve.track_id = kV2;
    dissolve.left_clip_id = kCutawayClip;
    dissolve.right_clip_id = kSecondCutawayClip;
    dissolve.duration = Frame(10);
    document.sequence.transitions = {dissolve};
    Check(Hidden(document, 143) == kInterviewHidden &&
              Hidden(document, 156) == kInterviewHidden,
          "two cutaways on either side of a dissolve hide the interview");
    Check(Hidden(document, 150) == kNone, "a dissolve under way hides nothing");
    Check(Hidden(document, 144) == kNone && Hidden(document, 155) == kNone,
          "nor does the frame on either side of it");
    DocumentTransition dip = dissolve;
    dip.type = "dip_to_color";
    document.sequence.transitions = {dip};
    Check(Hidden(document, 150) == kNone &&
              Hidden(document, 143) == kInterviewHidden,
          "a colour dip under way hides nothing either");
}

void Geometry() {
    {
        // The vertical sequence the interviews are cut in, from rushes
        // stored 3840x2160 and turned a quarter by their display matrix.
        Document document = Interview();
        document.sequence.width = 2160;
        document.sequence.height = 3840;
        const Picture upright =
            monitor_occlusion::UprightPicture(3840, 2160, 90, false);
        Check(upright.width == 2160 && upright.height == 3840,
              "a quarter turn swaps a picture's sides");
        Check(Hidden(document, 150,
                     Cached({{kInterview, upright}, {kCutaway, upright}})) ==
                  kInterviewHidden,
              "a rush turned upright to the frame's aspect covers it");
        Check(Hidden(document, 150,
                     Cached({{kInterview, upright},
                             {kCutaway, {3840, 2160, false}}})) == kNone,
              "left lying down, it does not");
    }
    {
        // 4:3 filled into 16:9: framed, the resolved rectangle decides.
        Document document = Interview();
        document.library[1] = Media(kCutaway, 1440, 1080);
        Cutaway(document).framing.mode = FramingMode::Fill;
        const PictureLookup picture = Cached({{kInterview, {1920, 1080, false}},
                                              {kCutaway, {1440, 1080, false}}});
        Check(Hidden(document, 150, picture) == kInterviewHidden,
              "a 4:3 cutaway filled into the frame covers it");
        Cutaway(document).framing.mode = FramingMode::Fit;
        Cutaway(document).framing.scale = {11, 10};
        Check(Hidden(document, 150, picture) == kNone,
              "fitted, even enlarged a little, it leaves bars");
    }
    {
        Document document = Interview();
        Cutaway(document).framing.mode = FramingMode::Fill;
        Cutaway(document).framing.scale = {9, 10};
        Check(Hidden(document, 150) == kNone,
              "a filled cutaway shrunk below the frame hides nothing");
        Cutaway(document).framing.scale = {1, 1};
        Cutaway(document).framing.position_x = {1, 20};
        Check(Hidden(document, 150) == kNone,
              "nor one pushed sideways off an edge");
    }
    {
        // A framing the engine cannot resolve: the media was never probed.
        Document document = Interview();
        document.library[1].metadata_complete = false;
        Cutaway(document).framing.mode = FramingMode::Fill;
        Check(Hidden(document, 150) == kNone,
              "a framing that does not resolve hides nothing");
    }
}

void KeyedOpacity() {
    // Held at 1 for two seconds, then down to 0 at the clip's last frame.
    Document document = Interview();
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    fade.keyframes = {{"01KP0000000000000000000040", Frame(0), {1, 1}},
                      {"01KP0000000000000000000041", Frame(50), {1, 1}},
                      {"01KP0000000000000000000042", Frame(99), {0, 1}}};
    Cutaway(document).animation = {fade};
    Check(Hidden(document, 125) == kInterviewHidden,
          "a keyed opacity sampled at exactly 1 hides the interview");
    Check(Hidden(document, 175) == kNone,
          "the same key sampled below 1 hides nothing");
}

void Stacks() {
    Document document = Interview();
    document.sequence.tracks.push_back(
        Track(kV3, 2, {Shot(kTitleClip, kOther, 120, 40)}));
    document.sequence.tracks[2].clips.front().opacity = {1, 2};
    Check(Hidden(document, 130) == kInterviewHidden,
          "a translucent shot over the cutaway leaves both shown, the "
          "interview hidden");
    document.sequence.tracks[2].clips.front().opacity = {1, 1};
    Check(Hidden(document, 130) == std::set<Ulid>({kV1, kV2}),
          "an opaque shot over both hides both");
    Check(Hidden(document, 110) == kInterviewHidden,
          "and the cutaway alone again hides the interview alone");
}

void NextShown() {
    const Document document = Interview();
    const Timeline timeline(document);
    const auto next = [&](int64_t frame, int direction) {
        return monitor_occlusion::NextShown(document, timeline, Frame(frame),
                                            direction, {1, 1}, kInterviewHidden,
                                            AllCached(), false);
    };
    const auto at = [](const std::map<Ulid, RationalTime>& shown,
                       const Ulid& track) -> std::optional<RationalTime> {
        const auto found = shown.find(track);
        if (found == shown.end()) {
            return std::nullopt;
        }
        return found->second;
    };
    Check(at(next(175, 1), kV1) == Frame(200),
          "playing, the interview is shown again at the cutaway's end");
    Check(next(150, 1).empty(),
          "two seconds before, that is past the one-second window");
    Check(at(next(150, 2), kV1) == Frame(200),
          "at twice the speed the window spans two seconds of timeline");
    Check(at(next(120, -1), kV1) == Frame(99),
          "backward, it is shown again on the frame before the cutaway");
    Check(next(175, 0).empty(), "at rest nothing is looked ahead");

    Document ending = Interview();
    Cutaway(ending) = Shot(kCutawayClip, kCutaway, 200, 50);
    const Timeline endingTimeline(ending);
    Check(monitor_occlusion::NextShown(ending, endingTimeline, Frame(240), 1,
                                       {1, 1}, kInterviewHidden, AllCached(),
                                       false)
              .empty(),
          "a cutaway that ends the timeline shows nothing again");
}

void Upright() {
    using monitor_occlusion::UprightPicture;
    const auto sides = [](const Picture& picture) {
        return std::make_pair(picture.width, picture.height);
    };
    Check(sides(UprightPicture(3840, 2160, 0, false)) ==
                  std::make_pair<int64_t, int64_t>(3840, 2160) &&
              sides(UprightPicture(3840, 2160, 180, false)) ==
                  std::make_pair<int64_t, int64_t>(3840, 2160) &&
              sides(UprightPicture(3840, 2160, -90, false)) ==
                  std::make_pair<int64_t, int64_t>(2160, 3840) &&
              sides(UprightPicture(3840, 2160, 270, false)) ==
                  std::make_pair<int64_t, int64_t>(2160, 3840),
          "half turns keep a picture's sides, quarter turns swap them");
    Check(UprightPicture(64, 36, 90, true).alpha, "an alpha channel stays one");
}

}  // namespace

int main() {
    PlainCutaway();
    EachClauseAlone();
    Transitions();
    Geometry();
    KeyedOpacity();
    Stacks();
    NextShown();
    Upright();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "monitor occlusion tests passed\n";
    return 0;
}
