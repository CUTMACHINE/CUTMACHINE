#include "Hesitations.h"
#include "Ulid.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "TestSupport.h"

// ITW2 -- ROADMAP.md. A vowel held on fixed formants is a hesitation
// candidate; speech whose formants keep moving is not.

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kRate = 16000;

struct Formants {
    double first;
    double second;
    double third;
};

// A voiced sound at 120 Hz shaped by three formants: each harmonic weighted
// by its nearness to them.
void Voice(std::vector<float>& samples, double seconds, const Formants& shape,
           double amplitude, double& phase) {
    const int count = static_cast<int>(seconds * kRate);
    constexpr double kPitch = 120.0;
    for (int index = 0; index < count; ++index) {
        double value = 0.0;
        for (int harmonic = 1; harmonic * kPitch < 4000.0; ++harmonic) {
            const double frequency = harmonic * kPitch;
            double gain = 0.0;
            for (const double formant :
                 {shape.first, shape.second, shape.third}) {
                const double distance = (frequency - formant) / 90.0;
                gain += std::exp(-distance * distance);
            }
            value += gain * std::sin(harmonic * phase);
        }
        phase += 2.0 * kPi * kPitch / kRate;
        samples.push_back(static_cast<float>(amplitude * value / 6.0));
    }
}

// Speech: a new vowel shape every 60 ms, the level rising and falling with
// the syllables.
void Speech(std::vector<float>& samples, double seconds, std::mt19937& random,
            double& phase) {
    const std::vector<Formants> vowels = {{730, 1090, 2440}, {270, 2290, 3010},
                                          {300, 870, 2240},  {530, 1840, 2480},
                                          {570, 840, 2410},  {440, 1020, 2240},
                                          {400, 1600, 2400}};
    std::uniform_int_distribution<size_t> pick(0, vowels.size() - 1);
    for (double elapsed = 0.0; elapsed < seconds; elapsed += 0.06) {
        const double amplitude =
            0.35 + 0.25 * std::sin(2.0 * kPi * 4.0 * elapsed);
        Voice(samples, 0.06, vowels[pick(random)], amplitude, phase);
    }
}

TranscriptWord Word(const std::string& text, double start, double end) {
    return {text,
            {static_cast<int64_t>(std::lround(start * 100)), 100},
            {static_cast<int64_t>(std::lround(end * 100)), 100}};
}

}  // namespace

int main() {
    std::mt19937 random(11);
    std::vector<float> samples;
    double phase = 0.0;
    Speech(samples, 2.0, random, phase);
    Voice(samples, 0.6, {400, 1600, 2400}, 0.45, phase);  // "euh", 2.0-2.6 s
    Speech(samples, 1.5, random, phase);

    std::vector<double> stability;
    std::vector<double> levels;
    MeasureTimbreStability(samples, stability, levels);
    Check(stability.size() > 400 && stability.size() == levels.size(),
          "a stability and a level per 10 ms window");

    HesitationSettings settings;
    HesitationReport report;
    DetectHesitations(stability, levels, {}, settings, report);
    const auto only = [&](const std::string& family) {
        return report.candidates.size() == 1 &&
               report.candidates[0].family == family &&
               report.candidates[0].start.value >= 190 &&
               report.candidates[0].start.value <= 212 &&
               report.candidates[0].end.value >= 250 &&
               report.candidates[0].end.value <= 262;
    };
    std::string listed;
    for (const HesitationCandidate& candidate : report.candidates)
        listed += " " + candidate.family + "[" +
                  std::to_string(candidate.start.value) + "," +
                  std::to_string(candidate.end.value) + ")";
    Check(
        only("held"),
        "the held vowel, and nothing in the moving speech, is found:" + listed);
    Check(
        report.median_stability_milli > 0 && !report.candidates.empty() &&
            report.candidates[0].stability_milli * 100 <
                report.median_stability_milli * settings.held_stability_percent,
        "the candidate's stability sits under the rush's threshold");

    // Placed against words: between two words it is a filler; inside one,
    // a stretched syllable.
    DetectHesitations(stability, levels,
                      {Word("et", 1.6, 1.95), Word("puis", 2.65, 3.0)},
                      settings, report);
    Check(only("filler") && report.candidates[0].previous_word == "et" &&
              report.candidates[0].next_word == "puis",
          "between two words, a filler with its neighbours");
    DetectHesitations(stability, levels,
                      {Word("et", 1.6, 1.9), Word("l'Horloge", 1.9, 2.8),
                       Word("du", 2.8, 3.0)},
                      settings, report);
    Check(only("stretched_syllable") &&
              report.candidates[0].within_word == "l'Horloge" &&
              report.candidates[0].previous_word == "et" &&
              report.candidates[0].next_word == "du",
          "inside a word, a stretched syllable of that word");

    // A hold shorter than a comma's prosody is not reported.
    std::vector<float> brief;
    phase = 0.0;
    Speech(brief, 2.0, random, phase);
    Voice(brief, 0.15, {400, 1600, 2400}, 0.45, phase);
    Speech(brief, 1.5, random, phase);
    MeasureTimbreStability(brief, stability, levels);
    DetectHesitations(stability, levels, {}, settings, report);
    Check(report.candidates.empty(),
          "a 150 ms hold is the prosody of a word, not a hesitation");

    // AGENT-2026-09 A48 -- on measurements written window by window: a
    // hold of 120 ms between two aligned words is a filler, the same hold
    // inside a word or with no words to place it stays under the 200 ms
    // floor, and a break of 40 ms joins the two halves of a hold.
    {
        std::vector<double> flat(300, 33.0);
        std::vector<double> level(300, -10.0);
        for (size_t window = 100; window < 112; ++window) flat[window] = 20.0;
        HesitationReport synthetic;
        DetectHesitations(flat, level,
                          {Word("et", 0.5, 1.0), Word("puis", 1.2, 1.6)},
                          settings, synthetic);
        Check(synthetic.candidates.size() == 1 &&
                  synthetic.candidates[0].family == "filler" &&
                  synthetic.candidates[0].start.value == 100 &&
                  synthetic.candidates[0].end.value == 112 &&
                  synthetic.candidates[0].previous_word == "et" &&
                  synthetic.candidates[0].next_word == "puis",
              "a 120 ms hold between two words is a filler");
        DetectHesitations(flat, level, {}, settings, synthetic);
        Check(synthetic.candidates.empty(),
              "with no words to place it, 120 ms stays under the floor");
        DetectHesitations(flat, level, {Word("l'Horloge", 0.9, 1.3)}, settings,
                          synthetic);
        Check(synthetic.candidates.empty(),
              "inside a word, 120 ms is a syllable, not a stretched one");
        std::vector<double> bridged(300, 33.0);
        for (size_t window = 100; window < 134; ++window)
            bridged[window] = 20.0;
        for (size_t window = 115; window < 119; ++window)
            bridged[window] = 33.0;
        DetectHesitations(bridged, level, {}, settings, synthetic);
        Check(synthetic.candidates.size() == 1 &&
                  synthetic.candidates[0].start.value == 100 &&
                  synthetic.candidates[0].end.value == 134,
              "a break of 40 ms does not end a hold");
        std::vector<double> split = bridged;
        split[119] = 33.0;
        DetectHesitations(split, level, {}, settings, synthetic);
        Check(synthetic.candidates.empty(),
              "a break of 50 ms does: two holds of 150 ms, neither reported");
        // A 200 ms hold covering a word of 160 ms whole is that word said
        // on one vowel, not a stretched syllable: a filler, so the cut
        // stops at the word's start.
        std::vector<double> covering(300, 33.0);
        for (size_t window = 100; window < 120; ++window)
            covering[window] = 20.0;
        DetectHesitations(covering, level,
                          {Word("et", 0.8, 1.0), Word("donc", 1.02, 1.18),
                           Word("moi", 1.2, 1.4)},
                          settings, synthetic);
        Check(synthetic.candidates.size() == 1 &&
                  synthetic.candidates[0].family == "filler" &&
                  synthetic.candidates[0].within_word.empty() &&
                  synthetic.candidates[0].previous_word == "et" &&
                  synthetic.candidates[0].next_word == "moi",
              "a hold over a word shorter than the floor is a filler");
    }

    // The cache round-trips.
    DetectHesitations(stability, levels, {}, settings, report);
    const auto path = std::filesystem::temp_directory_path() /
                      ("cutmachine-hesitations-" + GenerateUlid() + ".json");
    report.media_id = "01K4HES0000000000000000001";
    MeasureTimbreStability(samples, stability, levels);
    DetectHesitations(stability, levels, {}, settings, report);
    {
        std::ofstream output(path);
        output << SerializeHesitationReport(report, true);
    }
    HesitationReport reread;
    std::string error;
    Check(LoadHesitationReport(path.string(), reread, error) &&
              reread.candidates.size() == report.candidates.size() &&
              reread.median_stability_milli == report.median_stability_milli &&
              reread.stability_milli == report.stability_milli &&
              SerializeHesitationReport(reread, true) ==
                  SerializeHesitationReport(report, true),
          "the hesitation cache reads back as written: " + error);
    std::filesystem::remove(path);

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All hesitation tests passed\n";
    return 0;
}
