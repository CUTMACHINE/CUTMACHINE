#pragma once

// PERF10 -- docs/roadmap/PERF-2026-09.md. Which media decoders the editor
// keeps open.
//
// Opening a project used to open a decoder for every video source of the
// library, in parallel, before anything was asked of them: on a 169-rush 4K
// shoot that was about 1,030 threads and 2 GB before the first frame. And
// each decoder that had served a frame kept its buffers for good -- a 4K
// XAVC-I rush held 270 MB with frame threading, 350 MB more once the worker
// had read ahead -- so playing a 64-clip reel through grew the app past the
// Mac's 16 GB and froze the whole machine (24 September 2026).
//
// A decoder is now opened when the monitor needs its source, and at most
// `capacity` stay open: past it, the least recently used one that nothing on
// screen or about to be on screen needs is closed. Closing is cheap to undo
// -- a reopen is one avformat_open_input, tens of milliseconds, and the
// upcoming-cut warm-up asks for it a second before the cut -- and a closed
// decoder gives its memory back to the allocator, which the next open reuses
// (measured: twelve open/decode/close rounds stay within 13 MB).
//
// PERF12 -- "tens of milliseconds" holds for an idle internal drive, not for
// the external one projects are cut from. On a 5 TB USB disk in exFAT (4
// October 2026, 71 XAVC S-I rushes, 41 MB/s for one reader, 27 MB/s shared
// by two), an open takes 0.2 s while nothing else reads the disk, up to
// 7.5 s while playback streams another rush from it, and 20 s when three
// overlapped, as the one-second warm-up made them: every cut to a rush not
// yet open stopped the picture for that long. So the decoders playback will
// need are opened ahead (PlanOpens below): at rest, the next ones forward of
// the playhead; in playback, those of the next kLookaheadSeconds, soonest
// first, one at a time, within the same capacity, so memory and threads stay
// bounded as PERF10 left them. Nothing here opens a rush playback does not
// reach.
//
// Plain C++ with no AppKit or FFmpeg dependency, like PlaybackPresentation.h;
// main.mm only wires it. See tests/decoder_pool_tests.cc.

#include "RationalTime.h"
#include "Ulid.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

class Document;

namespace decoder_pool {

// Enough for three stacked video tracks, the sources the next second of
// playback crosses into, and the Source monitor. What is needed right now is
// never closed, so a timeline stacking more than this still decodes -- the
// capacity bounds what is kept around, not what is shown.
//
// PERF12 -- and just enough for an interview cut on two tracks: on the
// measured 73-second timeline (ten rushes, 22 shots), what is on screen plus
// every rush entered within kLookaheadSeconds is six at most (at 51.7 s), and
// played through, each rush opens once (tests/decoder_pool_tests.cc). A
// strict rotation of ten takes in three-second shots would need ten. A 4K
// XAVC-I decoder costs about 88 MB and one thread per core plus one
// (PERF10). PERF14 -- FrameCache used to divide its read-ahead budget by
// the decoders open, playing or not: more capacity meant a shorter
// read-ahead per rush. It now divides it among the sources playback reads
// (FrameCache::SetReadSources), so the capacity costs memory and threads
// only.
inline constexpr size_t kDefaultCapacity = 6;

// PERF12 -- how far ahead of the playhead decoders are opened, in seconds of
// playback at the current speed: twice what an open takes while another
// rush streams from the same external disk (up to 7.5 s measured, one open
// at a time), so the decoder is there before its cut.
inline constexpr int64_t kLookaheadSeconds = 15;

// PERF12 -- opens started ahead of need at the same time. One: on a spinning
// disk, a second reader in parallel slows the first and the stream playing.
inline constexpr size_t kMaxOpeningAhead = 1;

// PERF12 -- an open still unanswered after this long no longer holds the
// slot above, so one rush that hangs (a failing sector, a share that does
// not answer) does not stop the others from being opened ahead; and at most
// kMaxOpeningInFlight opens are ever outstanding, stuck ones included.
inline constexpr std::chrono::seconds kStaleOpen{15};
inline constexpr size_t kMaxOpeningInFlight = 3;

class Usage {
public:
    // Records that `source` was asked for a frame now.
    void Touch(const std::string& source) { lastUse_[source] = ++clock_; }

    // A closed decoder's history goes with it: reopened later, it starts as
    // the most recently used.
    void Forget(const std::string& source) { lastUse_.erase(source); }

    // Zero for a source never touched.
    uint64_t LastUse(const std::string& source) const {
        const auto found = lastUse_.find(source);
        return found == lastUse_.end() ? 0 : found->second;
    }

    // The open decoders to close so that at most `capacity` remain, least
    // recently used first. Never one in `needed`; when `needed` alone
    // exceeds the capacity, fewer are returned than the excess.
    std::vector<std::string> ToClose(const std::vector<std::string>& open,
                                     const std::set<std::string>& needed,
                                     size_t capacity) const {
        if (open.size() <= capacity) return {};
        std::vector<std::string> candidates;
        for (const std::string& source : open)
            if (needed.count(source) == 0) candidates.push_back(source);
        // Ties (never touched) close in the order given, which the caller
        // keeps deterministic.
        std::stable_sort(
            candidates.begin(), candidates.end(),
            [&](const std::string& left, const std::string& right) {
                return LastUse(left) < LastUse(right);
            });
        const size_t excess = open.size() - capacity;
        if (candidates.size() > excess) candidates.resize(excess);
        return candidates;
    }

private:
    std::map<std::string, uint64_t> lastUse_;
    uint64_t clock_ = 0;
};

// PERF12. A media source the monitor will show, and how soon: the timeline
// time from the playhead to the first of its frames playback reaches. Zero
// for one on screen now.
struct Need {
    Ulid source;
    RationalTime distance;
};

// The media sources playback reaches within `lookahead` seconds from
// `position`, each once, soonest first; those on screen come first, at
// distance zero. `direction` is the playback speed and sense (positive
// forward, negative backward, as main.mm keeps it); at rest (zero) it is
// forward at normal speed, the likeliest next move. At speed n the window
// spans n times `lookahead` of timeline, the same wall-clock warning. Only
// what the Record monitor composites counts: clips on visible video tracks,
// minus the angles a multicam group takes off screen; a motion clip has no
// decoder of its own. Equal distances keep track order, lowest index first.
std::vector<Need> UpcomingNeeds(const Document& document, RationalTime position,
                                int direction, RationalTime lookahead);

// PERF12. Where the decoders stand when a plan is made.
struct PoolState {
    std::set<Ulid> open;
    // The opens in flight, and how long each has been running.
    std::map<Ulid, std::chrono::steady_clock::duration> opening;
    // Being torn down: reopened once gone, not before (main.mm).
    std::set<Ulid> closing;
    // Media that is not there or failed to open: never planned, so it takes
    // neither a place in the pool nor the opening slot.
    std::set<Ulid> offline;
};

struct Plan {
    // The needs the pool holds on to, soonest first: everything on screen,
    // then what comes next, up to the capacity. Never closed (Usage).
    std::vector<Ulid> keep;
    // The kept sources that have no decoder and none on its way, soonest
    // first. The caller opens them in this order until `budget` have
    // actually started, skipping one it cannot open (no video to decode).
    std::vector<Ulid> open;
    size_t budget = 0;
};

// Which decoders to hold and which to open now. `needs` is UpcomingNeeds'
// answer, with the Source monitor's source added at distance zero when it
// shows one. On-screen needs are all kept even past the capacity, as Usage
// never closes them; the rest fill what the capacity leaves. At most
// `maxOpening` opens run at once, those older than kStaleOpen not counted,
// and never more than kMaxOpeningInFlight in all.
Plan PlanOpens(const std::vector<Need>& needs, const PoolState& state,
               size_t capacity, size_t maxOpening = kMaxOpeningAhead);

}  // namespace decoder_pool
