// SYNCHRO-2026-10 SY1 -- docs/roadmap/SYNCHRO-2026-10.md. The offset between
// two sounds of one take, on synthetic signals whose offset is known: exact
// to the sample through gain, noise, a filter and a room; refused by name
// for unrelated sounds, a tone, a loop and a drift past half a frame.
// Portable: no file is decoded.

#include "AudioOffset.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

constexpr int64_t kRate = 48000;

// Speech-like: noise bursts of 80 to 400 ms, each at its own level, between
// pauses of 40 to 300 ms with a faint room tone.
std::vector<float> Speechlike(int64_t count, uint32_t seed) {
    std::mt19937 random(seed);
    std::normal_distribution<float> noise(0.0f, 1.0f);
    std::uniform_int_distribution<int64_t> burst(80 * 48, 400 * 48);
    std::uniform_int_distribution<int64_t> pause(40 * 48, 300 * 48);
    std::uniform_real_distribution<float> level(0.05f, 0.5f);
    std::vector<float> samples(static_cast<size_t>(count));
    int64_t index = 0;
    while (index < count) {
        const int64_t loud = burst(random);
        const float gain = level(random);
        for (int64_t at = 0; at < loud && index < count; ++at, ++index) {
            samples[index] = gain * noise(random);
        }
        const int64_t quiet = pause(random);
        for (int64_t at = 0; at < quiet && index < count; ++at, ++index) {
            samples[index] = 0.002f * noise(random);
        }
    }
    return samples;
}

std::vector<float> Slice(const std::vector<float>& source, int64_t first,
                         int64_t count) {
    return {source.begin() + first, source.begin() + first + count};
}

// A camera's microphone across the room: a zero-phase low-pass (no delay
// of its own), a direct sound, reflections from 2 ms to 250 ms decaying,
// a lower gain and its own hiss.
std::vector<float> Distant(const std::vector<float>& dry, uint32_t seed) {
    std::vector<float> filtered(dry.size(), 0.0f);
    const float taps[5] = {1.0f / 9, 2.0f / 9, 3.0f / 9, 2.0f / 9, 1.0f / 9};
    for (size_t index = 0; index < dry.size(); ++index) {
        float value = 0.0f;
        for (int tap = -2; tap <= 2; ++tap) {
            const int64_t source = static_cast<int64_t>(index) + tap;
            if (source >= 0 && source < static_cast<int64_t>(dry.size())) {
                value += taps[tap + 2] * dry[source];
            }
        }
        filtered[index] = value;
    }
    std::mt19937 random(seed);
    std::uniform_int_distribution<int64_t> delay(96, 12000);
    std::uniform_real_distribution<float> sign(-1.0f, 1.0f);
    std::normal_distribution<float> hiss(0.0f, 0.004f);
    std::vector<float> room = filtered;
    for (int reflection = 0; reflection < 300; ++reflection) {
        const int64_t lag = delay(random);
        const float gain =
            0.6f * sign(random) * std::exp(-static_cast<float>(lag) / 4000.0f);
        for (size_t index = static_cast<size_t>(lag); index < room.size();
             ++index) {
            room[index] += gain * filtered[index - lag];
        }
    }
    for (float& value : room) {
        value = 0.25f * value + hiss(random);
    }
    return room;
}

// `source` read from `first` at 1 + ppm / 1e6 times its rate, by linear
// interpolation: a recorder whose clock runs fast.
std::vector<float> Drifting(const std::vector<float>& source, int64_t first,
                            int64_t count, double ppm) {
    std::vector<float> samples(static_cast<size_t>(count));
    const double step = 1.0 + ppm / 1e6;
    for (int64_t index = 0; index < count; ++index) {
        const double position = static_cast<double>(first) + index * step;
        const int64_t whole = static_cast<int64_t>(position);
        const double fraction = position - static_cast<double>(whole);
        samples[index] = static_cast<float>(source[whole] * (1.0 - fraction) +
                                            source[whole + 1] * fraction);
    }
    return samples;
}

bool Measure(const std::vector<float>& reference,
             const std::vector<float>& other, AudioOffsetReport& report,
             std::string& code, std::string& error) {
    return MeasureAudioOffset(reference, other, AudioOffsetSettings{}, report,
                              code, error);
}

}  // namespace

int main() {
    const std::vector<float> take = Speechlike(40 * kRate, 7);
    AudioOffsetReport report;
    std::string code;
    std::string error;

    // The other sound starts 12 345 samples into the reference.
    {
        const std::vector<float> reference = Slice(take, 0, 30 * kRate);
        const std::vector<float> other = Slice(take, 12345, 25 * kRate);
        Check(Measure(reference, other, report, code, error) &&
                  report.offset == 12345 && report.windows.size() == 3 &&
                  report.drift_samples == 0,
              "a later start is found to the sample: " + code + " " + error +
                  " offset " + std::to_string(report.offset));
        const std::string json =
            AudioOffsetReportJson(report, AudioOffsetSettings{});
        Check(
            json.find(R"("offset":{"value":12345,"rate":48000})") !=
                    std::string::npos &&
                json.find(R"("offset_frames":{"frames":6,)"
                          R"("frame_rate":"25/1","rounding":"nearest"})") !=
                    std::string::npos,
            "the answer: exact samples, and frames rounded and said: " + json);
    }

    // The other started 4.2 s earlier, quieter, with its own hiss.
    {
        const std::vector<float> reference =
            Slice(take, 10 * kRate, 25 * kRate);
        std::vector<float> other = Slice(take, 10 * kRate - 201600, 28 * kRate);
        std::mt19937 random(11);
        std::normal_distribution<float> hiss(0.0f, 0.01f);
        for (float& value : other) {
            value = 0.15f * value + hiss(random);
        }
        Check(Measure(reference, other, report, code, error) &&
                  report.offset == -201600,
              "an earlier, quieter, noisier start is found to the sample: " +
                  code + " " + error + " offset " +
                  std::to_string(report.offset));
    }

    // A camera across the room: filtered, reverberated, quieter.
    {
        const std::vector<float> reference = Slice(take, 0, 30 * kRate);
        const std::vector<float> other =
            Distant(Slice(take, 33333, 30 * kRate), 5);
        Check(Measure(reference, other, report, code, error) &&
                  report.offset == 33333,
              "a filtered, reverberated microphone is found to the sample: " +
                  code + " " + error + " offset " +
                  std::to_string(report.offset) + " correlation " +
                  std::to_string(report.correlation));
    }

    // Partial overlap: the recorder started 10 s before the camera and
    // stopped 10 s before it.
    {
        const std::vector<float> reference =
            Slice(take, 10 * kRate, 30 * kRate);
        const std::vector<float> other = Slice(take, 0, 30 * kRate);
        Check(Measure(reference, other, report, code, error) &&
                  report.offset == -10 * kRate && report.overlap_first == 0 &&
                  report.overlap_end == 20 * kRate,
              "a partial overlap is found, and its span: " + code + " " +
                  error + " " + std::to_string(report.overlap_first) + ".." +
                  std::to_string(report.overlap_end));
    }

    // Unrelated: another take altogether.
    {
        const std::vector<float> reference = Slice(take, 0, 20 * kRate);
        const std::vector<float> other = Speechlike(20 * kRate, 99);
        Check(!Measure(reference, other, report, code, error) &&
                  code == "WeakCorrelation",
              "an unrelated sound is refused as weak: " + code + " " + error);
    }

    // A tone: every period matches.
    {
        std::vector<float> tone(static_cast<size_t>(20 * kRate));
        for (size_t index = 0; index < tone.size(); ++index) {
            tone[index] = 0.3f * static_cast<float>(std::sin(
                                     2.0 * 3.14159265358979 * 440.0 *
                                     static_cast<double>(index) / kRate));
        }
        const std::vector<float> other =
            Slice(tone, 5000, static_cast<int64_t>(tone.size()) - 5000);
        Check(!Measure(tone, other, report, code, error) &&
                  (code == "AmbiguousPeak" || code == "WeakCorrelation"),
              "a tone is refused: " + code + " " + error);
    }

    // A loop: the same second over and over.
    {
        const std::vector<float> second = Speechlike(kRate, 3);
        std::vector<float> loop;
        for (int repeat = 0; repeat < 20; ++repeat) {
            loop.insert(loop.end(), second.begin(), second.end());
        }
        const std::vector<float> other =
            Slice(loop, 7000, static_cast<int64_t>(loop.size()) - 7000);
        Check(!Measure(loop, other, report, code, error) &&
                  code == "AmbiguousPeak",
              "a loop is refused as ambiguous: " + code + " " + error);
    }

    // Silence in one of them, and a sound too short to place.
    {
        const std::vector<float> reference = Slice(take, 0, 20 * kRate);
        const std::vector<float> silent(static_cast<size_t>(20 * kRate), 0.0f);
        Check(!Measure(reference, silent, report, code, error) &&
                  code == "WeakCorrelation",
              "a silent sound is refused: " + code + " " + error);
        Check(!Measure(reference, Slice(take, 0, kRate), report, code, error) &&
                  code == "NoOverlap",
              "a second of sound is too short to place: " + code + " " + error);
    }

    // A recorder 20 ppm fast over a minute: its drift is published, and
    // stays far under half a frame.
    {
        const std::vector<float> longTake = Speechlike(140 * kRate, 21);
        const std::vector<float> reference = Slice(longTake, 0, 60 * kRate);
        const std::vector<float> other =
            Drifting(longTake, 24000, 60 * kRate - 24000, 20.0);
        Check(Measure(reference, other, report, code, error) &&
                  // The middle window's: the offset grows by ~28 samples
                  // to the middle of the take.
                  report.offset >= 24000 && report.offset <= 24060 &&
                  report.drift_ppm > 15.0 && report.drift_ppm < 25.0 &&
                  report.drift_samples > 0,
              "20 ppm of drift is measured and published: " + code + " " +
                  error + " ppm " + std::to_string(report.drift_ppm) +
                  " samples " + std::to_string(report.drift_samples));
        // 200 ppm over two minutes: 24 ms, more than half a frame at 25.
        const std::vector<float> longReference =
            Slice(longTake, 0, 120 * kRate);
        const std::vector<float> fast =
            Drifting(longTake, 24000, 120 * kRate - 48000, 200.0);
        Check(!Measure(longReference, fast, report, code, error) &&
                  code == "ClockDrift",
              "200 ppm over two minutes is refused: " + code + " " + error);
    }

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All audio offset tests passed\n";
    return 0;
}
