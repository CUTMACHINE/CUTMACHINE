#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "RationalTime.h"

#include <array>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// R8 -- docs/roadmap/REEL-2026-09.md. Finds the single frames a shot holds
// that its neighbours do not: a studio flash, a black frame, a corrupted
// one.
//
// It exists because ShotQuality cannot see them and is not meant to.
// GenerateShotQuality samples four times a second, so at 50 i/s it misses a
// one-frame event eleven times out of twelve; the first film of this lot
// carried a flash of one frame (mean luma 104 -> 182 -> 104) that only the
// control render found. Raising ShotQuality's rate would make every rush's
// cache fifteen times bigger to answer a question asked of a few seconds of
// each, which is why this is a separate pass bounded to the ranges a
// timeline uses.
//
// It reports and situates; it does not qualify. The two versions that
// followed the same film start eight shots on the flash frame, on a beat:
// the same measurement that finds a defect finds a figure, and which one it
// is belongs to the author (guard 4 of the lot -- the author decides, the
// engine measures). Nothing here is named "error", and nothing here is an
// Operations.h operation: this is read-only analysis and writes only its own
// cache.
//
// Shaped like ShotQuality.h and SpeechOnset.h: a pure core that works on a
// series of measurements, a Generate* pass that decodes with FFmpeg and
// writes a cache, and a Load*/Deserialize* pass that reads the cache back
// without FFmpeg. The split matters more here than usual -- everything that
// decides whether a frame is isolated lives in DetectIsolatedFrames, which
// is plain C++ and is where the ticket's two acceptance criteria are tested,
// on synthetic series, with no media at all.

// Mean luma is carried, and cached, as an integer number of thousandths of
// an 8-bit luma level: 104.0 reads 104000. Thousandths of a level rather
// than a fraction of full scale so every threshold below reads in the unit
// the film's flash was measured in (104 to 182), and one exact division at
// the end of an integer sum keeps two runs over the same pixels identical
// byte for byte (PHILOSOPHY.md principle 6). No cache file holds a float.
constexpr int64_t kIsolatedFrameLumaScale = 1000;

// Luma bins per frame histogram, and the unit a histogram is stored and
// compared in: per-mille of the frame's pixels. 32 bins of 8 levels for the
// reason ShotQuality gives for the same choice -- finer bins put sensor
// noise across bin edges, coarser ones stop separating two pictures that
// share a mean -- and 256 divides by 32 exactly, so a bin index is a shift.
constexpr int32_t kIsolatedFrameHistogramBins = 32;
constexpr int64_t kIsolatedFrameHistogramScale = 1000;

// One measured frame. Deliberately small: a minute of 50 i/s is 3 000 of
// these, and the cache holds them as parallel integer arrays.
struct IsolatedFrameMeasurement {
    // Mean luma of the analysis plane, in kIsolatedFrameLumaScale units.
    int64_t mean_luma = 0;
    // Share of the plane's pixels in each luma bin, in per-mille. Floored,
    // so the bins sum to at most kIsolatedFrameHistogramScale; the missing
    // remainder is the same on every host and never reaches a threshold.
    std::array<int32_t, kIsolatedFrameHistogramBins> histogram{};
};

// A contiguous run of measured frames. `first_frame` is the frame's index in
// the media's own frame grid -- frame k is the k-th picture of the video
// stream, the same addressing DocumentClip::source_in and
// ExportVerification use -- so an event can be reported as a frame number
// without holding anything else.
struct IsolatedFrameSeries {
    int64_t first_frame = 0;
    std::vector<IsolatedFrameMeasurement> frames;
};

// ---------------------------------------------------------------------
// Pure measurement of one plane. `luma` is a tightly packed width*height
// plane of 8-bit samples, which is what the analysis pass asks FFmpeg for,
// so there is no stride to get wrong.
// ---------------------------------------------------------------------

int64_t FrameMeanLuma(const uint8_t* luma, int32_t width, int32_t height);

// Writes kIsolatedFrameHistogramBins per-mille shares into `bins`.
void FrameLumaHistogram(const uint8_t* luma, int32_t width, int32_t height,
                        int32_t* bins);

// Total-variation (L1) distance between two histograms: half the sum of
// absolute per-mille differences, so it reads as "the share of pixels that
// changed luma bin" and returns kIsolatedFrameHistogramScale for two frames
// with no bin in common. Total variation rather than chi-squared for the
// reason FrameHistogramDistanceMetric gives: it accumulates exactly in
// integers, and a metric that forced a float here would cost more than the
// separation it buys.
int64_t LumaHistogramDistance(const IsolatedFrameMeasurement& previous,
                              const IsolatedFrameMeasurement& current);

// ---------------------------------------------------------------------
// Pure detection. No FFmpeg, no filesystem, no Document -- this is what
// tests/isolated_frames_tests.cc exercises on synthetic series, and where
// the ticket's criteria 1 and 2 are decided.
// ---------------------------------------------------------------------

struct IsolatedFrameSettings {
    // Longest run reported. The ticket asks for "the deviations of one or
    // two frames that return to their neighbours' level", and the limit is
    // the rule, not a tuning knob: a three-frame deviation has a shape a
    // camera or a fade can produce, a one-frame one has not.
    int64_t maximum_run_frames = 2;
    // Frames looked at on each side of a candidate run, and the fewest that
    // must be there for it to be judged at all. A run at the very edge of a
    // measured range is not reported: nothing says it returns. That is what
    // the scan's context margin below is for.
    int64_t neighbourhood_frames = 6;
    int64_t minimum_neighbour_frames = 2;
    // How far the two sides of a candidate may sit from each other -- both
    // the medians of the two neighbourhood windows and the two immediate
    // neighbours -- in thousandths of a luma level. This is the condition
    // that separates the three false positives the ticket names from a real
    // isolated frame, and it does it by shape rather than size:
    //   - a cut moves the level and keeps it there, so the two sides
    //     disagree;
    //   - a lighting ramp moves it a little per frame, so the two windows
    //     sit a whole neighbourhood apart -- at 2 levels a frame and a
    //     six-frame window, 14 levels apart, twice this;
    //   - a fast pan does not travel at a constant brightness either, so
    //     the two windows disagree unless the moment happens to be
    //     symmetric.
    // Six levels leaves room for the ordinary breathing of an exposure
    // without letting any of the three through.
    //
    // A floor, though, and never only a floor: a moving shot never sits
    // still, and six absolute levels would refuse to judge a flash that
    // lands inside a pan -- the case the two later versions of the film made
    // a figure of. The condition is therefore taken against the
    // neighbourhood's own median step as well, at return_local_percent, and
    // the larger of the two wins. The ratio cannot save a ramp: a ramp's
    // step and its two windows' distance grow together, and the windows are
    // a whole neighbourhood apart while the ratio is two steps wide.
    int64_t return_tolerance = 6 * kIsolatedFrameLumaScale;
    int64_t return_local_percent = 200;
    // Least mean deviation from the baseline, in the same unit, for the
    // brightness trigger. Eighteen levels, chosen from the only real
    // measurement this ticket has: the film's flash deviates 78 levels, and
    // the same flash at half opacity inside a dissolve handle -- the case
    // acceptance criterion 1 names -- deviates 39. Eighteen sits below half
    // of that with the margin still better than two to one, and far above
    // any frame-to-frame variation a sensor produces.
    int64_t minimum_deviation = 18 * kIsolatedFrameLumaScale;
    // Least histogram distance from the neighbours, per-mille, for the
    // picture trigger. It catches what the mean cannot: a flash that lights
    // part of the frame, or a corrupted frame whose average is unchanged.
    // 150 per-mille is fifteen percent of the picture landing in another
    // luma bin and back in one frame.
    int64_t minimum_histogram_distance = 150;
    // The picture must return as well as the level: the two frames
    // bracketing the run may differ by no more than this, per-mille, or by
    // local_step_percent of the neighbourhood's own movement, whichever is
    // larger. It is the second guard against a cut between two shots of the
    // same average brightness; the relative half of it exists because the
    // floor alone would throw away a genuine flash inside a fast pan, where
    // the two bracketing frames are two frames of travel apart.
    int64_t return_histogram_distance = 120;
    // Both triggers are also taken against the neighbourhood's own
    // frame-to-frame movement, at this percentage of its median step. The
    // absolute floors above only keep this ratio from firing on nothing;
    // the ratio is what keeps a moving shot from reading as a string of
    // isolated frames. Same reasoning, and the same failure it was written
    // against, as ShotSegmentationSettings::cut_motion_local_percent: a
    // camera has inertia, so its movement is spread over several frames,
    // while a flash lands on one.
    int64_t local_step_percent = 300;
};

// One reported frame, or pair of frames.
struct IsolatedFrameEvent {
    // Frame index in the measured domain, and how many frames the run holds
    // (1 or 2, see maximum_run_frames).
    int64_t first_frame = 0;
    int64_t frames = 1;
    // Signed deviation from the baseline of the run's *least* deviating
    // frame, in kIsolatedFrameLumaScale units: what the whole run clears,
    // not its peak. Positive is brighter than the neighbours.
    int64_t deviation = 0;
    // The level the run departs from and returns to: the midpoint of the
    // two immediate neighbours, not of the whole neighbourhood. On a shot
    // that moves, the window medians sit several levels away from the
    // frames either side of the run, and a deviation taken against them
    // would measure the shot's own drift as well as the event.
    int64_t baseline = 0;
    // The run's smallest distance to either bracketing frame, per-mille, and
    // the distance between those two frames -- how far the picture came back.
    int64_t histogram_distance = 0;
    int64_t neighbour_histogram_distance = 0;
    // The neighbourhood's median frame-to-frame step and histogram distance,
    // carried so a caller can see what the run was judged against instead of
    // having to re-derive it.
    int64_t neighbour_step = 0;
    int64_t neighbour_step_distance = 0;
    // Which trigger fired. Both may be true; at least one always is.
    bool level_trigger = false;
    bool picture_trigger = false;
};

// The isolated frames of one contiguous series, ascending and
// non-overlapping. A series too short to have a neighbourhood yields none,
// which is a fact rather than a pass.
std::vector<IsolatedFrameEvent> DetectIsolatedFrames(
    const IsolatedFrameSeries& series, const IsolatedFrameSettings& settings);

// ---------------------------------------------------------------------
// Bounding the pass to what a timeline uses (acceptance criterion 3).
// ---------------------------------------------------------------------

// Half-open range of frames of one media's own frame grid.
struct SourceFrameSpan {
    int64_t first_frame = 0;
    int64_t end_frame = 0;
};

struct IsolatedFrameScanTarget {
    Ulid media_id;
    MediaRate rate;
    RationalTime duration;
    // Ascending, disjoint, clamped to the media.
    std::vector<SourceFrameSpan> spans;
};

struct IsolatedFrameScanSettings {
    // Frames added on each side of every used range. Two reasons, and the
    // second is the one that matters: a run at the edge of a measured range
    // cannot be judged (see neighbourhood_frames), and the montage figure
    // this ticket also has to find puts the flash on a shot's *first* frame.
    // Without the margin, the one frame the author cut to is the one frame
    // the pass cannot see.
    int64_t context_frames = 6;
    // Two ranges of the same media closer than this are measured as one.
    // Decoding runs through the gap anyway; skipping it only saves the
    // scaling and the measurement, which below this length costs more in
    // filter terms than it saves.
    int64_t merge_gap_frames = 50;
    // Ranges per media after merging. Past this, the closest pairs are
    // merged until it fits: a select expression with one term per range has
    // to stay a string FFmpeg parses, and a rush cut into two hundred
    // pieces is being read end to end whatever the expression says.
    int64_t maximum_spans = 64;
};

// The source ranges `document`'s active timeline reads, per media, transition
// handles included. Handles are not an approximation: a cross dissolve reads
// the left clip past its out and the right clip before its in, exactly as
// Timeline::ResolveTrackLayers resolves them, and the film's flash falling in
// one of those handles is acceptance criterion 1.
//
// PRD38 -- a clip's range runs from the source frame it shows on its first
// sequence frame to the one it shows on its last, both named by ClipSpeed.h's
// rule (ClipSourceFrameAt), so a clip's speed is the one the monitor and the
// export apply: at 2/1, fifty sequence frames from source frame 0 read up to
// frame 98, and at 2/5 the last frame shown can sit inside the source frame
// after the one source_in + duration * speed ends on.
//
// Video tracks only, motion clips left out (they have no media to decode).
// Fails on a clip whose source or rate the document does not carry, rather
// than guessing a frame grid, and on a sequence with no usable frame rate.
bool PlanIsolatedFrameScan(const Document& document,
                           const IsolatedFrameScanSettings& settings,
                           std::vector<IsolatedFrameScanTarget>& targets,
                           std::string& error);

// The whole of each media named, rather than what a timeline reads: the
// derush of a shoot before any of it is cut, where the studio flashes are
// found to be kept or thrown away. On 24 September 2026 this took ten
// DERUSH timelines holding every rush, built only to have ranges to scan.
// The order given is kept, duplicates once. Fails on a media the document
// does not hold, has no video, or has no frame grid.
bool PlanMediaIsolatedFrameScan(const Document& document,
                                const std::vector<Ulid>& mediaIds,
                                std::vector<IsolatedFrameScanTarget>& targets,
                                std::string& error);

// ---------------------------------------------------------------------
// FFmpeg measurement pass and its cache.
// ---------------------------------------------------------------------

struct IsolatedFrameScan {
    Ulid media_id;
    MediaRate rate;
    int32_t analysis_width = 0;
    int32_t analysis_height = 0;
    // Ascending and disjoint, in the media's own frame grid.
    std::vector<IsolatedFrameSeries> series;
};

struct IsolatedFrameMeasureSettings {
    // Fixed analysis size, aspect deliberately not preserved, for the reason
    // ShotQualitySettings gives: rescaling edges anisotropically neither
    // creates nor destroys what is being measured, and letterboxing would
    // add flat bars that shift every mean by a constant.
    //
    // 256x144 is 36 864 pixels, so one pixel is a fortieth of a per-mille:
    // the histogram's quantisation stays three orders of magnitude below the
    // smallest distance the detector looks at. Beyond that, resolution buys
    // nothing -- a flash lights the whole frame -- and the decode, not the
    // scaling, is what a 4K rush costs.
    int32_t analysis_width = 256;
    int32_t analysis_height = 144;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
};

// Decodes `inputPath`'s first video stream, measures the frames `spans`
// names, and writes the scan as JSON to `outputPath` (creating parent
// directories as needed). Mirrors GenerateShotQuality's process handling
// through ReadFfmpegRawFrames, including the tail of FFmpeg's errors.
//
// The stream is decoded from its start onto its own frame grid and stopped
// after the last frame asked for, never through an input seek: the same
// choice ExportVerification made, and for the same reason -- an event this
// pass reports is a frame *number*, and a seek that landed one frame out
// would move it without anything saying so. What the spans save is the
// scaling and the measurement of everything outside them, and the decode of
// everything past the last one.
//
// `context` carries progress only: ReadFfmpegRawFrames has no cancellation,
// unlike GenerateShotQuality's own process loop. Stated rather than hidden
// -- this pass is bounded to a few seconds of each rush where the shot
// quality analysis decodes whole rushes, so the two are not the same wait,
// but a caller that needs to stop it mid-decode cannot yet.
bool GenerateIsolatedFrameScan(const std::string& inputPath,
                               const std::string& outputPath,
                               const std::string& mediaId, MediaRate rate,
                               const std::vector<SourceFrameSpan>& spans,
                               const IsolatedFrameMeasureSettings& settings,
                               MediaTaskContext& context, std::string& error);

std::string SerializeIsolatedFrameScan(const IsolatedFrameScan& scan);
bool DeserializeIsolatedFrameScan(const std::string& json,
                                  IsolatedFrameScan& scan, std::string& error);

enum class IsolatedFrameCacheState {
    Loaded,
    // No cache file for this media: it was never scanned.
    Absent,
    // A file is there and this build cannot read it: a version it does not
    // read, a newer one, or a damaged file. The distinction is the one
    // BENCH-2026-09 B3 forced on the shot quality cache: an absent cache is
    // a media still unknown, an unreadable one is a scan to redo.
    Unreadable,
};

struct IsolatedFrameCacheRead {
    IsolatedFrameCacheState state = IsolatedFrameCacheState::Absent;
    int64_t found_version = 0;
    std::string detail;
};

inline constexpr int64_t kReadableIsolatedFrameVersions[] = {1};

IsolatedFrameCacheRead ReadIsolatedFrameCache(const std::string& path,
                                              IsolatedFrameScan& scan);
bool LoadIsolatedFrameScan(const std::string& path, IsolatedFrameScan& scan,
                           std::string& error);

// The parts of `target` no series of `scan` holds. Empty means the cache
// answers every range the timeline uses; anything else is what a report has
// to declare unscanned rather than pass over. A cache is keyed by media, not
// by edit, so trimming a clip outwards is the ordinary way to get one.
std::vector<SourceFrameSpan> UncoveredSpans(
    const IsolatedFrameScanTarget& target, const IsolatedFrameScan& scan);

// ---------------------------------------------------------------------
// Agent- and CLI-facing view.
// ---------------------------------------------------------------------

// The read-only isolated-frame view of `document`'s active timeline, built
// from scans the caller has already loaded and keyed by media id.
//
// Each event is situated as well as reported: ResolveSourceFrame gives the
// clips that play that frame and where they play it, so a caller reads "this
// frame of this rush, at this position of the montage" instead of a frame
// number it has to convert. An event with no clip is one the margin found
// beside a used range, or inside a transition handle -- still worth naming,
// since that is where a flash hides at half opacity.
//
// PRD38 -- ResolveSourceFrame refuses a media one of whose clips is not at
// 1/1. For that media each clip is asked ClipSpeed.h's rule instead, and the
// event is placed on the first sequence frame that shows one of its frames;
// a clip whose speed skips the whole run (one source frame in two at 2/1)
// does not show it. The rule answers for that media's clips at 1/1 too, so a
// rush at twice the sequence cadence played at 1/1 beside a slowed piece of
// itself situates only the frames it shows -- one in two -- where
// ResolveSourceFrame would have placed the others between two sequence
// frames.
//
// Media whose scan is missing, or whose scan does not cover every range the
// timeline uses, are listed under "unscanned" and are never silently
// omitted: the distinction "measured and clean" against "never measured" is
// the same one list_shot_quality makes, and dropping the second would read
// as the first.
std::string DescribeIsolatedFramesForAgent(
    const Document& document,
    const std::vector<IsolatedFrameScanTarget>& targets,
    const std::map<Ulid, IsolatedFrameScan>& scans,
    const std::map<Ulid, IsolatedFrameCacheRead>& reads,
    const IsolatedFrameSettings& settings);
