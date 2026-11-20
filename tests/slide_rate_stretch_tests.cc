// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md. Slide and rate stretch, the two
// timeline operations ported from FilmCraft (crates/edit/src/lib.rs, MIT OR
// Apache-2.0, commit 09aaf9b).
//
// What is pinned here: each applies exactly what it says (a slide moves one
// clip and its two neighbours' shared cuts, nothing else; a stretch keeps
// the source range and gives an exact speed), each undoes to the byte and
// redoes to the byte, each serializes canonically, and each refuses by name
// what it cannot do exactly -- never a rounded speed, never a slide into a
// gap. Portable: no AppKit, no Metal, no FFmpeg.

#include "ClipSpeed.h"
#include "EditLog.h"
#include "OperationFormat.h"
#include "Operations.h"
#include "Timeline.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

const Ulid kSourceA = "01K60000000000000000000001";
const Ulid kSourceB = "01K60000000000000000000002";
const Ulid kVideo = "01K60000000000000000000003";
const Ulid kAudio = "01K60000000000000000000004";
const Ulid kA = "01K60000000000000000000011";
const Ulid kB = "01K60000000000000000000012";
const Ulid kC = "01K60000000000000000000013";
const Ulid kSoundA = "01K60000000000000000000021";
const Ulid kSoundB = "01K60000000000000000000022";
const Ulid kSoundC = "01K60000000000000000000023";
const Ulid kGroupB = "01K60000000000000000000031";

DocumentClip Clip(const Ulid& id, const Ulid& source, RationalTime sourceIn,
                  RationalTime duration, RationalTime timelineIn) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = timelineIn;
    return clip;
}

// Three touching shots on one picture track, 10 frames each at 25 i/s,
// each with 100 frames of media on both sides.
Document Contiguous() {
    Document document;
    document.sources = {
        {kSourceA, "A.MP4", {25, 1}, {1000, 25}},
        {kSourceB, "B.MP4", {25, 1}, {1000, 25}},
    };
    document.sequence.tracks = {
        {kVideo,
         "video",
         0,
         {Clip(kA, kSourceA, {100, 25}, {10, 25}, {0, 25}),
          Clip(kB, kSourceA, {200, 25}, {10, 25}, {10, 25}),
          Clip(kC, kSourceB, {300, 25}, {10, 25}, {20, 25})}},
    };
    return document;
}

// The same, with a sound track under it; the middle shot and its sound
// share one link group.
Document Linked() {
    Document document = Contiguous();
    document.sequence.tracks.push_back(
        {kAudio,
         "audio",
         1,
         {Clip(kSoundA, kSourceA, {100, 25}, {10, 25}, {0, 25}),
          Clip(kSoundB, kSourceA, {200, 25}, {10, 25}, {10, 25}),
          Clip(kSoundC, kSourceB, {300, 25}, {10, 25}, {20, 25})}});
    document.FindClip(kB)->link_group_id = kGroupB;
    document.FindClip(kSoundB)->link_group_id = kGroupB;
    return document;
}

// Three shots with gaps between them: 0-10, 20-30, 40-50.
Document Spaced() {
    Document document = Contiguous();
    document.FindClip(kB)->timeline_in = {20, 25};
    document.FindClip(kC)->timeline_in = {40, 25};
    return document;
}

bool CanonicalRoundTrip(const Operation& operation) {
    const std::string json = SerializeOperation(operation);
    Operation parsed = RemoveClipOperation{};
    EditError error = EditError::None;
    std::string message;
    return DeserializeOperation(json, parsed, error, message) &&
           SerializeOperation(parsed) == json;
}

// Applies, then checks the journal entry serializes canonically, undo
// gives back the bytes before and redo the bytes after.
bool ApplyAndReverse(Document& document, const Operation& operation,
                     const std::string& what) {
    EditLog log;
    EditError error = EditError::None;
    std::string message;
    const std::string before = document.SaveToString();
    if (!log.Apply(document, operation, error, message)) {
        Check(false, what + " applies: " + std::string(EditErrorName(error)) +
                         ": " + message);
        return false;
    }
    const std::string after = document.SaveToString();
    Check(CanonicalRoundTrip(log.AppliedEntries().back().op),
          what + ": the journal entry round-trips canonically");
    Check(CanonicalRoundTrip(operation),
          what + ": the operation as asked round-trips canonically");
    Document undone = document;
    Check(log.Undo(undone, error, message) && undone.SaveToString() == before,
          what + ": undo restores the bytes: " + message);
    Check(log.Redo(undone, error, message) && undone.SaveToString() == after,
          what + ": redo replays the bytes: " + message);
    return true;
}

// The refusal `operation` meets: its error, and its message through
// `message`. The document and the journal are checked untouched.
EditError Refusal(const Document& document, const Operation& operation,
                  std::string& message) {
    Document copy = document;
    EditLog log;
    EditError error = EditError::None;
    message.clear();
    if (log.Apply(copy, operation, error, message)) return EditError::None;
    Check(copy.SaveToString() == document.SaveToString() &&
              log.AppliedCount() == 0,
          "a refused operation leaves the document and the journal alone");
    return error;
}

bool Contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

}  // namespace

int main() {
    Test("slide moves a clip between its neighbours, which absorb it", [] {
        Document document = Contiguous();
        const RationalTime durationBefore = Timeline(document).Duration();
        if (!ApplyAndReverse(document, SlideEditOperation{{kB}, {3, 25}, {}},
                             "slide by +3"))
            return;
        const DocumentClip* a = document.FindClip(kA);
        const DocumentClip* b = document.FindClip(kB);
        const DocumentClip* c = document.FindClip(kC);
        Check(a->source_in == RationalTime{100, 25} &&
                  a->duration == RationalTime{13, 25} &&
                  a->timeline_in == RationalTime{0, 25},
              "the left neighbour lengthens by its tail");
        Check(b->source_in == RationalTime{200, 25} &&
                  b->duration == RationalTime{10, 25} &&
                  b->timeline_in == RationalTime{13, 25},
              "the slid clip keeps its source range and duration");
        Check(c->source_in == RationalTime{303, 25} &&
                  c->duration == RationalTime{7, 25} &&
                  c->timeline_in == RationalTime{23, 25} &&
                  c->timeline_in.add(c->duration) == RationalTime{30, 25},
              "the right neighbour shortens by its head, its tail stays");
        Check(Timeline(document).Duration() == durationBefore,
              "a slide leaves the program duration alone");
    });

    Test("slide to the left lengthens the right neighbour by its head", [] {
        Document document = Contiguous();
        if (!ApplyAndReverse(document, SlideEditOperation{{kB}, {-4, 25}, {}},
                             "slide by -4"))
            return;
        Check(document.FindClip(kA)->duration == RationalTime{6, 25} &&
                  document.FindClip(kB)->timeline_in == RationalTime{6, 25} &&
                  document.FindClip(kC)->source_in == RationalTime{296, 25} &&
                  document.FindClip(kC)->duration == RationalTime{14, 25} &&
                  document.FindClip(kC)->timeline_in == RationalTime{16, 25},
              "both neighbours follow the clip, in the other direction");
    });

    Test("slide carries linked members named with the clip", [] {
        Document document = Linked();
        if (!ApplyAndReverse(document,
                             SlideEditOperation{{kB, kSoundB}, {2, 25}, {}},
                             "linked slide"))
            return;
        for (const Ulid& id : {kB, kSoundB})
            Check(document.FindClip(id)->timeline_in == RationalTime{12, 25},
                  "member '" + id + "' slides");
        for (const Ulid& id : {kA, kSoundA})
            Check(document.FindClip(id)->duration == RationalTime{12, 25},
                  "left neighbour '" + id + "' absorbs");
        for (const Ulid& id : {kC, kSoundC})
            Check(document.FindClip(id)->source_in == RationalTime{302, 25} &&
                      document.FindClip(id)->duration == RationalTime{8, 25},
                  "right neighbour '" + id + "' absorbs");
    });

    Test("slide reads a retimed neighbour's head at its speed", [] {
        Document document = Contiguous();
        document.FindClip(kC)->speed = {1, 2};
        if (!ApplyAndReverse(document, SlideEditOperation{{kB}, {2, 25}, {}},
                             "slide against 1/2"))
            return;
        Check(document.FindClip(kC)->source_in == RationalTime{301, 25} &&
                  document.FindClip(kC)->duration == RationalTime{8, 25},
              "two frames of timeline are one frame of a 1/2 neighbour");
        std::string message;
        Check(Refusal(document, SlideEditOperation{{kB}, {1, 25}, {}},
                      message) != EditError::None,
              "a slide that opens the 1/2 neighbour inside a source frame is "
              "refused, as every head edit there is: " +
                  message);
    });

    Test("slide keeps a dissolve on its cut when the handles allow it", [] {
        Document document = Contiguous();
        DocumentTransition dissolve;
        dissolve.id = "01K60000000000000000000041";
        dissolve.track_id = kVideo;
        dissolve.left_clip_id = kA;
        dissolve.right_clip_id = kB;
        dissolve.duration = {4, 25};
        document.sequence.transitions = {dissolve};
        std::string validation;
        Check(document.Validate(validation),
              "fixture validates: " + validation);
        ApplyAndReverse(document, SlideEditOperation{{kB}, {2, 25}, {}},
                        "slide under a dissolve");
        // The cut it sits on moved with the clip; a dissolve that the new
        // handles cannot feed is Document::Validate's to refuse, as for a
        // roll. A's media ends at 1000/25: its tail at 996/25 leaves the
        // two frames the dissolve reads past the cut, at 999/25 it does not.
        Document starved = Contiguous();
        starved.FindClip(kA)->source_in = {986, 25};
        starved.sequence.transitions = {dissolve};
        Check(starved.Validate(validation), "fixture validates: " + validation);
        std::string message;
        Check(Refusal(starved, SlideEditOperation{{kB}, {3, 25}, {}},
                      message) == EditError::ValidationFailed &&
                  Contains(message, "handles"),
              "a dissolve the slide leaves without handles is refused: " +
                  message);
    });

    Test("slide refuses what it cannot do", [] {
        const Document document = Contiguous();
        std::string message;
        Check(Refusal(document, SlideEditOperation{{kA}, {1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "on each side") &&
                  Contains(message, "nothing ends where it starts"),
              "the first clip has no left neighbour: " + message);
        Check(Refusal(document, SlideEditOperation{{kC}, {1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "nothing starts where it ends"),
              "the last clip has no right neighbour: " + message);
        Check(Refusal(Spaced(), SlideEditOperation{{kB}, {1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "move_clip"),
              "a clip between gaps is not slid into them: " + message);
        Check(Refusal(document, SlideEditOperation{{kB}, {10, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, kC) && Contains(message, "positive"),
              "a slide that empties the right neighbour: " + message);
        Check(Refusal(document, SlideEditOperation{{kB}, {-10, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, kA),
              "a slide that empties the left neighbour: " + message);

        Document shortTail = Contiguous();
        shortTail.FindClip(kA)->source_in = {990, 25};
        Check(Refusal(shortTail, SlideEditOperation{{kB}, {1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "source range") && Contains(message, kA),
              "the left neighbour has no media past its tail: " + message);
        Document shortHead = Contiguous();
        shortHead.FindClip(kC)->source_in = {0, 25};
        Check(Refusal(shortHead, SlideEditOperation{{kB}, {-1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, kC),
              "the right neighbour has no media before its head: " + message);

        Check(Refusal(Linked(), SlideEditOperation{{kB, kSoundA}, {1, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "one linked group"),
              "clips of different groups are not slid together: " + message);
        Check(Refusal(document, SlideEditOperation{{kB, kB}, {1, 25}, {}},
                      message) == EditError::InvalidOperation,
              "a clip named twice: " + message);
        Check(Refusal(document,
                      SlideEditOperation{
                          {"01K60000000000000000000099"}, {1, 25}, {}},
                      message) == EditError::UnknownClip,
              "an unknown clip: " + message);
        Check(Refusal(document, SlideEditOperation{{kB}, {1, 0}, {}},
                      message) == EditError::InvalidOperation,
              "a delta without a rate: " + message);
        Check(Refusal(document, SlideEditOperation{{}, {1, 25}, {}}, message) ==
                  EditError::InvalidOperation,
              "no clip at all: " + message);
        Document locked = Contiguous();
        locked.sequence.tracks[0].locked = true;
        Check(Refusal(locked, SlideEditOperation{{kB}, {1, 25}, {}}, message) ==
                  EditError::LockedTrack,
              "a locked track: " + message);
    });

    Test("slide moves the boundary between three adjacent captions", [] {
        Document document;
        CaptionStyle style;
        style.id = "01K60000000000000000000051";
        document.sequence.caption_styles = {style};
        const auto carton = [&](const Ulid& id, int64_t in, const char* text) {
            DocumentClip clip;
            clip.id = id;
            clip.timeline_in = {in, 25};
            clip.duration = {10, 25};
            clip.include_audio = false;
            clip.caption_group_id = style.id;
            clip.caption_text = text;
            return clip;
        };
        document.sequence.tracks = {
            {"01K60000000000000000000052",
             "caption",
             0,
             {carton(kA, 0, "un"), carton(kB, 10, "deux"),
              carton(kC, 20, "trois")}}};
        if (!ApplyAndReverse(document, SlideEditOperation{{kB}, {3, 25}, {}},
                             "caption slide"))
            return;
        Check(document.FindClip(kA)->duration == RationalTime{13, 25} &&
                  document.FindClip(kB)->timeline_in == RationalTime{13, 25} &&
                  document.FindClip(kC)->duration == RationalTime{7, 25} &&
                  document.FindClip(kB)->caption_text == "deux",
              "the middle carton moves, the others absorb, no source is read");
    });

    Test("rate stretch keeps the source range and gives an exact speed", [] {
        Document document = Spaced();
        const RationalTime spanBefore = ClipSourceSpan(*document.FindClip(kB));
        if (!ApplyAndReverse(
                document,
                RateStretchOperation{kB, TrimEdge::Tail, {20, 25}, {}},
                "stretch to twice as long"))
            return;
        const DocumentClip* b = document.FindClip(kB);
        Check(b->speed.num == 1 && b->speed.den == 2,
              "ten frames over twenty play at 1/2");
        Check(b->source_in == RationalTime{200, 25} &&
                  ClipSourceSpan(*b) == spanBefore &&
                  b->timeline_in == RationalTime{20, 25} &&
                  b->duration == RationalTime{20, 25},
              "the head stays, the source range is the same");
    });

    Test("rate stretch by the head keeps the tail where it is", [] {
        Document document = Spaced();
        if (!ApplyAndReverse(
                document,
                RateStretchOperation{kB, TrimEdge::Head, {15, 25}, {}},
                "head stretch"))
            return;
        const DocumentClip* b = document.FindClip(kB);
        Check(b->speed.num == 2 && b->speed.den == 3 &&
                  b->timeline_in == RationalTime{15, 25} &&
                  b->timeline_in.add(b->duration) == RationalTime{30, 25},
              "fifteen frames from ten: 2/3, ending where it ended");
        Document faster = Spaced();
        if (!ApplyAndReverse(
                faster, RateStretchOperation{kB, TrimEdge::Tail, {5, 25}, {}},
                "shorter stretch"))
            return;
        Check(faster.FindClip(kB)->speed.num == 2 &&
                  faster.FindClip(kB)->speed.den == 1,
              "five frames from ten: 2/1, the gap after it grows");
    });

    Test("rate stretch back to the original duration gives back the bytes", [] {
        Document document = Spaced();
        const std::string original = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document,
                        RateStretchOperation{kB, TrimEdge::Tail, {20, 25}, {}},
                        error, message) &&
                  log.Apply(
                      document,
                      RateStretchOperation{kB, TrimEdge::Tail, {10, 25}, {}},
                      error, message),
              "two stretches apply: " + message);
        Check(document.SaveToString() == original,
              "1/2 then back is 1/1 again, spelled as it was");
    });

    Test("rate stretch takes the clip's whole link group", [] {
        Document document = Linked();
        document.FindClip(kC)->timeline_in = {30, 25};
        document.FindClip(kSoundC)->timeline_in = {30, 25};
        if (!ApplyAndReverse(
                document,
                RateStretchOperation{kSoundB, TrimEdge::Tail, {20, 25}, {}},
                "linked stretch"))
            return;
        for (const Ulid& id : {kB, kSoundB}) {
            const DocumentClip* clip = document.FindClip(id);
            Check(clip->speed.num == 1 && clip->speed.den == 2 &&
                      clip->duration == RationalTime{20, 25},
                  "member '" + id + "' takes the one speed");
        }
    });

    Test("rate stretch refuses what has no exact answer", [] {
        const Document document = Spaced();
        std::string message;
        Check(Refusal(document,
                      RateStretchOperation{kB, TrimEdge::Tail, {21, 25}, {}},
                      message) == EditError::Overlap,
              "a stretch into the next clip is an overlap, nothing "
              "ripples: " +
                  message);
        Check(
            Refusal(document,
                    RateStretchOperation{kB, TrimEdge::Tail, {1001, 25}, {}},
                    message) == EditError::InvalidOperation &&
                Contains(message, "1000"),
            "10/1001 has no spelling with terms at or below 1000: " + message);
        Check(Refusal(document,
                      RateStretchOperation{kA, TrimEdge::Head, {20, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "timeline_in"),
              "a head stretch before the start of the timeline: " + message);
        Check(Refusal(document,
                      RateStretchOperation{kB, TrimEdge::Tail, {0, 25}, {}},
                      message) == EditError::InvalidDuration,
              "a duration of nothing: " + message);

        Document ramped = Spaced();
        ClipKeyframe slow;
        slow.id = "01K60000000000000000000061";
        slow.time = {0, 25};
        slow.value = {1, 2};
        ClipKeyframe fast = slow;
        fast.id = "01K60000000000000000000062";
        fast.time = {9, 25};
        fast.value = {1, 1};
        ramped.FindClip(kB)->speed_ramp = {slow, fast};
        std::string validation;
        Check(ramped.Validate(validation), "ramp fixture: " + validation);
        Check(Refusal(ramped,
                      RateStretchOperation{kB, TrimEdge::Tail, {15, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "ramp"),
              "a speed ramp: " + message);

        Document frozen = Spaced();
        frozen.FindClip(kB)->speed = {0, 1};
        Check(Refusal(frozen,
                      RateStretchOperation{kB, TrimEdge::Tail, {15, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "freeze"),
              "a freeze: " + message);

        Document uneven = Linked();
        uneven.FindClip(kSoundB)->duration = {8, 25};
        Check(Refusal(uneven,
                      RateStretchOperation{kB, TrimEdge::Tail, {10, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, kSoundB) && Contains(message, "(R3)"),
              "a group whose members would leave with two speeds: " + message);

        Document lockedSound = Linked();
        lockedSound.FindClip(kC)->timeline_in = {30, 25};
        lockedSound.FindClip(kSoundC)->timeline_in = {30, 25};
        lockedSound.sequence.tracks[1].locked = true;
        Check(Refusal(lockedSound,
                      RateStretchOperation{kB, TrimEdge::Tail, {20, 25}, {}},
                      message) == EditError::LockedTrack,
              "a member on a locked track stops the stretch: " + message);
        Check(
            Refusal(
                document,
                RateStretchOperation{
                    "01K60000000000000000000099", TrimEdge::Tail, {20, 25}, {}},
                message) == EditError::UnknownClip,
            "an unknown clip: " + message);
    });

    Test("rate stretch refuses a caption", [] {
        Document document;
        CaptionStyle style;
        style.id = "01K60000000000000000000071";
        document.sequence.caption_styles = {style};
        DocumentClip carton;
        carton.id = kA;
        carton.timeline_in = {0, 25};
        carton.duration = {10, 25};
        carton.include_audio = false;
        carton.caption_group_id = style.id;
        carton.caption_text = "titre";
        document.sequence.tracks = {
            {"01K60000000000000000000072", "caption", 0, {carton}}};
        std::string message;
        Check(Refusal(document,
                      RateStretchOperation{kA, TrimEdge::Tail, {20, 25}, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "video and audio"),
              "a caption reads no source to stretch: " + message);
    });

    // DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. The stretch of Resolve's
    // Trim mode: it ripples.
    Test("DEC40: a rippled stretch moves what follows by the change", [] {
        Document document = Contiguous();
        const RationalTime durationBefore = Timeline(document).Duration();
        if (!ApplyAndReverse(document,
                             RateStretchOperation{
                                 kB, TrimEdge::Tail, {20, 25}, {}, true, {}},
                             "rippled stretch, twice as long"))
            return;
        const DocumentClip* b = document.FindClip(kB);
        Check(b->speed.num == 1 && b->speed.den == 2 &&
                  b->timeline_in == RationalTime{10, 25} &&
                  b->duration == RationalTime{20, 25} &&
                  b->source_in == RationalTime{200, 25},
              "the clip plays its ten frames over twenty, from its head");
        Check(document.FindClip(kC)->timeline_in == RationalTime{30, 25} &&
                  document.FindClip(kA)->timeline_in == RationalTime{0, 25} &&
                  Timeline(document).Duration() == durationBefore.add({10, 25}),
              "the next clip moves ten frames later, the one before stays, "
              "and the program is ten frames longer");

        Document shorter = Contiguous();
        if (!ApplyAndReverse(
                shorter,
                RateStretchOperation{kB, TrimEdge::Tail, {5, 25}, {}, true, {}},
                "rippled stretch, half as long"))
            return;
        Check(shorter.FindClip(kB)->speed.num == 2 &&
                  shorter.FindClip(kC)->timeline_in == RationalTime{15, 25},
              "half as long, at 2/1: the next clip closes the gap");
    });

    Test("DEC40: with ripple the head stays, whichever edge is dragged", [] {
        Document document = Contiguous();
        if (!ApplyAndReverse(document,
                             RateStretchOperation{
                                 kB, TrimEdge::Head, {15, 25}, {}, true, {}},
                             "rippled head stretch"))
            return;
        const DocumentClip* b = document.FindClip(kB);
        Check(b->speed.num == 2 && b->speed.den == 3 &&
                  b->timeline_in == RationalTime{10, 25} &&
                  document.FindClip(kC)->timeline_in == RationalTime{25, 25},
              "fifteen frames at 2/3 from where it began, as ripple_trim "
              "keeps a head; the next clip moves five frames");
    });

    Test("DEC40: the link group's tracks and the sync tracks follow", [] {
        // A cutaway track with a shot over B's end, and one after it.
        const Ulid kCutaways = "01K60000000000000000000005";
        const Ulid kOver = "01K60000000000000000000041";
        const Ulid kAfter = "01K60000000000000000000042";
        const auto withCutaways = [&] {
            Document document = Linked();
            document.sequence.tracks.push_back(
                {kCutaways,
                 "video",
                 2,
                 {Clip(kOver, kSourceB, {500, 25}, {6, 25}, {16, 25}),
                  Clip(kAfter, kSourceB, {600, 25}, {5, 25}, {25, 25})}});
            return document;
        };
        Document longer = withCutaways();
        if (!ApplyAndReverse(
                longer,
                RateStretchOperation{
                    kSoundB, TrimEdge::Tail, {20, 25}, {}, true, {kCutaways}},
                "rippled linked stretch with a sync track"))
            return;
        Check(longer.FindClip(kB)->duration == RationalTime{20, 25} &&
                  longer.FindClip(kSoundB)->duration == RationalTime{20, 25} &&
                  longer.FindClip(kC)->timeline_in == RationalTime{30, 25} &&
                  longer.FindClip(kSoundC)->timeline_in == RationalTime{30, 25},
              "picture and sound stretch together; on each track the next "
              "clip moves");
        Check(longer.FindClip(kOver)->timeline_in == RationalTime{16, 25} &&
                  longer.FindClip(kAfter)->timeline_in == RationalTime{35, 25},
              "on the sync track, a shot over the old end stays, the shot "
              "after it moves ten frames");

        Document shorter = withCutaways();
        if (!ApplyAndReverse(
                shorter,
                RateStretchOperation{
                    kB, TrimEdge::Tail, {5, 25}, {}, true, {kCutaways}},
                "rippled shorter stretch with a sync track"))
            return;
        // [15, 20) is taken out of the sync track.
        const DocumentClip* over = shorter.FindClip(kOver);
        Check(over && over->timeline_in == RationalTime{15, 25} &&
                  over->duration == RationalTime{2, 25} &&
                  over->source_in == RationalTime{500, 25} &&
                  shorter.FindClip(kAfter)->timeline_in == RationalTime{20, 25},
              "time taken out collapses the sync track through the same "
              "five frames: the shot over the old end loses the four that "
              "fell in them and plays on from its head, the shot after it "
              "moves back");
    });

    Test("DEC40: a rippled stretch refuses what it cannot do exactly", [] {
        std::string message;
        Check(Refusal(Contiguous(),
                      RateStretchOperation{
                          kB, TrimEdge::Tail, {20, 25}, {}, false, {kAudio}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "ripple"),
              "sync tracks without the ripple: " + message);
        Check(Refusal(Contiguous(),
                      RateStretchOperation{kB,
                                           TrimEdge::Tail,
                                           {20, 25},
                                           {},
                                           true,
                                           {"01K60000000000000000000099"}},
                      message) == EditError::UnknownTrack,
              "an unknown sync track: " + message);
        Document lockedFollower = Linked();
        lockedFollower.sequence.tracks[1].locked = true;
        lockedFollower.FindClip(kB)->link_group_id.clear();
        lockedFollower.FindClip(kSoundB)->link_group_id.clear();
        Check(Refusal(lockedFollower,
                      RateStretchOperation{
                          kB, TrimEdge::Tail, {20, 25}, {}, true, {kAudio}},
                      message) == EditError::LockedTrack,
              "a locked sync track stops the ripple: " + message);
        Document twice = Linked();
        twice.FindClip(kC)->link_group_id = kGroupB;
        twice.FindClip(kC)->source_in = {200, 25};
        Check(Refusal(twice,
                      RateStretchOperation{
                          kB, TrimEdge::Tail, {20, 25}, {}, true, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "shares track"),
              "two members on one track, whose ripples would collide: " +
                  message);
        Check(Refusal(Contiguous(),
                      RateStretchOperation{
                          kB, TrimEdge::Tail, {1001, 25}, {}, true, {}},
                      message) == EditError::InvalidOperation &&
                  Contains(message, "1000"),
              "a speed with no exact spelling, ripple or not: " + message);
    });

    Test("DEC40: the ripple is written only when set", [] {
        const std::string plain = SerializeOperation(
            RateStretchOperation{kB, TrimEdge::Tail, {20, 25}, {}});
        Check(!Contains(plain, "ripple") && !Contains(plain, "sync_track_ids"),
              "a stretch without ripple keeps the bytes it had: " + plain);
        const std::string rippled = SerializeOperation(RateStretchOperation{
            kB, TrimEdge::Head, {20, 25}, {}, true, {kAudio}});
        Check(
            rippled ==
                "{\"type\":\"RateStretch\",\"clip_id\":\"" + kB +
                    "\",\"edge\":\"Head\",\"duration\":{\"value\":20,"
                    "\"rate\":25},\"ripple\":true,\"sync_track_ids\":[\"" +
                    kAudio + "\"],\"exact_tracks\":[]}",
            "a rippled stretch says so, its sync tracks after it: " + rippled);
        Check(CanonicalRoundTrip(RateStretchOperation{
                  kB, TrimEdge::Head, {20, 25}, {}, true, {kAudio}}),
              "and reads back to the same bytes");
    });

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "all slide and rate stretch tests passed\n";
    return 0;
}
