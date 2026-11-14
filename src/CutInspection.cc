#include "CutInspection.h"

#include "Json.h"
#include "Timeline.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <limits>
#include <set>
#include <sstream>

namespace {

// Whole frames of `time` at `rate`, or a refusal: a window edge that falls
// between two frames would be a second answer to where the frame is.
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
// `atrim` and `adelay` use.
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

// RMS over [begin, end) samples of interleaved stereo; empty is silence.
int64_t RmsDbfs(const std::vector<float>& samples, size_t begin, size_t end) {
    if (end <= begin) return kCutInspectionSilenceDbfs;
    double sum = 0.0;
    for (size_t index = begin; index < end; ++index)
        sum += static_cast<double>(samples[index]) * samples[index];
    return Dbfs(std::sqrt(sum / static_cast<double>(end - begin)));
}

mcp_json::Value Time(const RationalTime& time) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("value", mcp_json::Value::MakeInt(time.value));
    value.Set("rate", mcp_json::Value::MakeInt(time.rate));
    return value;
}

mcp_json::Value Word(const CutInspectionWord& word) {
    mcp_json::Value value = mcp_json::Value::MakeObject();
    value.Set("text", mcp_json::Value::MakeString(word.text));
    value.Set("timeline_in", Time(word.timeline_in));
    value.Set("duration", Time(word.duration));
    if (word.crosses_cut)
        value.Set("crosses_cut", mcp_json::Value::MakeBool(true));
    if (word.at_window_edge)
        value.Set("at_window_edge", mcp_json::Value::MakeBool(true));
    return value;
}

}  // namespace

bool PlanCutInspection(const Document& document,
                       const CutInspectionRequest& request,
                       CutInspectionPlan& plan, std::string& error) {
    plan = CutInspectionPlan{};
    error.clear();
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0) {
        error = "the timeline has no frame rate";
        return false;
    }
    plan.timeline_id = document.sequence.id;
    plan.rate = rate;
    if (request.images < kCutInspectionMinImages ||
        request.images > kCutInspectionMaxImages) {
        error = "images must be between " +
                std::to_string(kCutInspectionMinImages) + " and " +
                std::to_string(kCutInspectionMaxImages);
        return false;
    }
    int64_t before = 0;
    int64_t after = 0;
    const RationalTime bound{kCutInspectionMaxSideSeconds, 1};
    if (request.before.value < 0 || request.after.value < 0 ||
        request.before.rate <= 0 || request.after.rate <= 0 ||
        request.before > bound || request.after > bound) {
        error = "before and after must each lie between 0 and " +
                std::to_string(kCutInspectionMaxSideSeconds) + " s";
        return false;
    }
    if (!WholeFrames(request.before, rate, before) ||
        !WholeFrames(request.after, rate, after)) {
        error = "before and after must be whole frames of the timeline (" +
                std::to_string(rate.num) + '/' + std::to_string(rate.den) +
                " fps): a window edge between two frames names no frame";
        return false;
    }
    int64_t total = 0;
    try {
        const RationalTime duration = Timeline(document).Duration();
        if (!WholeFrames(duration, rate, total)) {
            error = "the timeline's duration is not a whole number of frames";
            return false;
        }
    } catch (const std::exception& exception) {
        error = exception.what();
        return false;
    }
    if (total <= 0) {
        error = "the timeline is empty: there is no cut to inspect";
        return false;
    }
    int64_t cut = 0;
    if (!request.clip_id.empty()) {
        const DocumentClip* found = nullptr;
        for (const DocumentTrack& track : document.sequence.tracks) {
            if (track.kind != "video" && track.kind != "audio") continue;
            for (const DocumentClip& clip : track.clips) {
                if (clip.id == request.clip_id) found = &clip;
            }
        }
        if (found == nullptr) {
            error = "clip '" + request.clip_id +
                    "' is on no video or audio track of this timeline";
            return false;
        }
        const RationalTime edge = request.at_tail
                                      ? found->timeline_in.add(found->duration)
                                      : found->timeline_in;
        if (!WholeFrames(edge, rate, cut)) {
            error = "clip '" + request.clip_id +
                    "' does not start or end on a frame of the timeline";
            return false;
        }
    } else if (request.position) {
        if (!WholeFrames(*request.position, rate, cut)) {
            error = "position must be a whole frame of the timeline";
            return false;
        }
        if (cut < 0 || cut > total) {
            error = "position lies outside the timeline's " +
                    std::to_string(total) + " frames";
            return false;
        }
    } else {
        error =
            "name the cut: a clip_id (with at_tail for its end), or a "
            "position";
        return false;
    }
    plan.cut = cut;
    plan.first = cut - before;
    plan.end = cut + after;
    if (plan.first < 0) {
        plan.first = 0;
        plan.clipped_at_start = true;
    }
    if (plan.end > total) {
        plan.end = total;
        plan.clipped_at_end = true;
    }
    if (plan.end <= plan.first) {
        error =
            "the window holds no frame of the timeline: widen `before` "
            "or `after`";
        return false;
    }

    // The pictures: evenly spread over the window, then the two frames on
    // either side of the cut added, which is what a look at a cut is for.
    // Evenly spread frames nearest to the cut give way to them, so the count
    // asked for is kept.
    const int64_t length = plan.end - plan.first;
    const int64_t wanted = std::min<int64_t>(request.images, length);
    std::set<int64_t> spread;
    for (int64_t index = 0; index < wanted; ++index) {
        const int64_t offset =
            wanted == 1
                ? 0
                : ((index * (length - 1)) + ((wanted - 1) / 2)) / (wanted - 1);
        spread.insert(plan.first + offset);
    }
    std::set<int64_t> required;
    if (cut - 1 >= plan.first && cut - 1 < plan.end) required.insert(cut - 1);
    if (cut >= plan.first && cut < plan.end) required.insert(cut);
    std::set<int64_t> shown = required;
    std::vector<int64_t> candidates(spread.begin(), spread.end());
    // Keep the window's own edges first, then the frames farthest from the
    // cut, until the count is reached.
    std::stable_sort(
        candidates.begin(), candidates.end(), [&](int64_t left, int64_t right) {
            const bool leftEdge = left == plan.first || left == plan.end - 1;
            const bool rightEdge = right == plan.first || right == plan.end - 1;
            if (leftEdge != rightEdge) return leftEdge;
            return std::llabs(left - cut) > std::llabs(right - cut);
        });
    for (const int64_t frame : candidates) {
        if (static_cast<int64_t>(shown.size()) >= wanted) break;
        shown.insert(frame);
    }
    plan.shown.assign(shown.begin(), shown.end());
    return true;
}

bool MeasureCutInspectionAudio(const std::vector<float>& samples,
                               int32_t sampleRate,
                               const CutInspectionPlan& plan,
                               CutInspectionAudio& audio, std::string& error) {
    audio = CutInspectionAudio{};
    error.clear();
    if (sampleRate <= 0) {
        error = "the window's sound has no sample rate";
        return false;
    }
    audio.sample_rate = sampleRate;
    const int64_t origin = SampleAt(plan.first, plan.rate, sampleRate);
    const auto offset = [&](int64_t frame) {
        return static_cast<size_t>(SampleAt(frame, plan.rate, sampleRate) -
                                   origin) *
               2;
    };
    const size_t expected = offset(plan.end);
    if (samples.size() < expected) {
        error = "the window's sound holds " +
                std::to_string(samples.size() / 2) + " samples, " +
                std::to_string(expected / 2) + " expected";
        return false;
    }
    double windowPeak = 0.0;
    for (int64_t frame = plan.first; frame < plan.end; ++frame) {
        const size_t begin = offset(frame);
        const size_t end = offset(frame + 1);
        double peak = 0.0;
        for (size_t index = begin; index < end; ++index)
            peak =
                std::max(peak, std::abs(static_cast<double>(samples[index])));
        windowPeak = std::max(windowPeak, peak);
        audio.frame_rms_dbfs.push_back(RmsDbfs(samples, begin, end));
        audio.frame_peak_dbfs.push_back(Dbfs(peak));
    }
    audio.window_peak_dbfs = Dbfs(windowPeak);
    const size_t side = static_cast<size_t>((static_cast<int64_t>(sampleRate) *
                                             kCutInspectionLevelMilliseconds) /
                                            1000) *
                        2;
    const size_t cutAt = offset(std::clamp(plan.cut, plan.first, plan.end));
    if (cutAt > 0) {
        const size_t begin = cutAt > side ? cutAt - side : 0;
        audio.before_cut_rms_dbfs = RmsDbfs(samples, begin, cutAt);
    }
    if (cutAt < expected) {
        audio.after_cut_rms_dbfs =
            RmsDbfs(samples, cutAt, std::min(expected, cutAt + side));
    }
    return true;
}

void CollectExpectedWords(const std::vector<TimelineTranscriptSpan>& spans,
                          const CutInspectionPlan& plan,
                          CutInspectionWords& words) {
    words = CutInspectionWords{};
    const RationalTime start = FrameTime(plan.first, plan.rate);
    const RationalTime stop = FrameTime(plan.end, plan.rate);
    for (const TimelineTranscriptSpan& span : spans) {
        const RationalTime end = span.timeline_in.add(span.duration);
        if (!(span.timeline_in < stop) || !(end > start)) continue;
        CutInspectionWord word;
        word.text = span.text;
        word.timeline_in = span.timeline_in;
        word.duration = span.duration;
        word.crosses_cut = span.straddles_cut;
        word.at_window_edge = span.timeline_in < start || end > stop;
        if (word.crosses_cut) {
            words.alerts.push_back(
                {"word_crosses_cut", word.text, word.timeline_in});
        }
        words.expected.push_back(std::move(word));
    }
}

void CompareHeardWords(const std::vector<CutInspectionWord>& heard,
                       const CutInspectionPlan& plan,
                       CutInspectionWords& words) {
    words.transcribed = true;
    words.heard = heard;
    const RationalTime start = FrameTime(plan.first, plan.rate);
    const RationalTime stop = FrameTime(plan.end, plan.rate);
    // A reading cannot be held to a word the window cuts: a heard word
    // within this of an edge may be the fragment of one.
    const RationalTime margin{1, 4};
    for (CutInspectionWord& word : words.heard) {
        word.at_window_edge =
            word.timeline_in < start.add(margin) ||
            word.timeline_in.add(word.duration) > stop.sub(margin);
    }
    // Nothing to compare against when the project has no transcript: every
    // heard word would come out "not expected", which says nothing.
    if (!words.expected_unavailable.empty()) return;
    // The two readings do not group their words alike -- one writes "à la"
    // then "mode", the other "à la mode"; one "? J'ai", the other "?" then
    // "J'ai" (measured on a synthetic interview). So they are compared token
    // by token, each token keeping the word it came from, and a token that
    // holds no letter or digit -- a lone "?" -- is not a word to miss.
    struct Token {
        std::string text;
        size_t word = 0;
    };
    const auto tokens = [](const std::vector<CutInspectionWord>& list) {
        std::vector<Token> result;
        for (size_t index = 0; index < list.size(); ++index) {
            std::istringstream stream(list[index].text);
            std::string token;
            while (stream >> token) {
                const bool wordLike =
                    std::any_of(token.begin(), token.end(), [](char byte) {
                        const auto value = static_cast<unsigned char>(byte);
                        return std::isalnum(value) != 0 || value >= 0x80;
                    });
                if (wordLike) result.push_back({token, index});
            }
        }
        return result;
    };
    const std::vector<Token> expectedTokens = tokens(words.expected);
    const std::vector<Token> heardTokens = tokens(words.heard);
    const size_t rows = expectedTokens.size();
    const size_t columns = heardTokens.size();
    const auto matches = [&](size_t row, size_t column) {
        return MatchHeardWord(expectedTokens[row].text,
                              heardTokens[column].text) != HeardWordMatch::None;
    };
    // Longest common subsequence under MatchHeardWord, so a token the model
    // wrote with another inflection still pairs with the one expected.
    std::vector<std::vector<int32_t>> table(
        rows + 1, std::vector<int32_t>(columns + 1, 0));
    for (size_t row = rows; row-- > 0;) {
        for (size_t column = columns; column-- > 0;) {
            table[row][column] =
                matches(row, column)
                    ? table[row + 1][column + 1] + 1
                    : std::max(table[row + 1][column], table[row][column + 1]);
        }
    }
    std::vector<bool> expectedMatched(rows, false);
    std::vector<bool> heardMatched(columns, false);
    size_t row = 0;
    size_t column = 0;
    while (row < rows && column < columns) {
        if (matches(row, column) &&
            table[row][column] == table[row + 1][column + 1] + 1) {
            expectedMatched[row] = true;
            heardMatched[column] = true;
            ++row;
            ++column;
        } else if (table[row + 1][column] >= table[row][column + 1]) {
            ++row;
        } else {
            ++column;
        }
    }
    for (size_t index = 0; index < rows; ++index) {
        const CutInspectionWord& word =
            words.expected[expectedTokens[index].word];
        if (expectedMatched[index] || word.at_window_edge) continue;
        words.alerts.push_back({"expected_not_heard",
                                expectedTokens[index].text, word.timeline_in});
    }
    for (size_t index = 0; index < columns; ++index) {
        const CutInspectionWord& word = words.heard[heardTokens[index].word];
        if (heardMatched[index] || word.at_window_edge) continue;
        words.alerts.push_back(
            {"heard_not_expected", heardTokens[index].text, word.timeline_in});
    }
    std::stable_sort(
        words.alerts.begin(), words.alerts.end(),
        [](const CutInspectionAlert& left, const CutInspectionAlert& right) {
            return left.timeline_in < right.timeline_in;
        });
}

std::string SerializeCutInspection(const CutInspectionPlan& plan,
                                   const CutInspectionAudio& audio,
                                   const CutInspectionWords& words) {
    using mcp_json::Value;
    Value result = Value::MakeObject();
    result.Set("timeline_id", Value::MakeString(plan.timeline_id));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(plan.rate.num));
    rate.Set("den", Value::MakeInt(plan.rate.den));
    result.Set("frame_rate", std::move(rate));
    Value window = Value::MakeObject();
    window.Set("first_frame", Value::MakeInt(plan.first));
    window.Set("end_frame", Value::MakeInt(plan.end));
    window.Set("timeline_in", Time(FrameTime(plan.first, plan.rate)));
    window.Set("duration", Time(FrameTime(plan.end - plan.first, plan.rate)));
    if (plan.clipped_at_start)
        window.Set("clipped_at_start", Value::MakeBool(true));
    if (plan.clipped_at_end)
        window.Set("clipped_at_end", Value::MakeBool(true));
    result.Set("window", std::move(window));
    Value cut = Value::MakeObject();
    cut.Set("frame", Value::MakeInt(plan.cut));
    cut.Set("timeline_in", Time(FrameTime(plan.cut, plan.rate)));
    result.Set("cut", std::move(cut));
    Value images = Value::MakeArray();
    for (const int64_t frame : plan.shown) {
        Value image = Value::MakeObject();
        image.Set("frame", Value::MakeInt(frame));
        image.Set("timeline_in", Time(FrameTime(frame, plan.rate)));
        image.Set("role", Value::MakeString(frame == plan.cut - 1 ? "before_cut"
                                            : frame == plan.cut   ? "after_cut"
                                                                  : "window"));
        images.Push(std::move(image));
    }
    result.Set("images", std::move(images));
    Value sound = Value::MakeObject();
    sound.Set("sample_rate", Value::MakeInt(audio.sample_rate));
    sound.Set("window_peak_dbfs", Value::MakeInt(audio.window_peak_dbfs));
    if (audio.before_cut_rms_dbfs)
        sound.Set("before_cut_rms_dbfs",
                  Value::MakeInt(*audio.before_cut_rms_dbfs));
    if (audio.after_cut_rms_dbfs)
        sound.Set("after_cut_rms_dbfs",
                  Value::MakeInt(*audio.after_cut_rms_dbfs));
    Value rms = Value::MakeArray();
    for (const int64_t level : audio.frame_rms_dbfs)
        rms.Push(Value::MakeInt(level));
    sound.Set("frame_rms_dbfs", std::move(rms));
    Value peaks = Value::MakeArray();
    for (const int64_t level : audio.frame_peak_dbfs)
        peaks.Push(Value::MakeInt(level));
    sound.Set("frame_peak_dbfs", std::move(peaks));
    result.Set("sound", std::move(sound));
    Value expected = Value::MakeArray();
    for (const CutInspectionWord& word : words.expected)
        expected.Push(Word(word));
    result.Set("expected_words", std::move(expected));
    if (!words.expected_unavailable.empty())
        result.Set("expected_words_unavailable",
                   Value::MakeString(words.expected_unavailable));
    result.Set("transcribed", Value::MakeBool(words.transcribed));
    if (words.transcribed) {
        Value heard = Value::MakeArray();
        for (const CutInspectionWord& word : words.heard)
            heard.Push(Word(word));
        result.Set("heard_words", std::move(heard));
    }
    Value alerts = Value::MakeArray();
    for (const CutInspectionAlert& alert : words.alerts) {
        Value entry = Value::MakeObject();
        entry.Set("kind", Value::MakeString(alert.kind));
        entry.Set("text", Value::MakeString(alert.text));
        entry.Set("timeline_in", Time(alert.timeline_in));
        alerts.Push(std::move(entry));
    }
    result.Set("alerts", std::move(alerts));
    return result.Dump();
}
