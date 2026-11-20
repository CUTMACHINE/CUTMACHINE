#pragma once

// PERF14 -- docs/roadmap/PERF-2026-09.md. What playback reads ahead of the
// picture it shows, and how far.
//
// Measured on 4 October 2026 on PRJA (71 Sony XAVC S-I rushes, H.264
// 4:2:2 Intra 10 bits in 4K, 31 MB/s each, on a 5 TB USB disk in exFAT read
// through FSKit): the same 73-second interview played with 3 late ticks in
// 2,401 from the internal SSD, and 961 from that disk. The decoder is not
// the limit; the disk is, and the way it is read. One reader gets 41.6 MB/s
// out of it, two at once 24 MB/s between them. The decoders read it one
// packet at a time, in the thread that decodes, a few pictures ahead: six
// (240 ms), because FrameCache divided its budget among every decoder open,
// playing or not -- and six more past the end of each shot, that nobody
// would see, read at the very moment the next shot needed the disk.
//
// Three rules, in plain C++ with no AppKit or FFmpeg dependency:
// - a shot is read up to its last frame and no further (SourceSpan);
// - the budget of decoded pictures is shared among the sources playback
//   reads, not among the decoders open (DecodedWindow);
// - the bytes of the next seconds are asked of the system ahead of the
//   decoder, in pieces of a second (NextByteAdvice), so that the disk
//   streams while the decoder decodes. An encoded 4K XAVC S-I frame is
//   1.2 MB where a decoded one is 33: seconds of encoded read-ahead cost the
//   memory of a few decoded pictures.
//
// See tests/read_ahead_tests.cc.

#include <cstddef>
#include <cstdint>
#include <optional>

namespace read_ahead {

// The source frames a shot shows, first and last included, in its source's
// own numbering (PlaybackReads.h works them out from a clip).
struct SourceSpan {
    int64_t first = 0;
    int64_t last = 0;

    bool operator==(const SourceSpan& other) const {
        return first == other.first && last == other.last;
    }
};

// PERF15 -- where one picture lies in its file: the bytes of its sample,
// by the container's index. A source's table holds one per frame, in frame
// order; empty when the index does not place them.
struct SampleRange {
    int64_t offset = 0;
    int64_t size = 0;
};

// Decoded pictures a decoder keeps around the one asked for: `ahead` in the
// direction of play, `behind` the other way, `total` counting the one asked.
struct Window {
    size_t ahead = 0;
    size_t behind = 0;
    size_t total = 1;
};

// The window of one of `sources` sources sharing `budgetBytes` of decoded
// pictures of `frameBytes` each: 70 % of the budget goes to read-ahead, the
// rest keeps what was just shown. Three quarters of a window lie ahead. The
// rule FrameCache applied since PERF10; only who shares it changes.
Window DecodedWindow(size_t budgetBytes, size_t frameBytes, size_t sources);

// The last frame a decoder asked for `frame` decodes ahead, moving in
// `direction` (positive forward): `ahead` frames on, held within `span` when
// there is one, and within the source's `frameCount` frames. Played
// backward, the lowest frame. Never past `frame` itself in the other
// direction: a frame outside its span reads nothing ahead.
int64_t AheadLimit(int64_t frame, int direction, size_t ahead,
                   const std::optional<SourceSpan>& span, int64_t frameCount);

// Source frames `first` to `last`; empty when last < first.
struct FrameRange {
    int64_t first = 0;
    int64_t last = -1;

    bool Empty() const { return last < first; }
    bool operator==(const FrameRange& other) const {
        return (Empty() && other.Empty()) ||
               (first == other.first && last == other.last);
    }
};

// PERF16 -- the frames a decoder asked for `frame`, moving in `direction`,
// decodes behind it -- the other way -- before what lies ahead: `behind`
// of them at rest, within the source's `frameCount` frames, to step back
// frame by frame; none in playback. What playback has shown is in the cache
// already, and what it has not it will not show: entering a shot, the
// frames before its first one. Decoded first, those made the shot's own
// frames wait. At a cut between two pieces of one rush, whose decoder is
// asked for the next piece only once it is due, that was ten frames of the
// gap between them, read from the disk outside the playback reader
// (PERF15), before the frame due. Measured on 4 October 2026, cold, 40 s on
// the external disk, one binary: 20 and 60 display ticks late on the two
// probed timelines, 14 to 28 at each such cut, against 2 to 6 and 11 to 12
// without; 1,120 frames decoded for 1,000 shown, against 1,030.
FrameRange BehindFill(int64_t frame, int direction, size_t behind, bool playing,
                      int64_t frameCount);

// PERF17 -- the frames a decoder asked for `frame` decodes past the out
// point of its shot, `span`, when the next piece shown on that track is of
// the same rush, from its first frame `next`: what is left of the `ahead`
// frames of its window once the shot's own are counted, as if the shot went
// on. Empty when the shot's end is out of the window's reach, when nothing
// of the same rush follows, and for a frame outside its span. Forward play
// only. The decoder of a rush on screen is not asked for the next piece's
// first frame until it is due (main.mm, -warmUpcomingDecoders: leaves alone
// a source being shown, so as not to take its frame away); at the cut it
// then seeked, and its frame threads started again, two to six display
// ticks late at every such cut (PERF16). With these frames, it seeks while
// it has nothing left to decode, and reads on from them at the cut.
FrameRange FollowOnFill(int64_t frame, size_t ahead,
                        const std::optional<SourceSpan>& span,
                        std::optional<int64_t> next, int64_t frameCount);

// Frames whose bytes are asked of the system, first and last included.
// Empty when last < first.
struct Advice {
    int64_t first = 0;
    int64_t last = -1;

    bool Empty() const { return last < first; }
    bool operator==(const Advice& other) const {
        return first == other.first && last == other.last;
    }
};

// How far ahead of the frame on screen the bytes of the shot playing are
// asked for: two seconds, asked a second at a time, so that each second is
// asked about a second before the decoder reads it. Measured on the disk
// above (cold cache, 2,400 display ticks, ticks with the top layer late,
// PRJA_Camille / SERIE_Camille): 999 / 1,058 without; 309 and 351 / 441
// and 470 at two seconds; 1,296 / 1,021 at one, asked too late to lead the
// decoder; 892 / 977 at three and 1,622 / 1,611 at four (that one before
// reads stopped at a cutaway), where what is asked early queues in front of
// what the picture needs now. The disk has about a quarter more than one
// stream to give: what is read ahead is taken from what is read now.
inline constexpr int64_t kByteHorizonSeconds = 2;

// The smallest piece asked for at once, and all that is asked for a shot
// not on screen yet (a cut or a cutaway's end within the next second): a
// second of it, so that it starts from memory without taking the disk from
// the shot that plays.
inline constexpr int64_t kByteChunkSeconds = 1;

// How far ahead of its cut the next shot is asked for: its decoder seeks
// and decodes its first frames, and its first second is asked of the
// system (main.mm, -warmUpcomingDecoders:). One second since PERF3.
inline constexpr int64_t kCutLookaheadSeconds = 1;

// How far ahead of its return a track hidden under a cutaway is asked for
// again (MonitorOcclusion.h, NextShown): two seconds, where a cut is asked
// for one. Its decoder then reads the first frames back while the cutaway's
// own read-ahead, two seconds long, has usually reached its last frame.
inline constexpr int64_t kReappearanceLookaheadSeconds = 2;

// The frames to ask the system for now, for a decoder asked for `frame`,
// moving in `direction`, with `horizon` frames of bytes wanted ahead (the
// frame itself counted) and `advised` already asked for. Within `span` when
// there is one, and within the source's `frameCount` frames.
//
// Following on from `advised` (the frame within it, or just past it), only
// what lies beyond is asked, and only once a whole `chunk` of it is due --
// or the end of the shot, which is never left short. Anywhere else (a jump,
// a cut to another part of the rush) everything from `frame` to the horizon
// is asked at once. Empty when there is nothing new to ask. The caller keeps
// the union with `advised` when they touch, the new advice otherwise.
Advice NextByteAdvice(int64_t frame, int direction,
                      const std::optional<SourceSpan>& span,
                      const Advice& advised, int64_t horizon, int64_t chunk,
                      int64_t frameCount);

// `advised` and `next` as one advice when they touch or overlap, else
// `next` alone: what a decoder records after asking for `next`.
Advice Merge(const Advice& advised, const Advice& next);

}  // namespace read_ahead
