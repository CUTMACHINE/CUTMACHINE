#include "ClipSpeed.h"
#include "Document.h"
#include "IsolatedFrames.h"
#include "Json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "TestSupport.h"

// R8 -- docs/roadmap/REEL-2026-09.md. The portable half of the ticket: the
// detection rule and the range planning, with no FFmpeg and no macOS. This
// is where acceptance criteria 1 and 2 are decided, because this is where
// they can be decided without a rush -- the film they were measured on is
// out of the repository (guard 7 of the lot), so the figures it gave are
// reproduced synthetically: a shot of 99 frames at mean luma 104 with one
// frame at 182, and the same flash halved by a dissolve's opacity.

namespace {

constexpr int32_t kWidth = 96;
constexpr int32_t kHeight = 54;

uint8_t Clamp(double value) {
    if (value < 0.0) return 0;
    if (value > 255.0) return 255;
    return static_cast<uint8_t>(std::lround(value));
}

// A textured plane whose values spread about `mean`. Textured on purpose: a
// flat plane would put every pixel in one histogram bin, so a shift of eight
// luma levels would move the whole distribution and the picture trigger
// would fire on nothing. Real footage spreads over many bins, and that is
// the case the thresholds have to survive.
std::vector<uint8_t> Shot(double mean, int32_t offset, double seed) {
    std::vector<uint8_t> plane(static_cast<size_t>(kWidth) * kHeight);
    for (int32_t y = 0; y < kHeight; ++y) {
        for (int32_t x = 0; x < kWidth; ++x) {
            const double u = x + offset;
            const double texture =
                28.0 * std::sin((u + seed) * 0.21) +
                18.0 * std::sin((y + seed) * 0.37) +
                12.0 * (((static_cast<int32_t>(u) * 7 + y * 13) % 16) - 8) /
                    8.0;
            plane[static_cast<size_t>(y) * kWidth + x] = Clamp(mean + texture);
        }
    }
    return plane;
}

// Two levels only, at the same mean as Shot(mean): a picture that shares an
// average brightness with another and no distribution at all. The cut
// between the two is what the picture-return condition exists for.
std::vector<uint8_t> Bimodal(double mean) {
    std::vector<uint8_t> plane(static_cast<size_t>(kWidth) * kHeight);
    for (size_t index = 0; index < plane.size(); ++index) {
        plane[index] = Clamp(index % 2 == 0 ? mean - 60.0 : mean + 60.0);
    }
    return plane;
}

IsolatedFrameMeasurement Measure(const std::vector<uint8_t>& plane) {
    IsolatedFrameMeasurement measurement;
    measurement.mean_luma = FrameMeanLuma(plane.data(), kWidth, kHeight);
    FrameLumaHistogram(plane.data(), kWidth, kHeight,
                       measurement.histogram.data());
    return measurement;
}

// A still shot of `count` frames at `mean`, with `flash` inserted at index
// `at` for `length` frames. The film's case is Still(99, 104, 40, 182, 1).
IsolatedFrameSeries Still(int64_t count, double mean, int64_t at, double flash,
                          int64_t length) {
    IsolatedFrameSeries series;
    const IsolatedFrameMeasurement body = Measure(Shot(mean, 0, 0.0));
    const IsolatedFrameMeasurement spike = Measure(Shot(flash, 0, 0.0));
    for (int64_t index = 0; index < count; ++index) {
        series.frames.push_back(index >= at && index < at + length ? spike
                                                                   : body);
    }
    return series;
}

int64_t Levels(int64_t scaled) { return scaled / kIsolatedFrameLumaScale; }

DocumentSource MakeSource(const Ulid& id, int32_t rate, int64_t frames) {
    DocumentSource source;
    source.id = id;
    source.path = id + ".mov";
    source.rate = {rate, 1};
    source.duration = {frames, rate};
    return source;
}

DocumentClip MakeClip(const Ulid& id, const Ulid& sourceId, int64_t sourceIn,
                      int64_t duration, int64_t timelineIn, int32_t rate) {
    DocumentClip clip;
    clip.id = id;
    clip.source_id = sourceId;
    clip.source_in = {sourceIn, rate};
    clip.duration = {duration, rate};
    clip.timeline_in = {timelineIn, rate};
    return clip;
}

}  // namespace

int main() {
    // ---- The three measurements ----------------------------------------
    {
        const std::vector<uint8_t> grey(static_cast<size_t>(kWidth) * kHeight,
                                        104);
        Check(FrameMeanLuma(grey.data(), kWidth, kHeight) ==
                  104 * kIsolatedFrameLumaScale,
              "a flat plane measures its own level");
        IsolatedFrameMeasurement flat = Measure(grey);
        Check(flat.histogram[13] == kIsolatedFrameHistogramScale,
              "and every pixel lands in the bin of that level");
        int64_t total = 0;
        const IsolatedFrameMeasurement textured = Measure(Shot(104.0, 0, 0.0));
        for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin)
            total += textured.histogram[static_cast<size_t>(bin)];
        Check(total <= kIsolatedFrameHistogramScale &&
                  total > kIsolatedFrameHistogramScale - 64,
              "a histogram sums to at most one, flooring bin by bin: " +
                  std::to_string(total));
        Check(LumaHistogramDistance(textured, textured) == 0,
              "a frame is at no distance from itself");
        Check(LumaHistogramDistance(flat, Measure(Shot(20.0, 0, 0.0))) > 900,
              "two pictures with no bin in common are a whole picture apart");
    }

    IsolatedFrameSettings settings;

    // ---- Criterion 1: the flash of the first film ----------------------
    // Mean luma 104 to 182 on one frame of a 99-frame shot, then back.
    {
        const IsolatedFrameSeries series = Still(99, 104.0, 40, 182.0, 1);
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1,
              "the flash of the first film is found once in its 99 frames, "
              "got " +
                  std::to_string(events.size()));
        if (events.size() == 1) {
            Check(events[0].first_frame == 40 && events[0].frames == 1,
                  "on its own frame, alone");
            Check(Levels(events[0].deviation) >= 70 &&
                      Levels(events[0].baseline) == 104,
                  "with the deviation and the baseline it was measured at (" +
                      std::to_string(Levels(events[0].deviation)) + " over " +
                      std::to_string(Levels(events[0].baseline)) + ")");
            Check(events[0].level_trigger && events[0].picture_trigger,
                  "and both the level and the picture say so");
        }
    }

    // The same flash inside a dissolve handle, where the export shows it at
    // half opacity over black: the deviation halves to 39 levels and the
    // whole picture moves half as far. It still has to be found -- that is
    // the second half of acceptance criterion 1.
    {
        const IsolatedFrameSeries series = Still(99, 52.0, 40, 91.0, 1);
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1 && events[0].first_frame == 40,
              "the same flash at half opacity is still found, got " +
                  std::to_string(events.size()) + " event(s)");
        if (events.size() == 1) {
            Check(Levels(events[0].deviation) >= 35 &&
                      Levels(events[0].deviation) <= 42,
                  "at half the deviation (" +
                      std::to_string(Levels(events[0].deviation)) + " levels)");
        }
    }

    // A flash spread over two frames is one event of two frames, not two
    // events and not none.
    {
        const IsolatedFrameSeries series = Still(60, 104.0, 30, 182.0, 2);
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1 && events[0].first_frame == 30 &&
                  events[0].frames == 2,
              "a two-frame flash is one event of two frames");
    }

    // Three frames is not an isolated frame: it has a shape a fade or a
    // camera can produce, and the rule says so rather than guessing.
    {
        const IsolatedFrameSeries series = Still(60, 104.0, 30, 182.0, 3);
        Check(DetectIsolatedFrames(series, settings).empty(),
              "a three-frame deviation is not reported");
    }

    // A black frame is the same event in the other direction.
    {
        const IsolatedFrameSeries series = Still(60, 104.0, 25, 0.0, 1);
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1 && events[0].deviation < 0,
              "a single black frame is found, darker than its neighbours");
    }

    // ---- Criterion 2: the three false positives the ticket names -------
    // A progressive lighting rise: two levels a frame for thirty frames.
    {
        IsolatedFrameSeries series;
        for (int64_t index = 0; index < 60; ++index) {
            series.frames.push_back(Measure(
                Shot(104.0 + 2.0 * std::min<int64_t>(index, 30), 0, 0.0)));
        }
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.empty(),
              "a progressive lighting rise raises no alarm, got " +
                  std::to_string(events.size()));
    }

    // A fast pan: the picture travels twenty pixels a frame across a
    // textured, unevenly lit scene, so both the level and the histogram move
    // hard from frame to frame and neither ever returns.
    {
        IsolatedFrameSeries series;
        for (int64_t index = 0; index < 60; ++index) {
            series.frames.push_back(
                Measure(Shot(104.0 + 9.0 * std::sin(index * 0.9),
                             static_cast<int32_t>(index * 20), 0.0)));
        }
        std::vector<int64_t> steps;
        for (size_t index = 1; index < series.frames.size(); ++index) {
            steps.push_back(std::abs(series.frames[index].mean_luma -
                                     series.frames[index - 1].mean_luma));
        }
        std::sort(steps.begin(), steps.end());
        // First assert the fixture is the hard case it was chosen for: a pan
        // that barely moved would pass the check below for the wrong reason.
        Check(Levels(steps[steps.size() / 2]) >= 4,
              "the pan moves the level by at least four levels a frame, "
              "median " +
                  std::to_string(Levels(steps[steps.size() / 2])));
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.empty(), "a fast pan raises no alarm, got " +
                                  std::to_string(events.size()));
    }

    // A cut: the level changes and stays changed.
    {
        IsolatedFrameSeries series;
        for (int64_t index = 0; index < 60; ++index) {
            series.frames.push_back(Measure(index < 30 ? Shot(104.0, 0, 0.0)
                                                       : Shot(150.0, 0, 11.0)));
        }
        Check(DetectIsolatedFrames(series, settings).empty(),
              "a montage cut raises no alarm");
    }

    // And the cut the level cannot see: two shots of the same average
    // brightness and no distribution in common. Only the picture-return
    // condition rejects this one.
    {
        IsolatedFrameSeries series;
        for (int64_t index = 0; index < 60; ++index) {
            series.frames.push_back(
                Measure(index < 30 ? Shot(104.0, 0, 0.0) : Bimodal(104.0)));
        }
        const IsolatedFrameMeasurement& before = series.frames[29];
        const IsolatedFrameMeasurement& after = series.frames[30];
        Check(std::abs(before.mean_luma - after.mean_luma) <
                  settings.return_tolerance,
              "the fixture's two shots do share an average brightness");
        Check(LumaHistogramDistance(before, after) > 500,
              "and share no distribution at all");
        Check(DetectIsolatedFrames(series, settings).empty(),
              "a cut between two shots of the same brightness raises no "
              "alarm either");
    }

    // A flash inside the pan: the hard case both ways round. The picture
    // returns two frames of travel later rather than to the same frame, so a
    // fixed picture-return floor would throw it away.
    {
        IsolatedFrameSeries series;
        for (int64_t index = 0; index < 60; ++index) {
            series.frames.push_back(Measure(
                Shot(index == 30 ? 182.0 : 104.0 + 9.0 * std::sin(index * 0.9),
                     static_cast<int32_t>(index * 20), 0.0)));
        }
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1 && events[0].first_frame == 30,
              "a flash inside a fast pan is still found, got " +
                  std::to_string(events.size()) + " event(s)");
    }

    // ---- What the rule refuses to answer -------------------------------
    // A run at the very edge of a measured range has no neighbourhood on one
    // side, so nothing says it returns and it is not reported. This is the
    // whole reason the scan adds a context margin around every used range.
    {
        IsolatedFrameSeries series = Still(20, 104.0, 1, 182.0, 1);
        Check(DetectIsolatedFrames(series, settings).empty(),
              "a flash one frame from the edge of a range is not judged");
        series = Still(20, 104.0, 10, 182.0, 1);
        series.first_frame = 1200;
        const std::vector<IsolatedFrameEvent> events =
            DetectIsolatedFrames(series, settings);
        Check(events.size() == 1 && events[0].first_frame == 1210,
              "and an event carries the media's own frame number");
    }

    // ---- Bounding the pass to what the timeline uses -------------------
    Document document;
    document.sequence.frame_rate = {25, 1};
    const Ulid fast = "01KR0000000000000000000050";
    const Ulid slow = "01KR0000000000000000000025";
    document.sources.push_back(MakeSource(fast, 50, 5000));
    document.sources.push_back(MakeSource(slow, 25, 2000));
    for (const DocumentSource& source : document.sources) {
        LibraryMedia media;
        media.id = source.id;
        media.path = source.path;
        media.filename = source.path;
        media.rate = source.rate;
        media.duration = source.duration;
        document.library.push_back(media);
    }
    DocumentTrack video;
    video.id = "01KR00000000000000000000V1";
    video.kind = "video";
    video.index = 1;
    // Two seconds of a 50 i/s rush at source frame 1000, then a second of
    // the 25 i/s one, then another piece of the first rush far away.
    video.clips.push_back(
        MakeClip("01KR00000000000000000000C1", fast, 1000, 50, 0, 50));
    video.clips.push_back(
        MakeClip("01KR00000000000000000000C2", slow, 400, 25, 25, 25));
    video.clips.push_back(
        MakeClip("01KR00000000000000000000C3", fast, 4000, 50, 50, 50));
    document.sequence.tracks.push_back(video);

    IsolatedFrameScanSettings planning;
    std::vector<IsolatedFrameScanTarget> targets;
    std::string error;
    Check(PlanIsolatedFrameScan(document, planning, targets, error),
          "the used ranges plan: " + error);
    Check(targets.size() == 2,
          "one target per media, got " + std::to_string(targets.size()));
    if (targets.size() == 2) {
        Check(targets[0].media_id == fast && targets[0].spans.size() == 2,
              "the two pieces of the 50 i/s rush stay two ranges");
        if (targets[0].spans.size() == 2) {
            // PRD38 -- a second of a 50 i/s rush in a 25 i/s sequence shows
            // source frames 1000, 1002 ... 1048: the range ends after the
            // last frame shown, not at source_in + duration.
            Check(targets[0].spans[0].first_frame == 1000 - 6 &&
                      targets[0].spans[0].end_frame == 1049 + 6,
                  "each one is the clip's own source frames plus the context "
                  "margin, got [" +
                      std::to_string(targets[0].spans[0].first_frame) + ", " +
                      std::to_string(targets[0].spans[0].end_frame) + ")");
        }
        // The sum of the ranges, not the sum of the rushes: 5 000 + 2 000
        // frames exist and 159 are measured. That is the whole point of
        // acceptance criterion 3.
        int64_t measured = 0;
        for (const IsolatedFrameScanTarget& target : targets)
            for (const SourceFrameSpan& span : target.spans)
                measured += span.end_frame - span.first_frame;
        Check(measured == 61 + 61 + 37,
              "and the pass covers 159 frames of the 7 000 the project "
              "holds, got " +
                  std::to_string(measured));
    }

    // A dissolve reads past the cut on one side and before it on the other,
    // and those handles are where the film's flash hid. The planned ranges
    // have to follow the transition, not the clip.
    {
        Document dissolved = document;
        dissolved.sequence.tracks[0].clips.resize(2);
        dissolved.sequence.tracks[0].clips[1] =
            MakeClip("01KR00000000000000000000C2", fast, 3000, 50, 50, 50);
        DocumentTransition transition;
        transition.id = "01KR00000000000000000000T1";
        transition.track_id = dissolved.sequence.tracks[0].id;
        transition.left_clip_id = "01KR00000000000000000000C1";
        transition.right_clip_id = "01KR00000000000000000000C2";
        transition.duration = {12, 25};
        transition.alignment = TransitionAlignment::Center;
        dissolved.sequence.transitions.push_back(transition);
        std::vector<IsolatedFrameScanTarget> handled;
        Check(PlanIsolatedFrameScan(dissolved, planning, handled, error),
              "a timeline with a dissolve plans: " + error);
        Check(handled.size() == 1 && handled[0].spans.size() == 2,
              "both pieces come from the same rush");
        if (handled.size() == 1 && handled[0].spans.size() == 2) {
            // Twelve sequence frames at 25 i/s split 6 before the cut and 6
            // after, which is 12 and 12 source frames at 50 i/s. PRD38 --
            // the outgoing clip's last frame shown is then its sequence
            // frame 30, source frame 1060.
            Check(handled[0].spans[0].end_frame == 1061 + 6,
                  "the outgoing clip is read into its tail handle, to " +
                      std::to_string(handled[0].spans[0].end_frame));
            Check(handled[0].spans[1].first_frame == 3000 - 12 - 6,
                  "and the incoming one from before its head, at " +
                      std::to_string(handled[0].spans[1].first_frame));
        }
    }

    // Ranges close enough that skipping the gap costs more than it saves are
    // measured as one.
    {
        Document adjacent = document;
        adjacent.sequence.tracks[0].clips.resize(2);
        adjacent.sequence.tracks[0].clips[1] =
            MakeClip("01KR00000000000000000000C2", fast, 1080, 50, 25, 50);
        std::vector<IsolatedFrameScanTarget> merged;
        Check(PlanIsolatedFrameScan(adjacent, planning, merged, error),
              "two nearby ranges plan: " + error);
        Check(merged.size() == 1 && merged[0].spans.size() == 1 &&
                  merged[0].spans[0].first_frame == 994 &&
                  merged[0].spans[0].end_frame == 1136,
              "they merge into one range");
    }

    // A motion clip has no media to decode, and a clip whose source the
    // document does not carry is refused rather than guessed at.
    {
        Document motion = document;
        motion.sequence.tracks[0].clips.resize(1);
        motion.sequence.tracks[0].clips[0].source_id.clear();
        motion.sequence.tracks[0].clips[0].composition_id =
            "01KR00000000000000000000M1";
        std::vector<IsolatedFrameScanTarget> none;
        Check(PlanIsolatedFrameScan(motion, planning, none, error) &&
                  none.empty(),
              "a motion clip plans nothing to decode");
        Document orphan = document;
        orphan.sequence.tracks[0].clips.resize(1);
        orphan.sequence.tracks[0].clips[0].source_id =
            "01KR00000000000000000000XX";
        std::vector<IsolatedFrameScanTarget> refused;
        Check(!PlanIsolatedFrameScan(orphan, planning, refused, error) &&
                  error.find("unknown source") != std::string::npos,
              "a clip with no source refuses the plan: " + error);
    }

    // ---- PRD38: the range follows the clip's speed ----------------------
    // ClipSpeed.h's three cases on the 25 i/s rush -- 2/1, 1/2, 2/5 -- and
    // the fashion film's 1/2 on the 50 i/s rush, in a 25 i/s sequence. With
    // no context margin and no merging, each planned range has to be the
    // frames the clip shows, first to last, every one of them asked of the
    // rule here, one sequence frame at a time.
    Document sped;
    sped.sequence.frame_rate = {25, 1};
    sped.sources = document.sources;
    sped.library = document.library;
    const Ulid twiceId = "01KR00000000000000000000S2";
    const Ulid halfId = "01KR00000000000000000000S3";
    const Ulid fifthsId = "01KR00000000000000000000S4";
    const Ulid filmId = "01KR00000000000000000000S5";
    {
        DocumentTrack track;
        track.id = "01KR00000000000000000000V2";
        track.kind = "video";
        track.index = 1;
        // Two seconds at 2/1 from source frame 0: frames 0, 2 ... 98.
        DocumentClip twice = MakeClip(twiceId, slow, 0, 50, 0, 25);
        twice.speed = {2, 1};
        // Two seconds at 1/2 from frame 400: 400, 400, 401, 401 ... 424.
        DocumentClip half = MakeClip(halfId, slow, 400, 50, 50, 25);
        half.speed = {1, 2};
        // One second at 2/5 from frame 1000: floor(2k/5), up to 1009.
        DocumentClip fifths = MakeClip(fifthsId, slow, 1000, 25, 100, 25);
        fifths.speed = {2, 5};
        // Two seconds at 1/2 of the 50 i/s rush from its frame 4000: each
        // source frame exactly once, 4000 to 4049.
        DocumentClip film = MakeClip(filmId, fast, 2000, 50, 125, 25);
        film.speed = {1, 2};
        track.clips = {twice, half, fifths, film};
        sped.sequence.tracks.push_back(track);
    }
    const auto shownRange = [&](const Ulid& clipId) {
        const DocumentClip* clip = sped.FindClip(clipId);
        const DocumentSource* source = sped.FindSource(clip->source_id);
        const int64_t frames = clip->duration.to_frames(25);
        int64_t first = std::numeric_limits<int64_t>::max();
        int64_t last = std::numeric_limits<int64_t>::min();
        for (int64_t frame = 0; frame < frames; ++frame) {
            const int64_t shown =
                ClipSourceFrameAt(*clip, {frame, 25}, source->rate);
            first = std::min(first, shown);
            last = std::max(last, shown);
        }
        return SourceFrameSpan{first, last + 1};
    };
    {
        IsolatedFrameScanSettings exact;
        exact.context_frames = 0;
        exact.merge_gap_frames = 0;
        std::vector<IsolatedFrameScanTarget> planned;
        Check(PlanIsolatedFrameScan(sped, exact, planned, error),
              "sped-up and slowed clips plan: " + error);
        const auto spanOf = [&](const Ulid& mediaId, size_t index) {
            for (const IsolatedFrameScanTarget& target : planned)
                if (target.media_id == mediaId && index < target.spans.size())
                    return target.spans[index];
            return SourceFrameSpan{-1, -1};
        };
        const auto same = [](const SourceFrameSpan& left,
                             const SourceFrameSpan& right) {
            return left.first_frame == right.first_frame &&
                   left.end_frame == right.end_frame;
        };
        const auto text = [](const SourceFrameSpan& span) {
            return "[" + std::to_string(span.first_frame) + ", " +
                   std::to_string(span.end_frame) + ")";
        };
        const struct {
            Ulid clip;
            Ulid media;
            size_t index;
            SourceFrameSpan expected;
            const char* label;
        } cases[] = {
            {twiceId, slow, 0, {0, 99}, "2/1 reads source frames 0 to 98"},
            {halfId, slow, 1, {400, 425}, "1/2 reads 400 to 424, each twice"},
            {fifthsId, slow, 2, {1000, 1010}, "2/5 reads 1000 to 1009"},
            {filmId, fast, 0, {4000, 4050}, "1/2 at 50 i/s reads 4000-4049"},
        };
        for (const auto& item : cases) {
            const SourceFrameSpan rule = shownRange(item.clip);
            const SourceFrameSpan plan = spanOf(item.media, item.index);
            Check(same(rule, item.expected) && same(plan, rule),
                  std::string(item.label) + ": the rule shows " + text(rule) +
                      ", the plan measures " + text(plan));
        }
    }
    {
        // The ticket's case, with the default margin: the shot at 2/1 used
        // to be measured to source frame 56.
        std::vector<IsolatedFrameScanTarget> planned;
        Check(PlanIsolatedFrameScan(sped, planning, planned, error) &&
                  !planned.empty() && planned[0].media_id == slow &&
                  planned[0].spans.size() == 3 &&
                  planned[0].spans[0].first_frame == 0 &&
                  planned[0].spans[0].end_frame == 99 + 6,
              "the 2/1 shot is measured to frame 98 and its margin: " + error);

        // A scan written by the plan that ignored the speed covers the shot
        // to frame 56 only. What it does not hold is named under
        // "unscanned", never passed over.
        IsolatedFrameScan old;
        old.media_id = slow;
        old.rate = {25, 1};
        old.analysis_width = 256;
        old.analysis_height = 144;
        old.series.push_back(Still(56, 104.0, 30, 104.0, 1));
        std::map<Ulid, IsolatedFrameScan> scans;
        scans.emplace(slow, old);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(DescribeIsolatedFramesForAgent(
                                         sped, planned, scans, {}, settings),
                                     parsed, parseError),
              "the view of the sped-up timeline is JSON: " + parseError);
        bool named = false;
        const mcp_json::Value* unscanned = parsed.Find("unscanned");
        if (unscanned != nullptr && unscanned->IsArray()) {
            for (const mcp_json::Value& entry : unscanned->AsArray()) {
                const mcp_json::Value* missing = entry.Find("missing_ranges");
                int64_t first = 0;
                int64_t end = 0;
                if (entry.Find("media_id")->AsString() == slow &&
                    missing != nullptr && missing->IsArray() &&
                    !missing->AsArray().empty() &&
                    missing->AsArray()[0].Find("first_frame")->AsInt64(first) &&
                    missing->AsArray()[0].Find("end_frame")->AsInt64(end)) {
                    named = first == 56 && end == 99 + 6;
                }
            }
        }
        Check(named,
              "the frames the old plan left out are listed as unscanned: " +
                  (unscanned != nullptr ? unscanned->Dump() : ""));

        // A scan that covers the plan, with a flash on source frame 40 --
        // shown by the 2/1 shot on its sequence frame 20 -- another on 61,
        // which 2/1 skips, a two-frame one on 81 and 82, of which 2/1 shows
        // the second on its sequence frame 41, and one on 410, which 1/2
        // shows on its sequence frames 20 and 21. ResolveSourceFrame refuses
        // this media (its clips are not at 1/1), so the rule situates them.
        IsolatedFrameScan full = old;
        full.series.clear();
        IsolatedFrameSeries head = Still(105, 104.0, 40, 182.0, 1);
        head.frames[61] = head.frames[40];
        head.frames[81] = head.frames[40];
        head.frames[82] = head.frames[40];
        IsolatedFrameSeries middle = Still(37, 104.0, 16, 182.0, 1);
        middle.first_frame = 394;
        IsolatedFrameSeries tail = Still(22, 104.0, 0, 104.0, 1);
        tail.first_frame = 994;
        full.series = {head, middle, tail};
        scans.clear();
        scans.emplace(slow, full);
        Check(mcp_json::Value::Parse(DescribeIsolatedFramesForAgent(
                                         sped, planned, scans, {}, settings),
                                     parsed, parseError),
              "the view of the covered timeline is JSON: " + parseError);
        std::map<int64_t, std::string> placed;
        const mcp_json::Value* sources = parsed.Find("sources");
        if (sources != nullptr && sources->IsArray()) {
            for (const mcp_json::Value& source : sources->AsArray()) {
                if (source.Find("media_id")->AsString() != slow) continue;
                for (const mcp_json::Value& item :
                     source.Find("isolated_frames")->AsArray()) {
                    int64_t frame = 0;
                    item.Find("first_frame")->AsInt64(frame);
                    placed[frame] = item.Find("clips")->Dump();
                }
            }
        }
        const auto clipsAt = [&](int64_t frame) {
            const auto found = placed.find(frame);
            return found == placed.end() ? std::string("<none>")
                                         : found->second;
        };
        const auto on = [](const Ulid& clipId, int64_t frame) {
            return R"([{"clip_id":")" + clipId +
                   R"(","track_id":"01KR00000000000000000000V2",)"
                   R"("timeline_position":{"value":)" +
                   std::to_string(frame) + R"(,"rate":25}}])";
        };
        Check(placed.size() == 4 && clipsAt(40) == on(twiceId, 20) &&
                  clipsAt(61) == "[]" && clipsAt(81) == on(twiceId, 41) &&
                  clipsAt(410) == on(halfId, 70),
              "each flash is placed where the montage shows it, the one 2/1 "
              "skips nowhere, and a run by the frame of it 2/1 shows: 40 " +
                  clipsAt(40) + ", 61 " + clipsAt(61) + ", 81 " + clipsAt(81) +
                  ", 410 " + clipsAt(410));
    }
    {
        // The fashion film's own mix: the 50 i/s rush at 1/1 in one shot,
        // which shows one source frame in two, and at 1/2 in another. The
        // rule situates the 1/1 shot's frames too, so its odd frame 1021 is
        // shown nowhere rather than placed between two sequence frames.
        Document mixed;
        mixed.sequence.frame_rate = {25, 1};
        mixed.sources = document.sources;
        mixed.library = document.library;
        DocumentTrack track;
        track.id = "01KR00000000000000000000V3";
        track.kind = "video";
        track.index = 1;
        DocumentClip plain =
            MakeClip("01KR00000000000000000000M2", fast, 1000, 50, 0, 50);
        DocumentClip slowed =
            MakeClip("01KR00000000000000000000M3", fast, 4000, 100, 50, 50);
        slowed.speed = {1, 2};
        track.clips = {plain, slowed};
        mixed.sequence.tracks.push_back(track);
        std::vector<IsolatedFrameScanTarget> planned;
        Check(PlanIsolatedFrameScan(mixed, planning, planned, error) &&
                  planned.size() == 1 && planned[0].spans.size() == 2 &&
                  planned[0].spans[0].first_frame == 994 &&
                  planned[0].spans[0].end_frame == 1055 &&
                  planned[0].spans[1].first_frame == 3994 &&
                  planned[0].spans[1].end_frame == 4056,
              "the 1/1 and the 1/2 piece of one rush plan their own frames: " +
                  error);
        IsolatedFrameScan scan;
        scan.media_id = fast;
        scan.rate = {50, 1};
        scan.analysis_width = 256;
        scan.analysis_height = 144;
        IsolatedFrameSeries first = Still(61, 104.0, 16, 182.0, 1);
        first.first_frame = 994;
        first.frames[27] = first.frames[16];
        IsolatedFrameSeries second = Still(62, 104.0, 27, 182.0, 1);
        second.first_frame = 3994;
        scan.series = {first, second};
        std::map<Ulid, IsolatedFrameScan> scans;
        scans.emplace(fast, scan);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(DescribeIsolatedFramesForAgent(
                                         mixed, planned, scans, {}, settings),
                                     parsed, parseError),
              "the view of the mixed rush is JSON: " + parseError);
        std::map<int64_t, std::string> placed;
        const mcp_json::Value* sources = parsed.Find("sources");
        if (sources != nullptr && sources->IsArray() &&
            sources->AsArray().size() == 1) {
            for (const mcp_json::Value& item :
                 sources->AsArray()[0].Find("isolated_frames")->AsArray()) {
                int64_t frame = 0;
                item.Find("first_frame")->AsInt64(frame);
                placed[frame] = item.Find("clips")->Dump();
            }
        }
        const auto at = [&](int64_t frame) {
            const auto found = placed.find(frame);
            return found == placed.end() ? std::string("<none>")
                                         : found->second;
        };
        const auto on = [](const std::string& clipId, int64_t frame) {
            return R"([{"clip_id":")" + clipId +
                   R"(","track_id":"01KR00000000000000000000V3",)"
                   R"("timeline_position":{"value":)" +
                   std::to_string(frame) + R"(,"rate":25}}])";
        };
        Check(placed.size() == 3 &&
                  at(1010) == on("01KR00000000000000000000M2", 5) &&
                  at(1021) == "[]" &&
                  at(4021) == on("01KR00000000000000000000M3", 46),
              "1/1 shows the even frame and not the odd one, 1/2 shows "
              "both: 1010 " +
                  at(1010) + ", 1021 " + at(1021) + ", 4021 " + at(4021));
    }

    // ---- The derush: rushes whole, whether the timeline uses them -------
    {
        std::vector<IsolatedFrameScanTarget> whole;
        Check(PlanMediaIsolatedFrameScan(document, {slow, fast, slow}, whole,
                                         error) &&
                  whole.size() == 2 && whole[0].media_id == slow &&
                  whole[1].media_id == fast,
              "named media plan once each, in the order given: " + error);
        if (whole.size() == 2) {
            Check(whole[0].spans.size() == 1 &&
                      whole[0].spans[0].first_frame == 0 &&
                      whole[0].spans[0].end_frame == 2000 &&
                      whole[1].spans.size() == 1 &&
                      whole[1].spans[0].end_frame == 5000 &&
                      whole[1].rate.num == 50,
                  "a named media is scanned from its first frame to its "
                  "last, on its own grid");
        }
        std::vector<IsolatedFrameScanTarget> refused;
        Check(!PlanMediaIsolatedFrameScan(
                  document, {"01KR00000000000000000000XX"}, refused, error) &&
                  error.find("unknown media_id") != std::string::npos,
              "an unknown media refuses the derush plan: " + error);
        Document audioOnly = document;
        audioOnly.library[1].metadata_complete = true;
        audioOnly.library[1].has_video = false;
        Check(!PlanMediaIsolatedFrameScan(audioOnly, {slow}, refused, error) &&
                  error.find("no video") != std::string::npos,
              "an audio-only media refuses the derush plan: " + error);
    }

    // ---- The cache ------------------------------------------------------
    IsolatedFrameScan scan;
    scan.media_id = fast;
    scan.rate = {50, 1};
    scan.analysis_width = 256;
    scan.analysis_height = 144;
    {
        IsolatedFrameSeries first = Still(99, 104.0, 40, 182.0, 1);
        first.first_frame = 994;
        IsolatedFrameSeries second = Still(62, 90.0, 2, 0.0, 1);
        second.first_frame = 3994;
        scan.series.push_back(first);
        scan.series.push_back(second);
    }
    const std::string json = SerializeIsolatedFrameScan(scan);
    Check(json.find('.') == std::string::npos,
          "a cache file holds no decimal point anywhere");
    IsolatedFrameScan reread;
    Check(DeserializeIsolatedFrameScan(json, reread, error),
          "the cache reads back: " + error);
    Check(SerializeIsolatedFrameScan(reread) == json,
          "and writes the same bytes again");
    Check(reread.series.size() == 2 && reread.series[0].first_frame == 994 &&
              reread.series[0].frames.size() == 99,
          "with its ranges on the media's own frame grid");
    Check(!DeserializeIsolatedFrameScan("{\"version\":2,\"media_id\":\"x\"}",
                                        reread, error),
          "another version is refused rather than guessed at");
    Check(!DeserializeIsolatedFrameScan(
              "{\"version\":1,\"media_id\":\"x\",\"frame_rate_num\":25,"
              "\"frame_rate_den\":1,\"analysis_width\":8,"
              "\"analysis_height\":8,\"histogram_bins\":16,\"ranges\":[]}",
              reread, error),
          "so is a cache written with another histogram");
    Check(!DeserializeIsolatedFrameScan(
              "{\"version\":1,\"media_id\":\"x\",\"frame_rate_num\":25,"
              "\"frame_rate_den\":1,\"analysis_width\":8,"
              "\"analysis_height\":8,\"histogram_bins\":32,"
              "\"ranges\":[{\"first_frame\":0,\"frames\":2,\"mean\":[1],"
              "\"histogram\":[]}]}",
              reread, error),
          "and a range whose arrays do not match its length");

    // What the cache does not hold is named, never passed over: a cache
    // written for one edit does not answer for a clip trimmed outwards.
    {
        IsolatedFrameScanTarget target;
        target.media_id = fast;
        target.rate = {50, 1};
        target.spans = {{994, 1093}, {2000, 2100}, {3994, 4014}};
        const std::vector<SourceFrameSpan> missing =
            UncoveredSpans(target, scan);
        Check(missing.size() == 1 && missing[0].first_frame == 2000 &&
                  missing[0].end_frame == 2100,
              "the uncovered range is named exactly, got " +
                  std::to_string(missing.size()) + " range(s)");
        target.spans = {{900, 1093}};
        const std::vector<SourceFrameSpan> head = UncoveredSpans(target, scan);
        Check(head.size() == 1 && head[0].first_frame == 900 &&
                  head[0].end_frame == 994,
              "and so is the head a trim added");
    }

    // ---- The agent-facing view -----------------------------------------
    {
        std::map<Ulid, IsolatedFrameScan> scans;
        scans.emplace(fast, scan);
        std::map<Ulid, IsolatedFrameCacheRead> reads;
        const std::string view = DescribeIsolatedFramesForAgent(
            document, targets, scans, reads, settings);
        mcp_json::Value parsed;
        std::string parseError;
        Check(mcp_json::Value::Parse(view, parsed, parseError),
              "the view is JSON: " + parseError);
        const mcp_json::Value* sources = parsed.Find("sources");
        const mcp_json::Value* unscanned = parsed.Find("unscanned");
        Check(sources != nullptr && sources->IsArray() &&
                  sources->AsArray().size() == 1,
              "the scanned media is reported");
        Check(unscanned != nullptr && unscanned->IsArray() &&
                  unscanned->AsArray().size() == 1,
              "and the media nobody scanned is listed, not dropped");
        if (unscanned != nullptr && unscanned->IsArray() &&
            unscanned->AsArray().size() == 1) {
            Check(unscanned->AsArray()[0].Find("reason")->AsString() ==
                      "never scanned",
                  "with the reason it is unknown");
        }
        int64_t count = 0;
        Check(parsed.Find("isolated_frame_count")->AsInt64(count) && count == 2,
              "both isolated frames of the cache are reported, got " +
                  std::to_string(count));
        if (sources != nullptr && sources->IsArray() &&
            sources->AsArray().size() == 1) {
            const mcp_json::Value* found =
                sources->AsArray()[0].Find("isolated_frames");
            Check(found != nullptr && found->IsArray() &&
                      found->AsArray().size() == 2,
                  "under the media they belong to");
            if (found != nullptr && found->IsArray() &&
                found->AsArray().size() == 2) {
                const mcp_json::Value& first = found->AsArray()[0];
                int64_t frame = 0;
                Check(
                    first.Find("first_frame")->AsInt64(frame) && frame == 1034,
                    "at the frame of the rush they sit on, got " +
                        std::to_string(frame));
                const mcp_json::Value* clips = first.Find("clips");
                Check(clips != nullptr && clips->IsArray() &&
                          clips->AsArray().size() == 1,
                      "situated on the clip that plays them");
                // The second sits in the context margin of the third clip,
                // outside any clip: reported all the same, with no clip.
                const mcp_json::Value* outside =
                    found->AsArray()[1].Find("clips");
                Check(outside != nullptr && outside->IsArray() &&
                          outside->AsArray().empty(),
                      "or with none when they sit in a handle or a margin");
            }
        }
    }

    if (failures == 0) std::cout << "isolated frame tests passed\n";
    return failures == 0 ? 0 : 1;
}
