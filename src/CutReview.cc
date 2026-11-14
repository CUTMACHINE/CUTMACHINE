#include "CutReview.h"

#include "ClipEnabled.h"
#include "Json.h"
#include "ShotQuality.h"
#include "Timeline.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

// Whole frames of `time` at `rate`, or false: a junction between two frames
// of the sequence has no frame to be reviewed at.
bool WholeFrames(const RationalTime& time, const MediaRate& rate,
                 int64_t& frames) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate.num;
    const __int128 denominator = static_cast<__int128>(time.rate) * rate.den;
    if (denominator == 0 || numerator % denominator != 0) return false;
    const __int128 value = numerator / denominator;
    if (value > std::numeric_limits<int64_t>::max() ||
        value < std::numeric_limits<int64_t>::min())
        return false;
    frames = static_cast<int64_t>(value);
    return true;
}

RationalTime FrameTime(int64_t frame, const MediaRate& rate) {
    return {frame * rate.den, rate.num};
}

// The sample nearest to the start of `frame`, the rounding the export's
// `atrim` uses, as CutInspection.cc reads the same window.
int64_t SampleAt(int64_t frame, const MediaRate& rate, int32_t sampleRate) {
    const __int128 numerator =
        static_cast<__int128>(frame) * rate.den * sampleRate;
    const __int128 denominator = rate.num;
    return static_cast<int64_t>((numerator + (denominator / 2)) / denominator);
}

int64_t Dbfs(double amplitude) {
    if (!(amplitude > 0.0)) return kCutInspectionSilenceDbfs;
    const double level = 20.0 * std::log10(amplitude);
    if (level <= static_cast<double>(kCutInspectionSilenceDbfs))
        return kCutInspectionSilenceDbfs;
    return static_cast<int64_t>(std::lround(level));
}

// RMS over the stereo frames [begin, end) of interleaved samples.
int64_t RmsDbfs(const std::vector<float>& samples, int64_t begin, int64_t end) {
    if (end <= begin) return kCutInspectionSilenceDbfs;
    double sum = 0.0;
    for (int64_t index = begin * 2; index < end * 2; ++index) {
        const double sample = samples[static_cast<size_t>(index)];
        sum += sample * sample;
    }
    return Dbfs(std::sqrt(sum / static_cast<double>((end - begin) * 2)));
}

bool UnitSpeed(const DocumentClip& clip) {
    return clip.speed.den != 0 && clip.speed.num == clip.speed.den &&
           clip.speed_ramp.empty();
}

// "3.2 %" from thousandths.
std::string Percent(int64_t perMille) {
    std::ostringstream text;
    text << (perMille / 10) << '.' << (std::llabs(perMille) % 10) << " %";
    return text.str();
}

std::string Level(int64_t dbfs) {
    if (dbfs <= kCutInspectionSilenceDbfs) return "digital silence";
    return std::to_string(dbfs) + " dBFS";
}

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

Value Optional(const std::optional<int64_t>& value) {
    return value ? Value::MakeInt(*value) : Value::MakeNull();
}

}  // namespace

bool ListTimelineJunctions(const Document& document,
                           std::vector<TimelineJunction>& junctions,
                           int64_t& offGrid, std::string& error) {
    junctions.clear();
    offGrid = 0;
    error.clear();
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "the timeline has no frame rate";
        return false;
    }
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::map<int64_t, TimelineJunction> byFrame;
    for (const DocumentTrack& track : document.sequence.tracks) {
        const bool video = track.kind == "video" && track.visible;
        const bool audio =
            track.kind == "audio" && !track.muted && (!hasSolo || track.solo);
        if (!video && !audio) continue;
        std::vector<const DocumentClip*> clips;
        // A disabled clip, neither seen nor heard, makes no cut of the film
        // (ClipEnabled.h).
        for (const DocumentClip& clip : track.clips)
            if (ClipIsHeard(clip)) clips.push_back(&clip);
        std::sort(clips.begin(), clips.end(),
                  [](const DocumentClip* left, const DocumentClip* right) {
                      return left->timeline_in < right->timeline_in;
                  });
        for (size_t index = 1; index < clips.size(); ++index) {
            const DocumentClip& outgoing = *clips[index - 1];
            const DocumentClip& incoming = *clips[index];
            const RationalTime edge =
                outgoing.timeline_in.add(outgoing.duration);
            if (edge != incoming.timeline_in) continue;
            int64_t frame = 0;
            if (!WholeFrames(edge, rate, frame)) {
                ++offGrid;
                continue;
            }
            TimelineJunctionCut cut;
            cut.track_id = track.id;
            cut.track_kind = track.kind;
            cut.outgoing_clip_id = outgoing.id;
            cut.incoming_clip_id = incoming.id;
            cut.outgoing_source_id = outgoing.composition_id.empty()
                                         ? outgoing.source_id
                                         : outgoing.composition_id;
            cut.incoming_source_id = incoming.composition_id.empty()
                                         ? incoming.source_id
                                         : incoming.composition_id;
            cut.continuous =
                cut.outgoing_source_id == cut.incoming_source_id &&
                UnitSpeed(outgoing) && UnitSpeed(incoming) &&
                outgoing.source_in.add(outgoing.duration) == incoming.source_in;
            TimelineJunction& junction = byFrame[frame];
            junction.frame = frame;
            junction.cuts.push_back(std::move(cut));
        }
    }
    for (auto& [frame, junction] : byFrame) {
        bool pictureContinuous = true;
        bool soundContinuous = true;
        for (const TimelineJunctionCut& cut : junction.cuts) {
            if (cut.track_kind == "video") {
                junction.picture = true;
                pictureContinuous = pictureContinuous && cut.continuous;
            } else {
                junction.sound = true;
                soundContinuous = soundContinuous && cut.continuous;
            }
        }
        junction.picture_continuous = junction.picture && pictureContinuous;
        junction.sound_continuous = junction.sound && soundContinuous;
        junctions.push_back(std::move(junction));
    }
    return true;
}

std::vector<int64_t> CutBandFrames(int64_t first, int64_t cut, int64_t end,
                                   int32_t perSide) {
    std::set<int64_t> frames;
    const auto spread = [&](int64_t from, int64_t to, bool towardsEnd) {
        // [from, to] inclusive; the frame at the cut's side always in.
        if (to < from || perSide <= 0) return;
        const int64_t span = to - from;
        const int64_t count = std::min<int64_t>(perSide, span + 1);
        for (int64_t index = 0; index < count; ++index) {
            const int64_t step = count == 1 ? 0 : (index * span) / (count - 1);
            frames.insert(towardsEnd ? to - step : from + step);
        }
    };
    spread(first, cut - 1, true);
    spread(cut, end - 1, false);
    return {frames.begin(), frames.end()};
}

bool PlanJunctionReview(const Document& document,
                        const TimelineJunction& junction,
                        const CutReviewRequest& request,
                        CutInspectionPlan& plan,
                        std::vector<int64_t>& bandFrames, std::string& error) {
    bandFrames.clear();
    CutInspectionRequest inspection;
    inspection.position =
        FrameTime(junction.frame, document.sequence.frame_rate);
    inspection.before = request.side;
    inspection.after = request.side;
    inspection.images = kCutInspectionMinImages;
    if (!PlanCutInspection(document, inspection, plan, error)) return false;
    bandFrames =
        CutBandFrames(plan.first, plan.cut, plan.end, request.images_per_side);
    std::set<int64_t> shown(bandFrames.begin(), bandFrames.end());
    for (int64_t frame = plan.cut - 2; frame <= plan.cut + 1; ++frame) {
        if (frame >= plan.first && frame < plan.end) shown.insert(frame);
    }
    plan.shown.assign(shown.begin(), shown.end());
    return true;
}

bool MeasureJunctionSound(const std::vector<float>& samples, int32_t sampleRate,
                          const CutInspectionPlan& plan,
                          JunctionSoundMeasures& measures, std::string& error) {
    measures = JunctionSoundMeasures{};
    error.clear();
    if (sampleRate <= 0) {
        error = "the window's sound has no sample rate";
        return false;
    }
    const int64_t origin = SampleAt(plan.first, plan.rate, sampleRate);
    const int64_t cut = SampleAt(plan.cut, plan.rate, sampleRate) - origin;
    const int64_t end = SampleAt(plan.end, plan.rate, sampleRate) - origin;
    if (static_cast<int64_t>(samples.size()) < end * 2) {
        error = "the window's sound holds " +
                std::to_string(samples.size() / 2) + " samples, " +
                std::to_string(end) + " expected";
        return false;
    }
    const int64_t window =
        (static_cast<int64_t>(sampleRate) * kCutReviewLevelWindowMilliseconds) /
        1000;
    const int64_t minimum =
        (static_cast<int64_t>(sampleRate) * kCutReviewMinimumSideMilliseconds) /
        1000;
    const int64_t edge =
        (static_cast<int64_t>(sampleRate) * kCutReviewEdgeMilliseconds) / 1000;
    // Windows counted outward from the cut, so both sides read the same
    // grid whatever the window's length.
    const auto side = [&](bool before, std::optional<int64_t>& ambience,
                          std::optional<int64_t>& voice) {
        const int64_t length = before ? cut : end - cut;
        if (length < minimum || window <= 0) return;
        std::vector<int64_t> levels;
        for (int64_t offset = 0; offset + window <= length; offset += window) {
            const int64_t begin = before ? cut - offset - window : cut + offset;
            levels.push_back(RmsDbfs(samples, begin, begin + window));
        }
        if (levels.empty()) return;
        ambience = ShotQualityPercentile(levels, kCutReviewAmbiencePercentile);
        voice = ShotQualityPercentile(levels, kCutReviewVoicePercentile);
    };
    side(true, measures.ambience_before_dbfs, measures.voice_before_dbfs);
    side(false, measures.ambience_after_dbfs, measures.voice_after_dbfs);
    if (cut > 0)
        measures.edge_before_dbfs =
            RmsDbfs(samples, std::max<int64_t>(0, cut - edge), cut);
    if (cut < end)
        measures.edge_after_dbfs =
            RmsDbfs(samples, cut, std::min(end, cut + edge));
    measures.measured = true;
    return true;
}

bool MeasureJunctionPicture(const std::vector<std::vector<uint8_t>>& planes,
                            int32_t width, int32_t height,
                            const CutInspectionPlan& plan,
                            JunctionPictureMeasures& measures,
                            std::string& error) {
    measures = JunctionPictureMeasures{};
    error.clear();
    const size_t size =
        static_cast<size_t>(std::max(width, 0)) * std::max(height, 0);
    if (planes.size() != plan.shown.size() || size == 0) {
        error = "the measured planes do not match the frames of the window";
        return false;
    }
    for (const std::vector<uint8_t>& plane : planes) {
        if (plane.size() != size) {
            error = "a measured plane is not " + std::to_string(width) + "x" +
                    std::to_string(height);
            return false;
        }
    }
    const auto plane = [&](int64_t frame) -> const uint8_t* {
        const auto found =
            std::find(plan.shown.begin(), plan.shown.end(), frame);
        if (found == plan.shown.end()) return nullptr;
        return planes[static_cast<size_t>(found - plan.shown.begin())].data();
    };
    // Thousandths of full scale, from ShotQuality's millionths.
    const auto perMille = [](int64_t metric) {
        return metric / (kShotQualityMetricScale / 1000);
    };
    const uint8_t* beforeBefore = plane(plan.cut - 2);
    const uint8_t* before = plane(plan.cut - 1);
    const uint8_t* at = plane(plan.cut);
    const uint8_t* afterAt = plane(plan.cut + 1);
    if (before != nullptr && at != nullptr) {
        measures.difference_per_mille =
            perMille(FrameMotionMetric(before, at, width, height));
        measures.histogram_per_mille =
            perMille(FrameHistogramDistanceMetric(before, at, width, height));
    }
    if (beforeBefore != nullptr && before != nullptr)
        measures.motion_before_per_mille =
            perMille(FrameMotionMetric(beforeBefore, before, width, height));
    if (at != nullptr && afterAt != nullptr)
        measures.motion_after_per_mille =
            perMille(FrameMotionMetric(at, afterAt, width, height));
    measures.measured = true;
    return true;
}

void JudgeJunction(const CutReviewThresholds& thresholds,
                   JunctionReview& review) {
    review.alerts.clear();
    const TimelineJunction& junction = review.junction;
    const JunctionSoundMeasures& sound = review.sound;
    if (junction.sound && !junction.sound_continuous && sound.measured) {
        if (sound.ambience_before_dbfs && sound.ambience_after_dbfs) {
            const int64_t jump = std::llabs(*sound.ambience_after_dbfs -
                                            *sound.ambience_before_dbfs);
            if (jump >= thresholds.ambience_jump_db) {
                review.alerts.push_back(
                    {"ambience_jump",
                     "the ambience jumps by " + std::to_string(jump) +
                         " dB across the cut (" +
                         Level(*sound.ambience_before_dbfs) + " before, " +
                         Level(*sound.ambience_after_dbfs) + " after)"});
            }
        }
        const auto voiced = [&](const std::optional<int64_t>& voice,
                                const std::optional<int64_t>& ambience) {
            return voice && ambience &&
                   *voice - *ambience >= thresholds.voice_over_ambience_db;
        };
        if (voiced(sound.voice_before_dbfs, sound.ambience_before_dbfs) &&
            voiced(sound.voice_after_dbfs, sound.ambience_after_dbfs)) {
            const int64_t jump =
                std::llabs(*sound.voice_after_dbfs - *sound.voice_before_dbfs);
            if (jump >= thresholds.voice_jump_db) {
                review.alerts.push_back(
                    {"voice_jump",
                     "the voice jumps by " + std::to_string(jump) +
                         " dB across the cut (" +
                         Level(*sound.voice_before_dbfs) + " before, " +
                         Level(*sound.voice_after_dbfs) + " after)"});
            }
        }
        const auto live = [&](const std::optional<int64_t>& edge,
                              const std::optional<int64_t>& ambience) {
            return edge && ambience && *edge >= thresholds.edge_minimum_dbfs &&
                   *edge - *ambience >= thresholds.edge_over_ambience_db;
        };
        const bool liveBefore =
            live(sound.edge_before_dbfs, sound.ambience_before_dbfs);
        const bool liveAfter =
            live(sound.edge_after_dbfs, sound.ambience_after_dbfs);
        if (liveBefore || liveAfter) {
            std::string finding = "the cut falls inside a sound:";
            if (liveBefore) {
                finding += " the last " +
                           std::to_string(kCutReviewEdgeMilliseconds) +
                           " ms before it stand " +
                           std::to_string(*sound.edge_before_dbfs -
                                          *sound.ambience_before_dbfs) +
                           " dB over the ambience";
            }
            if (liveAfter) {
                finding += std::string(liveBefore ? ";" : "") + " the first " +
                           std::to_string(kCutReviewEdgeMilliseconds) +
                           " ms after it stand " +
                           std::to_string(*sound.edge_after_dbfs -
                                          *sound.ambience_after_dbfs) +
                           " dB over the ambience";
            }
            finding += " (a word, a breath or a note cut short?)";
            review.alerts.push_back({"sound_cut_live", finding});
        }
    }
    for (const CutInspectionWord& word : review.words_across) {
        review.alerts.push_back(
            {"word_crosses_cut",
             "the cut runs through the word \"" + word.text + "\""});
    }
    const JunctionPictureMeasures& picture = review.picture;
    if (junction.picture && !junction.picture_continuous && picture.measured) {
        if (picture.difference_per_mille && picture.histogram_per_mille &&
            *picture.difference_per_mille <=
                thresholds.jump_cut_difference_per_mille &&
            *picture.histogram_per_mille <=
                thresholds.jump_cut_histogram_per_mille) {
            bool sameSource = false;
            for (const TimelineJunctionCut& cut : junction.cuts) {
                if (cut.track_kind == "video" && !cut.continuous &&
                    cut.outgoing_source_id == cut.incoming_source_id)
                    sameSource = true;
            }
            review.alerts.push_back(
                {"jump_cut", "the frames on either side of the cut differ by " +
                                 Percent(*picture.difference_per_mille) +
                                 " (luma histogram " +
                                 Percent(*picture.histogram_per_mille) +
                                 "): one framing on both sides, a jump cut" +
                                 (sameSource ? ", from the same rush" : "")});
        }
        if (picture.motion_before_per_mille && picture.motion_after_per_mille &&
            picture.histogram_per_mille &&
            *picture.motion_before_per_mille >= thresholds.moving_per_mille &&
            *picture.motion_after_per_mille <= thresholds.still_per_mille &&
            *picture.histogram_per_mille <=
                thresholds.same_scene_histogram_per_mille) {
            review.alerts.push_back(
                {"motion_interrupted",
                 "a movement stops dead at the cut: the two frames before it "
                 "differ by " +
                     Percent(*picture.motion_before_per_mille) +
                     ", the two after it by " +
                     Percent(*picture.motion_after_per_mille)});
        }
    }
}

bool ReviewCuts(const Document& document, const CutReviewRequest& request,
                const CutInspectionRenderer& render, CutReviewReport& report,
                std::vector<std::string>& bandJpegs, std::string& error) {
    report = CutReviewReport{};
    bandJpegs.clear();
    error.clear();
    if (request.images_per_side < 1 ||
        request.images_per_side > kCutReviewMaxImagesPerSide) {
        error = "images_per_side must be between 1 and " +
                std::to_string(kCutReviewMaxImagesPerSide);
        return false;
    }
    if (request.max_bands < 0 || request.max_bands > kCutReviewMaxBands) {
        error = "max_bands must be between 0 and " +
                std::to_string(kCutReviewMaxBands);
        return false;
    }
    if (!render) {
        error = "nothing can render the junctions' windows here";
        return false;
    }
    report.timeline_id = document.sequence.id;
    report.rate = document.sequence.frame_rate;
    report.thresholds = request.thresholds;
    std::vector<TimelineJunction> all;
    if (!ListTimelineJunctions(document, all, report.off_grid, error))
        return false;
    report.junctions_total = static_cast<int64_t>(all.size());
    std::vector<TimelineJunction> selected;
    if (request.frames.empty()) {
        for (const TimelineJunction& junction : all) {
            if (static_cast<int32_t>(selected.size()) < kCutReviewMaxJunctions)
                selected.push_back(junction);
            else
                report.not_reviewed.push_back(junction.frame);
        }
    } else {
        std::set<int64_t> asked(request.frames.begin(), request.frames.end());
        if (static_cast<int32_t>(asked.size()) > kCutReviewMaxJunctions) {
            error = "at most " + std::to_string(kCutReviewMaxJunctions) +
                    " junctions per review";
            return false;
        }
        for (const int64_t frame : asked) {
            const auto found = std::find_if(
                all.begin(), all.end(), [&](const TimelineJunction& junction) {
                    return junction.frame == frame;
                });
            if (found == all.end()) {
                error =
                    "no clip of a visible video track or an audible audio "
                    "track meets the next one at frame " +
                    std::to_string(frame) + ": there is no junction there";
                return false;
            }
            selected.push_back(*found);
        }
    }

    const MediaRate rate = document.sequence.frame_rate;
    std::vector<std::string> jpegs;
    for (const TimelineJunction& junction : selected) {
        JunctionReview review;
        review.junction = junction;
        std::vector<int64_t> bandFrames;
        const std::string where =
            "junction at frame " + std::to_string(junction.frame) + ": ";
        if (!PlanJunctionReview(document, junction, request, review.plan,
                                bandFrames, error)) {
            error = where + error;
            return false;
        }
        CutInspectionRenderSettings settings;
        settings.cell_dimension = kCutReviewCellDimension;
        settings.cut_band = true;
        settings.strip_frames = bandFrames;
        settings.band_width =
            CutBandWidth(document.sequence.width, document.sequence.height,
                         kCutReviewCellDimension, request.images_per_side,
                         request.images_per_side);
        settings.analysis_dimension = kCutReviewAnalysisDimension;
        settings.keep_samples = true;
        CutInspectionResult result;
        if (!render(review.plan, settings, result, error)) {
            error = "the window of the " + where + error;
            return false;
        }
        if (!MeasureJunctionSound(result.samples, result.audio.sample_rate,
                                  review.plan, review.sound, error) ||
            !MeasureJunctionPicture(
                result.analysis_planes, result.analysis_width,
                result.analysis_height, review.plan, review.picture, error)) {
            error = where + error;
            return false;
        }
        const RationalTime cutTime = FrameTime(junction.frame, rate);
        for (const CutInspectionWord& word : result.words.expected) {
            if (word.crosses_cut && word.timeline_in < cutTime &&
                cutTime < word.timeline_in.add(word.duration))
                review.words_across.push_back(word);
        }
        review.words_unavailable = result.words.expected_unavailable;
        JudgeJunction(request.thresholds, review);
        report.reviews.push_back(std::move(review));
        jpegs.push_back(std::move(result.jpeg));
    }

    // The bands: the junctions with alerts first, then the others, each
    // group in timeline order; shown in timeline order.
    std::vector<size_t> chosen;
    for (int pass = 0; pass < 2; ++pass) {
        for (size_t index = 0; index < report.reviews.size(); ++index) {
            if (static_cast<int32_t>(chosen.size()) >= request.max_bands) break;
            const bool flagged = !report.reviews[index].alerts.empty();
            if (flagged == (pass == 0)) chosen.push_back(index);
        }
    }
    std::sort(chosen.begin(), chosen.end());
    for (const size_t index : chosen) {
        report.reviews[index].band = static_cast<int32_t>(report.bands.size());
        report.bands.push_back(report.reviews[index].junction.frame);
        bandJpegs.push_back(std::move(jpegs[index]));
    }
    return true;
}

std::string SerializeCutReview(const CutReviewReport& report) {
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(report.timeline_id));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(report.rate.num));
    rate.Set("den", Value::MakeInt(report.rate.den));
    root.Set("rate", std::move(rate));
    root.Set("note", Value::MakeString(
                         "Alerts are measures crossing a declared threshold, "
                         "to examine: none says the cut is wrong, and no "
                         "alert does not say it is right."));
    const CutReviewThresholds& t = report.thresholds;
    Value thresholds = Value::MakeObject();
    thresholds.Set("ambience_jump_db", Value::MakeInt(t.ambience_jump_db));
    thresholds.Set("voice_jump_db", Value::MakeInt(t.voice_jump_db));
    thresholds.Set("voice_over_ambience_db",
                   Value::MakeInt(t.voice_over_ambience_db));
    thresholds.Set("edge_over_ambience_db",
                   Value::MakeInt(t.edge_over_ambience_db));
    thresholds.Set("edge_minimum_dbfs", Value::MakeInt(t.edge_minimum_dbfs));
    thresholds.Set("jump_cut_difference_per_mille",
                   Value::MakeInt(t.jump_cut_difference_per_mille));
    thresholds.Set("jump_cut_histogram_per_mille",
                   Value::MakeInt(t.jump_cut_histogram_per_mille));
    thresholds.Set("moving_per_mille", Value::MakeInt(t.moving_per_mille));
    thresholds.Set("still_per_mille", Value::MakeInt(t.still_per_mille));
    thresholds.Set("same_scene_histogram_per_mille",
                   Value::MakeInt(t.same_scene_histogram_per_mille));
    root.Set("thresholds", std::move(thresholds));
    root.Set("junctions_total", Value::MakeInt(report.junctions_total));
    root.Set("reviewed",
             Value::MakeInt(static_cast<int64_t>(report.reviews.size())));
    std::map<std::string, int64_t> counts;
    int64_t flagged = 0;
    Value junctions = Value::MakeArray();
    for (const JunctionReview& review : report.reviews) {
        const TimelineJunction& junction = review.junction;
        Value item = Value::MakeObject();
        item.Set("frame", Value::MakeInt(junction.frame));
        item.Set("position", TimeValue(FrameTime(junction.frame, report.rate)));
        Value cuts = Value::MakeArray();
        for (const TimelineJunctionCut& cut : junction.cuts) {
            Value entry = Value::MakeObject();
            entry.Set("track_id", Value::MakeString(cut.track_id));
            entry.Set("kind", Value::MakeString(cut.track_kind));
            entry.Set("outgoing_clip_id",
                      Value::MakeString(cut.outgoing_clip_id));
            entry.Set("incoming_clip_id",
                      Value::MakeString(cut.incoming_clip_id));
            entry.Set("same_source", Value::MakeBool(cut.outgoing_source_id ==
                                                     cut.incoming_source_id));
            entry.Set("continuous", Value::MakeBool(cut.continuous));
            cuts.Push(std::move(entry));
        }
        item.Set("cuts", std::move(cuts));
        Value window = Value::MakeObject();
        window.Set("first_frame", Value::MakeInt(review.plan.first));
        window.Set("end_frame", Value::MakeInt(review.plan.end));
        item.Set("window", std::move(window));
        Value sound = Value::MakeObject();
        sound.Set("ambience_before_dbfs",
                  Optional(review.sound.ambience_before_dbfs));
        sound.Set("ambience_after_dbfs",
                  Optional(review.sound.ambience_after_dbfs));
        sound.Set("voice_before_dbfs",
                  Optional(review.sound.voice_before_dbfs));
        sound.Set("voice_after_dbfs", Optional(review.sound.voice_after_dbfs));
        sound.Set("edge_before_dbfs", Optional(review.sound.edge_before_dbfs));
        sound.Set("edge_after_dbfs", Optional(review.sound.edge_after_dbfs));
        item.Set("sound", std::move(sound));
        Value picture = Value::MakeObject();
        picture.Set("difference_per_mille",
                    Optional(review.picture.difference_per_mille));
        picture.Set("histogram_per_mille",
                    Optional(review.picture.histogram_per_mille));
        picture.Set("motion_before_per_mille",
                    Optional(review.picture.motion_before_per_mille));
        picture.Set("motion_after_per_mille",
                    Optional(review.picture.motion_after_per_mille));
        item.Set("picture", std::move(picture));
        Value words = Value::MakeArray();
        for (const CutInspectionWord& word : review.words_across) {
            Value entry = Value::MakeObject();
            entry.Set("text", Value::MakeString(word.text));
            entry.Set("timeline_in", TimeValue(word.timeline_in));
            entry.Set("duration", TimeValue(word.duration));
            words.Push(std::move(entry));
        }
        item.Set("words_across", std::move(words));
        if (!review.words_unavailable.empty())
            item.Set("words_unavailable",
                     Value::MakeString(review.words_unavailable));
        Value alerts = Value::MakeArray();
        for (const CutReviewAlert& alert : review.alerts) {
            Value entry = Value::MakeObject();
            entry.Set("kind", Value::MakeString(alert.kind));
            entry.Set("finding", Value::MakeString(alert.finding));
            alerts.Push(std::move(entry));
            ++counts[alert.kind];
        }
        if (!review.alerts.empty()) ++flagged;
        item.Set("alerts", std::move(alerts));
        item.Set("band", review.band ? Value::MakeInt(*review.band)
                                     : Value::MakeNull());
        junctions.Push(std::move(item));
    }
    root.Set("with_alerts", Value::MakeInt(flagged));
    Value byKind = Value::MakeObject();
    for (const auto& [kind, count] : counts)
        byKind.Set(kind, Value::MakeInt(count));
    root.Set("alerts_by_kind", std::move(byKind));
    root.Set("junctions", std::move(junctions));
    Value bands = Value::MakeArray();
    for (const int64_t frame : report.bands) bands.Push(Value::MakeInt(frame));
    root.Set("bands", std::move(bands));
    root.Set("bands_layout",
             Value::MakeString(
                 "one band per junction, top to bottom: the frames before "
                 "the cut, a red bar, the frames from the cut; under them the "
                 "window's mix as a waveform, the cut in red, a grey tick "
                 "where each picture sits"));
    Value notReviewed = Value::MakeArray();
    for (const int64_t frame : report.not_reviewed)
        notReviewed.Push(Value::MakeInt(frame));
    root.Set("not_reviewed", std::move(notReviewed));
    root.Set("off_grid", Value::MakeInt(report.off_grid));
    return root.Dump();
}
