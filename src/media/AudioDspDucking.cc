#include "AudioDspDucking.h"

#include "Json.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

namespace audio_dsp {

namespace {

using mcp_json::Value;

constexpr int64_t kFramesPerSecond = 100;
constexpr double kSilenceDbfs = -100.0;

// `milliseconds` at `rate`, when it is a whole number of samples.
bool ExactSamples(int64_t milliseconds, int32_t rate, int64_t& samples) {
    const int64_t scaled = milliseconds * static_cast<int64_t>(rate);
    if (scaled % 1000 != 0) {
        return false;
    }
    samples = scaled / 1000;
    return true;
}

double Percentile(std::vector<double> values, int64_t percent) {
    const size_t index = static_cast<size_t>(
        (static_cast<int64_t>(values.size()) - 1) * percent / 100);
    std::nth_element(values.begin(),
                     values.begin() + static_cast<std::ptrdiff_t>(index),
                     values.end());
    return values[index];
}

Value Time(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

Value Level(double dbfs) {
    char text[32];
    std::snprintf(text, sizeof(text), "%.2f", dbfs);
    return Value::MakeRawNumber(text);
}

}  // namespace

bool ValidateDuckingSettings(const DuckingSettings& settings, int32_t rate,
                             std::string& error) {
    if (rate <= 0 || rate % kFramesPerSecond != 0) {
        error =
            "the sound's rate must be a whole number of samples every "
            "10 ms: " +
            std::to_string(rate);
        return false;
    }
    const DuckingSettings& s = settings;
    if (s.threshold_db < 1 || s.threshold_db > 60 || s.hysteresis_db < 0 ||
        s.hysteresis_db >= s.threshold_db || s.floor_percentile < 1 ||
        s.floor_percentile > 50 || s.depth_db < 1 || s.depth_db > 60 ||
        s.minimum_speech_milliseconds < 0 ||
        s.minimum_speech_milliseconds > 10000 || s.bridge_milliseconds < 0 ||
        s.bridge_milliseconds > 10000 || s.attack_milliseconds < 0 ||
        s.attack_milliseconds > 10000 || s.release_milliseconds < 0 ||
        s.release_milliseconds > 10000) {
        error =
            "ducking settings out of range: threshold_db 1..60, "
            "hysteresis_db 0..threshold_db - 1, floor_percentile 1..50, "
            "depth_db 1..60, every duration 0..10000 ms";
        return false;
    }
    int64_t minimum = 0;
    int64_t bridge = 0;
    int64_t attack = 0;
    int64_t release = 0;
    if (!ExactSamples(s.minimum_speech_milliseconds, rate, minimum) ||
        !ExactSamples(s.bridge_milliseconds, rate, bridge) ||
        !ExactSamples(s.attack_milliseconds, rate, attack) ||
        !ExactSamples(s.release_milliseconds, rate, release)) {
        error = "a duration is not a whole number of samples at " +
                std::to_string(rate) + " Hz";
        return false;
    }
    return true;
}

bool ProposeDucking(const std::vector<float>& samples, int32_t rate,
                    const DuckingSettings& settings, DuckingProposal& proposal,
                    std::string& code, std::string& error) {
    proposal = DuckingProposal{};
    code = "InvalidInput";
    if (!ValidateDuckingSettings(settings, rate, error)) return false;
    const DuckingSettings& s = settings;
    int64_t minimum = 0;
    int64_t bridge = 0;
    int64_t attack = 0;
    int64_t release = 0;
    ExactSamples(s.minimum_speech_milliseconds, rate, minimum);
    ExactSamples(s.bridge_milliseconds, rate, bridge);
    ExactSamples(s.attack_milliseconds, rate, attack);
    ExactSamples(s.release_milliseconds, rate, release);
    code.clear();
    const auto length = static_cast<int64_t>(samples.size());
    const int64_t hop = rate / kFramesPerSecond;
    proposal.sample_rate = rate;
    proposal.duration = RationalTime(length, rate);

    // Each frame's RMS level; the last one may be short.
    std::vector<double> levels;
    for (int64_t start = 0; start < length; start += hop) {
        const int64_t end = std::min(length, start + hop);
        double sum = 0.0;
        for (int64_t n = start; n < end; ++n) {
            const double x = samples[static_cast<size_t>(n)];
            sum += x * x;
        }
        const double power = sum / static_cast<double>(end - start);
        levels.push_back(power > 1e-12 ? 10.0 * std::log10(power) : -120.0);
    }
    std::vector<double> heard;
    for (const double level : levels) {
        if (level > kSilenceDbfs) {
            heard.push_back(level);
        }
    }
    if (heard.empty()) {
        proposal.silent = true;
        return true;
    }
    proposal.floor_dbfs = Percentile(heard, s.floor_percentile);
    proposal.speech_level_dbfs = Percentile(heard, 95);
    proposal.enter_dbfs =
        proposal.floor_dbfs + static_cast<double>(s.threshold_db);
    proposal.leave_dbfs =
        proposal.enter_dbfs - static_cast<double>(s.hysteresis_db);

    // Frame ranges with hysteresis, in samples.
    std::vector<std::pair<int64_t, int64_t>> ranges;
    bool speaking = false;
    int64_t opened = 0;
    for (size_t frame = 0; frame < levels.size(); ++frame) {
        const double level = levels[frame];
        const int64_t at = static_cast<int64_t>(frame) * hop;
        if (!speaking && level > proposal.enter_dbfs) {
            speaking = true;
            opened = at;
        } else if (speaking && level < proposal.leave_dbfs) {
            speaking = false;
            ranges.emplace_back(opened, at);
        }
    }
    if (speaking) {
        ranges.emplace_back(opened, length);
    }
    // Short gaps closed first, then short ranges dropped.
    std::vector<std::pair<int64_t, int64_t>> merged;
    for (const auto& range : ranges) {
        if (!merged.empty() && range.first - merged.back().second < bridge) {
            merged.back().second = range.second;
        } else {
            merged.push_back(range);
        }
    }
    for (const auto& [start, end] : merged) {
        if (end - start >= minimum) {
            proposal.speech.push_back(
                {RationalTime(start, rate), RationalTime(end, rate)});
        }
    }

    // The curve: ranges whose ramps meet are ducked as one.
    std::vector<std::pair<int64_t, int64_t>> groups;
    for (const DuckingRange& range : proposal.speech) {
        const int64_t start = range.start.value;
        const int64_t end = range.end.value;
        if (!groups.empty() &&
            start - attack <= groups.back().second + release) {
            groups.back().second = end;
        } else {
            groups.emplace_back(start, end);
        }
    }
    const int64_t down = -s.depth_db;
    const auto push = [&](int64_t at, int64_t gain) {
        if (!proposal.curve.empty() && proposal.curve.back().time.value == at) {
            proposal.curve.back().gain_db = gain;
            return;
        }
        proposal.curve.push_back({RationalTime(at, rate), gain});
    };
    for (const auto& [start, end] : groups) {
        const int64_t rampStart = std::max<int64_t>(0, start - attack);
        if (rampStart < start) {
            push(rampStart, 0);
        }
        push(start, down);
        if (end > start) {
            push(end, down);
        }
        const int64_t rampEnd = std::min(length, end + release);
        if (rampEnd > end) {
            push(rampEnd, 0);
        }
    }
    return true;
}

std::string DuckingProposalJson(const DuckingProposal& proposal,
                                const DuckingSettings& settings) {
    Value result = Value::MakeObject();
    result.Set("sample_rate", Value::MakeInt(proposal.sample_rate));
    result.Set("duration", Time(proposal.duration));
    result.Set("silent", Value::MakeBool(proposal.silent));
    Value levels = Value::MakeObject();
    levels.Set("floor_dbfs", Level(proposal.floor_dbfs));
    levels.Set("speech_dbfs", Level(proposal.speech_level_dbfs));
    levels.Set("enter_dbfs", Level(proposal.enter_dbfs));
    levels.Set("leave_dbfs", Level(proposal.leave_dbfs));
    result.Set("levels", std::move(levels));
    Value used = Value::MakeObject();
    used.Set("threshold_db", Value::MakeInt(settings.threshold_db));
    used.Set("hysteresis_db", Value::MakeInt(settings.hysteresis_db));
    used.Set("floor_percentile", Value::MakeInt(settings.floor_percentile));
    used.Set("minimum_speech_ms",
             Value::MakeInt(settings.minimum_speech_milliseconds));
    used.Set("bridge_ms", Value::MakeInt(settings.bridge_milliseconds));
    used.Set("depth_db", Value::MakeInt(settings.depth_db));
    used.Set("attack_ms", Value::MakeInt(settings.attack_milliseconds));
    used.Set("release_ms", Value::MakeInt(settings.release_milliseconds));
    result.Set("settings", std::move(used));
    Value speech = Value::MakeArray();
    for (const DuckingRange& range : proposal.speech) {
        Value item = Value::MakeObject();
        item.Set("start", Time(range.start));
        item.Set("end", Time(range.end));
        speech.Push(std::move(item));
    }
    result.Set("speech", std::move(speech));
    Value curve = Value::MakeArray();
    for (const DuckingPoint& point : proposal.curve) {
        Value item = Value::MakeObject();
        item.Set("time", Time(point.time));
        item.Set("gain_db", Value::MakeInt(point.gain_db));
        curve.Push(std::move(item));
    }
    result.Set("gain_curve", std::move(curve));
    // Nothing is written: the clip gain cannot be animated yet.
    result.Set("applied", Value::MakeBool(false));
    return result.Dump();
}

}  // namespace audio_dsp
