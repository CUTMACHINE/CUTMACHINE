// EF1 -- docs/roadmap/EFFETS-2026-10.md. Where an effect dropped on the
// timeline lands, and the operation it applies.

#include "EditLog.h"
#include "EffectsLibrary.h"

#include <iostream>
#include <string>
#include <variant>

#include "TestSupport.h"

namespace {

// Two adjacent shots at 25 fps, then a gap, then a third, on a picture
// track and its sound track. Each rush is 40 seconds long, so every clip
// has handles either side except where its source_in says otherwise.
Document Fixture() {
    Document document;
    document.sequence.frame_rate = {25, 1};
    document.sources = {
        {"01K90000000000000000000001", "A.MP4", {25, 1}, {1000, 25}},
    };
    const auto clip = [](const char* id, int64_t sourceIn, int64_t duration,
                         int64_t timelineIn) {
        DocumentClip made;
        made.id = id;
        made.source_id = "01K90000000000000000000001";
        made.source_in = {sourceIn, 25};
        made.duration = {duration, 25};
        made.timeline_in = {timelineIn, 25};
        return made;
    };
    DocumentTrack video;
    video.id = "01K90000000000000000000002";
    video.kind = "video";
    video.index = 0;
    video.clips = {clip("01K90000000000000000000003", 100, 50, 0),
                   clip("01K90000000000000000000004", 300, 50, 50),
                   clip("01K90000000000000000000005", 500, 50, 150)};
    DocumentTrack sound = video;
    sound.id = "01K90000000000000000000006";
    sound.kind = "audio";
    sound.index = 1;
    sound.clips = {clip("01K90000000000000000000007", 100, 50, 0),
                   clip("01K90000000000000000000008", 4, 50, 50)};
    document.sequence.tracks = {video, sound};
    return document;
}

bool Applies(Document& document, const Operation& operation,
             std::string& message) {
    EditLog log;
    EditError error = EditError::None;
    return log.Apply(document, operation, error, message);
}

}  // namespace

int main() {
    using namespace ui::effects;
    const Effect* dissolve = FindEffect("cross_dissolve");
    const Effect* black = FindEffect("dip_to_black");
    const Effect* fade = FindEffect("cross_fade");
    Check(dissolve && black && fade && FindEffect("nothing") == nullptr,
          "the catalogue names its effects by a stable id");
    if (!dissolve || !black || !fade) return 1;

    const Document document = Fixture();
    const Ulid video = "01K90000000000000000000002";
    const Ulid sound = "01K90000000000000000000006";

    // The nearer edge of the clip under the pointer.
    const DropTarget tail =
        FindDropTarget(document, *dissolve, video, {40, 25});
    const DropTarget head =
        FindDropTarget(document, *dissolve, video, {55, 25});
    Check(tail.valid && head.valid &&
              tail.left_clip_id == "01K90000000000000000000003" &&
              tail.right_clip_id == "01K90000000000000000000004" &&
              head.left_clip_id == tail.left_clip_id &&
              head.right_clip_id == tail.right_clip_id &&
              tail.cut == RationalTime{50, 25},
          "either side of a cut, the drop finds the same cut");

    const DropTarget lonely =
        FindDropTarget(document, *dissolve, video, {90, 25});
    Check(
        !lonely.valid && lonely.refusal.find("bord") != std::string::npos,
        "a dissolve refuses an edge with nothing beyond it: " + lonely.refusal);
    const DropTarget gap =
        FindDropTarget(document, *dissolve, video, {120, 25});
    Check(!gap.valid, "a drop in a gap lands nowhere");
    const DropTarget wrongTrack =
        FindDropTarget(document, *fade, video, {40, 25});
    Check(!wrongTrack.valid &&
              wrongTrack.refusal.find("piste son") != std::string::npos,
          "a cross fade is refused on a picture track");

    // A dissolve: one second at 25 fps, shortened to an even 24, centred.
    {
        const DropEdit edit = EditForDrop(document, *dissolve, tail);
        const auto* add =
            edit.operation
                ? std::get_if<AddTransitionOperation>(&*edit.operation)
                : nullptr;
        Check(add != nullptr && add->transition.type == "cross_dissolve" &&
                  add->transition.duration == RationalTime{24, 25} &&
                  add->transition.alignment == TransitionAlignment::Center,
              "a dissolve dropped on a cut lasts a second, centred");
        Document applied = document;
        std::string message;
        Check(edit.operation && Applies(applied, *edit.operation, message),
              "the dissolve applies: " + message);

        // Dropped again with another type, the cut's transition is
        // replaced, its length kept.
        const DropEdit replace = EditForDrop(
            applied, *black, FindDropTarget(applied, *black, video, {40, 25}));
        const auto* update =
            replace.operation
                ? std::get_if<UpdateTransitionOperation>(&*replace.operation)
                : nullptr;
        Check(update != nullptr && update->type == "dip_to_color" &&
                  update->color == "#000000" &&
                  update->duration == RationalTime{24, 25},
              "a second drop replaces the type and keeps the length");
        const DropEdit same =
            EditForDrop(applied, *dissolve,
                        FindDropTarget(applied, *dissolve, video, {40, 25}));
        Check(!same.operation && !same.refusal.empty(),
              "the same effect twice is refused");
    }

    // A dip at the edge of the track: an opening fade from black.
    {
        const DropTarget opening =
            FindDropTarget(document, *black, video, {5, 25});
        const DropEdit edit = EditForDrop(document, *black, opening);
        const auto* add =
            edit.operation
                ? std::get_if<AddTransitionOperation>(&*edit.operation)
                : nullptr;
        Check(
            opening.valid && opening.left_clip_id.empty() && add &&
                add->transition.alignment == TransitionAlignment::StartAtCut &&
                add->transition.duration == RationalTime{25, 25},
            "a fade to black dropped on a track's first shot opens it");
        Document applied = document;
        std::string message;
        Check(edit.operation && Applies(applied, *edit.operation, message),
              "the opening applies: " + message);
    }

    // A cross fade: shortened to the incoming sound's four frames of handle.
    {
        const DropEdit edit = EditForDrop(
            document, *fade, FindDropTarget(document, *fade, sound, {45, 25}));
        const auto* add =
            edit.operation
                ? std::get_if<AddTransitionOperation>(&*edit.operation)
                : nullptr;
        Check(add != nullptr && add->transition.type == "cross_fade" &&
                  add->transition.duration == RationalTime{8, 25},
              "a cross fade is shortened to the handles either side");
        Document applied = document;
        std::string message;
        Check(edit.operation && Applies(applied, *edit.operation, message),
              "the cross fade applies: " + message);

        Document bare = document;
        bare.sequence.tracks[1].clips[1].source_in = {0, 25};
        const DropEdit none = EditForDrop(
            bare, *fade, FindDropTarget(bare, *fade, sound, {45, 25}));
        Check(
            !none.operation && none.refusal.find("images") != std::string::npos,
            "with no handle, the drop says why: " + none.refusal);
    }

    // EF3 -- the Gaussian blur lands on the clip under the pointer, the
    // whole clip, and sets its blur to the default once.
    {
        const Effect* blur = FindEffect("gaussian_blur");
        Check(blur != nullptr && IsClipEffect(*blur) &&
                  !IsClipEffect(*dissolve) &&
                  std::string(CategoryTitle(blur->category)) == "Effets vidéo",
              "the blur is a clip effect, listed under « Effets vidéo »");
        if (blur == nullptr) return 1;

        DocumentSequence vertical;
        vertical.width = 1080;
        vertical.height = 1920;
        DocumentSequence uhd;
        uhd.width = 3840;
        uhd.height = 2160;
        DocumentSequence hd;
        hd.width = 1280;
        hd.height = 720;
        const EffectParamValue onVertical = DefaultClipBlur(vertical);
        const EffectParamValue onUhd = DefaultClipBlur(uhd);
        const EffectParamValue onHd = DefaultClipBlur(hd);
        Check(DefaultClipBlur(document.sequence).num == 10 &&
                  DefaultClipBlur(document.sequence).den == 1 &&
                  onVertical.num == 10 && onVertical.den == 1 &&
                  onUhd.num == 20 && onUhd.den == 1 && onHd.num == 20 &&
                  onHd.den == 3,
              "the default blur is a 108th of the frame's short side, exact");

        // Anywhere on the clip, not its nearest cut.
        const DropTarget early =
            FindDropTarget(document, *blur, video, {2, 25});
        const DropTarget late =
            FindDropTarget(document, *blur, video, {48, 25});
        Check(early.valid && late.valid &&
                  early.clip_id == "01K90000000000000000000003" &&
                  late.clip_id == early.clip_id && early.left_clip_id.empty() &&
                  early.right_clip_id.empty(),
              "the blur's target is the clip under the pointer, whole");
        const DropTarget onSound =
            FindDropTarget(document, *blur, sound, {10, 25});
        Check(!onSound.valid &&
                  onSound.refusal.find("plan vidéo") != std::string::npos,
              "a sound clip takes no blur: " + onSound.refusal);
        const DropTarget inGap =
            FindDropTarget(document, *blur, video, {120, 25});
        Check(!inGap.valid && inGap.clip_id.empty(),
              "a blur dropped in a gap lands nowhere");

        const DropEdit edit = EditForDrop(document, *blur, early);
        const auto* set =
            edit.operation ? std::get_if<SetClipBlurOperation>(&*edit.operation)
                           : nullptr;
        Check(set != nullptr && set->clip_id == early.clip_id &&
                  set->blur.num == 10 && set->blur.den == 1 &&
                  edit.summary == "Flou gaussien · 10 px",
              "the drop sets the default blur, one operation: " + edit.summary);
        Document applied = document;
        std::string message;
        Check(edit.operation && Applies(applied, *edit.operation, message) &&
                  applied.FindClip(early.clip_id)->blur.num == 10,
              "the blur applies: " + message);
        const DropEdit again = EditForDrop(
            applied, *blur, FindDropTarget(applied, *blur, video, {20, 25}));
        Check(!again.operation &&
                  again.refusal.find("Inspecteur") != std::string::npos,
              "a second drop on a blurred clip is refused, not stacked: " +
                  again.refusal);

        // A selected clip, wherever the playhead is.
        const DropTarget selected =
            ClipEffectTarget(document, *blur, "01K90000000000000000000005");
        Check(
            selected.valid &&
                selected.clip_id == "01K90000000000000000000005" &&
                !ClipEffectTarget(document, *blur, "01K90000000000000000000007")
                     .valid,
            "a selected picture clip is a target, a selected sound is not");

        Document locked = document;
        locked.sequence.tracks[0].locked = true;
        Check(!FindDropTarget(locked, *blur, video, {2, 25}).valid,
              "a locked track's clips take no blur");
        Document zoomed = document;
        ClipKeyframe first;
        first.id = "01K90000000000000000000K01";
        first.time = {0, 25};
        first.value = {1, 1};
        ClipKeyframe second = first;
        second.id = "01K90000000000000000000K02";
        second.time = {10, 25};
        second.value = {6, 5};
        ClipParameterAnimation scale;
        scale.parameter = ClipAnimatedParameter::FramingScale;
        scale.keyframes = {first, second};
        zoomed.sequence.tracks[0].clips[0].animation = {scale};
        const DropTarget moving = FindDropTarget(zoomed, *blur, video, {2, 25});
        Check(!moving.valid &&
                  moving.refusal.find("cadrage") != std::string::npos,
              "a clip whose framing moves takes no blur: " + moving.refusal);
    }

    if (failures == 0) std::cout << "effects_library: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
