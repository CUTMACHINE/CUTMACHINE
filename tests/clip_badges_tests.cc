// PRD24 -- ROADMAP.md. What a clip's rectangle says on the timeline, and
// where: the policy -[AppDelegate timelineRenderData] draws from.
//
// Portable on purpose: the display list is not tested, so everything it
// would otherwise decide -- which badges, their text, their order, which of
// them a narrow clip keeps, where each one sits -- is decided in ClipBadges.h
// and pinned here, without AppKit or Metal.
//
// The documents are real ones: every speed, framing, animation, grade,
// motion clip and multicam group below is put there by its operation,
// through EditLog::Apply, so that these tests read each fact where the
// engine writes it and not where a hand-filled struct would have it.

#include "ClipBadges.h"
#include "EditLog.h"
#include "Json.h"
#include "Multicam.h"
#include "Operations.h"
#include "UiTheme.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

const Ulid kMedia = "01KPRD24000000000000000001";
const Ulid kPicture = "01KPRD24000000000000000002";
const Ulid kOverlay = "01KPRD24000000000000000003";
const Ulid kSoundTrack = "01KPRD24000000000000000004";
const Ulid kPlain = "01KPRD24000000000000000011";
const Ulid kShot = "01KPRD24000000000000000012";
const Ulid kSound = "01KPRD24000000000000000013";
const Ulid kClose = "01KPRD24000000000000000014";
const Ulid kTitle = "01KPRD24000000000000000015";
const Ulid kLink = "01KPRD24000000000000000021";
const Ulid kComposition = "01KPRD24000000000000000031";
const Ulid kKeyA = "01KPRD24000000000000000041";
const Ulid kKeyB = "01KPRD24000000000000000042";
const Ulid kKeyC = "01KPRD24000000000000000043";
const Ulid kKeyD = "01KPRD24000000000000000044";

// A title of 60 frames, as a project stores one.
const char kTitleDefinition[] =
    R"({"width":1920,"height":1080,"fps":25,"duration":60,)"
    R"("root":{"type":"fill","uid":"01KPRD24000000000000000032",)"
    R"("children":[{"type":"text","uid":"01KPRD24000000000000000033",)"
    R"("content":"Titre","style":{"fontSize":80}}]}})";

void Apply(Document& document, EditLog& log, Operation operation,
           const std::string& what) {
    EditError error = EditError::None;
    std::string message;
    if (!log.Apply(document, std::move(operation), error, message)) {
        throw std::runtime_error(what + ": " + message);
    }
}

// A rush in the library is the one thing no timeline operation puts in a
// document (an ingest does); everything after it is an operation. Two video
// tracks and one audio track; on V1 a shot nothing is done to, then the shot
// the tests work on, with its linked sound under it on A1.
Document Montage(EditLog& log) {
    Document document;
    LibraryMedia media;
    media.id = kMedia;
    media.path = "/rushes/plan.mov";
    media.filename = "plan.mov";
    media.codec = "h264";
    media.width = 1920;
    media.height = 1080;
    media.orientation = "landscape";
    media.rate = {25, 1};
    // Long enough for a two-second shot at a hundred times its speed.
    media.duration = {100000, 25};
    media.has_audio = true;
    media.audio_rate = 48000;
    media.audio_channels = 2;
    document.library = {media};
    document.sources = {{kMedia, media.path, media.rate, media.duration}};

    Apply(document, log, AddTrackOperation{kPicture, "video", 0}, "V1");
    Apply(document, log, AddTrackOperation{kOverlay, "video", 1}, "V2");
    Apply(document, log, AddTrackOperation{kSoundTrack, "audio", 2}, "A1");
    const auto insert = [&](const Ulid& track, const Ulid& clip,
                            int64_t sourceIn, int64_t at) {
        InsertClipOperation operation;
        operation.track_id = track;
        operation.source_id = kMedia;
        operation.source_in = {sourceIn, 25};
        operation.duration = {50, 25};
        operation.timeline_in = {at, 25};
        operation.clip_id = clip;
        Apply(document, log, operation, "insert " + clip);
    };
    insert(kPicture, kPlain, 0, 0);
    insert(kPicture, kShot, 100, 50);
    insert(kSoundTrack, kSound, 100, 50);
    Apply(document, log, SetClipLinkOperation{kShot, kSound, kLink, kLink},
          "link the shot and its sound");
    return document;
}

ClipSignage Signage(const Document& document, const Ulid& clipId) {
    const DocumentClip* clip = document.FindClip(clipId);
    if (clip == nullptr) {
        throw std::runtime_error("no clip " + clipId);
    }
    return ClipSignageFor(document, *clip);
}

std::string Labels(const std::vector<ClipBadge>& badges) {
    std::string joined;
    for (const ClipBadge& badge : badges) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += badge.label;
    }
    return joined;
}

std::string Labels(const std::vector<PlacedClipBadge>& badges) {
    std::string joined;
    for (const PlacedClipBadge& badge : badges) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += badge.label;
    }
    return joined;
}

std::vector<ClipBadgeKind> Kinds(const std::vector<ClipBadge>& badges) {
    std::vector<ClipBadgeKind> kinds;
    kinds.reserve(badges.size());
    for (const ClipBadge& badge : badges) {
        kinds.push_back(badge.kind);
    }
    return kinds;
}

// "ÉTALO", written as ClipBadges.cc writes it: a precomposed É.
const std::string kGrade =
    "\xC3\x89"
    "TALO";

// DEC36 -- "GÉNÉRÉ", three precomposed É.
const std::string kGenerated =
    "G\xC3\x89"
    "N\xC3\x89"
    "R\xC3\x89";

// DEC36 -- the provenance generate_media writes (GN3), on the rush itself.
MediaGeneration Provenance() {
    MediaGeneration provenance;
    provenance.service = "fal.ai";
    provenance.model = "fal-ai/bria/expand";
    provenance.kind = "expand_image";
    provenance.prompt = "a sandy beach";
    provenance.seed = "1234";
    provenance.created_at = "2026-10-24T01:02:03Z";
    provenance.request_id = "REQ1";
    return provenance;
}

void SetSpeed(Document& document, EditLog& log, const Ulid& clipId,
              EffectParamValue speed) {
    SetClipSpeedOperation operation;
    operation.clip_id = clipId;
    operation.speed = speed;
    Apply(document, log, operation, "set_clip_speed");
}

void SetFraming(Document& document, EditLog& log, const Ulid& clipId,
                const ClipFraming& framing) {
    SetClipFramingOperation operation;
    operation.clips = {{clipId, framing}};
    Apply(document, log, operation, "set_clip_framing");
}

ClipKeyframe Key(const Ulid& id, int64_t frame, EffectParamValue value) {
    ClipKeyframe key;
    key.id = id;
    key.time = {frame, 25};
    key.value = value;
    return key;
}

void SetAnimation(Document& document, EditLog& log, const Ulid& clipId,
                  std::vector<ClipParameterAnimation> animation) {
    SetClipAnimationOperation operation;
    operation.clips = {{clipId, std::move(animation)}};
    Apply(document, log, operation, "set_clip_animation");
}

ClipParameterAnimation Fade() {
    ClipParameterAnimation fade;
    fade.parameter = ClipAnimatedParameter::Opacity;
    fade.keyframes = {Key(kKeyA, 0, {0, 1}), Key(kKeyB, 10, {1, 1})};
    return fade;
}

ClipParameterAnimation Zoom() {
    ClipParameterAnimation zoom;
    zoom.parameter = ClipAnimatedParameter::FramingScale;
    zoom.keyframes = {Key(kKeyC, 0, {1, 1}), Key(kKeyD, 12, {23, 20})};
    return zoom;
}

void SetGrade(Document& document, EditLog& log, const Ulid& clipId,
              bool graded) {
    SetClipEffectsOperation operation;
    operation.clip_id = clipId;
    if (graded) {
        ClipEffect exposure;
        exposure.type = "color.exposure";
        exposure.params["amount"] = {1, 3};
        operation.effects = {exposure};
    }
    Apply(document, log, operation, "set_clip_effects");
}

// The room a row of badges needs, worked out from the tokens: the insets at
// both ends, the plates, the gaps between them.
double RoomFor(const std::vector<ClipBadge>& badges, double leading = 0.0) {
    double room = ui::theme::kTimelineClipBadgeInset +
                  ui::theme::kTimelineClipBadgeEndInset;
    if (leading > 0.0) {
        room += leading + ui::theme::kTimelineClipBadgeGap;
    }
    for (size_t index = 0; index < badges.size(); ++index) {
        if (index != 0) {
            room += ui::theme::kTimelineClipBadgeGap;
        }
        room += ClipBadgeWidth(badges[index].label);
    }
    return room;
}

bool Near(double left, double right) { return std::fabs(left - right) < 1e-9; }

const std::vector<ClipBadge> kEverything = {
    {ClipBadgeKind::Speed, "50%"},
    {ClipBadgeKind::Animation, "ANIM"},
    {ClipBadgeKind::Framing, "CADRE"},
    {ClipBadgeKind::Grade, kGrade},
};

// The code points of a UTF-8 label; -1 where the bytes are not UTF-8.
std::vector<int> CodePoints(const std::string& label) {
    std::vector<int> points;
    for (size_t index = 0; index < label.size();) {
        const unsigned char lead = static_cast<unsigned char>(label[index]);
        if (lead < 0x80) {
            points.push_back(lead);
            ++index;
        } else if ((lead & 0xE0) == 0xC0 && index + 1 < label.size()) {
            points.push_back(
                ((lead & 0x1F) << 6) |
                (static_cast<unsigned char>(label[index + 1]) & 0x3F));
            index += 2;
        } else {
            points.push_back(-1);
            ++index;
        }
    }
    return points;
}

}  // namespace

int main() {
    // ---- What a clip carries ------------------------------------------

    Test("a shot nothing was done to carries nothing", [] {
        EditLog log;
        const Document document = Montage(log);
        for (const Ulid& clip : {kPlain, kShot, kSound}) {
            const ClipSignage signage = Signage(document, clip);
            Check(signage.badges.empty(),
                  clip + " carries " + Labels(signage.badges));
            Check(!signage.hidden_angle, clip + " is not a hidden angle");
        }
    });

    Test("a speed reads its exact fraction, with no floating point", [] {
        struct Case {
            EffectParamValue speed;
            const char* label;
        };
        // What set_clip_speed stores: it reduces the fraction it is given.
        const Case cases[] = {
            {{2, 1}, "2x"},     {{1, 2}, "50%"},   {{2, 5}, "40%"},
            {{3, 2}, "150%"},   {{5, 4}, "125%"},  {{1, 4}, "25%"},
            {{4, 3}, "4/3x"},   {{1, 8}, "1/8x"},  {{50, 100}, "50%"},
            {{4, 2}, "2x"},     {{24, 25}, "96%"}, {{3, 1}, "3x"},
            {{7, 20}, "35%"},   {{2, 3}, "2/3x"},  {{10, 1}, "10x"},
            {{1, 10}, "10%"},   {{9, 8}, "9/8x"},  {{1, 3}, "1/3x"},
            {{100, 1}, "100x"}, {{1, 100}, "1%"},
        };
        for (const Case& item : cases) {
            EditLog log;
            Document document = Montage(log);
            // The plain shot is in no link group and reads the rush from its
            // first frame, so every speed above fits its source.
            SetSpeed(document, log, kPlain, item.speed);
            const ClipSignage signage = Signage(document, kPlain);
            Check(signage.badges.size() == 1 &&
                      signage.badges.front().kind == ClipBadgeKind::Speed &&
                      signage.badges.front().label == item.label,
                  std::to_string(item.speed.num) + "/" +
                      std::to_string(item.speed.den) + " reads \"" +
                      Labels(signage.badges) + "\", expected \"" + item.label +
                      "\"");
        }
    });

    Test("a speed spelled unreduced reads as its reduced fraction", [] {
        // set_clip_speed reduces what it stores, so this spelling only
        // arrives in a document written by hand. It must not read as
        // another speed.
        Check(ClipSpeedBadgeLabel({4, 2}) == "2x",
              "4/2 reads " + ClipSpeedBadgeLabel({4, 2}));
        Check(ClipSpeedBadgeLabel({50, 100}) == "50%",
              "50/100 reads " + ClipSpeedBadgeLabel({50, 100}));
        Check(ClipSpeedBadgeLabel({6, 4}) == "150%",
              "6/4 reads " + ClipSpeedBadgeLabel({6, 4}));
        Check(ClipSpeedBadgeLabel({8, 6}) == "4/3x",
              "8/6 reads " + ClipSpeedBadgeLabel({8, 6}));
    });

    // R3, criterion 4 -- a ramp reads its slowest and fastest key.
    Test("a ramp reads from its slowest to its fastest key", [] {
        DocumentClip clip;
        ClipKeyframe key;
        key.id = "01K90000000000000000000R01";
        key.time = {0, 25};
        key.value = {1, 1};
        ClipKeyframe slow = key;
        slow.id = "01K90000000000000000000R02";
        slow.time = {10, 25};
        slow.value = {1, 4};
        ClipKeyframe back = key;
        back.id = "01K90000000000000000000R03";
        back.time = {20, 25};
        clip.speed_ramp = {key, slow, back};
        Check(ClipSpeedRampBadgeLabel(clip) == "25%-100%",
              "1 -> 1/4 -> 1 reads " + ClipSpeedRampBadgeLabel(clip));
        clip.speed_ramp[1].value = {0, 1};
        clip.speed_ramp[2].value = {2, 1};
        Check(ClipSpeedRampBadgeLabel(clip) == "0%-2x",
              "a stop and a double read " + ClipSpeedRampBadgeLabel(clip));
        clip.speed_ramp.clear();
        Check(ClipSpeedRampBadgeLabel(clip).empty(), "no ramp, no label");
    });

    Test("a speed label never divides by a term a document cannot carry", [] {
        Check(ClipSpeedBadgeLabel({1, 1}).empty(), "1/1 is no speed");
        Check(ClipSpeedBadgeLabel({2, 2}).empty(), "2/2 is no speed");
        Check(ClipSpeedBadgeLabel({1, 0}).empty(), "a zero denominator");
        Check(ClipSpeedBadgeLabel({0, 1}).empty(), "a zero speed");
        Check(ClipSpeedBadgeLabel({-1, 2}).empty(), "a negative speed");
        Check(ClipSpeedBadgeLabel({1, -2}).empty(), "a negative denominator");
        // The widest fraction ClipSpeed.h allows, and one past 32 bits once
        // multiplied by a hundred.
        Check(ClipSpeedBadgeLabel({999, 1000}) == "999/1000x",
              "999/1000 reads " + ClipSpeedBadgeLabel({999, 1000}));
        Check(ClipSpeedBadgeLabel({2000000000, 1}) == "2000000000x",
              "a whole multiple beyond a 32-bit percentage");
        Check(ClipSpeedBadgeLabel({2000000001, 2000000000}) ==
                  "2000000001/2000000000x",
              "a fraction whose percentage overflows 32 bits reads " +
                  ClipSpeedBadgeLabel({2000000001, 2000000000}));
    });

    Test("a speed set back to 1/1 takes its badge away, and so does an undo",
         [] {
             EditLog log;
             Document document = Montage(log);
             SetSpeed(document, log, kPlain, {1, 2});
             Check(Labels(Signage(document, kPlain).badges) == "50%",
                   "the slowed shot says so");
             SetSpeed(document, log, kPlain, {1, 1});
             Check(Signage(document, kPlain).badges.empty(),
                   "back at 1/1, nothing");
             EditError error = EditError::None;
             std::string message;
             Check(log.Undo(document, error, message), "undo: " + message);
             Check(Labels(Signage(document, kPlain).badges) == "50%",
                   "the undo brings 50% back");
             Check(log.Undo(document, error, message), "undo: " + message);
             Check(Signage(document, kPlain).badges.empty(),
                   "the second undo leaves the shot as it was inserted");
         });

    Test("the speed of a linked shot is said on its sound too", [] {
        EditLog log;
        Document document = Montage(log);
        SetSpeed(document, log, kShot, {2, 1});
        Check(Labels(Signage(document, kShot).badges) == "2x",
              "the picture reads " + Labels(Signage(document, kShot).badges));
        Check(Labels(Signage(document, kSound).badges) == "2x",
              "the sound reads " + Labels(Signage(document, kSound).badges));
        Check(Signage(document, kPlain).badges.empty(),
              "the shot beside them is untouched");
    });

    Test("a framing that is not the default one is said, the default is not",
         [] {
             EditLog log;
             Document document = Montage(log);
             ClipFraming fill;
             fill.mode = FramingMode::Fill;
             SetFraming(document, log, kShot, fill);
             const ClipSignage filled = Signage(document, kShot);
             Check(filled.badges.size() == 1 &&
                       filled.badges.front().kind == ClipBadgeKind::Framing &&
                       filled.badges.front().label == "CADRE",
                   "fill reads " + Labels(filled.badges));
             ClipFraming zoomed;
             zoomed.scale = {22, 20};
             SetFraming(document, log, kShot, zoomed);
             Check(Labels(Signage(document, kShot).badges) == "CADRE",
                   "a scale in fit is a framing too");
             ClipFraming offset;
             offset.position_x = {-1, 4};
             SetFraming(document, log, kShot, offset);
             Check(Labels(Signage(document, kShot).badges) == "CADRE",
                   "so is a position");
             // The default spelled otherwise: the operation reduces it to
             // the default the file omits.
             ClipFraming spelled;
             spelled.scale = {2, 2};
             spelled.crop_left = {0, 4};
             SetFraming(document, log, kShot, spelled);
             Check(Signage(document, kShot).badges.empty(),
                   "the default framing, however spelled, is no badge");
             Check(Signage(document, kSound).badges.empty(),
                   "a framing is the picture's: the sound says nothing");
         });

    Test("keys on the opacity or on the framing are one animation badge", [] {
        EditLog log;
        Document document = Montage(log);
        SetAnimation(document, log, kShot, {Fade()});
        const ClipSignage faded = Signage(document, kShot);
        Check(faded.badges.size() == 1 &&
                  faded.badges.front().kind == ClipBadgeKind::Animation &&
                  faded.badges.front().label == "ANIM",
              "an opacity fade reads " + Labels(faded.badges));
        SetAnimation(document, log, kShot, {Zoom()});
        Check(Labels(Signage(document, kShot).badges) == "ANIM",
              "a zoom reads " + Labels(Signage(document, kShot).badges));
        SetAnimation(document, log, kShot, {Zoom(), Fade()});
        Check(Labels(Signage(document, kShot).badges) == "ANIM",
              "two animated parameters are still one badge");
        // Keys on the scale are not a framing: the framing field itself is
        // still the default.
        Check(Kinds(Signage(document, kShot).badges) ==
                  std::vector<ClipBadgeKind>{ClipBadgeKind::Animation},
              "an animated scale is not reported as a framing");
        SetAnimation(document, log, kShot, {});
        Check(Signage(document, kShot).badges.empty(),
              "an emptied animation is no badge");
    });

    Test("a colour stack is said, an emptied one is not", [] {
        EditLog log;
        Document document = Montage(log);
        SetGrade(document, log, kShot, true);
        const ClipSignage graded = Signage(document, kShot);
        Check(graded.badges.size() == 1 &&
                  graded.badges.front().kind == ClipBadgeKind::Grade &&
                  graded.badges.front().label == kGrade,
              "a graded shot reads " + Labels(graded.badges));
        SetGrade(document, log, kShot, false);
        Check(Signage(document, kShot).badges.empty(),
              "an empty stack is no badge");
    });

    Test("a motion clip is said, with what it carries after it", [] {
        EditLog log;
        Document document = Montage(log);
        mcp_json::Value definition;
        std::string parseError;
        if (!mcp_json::Value::Parse(kTitleDefinition, definition, parseError)) {
            throw std::runtime_error(parseError);
        }
        AddMotionCompositionOperation add;
        add.composition = {kComposition, "Titre", definition};
        Apply(document, log, add, "add_motion_composition");
        InsertMotionClipOperation insert;
        insert.track_id = kOverlay;
        insert.composition_id = kComposition;
        insert.source_in = {0, 25};
        insert.duration = {50, 25};
        insert.timeline_in = {0, 25};
        insert.clip_id = kTitle;
        Apply(document, log, insert, "insert_motion_clip");
        const ClipSignage title = Signage(document, kTitle);
        Check(title.badges.size() == 1 &&
                  title.badges.front().kind == ClipBadgeKind::Motion &&
                  title.badges.front().label == "MOTION",
              "a title reads " + Labels(title.badges));
        // A fade is all the engine lets a motion clip carry: its speed and
        // its framing belong to the composition, and it takes no colour
        // effect (Operations.cc, MotionClip.cc).
        SetAnimation(document, log, kTitle, {Fade()});
        Check(
            Labels(Signage(document, kTitle).badges) == "MOTION ANIM",
            "a faded title reads " + Labels(Signage(document, kTitle).badges));
        Check(Signage(document, kPlain).badges.empty(),
              "the shot under the title says nothing");
    });

    Test("badges come in one fixed order, whatever order they were set in", [] {
        const std::vector<ClipBadgeKind> expected = {
            ClipBadgeKind::Speed, ClipBadgeKind::Animation,
            ClipBadgeKind::Framing, ClipBadgeKind::Grade};
        ClipFraming fill;
        fill.mode = FramingMode::Fill;
        {
            EditLog log;
            Document document = Montage(log);
            SetSpeed(document, log, kPlain, {1, 2});
            SetAnimation(document, log, kPlain, {Fade()});
            SetFraming(document, log, kPlain, fill);
            SetGrade(document, log, kPlain, true);
            const ClipSignage signage = Signage(document, kPlain);
            Check(Kinds(signage.badges) == expected &&
                      Labels(signage.badges) == "50% ANIM CADRE " + kGrade,
                  "set in priority order: " + Labels(signage.badges));
        }
        {
            EditLog log;
            Document document = Montage(log);
            SetGrade(document, log, kPlain, true);
            SetFraming(document, log, kPlain, fill);
            SetAnimation(document, log, kPlain, {Fade()});
            SetSpeed(document, log, kPlain, {1, 2});
            const ClipSignage signage = Signage(document, kPlain);
            Check(Kinds(signage.badges) == expected,
                  "set in reverse order: " + Labels(signage.badges));
        }
    });

    // DEC36 -- docs/roadmap/GENERATIF-2026-10.md. A clip of a media a
    // service generated says so, before its speed; the badge comes and goes
    // with the provenance, and a motion clip never carries it.
    Test("a generated picture says GENERE, before the speed", [] {
        EditLog log;
        Document document = Montage(log);
        SetSpeed(document, log, kShot, {1, 2});
        Check(Labels(Signage(document, kShot).badges) == "50%",
              "a filmed shot: its speed only");
        Apply(document, log, SetMediaGenerationOperation{kMedia, Provenance()},
              "mark the rush as generated");
        const ClipSignage shot = Signage(document, kShot);
        Check(Kinds(shot.badges) ==
                      std::vector<ClipBadgeKind>{ClipBadgeKind::Generated,
                                                 ClipBadgeKind::Speed} &&
                  Labels(shot.badges) == kGenerated + " 50%",
              "generated, then the speed: " + Labels(shot.badges));
        Check(Labels(Signage(document, kPlain).badges) == kGenerated,
              "every clip of the media says it: " +
                  Labels(Signage(document, kPlain).badges));
        const DocumentClip* plain = document.FindClip(kPlain);
        Check(plain != nullptr &&
                  Kinds(ClipSignageFor(document, *plain, true).badges) ==
                      std::vector<ClipBadgeKind>{ClipBadgeKind::Agent,
                                                 ClipBadgeKind::Generated},
              "the agent's mark stays first");
        const double room = RoomFor({shot.badges[0]}) + 0.01;
        Check(Labels(PlaceClipBadges(shot.badges, room)) == kGenerated,
              "narrow, the speed goes first: " +
                  Labels(PlaceClipBadges(shot.badges, room)));
        Check(ClipBadgeGlyphCount(kGenerated) == 6,
              "six glyphs for the atlas, not nine bytes");

        // A motion clip plays a composition, never the library.
        mcp_json::Value definition;
        std::string parseError;
        if (!mcp_json::Value::Parse(kTitleDefinition, definition, parseError)) {
            throw std::runtime_error(parseError);
        }
        AddMotionCompositionOperation add;
        add.composition = {kComposition, "Titre", definition};
        Apply(document, log, add, "a composition");
        InsertMotionClipOperation title;
        title.track_id = kOverlay;
        title.composition_id = kComposition;
        title.source_in = {0, 25};
        title.duration = {50, 25};
        title.timeline_in = {0, 25};
        title.clip_id = kTitle;
        Apply(document, log, title, "a title");
        Check(Kinds(Signage(document, kTitle).badges) ==
                  std::vector<ClipBadgeKind>{ClipBadgeKind::Motion},
              "a motion clip is not generated");

        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  log.Undo(document, error, message) &&
                  log.Undo(document, error, message),
              "undo the title, the composition and the provenance: " + message);
        Check(Labels(Signage(document, kShot).badges) == "50%",
              "undone, the badge goes: " +
                  Labels(Signage(document, kShot).badges));
    });

    Test("the priority is speed, gain, motion, animation, framing, grade", [] {
        // The order of the enumeration is the order of the row and the
        // order badges are given up in, last first. PRD25 -- the agent's
        // mark comes before all of them. DEC36 -- then a generated picture.
        Check(ClipBadgeKind::Agent < ClipBadgeKind::Generated &&
                  ClipBadgeKind::Generated < ClipBadgeKind::Speed &&
                  ClipBadgeKind::Speed < ClipBadgeKind::Gain &&
                  ClipBadgeKind::Gain < ClipBadgeKind::Motion &&
                  ClipBadgeKind::Motion < ClipBadgeKind::Animation &&
                  ClipBadgeKind::Animation < ClipBadgeKind::Framing &&
                  ClipBadgeKind::Framing < ClipBadgeKind::Grade,
              "ClipBadgeKind is declared in priority order");
        Check(ClipBadgeLabel(ClipBadgeKind::Speed).empty() &&
                  ClipBadgeLabel(ClipBadgeKind::Gain).empty(),
              "a speed and a gain have no fixed label");
        Check(ClipBadgeLabel(ClipBadgeKind::Agent) == "AGENT",
              "the agent's mark reads as the panel is called");
        Check(ClipBadgeLabel(ClipBadgeKind::Generated) == kGenerated,
              "a generated picture reads GÉNÉRÉ, accents included");
        Check(ClipBadgeLabel(ClipBadgeKind::Motion) == "MOTION" &&
                  ClipBadgeLabel(ClipBadgeKind::Animation) == "ANIM" &&
                  ClipBadgeLabel(ClipBadgeKind::Framing) == "CADRE" &&
                  ClipBadgeLabel(ClipBadgeKind::Grade) == kGrade,
              "the fixed labels");
    });

    Test("GitHub #330: a sound's gain says itself, its fades do not", [] {
        Check(ClipGainBadgeLabel({-18, 1}) == "-18 dB" &&
                  ClipGainBadgeLabel({3, 1}) == "+3 dB" &&
                  ClipGainBadgeLabel({-9, 2}) == "-4.5 dB" &&
                  ClipGainBadgeLabel({-1, 3}) == "-0.3 dB" &&
                  ClipGainBadgeLabel({-1, 20}) == "-0.1 dB" &&
                  ClipGainBadgeLabel({-120, 1}) == "-120 dB",
              "whole decibels, else the nearest tenth, half away from zero");
        Check(ClipGainBadgeLabel({0, 1}).empty() &&
                  ClipGainBadgeLabel({0, 7}).empty() &&
                  ClipGainBadgeLabel({1, 40}).empty() &&
                  ClipGainBadgeLabel({5, 0}).empty() &&
                  ClipGainBadgeLabel({5, -1}).empty(),
              "no gain, a gain under a twentieth, a bad fraction: nothing");
        EditLog log;
        Document document = Montage(log);
        Check(Signage(document, kSound).badges.empty(),
              "a sound at its own level says nothing");
        Apply(document, log,
              SetClipAudioOperation{kSound, {0, 1}, {12, 25}, {25, 25}},
              "fades only");
        Check(Signage(document, kSound).badges.empty(),
              "its fades are drawn on its body, not written on a badge");
        Apply(document, log,
              SetClipAudioOperation{kSound, {-18, 1}, {12, 25}, {25, 25}},
              "set_clip_audio");
        const ClipSignage signage = Signage(document, kSound);
        Check(Kinds(signage.badges) ==
                      std::vector<ClipBadgeKind>{ClipBadgeKind::Gain} &&
                  Labels(signage.badges) == "-18 dB" &&
                  ClipBadgeFillColor(ClipBadgeKind::Gain).a ==
                      ui::theme::kClipBadgeFill.a,
              "the gain reads on a plate like the speed's: " +
                  Labels(signage.badges));
        Check(Signage(document, kShot).badges.empty(),
              "the picture linked to the sound says nothing of it");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  Signage(document, kSound).badges.empty(),
              "an undo of the gain takes the badge away: " + message);
    });

    Test("the angle a multicam group takes off screen is a state, not a badge",
         [] {
             EditLog log;
             Document document = Montage(log);
             InsertClipOperation over;
             over.track_id = kOverlay;
             over.source_id = kMedia;
             over.source_in = {300, 25};
             over.duration = {50, 25};
             over.timeline_in = {50, 25};
             over.clip_id = kClose;
             Apply(document, log, over, "insert the close-up over the shot");
             Check(!Signage(document, kShot).hidden_angle &&
                       !Signage(document, kClose).hidden_angle,
                   "two stacked shots outside a group hide nothing");

             AddMulticamGroupOperation add;
             EditError error = EditError::None;
             std::string message;
             if (!BuildAddMulticamGroup(document, "",
                                        {{kShot, "Large"}, {kClose, "Serré"}},
                                        kShot, add, error, message)) {
                 throw std::runtime_error("group: " + message);
             }
             Apply(document, log, add, "add_multicam_group");
             const ClipSignage wide = Signage(document, kShot);
             const ClipSignage close = Signage(document, kClose);
             Check(!wide.hidden_angle, "the active angle is on screen");
             Check(close.hidden_angle, "the other angle is off screen");
             Check(wide.badges.empty() && close.badges.empty(),
                   "being an angle adds no badge: " + Labels(close.badges));
             Check(!Signage(document, kSound).hidden_angle,
                   "the sound of an angle is untouched (Multicam.h)");

             SetMulticamActiveAngleOperation change;
             if (!BuildSetMulticamActiveAngle(document, "", kClose, change,
                                              error, message)) {
                 throw std::runtime_error("angle: " + message);
             }
             Apply(document, log, change, "set_multicam_active_angle");
             Check(Signage(document, kShot).hidden_angle &&
                       !Signage(document, kClose).hidden_angle,
                   "switching the angle switches which clip is dimmed");

             // What the hidden angle carries is still said: it is dimmed,
             // not emptied.
             SetSpeed(document, log, kShot, {1, 2});
             const ClipSignage slowed = Signage(document, kShot);
             Check(slowed.hidden_angle && Labels(slowed.badges) == "50%",
                   "a hidden angle keeps its badges: " + Labels(slowed.badges));

             SetMulticamActiveAngleOperation none;
             if (!BuildSetMulticamActiveAngle(document, add.group.id, "", none,
                                              error, message)) {
                 throw std::runtime_error("no angle: " + message);
             }
             Apply(document, log, none, "show every angle");
             Check(!Signage(document, kShot).hidden_angle &&
                       !Signage(document, kClose).hidden_angle,
                   "a group with no active angle hides nothing");

             Check(log.Undo(document, error, message), "undo: " + message);
             Check(Signage(document, kShot).hidden_angle,
                   "an undo dims the angle again");
             Apply(document, log, RemoveMulticamGroupOperation{add.group.id},
                   "remove_multicam_group");
             Check(!Signage(document, kShot).hidden_angle &&
                       !Signage(document, kClose).hidden_angle,
                   "without the group, nothing is hidden");
         });

    // Hors lot -- l'auteur, 29 octobre 2026 : D grise le plan.
    Test("a disabled clip is a state, its badges kept", [] {
        EditLog log;
        Document document = Montage(log);
        SetSpeed(document, log, kShot, {1, 2});
        Check(!Signage(document, kShot).disabled,
              "an ordinary clip is not disabled");
        Apply(document, log, SetClipEnabledOperation{{{kShot, false}}},
              "set_clip_enabled");
        const ClipSignage disabled = Signage(document, kShot);
        Check(disabled.disabled && !disabled.hidden_angle,
              "a disabled clip is greyed, and is no hidden angle");
        Check(Labels(disabled.badges) == "50%",
              "a disabled clip keeps its badges: " + Labels(disabled.badges));
        Check(!Signage(document, kSound).disabled,
              "its sound, not named, is untouched");
        EditError error = EditError::None;
        std::string message;
        Check(log.Undo(document, error, message) &&
                  !Signage(document, kShot).disabled,
              "an undo enables it again: " + message);
    });

    Test("every label stays within the glyphs the Metal atlas holds", [] {
        // MetalUiAtlas.mm rasterises U+0020 to U+00FF, and a label is laid
        // out one glyph a code point.
        std::vector<std::string> labels = {ClipSpeedBadgeLabel({2, 1}),
                                           ClipSpeedBadgeLabel({1, 2}),
                                           ClipSpeedBadgeLabel({4, 3})};
        for (ClipBadgeKind kind :
             {ClipBadgeKind::Agent, ClipBadgeKind::Motion,
              ClipBadgeKind::Animation, ClipBadgeKind::Framing,
              ClipBadgeKind::Grade}) {
            labels.push_back(ClipBadgeLabel(kind));
        }
        for (const std::string& label : labels) {
            const std::vector<int> points = CodePoints(label);
            Check(!points.empty(), "an empty label");
            for (int point : points) {
                Check(point >= 0x20 && point <= 0xFF,
                      "\"" + label + "\" holds a glyph outside the atlas");
            }
            Check(ClipBadgeGlyphCount(label) == static_cast<int>(points.size()),
                  "\"" + label + "\" is measured in glyphs, not in bytes");
        }
        Check(kGrade.size() == 6 && ClipBadgeGlyphCount(kGrade) == 5,
              "the accented label is five glyphs in six bytes");
        Check(Near(ClipBadgeWidth(kGrade), ClipBadgeWidth("CADRE")),
              "and as wide as an unaccented one of five letters");
        Check(ClipBadgeGlyphCount("") == 0, "an empty label has no glyph");
    });

    // ---- Where the badges sit -----------------------------------------

    Test("one badge is a plate on the badge row, its label inside", [] {
        const std::vector<PlacedClipBadge> placed =
            PlaceClipBadges({{ClipBadgeKind::Speed, "50%"}}, 200.0);
        Check(placed.size() == 1, "one badge placed");
        if (placed.size() != 1) {
            return;
        }
        const PlacedClipBadge& badge = placed.front();
        Check(badge.kind == ClipBadgeKind::Speed && badge.label == "50%",
              "it is the badge given");
        // Pinned in points: a change of token is reread here.
        Check(Near(badge.x, 5.0) && Near(badge.y, 7.0) &&
                  Near(badge.width, 22.6) && Near(badge.height, 11.0),
              "the plate is at " + std::to_string(badge.x) + ", " +
                  std::to_string(badge.y) + ", " + std::to_string(badge.width) +
                  " x " + std::to_string(badge.height));
        Check(Near(badge.text_x, 6.0) && Near(badge.text_y, 5.0),
              "the label's quad starts at " + std::to_string(badge.text_x) +
                  ", " + std::to_string(badge.text_y));
        // The atlas draws a glyph one point inside its quad: the label's
        // advances then leave the same padding on both sides of the plate.
        const double penStart = badge.text_x + 1.0;
        const double penEnd =
            penStart + 3 * ui::theme::kTimelineClipBadgeGlyphAdvance;
        Check(Near(penStart - badge.x, ui::theme::kTimelineClipBadgePadding) &&
                  Near(badge.x + badge.width - penEnd,
                       ui::theme::kTimelineClipBadgePadding),
              "the label is centred in its plate");
        Check(badge.text_width >= 3 * ui::theme::kTimelineClipBadgeGlyphAdvance,
              "the atlas is given room for every glyph");
    });

    Test("badges follow each other left to right without touching", [] {
        const std::vector<PlacedClipBadge> placed =
            PlaceClipBadges(kEverything, 400.0);
        Check(Labels(placed) == Labels(kEverything),
              "a wide clip shows all four, in order: " + Labels(placed));
        for (size_t index = 1; index < placed.size(); ++index) {
            const PlacedClipBadge& before = placed[index - 1];
            const PlacedClipBadge& after = placed[index];
            Check(Near(after.x - (before.x + before.width),
                       ui::theme::kTimelineClipBadgeGap),
                  after.label + " starts one gap after " + before.label);
        }
        for (const PlacedClipBadge& badge : placed) {
            Check(Near(badge.width, ClipBadgeWidth(badge.label)),
                  badge.label + " is as wide as its label asks");
            Check(Near(badge.y, placed.front().y) &&
                      Near(badge.height, placed.front().height),
                  badge.label + " is on the same row");
        }
    });

    Test("the row stays inside the clip and above its name", [] {
        // Every quarter of a point from nothing to 320 points.
        for (int step = 0; step <= 1280; ++step) {
            const double width = step * 0.25;
            for (double leading : {0.0, 22.0}) {
                for (const PlacedClipBadge& badge :
                     PlaceClipBadges(kEverything, width, leading)) {
                    Check(badge.x >= ui::theme::kTimelineClipBadgeInset,
                          "a plate starts left of the row's inset");
                    Check(badge.x + badge.width <=
                              width - ui::theme::kTimelineClipBadgeEndInset +
                                  1e-9,
                          "a plate runs past the clip's right edge at width " +
                              std::to_string(width));
                    Check(badge.y >= ui::theme::kTimelineClipBodyTop,
                          "a plate starts above the clip's body");
                    Check(badge.y + badge.height <=
                              ui::theme::kTimelineClipNameBandTop,
                          "a plate covers the clip's name");
                    Check(badge.text_x >= badge.x &&
                              badge.text_x < badge.x + badge.width,
                          "a label starts outside its plate");
                }
            }
        }
    });

    Test("a clip too narrow for its first badge shows none", [] {
        const std::vector<ClipBadge> speed = {{ClipBadgeKind::Speed, "50%"}};
        const double room = RoomFor(speed);
        Check(Near(room, 29.6), "\"50%\" needs " + std::to_string(room));
        Check(PlaceClipBadges(speed, room + 0.01).size() == 1,
              "just enough room shows the badge");
        Check(PlaceClipBadges(speed, room - 0.01).empty(),
              "just too little shows nothing");
        Check(PlaceClipBadges(kEverything, room - 0.01).empty(),
              "and nothing of what comes after it either");
        for (double width :
             {0.0, -40.0, 1.0, std::numeric_limits<double>::quiet_NaN()}) {
            Check(PlaceClipBadges(kEverything, width).empty(),
                  "a clip with no width shows nothing");
        }
        Check(PlaceClipBadges({}, 400.0).empty(),
              "a clip that carries nothing shows nothing, however wide");
    });

    Test("as a clip narrows, badges go last first and the speed last", [] {
        // The widths at which each badge is given up, from the tokens.
        size_t shownBefore = kEverything.size() + 1;
        std::vector<bool> seen(kEverything.size() + 1, false);
        // Every fifth of a point from 240 points down to nothing.
        for (int step = 1200; step >= 0; --step) {
            const double width = step * 0.2;
            const std::vector<PlacedClipBadge> placed =
                PlaceClipBadges(kEverything, width);
            Check(placed.size() <= shownBefore,
                  "a narrower clip shows more badges at width " +
                      std::to_string(width));
            shownBefore = placed.size();
            seen[placed.size()] = true;
            for (size_t index = 0; index < placed.size(); ++index) {
                Check(placed[index].kind == kEverything[index].kind,
                      "at width " + std::to_string(width) +
                          " the row is not the head of the priority list: " +
                          Labels(placed));
            }
        }
        for (size_t count = 0; count < seen.size(); ++count) {
            Check(seen[count], "no width shows exactly " +
                                   std::to_string(count) + " badges");
        }
        // Each threshold, one by one: the grade goes first, then the
        // framing, then the animation, and the speed last.
        for (size_t kept = kEverything.size(); kept > 0; --kept) {
            const std::vector<ClipBadge> head(kEverything.begin(),
                                              kEverything.begin() + kept);
            const double room = RoomFor(head);
            Check(PlaceClipBadges(kEverything, room + 0.01).size() == kept,
                  "room for " + Labels(head) + " shows exactly that");
            Check(PlaceClipBadges(kEverything, room - 0.01).size() == kept - 1,
                  "a point less gives up " + head.back().label);
        }
    });

    Test("a badge that does not fit hides every badge behind it", [] {
        const std::vector<ClipBadge> title = {
            {ClipBadgeKind::Motion, "MOTION"},
            {ClipBadgeKind::Animation, "ANIM"}};
        const double roomForSecondAlone =
            RoomFor({{ClipBadgeKind::Animation, "ANIM"}});
        const double roomForFirst =
            RoomFor({{ClipBadgeKind::Motion, "MOTION"}});
        Check(roomForSecondAlone < roomForFirst,
              "the second badge is the shorter one");
        const double between = (roomForSecondAlone + roomForFirst) / 2.0;
        Check(PlaceClipBadges(title, between).empty(),
              "a shorter badge of a lower priority does not jump the queue");
        Check(Labels(PlaceClipBadges(title, roomForFirst + 0.01)) == "MOTION",
              "the first badge alone once it fits");
    });

    Test("the row starts after the sync badge of a drifted clip", [] {
        const std::vector<ClipBadge> speed = {{ClipBadgeKind::Speed, "2x"}};
        // "+3f" as timelineRenderData draws it since PRD25: one plate of the
        // row, as wide as a badge of three glyphs.
        const double kSyncBadge = ClipBadgeWidth("+3f");
        const std::vector<PlacedClipBadge> placed =
            PlaceClipBadges(speed, 200.0, kSyncBadge);
        Check(placed.size() == 1, "the speed is placed beside the drift");
        if (placed.size() == 1) {
            Check(Near(placed.front().x, ui::theme::kTimelineClipBadgeInset +
                                             kSyncBadge +
                                             ui::theme::kTimelineClipBadgeGap),
                  "one gap after the sync badge, at " +
                      std::to_string(placed.front().x));
        }
        const double room = RoomFor(speed, kSyncBadge);
        Check(room > RoomFor(speed), "the drift takes room from the row");
        Check(PlaceClipBadges(speed, room + 0.01, kSyncBadge).size() == 1 &&
                  PlaceClipBadges(speed, room - 0.01, kSyncBadge).empty(),
              "and the speed is given up sooner beside it");
        Check(PlaceClipBadges(speed, room - 0.01).size() == 1,
              "the same clip without a drift keeps its speed");
        for (double leading :
             {0.0, -5.0, std::numeric_limits<double>::quiet_NaN()}) {
            const std::vector<PlacedClipBadge> alone =
                PlaceClipBadges(speed, 200.0, leading);
            Check(alone.size() == 1 &&
                      Near(alone.front().x, ui::theme::kTimelineClipBadgeInset),
                  "nothing before the row leaves it at its inset");
        }
    });

    // ---- PRD25: the agent's mark, and the sync badge's plate --------------

    Test("the agent's mark comes first, and only when it is handed in", [] {
        EditLog log;
        Document document = Montage(log);
        ClipFraming fill;
        fill.mode = FramingMode::Fill;
        SetSpeed(document, log, kPlain, {1, 2});
        SetFraming(document, log, kPlain, fill);
        const DocumentClip* plain = document.FindClip(kPlain);
        if (plain == nullptr) throw std::runtime_error("no plain clip");
        const ClipSignage unmarked = ClipSignageFor(document, *plain);
        Check(
            Labels(unmarked.badges) == "50% CADRE",
            "without the journal's word, no mark: " + Labels(unmarked.badges));
        const ClipSignage marked = ClipSignageFor(document, *plain, true);
        Check(Labels(marked.badges) == "AGENT 50% CADRE" &&
                  marked.badges.front().kind == ClipBadgeKind::Agent,
              "marked, the agent's badge leads the row: " +
                  Labels(marked.badges));
        const DocumentClip* shot = document.FindClip(kShot);
        if (shot == nullptr) throw std::runtime_error("no shot");
        Check(Labels(ClipSignageFor(document, *shot, true).badges) == "AGENT",
              "a clip that carries nothing else shows the mark alone");
        // On a narrow clip the mark is what stays.
        const double room = RoomFor({marked.badges[0]}) + 0.01;
        Check(Labels(PlaceClipBadges(marked.badges, room)) == "AGENT",
              "narrow, the mark outlasts the rest: " +
                  Labels(PlaceClipBadges(marked.badges, room)));
    });

    Test("the agent's mark is drawn in the agent's colour, the rest dark", [] {
        const auto same = [](const ui::theme::Color& left,
                             const ui::theme::Color& right) {
            return left.r == right.r && left.g == right.g &&
                   left.b == right.b && left.a == right.a;
        };
        Check(same(ClipBadgeFillColor(ClipBadgeKind::Agent),
                   ui::theme::kAgentMark) &&
                  same(ClipBadgeTextColor(ClipBadgeKind::Agent),
                       ui::theme::kTextOnAccent),
              "the mark: the agent's colour under dark text");
        for (ClipBadgeKind kind :
             {ClipBadgeKind::Speed, ClipBadgeKind::Motion,
              ClipBadgeKind::Animation, ClipBadgeKind::Framing,
              ClipBadgeKind::Grade}) {
            Check(same(ClipBadgeFillColor(kind), ui::theme::kClipBadgeFill) &&
                      same(ClipBadgeTextColor(kind), ui::theme::kClipBadgeText),
                  "every other badge keeps PRD24's dark plate");
        }
    });

    Test("every plate of the row is placed by one function", [] {
        const std::vector<PlacedClipBadge> placed =
            PlaceClipBadges(kEverything, 400.0);
        for (const PlacedClipBadge& badge : placed) {
            const PlacedClipBadge plate = PlaceClipPlate(badge.label, badge.x);
            Check(Near(plate.x, badge.x) && Near(plate.y, badge.y) &&
                      Near(plate.width, badge.width) &&
                      Near(plate.height, badge.height) &&
                      Near(plate.text_x, badge.text_x) &&
                      Near(plate.text_y, badge.text_y) &&
                      Near(plate.text_width, badge.text_width),
                  badge.label + " is the plate PlaceClipPlate gives");
        }
    });

    Test("the sync badge's label sits in the middle of its plate", [] {
        // "+3f", at the row's inset, as timelineRenderData draws it. Its
        // label's quad used to start two points under the plate's top, and
        // the capitals, 3.8 to 10.8 points under a quad's top (UiTheme.h),
        // ran past the plate's 11 points onto the waveform.
        const PlacedClipBadge plate =
            PlaceClipPlate("+3f", ui::theme::kTimelineClipBadgeInset);
        Check(Near(plate.x, 5.0) && Near(plate.y, 7.0) &&
                  Near(plate.width, 22.6) && Near(plate.height, 11.0),
              "the plate is at " + std::to_string(plate.x) + ", " +
                  std::to_string(plate.y) + ", " + std::to_string(plate.width) +
                  " x " + std::to_string(plate.height));
        Check(Near(plate.text_y, plate.y - 2.0),
              "the label's quad starts two points above the plate, at " +
                  std::to_string(plate.text_y));
        constexpr double kCapitalsTop = 3.8;
        constexpr double kCapitalsHeight = 7.0;
        const double over = plate.text_y + kCapitalsTop - plate.y;
        const double under = plate.y + plate.height -
                             (plate.text_y + kCapitalsTop + kCapitalsHeight);
        Check(over >= 1.0 && under >= 1.0 && std::fabs(over - under) <= 1.0,
              "the capitals are in the middle of the plate: " +
                  std::to_string(over) + " above, " + std::to_string(under) +
                  " under");
        const double penStart = plate.text_x + 1.0;
        const double penEnd =
            penStart + 3 * ui::theme::kTimelineClipBadgeGlyphAdvance;
        Check(Near(penStart - plate.x, ui::theme::kTimelineClipBadgePadding) &&
                  Near(plate.x + plate.width - penEnd,
                       ui::theme::kTimelineClipBadgePadding) &&
                  plate.text_width >=
                      3 * ui::theme::kTimelineClipBadgeGlyphAdvance,
              "and across it");
    });

    Test("a document's badges go through the layout as they are", [] {
        EditLog log;
        Document document = Montage(log);
        ClipFraming fill;
        fill.mode = FramingMode::Fill;
        SetSpeed(document, log, kPlain, {2, 5});
        SetFraming(document, log, kPlain, fill);
        SetGrade(document, log, kPlain, true);
        const ClipSignage signage = Signage(document, kPlain);
        Check(Labels(PlaceClipBadges(signage.badges, 400.0)) ==
                  "40% CADRE " + kGrade,
              "wide: " + Labels(PlaceClipBadges(signage.badges, 400.0)));
        const double room =
            RoomFor({signage.badges[0], signage.badges[1]}) + 0.01;
        Check(Labels(PlaceClipBadges(signage.badges, room)) == "40% CADRE",
              "narrower, the grade goes first: " +
                  Labels(PlaceClipBadges(signage.badges, room)));
    });

    return failures == 0 ? 0 : 1;
}
