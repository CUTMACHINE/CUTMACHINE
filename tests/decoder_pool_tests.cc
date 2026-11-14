// Pure-C++ test for which decoders the editor keeps open (PERF10) and which
// it opens ahead of playback (PERF12). DecoderPool.h has no AppKit or FFmpeg
// dependency, so this builds and runs on a plain Linux host, the same way
// tests/playback_presentation_tests.cc does.
//
// What it guards: the number of open decoders stays bounded however many
// sources playback crosses, and what the monitor shows or is about to show
// is never the one closed. PERF12: the rushes playback reaches next are
// opened before it gets there, one at a time, never all of the project's,
// and a rush that is missing or hangs holds up none of the others.

#include "DecoderPool.h"

#include "Document.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <set>
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

std::string Name(int index) { return "SRC" + std::to_string(index); }

// A shot as the timeline holds it: start and length in frames at 25 i/s.
struct Shot {
    int64_t in;
    int64_t frames;
    const char* source;
};

DocumentTrack VideoTrack(int32_t index, const std::vector<Shot>& shots) {
    DocumentTrack track;
    track.kind = "video";
    track.index = index;
    for (const Shot& shot : shots) {
        DocumentClip clip;
        clip.source_id = shot.source;
        clip.timeline_in = {shot.in, 25};
        clip.duration = {shot.frames, 25};
        clip.source_in = {0, 25};
        track.clips.push_back(clip);
    }
    return track;
}

RationalTime Seconds(int64_t seconds) { return {seconds, 1}; }
RationalTime Frames(int64_t frames) { return {frames, 25}; }

std::vector<std::string> Sources(const std::vector<decoder_pool::Need>& needs) {
    std::vector<std::string> sources;
    for (const decoder_pool::Need& need : needs) sources.push_back(need.source);
    return sources;
}

const RationalTime kLookahead = Seconds(decoder_pool::kLookaheadSeconds);

// The interview the author found "not playing in real time" on 4 October 2026:
// 73 s, an interview on V1 and cutaways on V2 (track index 4), ten 4K XAVC
// S-I rushes on an external USB drive in exFAT. The frames are the
// project's own, the rushes renamed.
Document MeasuredInterview() {
    Document document;
    document.sequence.tracks = {VideoTrack(0, {{0, 151, "R37"},
                                               {151, 65, "R37"},
                                               {216, 196, "R38"},
                                               {412, 104, "R38"},
                                               {516, 106, "R38"},
                                               {622, 143, "R38"},
                                               {765, 86, "R38"},
                                               {851, 165, "R39"},
                                               {1016, 158, "R41"},
                                               {1174, 127, "R41"},
                                               {1301, 141, "R41"},
                                               {1442, 226, "R42"},
                                               {1668, 158, "R42"}}),
                                VideoTrack(4, {{75, 76, "R46"},
                                               {280, 132, "R50"},
                                               {522, 100, "R50"},
                                               {718, 133, "R45"},
                                               {918, 98, "R46"},
                                               {1218, 83, "R45"},
                                               {1358, 84, "R47"},
                                               {1520, 80, "R49"},
                                               {1668, 80, "R46"}})};
    return document;
}

// Plays a document through the whole policy, one 25 i/s frame at a time:
// UpcomingNeeds and PlanOpens choose what opens ahead, Usage what closes, as
// main.mm wires them, and each open lands a fixed time after it starts --
// `restOpenFrames` while the editor is at rest, `playingOpenFrames` once
// playback streams another rush from the same drive. A shot whose rush has
// no decoder gets one opened on demand, as -requestFrame:ofSource: does.
struct PlaybackRun {
    // Frames shown with a rush on screen that had no decoder yet.
    int64_t framesWithoutDecoder = 0;
    size_t largestPool = 0;
    size_t mostOpening = 0;
    int opens = 0;
    int reopens = 0;
    // Opens a shot on screen had to ask for itself during playback.
    int onDemandOpens = 0;
    std::set<std::string> everOpened;
};

PlaybackRun Play(const Document& document, int64_t restFrames,
                 int64_t playFrames, int64_t restOpenFrames,
                 int64_t playingOpenFrames,
                 const std::set<std::string>& offline = {},
                 const std::set<std::string>& hanging = {}) {
    using decoder_pool::PoolState;
    PlaybackRun run;
    decoder_pool::Usage usage;
    std::set<std::string> open;
    std::map<std::string, int64_t> opening;  // started at
    std::set<std::string> openedBefore;
    const auto startOpen = [&](const std::string& source, int64_t now) {
        opening[source] = now;
        ++run.opens;
        if (!openedBefore.insert(source).second) ++run.reopens;
        run.everOpened.insert(source);
    };
    for (int64_t frame = 0; frame < restFrames + playFrames; ++frame) {
        const bool playing = frame >= restFrames;
        const int64_t positionFrames = playing ? frame - restFrames : 0;
        const RationalTime position = Frames(positionFrames);
        // Opens that land now.
        for (auto item = opening.begin(); item != opening.end();) {
            const int64_t cost =
                hanging.count(item->first) != 0
                    ? INT64_MAX / 2
                    : (playing ? playingOpenFrames : restOpenFrames);
            if (frame - item->second < cost) {
                ++item;
                continue;
            }
            open.insert(item->first);
            usage.Touch(item->first);
            item = opening.erase(item);
        }
        const std::vector<decoder_pool::Need> needs =
            decoder_pool::UpcomingNeeds(document, position, playing ? 1 : 0,
                                        kLookahead);
        // What is on screen asks for its decoder whatever the plan says.
        std::set<std::string> onScreen;
        for (const decoder_pool::Need& need : needs)
            if (need.distance.value == 0 && offline.count(need.source) == 0)
                onScreen.insert(need.source);
        for (const std::string& source : onScreen) {
            if (open.count(source) != 0) {
                usage.Touch(source);
                continue;
            }
            if (playing) ++run.framesWithoutDecoder;
            if (opening.count(source) == 0) {
                startOpen(source, frame);
                if (playing) ++run.onDemandOpens;
            }
        }
        PoolState pool;
        pool.open = open;
        for (const auto& item : opening)
            pool.opening[item.first] =
                std::chrono::milliseconds((frame - item.second) * 40);
        pool.offline = offline;
        const decoder_pool::Plan plan = decoder_pool::PlanOpens(
            needs, pool, decoder_pool::kDefaultCapacity);
        size_t started = 0;
        for (const std::string& source : plan.open) {
            if (started >= plan.budget) break;
            startOpen(source, frame);
            ++started;
        }
        // Closing past the capacity, never what is planned or on screen.
        std::set<std::string> needed(plan.keep.begin(), plan.keep.end());
        needed.insert(onScreen.begin(), onScreen.end());
        const std::vector<std::string> openList(open.begin(), open.end());
        for (const std::string& source :
             usage.ToClose(openList, needed, decoder_pool::kDefaultCapacity)) {
            open.erase(source);
            usage.Forget(source);
        }
        run.largestPool = std::max(run.largestPool, open.size());
        run.mostOpening = std::max(run.mostOpening, opening.size());
    }
    return run;
}

}  // namespace

int main() {
    using decoder_pool::Usage;

    {
        Usage usage;
        usage.Touch("A");
        usage.Touch("B");
        Check(usage.ToClose({"A", "B"}, {}, 2).empty(),
              "nothing closes while the pool is within its capacity");
        Check(
            usage.LastUse("A") < usage.LastUse("B") && usage.LastUse("C") == 0,
            "touches are ordered and an untouched source has none");
    }

    {
        Usage usage;
        for (const char* source : {"A", "B", "C", "D"}) usage.Touch(source);
        usage.Touch("A");
        const std::vector<std::string> closed =
            usage.ToClose({"A", "B", "C", "D"}, {}, 2);
        Check(closed == std::vector<std::string>{"B", "C"},
              "the least recently used close first, down to the capacity");
    }

    {
        Usage usage;
        for (const char* source : {"A", "B", "C", "D"}) usage.Touch(source);
        const std::vector<std::string> closed =
            usage.ToClose({"A", "B", "C", "D"}, {"A", "B"}, 2);
        Check(closed == std::vector<std::string>{"C", "D"},
              "a needed source is never closed, however old its last use");
    }

    {
        Usage usage;
        for (const char* source : {"A", "B", "C"}) usage.Touch(source);
        const std::vector<std::string> closed =
            usage.ToClose({"A", "B", "C"}, {"A", "B", "C"}, 1);
        Check(closed.empty(),
              "when everything open is needed nothing closes, over capacity "
              "or not: what is on screen always decodes");
        const std::vector<std::string> partial =
            usage.ToClose({"A", "B", "C"}, {"B", "C"}, 1);
        Check(partial == std::vector<std::string>{"A"},
              "only the unneeded close when the needed exceed the capacity");
    }

    {
        Usage usage;
        usage.Touch("A");
        usage.Touch("B");
        usage.Forget("A");
        usage.Touch("C");
        Check(
            usage.LastUse("A") == 0 && usage.ToClose({"A", "B", "C"}, {}, 2) ==
                                           std::vector<std::string>{"A"},
            "a forgotten source has no history left");
    }

    // Playback across a long reel: one new source per cut, the cut's
    // outgoing and incoming sources needed. The pool never grows past its
    // capacity, which is what bounds the memory.
    {
        Usage usage;
        std::vector<std::string> open;
        size_t largest = 0;
        for (int cut = 0; cut < 64; ++cut) {
            const std::string incoming = Name(cut);
            usage.Touch(incoming);
            open.push_back(incoming);
            const std::set<std::string> needed = {
                incoming, cut > 0 ? Name(cut - 1) : incoming};
            for (const std::string& source :
                 usage.ToClose(open, needed, decoder_pool::kDefaultCapacity)) {
                usage.Forget(source);
                open.erase(std::find(open.begin(), open.end(), source));
            }
            largest = std::max(largest, open.size());
        }
        Check(largest == decoder_pool::kDefaultCapacity,
              "64 cuts through 64 sources keep at most the capacity open");
        Check(std::find(open.begin(), open.end(), Name(63)) != open.end() &&
                  std::find(open.begin(), open.end(), Name(62)) != open.end(),
              "the sources of the last cut are still open");
    }

    // PERF12 -- what playback reaches next, read from the timeline.
    {
        const Document document = MeasuredInterview();
        const std::vector<decoder_pool::Need> atRest =
            decoder_pool::UpcomingNeeds(document, Frames(0), 0, kLookahead);
        Check(Sources(atRest) ==
                  std::vector<std::string>{"R37", "R46", "R38", "R50"},
              "at rest, the rush on screen first, then the next ones forward "
              "within the lookahead, each once, soonest first");
        Check(atRest.size() == 4 && atRest[0].distance == Frames(0) &&
                  atRest[1].distance == Frames(75) &&
                  atRest[2].distance == Frames(216) &&
                  atRest[3].distance == Frames(280),
              "each need says how far its first frame is");
        const std::vector<decoder_pool::Need> later =
            decoder_pool::UpcomingNeeds(document, Frames(400), 1, kLookahead);
        Check(Sources(later) == std::vector<std::string>{"R38", "R50", "R45"},
              "a rush on screen and coming back is needed once, at distance "
              "zero, and the window stops at the lookahead (R39, 451 "
              "frames ahead, is not in it)");
        const std::vector<std::string> faster = Sources(
            decoder_pool::UpcomingNeeds(document, Frames(400), 2, kLookahead));
        Check(faster == std::vector<std::string>{"R38", "R50", "R45", "R39",
                                                 "R46", "R41"},
              "at twice the speed the window spans twice the timeline");
        const std::vector<decoder_pool::Need> backward =
            decoder_pool::UpcomingNeeds(document, Frames(900), -1, kLookahead);
        Check(Sources(backward) ==
                  std::vector<std::string>{"R39", "R38", "R45", "R50"},
              "played backward, the rushes behind the playhead, each entered "
              "by its end, the interview before the cutaway that ends with "
              "it");
        Check(backward.size() == 4 && backward[1].distance == Frames(49) &&
                  backward[2].distance == Frames(49) &&
                  backward[3].distance == Frames(278),
              "a clip is entered backward at its last frame");
    }

    {
        // Only what the Record monitor composites is opened: not a hidden
        // track, not an audio or caption track, not a motion clip (its
        // videos are MotionMedia's), not an angle a multicam group hides.
        Document document;
        DocumentTrack hidden = VideoTrack(2, {{0, 50, "HIDDEN"}});
        hidden.visible = false;
        DocumentTrack audio = VideoTrack(3, {{0, 50, "AUDIO"}});
        audio.kind = "audio";
        DocumentTrack caption = VideoTrack(5, {{0, 50, "CAPTION"}});
        caption.kind = "caption";
        DocumentTrack motion = VideoTrack(1, {{0, 50, ""}});
        motion.clips[0].composition_id = "COMPOSITION";
        DocumentTrack angles =
            VideoTrack(0, {{0, 50, "ANGLE_A"}, {50, 50, "SHOWN"}});
        DocumentTrack other = VideoTrack(4, {{0, 50, "ANGLE_B"}});
        MulticamGroup group;
        group.angles = {{GenerateUlid(), "A", angles.clips[0].id},
                        {GenerateUlid(), "B", other.clips[0].id}};
        group.active_angle_id = group.angles[1].id;
        document.sequence.tracks = {hidden, audio,  caption,
                                    motion, angles, other};
        document.sequence.multicam_groups = {group};
        Check(Sources(decoder_pool::UpcomingNeeds(document, Frames(0), 1,
                                                  kLookahead)) ==
                  std::vector<std::string>{"ANGLE_B", "SHOWN"},
              "hidden, audio and caption tracks, motion clips and hidden "
              "angles open nothing");
    }

    {
        // Equal distances keep track order; a clip that starts at the
        // playhead is on screen.
        Document document;
        document.sequence.tracks = {VideoTrack(3, {{25, 25, "UPPER"}}),
                                    VideoTrack(1, {{25, 25, "LOWER"}}),
                                    VideoTrack(2, {{0, 25, "FIRST"}})};
        Check(Sources(decoder_pool::UpcomingNeeds(document, Frames(25), 1,
                                                  kLookahead)) ==
                  std::vector<std::string>{"LOWER", "UPPER"},
              "two shots entering together open in track order, and a shot "
              "ending at the playhead is behind it");
        Check(Sources(decoder_pool::UpcomingNeeds(document, Frames(25), -1,
                                                  kLookahead)) ==
                  std::vector<std::string>{"LOWER", "FIRST", "UPPER"},
              "played backward from a cut, both sides of it are needed at "
              "once, in track order");
    }

    // PERF12 -- which decoders to hold and which to open now.
    {
        using decoder_pool::Need;
        using decoder_pool::PlanOpens;
        using decoder_pool::PoolState;
        const std::vector<Need> needs = {
            {"SHOWN", Frames(0)},   {"NEXT", Frames(25)},
            {"THEN", Frames(50)},   {"LATER", Frames(75)},
            {"FAR", Frames(100)},   {"FARTHER", Frames(125)},
            {"BEYOND", Frames(150)}};
        PoolState pool;
        pool.open = {"SHOWN"};
        decoder_pool::Plan plan = PlanOpens(needs, pool, 6);
        Check(plan.keep == std::vector<std::string>{"SHOWN", "NEXT", "THEN",
                                                    "LATER", "FAR", "FARTHER"},
              "the pool holds what is on screen and the next ones, up to the "
              "capacity: never all of the project's rushes");
        Check(plan.open == std::vector<std::string>{"NEXT", "THEN", "LATER",
                                                    "FAR", "FARTHER"} &&
                  plan.budget == 1,
              "the soonest rush without a decoder opens first, one at a time");

        pool.opening = {{"NEXT", std::chrono::seconds(3)}};
        plan = PlanOpens(needs, pool, 6);
        Check(plan.budget == 0 && plan.open.front() == "THEN",
              "while an open runs, the next one waits for it");

        pool.opening = {{"NEXT", decoder_pool::kStaleOpen}};
        plan = PlanOpens(needs, pool, 6);
        Check(plan.budget == 1,
              "an open that has not answered for kStaleOpen no longer holds "
              "the slot");
        pool.opening = {{"NEXT", decoder_pool::kStaleOpen},
                        {"THEN", decoder_pool::kStaleOpen},
                        {"LATER", decoder_pool::kStaleOpen}};
        plan = PlanOpens(needs, pool, 6);
        Check(plan.budget == 0,
              "stuck opens or not, never more than kMaxOpeningInFlight in "
              "flight");

        pool = {};
        pool.open = {"SHOWN"};
        pool.offline = {"NEXT", "THEN"};
        plan = PlanOpens(needs, pool, 6);
        Check(plan.keep == std::vector<std::string>{"SHOWN", "LATER", "FAR",
                                                    "FARTHER", "BEYOND"} &&
                  plan.open.front() == "LATER" && plan.budget == 1,
              "an offline rush takes neither a place in the pool nor the "
              "opening slot: the next ones open as if it were not there");

        pool = {};
        pool.closing = {"NEXT"};
        plan = PlanOpens(needs, pool, 6);
        Check(plan.keep[1] == "NEXT" &&
                  std::find(plan.open.begin(), plan.open.end(), "NEXT") ==
                      plan.open.end(),
              "a rush still being closed is held, and reopened only once "
              "closed");

        const std::vector<Need> crowded = {{"V1", Frames(0)},
                                           {"V2", Frames(0)},
                                           {"V3", Frames(0)},
                                           {"NEXT", Frames(25)}};
        plan = PlanOpens(crowded, {}, 2);
        Check(plan.keep == std::vector<std::string>{"V1", "V2", "V3"},
              "what is on screen is held past the capacity, and nothing "
              "ahead then");
    }

    // PERF12 -- the measured interview played through the whole policy.
    // Opens cost what they cost on 4 October: 0.3 s at rest, 7 s while
    // playback streams another rush from the same drive (one open at a
    // time). Two seconds at rest, then the 73 s played through.
    {
        const Document document = MeasuredInterview();
        const PlaybackRun run = Play(document, 50, 1826, 8, 175);
        Check(run.framesWithoutDecoder == 0,
              "with opens of 7 s in playback, no shot of the interview "
              "starts before its rush is open (" +
                  std::to_string(run.framesWithoutDecoder) +
                  " frames without a decoder)");
        Check(run.onDemandOpens == 0,
              "every open in playback was made ahead, none on demand at a "
              "cut");
        Check(run.largestPool <= decoder_pool::kDefaultCapacity &&
                  run.mostOpening <= decoder_pool::kMaxOpeningAhead,
              "never more than the capacity open, nor more than one open in "
              "flight");
        Check(run.reopens == 0 && run.everOpened.size() == 10,
              "the ten rushes of the timeline each open once: the capacity "
              "of six is enough for this interview");
    }

    {
        // A rush that is not there costs the others nothing.
        const Document document = MeasuredInterview();
        const PlaybackRun run = Play(document, 50, 1826, 8, 175, {"R46"});
        Check(run.framesWithoutDecoder == 0 &&
                  run.everOpened.count("R46") == 0 &&
                  run.everOpened.size() == 9,
              "an offline rush is never opened, and every other one still "
              "opens ahead of its cut");
    }

    {
        // A rush whose open never answers: once it has run for kStaleOpen
        // the next ones open anyway.
        const Document document = MeasuredInterview();
        const PlaybackRun run = Play(document, 50, 1826, 8, 175, {}, {"R46"});
        Check(run.everOpened.size() == 10 &&
                  run.mostOpening <= decoder_pool::kMaxOpeningInFlight,
              "a hanging open holds up the opens behind it for kStaleOpen "
              "at most, and opens in flight stay bounded");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "decoder pool tests passed\n";
    return 0;
}
