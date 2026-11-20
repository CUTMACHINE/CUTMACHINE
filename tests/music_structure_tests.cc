// Where a track's energy moves, beat by beat (MusicStructure.h). Plain C++,
// no AppKit and no FFmpeg: the grid and the envelope are built here, so this
// runs on a plain Linux host like the other portable suites.
//
// What it guards: the bass coming in is named once, on the beat it comes in,
// and a phrase that only thickens is not named at all.

#include "MusicStructure.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

constexpr int32_t kRate = 44100;
// 124 BPM at 44.1 kHz, the period of the track the reel was cut on.
constexpr int64_t kPeriod = 21338;
constexpr uint32_t kHop = 512;

TempoGrid MakeGrid(int64_t beats) {
    TempoGrid grid;
    grid.status = TempoGridStatus::Fitted;
    grid.sample_rate = kRate;
    grid.period_samples = kPeriod;
    grid.phase_samples = 0;
    for (int64_t index = 0; index < beats; ++index) {
        TempoGridBeat beat;
        beat.index = index;
        beat.sample = index * kPeriod;
        beat.bar = static_cast<int32_t>(index / 4) + 1;
        beat.beat_in_bar = static_cast<int32_t>(index % 4) + 1;
        grid.beats.push_back(beat);
    }
    return grid;
}

// One frame per hop over `beats` beats, the low band at `low(beat)`
// hundredths of a decibel, the other two bands flat.
template <typename Level>
AudioBandEnvelope MakeEnvelope(int64_t beats, Level low) {
    AudioBandEnvelope bands;
    bands.sample_rate = kRate;
    bands.hop_size = kHop;
    bands.window_size = 2 * kHop;
    const int64_t frames = (beats * kPeriod) / kHop + 2;
    for (int64_t frame = 0; frame < frames; ++frame) {
        const int64_t centre = frame * kHop + kHop;
        bands.low_centi_db.push_back(low(centre / kPeriod));
        bands.high_centi_db.push_back(-4000);
        bands.full_centi_db.push_back(-2000);
    }
    return bands;
}

}  // namespace

int main() {
    // An intro with the bass 30 dB down, the drop on beat 32, a break on
    // beat 64 and the drop back on beat 80.
    const TempoGrid grid = MakeGrid(112);
    const AudioBandEnvelope bands = MakeEnvelope(112, [](int64_t beat) {
        return (beat >= 32 && beat < 64) || beat >= 80 ? -3000 : -6000;
    });
    std::vector<MusicBeatEnergy> beats;
    std::string error;
    Check(MeasureBeatEnergies(bands, grid, beats, error) && beats.size() == 112,
          "every beat of the grid is measured: " + error);
    if (beats.size() == 112) {
        Check(beats[0].low_centi_db == -3000 && beats[40].low_centi_db == 0 &&
                  beats[40].bar == 11 && beats[40].beat_in_bar == 1 &&
                  beats[40].sample == 40 * kPeriod,
              "levels are relative to the loudest beat, on the grid's own "
              "beats, got " +
                  std::to_string(beats[0].low_centi_db) + " and " +
                  std::to_string(beats[40].low_centi_db));
        Check(beats[0].full_centi_db == 0 && beats[0].high_centi_db == 0,
              "a flat band is at its own loudest everywhere");
    }

    const std::vector<MusicEnergyChange> changes =
        FindEnergyChanges(beats, MusicEnergySettings{});
    std::string found;
    for (const MusicEnergyChange& change : changes)
        found += ' ' + change.kind + '@' + std::to_string(change.index) + '(' +
                 std::to_string(change.low_change_centi_db) + ')';
    Check(changes.size() == 3, "one change per step, got" + found);
    if (changes.size() == 3) {
        Check(changes[0].kind == "rise" && changes[0].index == 32 &&
                  changes[0].bar == 9 && changes[0].beat_in_bar == 1 &&
                  changes[0].low_change_centi_db == 3000 &&
                  changes[0].full_change_centi_db == 0,
              "the drop is named on the beat the bass comes in, +30 dB");
        Check(changes[1].kind == "fall" && changes[1].index == 64 &&
                  changes[1].low_change_centi_db == -3000,
              "the break on the beat it goes");
        Check(changes[2].kind == "rise" && changes[2].index == 80,
              "and the drop back");
    }

    // A phrase that thickens by 3 dB is not a drop.
    {
        const AudioBandEnvelope thicker = MakeEnvelope(
            64, [](int64_t beat) { return beat >= 32 ? -5700 : -6000; });
        std::vector<MusicBeatEnergy> mild;
        Check(MeasureBeatEnergies(thicker, MakeGrid(64), mild, error) &&
                  FindEnergyChanges(mild, MusicEnergySettings{}).empty(),
              "a 3 dB step is under the threshold");
    }

    // Refusals.
    {
        TempoGrid refused = grid;
        refused.status = TempoGridStatus::FreeTempo;
        std::vector<MusicBeatEnergy> none;
        Check(!MeasureBeatEnergies(bands, refused, none, error) &&
                  error.find("refused") != std::string::npos,
              "a refused grid gives no beats to measure on");
        AudioBandEnvelope other = bands;
        other.sample_rate = 48000;
        Check(!MeasureBeatEnergies(other, grid, none, error),
              "an envelope at another rate than the grid is refused");
        Check(FindEnergyChanges({}, MusicEnergySettings{}).empty(),
              "no beats, no change");
    }

    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "music structure tests passed\n";
    return 0;
}
