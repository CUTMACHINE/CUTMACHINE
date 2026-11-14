// PERF14 -- ReadAhead.h, PlaybackReads.h. What playback reads ahead of the
// picture it shows: a shot up to its last frame and no further, the budget
// of decoded pictures shared among the sources read, the bytes of the next
// seconds asked of the system in pieces of a second. Plain C++ with no
// AppKit, so this runs on a plain Linux host.
//
// PERF15 -- PlaybackReader.h. The one reader that reads, in the order they
// are shown, the samples of the pictures about to be shown: what it plans,
// the byte runs it makes of a sample table, and what it reads of a real
// file. PERF16 -- and nothing decoded behind the frame asked for in
// playback. PERF17 -- and on past a shot's out point into the next piece
// of the same rush.

#include "PlaybackReader.h"
#include "PlaybackReads.h"
#include "ReadAhead.h"

#include "Document.h"
#include "Timeline.h"

#include <unistd.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
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

using read_ahead::Advice;
using read_ahead::SourceSpan;

const Ulid kRush = "01KQ0000000000000000000001";
const Ulid kCutaway = "01KQ0000000000000000000002";
const Ulid kClip = "01KQ0000000000000000000010";
const Ulid kNextClip = "01KQ0000000000000000000011";
const Ulid kCutawayClip = "01KQ0000000000000000000012";
const Ulid kTrack = "01KQ0000000000000000000020";

RationalTime Frame(int64_t frame) { return {frame, 25}; }

DocumentClip Shot(const Ulid& id, const Ulid& source, int64_t in,
                  int64_t frames, int64_t sourceIn) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = source;
    clip.timeline_in = Frame(in);
    clip.duration = Frame(frames);
    clip.source_in = Frame(sourceIn);
    return clip;
}

// Two shots of one rush cut together, then a shot of another, in a 25 i/s
// sequence; every rush 25 i/s, a minute long.
Document Edit() {
    Document document;
    document.sequence.width = 1920;
    document.sequence.height = 1080;
    document.sequence.frame_rate = {25, 1};
    for (const Ulid& id : {kRush, kCutaway}) {
        LibraryMedia media;
        media.id = id;
        media.path = id + ".mp4";
        media.filename = media.path;
        media.rate = {25, 1};
        media.duration = {1500, 25};
        document.sources.push_back(
            {media.id, media.path, media.rate, media.duration});
        document.library.push_back(std::move(media));
    }
    DocumentTrack track;
    track.id = kTrack;
    track.kind = "video";
    track.clips = {Shot(kClip, kRush, 0, 50, 100),
                   Shot(kNextClip, kRush, 50, 75, 400),
                   Shot(kCutawayClip, kCutaway, 125, 100, 0)};
    document.sequence.tracks = {track};
    return document;
}

// A decoder asked for each frame of a shot in turn, as playback asks it,
// recording what it advises: the pieces asked of the system.
std::vector<Advice> PlayThrough(int64_t first, int64_t last,
                                const std::optional<SourceSpan>& span,
                                int64_t horizon, int64_t chunk,
                                int64_t frameCount) {
    std::vector<Advice> asked;
    Advice advised;
    for (int64_t frame = first; frame <= last; ++frame) {
        const Advice next = read_ahead::NextByteAdvice(
            frame, 1, span, advised, horizon, chunk, frameCount);
        if (next.Empty()) continue;
        asked.push_back(next);
        advised = read_ahead::Merge(advised, next);
    }
    return asked;
}

}  // namespace

int main() {
    // --- DecodedWindow: FrameCache's rule, shared by whom it says ---------
    // A 4K XAVC S-I picture, 3840x2160 in 10-bit 4:2:2, as FrameCache counts
    // it, in the 2 GB budget main.mm gives the cache.
    const size_t budget = 2000000000ULL;
    const size_t picture = 3840ULL * 2160ULL * 2ULL * 2ULL;
    const read_ahead::Window six =
        read_ahead::DecodedWindow(budget, picture, 6);
    Check(six.total == 8 && six.ahead == 6 && six.behind == 1,
          "six decoders open share the budget as before: " +
              std::to_string(six.ahead) + " ahead, " +
              std::to_string(six.behind) + " behind");
    const read_ahead::Window one =
        read_ahead::DecodedWindow(budget, picture, 1);
    Check(one.total == 43 && one.ahead == 32 && one.behind == 10,
          "one rush read alone has the whole budget: " +
              std::to_string(one.ahead) + " ahead");
    const read_ahead::Window two =
        read_ahead::DecodedWindow(budget, picture, 2);
    Check(two.ahead > six.ahead * 2 && two.ahead < one.ahead,
          "two rushes read share it in two");
    Check(read_ahead::DecodedWindow(budget, picture, 0).total ==
              read_ahead::DecodedWindow(budget, picture, 1).total,
          "nobody reading counts as one");
    const read_ahead::Window none = read_ahead::DecodedWindow(budget, 0, 1);
    Check(none.total == 1 && none.ahead == 0 && none.behind == 0,
          "an unknown picture size reads nothing ahead");
    const read_ahead::Window tiny = read_ahead::DecodedWindow(1000, picture, 1);
    Check(tiny.total == 1 && tiny.ahead == 0,
          "a budget smaller than a picture reads nothing ahead");

    // --- PERF16 BehindFill: at rest only ---------------------------------
    {
        using read_ahead::FrameRange;
        Check(read_ahead::BehindFill(100, 1, 10, false, 1500) ==
                  FrameRange{90, 99},
              "at rest, the frames before the one asked for, to step back");
        Check(read_ahead::BehindFill(100, -1, 10, false, 1500) ==
                  FrameRange{101, 110},
              "played backward, the frames after it");
        Check(read_ahead::BehindFill(3, 1, 10, false, 1500) == FrameRange{0, 2},
              "never before the rush's first frame");
        Check(read_ahead::BehindFill(1495, -1, 10, false, 1500) ==
                  FrameRange{1496, 1499},
              "nor past its last");
        Check(read_ahead::BehindFill(100, 1, 10, true, 1500).Empty() &&
                  read_ahead::BehindFill(100, -1, 10, true, 1500).Empty(),
              "in playback, nothing behind: entering a shot, the frames "
              "before it are not shown");
        Check(read_ahead::BehindFill(0, 1, 10, false, 1500).Empty() &&
                  read_ahead::BehindFill(100, 1, 0, false, 1500).Empty(),
              "nothing behind the first frame, nor with no window behind");
    }

    // --- PERF17 FollowOnFill: the window goes on into the next piece -----
    {
        using read_ahead::FrameRange;
        const SourceSpan piece{100, 149};
        Check(read_ahead::FollowOnFill(140, 32, piece, 400, 1500) ==
                  FrameRange{400, 422},
              "nine frames left of the shot, the window's other 23 from the "
              "next piece's first");
        Check(read_ahead::FollowOnFill(117, 32, piece, 400, 1500).Empty() &&
                  read_ahead::FollowOnFill(100, 32, piece, 400, 1500).Empty(),
              "nothing while the shot fills the window");
        Check(read_ahead::FollowOnFill(149, 32, piece, 1490, 1500) ==
                  FrameRange{1490, 1499},
              "never past the rush's last frame");
        Check(read_ahead::FollowOnFill(140, 32, piece, std::nullopt, 1500)
                      .Empty() &&
                  read_ahead::FollowOnFill(140, 32, std::nullopt, 400, 1500)
                      .Empty(),
              "nothing without a next piece, nor without a span");
        Check(read_ahead::FollowOnFill(160, 32, piece, 400, 1500).Empty(),
              "nothing for a frame outside its span");
    }

    // --- AheadLimit: within the shot, within the rush ---------------------
    const SourceSpan shot{100, 149};
    Check(read_ahead::AheadLimit(100, 1, 32, shot, 1500) == 132,
          "ahead of a shot's first frame, the window");
    Check(read_ahead::AheadLimit(140, 1, 33, shot, 1500) == 149,
          "near its end, the shot's last frame: nothing past the out point");
    Check(read_ahead::AheadLimit(140, 1, 33, std::nullopt, 1500) == 173,
          "without a span, the window as before");
    Check(read_ahead::AheadLimit(1490, 1, 33, std::nullopt, 1500) == 1499,
          "never past the rush's last frame");
    Check(read_ahead::AheadLimit(110, -1, 33, shot, 1500) == 100,
          "played backward, the shot's first frame");
    Check(read_ahead::AheadLimit(10, -1, 33, std::nullopt, 1500) == 0,
          "and never before the rush's first");
    Check(read_ahead::AheadLimit(160, 1, 33, shot, 1500) == 160,
          "a frame past its span reads nothing ahead");

    // --- NextByteAdvice: a shot played through ----------------------------
    // Four seconds ahead, in pieces of one, over a shot of 50 frames: the
    // whole shot is asked for on its first frame, and nothing after.
    {
        const std::vector<Advice> asked =
            PlayThrough(100, 149, shot, 100, 25, 1500);
        Check(asked.size() == 1 && asked[0] == Advice{100, 149},
              "a shot shorter than the horizon is asked for at once, to its "
              "last frame and no further");
    }
    // A shot of 300 frames: the first four seconds at once, then a second
    // at a time, each as soon as a whole second is due, up to the last
    // frame -- which is never left short.
    {
        const SourceSpan longShot{0, 299};
        const std::vector<Advice> asked =
            PlayThrough(0, 299, longShot, 100, 25, 1500);
        bool contiguous = !asked.empty() && asked.front().first == 0;
        for (size_t index = 1; index < asked.size(); ++index)
            contiguous =
                contiguous && asked[index].first == asked[index - 1].last + 1;
        Check(contiguous && asked.back().last == 299,
              "the pieces follow on, from the first frame to the last");
        Check(asked.front() == Advice{0, 99},
              "the horizon is asked for on the first frame");
        bool chunked = true;
        for (size_t index = 1; index + 1 < asked.size(); ++index)
            chunked =
                chunked && asked[index].last - asked[index].first + 1 == 25;
        Check(chunked && asked.size() == 9,
              "then a second at a time: " + std::to_string(asked.size()) +
                  " pieces");
    }
    // Unbounded (a dissolve), the horizon goes on, held within the rush.
    {
        const std::vector<Advice> asked =
            PlayThrough(1400, 1499, std::nullopt, 100, 25, 1500);
        Check(!asked.empty() && asked.back().last == 1499,
              "without a span, up to the rush's end and not past it");
    }
    // A jump (a cut to another part of the rush) asks from the new frame.
    {
        const Advice advised{100, 199};
        const Advice next = read_ahead::NextByteAdvice(
            400, 1, SourceSpan{400, 474}, advised, 100, 25, 1500);
        Check(next == Advice{400, 474},
              "a jump asks for the new shot from its frame: " +
                  std::to_string(next.first) + "-" + std::to_string(next.last));
        Check(read_ahead::Merge(advised, next) == next,
              "and what was asked before is not counted as following it");
    }
    // A shot not on screen yet asks for its first second only.
    {
        const Advice next = read_ahead::NextByteAdvice(
            400, 1, SourceSpan{400, 474}, {}, 25, 25, 1500);
        Check(next == Advice{400, 424},
              "a second of the next shot, ahead of its cut");
    }
    // Played backward, the mirror.
    {
        const Advice first =
            read_ahead::NextByteAdvice(149, -1, shot, {}, 100, 25, 1500);
        Check(first == Advice{100, 149},
              "backward, down to the shot's first frame");
        const Advice none =
            read_ahead::NextByteAdvice(140, -1, shot, first, 100, 25, 1500);
        Check(none.Empty(), "and nothing more once it is asked for");
    }
    Check(read_ahead::NextByteAdvice(200, 1, shot, {}, 100, 25, 1500).Empty(),
          "a frame outside its shot asks nothing");
    Check(read_ahead::NextByteAdvice(10, 1, std::nullopt, {}, 0, 25, 1500)
              .Empty(),
          "no horizon asks nothing");
    Check(read_ahead::Merge({10, 20}, {21, 30}) == Advice{10, 30} &&
              read_ahead::Merge({10, 20}, {}) == Advice{10, 20} &&
              read_ahead::Merge({}, {5, 9}) == Advice{5, 9},
          "advices that touch become one");

    // --- PlaybackReads: the span of a clip, from the document -------------
    Document edit = Edit();
    const DocumentClip* clip = edit.FindClip(kClip);
    const DocumentClip* next = edit.FindClip(kNextClip);
    Check(clip != nullptr && next != nullptr, "the edit holds its shots");
    if (clip != nullptr && next != nullptr) {
        const auto span = playback_reads::ClipReadSpan(edit, *clip);
        Check(span && *span == SourceSpan{100, 149},
              "a shot of 50 frames from source frame 100 reads 100 to 149");
        const auto nextSpan = playback_reads::ClipReadSpan(edit, *next);
        Check(nextSpan && *nextSpan == SourceSpan{400, 474},
              "the next shot of the same rush reads its own range");
        Check(playback_reads::SourceFramesIn(edit, *clip, 4) == 100,
              "four seconds of a 25 i/s rush are 100 frames");
    }
    {
        Document fast = edit;
        DocumentClip& shotAtTwice = fast.sequence.tracks[0].clips[0];
        shotAtTwice.speed = {2, 1};
        const auto span = playback_reads::ClipReadSpan(fast, shotAtTwice);
        Check(span && *span == SourceSpan{100, 198},
              "at twice the speed, 50 frames of timeline read 99 of source");
        Check(playback_reads::SourceFramesIn(fast, shotAtTwice, 4) == 200,
              "and four seconds of it are 200 source frames");
        DocumentClip& half = fast.sequence.tracks[0].clips[1];
        half.speed = {1, 2};
        Check(playback_reads::SourceFramesIn(fast, half, 1) == 13,
              "at half speed, a second is 12.5 frames, rounded up");
    }
    {
        Document dissolved = edit;
        DocumentTransition transition;
        transition.track_id = kTrack;
        transition.left_clip_id = kClip;
        transition.right_clip_id = kNextClip;
        transition.duration = Frame(10);
        dissolved.sequence.transitions.push_back(transition);
        Check(!playback_reads::ClipReadSpan(
                  dissolved, dissolved.sequence.tracks[0].clips[0]) &&
                  !playback_reads::ClipReadSpan(
                      dissolved, dissolved.sequence.tracks[0].clips[1]) &&
                  playback_reads::ClipReadSpan(
                      dissolved, dissolved.sequence.tracks[0].clips[2]),
              "both shots of a dissolve read past their range: unbounded");
    }
    {
        Document orphan = edit;
        DocumentClip lost = orphan.sequence.tracks[0].clips[0];
        lost.source_id = "01KQ0000000000000000000099";
        Check(!playback_reads::ClipReadSpan(orphan, lost) &&
                  playback_reads::SourceFramesIn(orphan, lost, 4) == 0,
              "a clip of a source the document does not hold has no span");
    }

    // --- VisibleReadSpan: no further than the picture is seen -------------
    // An interview of ten seconds on V1 from source frame 1000, a cutaway
    // from 4 s to 8 s on V2, opaque and full frame: from the start, V1 is
    // read up to the frame before the cutaway, not to its out point.
    {
        Document cut;
        cut.sequence.width = 1920;
        cut.sequence.height = 1080;
        cut.sequence.frame_rate = {25, 1};
        for (const Ulid& id : {kRush, kCutaway}) {
            LibraryMedia media;
            media.id = id;
            media.path = id + ".mp4";
            media.filename = media.path;
            media.width = 1920;
            media.height = 1080;
            media.pixel_format = "yuv422p10le";
            media.rate = {25, 1};
            media.duration = {1500, 25};
            cut.sources.push_back(
                {media.id, media.path, media.rate, media.duration});
            cut.library.push_back(std::move(media));
        }
        DocumentTrack v1;
        v1.id = kTrack;
        v1.kind = "video";
        v1.index = 0;
        v1.clips = {Shot(kClip, kRush, 0, 250, 1000)};
        DocumentTrack v2;
        v2.id = "01KQ0000000000000000000021";
        v2.kind = "video";
        v2.index = 1;
        v2.clips = {Shot(kCutawayClip, kCutaway, 100, 100, 0)};
        cut.sequence.tracks = {v1, v2};
        const auto spanAt = [&](const Document& document, RationalTime at,
                                int direction, int64_t reach) {
            const Timeline timeline(document);
            return playback_reads::VisibleReadSpan(
                document, timeline, kTrack,
                document.sequence.tracks[0].clips[0], at, direction,
                RationalTime{reach, 1},
                playback_reads::PicturesFromLibrary(document), false);
        };
        const auto ahead = spanAt(cut, Frame(0), 1, 10);
        Check(ahead && *ahead == SourceSpan{1000, 1099},
              "the interview is read up to the frame before the cutaway");
        const auto far = spanAt(cut, Frame(0), 1, 2);
        Check(far && *far == SourceSpan{1000, 1249},
              "a cutaway out of reach does not bound it yet");
        const auto after = spanAt(cut, Frame(200), 1, 10);
        Check(after && *after == SourceSpan{1000, 1249},
              "after the cutaway, up to its out point");
        const auto back = spanAt(cut, Frame(249), -1, 10);
        Check(back && *back == SourceSpan{1200, 1249},
              "played backward, down to the frame after the cutaway");
        Document translucent = cut;
        translucent.sequence.tracks[1].clips[0].opacity = {1, 2};
        const auto seen = spanAt(translucent, Frame(0), 1, 10);
        Check(seen && *seen == SourceSpan{1000, 1249},
              "under a translucent cutaway the interview is still seen");
        Document keyed = cut;
        keyed.library[1].pixel_format = "yuva444p10le";
        const auto under = spanAt(keyed, Frame(0), 1, 10);
        Check(under && *under == SourceSpan{1000, 1249},
              "nor under a cutaway with an alpha channel");
        Document narrow = cut;
        narrow.library[1].width = 1440;
        const auto pillar = spanAt(narrow, Frame(0), 1, 10);
        Check(pillar && *pillar == SourceSpan{1000, 1249},
              "nor under a cutaway that leaves bars");
    }

    // --- PERF17 NextPieceFrame: a cut within one rush --------------------
    {
        // Two pieces of the rush cut together on V1 (frames 0-99 from 1000,
        // then 100-249 from 1200), a cutaway on V2 from 100 to 199.
        Document cut;
        cut.sequence.width = 1920;
        cut.sequence.height = 1080;
        cut.sequence.frame_rate = {25, 1};
        for (const Ulid& id : {kRush, kCutaway}) {
            LibraryMedia media;
            media.id = id;
            media.path = id + ".mp4";
            media.filename = media.path;
            media.width = 1920;
            media.height = 1080;
            media.pixel_format = "yuv422p10le";
            media.rate = {25, 1};
            media.duration = {1500, 25};
            cut.sources.push_back(
                {media.id, media.path, media.rate, media.duration});
            cut.library.push_back(std::move(media));
        }
        DocumentTrack v1;
        v1.id = kTrack;
        v1.kind = "video";
        v1.index = 0;
        v1.clips = {Shot(kClip, kRush, 0, 100, 1000),
                    Shot(kNextClip, kRush, 100, 150, 1200)};
        DocumentTrack v2;
        v2.id = "01KQ0000000000000000000021";
        v2.kind = "video";
        v2.index = 1;
        v2.clips = {Shot(kCutawayClip, kCutaway, 100, 100, 0)};
        Document plain = cut;
        plain.sequence.tracks = {v1};
        cut.sequence.tracks = {v1, v2};
        const auto nextOf = [&](const Document& document, int64_t at,
                                int direction, int64_t reach,
                                const Ulid& clipId) {
            const Timeline timeline(document);
            return playback_reads::NextPieceFrame(
                document, timeline, kTrack, *document.FindClip(clipId),
                Frame(at), direction, RationalTime{reach, 1},
                playback_reads::PicturesFromLibrary(document), false);
        };
        Check(nextOf(plain, 80, 1, 2, kClip) == std::optional<int64_t>{1200},
              "the next piece of the same rush, from its first frame");
        Check(!nextOf(plain, 20, 1, 2, kClip),
              "not while the cut is out of reach");
        Check(!nextOf(plain, 80, -1, 2, kClip), "nor played backward");
        Check(!nextOf(plain, 120, 1, 10, kNextClip),
              "nor at the end of the timeline");
        Check(!nextOf(cut, 80, 1, 2, kClip),
              "nor when an opaque shot above hides its start");
        Document translucent = cut;
        translucent.sequence.tracks[1].clips[0].opacity = {1, 2};
        Check(nextOf(translucent, 80, 1, 2, kClip) ==
                  std::optional<int64_t>{1200},
              "under a translucent one it is seen, and decoded on into");
        Document dissolved = plain;
        DocumentTransition dissolve;
        dissolve.track_id = kTrack;
        dissolve.left_clip_id = kClip;
        dissolve.right_clip_id = kNextClip;
        dissolve.duration = Frame(10);
        dissolved.sequence.transitions = {dissolve};
        Check(!nextOf(dissolved, 80, 1, 2, kClip),
              "nor through a dissolve, which reads both pieces at once");
        Document other = plain;
        other.sequence.tracks[0].clips[1].source_id = kCutaway;
        Check(!nextOf(other, 80, 1, 2, kClip),
              "nor when the next piece is of another rush");
    }

    // --- PERF15 PlanReads: the frames shown next, in the order shown ------
    {
        using playback_reader::PlannedRead;
        const Document document = Edit();
        const Timeline timeline(document);
        const auto none = [](const Ulid&, int64_t) {
            return std::optional<monitor_occlusion::Picture>{};
        };
        // Frames 40 to 69: the end of the first shot, then the next shot of
        // the same rush, its run cut after twelve frames.
        const auto plan =
            playback_reader::PlanReads(document, timeline, 40, 30, none, false);
        Check(plan == std::vector<PlannedRead>{{kRush, 140, 149, 40},
                                               {kRush, 400, 411, 50},
                                               {kRush, 412, 419, 62}},
              "a cut of one rush is two runs, in the order shown");
        const auto end = playback_reader::PlanReads(document, timeline, 220, 30,
                                                    none, false);
        Check(end == std::vector<PlannedRead>{{kCutaway, 95, 99, 220}},
              "the plan stops at the end of the timeline");
        Check(playback_reader::PlanReads(document, timeline, 40, 0, none, false)
                  .empty(),
              "no frames, no plan");
    }
    {
        using playback_reader::PlannedRead;
        Document cut;
        cut.sequence.width = 1920;
        cut.sequence.height = 1080;
        cut.sequence.frame_rate = {25, 1};
        for (const Ulid& id : {kRush, kCutaway}) {
            LibraryMedia media;
            media.id = id;
            media.path = id + ".mp4";
            media.filename = media.path;
            media.width = 1920;
            media.height = 1080;
            media.pixel_format = "yuv422p10le";
            media.rate = {25, 1};
            media.duration = {1500, 25};
            cut.sources.push_back(
                {media.id, media.path, media.rate, media.duration});
            cut.library.push_back(std::move(media));
        }
        DocumentTrack v1;
        v1.id = kTrack;
        v1.kind = "video";
        v1.index = 0;
        v1.clips = {Shot(kClip, kRush, 0, 250, 1000)};
        DocumentTrack v2;
        v2.id = "01KQ0000000000000000000021";
        v2.kind = "video";
        v2.index = 1;
        v2.clips = {Shot(kCutawayClip, kCutaway, 100, 100, 0)};
        cut.sequence.tracks = {v1, v2};
        const auto planOf = [](const Document& document) {
            const Timeline timeline(document);
            return playback_reader::PlanReads(
                document, timeline, 90, 30,
                playback_reads::PicturesFromLibrary(document), false);
        };
        Check(planOf(cut) == std::vector<PlannedRead>{{kRush, 1090, 1099, 90},
                                                      {kCutaway, 0, 11, 100},
                                                      {kCutaway, 12, 19, 112}},
              "under an opaque cutaway the interview is not read");
        Document translucent = cut;
        translucent.sequence.tracks[1].clips[0].opacity = {1, 2};
        Check(planOf(translucent) ==
                  std::vector<PlannedRead>{{kRush, 1090, 1101, 90},
                                           {kCutaway, 0, 11, 100},
                                           {kRush, 1102, 1113, 102},
                                           {kCutaway, 12, 19, 112},
                                           {kRush, 1114, 1119, 114}},
              "layers shown together are read in turn, as they are shown");
        Document hidden = cut;
        hidden.sequence.tracks[1].visible = false;
        Check(planOf(hidden) ==
                  std::vector<PlannedRead>{{kRush, 1090, 1101, 90},
                                           {kRush, 1102, 1113, 102},
                                           {kRush, 1114, 1119, 114}},
              "a hidden track is not read and hides nothing");
    }

    // --- PERF15 RunsFor: the picture's samples, the sound skipped ---------
    {
        using playback_reader::ByteRun;
        using read_ahead::SampleRange;
        // Two frames, sound, two frames.
        const std::vector<SampleRange> samples = {
            {0, 100}, {100, 100}, {300, 100}, {400, 100}};
        Check(playback_reader::RunsFor(samples, 0, 3, 250) ==
                  std::vector<ByteRun>{{0, 200}, {300, 200}},
              "touching samples are one read, the sound between skipped");
        Check(playback_reader::RunsFor(samples, 0, 3, 150) ==
                  std::vector<ByteRun>{
                      {0, 100}, {100, 100}, {300, 100}, {400, 100}},
              "no read longer than the bound");
        Check(playback_reader::RunsFor(samples, 1, 2, 1000) ==
                  std::vector<ByteRun>{{100, 100}, {300, 100}},
              "only the frames asked for");
        Check(playback_reader::RunsFor(samples, -5, 99, 1000) ==
                  std::vector<ByteRun>{{0, 200}, {300, 200}},
              "frames outside the table are left out");
        Check(playback_reader::RunsFor({{0, 500}, {500, 10}}, 0, 1, 100) ==
                  std::vector<ByteRun>{{0, 500}, {500, 10}},
              "a sample longer than the bound is a read of its own");
        Check(playback_reader::RunsFor({}, 0, 10, 100).empty(),
              "no table, nothing to read");
    }

    // --- PERF15 Reader: what it reads of a real file -----------------------
    {
        // The rush as a file of 1500 samples of 16 bytes, one after another.
        constexpr int64_t kSample = 16;
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() /
            ("cutmachine_reader_" + std::to_string(getpid()) + ".bin");
        {
            std::ofstream out(path, std::ios::binary);
            const std::string sample(static_cast<size_t>(kSample), 'x');
            for (int frame = 0; frame < 1500; ++frame) out << sample;
        }
        auto samples = std::make_shared<std::vector<read_ahead::SampleRange>>();
        for (int64_t frame = 0; frame < 1500; ++frame)
            samples->push_back({frame * kSample, kSample});
        playback_reader::Reader reader;
        reader.SetSource(kRush, path.string(), samples);
        // One second ahead of frame 40: frames 140-149, 400-411, 412-414 of
        // the rush, three reads.
        reader.Start(std::make_unique<Document>(Edit()), false, Frame(40), 1,
                     false);
        int64_t bytes = 0;
        while (const int64_t read = reader.Step()) bytes += read;
        auto stats = reader.GetStats();
        Check(bytes == 25 * kSample && stats.bytes == 25 * kSample &&
                  stats.reads == 3,
              "the next second is read once, a run at a time");
        Check(reader.Step() == 0, "and not again");
        // Ten frames on: what was read is not read again, only what the
        // horizon now reaches (415-424).
        reader.SetPosition(Frame(50));
        bytes = 0;
        while (const int64_t read = reader.Step()) bytes += read;
        stats = reader.GetStats();
        Check(bytes == 10 * kSample && stats.plans == 2,
              "moving on reads only what was not read");
        // The cutaway has no table: its frames are left to its decoder.
        reader.SetPosition(Frame(120));
        while (reader.Step() != 0) {
        }
        stats = reader.GetStats();
        Check(stats.unplaced_sources == 1,
              "a source without a table is counted, not read");
        reader.Stop();
        Check(!reader.Running() && reader.Step() == 0,
              "stopped, the reader reads nothing");
        // Threaded, it reads the same on its own.
        playback_reader::Reader threaded;
        threaded.SetSource(kRush, path.string(), samples);
        threaded.Start(std::make_unique<Document>(Edit()), false, Frame(40), 1,
                       true);
        for (int wait = 0;
             wait < 200 && threaded.GetStats().bytes < 25 * kSample; ++wait)
            usleep(5000);
        threaded.Stop();
        Check(threaded.GetStats().bytes == 25 * kSample,
              "its thread reads the next second on its own");
        std::filesystem::remove(path);
    }

    if (failures) {
        std::cerr << failures << " read-ahead assertion(s) failed\n";
        return 1;
    }
    std::cout << "PASS: read ahead\n";
    return 0;
}
