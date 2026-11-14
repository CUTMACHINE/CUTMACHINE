#include "TimelineAudioReport.h"

#include "ClipEnabled.h"
#include "FfmpegPipe.h"
#include "Json.h"
#include "Timeline.h"
#include "TimelineTranscription.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <locale>
#include <sstream>

namespace {

using mcp_json::Value;

constexpr int64_t kWindowsPerSecond = 100;
constexpr int64_t kEnvelopeRate = 16000;
constexpr int64_t kSamplesPerWindow = kEnvelopeRate / kWindowsPerSecond;

int64_t Dbfs(double amplitude) {
    if (amplitude <= 1e-6) return -120;
    return static_cast<int64_t>(std::lround(20.0 * std::log10(amplitude)));
}

// ebur128 prints its summary at info level; the value follows `label` in
// the section that starts with `section`.
std::optional<double> SummaryValue(const std::string& text,
                                   const std::string& section,
                                   const std::string& label) {
    const size_t start = text.rfind(section);
    if (start == std::string::npos) return std::nullopt;
    const size_t found = text.find(label, start + section.size());
    if (found == std::string::npos) return std::nullopt;
    std::istringstream input(text.substr(found + label.size(), 32));
    input.imbue(std::locale::classic());
    double value = 0.0;
    if (!(input >> value)) return std::nullopt;
    return value;
}

struct AudibleInterval {
    RationalTime start;
    RationalTime end;
    Ulid source_id;
    std::optional<int64_t> threshold_dbfs;
};

Value TimeValue(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

}  // namespace

bool AnalyzeTimelineAudio(const Document& document,
                          const std::filesystem::path& projectPath,
                          const std::map<Ulid, SpeechOnsetReport>& floors,
                          const TimelineAudioSettings& settings,
                          TimelineAudioReport& report, std::string& error) {
    report = {};
    error.clear();
    TimelineAudioPlan plan;
    if (!BuildTimelineAudioPlan(document, projectPath, plan, error))
        return false;
    report.duration = plan.duration;

    // Which rushes play when, with the floor each one brings.
    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::vector<AudibleInterval> intervals;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip is not in the mix (ClipEnabled.h).
            if (!ClipIsHeard(clip)) continue;
            const LibraryMedia* media =
                document.FindLibraryMedia(clip.source_id);
            if (media != nullptr && media->metadata_complete &&
                !media->has_audio)
                continue;
            AudibleInterval interval;
            interval.start = clip.timeline_in;
            interval.end = clip.timeline_in.add(clip.duration);
            interval.source_id = clip.source_id;
            const auto found = floors.find(clip.source_id);
            if (found != floors.end() && found->second.noise_floor > 0 &&
                found->second.speech_level > found->second.noise_floor) {
                const double gain =
                    clip.audio_gain_db.den != 0
                        ? static_cast<double>(clip.audio_gain_db.num) /
                              clip.audio_gain_db.den
                        : 0.0;
                const double floor =
                    20.0 *
                    std::log10(static_cast<double>(found->second.noise_floor) /
                               kSpeechLevelScale);
                const double speech =
                    20.0 *
                    std::log10(static_cast<double>(found->second.speech_level) /
                               kSpeechLevelScale);
                interval.threshold_dbfs = static_cast<int64_t>(std::lround(
                    floor +
                    ((speech - floor) * settings.dip_position_percent / 100.0) +
                    gain));
            } else if (std::find(report.sources_without_floor.begin(),
                                 report.sources_without_floor.end(),
                                 clip.source_id) ==
                       report.sources_without_floor.end()) {
                report.sources_without_floor.push_back(clip.source_id);
            }
            intervals.push_back(std::move(interval));
        }
    }

    std::vector<std::string> arguments = plan.input_arguments;
    for (size_t index = 0; index + 1 < arguments.size(); ++index) {
        if (arguments[index] == "-loglevel") arguments[index + 1] = "info";
    }
    // The plan's arguments start after the executable.
    arguments.insert(arguments.begin(), settings.ffmpeg_path);
    arguments.insert(
        arguments.end(),
        {"-nostats", "-filter_complex",
         plan.filter_graph +
             ";[audio]asplit=2[envelope_in][loudness_in];"
             "[envelope_in]aresample=" +
             std::to_string(kEnvelopeRate) +
             ",aformat=sample_fmts=flt:channel_layouts=stereo[envelope];"
             "[loudness_in]ebur128=peak=true:framelog=quiet[loudness]",
         "-map", "[envelope]", "-f", "f32le", "pipe:1", "-map", "[loudness]",
         "-f", "null", "-"});

    std::vector<int64_t> levels;
    double headSquares = 0.0;
    double headPeak = 0.0;
    int64_t headSamples = 0;
    const int64_t headLimit =
        (settings.head_milliseconds * kEnvelopeRate) / 1000;
    std::string diagnostics;
    // Stereo in. Hollows are judged on the mono FFmpeg makes with -ac 1 --
    // each channel at -3 dB, summed -- because that is how the speech onset
    // analysis read the rushes whose floors they are compared with. The head
    // is measured per channel, as a meter would.
    constexpr double kMonoCoefficient = 0.7071067811865476;
    const bool read = ReadFfmpegRawFrames(
        arguments, static_cast<size_t>(kSamplesPerWindow) * 2 * sizeof(float),
        [&](const uint8_t* bytes) {
            double squares = 0.0;
            for (int64_t sample = 0; sample < kSamplesPerWindow; ++sample) {
                float left = 0.0f;
                float right = 0.0f;
                std::memcpy(&left, bytes + (2 * sample * sizeof(float)),
                            sizeof(float));
                std::memcpy(&right,
                            bytes + (((2 * sample) + 1) * sizeof(float)),
                            sizeof(float));
                const double mono = kMonoCoefficient * (left + right);
                squares += mono * mono;
                if (headSamples < headLimit) {
                    headSquares += (static_cast<double>(left) * left +
                                    static_cast<double>(right) * right) /
                                   2.0;
                    headPeak = std::max(
                        {headPeak, static_cast<double>(std::fabs(left)),
                         static_cast<double>(std::fabs(right))});
                    ++headSamples;
                }
            }
            levels.push_back(Dbfs(std::sqrt(squares / kSamplesPerWindow)));
        },
        error, &diagnostics);
    if (!read) {
        error = "unable to assemble the timeline audio: " + error;
        return false;
    }
    report.integrated_lufs =
        SummaryValue(diagnostics, "Integrated loudness:", "I:");
    report.loudness_range_lu =
        SummaryValue(diagnostics, "Loudness range:", "LRA:");
    report.true_peak_dbfs = SummaryValue(diagnostics, "True peak:", "Peak:");
    report.windows = static_cast<int64_t>(levels.size());
    if (headSamples > 0) {
        report.head_level_dbfs = Dbfs(std::sqrt(headSquares / headSamples));
        report.head_peak_dbfs = Dbfs(headPeak);
    }

    // Per window: a gap, a hollow under its floor, voiced, or unmeasured.
    enum class State { Gap, Hollow, Voiced, Unmeasured };
    std::vector<State> states(levels.size(), State::Unmeasured);
    std::vector<std::optional<int64_t>> windowFloors(levels.size());
    for (size_t window = 0; window < levels.size(); ++window) {
        const RationalTime centre{static_cast<int64_t>(2 * window + 1),
                                  static_cast<int32_t>(2 * kWindowsPerSecond)};
        bool any = false;
        bool unmeasured = false;
        std::optional<int64_t> floor;
        for (const AudibleInterval& interval : intervals) {
            if (centre < interval.start || !(centre < interval.end)) continue;
            any = true;
            if (!interval.threshold_dbfs) {
                unmeasured = true;
            } else {
                floor = floor ? std::max(*floor, *interval.threshold_dbfs)
                              : *interval.threshold_dbfs;
            }
        }
        windowFloors[window] = floor;
        if (!any) {
            states[window] = State::Gap;
        } else if (unmeasured) {
            states[window] = State::Unmeasured;
            ++report.unmeasured_windows;
        } else {
            states[window] =
                levels[window] < *floor ? State::Hollow : State::Voiced;
        }
    }

    const int64_t minimumWindows = std::max<int64_t>(
        1, (settings.minimum_dip_milliseconds * kWindowsPerSecond) / 1000);
    for (size_t begin = 0; begin < states.size();) {
        const State state = states[begin];
        size_t end = begin + 1;
        while (end < states.size() && states[end] == state) ++end;
        if ((state == State::Gap || state == State::Hollow) &&
            static_cast<int64_t>(end - begin) >= minimumWindows) {
            TimelineAudioDip dip;
            dip.kind = state == State::Gap ? "gap" : "hollow";
            dip.start = {static_cast<int64_t>(begin),
                         static_cast<int32_t>(kWindowsPerSecond)};
            dip.end = {static_cast<int64_t>(end),
                       static_cast<int32_t>(kWindowsPerSecond)};
            double sum = 0.0;
            for (size_t window = begin; window < end; ++window) {
                sum += levels[window];
                if (windowFloors[window])
                    dip.threshold_dbfs = dip.threshold_dbfs
                                             ? std::max(*dip.threshold_dbfs,
                                                        *windowFloors[window])
                                             : *windowFloors[window];
            }
            dip.level_dbfs = static_cast<int64_t>(
                std::lround(sum / static_cast<double>(end - begin)));
            const RationalTime length = dip.end.sub(dip.start);
            if (report.longest_dip < length) report.longest_dip = length;
            report.dips.push_back(std::move(dip));
        }
        begin = end;
    }

    // Head air: the first 100 ms run of voiced windows.
    constexpr size_t kSustainWindows = 10;
    size_t run = 0;
    for (size_t window = 0; window < states.size(); ++window) {
        if (states[window] == State::Unmeasured) break;
        run = states[window] == State::Voiced ? run + 1 : 0;
        if (run == kSustainWindows) {
            report.head_air =
                RationalTime{static_cast<int64_t>(window + 1 - kSustainWindows),
                             static_cast<int32_t>(kWindowsPerSecond)};
            break;
        }
    }
    return true;
}

bool MeasureFileLoudness(const std::string& path, const std::string& ffmpegPath,
                         FileLoudness& loudness, std::string& error) {
    loudness = {};
    error.clear();
    std::string diagnostics;
    // Nothing is written to the pipe: the meter's summary is on the error
    // output, which the reader hands back.
    const bool read = ReadFfmpegRawFrames(
        {ffmpegPath, "-hide_banner", "-nostdin", "-nostats", "-loglevel",
         "info", "-i", path, "-map", "0:a:0", "-af",
         "ebur128=peak=true:framelog=quiet", "-f", "null", "-"},
        1, [](const uint8_t*) {}, error, &diagnostics);
    if (!read) {
        error = "unable to measure the loudness of '" + path + "': " + error;
        return false;
    }
    loudness.integrated_lufs =
        SummaryValue(diagnostics, "Integrated loudness:", "I:");
    loudness.loudness_range_lu =
        SummaryValue(diagnostics, "Loudness range:", "LRA:");
    loudness.true_peak_dbfs = SummaryValue(diagnostics, "True peak:", "Peak:");
    // The meter prints its absolute gate, -70 LUFS, for a signal no block
    // of which passes it: silence, not a loudness.
    if (loudness.integrated_lufs && *loudness.integrated_lufs <= -70.0)
        loudness.integrated_lufs.reset();
    return true;
}

std::string SerializeTimelineAudioReport(const TimelineAudioReport& report,
                                         MediaRate rate) {
    const auto frames = [&](const RationalTime& time) {
        return time.to_frames(rate.num, rate.den);
    };
    const auto optionalNumber = [](const std::optional<double>& value) {
        return value ? Value::MakeDouble(std::round(*value * 10.0) / 10.0)
                     : Value::MakeNull();
    };
    Value root = Value::MakeObject();
    root.Set("duration", TimeValue(report.duration));
    root.Set("integrated_lufs", optionalNumber(report.integrated_lufs));
    root.Set("true_peak_dbfs", optionalNumber(report.true_peak_dbfs));
    root.Set("loudness_range_lu", optionalNumber(report.loudness_range_lu));
    Value head = Value::MakeObject();
    head.Set("level_dbfs", Value::MakeInt(report.head_level_dbfs));
    head.Set("peak_dbfs", Value::MakeInt(report.head_peak_dbfs));
    if (report.head_air) {
        head.Set("air", TimeValue(*report.head_air));
        head.Set("air_frames", Value::MakeInt(frames(*report.head_air)));
    } else {
        head.Set("air", Value::MakeNull());
    }
    root.Set("head", std::move(head));
    Value dips = Value::MakeArray();
    for (const TimelineAudioDip& dip : report.dips) {
        Value item = Value::MakeObject();
        item.Set("kind", Value::MakeString(dip.kind));
        item.Set("start", TimeValue(dip.start));
        item.Set("end", TimeValue(dip.end));
        item.Set("start_frame", Value::MakeInt(frames(dip.start)));
        item.Set("end_frame", Value::MakeInt(frames(dip.end)));
        item.Set("level_dbfs", Value::MakeInt(dip.level_dbfs));
        item.Set("threshold_dbfs", dip.threshold_dbfs
                                       ? Value::MakeInt(*dip.threshold_dbfs)
                                       : Value::MakeNull());
        dips.Push(std::move(item));
    }
    root.Set("dips", std::move(dips));
    root.Set("longest_dip", TimeValue(report.longest_dip));
    root.Set("windows", Value::MakeInt(report.windows));
    root.Set("unmeasured_windows", Value::MakeInt(report.unmeasured_windows));
    Value missing = Value::MakeArray();
    for (const Ulid& source : report.sources_without_floor)
        missing.Push(Value::MakeString(source));
    root.Set("sources_without_floor", std::move(missing));
    return root.Dump();
}
