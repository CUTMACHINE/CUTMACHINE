// R3 -- docs/roadmap/REEL-2026-09.md. The rule that says which source frame
// a clip shows, frame by frame, the way ITW1 pinned the export.
//
// Portable on purpose (guard rail 1 of the lot): the monitor, the export,
// the sheets and verify_export all reach this arithmetic, and a second
// answer to the same question would diverge without any of them failing.
// Everything here runs without AppKit, Metal or FFmpeg.

#include "ClipSpeed.h"
#include "EditLog.h"
#include "Operations.h"
#include "Timeline.h"

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

std::string Join(const std::vector<int64_t>& values) {
    std::string joined;
    for (size_t index = 0; index < values.size(); ++index) {
        if (index) joined += ", ";
        joined += std::to_string(values[index]);
    }
    return joined;
}

// The source frames a clip shows, one per output frame, read through the
// rule itself.
std::vector<int64_t> Frames(const DocumentClip& clip, MediaRate sequenceRate,
                            MediaRate sourceRate, int64_t count) {
    std::vector<int64_t> frames;
    for (int64_t index = 0; index < count; ++index) {
        const RationalTime offset{index * sequenceRate.den, sequenceRate.num};
        frames.push_back(ClipSourceFrameAt(clip, offset, sourceRate));
    }
    return frames;
}

DocumentClip ShotAt(EffectParamValue speed, RationalTime sourceIn,
                    RationalTime duration) {
    DocumentClip clip;
    clip.id = "01K50000000000000000000011";
    clip.source_id = "01K50000000000000000000001";
    clip.source_in = sourceIn;
    clip.duration = duration;
    clip.timeline_in = {0, 25};
    clip.speed = speed;
    return clip;
}

// The same shot, resolved through Timeline: what the monitor and
// verify_export read. It must name the frames the rule names, or the two
// calculations have already parted.
std::vector<int64_t> TimelineFrames(const DocumentClip& clip,
                                    MediaRate sequenceRate,
                                    MediaRate sourceRate,
                                    RationalTime sourceDuration,
                                    int64_t count) {
    Document document;
    document.sequence.frame_rate = sequenceRate;
    document.sources = {
        {clip.source_id, "rush.mov", sourceRate, sourceDuration}};
    DocumentTrack track;
    track.id = "01K50000000000000000000010";
    track.kind = "video";
    track.clips = {clip};
    document.sequence.tracks = {track};
    Timeline timeline(document);
    std::vector<int64_t> frames;
    for (int64_t index = 0; index < count; ++index) {
        const RationalTime at{index * sequenceRate.den, sequenceRate.num};
        const std::optional<ResolvedFrame> resolved =
            timeline.ResolveTrack(track.id, at);
        frames.push_back(resolved ? resolved->source_frame : -1);
    }
    return frames;
}

constexpr MediaRate k25{25, 1};
constexpr MediaRate k50{50, 1};

// R3, criterion 4 -- one key of a ramp, at a sequence frame of 25 i/s.
ClipKeyframe Key(
    const std::string& id, int64_t frame, EffectParamValue speed,
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear) {
    ClipKeyframe key;
    key.id = id;
    key.time = {frame, 25};
    key.value = speed;
    key.interpolation = interpolation;
    return key;
}

// The ticket's own case: from 1 down to 1/4 over 10 frames, held at 1/4
// for 20, back up to 1 over 10, then 1.
std::vector<ClipKeyframe> TicketRamp() {
    return {Key("01K50000000000000000000R01", 0, {1, 1}),
            Key("01K50000000000000000000R02", 10, {1, 4},
                KeyframeInterpolation::Hold),
            Key("01K50000000000000000000R03", 30, {1, 4}),
            Key("01K50000000000000000000R04", 40, {1, 1})};
}

// A one-track document around `clip`, reading a 25 i/s rush of `frames`.
Document RampDocument(const DocumentClip& clip, int64_t frames) {
    Document document;
    document.sequence.id = "01K50000000000000000000R10";
    document.sequence.frame_rate = k25;
    document.sources = {{clip.source_id, "rush.mov", k25, {frames, 25}}};
    DocumentTrack track;
    track.id = "01K50000000000000000000R11";
    track.kind = "video";
    track.clips = {clip};
    document.sequence.tracks = {track};
    return document;
}

// What the monitor shows, frame by frame, over `count` timeline frames.
std::vector<int64_t> MonitorFrames(const Document& document, int64_t count) {
    Timeline timeline(document);
    std::vector<int64_t> frames;
    for (int64_t index = 0; index < count; ++index) {
        const std::optional<ResolvedFrame> resolved = timeline.ResolveTrack(
            document.sequence.tracks[0].id, RationalTime{index, 25});
        frames.push_back(resolved ? resolved->source_frame : -1);
    }
    return frames;
}

}  // namespace

int main() {
    // Criterion 1 of the ticket, on the rule itself. The 50 i/s case is the
    // one 31 shots out of 31 of the film measured used.
    Test("a 50 i/s rush at 1/2 in a 25 i/s sequence shows every frame once",
         [] {
             const DocumentClip clip = ShotAt({1, 2}, {0, 50}, {8, 25});
             const std::vector<int64_t> expected = {0, 1, 2, 3, 4, 5, 6, 7};
             const std::vector<int64_t> frames = Frames(clip, k25, k50, 8);
             Check(frames == expected, "got " + Join(frames));
             Check(TimelineFrames(clip, k25, k50, {500, 50}, 8) == expected,
                   "Timeline disagrees with the rule");
         });

    Test("the same rush at 1/1 shows one frame in two", [] {
        const DocumentClip clip = ShotAt({1, 1}, {0, 50}, {8, 25});
        const std::vector<int64_t> expected = {0, 2, 4, 6, 8, 10, 12, 14};
        const std::vector<int64_t> frames = Frames(clip, k25, k50, 8);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k50, {500, 50}, 8) == expected,
              "Timeline disagrees with the rule");
    });

    // The rush the editor left aside rather than interpolate: at 1/2 a 25
    // i/s source repeats each frame, which is a duplicate and says so.
    Test("a 25 i/s rush at 1/2 repeats each source frame, never blends", [] {
        const DocumentClip clip = ShotAt({1, 2}, {0, 25}, {8, 25});
        const std::vector<int64_t> expected = {0, 0, 1, 1, 2, 2, 3, 3};
        const std::vector<int64_t> frames = Frames(clip, k25, k25, 8);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k25, {500, 25}, 8) == expected,
              "Timeline disagrees with the rule");
    });

    Test("a 50 i/s rush at 2 shows one frame in four", [] {
        const DocumentClip clip = ShotAt({2, 1}, {0, 50}, {8, 25});
        const std::vector<int64_t> expected = {0, 4, 8, 12, 16, 20, 24, 28};
        const std::vector<int64_t> frames = Frames(clip, k25, k50, 8);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k50, {500, 50}, 8) == expected,
              "Timeline disagrees with the rule");
    });

    // 2/5 is the case where the rounding has to be spelled out: 2.5 output
    // frames per source frame, so some source frames are seen twice and
    // some three times, and the floor decides which.
    Test("a 25 i/s rush at 2/5 rounds down, twice then three times", [] {
        const DocumentClip clip = ShotAt({2, 5}, {0, 25}, {10, 25});
        const std::vector<int64_t> expected = {0, 0, 0, 1, 1, 2, 2, 2, 3, 3};
        const std::vector<int64_t> frames = Frames(clip, k25, k25, 10);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k25, {500, 25}, 10) == expected,
              "Timeline disagrees with the rule");
    });

    Test("a 50 i/s rush at 2/5 reads four source frames in five", [] {
        const DocumentClip clip = ShotAt({2, 5}, {0, 50}, {10, 25});
        const std::vector<int64_t> expected = {0, 0, 1, 2, 3, 4, 4, 5, 6, 7};
        const std::vector<int64_t> frames = Frames(clip, k25, k50, 10);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k50, {500, 50}, 10) == expected,
              "Timeline disagrees with the rule");
    });

    // A shot that does not open on frame zero: the offset is added in exact
    // time, so the whole run is the same run, shifted.
    Test("a slow motion opened mid-rush counts from the frame it opens on", [] {
        const DocumentClip clip = ShotAt({1, 2}, {60, 50}, {6, 25});
        const std::vector<int64_t> expected = {60, 61, 62, 63, 64, 65};
        const std::vector<int64_t> frames = Frames(clip, k25, k50, 6);
        Check(frames == expected, "got " + Join(frames));
        Check(TimelineFrames(clip, k25, k50, {500, 50}, 6) == expected,
              "Timeline disagrees with the rule");
    });

    Test("the source a clip consumes is its duration times its speed", [] {
        const DocumentClip half = ShotAt({1, 2}, {0, 50}, {100, 25});
        Check(ClipSourceSpan(half) == RationalTime{50, 25},
              "a 4 s shot at 1/2 reads 2 s");
        Check(ClipSourceEnd(half) == RationalTime{2, 1}, "and ends at 2 s");
        const DocumentClip twice = ShotAt({2, 1}, {0, 50}, {100, 25});
        Check(ClipSourceSpan(twice) == RationalTime{200, 25},
              "a 4 s shot at 2 reads 8 s");
        const DocumentClip plain = ShotAt({1, 1}, {0, 50}, {100, 25});
        Check(ClipSourceSpan(plain).value == 100 &&
                  ClipSourceSpan(plain).rate == 25,
              "at 1/1 the span is the duration, rate and value untouched");
    });

    // Guard rail 3: a clip nobody gave a speed keeps the exact spelling of
    // every time it carries. A reduction would change the bytes a trim
    // stores.
    Test("unit speed is the identity, spelling included", [] {
        const RationalTime time{10, 25};
        const RationalTime scaled = ScaleBySpeed(time, {1, 1});
        Check(scaled.value == 10 && scaled.rate == 25,
              "1/1 left 10/25 as itself");
        const RationalTime same = ScaleBySpeed(time, {3, 3});
        Check(same.value == 10 && same.rate == 25, "3/3 is 1/1");
        const RationalTime halved = ScaleBySpeed(time, {1, 2});
        Check(halved.value == 10 && halved.rate == 50, "1/2 of 10/25 is 10/50");
        Check(UnscaleBySpeed(halved, {1, 2}) == time,
              "and unscaling gives the timeline span back");
    });

    Test("the effective source cadence is the source times the speed", [] {
        const MediaRate half = SpeedAdjustedSourceRate(k50, {1, 2});
        Check(half.num == 25 && half.den == 1,
              "a 50 i/s rush at 1/2 reads as a 25 i/s one");
        const MediaRate fifths = SpeedAdjustedSourceRate(k25, {2, 5});
        Check(fifths.num == 10 && fifths.den == 1, "25 i/s at 2/5 is 10 i/s");
        const MediaRate drop = SpeedAdjustedSourceRate({30000, 1001}, {2, 5});
        Check(drop.num == 12000 && drop.den == 1001,
              "and a 1001-based cadence stays a 1001-based rational");
        const MediaRate untouched = SpeedAdjustedSourceRate(k50, {1, 1});
        Check(untouched.num == 50 && untouched.den == 1,
              "unit speed changes nothing");
    });

    Test("a speed is a positive fraction with bounded terms", [] {
        std::string error;
        Check(ClipSpeedIsValid({1, 2}, error), "1/2 is a speed");
        Check(ClipSpeedIsValid({1, 1}, error), "1/1 is a speed");
        Check(!ClipSpeedIsValid({-1, 2}, error) && !error.empty(),
              "a reverse is refused by name");
        // R4 -- decided on 8 October 2026: 0/1 is the freeze, a speed.
        Check(ClipSpeedIsValid({0, 1}, error), "a freeze is a speed (R4)");
        Check(!ClipSpeedIsValid({1, 0}, error), "so is a zero denominator");
        Check(!ClipSpeedIsValid({kMaxClipSpeedTerm + 1, 1}, error),
              "and a term past the bound");
    });

    // Criterion 2: the sound follows the picture by the same exact factor,
    // and each atempo link stays inside the window every FFmpeg build
    // accepts.
    Test("the atempo chain multiplies out to the speed exactly", [] {
        Check(SpeedAtempoChain({1, 1}).empty(), "unit speed stretches nothing");
        Check(SpeedAtempoChain({1, 2}) == ",atempo=1/2", "1/2 is one link");
        Check(SpeedAtempoChain({2, 5}) == ",atempo=1/2,atempo=4/5",
              "2/5 is 1/2 then 4/5, both inside [0.5, 2]; got " +
                  SpeedAtempoChain({2, 5}));
        Check(SpeedAtempoChain({2, 1}) == ",atempo=2/1", "2 is one link");
        Check(SpeedAtempoChain({4, 1}) == ",atempo=2,atempo=2/1",
              "4 is two doublings; got " + SpeedAtempoChain({4, 1}));
        Check(SpeedAtempoChain({1, 5}) == ",atempo=1/2,atempo=1/2,atempo=4/5",
              "1/5 halves twice then finishes at 4/5, and 1/4 * 4/5 is 1/5; "
              "got " +
                  SpeedAtempoChain({1, 5}));
    });

    Test("a source_in on a frame boundary is told from one inside a frame", [] {
        Check(IsOnSourceFrameBoundary({60, 50}, k50), "60/50 is frame 60");
        Check(IsOnSourceFrameBoundary({6, 5}, k50), "6/5 is frame 60");
        Check(!IsOnSourceFrameBoundary({1, 100}, k50),
              "half a frame is not a frame");
        Check(IsOnSourceFrameBoundary({0, 1}, {30000, 1001}),
              "zero is a boundary at any cadence");
        Check(!IsOnSourceFrameBoundary({1, 25}, {30000, 1001}),
              "and a 25 i/s frame is not a 23.976 i/s one");
    });

    // What a single-frame read seeks to: the first instant of the frame the
    // rule names, never a time inside it, which FFmpeg reads as the next.
    Test("a time inside a source frame seeks to that frame's start", [] {
        const MediaRate k25{25, 1};
        const RationalTime inside = SourceFrameStart({39, 50}, k25);
        Check(inside == RationalTime{19, 25} &&
                  IsOnSourceFrameBoundary(inside, k25),
              "19.5 frames is frame 19, from 19/25");
        Check(SourceFrameStart({20, 25}, k25) == RationalTime{20, 25},
              "a boundary stays where it is");
        const MediaRate ntsc{30000, 1001};
        const RationalTime film = SourceFrameStart({1, 25}, ntsc);
        Check(film == RationalTime{1001, 30000},
              "1/25 s falls in 23.976 i/s frame 1 (got " +
                  std::to_string(film.value) + "/" + std::to_string(film.rate) +
                  ")");
    });

    // The refusal guard rail 5 asks for: named, and naming the clip.
    Test("a refusal names the clip and its speed", [] {
        const DocumentClip clip = ShotAt({1, 2}, {0, 50}, {8, 25});
        const std::string refusal = ClipSpeedRefusal(clip, "tighten_pauses");
        Check(refusal.find("tighten_pauses") != std::string::npos &&
                  refusal.find(clip.id) != std::string::npos &&
                  refusal.find("1/2") != std::string::npos,
              "got " + refusal);
    });

    // R4 -- decided on 8 October 2026. A freeze is speed 0/1: the rule,
    // the monitor and verify_export name the held frame for every output
    // frame; an insert lays one, the rush's last frame included; a sound
    // and a linked clip are refused; one undo restores every byte.
    Test("a freeze holds one source frame", [] {
        const DocumentClip held = ShotAt({0, 1}, {37, 50}, {10, 25});
        Check(Join(Frames(held, k25, k50, 4)) == "37, 37, 37, 37" &&
                  Join(TimelineFrames(held, k25, k50, {100, 25}, 4)) ==
                      "37, 37, 37, 37",
              "the rule and the timeline both hold frame 37");
        Check(ClipSourceEnd(held) == held.source_in,
              "a freeze reads no span of its source");
        std::string error;
        Check(ClipSpeedIsValid({0, 1}, error) &&
                  !ClipSpeedIsValid({-1, 2}, error),
              "0/1 is a speed; a reverse is not");
    });

    Test("an insert lays a freeze of the rush's last frame", [] {
        Document document;
        document.sequence.id = "01K50000000000000000000F01";
        document.sequence.frame_rate = k25;
        document.sources = {
            {"01K50000000000000000000F02", "rush.mov", k50, {200, 50}}};
        DocumentTrack video;
        video.id = "01K50000000000000000000F03";
        video.kind = "video";
        DocumentTrack sound;
        sound.id = "01K50000000000000000000F04";
        sound.kind = "audio";
        sound.index = 1;
        document.sequence.tracks = {video, sound};
        const std::string before = document.SaveToString();
        InsertClipOperation insert;
        insert.track_id = video.id;
        insert.source_id = "01K50000000000000000000F02";
        insert.source_in = {199, 50};
        insert.duration = {50, 25};
        insert.timeline_in = {0, 25};
        insert.speed = {0, 1};
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(log.Apply(document, insert, error, message),
              "two seconds of the last frame are laid: " + message);
        const DocumentClip& laid = document.sequence.tracks[0].clips.front();
        Check(laid.speed.num == 0 && laid.speed.den == 1 &&
                  laid.duration == RationalTime{50, 25},
              "the clip holds its frame for two seconds");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(serialized.find("\"speed\":{\"num\":0,\"den\":1}") !=
                      std::string::npos &&
                  DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "the journal keeps the speed and round-trips: " + serialized);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == before,
              "one undo restores every byte");
        InsertClipOperation pastEnd = insert;
        pastEnd.source_in = {200, 50};
        Check(!log.Apply(document, pastEnd, error, message) &&
                  error == EditError::SourceOutOfBounds,
              "a frame past the rush's end is refused");
        InsertClipOperation onSound = insert;
        onSound.track_id = sound.id;
        Check(!log.Apply(document, onSound, error, message) &&
                  error == EditError::InvalidOperation,
              "a freeze on a sound track is refused");
        InsertClipOperation plain = insert;
        plain.speed = {1, 1};
        Check(SerializeOperation(plain).find("speed") == std::string::npos,
              "a plain insert keeps its old bytes");
    });

    Test("set_clip_speed refuses to freeze a linked clip", [] {
        Document document;
        document.sequence.id = "01K50000000000000000000G01";
        document.sequence.frame_rate = k25;
        document.sources = {
            {"01K50000000000000000000G02", "rush.mov", k25, {100, 25}}};
        DocumentClip picture = ShotAt({1, 1}, {10, 25}, {20, 25});
        picture.id = "01K50000000000000000000G03";
        picture.source_id = "01K50000000000000000000G02";
        picture.link_group_id = "01K50000000000000000000G04";
        DocumentTrack video;
        video.id = "01K50000000000000000000G05";
        video.kind = "video";
        video.clips = {picture};
        document.sequence.tracks = {video};
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        Check(!log.Apply(document, SetClipSpeedOperation{picture.id, {0, 1}},
                         error, message) &&
                  message.find("unlink") != std::string::npos,
              "a freeze would leave its sound nothing to hold: " + message);
        document.sequence.tracks[0].clips[0].link_group_id.clear();
        Check(log.Apply(document, SetClipSpeedOperation{picture.id, {0, 1}},
                        error, message),
              "unlinked, the clip freezes: " + message);
    });

    // R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. Worked by hand from
    // the integral: over the first ten frames P(k) = k - 3k^2/80 source
    // frames, then 6.25 + (k - 10)/4, then 11.25 + d/4 + 3d^2/80, then
    // 17.5 + (k - 40). Each output frame shows the floor; nothing else
    // repeats or skips a frame.
    Test("a ramp from 1 to 1/4 and back names the frames its integral does",
         [] {
             DocumentClip clip = ShotAt({1, 1}, {0, 25}, {60, 25});
             clip.speed_ramp = TicketRamp();
             std::vector<int64_t> expected = {
                 0,  0,  1,  2,  3,  4,  4,  5,  5,  5,  6,  6,  6,  7,
                 7,  7,  7,  8,  8,  8,  8,  9,  9,  9,  9,  10, 10, 10,
                 10, 11, 11, 11, 11, 12, 12, 13, 14, 14, 15, 16, 17};
             for (int64_t frame = 18; frame <= 36; ++frame)
                 expected.push_back(frame);
             const std::vector<int64_t> frames = Frames(clip, k25, k25, 60);
             Check(frames == expected, "got " + Join(frames));
             Check(ClipSourceFrames(clip, 0, 60, k25, k25) == expected,
                   "the export's list is the same rule");
             Check(MonitorFrames(RampDocument(clip, 100), 60) == expected,
                   "and so is the monitor");
             Check(ClipSourceSpan(clip) == RationalTime{3, 2},
                   "the clip reads 37.5 source frames, 3/2 s");
             for (size_t index = 1; index < frames.size(); ++index) {
                 const int64_t step = frames[index] - frames[index - 1];
                 Check(step == 0 || step == 1,
                       "never faster than 1, so no frame is skipped, at " +
                           std::to_string(index));
             }
             Check(ClipPeakSpeed(clip).num == 1 && ClipPeakSpeed(clip).den == 1,
                   "its fastest is 1");
         });

    Test("a ramp's integral is exact at 30000/1001 and from a 50 i/s rush", [] {
        DocumentClip clip =
            ShotAt({1, 1}, {0, 1}, RationalTime{90 * 1001, 30000});
        clip.speed_ramp = {Key("01K50000000000000000000R21", 0, {2, 1}),
                           Key("01K50000000000000000000R22", 0, {1, 1})};
        clip.speed_ramp[0].time = {0, 30000};
        clip.speed_ramp[1].time = {45 * 1001, 30000};
        const MediaRate source{60000, 1001};
        // At 2 the 60 i/s rush gives four frames per output frame; the
        // integral of 2 - k/45 over k frames is 2k - k^2/90 output frames,
        // twice that in source frames.
        bool exact = true;
        for (int64_t k = 0; k <= 45; ++k) {
            const int64_t shown =
                ClipSourceFrameAt(clip, RationalTime{k * 1001, 30000}, source);
            const int64_t numerator = 2 * (180 * k - k * k);
            if (shown != numerator / 90) exact = false;
        }
        Check(exact,
              "every frame of the deceleration is floor(2(2k - k^2/90))");
        // 67.5 output frames at frame 45, then 44 more at 1/1: 111.5, so
        // source frame 223.
        Check(ClipSourceFrameAt(clip, RationalTime{89 * 1001, 30000}, source) ==
                  223,
              "then two source frames per output frame at 1/1");
    });

    Test("a ramp is refused where it cannot be exact or cannot be heard", [] {
        std::string error;
        std::vector<ClipKeyframe> ramp = TicketRamp();
        Check(ClipSpeedRampIsValid(ramp, error), "the ticket's ramp: " + error);
        Check(ClipSpeedRampIsValid({}, error), "no ramp is valid");
        Check(!ClipSpeedRampIsValid({ramp[0]}, error) &&
                  error.find("constant") != std::string::npos,
              "one key is a constant speed: " + error);
        std::vector<ClipKeyframe> eased = ramp;
        eased[0].interpolation = KeyframeInterpolation::CubicBezier;
        Check(!ClipSpeedRampIsValid(eased, error) &&
                  error.find("cubic-bezier") != std::string::npos,
              "a cubic-bezier has no exact integral: " + error);
        std::vector<ClipKeyframe> backwards = ramp;
        backwards[1].value = {-1, 4};
        Check(!ClipSpeedRampIsValid(backwards, error),
              "a reverse is not a speed");
        std::vector<ClipKeyframe> unordered = ramp;
        unordered[2].time = {5, 25};
        Check(!ClipSpeedRampIsValid(unordered, error),
              "keys go forward in time");
        std::vector<ClipKeyframe> twins = ramp;
        twins[1].id = twins[0].id;
        Check(!ClipSpeedRampIsValid(twins, error), "each key has its own id");

        DocumentClip clip = ShotAt({1, 1}, {0, 25}, {60, 25});
        clip.speed_ramp = ramp;
        Document document = RampDocument(clip, 100);
        Check(document.Validate(error), "a ramped clip validates: " + error);
        Document both = document;
        both.sequence.tracks[0].clips[0].speed = {1, 2};
        Check(!both.Validate(error) &&
                  error.find("both a constant speed") != std::string::npos,
              "a constant speed and a ramp together: " + error);
        Document linked = document;
        linked.sequence.tracks[0].clips[0].link_group_id =
            "01K50000000000000000000R30";
        Check(!linked.Validate(error) &&
                  error.find("link group") != std::string::npos,
              "a ramp in a link group: " + error);
        Document sound = document;
        sound.sequence.tracks[0].kind = "audio";
        Check(!sound.Validate(error) &&
                  error.find("picture only") != std::string::npos,
              "a ramp on a sound track: " + error);
        // 37.5 source frames read: a 38-frame rush holds frame 37, the last
        // one the ramp shows; a 37-frame one does not.
        Check(RampDocument(clip, 38).Validate(error),
              "the rush's last frame can be reached: " + error);
        Check(!RampDocument(clip, 37).Validate(error),
              "a ramp past its rush's end is refused");
        // A ramp that ends on a stop shows the end of what it reads.
        DocumentClip stop = ShotAt({1, 1}, {0, 25}, {20, 25});
        stop.speed_ramp = {Key("01K50000000000000000000R31", 0, {1, 1}),
                           Key("01K50000000000000000000R32", 10, {0, 1})};
        // 0..10 reads 5 frames, then nothing: frames 10..19 all show 5.
        Check(Frames(stop, k25, k25, 20).back() == 5,
              "a ramp to a stop holds frame 5");
        Check(!RampDocument(stop, 5).Validate(error) &&
                  error.find("last frame") != std::string::npos,
              "and needs a rush that has it: " + error);
        Check(RampDocument(stop, 6).Validate(error), "a 6-frame rush has it");
    });

    // The undo of a cut is a join, and the join used to check that the
    // right piece opens at the left one's source_in + duration: true at 1/1
    // only, so a cut through a slow motion could not be undone.
    Test("a cut through a clip at 1/2 undoes", [] {
        DocumentClip clip = ShotAt({1, 2}, {10, 25}, {40, 25});
        Document document = RampDocument(clip, 100);
        const std::string bytes = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        SplitClipOperation split;
        split.clip_id = clip.id;
        split.timeline_position = {14, 25};
        Check(log.Apply(document, split, error, message) &&
                  document.sequence.tracks[0].clips[1].source_in ==
                      RationalTime{17, 25},
              "the right piece opens 7 source frames further: " + message);
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == bytes,
              "and the join puts the clip back, byte for byte: " + message);
    });

    Test("a ramp survives a trim and a cut on the frames it was put on", [] {
        DocumentClip clip = ShotAt({1, 1}, {0, 25}, {60, 25});
        clip.speed_ramp = TicketRamp();
        Document document = RampDocument(clip, 100);
        const std::vector<int64_t> before = MonitorFrames(document, 60);
        const std::string bytes = document.SaveToString();
        Check(bytes.find("\"version\": 18") != std::string::npos,
              "a ramp needs document version 18");
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        // Every cut, each one undone: both pieces show what the whole did.
        bool cuts = true;
        for (int64_t at = 1; at < 60; ++at) {
            SplitClipOperation split;
            split.clip_id = clip.id;
            split.timeline_position = {at, 25};
            if (!log.Apply(document, split, error, message) ||
                MonitorFrames(document, 60) != before ||
                !log.Undo(document, error, message) ||
                document.SaveToString() != bytes) {
                cuts = false;
                std::cerr << "cut at " << at << ": " << message << '\n';
                break;
            }
        }
        Check(cuts, "a cut anywhere in the ramp moves no frame");
        // A head trim of 7 frames: the frames that remain show what they
        // showed, and the keys stayed on them.
        TrimClipOperation trim;
        trim.clip_id = clip.id;
        trim.edge = TrimEdge::Head;
        trim.delta = {7, 25};
        Check(log.Apply(document, trim, error, message),
              "the head trims: " + message);
        const std::vector<int64_t> after = MonitorFrames(document, 60);
        Check(std::equal(after.begin() + 7, after.end(), before.begin() + 7) &&
                  after[6] == -1,
              "the 53 frames left show the same source frames");
        const DocumentClip& trimmed = document.sequence.tracks[0].clips[0];
        // P(7) = 7 - 3 * 49 / 80 = 413/80 source frames, 413/2000 s.
        Check(trimmed.speed_ramp[0].time == RationalTime{-7, 25} &&
                  trimmed.source_in == RationalTime{413, 2000},
              "keys moved back by 7 frames, source_in to 413/2000 s: " +
                  std::to_string(trimmed.source_in.value) + "/" +
                  std::to_string(trimmed.source_in.rate));
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == bytes,
              "one undo restores every byte");
        // A slip by +5 then -5 comes back.
        SlipEditOperation slip;
        slip.clip_ids = {clip.id};
        slip.delta = {5, 25};
        SlipEditOperation back = slip;
        back.delta = {-5, 25};
        Check(log.Apply(document, slip, error, message) &&
                  log.Apply(document, back, error, message) &&
                  document.SaveToString() == bytes,
              "a slip and its opposite cancel: " + message);
    });

    Test("set_clip_speed lays a ramp, clears it, and undoes both", [] {
        DocumentClip clip = ShotAt({1, 2}, {0, 25}, {60, 25});
        Document document = RampDocument(clip, 200);
        const std::string bytes = document.SaveToString();
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        SetClipSpeedOperation ramp{clip.id, {1, 1}, TicketRamp()};
        ramp.ramp[1].value = {2, 8};
        Check(log.Apply(document, ramp, error, message),
              "a ramp replaces the 1/2: " + message);
        const DocumentClip& ramped = document.sequence.tracks[0].clips[0];
        Check(ramped.speed.num == 1 && ramped.speed.den == 1 &&
                  ramped.speed_ramp.size() == 4 &&
                  ramped.speed_ramp[1].value.num == 1 &&
                  ramped.speed_ramp[1].value.den == 4,
              "the clip is at 1/1 with four keys, 2/8 reduced to 1/4");
        const std::string serialized =
            SerializeOperation(log.AppliedEntries().back().op);
        Operation parsed = RemoveClipOperation{};
        Check(serialized.find("\"ramp\":[") != std::string::npos &&
                  DeserializeOperation(serialized, parsed, error, message) &&
                  SerializeOperation(parsed) == serialized,
              "the journal keeps the keys and round-trips: " + serialized);
        Document reread;
        std::string loadError;
        Check(Document::LoadFromString(document.SaveToString(), reread,
                                       loadError) &&
                  reread.SaveToString() == document.SaveToString(),
              "the document reads back to the same bytes: " + loadError);
        Check(log.Apply(document, SetClipSpeedOperation{clip.id, {2, 1}}, error,
                        message) &&
                  document.sequence.tracks[0].clips[0].speed_ramp.empty(),
              "a constant speed clears the ramp: " + message);
        Check(log.Undo(document, error, message) &&
                  document.sequence.tracks[0].clips[0].speed_ramp.size() == 4,
              "undo brings the ramp back");
        Check(log.Undo(document, error, message) &&
                  document.SaveToString() == bytes,
              "and the 1/2 under it, byte for byte");
        Check(!log.Apply(document,
                         SetClipSpeedOperation{clip.id, {1, 2}, TicketRamp()},
                         error, message) &&
                  message.find("not both") != std::string::npos,
              "a speed and a ramp at once are refused: " + message);
        Check(SerializeOperation(SetClipSpeedOperation{clip.id, {1, 2}})
                      .find("ramp") == std::string::npos,
              "a constant speed keeps its old bytes");
    });

    if (failures > 0) {
        std::cerr << failures << " clip speed check(s) failed\n";
        return 1;
    }
    std::cout << "all clip speed tests passed\n";
    return 0;
}
