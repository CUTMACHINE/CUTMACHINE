// FC6 -- docs/roadmap/FILMCRAFT-2026-10.md. The proposed ducking
// (AudioDspDucking.h) on synthetic voices whose answer is known to the
// sample: a voice over a room at -60 dBFS, a gap short enough to be closed,
// a cough short enough to be dropped, a dip inside a word that the
// hysteresis keeps, the curve's ramps, their merging, their clamping at the
// ends of the sound, and the settings refused rather than rounded.
// Portable.

#include "AudioDspDucking.h"
#include "Json.h"

#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using audio_dsp::DuckingPoint;
using audio_dsp::DuckingProposal;
using audio_dsp::DuckingRange;
using audio_dsp::DuckingSettings;
using audio_dsp::ProposeDucking;

int failures = 0;

void Check(bool condition, const std::string& label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

constexpr int32_t kRate = 48000;
constexpr double kPi = 3.14159265358979323846;

// A room at -60 dBFS RMS (uniform noise), `seconds` long.
std::vector<float> Room(double seconds, int32_t rate = kRate) {
    uint64_t state = 0x2545F4914F6CDD1Dull;
    const auto count = static_cast<size_t>(seconds * rate);
    std::vector<float> out(count);
    // Uniform in [-a, a] has an RMS of a / sqrt(3).
    const double amplitude = std::sqrt(3.0) * std::pow(10.0, -60.0 / 20.0);
    for (float& value : out) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const double uniform = ((state * 0x2545F4914F6CDD1Dull) >> 40) /
                                   static_cast<double>(1ull << 24) * 2.0 -
                               1.0;
        value = static_cast<float>(uniform * amplitude);
    }
    return out;
}

// A voice-like tone (220 Hz, its level in dBFS RMS) over [from, to) samples.
void Speak(std::vector<float>& x, int64_t from, int64_t to, double dbfs) {
    const double amplitude = std::sqrt(2.0) * std::pow(10.0, dbfs / 20.0);
    for (int64_t n = from; n < to; ++n) {
        x[static_cast<size_t>(n)] += static_cast<float>(
            amplitude * std::sin(2.0 * kPi * 220.0 * n / kRate));
    }
}

int64_t At(double seconds) { return std::llround(seconds * kRate); }

bool SameRanges(const std::vector<DuckingRange>& got,
                const std::vector<std::pair<int64_t, int64_t>>& expected) {
    if (got.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].start.rate != kRate || got[i].end.rate != kRate ||
            got[i].start.value != expected[i].first ||
            got[i].end.value != expected[i].second) {
            return false;
        }
    }
    return true;
}

bool SameCurve(const std::vector<DuckingPoint>& got,
               const std::vector<std::pair<int64_t, int64_t>>& expected) {
    if (got.size() != expected.size()) {
        return false;
    }
    for (size_t i = 0; i < got.size(); ++i) {
        if (got[i].time.rate != kRate ||
            got[i].time.value != expected[i].first ||
            got[i].gain_db != expected[i].second) {
            return false;
        }
    }
    return true;
}

std::string Describe(const DuckingProposal& proposal) {
    std::string text;
    for (const DuckingRange& range : proposal.speech) {
        text += "[" + std::to_string(range.start.value) + ", " +
                std::to_string(range.end.value) + ") ";
    }
    text += "| ";
    for (const DuckingPoint& point : proposal.curve) {
        text += "(" + std::to_string(point.time.value) + ", " +
                std::to_string(point.gain_db) + ") ";
    }
    return text;
}

std::vector<float> Voice() {
    std::vector<float> x = Room(10.0);
    Speak(x, At(1.0), At(2.5), -20.0);
    // A dip inside a word, 50 ms at -52 dBFS: under the level that opens a
    // range (-50), over the one that closes it (-54).
    for (int64_t n = At(1.8); n < At(1.85); ++n) {
        x[static_cast<size_t>(n)] *= 0.0f;
    }
    Speak(x, At(1.8), At(1.85), -52.0);
    // 200 ms of room: under the 300 ms a gap needs to stay open.
    Speak(x, At(2.7), At(4.0), -20.0);
    // A cough of 50 ms: under the 200 ms a range needs.
    Speak(x, At(5.0), At(5.05), -15.0);
    Speak(x, At(6.0), At(7.5), -25.0);
    return x;
}

void TestRangesAndCurve() {
    const std::vector<float> x = Voice();
    DuckingSettings settings;
    DuckingProposal proposal;
    std::string code;
    std::string error;
    Check(ProposeDucking(x, kRate, settings, proposal, code, error),
          "a voice is analysed: " + error);
    Check(std::abs(proposal.floor_dbfs + 60.0) < 0.5,
          "the floor is the room's -60 dBFS: " +
              std::to_string(proposal.floor_dbfs));
    Check(std::abs(proposal.enter_dbfs - proposal.floor_dbfs - 10.0) < 1e-9 &&
              std::abs(proposal.leave_dbfs - proposal.enter_dbfs + 4.0) < 1e-9,
          "speech opens 10 dB over the floor and closes 4 dB under that");
    Check(SameRanges(proposal.speech, {{At(1.0), At(4.0)}, {At(6.0), At(7.5)}}),
          "two ranges, the short gap closed, the dip kept, the cough "
          "dropped, to the sample: " +
              Describe(proposal));
    Check(
        proposal.duration.value == At(10.0) && proposal.duration.rate == kRate,
        "the duration is the sound's, in samples");
    // 150 ms is 7 200 samples, 400 ms 19 200.
    Check(SameCurve(proposal.curve, {{At(1.0) - 7200, 0},
                                     {At(1.0), -12},
                                     {At(4.0), -12},
                                     {At(4.0) + 19200, 0},
                                     {At(6.0) - 7200, 0},
                                     {At(6.0), -12},
                                     {At(7.5), -12},
                                     {At(7.5) + 19200, 0}}),
          "down 12 dB from 150 ms before each range, back up over 400 ms "
          "after: " +
              Describe(proposal));

    // Without the bridge the gap opens; without the minimum the cough is
    // kept.
    settings.bridge_milliseconds = 0;
    settings.minimum_speech_milliseconds = 0;
    ProposeDucking(x, kRate, settings, proposal, code, error);
    Check(SameRanges(proposal.speech, {{At(1.0), At(2.5)},
                                       {At(2.7), At(4.0)},
                                       {At(5.0), At(5.05)},
                                       {At(6.0), At(7.5)}}),
          "without bridge or minimum: four ranges: " + Describe(proposal));
    // Without hysteresis (nor a bridge to close it again) the dip closes
    // the word.
    settings = DuckingSettings{};
    settings.hysteresis_db = 0;
    settings.bridge_milliseconds = 0;
    ProposeDucking(x, kRate, settings, proposal, code, error);
    Check(proposal.speech.size() == 4 &&
              proposal.speech[0].end.value == At(1.8) &&
              proposal.speech[1].start.value == At(1.85),
          "without hysteresis the dip closes the range: " + Describe(proposal));

    // A release long enough to meet the next ramp: ducked as one.
    settings = DuckingSettings{};
    settings.release_milliseconds = 2000;
    ProposeDucking(x, kRate, settings, proposal, code, error);
    Check(SameCurve(proposal.curve, {{At(1.0) - 7200, 0},
                                     {At(1.0), -12},
                                     {At(7.5), -12},
                                     {At(9.5), 0}}),
          "ramps that meet stay ducked between the ranges: " +
              Describe(proposal));
}

void TestEdges() {
    // Speech from 50 ms, and again to the end: the first down ramp starts
    // with the sound, there is no ramp back up after the last range.
    std::vector<float> x = Room(3.0);
    Speak(x, At(0.05), At(1.5), -20.0);
    Speak(x, At(2.5), At(3.0), -20.0);
    DuckingProposal proposal;
    std::string code;
    std::string error;
    Check(ProposeDucking(x, kRate, DuckingSettings{}, proposal, code, error),
          "speech at the edges is analysed");
    Check(SameCurve(proposal.curve, {{0, 0},
                                     {At(0.05), -12},
                                     {At(1.5), -12},
                                     {At(1.9), 0},
                                     {At(2.35), 0},
                                     {At(2.5), -12},
                                     {At(3.0), -12}}),
          "a ramp cut by the start starts with the sound, none after the "
          "end: " +
              Describe(proposal));

    // Silence: nothing to duck under.
    const std::vector<float> silence(static_cast<size_t>(kRate), 0.0f);
    Check(ProposeDucking(silence, kRate, DuckingSettings{}, proposal, code,
                         error) &&
              proposal.silent && proposal.speech.empty() &&
              proposal.curve.empty(),
          "digital silence is said silent, with no range");

    // The room alone: no range.
    const std::vector<float> room = Room(3.0);
    ProposeDucking(room, kRate, DuckingSettings{}, proposal, code, error);
    Check(!proposal.silent && proposal.speech.empty(),
          "a room without a voice has no range");
}

void TestRefusals() {
    const std::vector<float> x = Room(1.0);
    DuckingProposal proposal;
    std::string code;
    std::string error;
    Check(!ProposeDucking(x, 22050, DuckingSettings{}, proposal, code, error) &&
              code == "InvalidInput",
          "22 050 Hz is not a whole number of samples every 10 ms: refused");
    DuckingSettings settings;
    settings.attack_milliseconds = 1;
    Check(!ProposeDucking(x, 44100, settings, proposal, code, error) &&
              code == "InvalidInput",
          "1 ms at 44.1 kHz is not a whole sample: refused, not rounded");
    settings = DuckingSettings{};
    Check(ProposeDucking(x, 44100, settings, proposal, code, error),
          "the defaults land on whole samples at 44.1 kHz: " + error);
    settings.hysteresis_db = settings.threshold_db;
    Check(!ProposeDucking(x, kRate, settings, proposal, code, error) &&
              code == "InvalidInput",
          "a hysteresis as wide as the threshold is refused");
}

void TestJson() {
    const std::vector<float> x = Voice();
    DuckingSettings settings;
    DuckingProposal proposal;
    std::string code;
    std::string error;
    ProposeDucking(x, kRate, settings, proposal, code, error);
    mcp_json::Value json;
    std::string parseError;
    Check(mcp_json::Value::Parse(
              audio_dsp::DuckingProposalJson(proposal, settings), json,
              parseError),
          "the answer is JSON: " + parseError);
    const mcp_json::Value* speech = json.Find("speech");
    const mcp_json::Value* curve = json.Find("gain_curve");
    const mcp_json::Value* applied = json.Find("applied");
    Check(speech != nullptr && speech->AsArray().size() == 2 &&
              curve != nullptr && curve->AsArray().size() == 8,
          "the answer carries the ranges and the curve");
    Check(applied != nullptr && applied->IsBool() && !applied->AsBool(),
          "the answer says nothing was applied");
    const mcp_json::Value& first = speech->AsArray()[0];
    const mcp_json::Value* start = first.Find("start");
    int64_t value = 0;
    int64_t rate = 0;
    Check(start != nullptr && start->Find("value")->AsInt64(value) &&
              start->Find("rate")->AsInt64(rate) && value == At(1.0) &&
              rate == kRate,
          "a time is {value, rate} at the sound's rate");
}

}  // namespace

int main() {
    TestRangesAndCurve();
    TestEdges();
    TestRefusals();
    TestJson();
    if (failures != 0) {
        std::cerr << failures << " ducking check(s) failed\n";
        return 1;
    }
    std::cout << "ducking tests passed\n";
    return 0;
}
