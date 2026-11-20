// FC5 -- docs/roadmap/FILMCRAFT-2026-10.md. FFmpeg as an oracle for the
// biquads of AudioDsp.h: its equalizer, highpass, lowpass, lowshelf,
// highshelf and bandreject filters are the same RBJ cookbook sections
// (libavfilter/af_biquads.c), run here in double (aformat=dbl) on the same
// noise. Its sections are in direct form I, ours in transposed direct form
// II: the two agree to the rounding of the output to float, and the
// tolerance below (1e-6 on a signal of 0.5) is that rounding with room.
//
// The FFmpeg resolved as the engine resolves it (FfmpegBinary.h). Without
// one, or one without these filters, the suite says so and exits 77, which
// CTest reports as skipped, not passed.

#include "AudioDsp.h"
#include "AudioDspFilters.h"
#include "FfmpegBinary.h"
#include "ProcessLauncher.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

#include "TestSupport.h"

namespace {

constexpr int32_t kRate = 48000;
constexpr size_t kLength = 48000;

std::vector<float> Noise() {
    uint64_t state = 0x9E3779B97F4A7C15ull;
    std::vector<float> out(kLength);
    for (float& value : out) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const uint64_t bits = state * 0x2545F4914F6CDD1Dull;
        value = static_cast<float>(
            ((bits >> 40) / static_cast<double>(1ull << 24)) - 0.5);
    }
    return out;
}

bool WriteRaw(const std::filesystem::path& path,
              const std::vector<float>& samples) {
    std::ofstream file(path, std::ios::binary);
    file.write(reinterpret_cast<const char*>(samples.data()),
               static_cast<std::streamsize>(samples.size() * sizeof(float)));
    return static_cast<bool>(file);
}

std::vector<float> ReadRaw(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::vector<char> bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    std::vector<float> samples(bytes.size() / sizeof(float));
    std::memcpy(samples.data(), bytes.data(), samples.size() * sizeof(float));
    return samples;
}

enum class Outcome { kRan, kSkipped, kMissingFilter, kFailed };

// `filter` run by FFmpeg on `input`, in double; the refusal when it cannot.
Outcome RunFfmpeg(const std::filesystem::path& directory,
                  const std::vector<float>& input, const std::string& filter,
                  std::vector<float>& output, std::string& why) {
    const std::filesystem::path in = directory / "in.f32";
    const std::filesystem::path out = directory / "out.f32";
    if (!WriteRaw(in, input)) {
        why = "cannot write " + in.string();
        return Outcome::kFailed;
    }
    process::Request request;
    request.arguments = {"",
                         "-hide_banner",
                         "-loglevel",
                         "error",
                         "-nostdin",
                         "-f",
                         "f32le",
                         "-ar",
                         std::to_string(kRate),
                         "-ac",
                         "1",
                         "-i",
                         in.string(),
                         "-af",
                         "aformat=sample_fmts=dbl," + filter,
                         "-f",
                         "f32le",
                         "-y",
                         out.string()};
    request.standard_output = process::Stream::kDiscard;
    request.standard_error = process::Stream::kPipe;
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        why = ffmpeg::StartError(result, "pipes");
        return Outcome::kSkipped;
    }
    if (!result.Succeeded()) {
        // A build without this filter (a minimal or bundled FFmpeg) cannot
        // be an oracle for it: said, and not counted as a failure.
        if (result.error_tail.find("No such filter") != std::string::npos) {
            why = result.error_tail;
            while (!why.empty() &&
                   std::isspace(static_cast<unsigned char>(why.back())) != 0) {
                why.pop_back();
            }
            return Outcome::kMissingFilter;
        }
        why = "FFmpeg failed on '" + filter + "': " + result.error_tail;
        return Outcome::kFailed;
    }
    output = ReadRaw(out);
    return Outcome::kRan;
}

}  // namespace

int main() {
    using audio_dsp::BiquadCoefficients;
    using audio_dsp::BiquadState;
    using audio_dsp::BiquadType;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() /
        ("cutmachine-audio-dsp-ffmpeg-" + std::to_string(::getpid()));
    std::filesystem::create_directories(directory);
    const std::vector<float> input = Noise();
    const std::string q = "0.70710678118654752";

    struct Case {
        std::string name;
        std::string filter;
        // Our side: the effect a caller would use, or a section alone.
        std::function<std::vector<float>(std::vector<float>)> ours;
    };
    const auto section = [](BiquadType type, double frequency, double quality,
                            double gain) {
        return [=](std::vector<float> x) {
            const BiquadCoefficients c = BiquadCoefficients::Design(
                type, kRate, frequency, quality, gain);
            BiquadState state;
            for (float& value : x) {
                value = static_cast<float>(state.Process(c, value));
            }
            return x;
        };
    };
    const auto effect = [](const std::string& id,
                           std::vector<std::pair<std::string, double>> set) {
        return [=](std::vector<float> x) {
            std::unique_ptr<audio_dsp::Effect> fx =
                audio_dsp::CreateEffect(id, kRate, 1);
            for (const auto& [name, value] : set) {
                fx->SetParameter(name, value);
            }
            fx->Reset();
            float* channels[1] = {x.data()};
            fx->Process(channels, x.size());
            return x;
        };
    };
    const std::vector<Case> cases = {
        {"the EQ's peaking band, +6 dB at 1 kHz, Q 1.5",
         "equalizer=f=1000:t=q:w=1.5:g=6",
         effect("parametric_eq", {{"band3.frequency", 1000.0},
                                  {"band3.q", 1.5},
                                  {"band3.gain", 6.0}})},
        {"the EQ's peaking band, -12 dB at 300 Hz, Q 4",
         "equalizer=f=300:t=q:w=4:g=-12",
         effect("parametric_eq", {{"band2.frequency", 300.0},
                                  {"band2.q", 4.0},
                                  {"band2.gain", -12.0}})},
        {"the high-pass at 100 Hz, 12 dB an octave",
         "highpass=f=100:t=q:w=" + q + ":p=2",
         effect("high_pass", {{"frequency", 100.0}})},
        {"a low-pass at 2 kHz", "lowpass=f=2000:t=q:w=" + q + ":p=2",
         section(BiquadType::kLowPass, 2000.0, std::sqrt(0.5), 0.0)},
        {"a low shelf, +9 dB at 200 Hz", "lowshelf=f=200:t=q:w=" + q + ":g=9",
         section(BiquadType::kLowShelf, 200.0, std::sqrt(0.5), 9.0)},
        {"a high shelf, -9 dB at 5 kHz",
         "highshelf=f=5000:t=q:w=" + q + ":g=-9",
         section(BiquadType::kHighShelf, 5000.0, std::sqrt(0.5), -9.0)},
        {"a notch at 50 Hz, Q 30", "bandreject=f=50:t=q:w=30",
         section(BiquadType::kNotch, 50.0, 30.0, 0.0)},
    };
    int ran = 0;
    int missing = 0;
    for (const Case& c : cases) {
        std::vector<float> theirs;
        std::string why;
        const Outcome outcome =
            RunFfmpeg(directory, input, c.filter, theirs, why);
        if (outcome == Outcome::kSkipped) {
            std::cout << "SKIP: no FFmpeg to compare the biquads with (" << why
                      << ")\n";
            std::filesystem::remove_all(directory);
            return 77;
        }
        if (outcome == Outcome::kMissingFilter) {
            std::cout << "SKIP: " << c.name << ": this FFmpeg lacks "
                      << c.filter << " (" << why << ")\n";
            ++missing;
            continue;
        }
        if (outcome == Outcome::kFailed) {
            Check(false, c.name + ": " + why);
            continue;
        }
        ++ran;
        const std::vector<float> mine = c.ours(input);
        double worst = 0.0;
        if (theirs.size() != mine.size()) {
            Check(false, c.name + ": FFmpeg gave " +
                             std::to_string(theirs.size()) + " samples for " +
                             std::to_string(mine.size()));
            continue;
        }
        double changed = 0.0;
        for (size_t n = 0; n < mine.size(); ++n) {
            worst = std::max(
                worst, std::abs(static_cast<double>(mine[n]) - theirs[n]));
            changed = std::max(
                changed, std::abs(static_cast<double>(theirs[n] - input[n])));
        }
        // Two pass-throughs would agree too.
        Check(changed > 1e-3, c.name + ": FFmpeg's filter changed the sound");
        std::cout << c.name << ", against FFmpeg's " << c.filter
                  << ": largest difference " << worst << '\n';
        Check(worst < 1e-6,
              c.name + " is FFmpeg's within 1e-6: " + std::to_string(worst));
    }
    std::filesystem::remove_all(directory);
    if (failures != 0) {
        std::cerr << failures << " FFmpeg oracle check(s) failed\n";
        return 1;
    }
    std::cout << ran << " biquads compared with FFmpeg\n";
    if (missing != 0) {
        std::cout << "SKIP: " << missing
                  << " biquad(s) not compared, their filter missing\n";
        return 77;
    }
    return 0;
}
