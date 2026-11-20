#include "TimelineCheck.h"

#include "ClipAnimation.h"
#include "ClipEnabled.h"
#include "CutReview.h"
#include "FfmpegPipe.h"
#include "Generation.h"
#include "MotionComposition.h"
#include "Timeline.h"
#include "TimelineAudioReport.h"
#include "TimelineStats.h"
#include "TransitionRender.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <deque>
#include <exception>
#include <filesystem>
#include <limits>
#include <map>
#include <regex>
#include <set>
#include <utility>

namespace {

using mcp_json::Value;

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

Value FractionValue(int32_t num, int32_t den) {
    Value value = Value::MakeObject();
    value.Set("num", Value::MakeInt(num));
    value.Set("den", Value::MakeInt(den));
    return value;
}

bool ReadPolicyTime(const Value& field, const std::string& key,
                    RationalTime& out, std::string& error) {
    const std::string path = "policy." + key;
    if (!field.IsObject()) {
        error = "'" + path + "' must be an object with 'value' and 'rate'";
        return false;
    }
    for (const auto& entry : field.AsObject()) {
        if (entry.first != "value" && entry.first != "rate") {
            error = "'" + path + "' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    const Value* value = field.Find("value");
    const Value* rate = field.Find("rate");
    int64_t valueNumber = 0;
    int64_t rateNumber = 0;
    if (value == nullptr || rate == nullptr || !value->IsNumber() ||
        !rate->IsNumber() || !value->AsInt64(valueNumber) ||
        !rate->AsInt64(rateNumber)) {
        error = "'" + path + "' requires exact integers 'value' and 'rate'";
        return false;
    }
    if (rateNumber <= 0 || rateNumber > std::numeric_limits<int32_t>::max()) {
        error = "'" + path + ".rate' must be a positive 32-bit integer";
        return false;
    }
    if (valueNumber < 0) {
        error = "'" + path + "' must not be negative";
        return false;
    }
    out = RationalTime{valueNumber, static_cast<int32_t>(rateNumber)};
    return true;
}

const char* StatusName(TimelineCheckStatus status) {
    switch (status) {
        case TimelineCheckStatus::Passed:
            return "passed";
        case TimelineCheckStatus::Failed:
            return "failed";
        case TimelineCheckStatus::NotRun:
            return "not_run";
    }
    return "not_run";
}

const char* EndingName(EndingFrame frame) {
    switch (frame) {
        case EndingFrame::Picture:
            return "picture";
        case EndingFrame::Black:
            return "black";
        case EndingFrame::Fading:
            return "fading";
    }
    return "picture";
}

// One frame at the end of the film and what made it so: the dip that
// covers it, or the clip whose opacity lets the black through. Both empty
// for a frame with nothing on it.
struct EndingState {
    EndingFrame kind = EndingFrame::Black;
    Ulid transition_id;
    Ulid clip_id;
    // A picture drawn by a motion clip: the document does not see inside
    // its composition, so only the file can say whether it is black.
    bool motion = false;
};

// The visible video tracks, the one on top first.
std::vector<const DocumentTrack*> VisibleVideoTracksTopFirst(
    const Document& document) {
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "video" && track.visible) {
            tracks.push_back(&track);
        }
    }
    // The export composites in ascending index: the highest is on top.
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* left, const DocumentTrack* right) {
                         return left->index > right->index;
                     });
    return tracks;
}

// A clip's opacity at a frame of the sequence, its keys sampled.
bool ClipOpacityAtFrame(const DocumentClip& clip, int64_t frame,
                        const MediaRate& rate, EffectParamValue& opacity,
                        std::string& error) {
    opacity = clip.opacity;
    if (!ClipAnimates(clip, ClipAnimatedParameter::Opacity)) {
        return true;
    }
    const int64_t local =
        frame - clip.timeline_in.to_frames(rate.num, rate.den);
    ClipSampledParameters sampled;
    if (!SampleClipParameters(clip, local, rate, sampled, error)) {
        return false;
    }
    opacity = sampled.opacity;
    return true;
}

bool ClassifyEnding(const Document& document, int64_t first, int64_t count,
                    std::vector<EndingState>& states, std::string& error) {
    states.clear();
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "invalid sequence frame rate";
        return false;
    }
    const std::vector<const DocumentTrack*> tracks =
        VisibleVideoTracksTopFirst(document);
    try {
        const Timeline timeline(document);
        for (int64_t frame = first; frame < first + count; ++frame) {
            const RationalTime position{frame * rate.den, rate.num};
            EndingState state;
            // A clip partly transparent over the tracks below it: the
            // frame is a picture if one of them shows one, a fade to the
            // composite's black otherwise. A clip at zero lets them through
            // entirely, and is the cause when nothing is under it.
            const DocumentClip* fadingOver = nullptr;
            const DocumentClip* transparent = nullptr;
            for (const DocumentTrack* track : tracks) {
                const std::vector<ResolvedLayer> layers =
                    timeline.ResolveTrackLayers(track->id, position);
                if (layers.empty()) {
                    continue;
                }
                const std::optional<ResolvedColorVeil> veil =
                    timeline.ResolveTrackColorVeil(track->id, position);
                if (veil && veil->alpha.num != 0) {
                    // A dip through another colour ends on that colour,
                    // not on black.
                    if (!(veil->color == TransitionColor{0, 0, 0})) {
                        state.kind = EndingFrame::Picture;
                    } else {
                        state.kind = veil->alpha.num == veil->alpha.den
                                         ? EndingFrame::Black
                                         : EndingFrame::Fading;
                        state.transition_id = veil->transition_id;
                    }
                    break;
                }
                // A dissolve shows two pictures.
                if (layers.size() > 1) {
                    state.kind = EndingFrame::Picture;
                    break;
                }
                const DocumentClip* clip =
                    document.FindClip(layers.front().frame.clip_id);
                EffectParamValue opacity{1, 1};
                if (clip != nullptr &&
                    !ClipOpacityAtFrame(*clip, frame, rate, opacity, error)) {
                    return false;
                }
                if (opacity.den > 0 && opacity.num <= 0) {
                    if (transparent == nullptr) {
                        transparent = clip;
                    }
                    continue;
                }
                if (opacity.den > 0 && opacity.num < opacity.den) {
                    if (fadingOver == nullptr) {
                        fadingOver = clip;
                    }
                    continue;
                }
                state.kind = EndingFrame::Picture;
                state.motion = clip != nullptr && IsMotionClip(*clip);
                break;
            }
            if (fadingOver != nullptr && state.kind != EndingFrame::Picture) {
                state.kind = EndingFrame::Fading;
                state.transition_id.clear();
                state.clip_id = fadingOver->id;
            } else if (transparent != nullptr &&
                       state.kind == EndingFrame::Black &&
                       state.transition_id.empty()) {
                state.clip_id = transparent->id;
            }
            states.push_back(std::move(state));
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    return true;
}

// The film's length in whole sequence frames, as the export and
// verify_export count it.
bool FilmFrames(const Document& document, int64_t& frames, std::string& error) {
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "invalid sequence frame rate";
        return false;
    }
    try {
        const RationalTime duration = Timeline(document).Duration();
        const __int128 numerator =
            static_cast<__int128>(duration.value) * rate.num;
        const __int128 denominator =
            static_cast<__int128>(duration.rate) * rate.den;
        if (numerator % denominator != 0) {
            error = "timeline duration must contain whole sequence frames";
            return false;
        }
        frames = static_cast<int64_t>(numerator / denominator);
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    return true;
}

bool PolicyChangesDefault(const TimelinePolicy& policy) {
    const TimelinePolicy standard;
    return policy.duration_min || policy.duration_max ||
           policy.final_black != standard.final_black ||
           policy.jump_cuts != standard.jump_cuts || policy.final_window ||
           !policy.kit_name.empty() || policy.loudness_target_lufs ||
           policy.true_peak_max_dbtp != standard.true_peak_max_dbtp ||
           !policy.aspect_ratio.empty() ||
           static_cast<int64_t>(policy.minimum_contrast_num) *
                   standard.minimum_contrast_den !=
               static_cast<int64_t>(standard.minimum_contrast_num) *
                   policy.minimum_contrast_den;
}

TimelineCheckResult LinksCheck(const Document& document) {
    TimelineCheckResult result;
    result.name = "links";
    // Off screen, an angle or a disabled clip plays no picture; a disabled
    // sound plays nothing (ClipEnabled.h).
    const std::set<Ulid> hidden = OffScreenClipIds(document.sequence);
    bool solo = false;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind == "audio" && track.solo) {
            solo = true;
        }
    }
    int64_t pairs = 0;
    Value unlinked = Value::MakeArray();
    for (const DocumentTrack& videoTrack : document.sequence.tracks) {
        if (videoTrack.kind != "video" || !videoTrack.visible) {
            continue;
        }
        for (const DocumentClip& video : videoTrack.clips) {
            if (IsMotionClip(video) || video.source_id.empty() ||
                hidden.count(video.id) != 0) {
                continue;
            }
            const RationalTime videoEnd = video.timeline_in.add(video.duration);
            for (const DocumentTrack& audioTrack : document.sequence.tracks) {
                if (audioTrack.kind != "audio" || audioTrack.muted ||
                    (solo && !audioTrack.solo)) {
                    continue;
                }
                for (const DocumentClip& audio : audioTrack.clips) {
                    if (audio.source_id != video.source_id ||
                        !ClipIsHeard(audio)) {
                        continue;
                    }
                    const RationalTime audioEnd =
                        audio.timeline_in.add(audio.duration);
                    if (!(audio.timeline_in < videoEnd &&
                          video.timeline_in < audioEnd)) {
                        continue;
                    }
                    ++pairs;
                    if (!video.link_group_id.empty() &&
                        video.link_group_id == audio.link_group_id) {
                        continue;
                    }
                    Value pair = Value::MakeObject();
                    pair.Set("video_clip_id", Value::MakeString(video.id));
                    pair.Set("audio_clip_id", Value::MakeString(audio.id));
                    // TimelineStats' drift: zero when the two read their
                    // source at one offset, linked or not.
                    pair.Set("drift",
                             TimeValue(video.source_in.sub(video.timeline_in)
                                           .sub(audio.source_in.sub(
                                               audio.timeline_in))));
                    unlinked.Push(std::move(pair));
                }
            }
        }
    }
    result.evidence.Set("pairs", Value::MakeInt(pairs));
    const int64_t count = static_cast<int64_t>(unlinked.AsArray().size());
    result.evidence.Set("unlinked", std::move(unlinked));
    if (count > 0) {
        result.status = TimelineCheckStatus::Failed;
        result.reason =
            std::to_string(count) +
            " picture and sound pair(s) of one source play together "
            "without being linked: set_clip_link links them";
    }
    return result;
}

TimelineCheckResult SyncCheck(const TimelineStats& stats) {
    TimelineCheckResult result;
    result.name = "sync";
    result.evidence.Set("linked_pairs", Value::MakeInt(stats.linked_pairs));
    Value drifts = Value::MakeArray();
    for (const TimelineStats::SyncDrift& drift : stats.out_of_sync) {
        Value entry = Value::MakeObject();
        entry.Set("video_clip_id", Value::MakeString(drift.video_clip_id));
        entry.Set("audio_clip_id", Value::MakeString(drift.audio_clip_id));
        entry.Set("drift", TimeValue(drift.drift));
        drifts.Push(std::move(entry));
    }
    result.evidence.Set("out_of_sync", std::move(drifts));
    if (!stats.out_of_sync.empty()) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = std::to_string(stats.out_of_sync.size()) +
                        " linked pair(s) read their source at different "
                        "offsets";
    }
    return result;
}

TimelineCheckResult DurationCheck(const TimelinePolicy& policy,
                                  const RationalTime& duration) {
    TimelineCheckResult result;
    result.name = "duration";
    result.evidence.Set("duration", TimeValue(duration));
    if (policy.duration_min) {
        result.evidence.Set("minimum", TimeValue(*policy.duration_min));
    }
    if (policy.duration_max) {
        result.evidence.Set("maximum", TimeValue(*policy.duration_max));
    }
    if (!policy.duration_min && !policy.duration_max) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = "the policy sets no duration_min or duration_max";
    } else if (policy.duration_min && duration < *policy.duration_min) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = "the timeline is shorter than duration_min";
    } else if (policy.duration_max && duration > *policy.duration_max) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = "the timeline is longer than duration_max";
    }
    return result;
}

// A level to a tenth, the meter's resolution: "-14.2".
Value LevelValue(double level) {
    char text[32];
    const long tenths = std::lround(level * 10.0);
    std::snprintf(text, sizeof(text), "%s%ld.%ld", tenths < 0 ? "-" : "",
                  std::labs(tenths) / 10, std::labs(tenths) % 10);
    return Value::MakeRawNumber(text);
}

// PA2 -- the delivered file's loudness against the policy's target.
TimelineCheckResult LoudnessCheck(const TimelinePolicy& policy,
                                  const TimelineFileMeasures* file) {
    TimelineCheckResult result;
    result.name = "loudness";
    result.evidence.Set("target_lufs",
                        Value::MakeInt(*policy.loudness_target_lufs));
    result.evidence.Set("tolerance_lu", LevelValue(kLoudnessToleranceLu));
    result.evidence.Set("true_peak_max_dbtp",
                        Value::MakeInt(policy.true_peak_max_dbtp));
    if (file == nullptr) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = "no delivered file was measured";
        return result;
    }
    if (!file->loudness_measured) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = file->loudness_error.empty()
                            ? "the file's loudness was not measured"
                            : file->loudness_error;
        return result;
    }
    if (file->integrated_lufs)
        result.evidence.Set("integrated_lufs",
                            LevelValue(*file->integrated_lufs));
    if (file->true_peak_dbfs)
        result.evidence.Set("true_peak_dbtp",
                            LevelValue(*file->true_peak_dbfs));
    if (!file->integrated_lufs) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = "the delivered file is silent";
    } else if (std::fabs(*file->integrated_lufs -
                         *policy.loudness_target_lufs) >
               kLoudnessToleranceLu + 1e-9) {
        result.status = TimelineCheckStatus::Failed;
        result.reason =
            *file->integrated_lufs < *policy.loudness_target_lufs
                ? "the delivered file is quieter than loudness_target"
                : "the delivered file is louder than loudness_target";
    } else if (file->true_peak_dbfs &&
               *file->true_peak_dbfs > policy.true_peak_max_dbtp + 1e-9) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = "the delivered file peaks above true_peak_max";
    }
    return result;
}

// PA11 -- the sequence's shape against the policy's.
TimelineCheckResult FormatCheck(const Document& document,
                                const TimelinePolicy& policy) {
    TimelineCheckResult result;
    result.name = "format";
    result.evidence.Set("aspect_ratio", Value::MakeString(policy.aspect_ratio));
    result.evidence.Set("width", Value::MakeInt(document.sequence.width));
    result.evidence.Set("height", Value::MakeInt(document.sequence.height));
    int64_t width = 0;
    int64_t height = 0;
    std::string ignored;
    // ParseTimelinePolicy has already refused a ratio that does not parse.
    if (!generation::ParseAspectRatio(policy.aspect_ratio, width, height,
                                      ignored) ||
        document.sequence.width * height != document.sequence.height * width) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = "the sequence is not " + policy.aspect_ratio;
    }
    return result;
}

// Consecutive frames of one kind and one cause, as [from, until).
Value EndingRuns(const std::vector<EndingState>& states, int64_t first) {
    Value runs = Value::MakeArray();
    size_t index = 0;
    while (index < states.size()) {
        const EndingState& state = states[index];
        size_t end = index + 1;
        while (end < states.size() && states[end].kind == state.kind &&
               states[end].transition_id == state.transition_id &&
               states[end].clip_id == state.clip_id) {
            ++end;
        }
        if (state.kind != EndingFrame::Picture) {
            Value run = Value::MakeObject();
            run.Set("from_frame",
                    Value::MakeInt(first + static_cast<int64_t>(index)));
            run.Set("until_frame",
                    Value::MakeInt(first + static_cast<int64_t>(end)));
            run.Set("kind", Value::MakeString(EndingName(state.kind)));
            if (!state.transition_id.empty()) {
                run.Set("cause", Value::MakeString("dip_to_color"));
                run.Set("transition_id",
                        Value::MakeString(state.transition_id));
            } else if (!state.clip_id.empty()) {
                run.Set("cause", Value::MakeString("clip_opacity"));
                run.Set("clip_id", Value::MakeString(state.clip_id));
            } else {
                run.Set("cause", Value::MakeString("nothing_on_screen"));
            }
            runs.Push(std::move(run));
        }
        index = end;
    }
    return runs;
}

bool FinalBlackCheck(const Document& document, const TimelinePolicy& policy,
                     const TimelineFileMeasures* file,
                     TimelineCheckResult& result, std::string& error) {
    result = TimelineCheckResult{};
    result.name = "final_black";
    int64_t window = 0;
    if (!FinalWindowFrames(document.sequence, policy, window, error)) {
        return false;
    }
    int64_t frames = 0;
    std::string durationError;
    if (!FilmFrames(document, frames, durationError)) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = durationError;
        return true;
    }
    if (frames == 0) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = "the timeline is empty";
        return true;
    }
    const int64_t first = std::max<int64_t>(0, frames - window);
    std::vector<EndingState> states;
    if (!ClassifyEnding(document, first, frames - first, states, error)) {
        return false;
    }
    result.evidence.Set("from_frame", Value::MakeInt(first));
    result.evidence.Set("until_frame", Value::MakeInt(frames));
    Value runs = EndingRuns(states, first);
    const bool documentBlack = !runs.AsArray().empty();
    result.evidence.Set("document", std::move(runs));
    const int64_t motionFrames =
        std::count_if(states.begin(), states.end(),
                      [](const EndingState& state) { return state.motion; });
    if (motionFrames > 0) {
        result.evidence.Set("motion_frames", Value::MakeInt(motionFrames));
    }

    bool fileBlack = false;
    if (file != nullptr) {
        // The file's last frames, aligned on the film's last frame.
        const int64_t measured =
            static_cast<int64_t>(file->final_frames_black.size());
        Value fileRuns = Value::MakeArray();
        int64_t index = 0;
        while (index < measured) {
            if (!file->final_frames_black[static_cast<size_t>(index)]) {
                ++index;
                continue;
            }
            int64_t end = index + 1;
            while (end < measured &&
                   file->final_frames_black[static_cast<size_t>(end)]) {
                ++end;
            }
            Value run = Value::MakeObject();
            run.Set("from_frame", Value::MakeInt(frames - measured + index));
            run.Set("until_frame", Value::MakeInt(frames - measured + end));
            fileRuns.Push(std::move(run));
            index = end;
        }
        fileBlack = !fileRuns.AsArray().empty();
        result.evidence.Set("file_frames", Value::MakeInt(measured));
        result.evidence.Set("file", std::move(fileRuns));
    }
    if (policy.final_black == FinalBlackRule::Allowed) {
        return true;
    }
    if (documentBlack || fileBlack) {
        result.status = TimelineCheckStatus::Failed;
        result.reason =
            documentBlack
                ? "the end of the film goes to black, which the policy forbids"
                : "the delivered file shows black frames at its end, which "
                  "the policy forbids";
    }
    return true;
}

TimelineCheckResult CaptionContrastCheck(const TimelinePolicy& policy,
                                         const TimelineFileMeasures* file) {
    TimelineCheckResult result;
    result.name = "caption_contrast";
    result.evidence.Set("minimum_contrast",
                        FractionValue(policy.minimum_contrast_num,
                                      policy.minimum_contrast_den));
    if (file == nullptr) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason =
            "measured on the delivered file: give the exported file's path";
        return result;
    }
    if (!file->legibility_error.empty()) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason = file->legibility_error;
        return result;
    }
    const double minimum = static_cast<double>(policy.minimum_contrast_num) /
                           static_cast<double>(policy.minimum_contrast_den);
    Value below = Value::MakeArray();
    for (const CaptionLegibility& carton : file->legibility) {
        if (!CaptionLegibilityBelowMinimum(carton, minimum)) {
            continue;
        }
        Value item = Value::MakeObject();
        item.Set("clip_id", Value::MakeString(carton.clip_id));
        item.Set("text", Value::MakeString(carton.text));
        item.Set("frame", Value::MakeInt(carton.frame));
        // Measurements, not document time: two decimals, as
        // check_caption_legibility reports them.
        item.Set("contrast_median",
                 Value::MakeDouble(std::round(carton.contrast_median * 100.0) /
                                   100.0));
        item.Set(
            "contrast_low",
            Value::MakeDouble(std::round(carton.contrast_low * 100.0) / 100.0));
        // PA4 -- a carton can fall on its lit word alone: say so.
        if (carton.highlighted)
            item.Set("highlight_contrast_median",
                     Value::MakeDouble(
                         std::round(carton.highlight_contrast_median * 100.0) /
                         100.0));
        below.Push(std::move(item));
    }
    const int64_t count = static_cast<int64_t>(below.AsArray().size());
    result.evidence.Set(
        "cartons",
        Value::MakeInt(static_cast<int64_t>(file->legibility.size())));
    result.evidence.Set("below_minimum", std::move(below));
    if (count > 0) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = std::to_string(count) +
                        " carton(s) under the minimum contrast: "
                        "update_caption_style gives them an outline or a box";
    }
    return result;
}

// DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. What a motion
// clip draws with: the fonts of its texts and every colour its author
// wrote or bound -- styles, text ranges, repeat cycles, keyframes, gradient
// stops, filter and outline colours -- resolved for this instance, and the
// defaults the evaluator falls back on (a text with no colour is black, a
// path with no fill is filled black, a text with no family is system-ui).
// Read from the composition as written, not from evaluated frames: a fade
// from one charter colour to another passes through colours the charter
// does not list, and those are no choice of anyone's.
struct MotionUse {
    std::string node;
    std::string property;
    std::string value;
};

class MotionPaletteReader {
public:
    MotionPaletteReader(const MotionComposition& composition,
                        const Value& instance)
        : composition_(composition) {
        for (const MotionPropDefinition& prop : composition.props) {
            props_[prop.name] = prop.default_value;
        }
        if (instance.IsObject()) {
            for (const auto& [name, value] : instance.AsObject()) {
                props_[name] = value;
            }
        }
    }

    void Read(std::vector<MotionUse>& fonts, std::vector<MotionUse>& colors,
              std::vector<std::string>& unread) {
        fonts_ = &fonts;
        colors_ = &colors;
        unread_ = &unread;
        Inherited inherited;
        Walk(composition_.root, "root", inherited);
    }

private:
    // What CSS and SVG pass down to the text and the paths below a node.
    struct Inherited {
        std::vector<std::string> font = {"system-ui"};
        std::vector<std::string> color = {"#000000"};
        std::vector<std::string> fill = {"black"};
        std::vector<std::string> stroke = {"none"};
    };

    static std::string KeyText(const Value& value) {
        if (value.IsString()) {
            return value.AsString();
        }
        if (value.IsBool()) {
            return value.AsBool() ? "true" : "false";
        }
        return value.Dump();
    }

    const Value* Lookup(const MotionValue& binding, const Value& key) const {
        const std::string text = KeyText(key);
        if (!binding.table.empty()) {
            for (const MotionTokenTable& table : composition_.tokens) {
                if (table.name != binding.table) {
                    continue;
                }
                for (const auto& [entry, value] : table.entries) {
                    if (entry == text) {
                        return &value;
                    }
                }
            }
            return nullptr;
        }
        for (const auto& [entry, value] : binding.inline_map) {
            if (entry == text) {
                return &value;
            }
        }
        return nullptr;
    }

    // Every value `binding` can take here: one, or one per element of the
    // enclosing repeat.
    std::vector<Value> Resolve(const MotionValue& binding) const {
        std::vector<Value> values;
        switch (binding.kind) {
            case MotionBindingKind::Literal:
                values.push_back(binding.literal);
                break;
            case MotionBindingKind::Prop:
            case MotionBindingKind::PropMap: {
                if (!binding.component.empty()) {
                    break;
                }
                const auto found = props_.find(binding.prop);
                if (found == props_.end()) {
                    break;
                }
                if (binding.kind == MotionBindingKind::Prop) {
                    values.push_back(found->second);
                } else if (const Value* mapped =
                               Lookup(binding, found->second)) {
                    values.push_back(*mapped);
                }
                break;
            }
            case MotionBindingKind::PropNotEmpty: {
                const auto found = props_.find(binding.prop);
                if (found != props_.end()) {
                    values.push_back(
                        Value::MakeBool(!(found->second.IsString() &&
                                          found->second.AsString().empty())));
                }
                break;
            }
            case MotionBindingKind::Item:
            case MotionBindingKind::ItemMap:
                if (items_.empty()) {
                    break;
                }
                for (const Value& item : items_.back()) {
                    if (binding.kind == MotionBindingKind::Item) {
                        values.push_back(item);
                    } else if (const Value* mapped = Lookup(binding, item)) {
                        values.push_back(*mapped);
                    }
                }
                break;
            default:
                break;
        }
        return values;
    }

    // The colours inside a gradient, a filter or a text outline.
    static std::vector<std::string> ColorsIn(const std::string& text) {
        static const std::regex kColor(
            R"(#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)|\b(?:white|black)\b)");
        std::vector<std::string> colors;
        for (std::sregex_iterator match(text.begin(), text.end(), kColor), end;
             match != end; ++match) {
            colors.push_back(match->str());
        }
        return colors;
    }

    void AddColors(const std::string& node, const std::string& property,
                   MotionValueKind kind, const Value& value) {
        if (!value.IsString()) {
            return;
        }
        const std::string& text = value.AsString();
        if (kind == MotionValueKind::Color) {
            if (text != "none" && text != "currentColor") {
                colors_->push_back({node, property, text});
            }
            return;
        }
        for (const std::string& color : ColorsIn(text)) {
            colors_->push_back({node, property, color});
        }
    }

    static bool HasColors(const std::string& property, MotionValueKind& kind) {
        kind = MotionPropertyValueKind(property);
        if (property == "textStroke") {
            kind = MotionValueKind::Filter;  // A width and a colour.
            return true;
        }
        return kind == MotionValueKind::Color ||
               kind == MotionValueKind::Gradient ||
               kind == MotionValueKind::Filter;
    }

    // An inherited property is read where it is drawn: `color` on a text,
    // `fill` and `stroke` on a path; written on a box, it reaches them
    // through Inherited.
    static bool DrawnHere(const std::string& property, MotionNodeType type) {
        if (property == "color") {
            return type == MotionNodeType::Text;
        }
        if (property == "fill" || property == "stroke") {
            return type == MotionNodeType::Path;
        }
        return true;
    }

    void ReadStyle(
        const std::string& node, MotionNodeType type,
        const std::vector<std::pair<std::string, MotionValue>>& style) {
        for (const auto& [property, binding] : style) {
            MotionValueKind kind;
            if (property == "fontFamily" || !DrawnHere(property, type) ||
                !HasColors(property, kind)) {
                continue;
            }
            for (const Value& value : Resolve(binding)) {
                AddColors(node, property, kind, value);
            }
        }
    }

    // What a node's own style gives `property` for its descendants: its
    // resolved values, or nothing when it does not set it.
    std::vector<std::string> Own(const MotionNode& node,
                                 const std::string& property) const {
        std::vector<std::string> own;
        for (const auto& [name, binding] : node.style) {
            if (name != property) {
                continue;
            }
            for (const Value& value : Resolve(binding)) {
                if (value.IsString()) {
                    own.push_back(value.AsString());
                }
            }
        }
        return own;
    }

    bool Animates(const MotionNode& node, const std::string& property) const {
        return std::any_of(
            node.animate.begin(), node.animate.end(),
            [&](const auto& entry) { return entry.first == property; });
    }

    void Walk(const MotionNode& node, const std::string& path,
              Inherited inherited) {
        const std::string name = node.id.empty() ? path : node.id;
        // A node this instance hides draws nothing.
        if (node.visible) {
            const std::vector<Value> shown = Resolve(*node.visible);
            if (!shown.empty() &&
                std::none_of(shown.begin(), shown.end(), [](const Value& v) {
                    return !v.IsBool() || v.AsBool();
                })) {
                return;
            }
        }
        if (std::vector<std::string> font = Own(node, "fontFamily");
            !font.empty()) {
            inherited.font = font;
        }
        ReadStyle(name, node.type, node.style);
        for (const MotionTextRange& range : node.ranges) {
            ReadStyle(name, MotionNodeType::Text, range.style);
        }
        for (const auto& [property, values] : node.cycle) {
            MotionValueKind kind;
            if (!HasColors(property, kind)) {
                continue;
            }
            for (const MotionValue& binding : values) {
                for (const Value& value : Resolve(binding)) {
                    AddColors(name, property, kind, value);
                }
            }
        }
        for (const auto& [property, animation] : node.animate) {
            MotionValueKind kind;
            if (!HasColors(property, kind)) {
                continue;
            }
            for (size_t index = 0; index < animation.values.size(); ++index) {
                const auto bound = std::find_if(
                    animation.bound_values.begin(),
                    animation.bound_values.end(),
                    [&](const auto& entry) { return entry.first == index; });
                if (bound == animation.bound_values.end()) {
                    AddColors(name, property, kind, animation.values[index]);
                    continue;
                }
                for (const Value& value : Resolve(bound->second)) {
                    AddColors(name, property, kind, value);
                }
            }
        }
        // The text colour and the SVG paint reach the descendants that do
        // not set their own; what nobody sets is the evaluator's default.
        // A value that only an animation gives is read on the animation.
        const auto inherit = [&](const char* property,
                                 std::vector<std::string>& passed,
                                 MotionNodeType drawn) {
            std::vector<std::string> own = Own(node, property);
            const bool set = !own.empty() || Animates(node, property);
            if (node.type == drawn && !set) {
                for (const std::string& value : passed) {
                    if (value != "none" && value != "currentColor") {
                        colors_->push_back({name, property, value});
                    }
                }
            }
            if (set) {
                passed = std::move(own);
            }
        };
        inherit("color", inherited.color, MotionNodeType::Text);
        inherit("fill", inherited.fill, MotionNodeType::Path);
        inherit("stroke", inherited.stroke, MotionNodeType::Path);
        if (node.type == MotionNodeType::Text) {
            for (const std::string& font : inherited.font) {
                fonts_->push_back({name, "fontFamily", font});
            }
        }
        if (node.type == MotionNodeType::Composition) {
            unread_->push_back(name +
                               ": an instance of another composition, "
                               "checked where it is placed itself");
        }
        bool repeat = false;
        if (node.type == MotionNodeType::Repeat && node.each) {
            std::vector<Value> elements;
            for (const Value& list : Resolve(*node.each)) {
                if (list.IsArray()) {
                    for (const Value& element : list.AsArray()) {
                        elements.push_back(element);
                    }
                }
            }
            items_.push_back(std::move(elements));
            repeat = true;
        }
        for (size_t index = 0; index < node.children.size(); ++index) {
            Walk(node.children[index], path + "." + std::to_string(index),
                 inherited);
        }
        if (repeat) {
            items_.pop_back();
        }
    }

    const MotionComposition& composition_;
    std::map<std::string, Value> props_;
    std::vector<std::vector<Value>> items_;
    std::vector<MotionUse>* fonts_ = nullptr;
    std::vector<MotionUse>* colors_ = nullptr;
    std::vector<std::string>* unread_ = nullptr;
};

std::string Lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return text;
}

// A caption colour (FFmpeg's syntax) as "#rrggbb"; empty when transparent
// or unreadable.
std::string CaptionColorKey(const std::string& text) {
    std::array<uint8_t, 4> rgba{};
    if (!ParseCaptionColor(text, rgba) || rgba[3] == 0) {
        return std::string();
    }
    char key[8];
    std::snprintf(key, sizeof key, "#%02x%02x%02x", rgba[0], rgba[1], rgba[2]);
    return key;
}

TimelineCheckResult CharterCheck(const Document& document,
                                 const TimelinePolicy& policy) {
    TimelineCheckResult result;
    result.name = "charter";
    if (!policy.kit) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason =
            "the policy names no kit: give policy.kit, a brand's charter of "
            "the library (list_motion_templates kits)";
        return result;
    }
    const MotionKitInfo& kit = *policy.kit;
    const std::vector<std::string> colors = MotionKitColors(kit);
    const std::vector<std::string> fonts = MotionKitFonts(kit);
    std::set<std::string> allowedColors(colors.begin(), colors.end());
    std::set<std::string> allowedFonts;
    for (const std::string& font : fonts) {
        allowedFonts.insert(Lowercase(font));
    }
    Value kitEntry = Value::MakeObject();
    kitEntry.Set("name", Value::MakeString(kit.name));
    if (kit.version > 0) {
        kitEntry.Set("version", Value::MakeInt(kit.version));
    }
    Value colorList = Value::MakeArray();
    for (const std::string& color : colors) {
        colorList.Push(Value::MakeString(color));
    }
    Value fontList = Value::MakeArray();
    for (const std::string& font : fonts) {
        fontList.Push(Value::MakeString(font));
    }
    kitEntry.Set("colors", std::move(colorList));
    kitEntry.Set("fonts", std::move(fontList));
    result.evidence.Set("kit", std::move(kitEntry));

    // A value outside the charter, once per place it is written: a caption
    // style however many cartons use it, a motion clip's node and property.
    Value outside = Value::MakeArray();
    const auto colorOutside = [&](const std::string& key) {
        return !key.empty() && allowedColors.count(key) == 0;
    };
    const auto fontOutside = [&](const std::string& font) {
        return allowedFonts.count(Lowercase(font)) == 0;
    };

    // Caption styles, in the order the sequence lists them, with the
    // cartons the export burns that use each: a visible caption track's
    // clips with a text. A carton without a style is drawn in a new
    // style's defaults (Export.cc), and judged as such, style_id empty.
    std::map<Ulid, std::vector<Ulid>> cartons;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "caption" || !track.visible) {
            continue;
        }
        for (const DocumentClip& clip : track.clips) {
            // A disabled carton is not burned (ClipEnabled.h).
            if (!clip.caption_text.empty() &&
                ClipIsSeen(document.sequence, clip)) {
                cartons[clip.caption_group_id].push_back(clip.id);
            }
        }
    }
    std::vector<CaptionStyle> styles = document.sequence.caption_styles;
    if (cartons.count(Ulid()) != 0) {
        CaptionStyle unstyled;
        unstyled.id = Ulid();
        styles.push_back(unstyled);
    }
    int64_t stylesRead = 0;
    for (const CaptionStyle& style : styles) {
        const auto used = cartons.find(style.id);
        if (used == cartons.end()) {
            continue;
        }
        ++stylesRead;
        const auto finding = [&](const char* setting,
                                 const std::string& value) {
            Value item = Value::MakeObject();
            item.Set("in", Value::MakeString("caption_style"));
            item.Set("style_id", Value::MakeString(style.id));
            item.Set("setting", Value::MakeString(setting));
            item.Set("value", Value::MakeString(value));
            item.Set("cartons",
                     Value::MakeInt(static_cast<int64_t>(used->second.size())));
            item.Set("first_clip_id", Value::MakeString(used->second.front()));
            outside.Push(std::move(item));
        };
        const std::string family = CaptionFontName(style);
        if (fontOutside(family)) {
            finding("font_family", family);
        }
        const std::pair<const char*, const std::string*> painted[] = {
            {"color", &style.color},
            {"outline_color",
             style.outline_width > 0 ? &style.outline_color : nullptr},
            {"shadow_color",
             style.shadow_offset > 0 ? &style.shadow_color : nullptr},
            {"background_color", style.background_color.empty()
                                     ? nullptr
                                     : &style.background_color},
        };
        for (const auto& [setting, value] : painted) {
            if (value != nullptr && colorOutside(CaptionColorKey(*value))) {
                finding(setting, *value);
            }
        }
    }

    // Motion clips of visible picture tracks.
    int64_t motionRead = 0;
    Value unread = Value::MakeArray();
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || !track.visible) {
            continue;
        }
        for (const DocumentClip& clip : track.clips) {
            if (!IsMotionClip(clip)) {
                continue;
            }
            const DocumentComposition* stored =
                document.FindComposition(clip.composition_id);
            MotionComposition composition;
            MotionError parseError;
            if (stored == nullptr ||
                !ParseMotionComposition(stored->definition, composition,
                                        parseError)) {
                unread.Push(Value::MakeString(
                    clip.id + ": its composition could not be read"));
                continue;
            }
            ++motionRead;
            std::vector<MotionUse> usedFonts;
            std::vector<MotionUse> usedColors;
            std::vector<std::string> notRead;
            MotionPaletteReader(composition, clip.motion_props)
                .Read(usedFonts, usedColors, notRead);
            std::set<std::string> said;
            const auto finding = [&](const MotionUse& use) {
                if (!said.insert(use.node + '\n' + use.property + '\n' +
                                 use.value)
                         .second) {
                    return;
                }
                Value item = Value::MakeObject();
                item.Set("in", Value::MakeString("motion"));
                item.Set("clip_id", Value::MakeString(clip.id));
                item.Set("composition_id",
                         Value::MakeString(clip.composition_id));
                item.Set("node", Value::MakeString(use.node));
                item.Set("property", Value::MakeString(use.property));
                item.Set("value", Value::MakeString(use.value));
                outside.Push(std::move(item));
            };
            for (const MotionUse& use : usedFonts) {
                if (fontOutside(use.value)) {
                    finding(use);
                }
            }
            for (const MotionUse& use : usedColors) {
                if (colorOutside(MotionColorKey(use.value))) {
                    finding(use);
                }
            }
            for (const std::string& note : notRead) {
                unread.Push(Value::MakeString(clip.id + " " + note));
            }
        }
    }
    result.evidence.Set("caption_styles", Value::MakeInt(stylesRead));
    result.evidence.Set("motion_clips", Value::MakeInt(motionRead));
    // Each value outside the charter once, with how many places write it:
    // a film that fills a grid with one off-charter blue names it in five
    // hundred nodes, and the question is one colour.
    std::vector<std::pair<std::string, int64_t>> values;
    for (const Value& item : outside.AsArray()) {
        const Value* found = item.Find("value");
        const std::string value = found != nullptr && found->IsString()
                                      ? found->AsString()
                                      : std::string();
        const auto known = std::find_if(
            values.begin(), values.end(),
            [&](const auto& entry) { return entry.first == value; });
        if (known == values.end()) {
            values.emplace_back(value, 1);
        } else {
            ++known->second;
        }
    }
    Value summary = Value::MakeArray();
    for (const auto& [value, places] : values) {
        Value entry = Value::MakeObject();
        entry.Set("value", Value::MakeString(value));
        entry.Set("places", Value::MakeInt(places));
        summary.Push(std::move(entry));
    }
    result.evidence.Set("outside_values", std::move(summary));
    const int64_t count = static_cast<int64_t>(outside.AsArray().size());
    result.evidence.Set("outside", std::move(outside));
    if (!unread.AsArray().empty()) {
        result.evidence.Set("not_read", std::move(unread));
    }
    if (count > 0) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = std::to_string(values.size()) +
                        " font(s) or colour(s) outside the charter '" +
                        kit.name + "', in " + std::to_string(count) +
                        " place(s): update_caption_style, or the props of "
                        "the motion (set_props), bring them back to it";
    }
    return result;
}

bool SameFraction(const EffectParamValue& left, const EffectParamValue& right) {
    return static_cast<int64_t>(left.num) * right.den ==
           static_cast<int64_t>(right.num) * left.den;
}

// Two clips that frame their picture alike and hold it there. A punch-in,
// a reframe or a moving framing on either side changes the shot, which is
// how an editor breaks a jump without a cutaway (the author's Resolve pass of
// 30 October 2026: seven films of eleven).
bool SameStillFraming(const DocumentClip& left, const DocumentClip& right) {
    for (const DocumentClip* clip : {&left, &right}) {
        for (const ClipAnimatedParameter parameter :
             {ClipAnimatedParameter::FramingScale,
              ClipAnimatedParameter::FramingPositionX,
              ClipAnimatedParameter::FramingPositionY}) {
            if (ClipAnimates(*clip, parameter)) {
                return false;
            }
        }
    }
    const ClipFraming& a = left.framing;
    const ClipFraming& b = right.framing;
    return a.mode == b.mode && SameFraction(a.scale, b.scale) &&
           SameFraction(a.position_x, b.position_x) &&
           SameFraction(a.position_y, b.position_y) &&
           SameFraction(a.crop_left, b.crop_left) &&
           SameFraction(a.crop_right, b.crop_right) &&
           SameFraction(a.crop_top, b.crop_top) &&
           SameFraction(a.crop_bottom, b.crop_bottom);
}

// The clip whose picture alone makes this frame of the film, composed as
// the export composes it; empty when a dip, a dissolve or a clip at part
// opacity mixes pictures there, or when nothing is on screen. A motion clip
// is drawn over what lies under it, so the search goes on below it.
bool SolePictureClip(const Document& document, const Timeline& timeline,
                     const std::vector<const DocumentTrack*>& tracks,
                     int64_t frame, Ulid& clipId, std::string& error) {
    clipId.clear();
    const MediaRate rate = document.sequence.frame_rate;
    const RationalTime position{frame * rate.den, rate.num};
    for (const DocumentTrack* track : tracks) {
        const std::vector<ResolvedLayer> layers =
            timeline.ResolveTrackLayers(track->id, position);
        if (layers.empty()) {
            continue;
        }
        const std::optional<ResolvedColorVeil> veil =
            timeline.ResolveTrackColorVeil(track->id, position);
        if ((veil && veil->alpha.num != 0) || layers.size() > 1) {
            return true;
        }
        const DocumentClip* clip =
            document.FindClip(layers.front().frame.clip_id);
        if (clip == nullptr) {
            return true;
        }
        if (IsMotionClip(*clip)) {
            continue;
        }
        EffectParamValue opacity{1, 1};
        if (!ClipOpacityAtFrame(*clip, frame, rate, opacity, error)) {
            return false;
        }
        if (opacity.den > 0 && opacity.num <= 0) {
            continue;
        }
        if (opacity.den > 0 && opacity.num < opacity.den) {
            return true;
        }
        clipId = clip->id;
        return true;
    }
    return true;
}

bool JumpCutsCheck(const Document& document, const TimelinePolicy& policy,
                   std::vector<TimelineJumpCut>& jumps,
                   TimelineCheckResult& result, std::string& error) {
    jumps.clear();
    result = TimelineCheckResult{};
    result.name = "jump_cuts";
    // The cuts review_cuts reviews, so that both name the same junctions.
    std::vector<TimelineJunction> junctions;
    int64_t offGrid = 0;
    if (!ListTimelineJunctions(document, junctions, offGrid, error)) {
        return false;
    }
    const std::vector<const DocumentTrack*> tracks =
        VisibleVideoTracksTopFirst(document);
    try {
        const Timeline timeline(document);
        for (const TimelineJunction& junction : junctions) {
            if (!junction.picture) {
                continue;
            }
            for (const TimelineJunctionCut& cut : junction.cuts) {
                if (cut.track_kind != "video" || cut.continuous ||
                    cut.outgoing_source_id != cut.incoming_source_id) {
                    continue;
                }
                const DocumentClip* outgoing =
                    document.FindClip(cut.outgoing_clip_id);
                const DocumentClip* incoming =
                    document.FindClip(cut.incoming_clip_id);
                if (outgoing == nullptr || incoming == nullptr ||
                    IsMotionClip(*outgoing) ||
                    !SameStillFraming(*outgoing, *incoming)) {
                    continue;
                }
                Ulid before;
                Ulid after;
                if (!SolePictureClip(document, timeline, tracks,
                                     junction.frame - 1, before, error) ||
                    !SolePictureClip(document, timeline, tracks, junction.frame,
                                     after, error)) {
                    return false;
                }
                if (before == outgoing->id && after == incoming->id) {
                    jumps.push_back({junction.frame, incoming->timeline_in,
                                     cut.track_id, outgoing->id, incoming->id,
                                     outgoing->source_id});
                }
            }
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    result.evidence.Set("count",
                        Value::MakeInt(static_cast<int64_t>(jumps.size())));
    if (offGrid > 0) {
        result.evidence.Set("off_grid", Value::MakeInt(offGrid));
    }
    if (policy.jump_cuts == JumpCutRule::Allowed) {
        result.status = TimelineCheckStatus::NotRun;
        result.reason =
            "the policy allows jump cuts: they are listed under jump_cuts, "
            "judged by none";
    } else if (!jumps.empty()) {
        result.status = TimelineCheckStatus::Failed;
        result.reason = std::to_string(jumps.size()) +
                        " cut(s) inside one rush show the same framing on "
                        "both sides, with nothing over them";
    }
    return true;
}

}  // namespace

bool ParseTimelinePolicy(const Value& value, TimelinePolicy& policy,
                         std::string& error) {
    policy = TimelinePolicy{};
    error.clear();
    if (!value.IsObject()) {
        error = "'policy' must be a JSON object";
        return false;
    }
    static const std::vector<std::string> kKeys = {
        "name",          "duration_min", "duration_max",
        "final_black",   "final_window", "minimum_contrast",
        "kit",           "jump_cuts",    "loudness_target",
        "true_peak_max", "aspect_ratio"};
    for (const auto& entry : value.AsObject()) {
        if (std::find(kKeys.begin(), kKeys.end(), entry.first) == kKeys.end()) {
            error = "'policy' has unknown key '" + entry.first + "'";
            return false;
        }
    }
    bool named = false;
    if (const Value* name = value.Find("name")) {
        if (!name->IsString() || name->AsString().empty() ||
            name->AsString().size() > 64) {
            error = "'policy.name' must be a string of 1 to 64 bytes";
            return false;
        }
        policy.name = name->AsString();
        named = true;
    }
    for (const char* key : {"duration_min", "duration_max", "final_window"}) {
        const Value* field = value.Find(key);
        if (field == nullptr) {
            continue;
        }
        RationalTime time;
        if (!ReadPolicyTime(*field, key, time, error)) {
            return false;
        }
        if (std::string(key) == "duration_min") {
            policy.duration_min = time;
        } else if (std::string(key) == "duration_max") {
            policy.duration_max = time;
        } else {
            policy.final_window = time;
        }
    }
    if (policy.duration_min && policy.duration_max &&
        *policy.duration_min > *policy.duration_max) {
        error = "'policy.duration_min' is longer than 'policy.duration_max'";
        return false;
    }
    if (policy.final_window && policy.final_window->value == 0) {
        error = "'policy.final_window' must be longer than zero";
        return false;
    }
    if (const Value* rule = value.Find("final_black")) {
        if (rule->IsString() && rule->AsString() == "forbidden") {
            policy.final_black = FinalBlackRule::Forbidden;
        } else if (rule->IsString() && rule->AsString() == "allowed") {
            policy.final_black = FinalBlackRule::Allowed;
        } else {
            error = "'policy.final_black' must be \"forbidden\" or \"allowed\"";
            return false;
        }
    }
    if (const Value* rule = value.Find("jump_cuts")) {
        if (rule->IsString() && rule->AsString() == "forbidden") {
            policy.jump_cuts = JumpCutRule::Forbidden;
        } else if (rule->IsString() && rule->AsString() == "allowed") {
            policy.jump_cuts = JumpCutRule::Allowed;
        } else {
            error = "'policy.jump_cuts' must be \"forbidden\" or \"allowed\"";
            return false;
        }
    }
    if (const Value* contrast = value.Find("minimum_contrast")) {
        const Value* num =
            contrast->IsObject() ? contrast->Find("num") : nullptr;
        const Value* den =
            contrast->IsObject() ? contrast->Find("den") : nullptr;
        int64_t numValue = 0;
        int64_t denValue = 0;
        if (num == nullptr || den == nullptr ||
            contrast->AsObject().size() != 2 || !num->IsNumber() ||
            !den->IsNumber() || !num->AsInt64(numValue) ||
            !den->AsInt64(denValue)) {
            error = "'policy.minimum_contrast' must be {\"num\",\"den\"}";
            return false;
        }
        // The bounds of check_caption_legibility: a WCAG ratio lies
        // between 1 and 21.
        if (denValue <= 0 || denValue > 1000 || numValue < denValue ||
            numValue > 21 * denValue) {
            error = "'policy.minimum_contrast' must lie between 1 and 21";
            return false;
        }
        policy.minimum_contrast_num = static_cast<int32_t>(numValue);
        policy.minimum_contrast_den = static_cast<int32_t>(denValue);
    }
    if (const Value* kit = value.Find("kit")) {
        if (!kit->IsString() || !IsMotionPieceName(kit->AsString())) {
            error =
                "'policy.kit' must be the name of a charter of the library: "
                "letters, digits, '-' and '_'";
            return false;
        }
        policy.kit_name = kit->AsString();
    }
    // PA2 -- whole LUFS and whole dBTP: the meter reads to a tenth, and a
    // target is a round number in every delivery specification.
    if (const Value* target = value.Find("loudness_target")) {
        int64_t lufs = 0;
        if (!target->IsNumber() || !target->AsInt64(lufs) || lufs < -40 ||
            lufs > -5) {
            error =
                "'policy.loudness_target' must be a whole number of "
                "LUFS from -40 to -5";
            return false;
        }
        policy.loudness_target_lufs = static_cast<int32_t>(lufs);
    }
    if (const Value* peak = value.Find("true_peak_max")) {
        int64_t dbtp = 0;
        if (!peak->IsNumber() || !peak->AsInt64(dbtp) || dbtp < -20 ||
            dbtp > 0) {
            error =
                "'policy.true_peak_max' must be a whole number of dBTP "
                "from -20 to 0";
            return false;
        }
        if (!policy.loudness_target_lufs) {
            error =
                "'policy.true_peak_max' is judged with the loudness: "
                "set 'policy.loudness_target' too";
            return false;
        }
        policy.true_peak_max_dbtp = static_cast<int32_t>(dbtp);
    }
    // PA11 -- the shape a delivery asks for.
    if (const Value* aspect = value.Find("aspect_ratio")) {
        int64_t width = 0;
        int64_t height = 0;
        std::string detail;
        if (!aspect->IsString() ||
            !generation::ParseAspectRatio(aspect->AsString(), width, height,
                                          detail)) {
            error =
                "'policy.aspect_ratio' must be two whole numbers, as "
                "\"9:16\"";
            return false;
        }
        policy.aspect_ratio = aspect->AsString();
    }
    if (!named && PolicyChangesDefault(policy)) {
        policy.name = "custom";
    }
    return true;
}

bool TimelineCheckReport::Passed() const {
    return std::none_of(checks.begin(), checks.end(),
                        [](const TimelineCheckResult& check) {
                            return check.status == TimelineCheckStatus::Failed;
                        });
}

bool FinalWindowFrames(const DocumentSequence& sequence,
                       const TimelinePolicy& policy, int64_t& frames,
                       std::string& error) {
    const MediaRate rate = sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "invalid sequence frame rate";
        return false;
    }
    if (!policy.final_window) {
        // One second, rounded up to whole frames.
        frames = (static_cast<int64_t>(rate.num) + rate.den - 1) / rate.den;
        return true;
    }
    const __int128 numerator =
        static_cast<__int128>(policy.final_window->value) * rate.num;
    const __int128 denominator =
        static_cast<__int128>(policy.final_window->rate) * rate.den;
    if (numerator % denominator != 0) {
        error =
            "'policy.final_window' must be a whole number of sequence frames";
        return false;
    }
    frames = static_cast<int64_t>(numerator / denominator);
    return true;
}

bool ClassifyEndingFrames(const Document& document, int64_t first,
                          int64_t count, std::vector<EndingFrame>& frames,
                          std::string& error) {
    std::vector<EndingState> states;
    if (!ClassifyEnding(document, first, count, states, error)) {
        return false;
    }
    frames.clear();
    for (const EndingState& state : states) {
        frames.push_back(state.kind);
    }
    return true;
}

bool CheckTimeline(const Document& document, const TimelinePolicy& policy,
                   const TimelineFileMeasures* file,
                   TimelineCheckReport& report, std::string& error) {
    report = TimelineCheckReport{};
    error.clear();
    report.policy = policy;
    report.timeline_id = document.sequence.id;
    report.file_measured = file != nullptr;
    TimelineStats stats;
    if (!CalculateTimelineStats(document, stats, error)) {
        return false;
    }
    report.duration = stats.duration;
    try {
        report.checks.push_back(LinksCheck(document));
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    report.checks.push_back(SyncCheck(stats));
    report.checks.push_back(DurationCheck(policy, stats.duration));
    TimelineCheckResult finalBlack;
    if (!FinalBlackCheck(document, policy, file, finalBlack, error)) {
        return false;
    }
    report.checks.push_back(std::move(finalBlack));
    report.checks.push_back(CaptionContrastCheck(policy, file));
    if (!policy.kit_name.empty() &&
        (!policy.kit || policy.kit->name != policy.kit_name)) {
        error = "the policy names kit '" + policy.kit_name +
                "' and it was not looked up in the library";
        return false;
    }
    report.checks.push_back(CharterCheck(document, policy));
    TimelineCheckResult jumpCuts;
    if (!JumpCutsCheck(document, policy, report.jump_cuts, jumpCuts, error)) {
        return false;
    }
    report.checks.push_back(std::move(jumpCuts));
    // PA2, PA11 -- listed only when the policy asks: a report whose policy
    // names neither keeps the seven checks it always had.
    if (policy.loudness_target_lufs)
        report.checks.push_back(LoudnessCheck(policy, file));
    if (!policy.aspect_ratio.empty())
        report.checks.push_back(FormatCheck(document, policy));
    // DEC36 -- what the agent's report must name, read from the document.
    // Out of lot (29 October 2026) -- and the clips taken out of the film
    // by set_clip_enabled: a generated shot that is disabled plays no
    // generated picture, and is listed as disabled instead.
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.enabled) {
                report.disabled.push_back(
                    {clip.id, track.id, clip.timeline_in, clip.duration});
                continue;
            }
            if (!ClipPlaysGeneratedMedia(document, clip)) {
                continue;
            }
            report.generated.push_back(
                {clip.id, track.id, clip.source_id, clip.timeline_in,
                 clip.duration,
                 document.FindLibraryMedia(clip.source_id)->generation});
        }
    }
    return true;
}

bool GreyFrameIsBlack(const uint8_t* pixels, size_t count) {
    if (count == 0) {
        return false;
    }
    // blackdetect's pixel_black_th (0.10 of full scale) and
    // picture_black_ratio_th (0.98).
    size_t black = 0;
    for (size_t index = 0; index < count; ++index) {
        if (pixels[index] <= 25) {
            ++black;
        }
    }
    return black * 50 >= count * 49;
}

bool MeasureDeliveredFile(const Document& document,
                          const TimelinePolicy& policy, const std::string& path,
                          const std::string& ffmpegPath,
                          TimelineFileMeasures& measures, std::string& error) {
    measures = TimelineFileMeasures{};
    error.clear();
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(path, filesystemError)) {
        error = "exported file does not exist: " + path;
        return false;
    }
    int64_t window = 0;
    if (!FinalWindowFrames(document.sequence, policy, window, error)) {
        return false;
    }
    const MediaRate rate = document.sequence.frame_rate;
    const int32_t width = document.sequence.width;
    const int32_t height = document.sequence.height;
    if (width <= 0 || height <= 0) {
        error = "invalid sequence format";
        return false;
    }
    // Small and grey, as verify_export reads a file: the ratio of black
    // pixels survives an area average.
    const int32_t thumbnailWidth = 160;
    const int32_t thumbnailHeight = std::max<int32_t>(
        2, static_cast<int32_t>(
               (static_cast<int64_t>(thumbnailWidth) * height / width) &
               ~int64_t{1}));
    const size_t frameBytes = static_cast<size_t>(thumbnailWidth) *
                              static_cast<size_t>(thumbnailHeight);
    // Whole seconds before the end, one more than the window holds: the
    // seek only has to start early enough, the frames kept are counted back
    // from the last one the file decodes.
    const int64_t seconds = (window * rate.den + rate.num - 1) / rate.num + 1;
    std::deque<bool> tail;
    if (!ReadFfmpegRawFrames(
            {ffmpegPath,
             "-hide_banner",
             "-loglevel",
             "error",
             "-nostdin",
             "-sseof",
             "-" + std::to_string(seconds),
             "-i",
             path,
             "-map",
             "0:v:0",
             "-an",
             "-sn",
             "-fps_mode",
             "passthrough",
             "-vf",
             "scale=" + std::to_string(thumbnailWidth) + ':' +
                 std::to_string(thumbnailHeight) + ":flags=area,format=gray",
             "-f",
             "rawvideo",
             "-pix_fmt",
             "gray",
             "pipe:1"},
            frameBytes,
            [&](const uint8_t* pixels) {
                tail.push_back(GreyFrameIsBlack(pixels, frameBytes));
                if (static_cast<int64_t>(tail.size()) > window) {
                    tail.pop_front();
                }
            },
            error)) {
        return false;
    }
    measures.final_frames_black.assign(tail.begin(), tail.end());
    // The cartons are located by the caption renderer; a process without
    // one leaves this check unrun, not the others.
    std::string legibilityError;
    if (!CheckCaptionLegibility(document, path, ffmpegPath, measures.legibility,
                                legibilityError)) {
        measures.legibility.clear();
        measures.legibility_error = legibilityError.empty()
                                        ? "the cartons could not be measured"
                                        : legibilityError;
    }
    // PA2 -- one more decode of the sound, paid only by a policy that
    // judges it.
    if (policy.loudness_target_lufs) {
        FileLoudness loudness;
        std::string loudnessError;
        if (MeasureFileLoudness(path, ffmpegPath, loudness, loudnessError)) {
            measures.loudness_measured = true;
            measures.integrated_lufs = loudness.integrated_lufs;
            measures.true_peak_dbfs = loudness.true_peak_dbfs;
        } else {
            measures.loudness_error = loudnessError;
        }
    }
    return true;
}

std::string SerializeTimelineCheck(const TimelineCheckReport& report) {
    Value root = Value::MakeObject();
    Value policy = Value::MakeObject();
    policy.Set("name", Value::MakeString(report.policy.name));
    if (report.policy.duration_min) {
        policy.Set("duration_min", TimeValue(*report.policy.duration_min));
    }
    if (report.policy.duration_max) {
        policy.Set("duration_max", TimeValue(*report.policy.duration_max));
    }
    policy.Set(
        "final_black",
        Value::MakeString(report.policy.final_black == FinalBlackRule::Forbidden
                              ? "forbidden"
                              : "allowed"));
    policy.Set("jump_cuts", Value::MakeString(report.policy.jump_cuts ==
                                                      JumpCutRule::Forbidden
                                                  ? "forbidden"
                                                  : "allowed"));
    if (report.policy.final_window) {
        policy.Set("final_window", TimeValue(*report.policy.final_window));
    }
    policy.Set("minimum_contrast",
               FractionValue(report.policy.minimum_contrast_num,
                             report.policy.minimum_contrast_den));
    if (!report.policy.kit_name.empty()) {
        policy.Set("kit", Value::MakeString(report.policy.kit_name));
    }
    if (report.policy.loudness_target_lufs) {
        policy.Set("loudness_target",
                   Value::MakeInt(*report.policy.loudness_target_lufs));
        policy.Set("true_peak_max",
                   Value::MakeInt(report.policy.true_peak_max_dbtp));
    }
    if (!report.policy.aspect_ratio.empty()) {
        policy.Set("aspect_ratio",
                   Value::MakeString(report.policy.aspect_ratio));
    }
    root.Set("policy", std::move(policy));
    root.Set("timeline_id", Value::MakeString(report.timeline_id));
    root.Set("duration", TimeValue(report.duration));
    root.Set("file_measured", Value::MakeBool(report.file_measured));
    root.Set("passed", Value::MakeBool(report.Passed()));
    Value failed = Value::MakeArray();
    Value notRun = Value::MakeArray();
    Value checks = Value::MakeArray();
    for (const TimelineCheckResult& check : report.checks) {
        if (check.status == TimelineCheckStatus::Failed) {
            failed.Push(Value::MakeString(check.name));
        }
        if (check.status == TimelineCheckStatus::NotRun) {
            notRun.Push(Value::MakeString(check.name));
        }
        Value entry = Value::MakeObject();
        entry.Set("name", Value::MakeString(check.name));
        entry.Set("status", Value::MakeString(StatusName(check.status)));
        if (!check.reason.empty()) {
            entry.Set("reason", Value::MakeString(check.reason));
        }
        for (const auto& [key, value] : check.evidence.AsObject()) {
            entry.Set(key, value);
        }
        checks.Push(std::move(entry));
    }
    root.Set("failed", std::move(failed));
    root.Set("not_run", std::move(notRun));
    root.Set("checks", std::move(checks));
    // DEC36 -- always present, an empty list when nothing was generated, so
    // that "no generated shot" is a reading and not an absence.
    Value generated = Value::MakeArray();
    for (const TimelineGeneratedClip& clip : report.generated) {
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(clip.clip_id));
        entry.Set("track_id", Value::MakeString(clip.track_id));
        entry.Set("media_id", Value::MakeString(clip.media_id));
        entry.Set("timeline_in", TimeValue(clip.timeline_in));
        entry.Set("duration", TimeValue(clip.duration));
        entry.Set("service", Value::MakeString(clip.generation.service));
        entry.Set("model", Value::MakeString(clip.generation.model));
        entry.Set("kind", Value::MakeString(clip.generation.kind));
        entry.Set("created_at", Value::MakeString(clip.generation.created_at));
        generated.Push(std::move(entry));
    }
    root.Set("generated", std::move(generated));
    // Out of lot -- always present too: "nothing disabled" is a reading.
    Value disabled = Value::MakeArray();
    for (const TimelineDisabledClip& clip : report.disabled) {
        Value entry = Value::MakeObject();
        entry.Set("clip_id", Value::MakeString(clip.clip_id));
        entry.Set("track_id", Value::MakeString(clip.track_id));
        entry.Set("timeline_in", TimeValue(clip.timeline_in));
        entry.Set("duration", TimeValue(clip.duration));
        disabled.Push(std::move(entry));
    }
    root.Set("disabled", std::move(disabled));
    // Always present too, whatever the policy: "no jump cut" is a reading.
    Value jumpCuts = Value::MakeArray();
    for (const TimelineJumpCut& jump : report.jump_cuts) {
        Value entry = Value::MakeObject();
        entry.Set("frame", Value::MakeInt(jump.frame));
        entry.Set("timeline_in", TimeValue(jump.timeline_in));
        entry.Set("track_id", Value::MakeString(jump.track_id));
        entry.Set("outgoing_clip_id", Value::MakeString(jump.outgoing_clip_id));
        entry.Set("incoming_clip_id", Value::MakeString(jump.incoming_clip_id));
        entry.Set("source_id", Value::MakeString(jump.source_id));
        jumpCuts.Push(std::move(entry));
    }
    root.Set("jump_cuts", std::move(jumpCuts));
    return root.Dump();
}
