#include "MusicStructure.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace {

double Power(int32_t centiDb) {
    return std::pow(10.0, static_cast<double>(centiDb) / 1000.0);
}

int32_t CentiDb(double power) {
    return static_cast<int32_t>(
        std::lround(1000.0 * std::log10(std::max(power, 1e-12))));
}

// Mean power of one band over beats [first, end), from absolute levels.
double MeanPower(const std::vector<int32_t>& levels, size_t first, size_t end) {
    double sum = 0.0;
    for (size_t index = first; index < end; ++index)
        sum += Power(levels[index]);
    return end > first ? sum / static_cast<double>(end - first) : 0.0;
}

}  // namespace

bool MeasureBeatEnergies(const AudioBandEnvelope& bands, const TempoGrid& grid,
                         std::vector<MusicBeatEnergy>& beats,
                         std::string& error) {
    beats.clear();
    error.clear();
    if (!grid.accepted() || grid.beats.empty()) {
        error = "the tempo grid was refused: no beats to measure on";
        return false;
    }
    if (bands.sample_rate != static_cast<uint32_t>(grid.sample_rate) ||
        bands.hop_size == 0 ||
        bands.low_centi_db.size() != bands.full_centi_db.size() ||
        bands.high_centi_db.size() != bands.full_centi_db.size()) {
        error = "the band envelope does not match the tempo grid's analysis";
        return false;
    }
    const int64_t hop = bands.hop_size;
    const int64_t halfWindow = bands.window_size / 2;
    const auto frames = static_cast<int64_t>(bands.full_centi_db.size());
    // First frame whose window centre is at or after `sample`.
    const auto frameAt = [&](int64_t sample) {
        const int64_t start = sample - halfWindow;
        if (start <= 0) return int64_t{0};
        return std::min(frames, (start + hop - 1) / hop);
    };
    struct Absolute {
        double low = 0.0;
        double high = 0.0;
        double full = 0.0;
        bool measured = false;
    };
    std::vector<Absolute> absolute(grid.beats.size());
    for (size_t index = 0; index < grid.beats.size(); ++index) {
        const int64_t begin = grid.beats[index].sample;
        const int64_t end = index + 1 < grid.beats.size()
                                ? grid.beats[index + 1].sample
                                : begin + grid.period_samples;
        const int64_t first = frameAt(begin);
        const int64_t last = frameAt(end);
        for (int64_t frame = first; frame < last; ++frame) {
            absolute[index].low += Power(bands.low_centi_db[frame]);
            absolute[index].high += Power(bands.high_centi_db[frame]);
            absolute[index].full += Power(bands.full_centi_db[frame]);
        }
        if (last > first) {
            const auto count = static_cast<double>(last - first);
            absolute[index].low /= count;
            absolute[index].high /= count;
            absolute[index].full /= count;
            absolute[index].measured = true;
        }
    }
    double loudestLow = 0.0;
    double loudestHigh = 0.0;
    double loudestFull = 0.0;
    for (const Absolute& beat : absolute) {
        loudestLow = std::max(loudestLow, beat.low);
        loudestHigh = std::max(loudestHigh, beat.high);
        loudestFull = std::max(loudestFull, beat.full);
    }
    for (size_t index = 0; index < grid.beats.size(); ++index) {
        if (!absolute[index].measured) continue;
        const TempoGridBeat& source = grid.beats[index];
        MusicBeatEnergy beat;
        beat.index = source.index;
        beat.sample = source.sample;
        beat.bar = source.bar;
        beat.beat_in_bar = source.beat_in_bar;
        beat.low_centi_db = CentiDb(absolute[index].low) - CentiDb(loudestLow);
        beat.high_centi_db =
            CentiDb(absolute[index].high) - CentiDb(loudestHigh);
        beat.full_centi_db =
            CentiDb(absolute[index].full) - CentiDb(loudestFull);
        beats.push_back(beat);
    }
    if (beats.empty()) {
        error = "the band envelope covers none of the grid's beats";
        return false;
    }
    return true;
}

std::vector<MusicEnergyChange> FindEnergyChanges(
    const std::vector<MusicBeatEnergy>& beats,
    const MusicEnergySettings& settings) {
    std::vector<MusicEnergyChange> changes;
    const auto before = static_cast<size_t>(std::max(settings.beats_before, 1));
    const auto after = static_cast<size_t>(std::max(settings.beats_after, 1));
    if (beats.size() < before + after) return changes;
    std::vector<int32_t> low;
    std::vector<int32_t> full;
    low.reserve(beats.size());
    full.reserve(beats.size());
    for (const MusicBeatEnergy& beat : beats) {
        low.push_back(beat.low_centi_db);
        full.push_back(beat.full_centi_db);
    }
    // The step at each beat that has both windows, then only the beats that
    // are the strongest step of their kind within one `after` window: a
    // drop spreads over the beats around it, and it is one drop.
    std::vector<int32_t> step(beats.size(), 0);
    for (size_t index = before; index + after <= beats.size(); ++index) {
        step[index] = CentiDb(MeanPower(low, index, index + after)) -
                      CentiDb(MeanPower(low, index - before, index));
    }
    for (size_t index = before; index + after <= beats.size(); ++index) {
        const int32_t value = step[index];
        if (std::abs(value) < settings.threshold_centi_db) continue;
        bool strongest = true;
        const size_t from = index >= after ? index - after : 0;
        const size_t to = std::min(beats.size(), index + after + 1);
        for (size_t other = from; other < to && strongest; ++other) {
            if (other == index || (step[other] > 0) != (value > 0)) continue;
            strongest =
                std::abs(step[other]) < std::abs(value) ||
                (std::abs(step[other]) == std::abs(value) && other > index);
        }
        if (!strongest) continue;
        MusicEnergyChange change;
        change.kind = value > 0 ? "rise" : "fall";
        change.index = beats[index].index;
        change.sample = beats[index].sample;
        change.bar = beats[index].bar;
        change.beat_in_bar = beats[index].beat_in_bar;
        change.low_change_centi_db = value;
        change.full_change_centi_db =
            CentiDb(MeanPower(full, index, index + after)) -
            CentiDb(MeanPower(full, index - before, index));
        changes.push_back(change);
    }
    return changes;
}
