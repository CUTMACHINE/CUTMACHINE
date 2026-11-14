#include "Hesitations.h"

#include "FfmpegPipe.h"
#include "Fft.h"
#include "Json.h"
#include "Ulid.h"
#include "WordAlignment.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>

namespace {

using mcp_json::Value;

constexpr int32_t kSampleRate = 16000;
constexpr int32_t kHop = 160;    // 10 ms
constexpr int32_t kFrame = 400;  // 25 ms
constexpr int32_t kFftSize = 512;
constexpr int32_t kMelBands = 128;
constexpr int32_t kCoefficients = 20;  // c1..c19 compared; c0 is loudness.
constexpr int32_t kContextSteps = 5;   // +/-50 ms

double HzToMel(double hz) { return 2595.0 * std::log10(1.0 + hz / 700.0); }
double MelToHz(double mel) {
    return 700.0 * (std::pow(10.0, mel / 2595.0) - 1.0);
}

// Triangular mel filters over the power spectrum bins, 0 to 8 kHz.
const std::vector<std::vector<double>>& MelFilters() {
    static const std::vector<std::vector<double>> filters = [] {
        const int32_t bins = kFftSize / 2 + 1;
        std::vector<double> centres(kMelBands + 2);
        const double top = HzToMel(kSampleRate / 2.0);
        for (int32_t index = 0; index < kMelBands + 2; ++index)
            centres[index] = MelToHz(top * index / (kMelBands + 1));
        std::vector<std::vector<double>> result(kMelBands,
                                                std::vector<double>(bins, 0.0));
        for (int32_t band = 0; band < kMelBands; ++band) {
            for (int32_t bin = 0; bin < bins; ++bin) {
                const double hz =
                    static_cast<double>(bin) * kSampleRate / kFftSize;
                const double rising =
                    (hz - centres[band]) / (centres[band + 1] - centres[band]);
                const double falling = (centres[band + 2] - hz) /
                                       (centres[band + 2] - centres[band + 1]);
                result[band][bin] = std::max(0.0, std::min(rising, falling));
            }
        }
        return result;
    }();
    return filters;
}

int64_t Percentile(std::vector<double> values, int percent) {
    if (values.empty()) return -120;
    std::sort(values.begin(), values.end());
    const size_t index =
        std::min(values.size() - 1, values.size() * percent / 100);
    return static_cast<int64_t>(std::lround(values[index]));
}

struct Run {
    size_t begin;
    size_t end;
};

// Runs of set flags at least `minimum` long. A gap of at most `bridge`
// unset windows between two set ones does not end a run (A48: a held vowel
// whose stability flickers for 30 ms is one hold, not two).
std::vector<Run> Runs(const std::vector<bool>& flags, size_t minimum,
                      size_t bridge) {
    std::vector<Run> runs;
    for (size_t begin = 0; begin < flags.size();) {
        if (!flags[begin]) {
            ++begin;
            continue;
        }
        size_t end = begin;
        while (end < flags.size()) {
            if (flags[end]) {
                ++end;
                continue;
            }
            size_t next = end;
            while (next < flags.size() && !flags[next] && next - end < bridge)
                ++next;
            if (next < flags.size() && flags[next] && next - end <= bridge) {
                end = next;
                continue;
            }
            break;
        }
        if (end - begin >= minimum) runs.push_back({begin, end});
        begin = end;
    }
    return runs;
}

RationalTime WindowTime(size_t window) {
    return {static_cast<int64_t>(window), 100};
}

bool ReadIntArray(const Value* array, std::vector<int64_t>& output) {
    if (array == nullptr || !array->IsArray()) return false;
    output.clear();
    for (const Value& item : array->AsArray()) {
        int64_t value = 0;
        if (!item.AsInt64(value)) return false;
        output.push_back(value);
    }
    return true;
}

}  // namespace

void MeasureTimbreStability(const std::vector<float>& samples,
                            std::vector<double>& stability,
                            std::vector<double>& levelDb) {
    stability.clear();
    levelDb.clear();
    if (samples.size() < kFrame) return;
    const size_t frames = (samples.size() - kFrame) / kHop + 1;
    std::vector<double> window(kFrame);
    for (int32_t index = 0; index < kFrame; ++index)
        window[index] =
            0.5 - 0.5 * std::cos(2.0 * fft::kPi * index / (kFrame - 1));
    const auto& filters = MelFilters();
    std::vector<std::vector<double>> melDb(frames,
                                           std::vector<double>(kMelBands));
    double loudest = -std::numeric_limits<double>::infinity();
    levelDb.resize(frames);
    std::vector<std::complex<double>> spectrum(kFftSize);
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t offset = frame * kHop;
        double squares = 0.0;
        std::fill(spectrum.begin(), spectrum.end(), 0.0);
        for (int32_t index = 0; index < kFrame; ++index) {
            const double sample = samples[offset + index];
            squares += sample * sample;
            spectrum[index] = sample * window[index];
        }
        const double rms = std::sqrt(squares / kFrame);
        levelDb[frame] = rms > 1e-6 ? 20.0 * std::log10(rms) : -120.0;
        fft::Forward(spectrum);
        for (int32_t band = 0; band < kMelBands; ++band) {
            double energy = 0.0;
            for (int32_t bin = 0; bin <= kFftSize / 2; ++bin) {
                if (filters[band][bin] == 0.0) continue;
                energy += filters[band][bin] * std::norm(spectrum[bin]);
            }
            const double db = 10.0 * std::log10(std::max(energy, 1e-10));
            melDb[frame][band] = db;
            loudest = std::max(loudest, db);
        }
    }
    // As a power-to-dB conversion does: nothing more than 80 dB under the
    // loudest band, so silence does not dominate the coefficients.
    const double floor = loudest - 80.0;
    std::vector<std::vector<double>> coefficients(
        frames, std::vector<double>(kCoefficients - 1));
    for (size_t frame = 0; frame < frames; ++frame) {
        for (double& db : melDb[frame]) db = std::max(db, floor);
        for (int32_t k = 1; k < kCoefficients; ++k) {
            double sum = 0.0;
            for (int32_t band = 0; band < kMelBands; ++band)
                sum += melDb[frame][band] *
                       std::cos(fft::kPi * k * (2.0 * band + 1.0) /
                                (2.0 * kMelBands));
            coefficients[frame][k - 1] = sum * std::sqrt(2.0 / kMelBands);
        }
    }
    std::vector<double> steps(frames, 0.0);
    for (size_t frame = 0; frame + 1 < frames; ++frame) {
        double sum = 0.0;
        for (int32_t k = 0; k < kCoefficients - 1; ++k) {
            const double delta =
                coefficients[frame + 1][k] - coefficients[frame][k];
            sum += delta * delta;
        }
        steps[frame] = std::sqrt(sum);
    }
    stability.assign(frames, 0.0);
    for (size_t frame = 0; frame < frames; ++frame) {
        const size_t first = frame >= kContextSteps ? frame - kContextSteps : 0;
        const size_t last = std::min(frames - 1, frame + kContextSteps);
        double sum = 0.0;
        size_t count = 0;
        for (size_t step = first; step < last; ++step) {
            sum += steps[step];
            ++count;
        }
        stability[frame] = count > 0 ? sum / count : 0.0;
    }
}

void DetectHesitations(const std::vector<double>& stability,
                       const std::vector<double>& levelDb,
                       const std::vector<TranscriptWord>& words,
                       const HesitationSettings& settings,
                       HesitationReport& report) {
    report.windows_per_second = 100;
    report.candidates.clear();
    std::vector<double> audible;
    for (const double level : levelDb)
        if (level > -100.0) audible.push_back(level);
    report.speech_level_db = Percentile(audible, 90);
    report.floor_level_db = Percentile(audible, 5);
    const size_t windows = std::min(stability.size(), levelDb.size());
    report.stability_milli.assign(windows, 0);
    report.level_db.assign(windows, -120);

    // Seconds of source per window, compared exactly against word bounds.
    const auto insideWord = [&](size_t window) -> int64_t {
        const RationalTime time{static_cast<int64_t>(2 * window + 1), 200};
        for (size_t index = 0; index < words.size(); ++index) {
            if (!(time < words[index].start) && time < words[index].end)
                return static_cast<int64_t>(index);
        }
        return -1;
    };
    // The rush's own speech sets the scale of "held".
    std::vector<double> voicedStability;
    for (size_t window = 0; window < windows; ++window) {
        if (report.speech_level_db - levelDb[window] <=
            settings.held_level_range_db)
            voicedStability.push_back(stability[window]);
    }
    std::sort(voicedStability.begin(), voicedStability.end());
    const double median = voicedStability.empty()
                              ? 0.0
                              : voicedStability[voicedStability.size() / 2];
    report.median_stability_milli =
        static_cast<int64_t>(std::lround(median * 1000.0));
    const double heldStability =
        median * settings.held_stability_percent / 100.0;
    // A murmur is quieter and breathier: allowed a little more movement.
    const double murmurStability = median * 0.9;
    std::vector<bool> held(windows, false);
    std::vector<bool> quiet(windows, false);
    std::vector<int64_t> wordOf(windows, -1);
    for (size_t window = 0; window < windows; ++window) {
        const double below = report.speech_level_db - levelDb[window];
        const bool voiced = below <= settings.held_level_range_db;
        report.level_db[window] =
            static_cast<int64_t>(std::lround(levelDb[window]));
        if (voiced)
            report.stability_milli[window] =
                static_cast<int64_t>(std::lround(stability[window] * 1000.0));
        wordOf[window] = words.empty() ? -1 : insideWord(window);
        held[window] = voiced && stability[window] < heldStability;
        quiet[window] = !words.empty() && wordOf[window] < 0 &&
                        below >= settings.murmur_min_db_below_speech &&
                        below <= settings.murmur_max_db_below_speech &&
                        levelDb[window] >= report.floor_level_db + 6.0 &&
                        stability[window] < murmurStability;
    }
    const auto neighbours = [&](size_t begin, size_t end,
                                HesitationCandidate& candidate) {
        const RationalTime start = WindowTime(begin);
        const RationalTime stop = WindowTime(end);
        // The word under way when the hesitation starts, or the last one
        // before it; the first one starting after it ends.
        for (const TranscriptWord& word : words) {
            if (word.start < start) candidate.previous_word = word.text;
            if (!(word.start < stop) && candidate.next_word.empty())
                candidate.next_word = word.text;
        }
    };
    const auto summarise = [&](const Run& run, HesitationCandidate& candidate) {
        candidate.start = WindowTime(run.begin);
        candidate.end = WindowTime(run.end);
        double stable = 0.0;
        double level = 0.0;
        for (size_t window = run.begin; window < run.end; ++window) {
            stable += stability[window];
            level += levelDb[window];
        }
        const double count = static_cast<double>(run.end - run.begin);
        candidate.stability_milli =
            static_cast<int64_t>(std::lround(stable / count * 1000.0));
        candidate.level_db_below_speech = static_cast<int64_t>(
            std::lround(report.speech_level_db - level / count));
    };
    // Runs are formed at the lower floor and a stretched syllable (or a hold
    // with no words to place it) is kept only from the higher one: the
    // family decides which floor applies (A48).
    const size_t insideWordWindows =
        static_cast<size_t>(settings.minimum_held_milliseconds / 10);
    const size_t betweenWordsWindows =
        static_cast<size_t>(settings.minimum_gap_held_milliseconds / 10);
    const size_t bridgeWindows =
        static_cast<size_t>(settings.bridge_milliseconds / 10);
    for (const Run& run :
         Runs(held,
              words.empty() ? insideWordWindows
                            : std::min(insideWordWindows, betweenWordsWindows),
              bridgeWindows)) {
        HesitationCandidate candidate;
        summarise(run, candidate);
        const size_t length = run.end - run.begin;
        if (words.empty()) {
            candidate.family = "held";
        } else {
            // Most windows inside one word: that word is stretched.
            std::map<int64_t, size_t> counts;
            for (size_t window = run.begin; window < run.end; ++window)
                ++counts[wordOf[window]];
            int64_t dominant = -1;
            size_t best = 0;
            for (const auto& [word, count] : counts) {
                if (count > best) {
                    best = count;
                    dominant = word;
                }
            }
            // A word shorter than the floor cannot hold a stretched
            // syllable: a hold covering it whole is the word said on one
            // steady vowel ("donc," of four frames under a five-frame
            // hold), placed as a filler so the cut stops at its start.
            const bool shortWord =
                dominant >= 0 &&
                words[dominant].end.sub(words[dominant].start) <
                    RationalTime{static_cast<int64_t>(insideWordWindows), 100};
            if (dominant >= 0 && best * 2 >= length && !shortWord) {
                if (length < insideWordWindows) continue;
                candidate.family = "stretched_syllable";
                candidate.within_word = words[dominant].text;
                if (dominant > 0)
                    candidate.previous_word = words[dominant - 1].text;
                if (static_cast<size_t>(dominant + 1) < words.size())
                    candidate.next_word = words[dominant + 1].text;
            } else {
                if (length < betweenWordsWindows) continue;
                candidate.family = "filler";
                neighbours(run.begin, run.end, candidate);
            }
        }
        report.candidates.push_back(std::move(candidate));
    }
    for (const Run& run :
         Runs(quiet,
              static_cast<size_t>(settings.minimum_murmur_milliseconds / 10),
              bridgeWindows)) {
        HesitationCandidate candidate;
        summarise(run, candidate);
        candidate.family = "murmur";
        neighbours(run.begin, run.end, candidate);
        report.candidates.push_back(std::move(candidate));
    }
    std::stable_sort(
        report.candidates.begin(), report.candidates.end(),
        [](const HesitationCandidate& left, const HesitationCandidate& right) {
            return left.start < right.start;
        });
}

bool GenerateHesitationReport(const std::string& inputPath,
                              const std::string& outputPath,
                              const std::string& mediaId,
                              const std::vector<TranscriptWord>& words,
                              const HesitationSettings& settings,
                              HesitationReport& report, std::string& error) {
    report = {};
    report.media_id = mediaId;
    std::vector<float> samples;
    constexpr size_t kChunk = 1600;
    if (!ReadFfmpegRawFrames(
            {settings.ffmpeg_path, "-hide_banner", "-loglevel", "error",
             "-nostdin", "-i", inputPath, "-map", "0:a:0", "-vn", "-sn", "-ac",
             "1", "-ar", std::to_string(kSampleRate), "-f", "f32le", "pipe:1"},
            kChunk * sizeof(float),
            [&](const uint8_t* bytes) {
                const size_t offset = samples.size();
                samples.resize(offset + kChunk);
                std::memcpy(samples.data() + offset, bytes,
                            kChunk * sizeof(float));
            },
            error)) {
        error = "unable to decode audio for hesitations: " + error;
        return false;
    }
    std::vector<double> stability;
    std::vector<double> levels;
    MeasureTimbreStability(samples, stability, levels);
    DetectHesitations(stability, levels, words, settings, report);
    std::error_code filesystemError;
    const std::filesystem::path destination(outputPath);
    std::filesystem::create_directories(destination.parent_path(),
                                        filesystemError);
    const std::filesystem::path temporary =
        destination.parent_path() /
        (destination.stem().string() + ".partial-" + GenerateUlid() + ".json");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file << SerializeHesitationReport(report, true);
        if (!file) {
            std::filesystem::remove(temporary, filesystemError);
            error = "unable to write the hesitation cache";
            return false;
        }
    }
    std::filesystem::rename(temporary, destination, filesystemError);
    if (filesystemError) {
        std::filesystem::remove(temporary, filesystemError);
        error = "unable to publish the hesitation cache";
        return false;
    }
    return true;
}

std::string SerializeHesitationReport(const HesitationReport& report,
                                      bool includeWindows) {
    Value root = Value::MakeObject();
    root.Set("version", Value::MakeInt(1));
    root.Set("media_id", Value::MakeString(report.media_id));
    root.Set("windows_per_second", Value::MakeInt(report.windows_per_second));
    root.Set("speech_level_db", Value::MakeInt(report.speech_level_db));
    root.Set("floor_level_db", Value::MakeInt(report.floor_level_db));
    root.Set("median_stability_milli",
             Value::MakeInt(report.median_stability_milli));
    Value candidates = Value::MakeArray();
    for (const HesitationCandidate& candidate : report.candidates) {
        Value item = Value::MakeObject();
        item.Set("family", Value::MakeString(candidate.family));
        Value start = Value::MakeObject();
        start.Set("value", Value::MakeInt(candidate.start.value));
        start.Set("rate", Value::MakeInt(candidate.start.rate));
        item.Set("start", std::move(start));
        Value end = Value::MakeObject();
        end.Set("value", Value::MakeInt(candidate.end.value));
        end.Set("rate", Value::MakeInt(candidate.end.rate));
        item.Set("end", std::move(end));
        item.Set("stability_milli", Value::MakeInt(candidate.stability_milli));
        item.Set("level_db_below_speech",
                 Value::MakeInt(candidate.level_db_below_speech));
        item.Set("previous_word", Value::MakeString(candidate.previous_word));
        item.Set("next_word", Value::MakeString(candidate.next_word));
        item.Set("within_word", Value::MakeString(candidate.within_word));
        candidates.Push(std::move(item));
    }
    root.Set("candidates", std::move(candidates));
    if (includeWindows) {
        Value stability = Value::MakeArray();
        for (const int64_t value : report.stability_milli)
            stability.Push(Value::MakeInt(value));
        root.Set("stability_milli", std::move(stability));
        Value levels = Value::MakeArray();
        for (const int64_t value : report.level_db)
            levels.Push(Value::MakeInt(value));
        root.Set("level_db", std::move(levels));
    }
    return root.Dump();
}

bool LoadHesitationReport(const std::string& path, HesitationReport& report,
                          std::string& error) {
    report = {};
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "no hesitation analysis";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    Value root;
    if (!Value::Parse(buffer.str(), root, error) || !root.IsObject()) {
        error = "malformed hesitation cache: " + error;
        return false;
    }
    int64_t version = 0;
    const Value* versionValue = root.Find("version");
    const Value* mediaId = root.Find("media_id");
    const Value* candidates = root.Find("candidates");
    if (versionValue == nullptr || !versionValue->AsInt64(version) ||
        version != 1 || mediaId == nullptr || !mediaId->IsString() ||
        candidates == nullptr || !candidates->IsArray()) {
        error = "unsupported hesitation cache";
        return false;
    }
    report.media_id = mediaId->AsString();
    const auto readInt = [&](const Value& object, const char* key,
                             int64_t& output) {
        const Value* value = object.Find(key);
        return value != nullptr && value->AsInt64(output);
    };
    const auto readText = [&](const Value& object, const char* key,
                              std::string& output) {
        const Value* value = object.Find(key);
        if (value == nullptr || !value->IsString()) return false;
        output = value->AsString();
        return true;
    };
    const auto readTime = [&](const Value& object, const char* key,
                              RationalTime& output) {
        const Value* value = object.Find(key);
        int64_t number = 0;
        int64_t rate = 0;
        if (value == nullptr || !readInt(*value, "value", number) ||
            !readInt(*value, "rate", rate) || rate <= 0 ||
            rate > std::numeric_limits<int32_t>::max())
            return false;
        output = {number, static_cast<int32_t>(rate)};
        return true;
    };
    int64_t windows = 0;
    if (!readInt(root, "windows_per_second", windows) ||
        !readInt(root, "speech_level_db", report.speech_level_db) ||
        !readInt(root, "floor_level_db", report.floor_level_db) ||
        !readInt(root, "median_stability_milli",
                 report.median_stability_milli)) {
        error = "hesitation cache lacks its levels";
        return false;
    }
    report.windows_per_second = static_cast<int32_t>(windows);
    for (const Value& item : candidates->AsArray()) {
        HesitationCandidate candidate;
        if (!readText(item, "family", candidate.family) ||
            !readTime(item, "start", candidate.start) ||
            !readTime(item, "end", candidate.end) ||
            !readInt(item, "stability_milli", candidate.stability_milli) ||
            !readInt(item, "level_db_below_speech",
                     candidate.level_db_below_speech) ||
            !readText(item, "previous_word", candidate.previous_word) ||
            !readText(item, "next_word", candidate.next_word) ||
            !readText(item, "within_word", candidate.within_word)) {
            error = "hesitation cache contains an invalid candidate";
            return false;
        }
        report.candidates.push_back(std::move(candidate));
    }
    ReadIntArray(root.Find("stability_milli"), report.stability_milli);
    ReadIntArray(root.Find("level_db"), report.level_db);
    return true;
}

bool AnalyzeProjectSourceHesitations(const std::filesystem::path& projectPath,
                                     const std::vector<LibraryMedia>& library,
                                     const Ulid& sourceId,
                                     const HesitationSettings& settings,
                                     HesitationReport& report,
                                     std::string& error) {
    const auto media = std::find_if(
        library.begin(), library.end(),
        [&](const LibraryMedia& item) { return item.id == sourceId; });
    if (media == library.end()) {
        error = "unknown media_id '" + sourceId + "'";
        return false;
    }
    if (media->metadata_complete && !media->has_audio) {
        error = "media_id '" + sourceId + "' has no audio";
        return false;
    }
    const std::filesystem::path directory =
        std::filesystem::absolute(projectPath).parent_path();
    std::filesystem::path input(media->path);
    if (input.is_relative()) input = directory / input;
    Transcript transcript;
    std::string ignored;
    std::vector<TranscriptWord> words;
    if (LoadSourceWords(directory / ".cutmachine", sourceId, transcript,
                        ignored))
        words = transcript.words;
    return GenerateHesitationReport(
        input.lexically_normal().string(),
        (directory / ".cutmachine" / "hesitations" / (sourceId + ".json"))
            .string(),
        sourceId, words, settings, report, error);
}

bool ReadProjectSourceHesitations(const std::filesystem::path& projectPath,
                                  const Ulid& sourceId,
                                  HesitationReport& report,
                                  std::string& error) {
    return LoadHesitationReport(
        (std::filesystem::absolute(projectPath).parent_path() / ".cutmachine" /
         "hesitations" / (sourceId + ".json"))
            .string(),
        report, error);
}
