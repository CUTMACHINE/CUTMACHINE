#include "IsolatedFrames.h"

#include "ClipSpeed.h"
#include "FfmpegPipe.h"
#include "Json.h"
#include "SourceAddress.h"
#include "Timeline.h"
#include "Ulid.h"

#include <algorithm>
#include <exception>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

// Lower median of a copy of `values`, by the same rule and for the same
// reason ShotQualityPercentile states: on an even count it takes the lower
// of the two middle values rather than averaging them, so every figure this
// file reports is one that was actually measured.
int64_t Median(std::vector<int64_t> values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[(values.size() - 1) / 2];
}

int64_t AbsoluteValue(int64_t value) { return value < 0 ? -value : value; }

bool ReadInt64Field(const Value& object, const std::string& key,
                    int64_t& output) {
    const Value* field = object.Find(key);
    return field != nullptr && field->AsInt64(output);
}

// Sorted, disjoint spans covering the same frames as `spans`, merging any
// two closer than `gap`. Merging is deterministic: the input is sorted
// first, so two callers holding the same ranges in another order plan the
// same pass.
std::vector<SourceFrameSpan> MergeSpans(std::vector<SourceFrameSpan> spans,
                                        int64_t gap) {
    std::sort(spans.begin(), spans.end(),
              [](const SourceFrameSpan& left, const SourceFrameSpan& right) {
                  return left.first_frame != right.first_frame
                             ? left.first_frame < right.first_frame
                             : left.end_frame < right.end_frame;
              });
    std::vector<SourceFrameSpan> merged;
    for (const SourceFrameSpan& span : spans) {
        if (span.end_frame <= span.first_frame) continue;
        if (!merged.empty() &&
            span.first_frame - merged.back().end_frame <= (gap < 0 ? 0 : gap)) {
            merged.back().end_frame =
                std::max(merged.back().end_frame, span.end_frame);
            continue;
        }
        merged.push_back(span);
    }
    return merged;
}

// Merges the closest pairs until at most `limit` spans remain.
void LimitSpanCount(std::vector<SourceFrameSpan>& spans, int64_t limit) {
    if (limit < 1) limit = 1;
    while (static_cast<int64_t>(spans.size()) > limit) {
        size_t closest = 0;
        int64_t best = std::numeric_limits<int64_t>::max();
        for (size_t index = 1; index < spans.size(); ++index) {
            const int64_t distance =
                spans[index].first_frame - spans[index - 1].end_frame;
            if (distance < best) {
                best = distance;
                closest = index;
            }
        }
        spans[closest - 1].end_frame =
            std::max(spans[closest - 1].end_frame, spans[closest].end_frame);
        spans.erase(spans.begin() + static_cast<std::ptrdiff_t>(closest));
    }
}

// The frames a transition makes a clip read beyond its own edit rectangle,
// as exact times. A transition belongs to the sequence, the left clip is
// read `post` past its out and the right clip `pre` before its in, and the
// split of the duration is TransitionRender.h's -- the one the monitor, the
// export and Document::Validate also use.
//
// R5 -- a colour dip reads nothing beyond the edit: it veils the frames
// that are already there. Its handles are zero, so an isolated frame is
// still looked for on the shot's own range and not on frames no delivery
// contains.
void TransitionHandles(const DocumentSequence& sequence,
                       const DocumentTransition& transition, RationalTime& pre,
                       RationalTime& post) {
    pre = RationalTime{};
    post = RationalTime{};
    if (TransitionIsColorDip(transition)) return;
    TransitionSpan span;
    std::string error;
    if (!ResolveTransitionSpan(sequence, transition, span, error)) return;
    pre = RationalTime{span.pre_frames * sequence.frame_rate.den,
                       sequence.frame_rate.num};
    post = RationalTime{span.post_frames * sequence.frame_rate.den,
                        sequence.frame_rate.num};
}

// PRD38 -- the index of the first frame of a `rate` grid that starts at or
// after `time`.
int64_t FirstFrameFrom(const RationalTime& time, const MediaRate& rate) {
    const int64_t frame = time.to_frames(rate.num, rate.den);
    return IsOnSourceFrameBoundary(time, rate) ? frame : frame + 1;
}

// PRD38 -- the sequence frames a clip covers, [first, last], with `pre` and
// `post` of transition handle on either side. A sequence frame is the
// clip's when it starts inside that span, which is how Timeline resolves a
// position to a clip. False when the clip covers no whole frame.
bool ClipSequenceFrames(const DocumentClip& clip, const MediaRate& rate,
                        const RationalTime& pre, const RationalTime& post,
                        int64_t& first, int64_t& last) {
    first = FirstFrameFrom(clip.timeline_in.sub(pre), rate);
    last =
        FirstFrameFrom(clip.timeline_in.add(clip.duration).add(post), rate) - 1;
    return first <= last;
}

// PRD38 -- the source frame `clip` shows on sequence frame `frame`, asked of
// ClipSpeed.h's rule, the one the monitor and the export also follow. A
// frame before the clip's head or past its tail is a transition handle, and
// the rule reads it the way Timeline::ResolveTrackLayers does.
int64_t SourceFrameShownOn(const DocumentClip& clip, const MediaRate& rate,
                           const MediaRate& sourceRate, int64_t frame) {
    const RationalTime position{frame * static_cast<int64_t>(rate.den),
                                rate.num};
    return ClipSourceFrameAt(clip, position.sub(clip.timeline_in), sourceRate);
}

// PRD38 -- where the montage shows a run of source frames of `mediaId`,
// [frame, frame + count): for each clip of that media, the first sequence
// frame whose picture is one of them, found by asking ClipSpeed.h's rule
// itself. The rule never decreases from one frame to the next (a speed is
// positive), so a binary search over the clip's frames asks it about twenty
// times at most. A clip that skips the whole run -- at 2/1, one source frame
// in two -- shows it nowhere; a two-frame run whose first frame is skipped
// is shown by its second.
//
// It answers where ResolveSourceFrame refuses: a media one of whose clips is
// not at 1/1. Without it, an event of a sped-up shot was reported with no
// clip, which reads as a frame lying in a handle or in the margin.
struct ShownSourceFrame {
    Ulid clip_id;
    Ulid track_id;
    int32_t track_index = 0;
    RationalTime timeline_position;
};

std::vector<ShownSourceFrame> ClipsShowingSourceFrames(
    const Document& document, const Ulid& mediaId, const MediaRate& sourceRate,
    int64_t frame, int64_t count) {
    std::vector<ShownSourceFrame> shown;
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || sourceRate.num <= 0 ||
        sourceRate.den <= 0) {
        return shown;
    }
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.source_id != mediaId || IsMotionClip(clip)) continue;
            try {
                int64_t first = 0;
                int64_t last = 0;
                if (!ClipSequenceFrames(clip, rate, RationalTime{},
                                        RationalTime{}, first, last))
                    continue;
                int64_t low = first;
                int64_t high = last + 1;
                while (low < high) {
                    const int64_t middle = low + ((high - low) / 2);
                    if (SourceFrameShownOn(clip, rate, sourceRate, middle) <
                        frame) {
                        low = middle + 1;
                    } else {
                        high = middle;
                    }
                }
                if (low > last ||
                    SourceFrameShownOn(clip, rate, sourceRate, low) >=
                        frame + std::max<int64_t>(count, 1))
                    continue;
                shown.push_back(
                    {clip.id, track.id, track.index,
                     RationalTime{low * static_cast<int64_t>(rate.den),
                                  rate.num}});
            } catch (const std::exception&) {
                // The plan asked the same rule at this clip's ends without
                // overflowing; a clip that overflows here is not situated.
                continue;
            }
        }
    }
    std::sort(shown.begin(), shown.end(),
              [](const ShownSourceFrame& left, const ShownSourceFrame& right) {
                  if (left.track_index != right.track_index)
                      return left.track_index < right.track_index;
                  return left.timeline_position < right.timeline_position;
              });
    return shown;
}

Value TimeValue(const RationalTime& time) {
    Value result = Value::MakeObject();
    result.Set("value", Value::MakeInt(time.value));
    result.Set("rate", Value::MakeInt(time.rate));
    return result;
}

Value SpanValue(const SourceFrameSpan& span) {
    Value result = Value::MakeObject();
    result.Set("first_frame", Value::MakeInt(span.first_frame));
    result.Set("end_frame", Value::MakeInt(span.end_frame));
    return result;
}

}  // namespace

int64_t FrameMeanLuma(const uint8_t* luma, int32_t width, int32_t height) {
    if (luma == nullptr || width < 1 || height < 1) return 0;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    __int128 total = 0;
    for (int64_t index = 0; index < count; ++index) total += luma[index];
    return static_cast<int64_t>(total * kIsolatedFrameLumaScale / count);
}

void FrameLumaHistogram(const uint8_t* luma, int32_t width, int32_t height,
                        int32_t* bins) {
    if (bins == nullptr) return;
    for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin)
        bins[bin] = 0;
    if (luma == nullptr || width < 1 || height < 1) return;
    const int64_t count =
        static_cast<int64_t>(width) * static_cast<int64_t>(height);
    std::array<int64_t, kIsolatedFrameHistogramBins> counts{};
    // 256 / kIsolatedFrameHistogramBins is a power of two, so the bin index
    // is a shift and no sample can land out of range.
    const int32_t shift = 256 / kIsolatedFrameHistogramBins;
    for (int64_t index = 0; index < count; ++index)
        ++counts[static_cast<size_t>(luma[index] / shift)];
    for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin) {
        bins[bin] = static_cast<int32_t>(counts[static_cast<size_t>(bin)] *
                                         kIsolatedFrameHistogramScale / count);
    }
}

int64_t LumaHistogramDistance(const IsolatedFrameMeasurement& previous,
                              const IsolatedFrameMeasurement& current) {
    int64_t total = 0;
    for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin) {
        total += AbsoluteValue(
            static_cast<int64_t>(current.histogram[static_cast<size_t>(bin)]) -
            previous.histogram[static_cast<size_t>(bin)]);
    }
    return total / 2;
}

std::vector<IsolatedFrameEvent> DetectIsolatedFrames(
    const IsolatedFrameSeries& series, const IsolatedFrameSettings& settings) {
    std::vector<IsolatedFrameEvent> events;
    const int64_t count = static_cast<int64_t>(series.frames.size());
    const int64_t window = std::max<int64_t>(settings.neighbourhood_frames, 1);
    const int64_t least =
        std::max<int64_t>(settings.minimum_neighbour_frames, 1);
    const int64_t longest = std::max<int64_t>(settings.maximum_run_frames, 1);
    const auto& frames = series.frames;
    for (int64_t start = least; start < count;) {
        bool emitted = false;
        for (int64_t length = 1; length <= longest && !emitted; ++length) {
            const int64_t end = start + length;  // Exclusive.
            if (end + least > count) break;
            const int64_t beforeFirst = std::max<int64_t>(0, start - window);
            const int64_t afterEnd = std::min<int64_t>(count, end + window);
            if (start - beforeFirst < least || afterEnd - end < least) break;
            std::vector<int64_t> before;
            std::vector<int64_t> after;
            std::vector<int64_t> steps;
            std::vector<int64_t> stepDistances;
            for (int64_t index = beforeFirst; index < start; ++index) {
                before.push_back(frames[static_cast<size_t>(index)].mean_luma);
                if (index > beforeFirst) {
                    steps.push_back(AbsoluteValue(
                        frames[static_cast<size_t>(index)].mean_luma -
                        frames[static_cast<size_t>(index - 1)].mean_luma));
                    stepDistances.push_back(LumaHistogramDistance(
                        frames[static_cast<size_t>(index - 1)],
                        frames[static_cast<size_t>(index)]));
                }
            }
            for (int64_t index = end; index < afterEnd; ++index) {
                after.push_back(frames[static_cast<size_t>(index)].mean_luma);
                if (index > end) {
                    steps.push_back(AbsoluteValue(
                        frames[static_cast<size_t>(index)].mean_luma -
                        frames[static_cast<size_t>(index - 1)].mean_luma));
                    stepDistances.push_back(LumaHistogramDistance(
                        frames[static_cast<size_t>(index - 1)],
                        frames[static_cast<size_t>(index)]));
                }
            }
            const int64_t step = Median(steps);
            const int64_t stepDistance = Median(stepDistances);
            const int64_t beforeLevel = Median(before);
            const int64_t afterLevel = Median(after);
            // The two sides must agree on a level, and each immediate
            // neighbour must already be at it. This is what a cut and a
            // lighting ramp fail; a pan fails it on the immediate
            // neighbours, which never settle. Judged against the
            // neighbourhood's own movement as well as against the floor, so
            // that a shot in motion can still hold a flash.
            const int64_t tolerance =
                std::max(settings.return_tolerance,
                         step * settings.return_local_percent / 100);
            if (AbsoluteValue(beforeLevel - afterLevel) > tolerance) continue;
            const IsolatedFrameMeasurement& leftNeighbour =
                frames[static_cast<size_t>(start - 1)];
            const IsolatedFrameMeasurement& rightNeighbour =
                frames[static_cast<size_t>(end)];
            if (AbsoluteValue(leftNeighbour.mean_luma -
                              rightNeighbour.mean_luma) > tolerance) {
                continue;
            }
            // The level the run departed from and came back to is the two
            // immediate neighbours', not the whole neighbourhood's. On a
            // shot that moves, the window medians are several levels away
            // from the frames either side of the run, and taking the
            // deviation against them would measure the shot's own drift as
            // well as the event -- which is how the first attempt threw
            // away a flash sitting inside a pan.
            const int64_t baseline =
                (leftNeighbour.mean_luma + rightNeighbour.mean_luma) / 2;
            // And the picture must come back, not only the level. Taken
            // against the neighbourhood's own movement as well as against
            // the floor, because the floor alone would throw away a flash
            // inside a fast pan: there the two bracketing frames are two
            // frames of travel apart and legitimately far from each other.
            const int64_t neighbourDistance =
                LumaHistogramDistance(leftNeighbour, rightNeighbour);
            if (neighbourDistance >
                std::max(settings.return_histogram_distance,
                         stepDistance * settings.local_step_percent / 100)) {
                continue;
            }
            const int64_t requiredDeviation =
                std::max(settings.minimum_deviation,
                         step * settings.local_step_percent / 100);
            const int64_t requiredDistance =
                std::max(settings.minimum_histogram_distance,
                         stepDistance * settings.local_step_percent / 100);
            // The run is judged on its *weakest* frame: a two-frame event
            // qualifies only if both frames do, which is what keeps the
            // first frame of a cut from dragging the second one in.
            int64_t weakestDeviation = std::numeric_limits<int64_t>::max();
            int64_t weakestDistance = std::numeric_limits<int64_t>::max();
            int64_t signedDeviation = 0;
            bool sameDirection = true;
            int firstSign = 0;
            for (int64_t index = start; index < end; ++index) {
                const IsolatedFrameMeasurement& frame =
                    frames[static_cast<size_t>(index)];
                const int64_t deviation = frame.mean_luma - baseline;
                const int sign = deviation < 0 ? -1 : 1;
                if (index == start) {
                    firstSign = sign;
                } else if (sign != firstSign) {
                    sameDirection = false;
                }
                if (AbsoluteValue(deviation) < weakestDeviation) {
                    weakestDeviation = AbsoluteValue(deviation);
                    signedDeviation = deviation;
                }
                weakestDistance = std::min(
                    weakestDistance,
                    std::min(LumaHistogramDistance(leftNeighbour, frame),
                             LumaHistogramDistance(rightNeighbour, frame)));
            }
            const bool level =
                sameDirection && weakestDeviation >= requiredDeviation;
            const bool picture = weakestDistance >= requiredDistance;
            if (!level && !picture) continue;
            IsolatedFrameEvent event;
            event.first_frame = series.first_frame + start;
            event.frames = length;
            event.deviation = signedDeviation;
            event.baseline = baseline;
            event.histogram_distance = weakestDistance;
            event.neighbour_histogram_distance = neighbourDistance;
            event.neighbour_step = step;
            event.neighbour_step_distance = stepDistance;
            event.level_trigger = level;
            event.picture_trigger = picture;
            events.push_back(event);
            start = end;
            emitted = true;
        }
        if (!emitted) ++start;
    }
    return events;
}

bool PlanIsolatedFrameScan(const Document& document,
                           const IsolatedFrameScanSettings& settings,
                           std::vector<IsolatedFrameScanTarget>& targets,
                           std::string& error) {
    targets.clear();
    error.clear();
    const int64_t context = std::max<int64_t>(settings.context_frames, 0);
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "the sequence has no usable frame rate";
        return false;
    }
    std::vector<Ulid> order;
    std::map<Ulid, std::vector<SourceFrameSpan>> spans;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video") continue;
        for (const DocumentClip& clip : track.clips) {
            // A motion clip reads a composition, not a media: there is
            // nothing to decode and no source frame grid to report on.
            if (IsMotionClip(clip)) continue;
            const DocumentSource* source = document.FindSource(clip.source_id);
            if (source == nullptr || source->rate.num <= 0 ||
                source->rate.den <= 0) {
                error = "clip '" + clip.id +
                        "' references an unknown source or source rate";
                return false;
            }
            RationalTime pre;
            RationalTime post;
            for (const DocumentTransition& transition :
                 document.sequence.transitions) {
                if (transition.track_id != track.id) continue;
                if (transition.left_clip_id != clip.id &&
                    transition.right_clip_id != clip.id) {
                    continue;
                }
                RationalTime before;
                RationalTime after;
                TransitionHandles(document.sequence, transition, before, after);
                if (transition.left_clip_id == clip.id) post = post.add(after);
                if (transition.right_clip_id == clip.id) pre = pre.add(before);
            }
            // PRD38 -- the source frames the clip shows, first to last, are
            // ClipSpeed.h's rule asked at the first and the last sequence
            // frame it covers: the frames the monitor and the export show,
            // at the clip's speed. The span used to end at source_in plus
            // the clip's duration, so a shot at 2/1 read source frames 0 to
            // 98 and was measured to 56, with nothing under "unscanned".
            // The rule never decreases along a clip, so its two ends bound
            // every frame in between.
            SourceFrameSpan span;
            try {
                int64_t firstFrame = 0;
                int64_t lastFrame = 0;
                if (!ClipSequenceFrames(clip, rate, pre, post, firstFrame,
                                        lastFrame))
                    continue;
                span.first_frame =
                    SourceFrameShownOn(clip, rate, source->rate, firstFrame) -
                    context;
                span.end_frame =
                    SourceFrameShownOn(clip, rate, source->rate, lastFrame) +
                    1 + context;
            } catch (const std::exception& exception) {
                error = "clip '" + clip.id +
                        "' has no exact source range: " + exception.what();
                return false;
            }
            const int64_t frames =
                source->duration.to_frames(source->rate.num, source->rate.den);
            span.first_frame = std::max<int64_t>(0, span.first_frame);
            if (frames > 0) span.end_frame = std::min(span.end_frame, frames);
            if (span.end_frame <= span.first_frame) continue;
            if (spans.find(clip.source_id) == spans.end())
                order.push_back(clip.source_id);
            spans[clip.source_id].push_back(span);
        }
    }
    for (const Ulid& mediaId : order) {
        const DocumentSource* source = document.FindSource(mediaId);
        IsolatedFrameScanTarget target;
        target.media_id = mediaId;
        target.rate = source->rate;
        target.duration = source->duration;
        target.spans =
            MergeSpans(std::move(spans[mediaId]), settings.merge_gap_frames);
        LimitSpanCount(target.spans, settings.maximum_spans);
        targets.push_back(std::move(target));
    }
    return true;
}

bool PlanMediaIsolatedFrameScan(const Document& document,
                                const std::vector<Ulid>& mediaIds,
                                std::vector<IsolatedFrameScanTarget>& targets,
                                std::string& error) {
    targets.clear();
    error.clear();
    std::set<Ulid> seen;
    for (const Ulid& mediaId : mediaIds) {
        if (!seen.insert(mediaId).second) continue;
        const DocumentSource* source = document.FindSource(mediaId);
        const LibraryMedia* media = document.FindLibraryMedia(mediaId);
        if (source == nullptr || media == nullptr) {
            error = "unknown media_id '" + mediaId + "'";
            return false;
        }
        if (media->metadata_complete && !media->has_video) {
            error = "media_id '" + mediaId + "' has no video stream to scan";
            return false;
        }
        const int64_t frames =
            source->rate.num > 0 && source->rate.den > 0
                ? source->duration.to_frames(source->rate.num, source->rate.den)
                : 0;
        if (frames <= 0) {
            error = "media_id '" + mediaId + "' has no frame grid to scan";
            return false;
        }
        IsolatedFrameScanTarget target;
        target.media_id = mediaId;
        target.rate = source->rate;
        target.duration = source->duration;
        target.spans = {SourceFrameSpan{0, frames}};
        targets.push_back(std::move(target));
    }
    if (targets.empty()) error = "no media to scan";
    return !targets.empty();
}

bool GenerateIsolatedFrameScan(const std::string& inputPath,
                               const std::string& outputPath,
                               const std::string& mediaId, MediaRate rate,
                               const std::vector<SourceFrameSpan>& spans,
                               const IsolatedFrameMeasureSettings& settings,
                               MediaTaskContext& context, std::string& error) {
    error.clear();
    if (rate.num <= 0 || rate.den <= 0 || settings.analysis_width < 1 ||
        settings.analysis_height < 1) {
        error = "invalid isolated frame settings or source rate";
        return false;
    }
    const std::vector<SourceFrameSpan> wanted = MergeSpans(spans, 0);
    if (wanted.empty()) {
        error = "no source range to scan";
        return false;
    }
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    if (filesystemError) {
        error = "unable to create isolated frame directory: " +
                filesystemError.message();
        return false;
    }

    int64_t total = 0;
    std::string select;
    for (const SourceFrameSpan& span : wanted) {
        total += span.end_frame - span.first_frame;
        if (!select.empty()) select += '+';
        // Commas inside a filter argument separate filters, so they are
        // escaped rather than quoted: the arguments go to FFmpeg through
        // posix_spawn, never through a shell. `between` is inclusive, hence
        // the last frame rather than the half-open end.
        select += "between(n\\," + std::to_string(span.first_frame) + "\\," +
                  std::to_string(span.end_frame - 1) + ")";
    }
    // `setpts=PTS-STARTPTS,fps=` puts the stream on its own frame grid, so
    // `n` inside `select` is the source frame number and nothing else --
    // the addressing ExportVerification uses for the same reason. The
    // ordering matters twice: `select` before `scale` is what stops a 4K
    // rush from being rescaled outside its used ranges, and
    // `-fps_mode passthrough` is what stops the rawvideo muxer from filling
    // the gaps `select` leaves with duplicated frames -- without it every
    // frame number after the first gap would be wrong, silently.
    const std::string filter =
        "setpts=PTS-STARTPTS,fps=" + std::to_string(rate.num) + "/" +
        std::to_string(rate.den) + ",select=" + select +
        ",scale=" + std::to_string(settings.analysis_width) + ":" +
        std::to_string(settings.analysis_height) + ":flags=area,format=gray";
    const size_t frameBytes = static_cast<size_t>(settings.analysis_width) *
                              static_cast<size_t>(settings.analysis_height);

    IsolatedFrameScan scan;
    scan.media_id = mediaId;
    scan.rate = rate;
    scan.analysis_width = settings.analysis_width;
    scan.analysis_height = settings.analysis_height;
    size_t spanIndex = 0;
    int64_t measured = 0;
    if (!ReadFfmpegRawFrames(
            {settings.ffmpeg_path,
             "-hide_banner",
             "-loglevel",
             "error",
             "-nostdin",
             "-i",
             inputPath,
             "-map",
             "0:v:0",
             "-an",
             "-sn",
             "-fps_mode",
             "passthrough",
             "-vf",
             filter,
             "-frames:v",
             std::to_string(total),
             "-f",
             "rawvideo",
             "-pix_fmt",
             "gray",
             "pipe:1"},
            frameBytes,
            [&](const uint8_t* pixels) {
                if (spanIndex >= wanted.size()) return;
                if (scan.series.empty() ||
                    static_cast<int64_t>(scan.series.back().frames.size()) >=
                        wanted[spanIndex].end_frame -
                            wanted[spanIndex].first_frame) {
                    if (!scan.series.empty()) ++spanIndex;
                    if (spanIndex >= wanted.size()) return;
                    IsolatedFrameSeries series;
                    series.first_frame = wanted[spanIndex].first_frame;
                    scan.series.push_back(std::move(series));
                }
                IsolatedFrameMeasurement measurement;
                measurement.mean_luma = FrameMeanLuma(
                    pixels, settings.analysis_width, settings.analysis_height);
                FrameLumaHistogram(pixels, settings.analysis_width,
                                   settings.analysis_height,
                                   measurement.histogram.data());
                scan.series.back().frames.push_back(measurement);
                ++measured;
                context.SetProgress(total <= 0 ? 0.0
                                               : static_cast<double>(measured) /
                                                     static_cast<double>(total),
                                    "Images isolées");
            },
            error)) {
        error = "unable to decode '" + inputPath + "': " + error;
        return false;
    }
    // A stream shorter than the timeline believes is not an error -- the
    // document may address a media that was replaced -- but the cache says
    // what it actually holds, never what it was asked for, so a later report
    // can name the frames nobody measured.
    while (!scan.series.empty() && scan.series.back().frames.empty())
        scan.series.pop_back();
    if (scan.series.empty()) {
        error = "video stream produced no analysis frames";
        return false;
    }

    // Written through a neighbouring temporary and renamed, as the shot
    // quality cache is: a crashed run leaves no half-parsed file for the
    // next reader to trust.
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() +
         destination.extension().string());
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const std::string json = SerializeIsolatedFrameScan(scan);
        file.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!file) {
            file.close();
            std::filesystem::remove(temporary, filesystemError);
            error = "unable to write isolated frame cache";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "unable to publish isolated frame cache: " +
                filesystemError.message();
        return false;
    }
    return true;
}

// Cache layout, written strictly and read tolerantly. Each range stores its
// first frame and parallel integer arrays -- one mean per frame, one
// flattened histogram of kIsolatedFrameHistogramBins per frame -- so a frame
// number is an index rather than a stored field, and the file holds no float
// literal anywhere. It is the biggest of this project's caches per second
// of media -- 88 bytes a frame measured, so about 260 kB per minute at
// 50 i/s -- which is the price of measuring every frame, and the reason
// the pass is bounded to the used ranges.
std::string SerializeIsolatedFrameScan(const IsolatedFrameScan& scan) {
    std::ostringstream output;
    output << "{\"version\":1,\"media_id\":\""
           << mcp_json::EscapeJsonString(scan.media_id)
           << "\",\"frame_rate_num\":" << scan.rate.num
           << ",\"frame_rate_den\":" << scan.rate.den
           << ",\"analysis_width\":" << scan.analysis_width
           << ",\"analysis_height\":" << scan.analysis_height
           << ",\"histogram_bins\":" << kIsolatedFrameHistogramBins
           << ",\"luma_scale\":" << kIsolatedFrameLumaScale
           << ",\"histogram_scale\":" << kIsolatedFrameHistogramScale
           << ",\"ranges\":[";
    for (size_t index = 0; index < scan.series.size(); ++index) {
        const IsolatedFrameSeries& series = scan.series[index];
        if (index) output << ',';
        output << "{\"first_frame\":" << series.first_frame
               << ",\"frames\":" << series.frames.size() << ",\"mean\":[";
        for (size_t frame = 0; frame < series.frames.size(); ++frame) {
            if (frame) output << ',';
            output << series.frames[frame].mean_luma;
        }
        output << "],\"histogram\":[";
        bool first = true;
        for (const IsolatedFrameMeasurement& frame : series.frames) {
            for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin) {
                if (!first) output << ',';
                first = false;
                output << frame.histogram[static_cast<size_t>(bin)];
            }
        }
        output << "]}";
    }
    output << "]}";
    return output.str();
}

namespace {

// DeserializeIsolatedFrameScan, also handing back the version the cache
// declares (0 when it declares none) so a refusal can say which one it met.
bool DeserializeVersionedIsolatedFrameScan(const std::string& json,
                                           IsolatedFrameScan& scan,
                                           int64_t& version,
                                           std::string& error) {
    error.clear();
    version = 0;
    Value root;
    std::string parseError;
    if (!Value::Parse(json, root, parseError) || !root.IsObject()) {
        error = "malformed isolated frame cache: " + parseError;
        return false;
    }
    if (!ReadInt64Field(root, "version", version) || version != 1) {
        error = "unsupported isolated frame cache version";
        return false;
    }
    IsolatedFrameScan parsed;
    const Value* mediaId = root.Find("media_id");
    if (mediaId == nullptr || !mediaId->IsString()) {
        error = "isolated frame cache has no media_id";
        return false;
    }
    parsed.media_id = mediaId->AsString();
    int64_t num = 0;
    int64_t den = 0;
    int64_t width = 0;
    int64_t height = 0;
    int64_t bins = 0;
    if (!ReadInt64Field(root, "frame_rate_num", num) || num <= 0 ||
        num > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "frame_rate_den", den) || den <= 0 ||
        den > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_width", width) || width <= 0 ||
        width > std::numeric_limits<int32_t>::max() ||
        !ReadInt64Field(root, "analysis_height", height) || height <= 0 ||
        height > std::numeric_limits<int32_t>::max()) {
        error = "isolated frame cache has invalid analysis settings";
        return false;
    }
    // Refused rather than resampled: a histogram of another width is not
    // this build's histogram, and rebinning one would be the silent
    // fallback this project refuses everywhere else. A cache costs one
    // bounded decode to rebuild.
    if (!ReadInt64Field(root, "histogram_bins", bins) ||
        bins != kIsolatedFrameHistogramBins) {
        error = "isolated frame cache was written with another histogram";
        return false;
    }
    parsed.rate = {static_cast<int32_t>(num), static_cast<int32_t>(den)};
    parsed.analysis_width = static_cast<int32_t>(width);
    parsed.analysis_height = static_cast<int32_t>(height);
    const Value* ranges = root.Find("ranges");
    if (ranges == nullptr || !ranges->IsArray()) {
        error = "isolated frame cache has no ranges";
        return false;
    }
    int64_t previousEnd = std::numeric_limits<int64_t>::min();
    for (const Value& entry : ranges->AsArray()) {
        if (!entry.IsObject()) {
            error = "isolated frame cache range is not an object";
            return false;
        }
        int64_t firstFrame = 0;
        int64_t frames = 0;
        if (!ReadInt64Field(entry, "first_frame", firstFrame) ||
            firstFrame < 0 || !ReadInt64Field(entry, "frames", frames) ||
            frames <= 0) {
            error = "isolated frame cache range has invalid bounds";
            return false;
        }
        if (firstFrame < previousEnd) {
            error = "isolated frame cache ranges overlap or are unordered";
            return false;
        }
        previousEnd = firstFrame + frames;
        const Value* mean = entry.Find("mean");
        const Value* histogram = entry.Find("histogram");
        if (mean == nullptr || !mean->IsArray() || histogram == nullptr ||
            !histogram->IsArray() ||
            static_cast<int64_t>(mean->AsArray().size()) != frames ||
            static_cast<int64_t>(histogram->AsArray().size()) !=
                frames * kIsolatedFrameHistogramBins) {
            error = "isolated frame cache arrays are missing or mismatched";
            return false;
        }
        IsolatedFrameSeries series;
        series.first_frame = firstFrame;
        series.frames.reserve(static_cast<size_t>(frames));
        for (int64_t index = 0; index < frames; ++index) {
            IsolatedFrameMeasurement measurement;
            if (!mean->AsArray()[static_cast<size_t>(index)].AsInt64(
                    measurement.mean_luma) ||
                measurement.mean_luma < 0 ||
                measurement.mean_luma > 255 * kIsolatedFrameLumaScale) {
                error = "isolated frame cache contains an invalid mean";
                return false;
            }
            for (int32_t bin = 0; bin < kIsolatedFrameHistogramBins; ++bin) {
                int64_t share = 0;
                if (!histogram
                         ->AsArray()[static_cast<size_t>(
                             index * kIsolatedFrameHistogramBins + bin)]
                         .AsInt64(share) ||
                    share < 0 || share > kIsolatedFrameHistogramScale) {
                    error =
                        "isolated frame cache contains an invalid histogram";
                    return false;
                }
                measurement.histogram[static_cast<size_t>(bin)] =
                    static_cast<int32_t>(share);
            }
            series.frames.push_back(measurement);
        }
        parsed.series.push_back(std::move(series));
    }
    scan = std::move(parsed);
    return true;
}

}  // namespace

bool DeserializeIsolatedFrameScan(const std::string& json,
                                  IsolatedFrameScan& scan, std::string& error) {
    int64_t version = 0;
    return DeserializeVersionedIsolatedFrameScan(json, scan, version, error);
}

IsolatedFrameCacheRead ReadIsolatedFrameCache(const std::string& path,
                                              IsolatedFrameScan& scan) {
    IsolatedFrameCacheRead read;
    std::error_code existsError;
    if (!std::filesystem::exists(path, existsError)) {
        read.state = IsolatedFrameCacheState::Absent;
        read.detail = "unable to open isolated frame cache";
        return read;
    }
    read.state = IsolatedFrameCacheState::Unreadable;
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        read.detail = "unable to open isolated frame cache";
        return read;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    if (!input.good() && !input.eof()) {
        read.detail = "unable to read isolated frame cache";
        return read;
    }
    if (DeserializeVersionedIsolatedFrameScan(
            buffer.str(), scan, read.found_version, read.detail)) {
        read.state = IsolatedFrameCacheState::Loaded;
    }
    return read;
}

bool LoadIsolatedFrameScan(const std::string& path, IsolatedFrameScan& scan,
                           std::string& error) {
    const IsolatedFrameCacheRead read = ReadIsolatedFrameCache(path, scan);
    error = read.detail;
    return read.state == IsolatedFrameCacheState::Loaded;
}

std::vector<SourceFrameSpan> UncoveredSpans(
    const IsolatedFrameScanTarget& target, const IsolatedFrameScan& scan) {
    std::vector<SourceFrameSpan> missing;
    for (const SourceFrameSpan& span : target.spans) {
        int64_t cursor = span.first_frame;
        for (const IsolatedFrameSeries& series : scan.series) {
            const int64_t end =
                series.first_frame + static_cast<int64_t>(series.frames.size());
            if (end <= cursor) continue;
            if (series.first_frame >= span.end_frame) break;
            if (series.first_frame > cursor)
                missing.push_back(
                    {cursor, std::min(series.first_frame, span.end_frame)});
            cursor = std::max(cursor, end);
            if (cursor >= span.end_frame) break;
        }
        if (cursor < span.end_frame)
            missing.push_back({cursor, span.end_frame});
    }
    return missing;
}

std::string DescribeIsolatedFramesForAgent(
    const Document& document,
    const std::vector<IsolatedFrameScanTarget>& targets,
    const std::map<Ulid, IsolatedFrameScan>& scans,
    const std::map<Ulid, IsolatedFrameCacheRead>& reads,
    const IsolatedFrameSettings& settings) {
    Value root = Value::MakeObject();
    root.Set("luma_scale", Value::MakeInt(kIsolatedFrameLumaScale));
    root.Set("histogram_scale", Value::MakeInt(kIsolatedFrameHistogramScale));
    Value sources = Value::MakeArray();
    Value unscanned = Value::MakeArray();
    int64_t found = 0;
    for (const IsolatedFrameScanTarget& target : targets) {
        const LibraryMedia* media = document.FindLibraryMedia(target.media_id);
        const auto scan = scans.find(target.media_id);
        if (scan == scans.end()) {
            Value entry = Value::MakeObject();
            entry.Set("media_id", Value::MakeString(target.media_id));
            if (media != nullptr)
                entry.Set("filename", Value::MakeString(media->filename));
            const auto read = reads.find(target.media_id);
            entry.Set("reason", Value::MakeString(
                                    read == reads.end() ||
                                            read->second.state ==
                                                IsolatedFrameCacheState::Absent
                                        ? "never scanned"
                                        : read->second.detail));
            Value spans = Value::MakeArray();
            for (const SourceFrameSpan& span : target.spans)
                spans.Push(SpanValue(span));
            entry.Set("missing_ranges", std::move(spans));
            unscanned.Push(std::move(entry));
            continue;
        }
        const std::vector<SourceFrameSpan> missing =
            UncoveredSpans(target, scan->second);
        if (!missing.empty()) {
            Value entry = Value::MakeObject();
            entry.Set("media_id", Value::MakeString(target.media_id));
            if (media != nullptr)
                entry.Set("filename", Value::MakeString(media->filename));
            entry.Set("reason",
                      Value::MakeString(
                          "the cached scan does not cover every range this "
                          "timeline uses"));
            Value spans = Value::MakeArray();
            for (const SourceFrameSpan& span : missing)
                spans.Push(SpanValue(span));
            entry.Set("missing_ranges", std::move(spans));
            unscanned.Push(std::move(entry));
        }
        Value entry = Value::MakeObject();
        entry.Set("media_id", Value::MakeString(target.media_id));
        if (media != nullptr)
            entry.Set("filename", Value::MakeString(media->filename));
        entry.Set("frame_rate",
                  Value::MakeString(std::to_string(target.rate.num) + "/" +
                                    std::to_string(target.rate.den)));
        int64_t scanned = 0;
        Value isolated = Value::MakeArray();
        for (const IsolatedFrameSeries& series : scan->second.series) {
            scanned += static_cast<int64_t>(series.frames.size());
            for (const IsolatedFrameEvent& event :
                 DetectIsolatedFrames(series, settings)) {
                ++found;
                Value item = Value::MakeObject();
                item.Set("first_frame", Value::MakeInt(event.first_frame));
                item.Set("frames", Value::MakeInt(event.frames));
                item.Set("source_in",
                         TimeValue({event.first_frame * target.rate.den,
                                    target.rate.num}));
                item.Set("deviation", Value::MakeInt(event.deviation));
                item.Set("baseline", Value::MakeInt(event.baseline));
                item.Set("brighter", Value::MakeBool(event.deviation > 0));
                item.Set("histogram_distance",
                         Value::MakeInt(event.histogram_distance));
                item.Set("neighbour_step",
                         Value::MakeInt(event.neighbour_step));
                item.Set("level_trigger", Value::MakeBool(event.level_trigger));
                item.Set("picture_trigger",
                         Value::MakeBool(event.picture_trigger));
                // Where the montage plays that frame, so the answer is not a
                // frame number the caller has to convert. Empty means the
                // frame sits in a transition handle or in the margin beside
                // a used range -- which is exactly where a flash hides at
                // half opacity -- or, PRD38, between two frames a sped-up
                // clip shows.
                std::vector<SourceFrameMatch> matches;
                std::string ignored;
                Value clips = Value::MakeArray();
                if (ResolveSourceFrame(document, target.media_id,
                                       event.first_frame, matches, ignored)) {
                    for (const SourceFrameMatch& match : matches) {
                        Value where = Value::MakeObject();
                        where.Set("clip_id", Value::MakeString(match.clip_id));
                        where.Set("track_id",
                                  Value::MakeString(match.track_id));
                        where.Set("timeline_position",
                                  TimeValue(match.timeline_position));
                        clips.Push(std::move(where));
                    }
                } else {
                    // PRD38 -- refused because a clip of this media is not
                    // at 1/1: situated by ClipSpeed.h's rule instead, the
                    // whole run rather than its first frame.
                    for (const ShownSourceFrame& shown :
                         ClipsShowingSourceFrames(
                             document, target.media_id, target.rate,
                             event.first_frame, event.frames)) {
                        Value where = Value::MakeObject();
                        where.Set("clip_id", Value::MakeString(shown.clip_id));
                        where.Set("track_id",
                                  Value::MakeString(shown.track_id));
                        where.Set("timeline_position",
                                  TimeValue(shown.timeline_position));
                        clips.Push(std::move(where));
                    }
                }
                item.Set("clips", std::move(clips));
                isolated.Push(std::move(item));
            }
        }
        entry.Set("scanned_frames", Value::MakeInt(scanned));
        entry.Set("isolated_frames", std::move(isolated));
        sources.Push(std::move(entry));
    }
    root.Set("sources", std::move(sources));
    root.Set("unscanned", std::move(unscanned));
    root.Set("isolated_frame_count", Value::MakeInt(found));
    return root.Dump();
}
