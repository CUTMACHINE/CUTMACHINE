#include "TempoGrid.h"

#include "ClipEnabled.h"
#include "Fft.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <limits>

namespace {

// Onsets reduced to what a grid fit needs: exact sample positions, ascending,
// and the broadband novelty strength that decides which attacks lead.
struct OnsetTable {
    std::vector<int64_t> sample;
    std::vector<double> strength;

    size_t size() const { return sample.size(); }
    bool empty() const { return sample.empty(); }
};

struct LevelFit {
    int64_t period = 0;
    int64_t phase = 0;
};

// Beats of one metrical level that an onset actually plays, and how strong
// those onsets are. The arbitration reads both: a level can be fully played
// and still be a subdivision, if every other beat of it is weak.
struct LevelSupport {
    int64_t slots = 0;
    int64_t matched = 0;
    double strength_sum = 0.0;

    int32_t percent() const {
        return slots > 0
                   ? static_cast<int32_t>((matched * 100 + slots / 2) / slots)
                   : 0;
    }
    double mean_strength() const {
        return matched > 0 ? strength_sum / static_cast<double>(matched) : 0.0;
    }
};

int64_t FloorDivide(__int128 numerator, __int128 denominator) {
    __int128 quotient = numerator / denominator;
    if (numerator % denominator != 0 && ((numerator < 0) != (denominator < 0)))
        --quotient;
    return static_cast<int64_t>(quotient);
}

// Floor of a time expressed in samples at `rate`. Used for window bounds and
// for the frame offset published beside its exact rational.
int64_t FloorSamples(const RationalTime& time, int32_t rate) {
    return FloorDivide(static_cast<__int128>(time.value) * rate, time.rate);
}

// Nearest sample, halves up. Used only where the author hands the engine an
// approximate position (the declared downbeat), never on a grid position.
int64_t RoundSamples(const RationalTime& time, int32_t rate) {
    const __int128 numerator = static_cast<__int128>(time.value) * rate;
    const __int128 denominator = time.rate;
    const int64_t floored = FloorDivide(numerator, denominator);
    const __int128 remainder =
        numerator - static_cast<__int128>(floored) * denominator;
    return remainder * 2 >= denominator ? floored + 1 : floored;
}

// Beat index nearest to `sample`, in exact integers.
int64_t NearestIndex(int64_t sample, int64_t phase, int64_t period) {
    const int64_t distance = sample - phase;
    int64_t quotient = distance / period;
    int64_t remainder = distance % period;
    if (remainder < 0) {
        --quotient;
        remainder += period;
    }
    return remainder * 2 >= period ? quotient + 1 : quotient;
}

int64_t FirstIndexAtOrAfter(int64_t sample, int64_t phase, int64_t period) {
    // ceil((sample - phase) / period), exactly, for either sign.
    return -FloorDivide(-(static_cast<__int128>(sample) - phase), period);
}

int64_t LastIndexBefore(int64_t sample, int64_t phase, int64_t period) {
    const int64_t distance = sample - phase - 1;
    return FloorDivide(distance, period);
}

int64_t ToleranceOf(int64_t period, const TempoGridSettings& settings) {
    const int64_t tolerance =
        period * settings.match_tolerance_num / settings.match_tolerance_den;
    return std::max<int64_t>(1, tolerance);
}

bool NearestOnset(const OnsetTable& table, int64_t target, int64_t tolerance,
                  int64_t& residual, double& strength) {
    if (table.empty()) return false;
    const size_t upper = static_cast<size_t>(
        std::lower_bound(table.sample.begin(), table.sample.end(), target) -
        table.sample.begin());
    bool found = false;
    int64_t best = 0;
    size_t bestIndex = 0;
    if (upper < table.size()) {
        best = table.sample[upper] - target;
        bestIndex = upper;
        found = true;
    }
    if (upper > 0) {
        const int64_t candidate = table.sample[upper - 1] - target;
        // Ties go to the earlier onset, so two runs never disagree.
        if (!found || std::llabs(candidate) < std::llabs(best) ||
            (std::llabs(candidate) == std::llabs(best) && candidate < best)) {
            best = candidate;
            bestIndex = upper - 1;
            found = true;
        }
    }
    if (!found || std::llabs(best) > tolerance) return false;
    residual = best;
    strength = table.strength[bestIndex];
    return true;
}

LevelSupport SupportOf(const OnsetTable& table, int64_t first, int64_t step,
                       int64_t count, int64_t tolerance) {
    LevelSupport support;
    if (count <= 0 || step <= 0) return support;
    support.slots = count;
    for (int64_t slot = 0; slot < count; ++slot) {
        int64_t residual = 0;
        double strength = 0.0;
        if (NearestOnset(table, first + slot * step, tolerance, residual,
                         strength)) {
            ++support.matched;
            support.strength_sum += strength;
        }
    }
    return support;
}

// Resultant length of the onset phases taken modulo `period` (circular
// statistics). Phase-invariant, so it grades a period without having fitted
// a phase yet, and it is the number the header calls `coherence`.
double CoherenceOf(const OnsetTable& table, int64_t period) {
    if (table.empty() || period <= 0) return 0.0;
    double x = 0.0;
    double y = 0.0;
    double weight = 0.0;
    for (size_t i = 0; i < table.size(); ++i) {
        const int64_t remainder =
            ((table.sample[i] % period) + period) % period;
        const double angle = 2.0 * fft::kPi * static_cast<double>(remainder) /
                             static_cast<double>(period);
        x += table.strength[i] * std::cos(angle);
        y += table.strength[i] * std::sin(angle);
        weight += table.strength[i];
    }
    return weight > 0.0 ? std::hypot(x, y) / weight : 0.0;
}

// Circular mean of the same phases, which is the phase estimate. Weighting
// by strength is what puts the grid on the broadband attacks instead of on
// an off-beat bass note: the loud hits outvote the quiet ones.
int64_t PhaseOf(const OnsetTable& table, int64_t period) {
    if (table.empty() || period <= 0) return 0;
    double x = 0.0;
    double y = 0.0;
    for (size_t i = 0; i < table.size(); ++i) {
        const int64_t remainder =
            ((table.sample[i] % period) + period) % period;
        const double angle = 2.0 * fft::kPi * static_cast<double>(remainder) /
                             static_cast<double>(period);
        x += table.strength[i] * std::cos(angle);
        y += table.strength[i] * std::sin(angle);
    }
    double angle = std::atan2(y, x);
    if (angle < 0.0) angle += 2.0 * fft::kPi;
    const int64_t phase =
        std::llround(angle * static_cast<double>(period) / (2.0 * fft::kPi));
    return ((phase % period) + period) % period;
}

// Alternates a circular-mean phase with a weighted least-squares fit of the
// matching onsets against their integer beat indices. The regression is what
// takes the period below the analysis hop: one onset is quantized to 11.6 ms,
// two hundred of them fitted together are not.
LevelFit RefineLevel(const OnsetTable& table, int64_t seed, int64_t minPeriod,
                     int64_t maxPeriod, const TempoGridSettings& settings) {
    LevelFit fit;
    fit.period = std::min(std::max(seed, minPeriod), maxPeriod);
    fit.phase = PhaseOf(table, fit.period);
    for (int pass = 0; pass < 6; ++pass) {
        const int64_t tolerance = ToleranceOf(fit.period, settings);
        double weight = 0.0;
        double sumIndex = 0.0;
        double sumIndexSquared = 0.0;
        double sumTime = 0.0;
        double sumIndexTime = 0.0;
        int64_t count = 0;
        for (size_t i = 0; i < table.size(); ++i) {
            const int64_t sample = table.sample[i];
            const int64_t index = NearestIndex(sample, fit.phase, fit.period);
            if (std::llabs(sample - (fit.phase + index * fit.period)) >
                tolerance) {
                continue;
            }
            const double w = table.strength[i];
            const double n = static_cast<double>(index);
            const double t = static_cast<double>(sample);
            weight += w;
            sumIndex += w * n;
            sumIndexSquared += w * n * n;
            sumTime += w * t;
            sumIndexTime += w * n * t;
            ++count;
        }
        if (count < 4 || weight <= 0.0) break;
        const double denominator =
            weight * sumIndexSquared - sumIndex * sumIndex;
        if (std::fabs(denominator) < 1e-6) break;
        const double slope =
            (weight * sumIndexTime - sumIndex * sumTime) / denominator;
        const double intercept = (sumTime - slope * sumIndex) / weight;
        if (!std::isfinite(slope) || !std::isfinite(intercept)) break;
        const int64_t period = std::llround(slope);
        if (period < minPeriod || period > maxPeriod) break;
        int64_t phase = std::llround(intercept);
        phase = ((phase % period) + period) % period;
        if (period == fit.period && phase == fit.phase) break;
        fit.period = period;
        fit.phase = phase;
    }
    return fit;
}

// Dixon 2001: every inter-onset interval inside the search range votes for a
// period, then neighbouring intervals are agglomerated. Weighting a vote by
// the product of the two onsets' strengths keeps a run of quiet decay peaks
// from outvoting the kicks.
std::vector<int64_t> CandidatePeriods(const OnsetTable& table,
                                      int64_t minPeriod, int64_t maxPeriod) {
    struct Vote {
        int64_t interval = 0;
        double weight = 0.0;
    };
    std::vector<Vote> votes;
    constexpr size_t kLookahead = 8;
    for (size_t a = 0; a < table.size(); ++a) {
        for (size_t b = a + 1; b < table.size() && b <= a + kLookahead; ++b) {
            const int64_t interval = table.sample[b] - table.sample[a];
            if (interval < minPeriod) continue;
            if (interval > maxPeriod) break;
            votes.push_back({interval, table.strength[a] * table.strength[b]});
        }
    }
    if (votes.empty()) return {};
    std::sort(votes.begin(), votes.end(),
              [](const Vote& left, const Vote& right) {
                  return left.interval != right.interval
                             ? left.interval < right.interval
                             : left.weight < right.weight;
              });

    struct Cluster {
        int64_t centre = 0;
        double weight = 0.0;
    };
    std::vector<Cluster> clusters;
    size_t start = 0;
    while (start < votes.size()) {
        // A sixteenth of the period, which is 31 ms at 121 BPM: the width
        // Dixon uses, expressed relative so it holds at any tempo.
        const int64_t width = std::max<int64_t>(1, votes[start].interval / 16);
        size_t end = start;
        double weight = 0.0;
        double weightedSum = 0.0;
        while (end < votes.size() &&
               votes[end].interval <= votes[start].interval + width) {
            weight += votes[end].weight;
            weightedSum +=
                votes[end].weight * static_cast<double>(votes[end].interval);
            ++end;
        }
        if (weight > 0.0) {
            clusters.push_back({std::llround(weightedSum / weight), weight});
        }
        start = end;
    }
    std::sort(clusters.begin(), clusters.end(),
              [](const Cluster& left, const Cluster& right) {
                  return left.weight != right.weight
                             ? left.weight > right.weight
                             : left.centre < right.centre;
              });
    std::vector<int64_t> periods;
    for (const Cluster& cluster : clusters) {
        if (periods.size() >= 6) break;
        if (cluster.centre < minPeriod || cluster.centre > maxPeriod) continue;
        periods.push_back(cluster.centre);
    }
    return periods;
}

double TempoPreference(double bpm, const TempoGridSettings& settings) {
    if (bpm <= 0.0 || settings.preferred_bpm <= 0.0 ||
        settings.preference_octaves <= 0.0) {
        return 1.0;
    }
    const double octaves =
        std::log2(bpm / settings.preferred_bpm) / settings.preference_octaves;
    return std::exp(-0.5 * octaves * octaves);
}

double BpmOf(int64_t period, int32_t rate) {
    return period > 0
               ? 60.0 * static_cast<double>(rate) / static_cast<double>(period)
               : 0.0;
}

// Half-time: the midpoints between this level's beats are played, and played
// as hard as the beats themselves. The other half of a half-time reading is
// the rest of the kicks; an off-beat bass note is not, and is what the
// strength ratio keeps out.
bool IsHalfTimeReading(const OnsetTable& table, const LevelFit& fit,
                       int64_t firstIndex, int64_t lastIndex,
                       const TempoGridSettings& settings) {
    const int64_t count = lastIndex - firstIndex + 1;
    if (count < 8) return false;
    const int64_t tolerance = ToleranceOf(fit.period / 2, settings);
    const int64_t first = fit.phase + firstIndex * fit.period;
    const LevelSupport beats =
        SupportOf(table, first, fit.period, count, tolerance);
    const LevelSupport midpoints =
        SupportOf(table, first + fit.period / 2, fit.period, count, tolerance);
    if (midpoints.percent() < settings.subdivision_support_percent) {
        return false;
    }
    const double ratio =
        static_cast<double>(settings.weak_level_strength_percent) / 100.0;
    return midpoints.mean_strength() >= ratio * beats.mean_strength();
}

// Double-time: one beat out of two is unplayed, or systematically weaker.
// Either way this level is a subdivision of the real pulse. `strongerIsOdd`
// says which parity the doubled grid must land on.
bool IsDoubleTimeReading(const OnsetTable& table, const LevelFit& fit,
                         int64_t firstIndex, int64_t lastIndex,
                         const TempoGridSettings& settings) {
    const int64_t count = lastIndex - firstIndex + 1;
    if (count < 8) return false;
    const int64_t tolerance = ToleranceOf(fit.period, settings);
    const int64_t first = fit.phase + firstIndex * fit.period;
    const LevelSupport even =
        SupportOf(table, first, 2 * fit.period, (count + 1) / 2, tolerance);
    const LevelSupport odd = SupportOf(table, first + fit.period,
                                       2 * fit.period, count / 2, tolerance);
    if (even.slots == 0 || odd.slots == 0) return false;
    // Which parity the doubled grid lands on is not decided here: the phase
    // is re-fitted by strength-weighted circular mean at the new period, and
    // that lands on the loud parity on its own.
    const bool strongerIsOdd = odd.mean_strength() > even.mean_strength();
    const LevelSupport& weak = strongerIsOdd ? even : odd;
    const LevelSupport& strong = strongerIsOdd ? odd : even;
    const double ratio =
        static_cast<double>(settings.weak_level_strength_percent) / 100.0;
    const bool unplayed =
        weak.percent() < settings.minimum_beat_support_percent &&
        strong.percent() >= settings.minimum_beat_support_percent;
    const bool weaker = strong.mean_strength() > 0.0 &&
                        weak.mean_strength() <= ratio * strong.mean_strength();
    return unplayed || weaker;
}

// The stretch the period and the phase are fitted on: the longest run of
// beats this level actually plays, broken by more than `stable_section_gap`
// silent beats in a row. A track that drops to half-time for eight bars, or
// breaks down entirely, must not pull the fit off the groove.
void StableSection(const OnsetTable& table, const LevelFit& fit,
                   int64_t firstIndex, int64_t lastIndex,
                   const TempoGridSettings& settings, int64_t& stableFirst,
                   int64_t& stableLast) {
    stableFirst = firstIndex;
    stableLast = lastIndex;
    if (lastIndex < firstIndex) return;
    const int64_t tolerance = ToleranceOf(fit.period, settings);
    const int64_t gapLimit = std::max<int32_t>(0, settings.stable_section_gap);
    int64_t bestFirst = firstIndex;
    int64_t bestLast = firstIndex;
    int64_t bestMatched = -1;
    int64_t runFirst = firstIndex;
    int64_t runMatched = 0;
    int64_t gap = 0;
    bool open = false;
    for (int64_t index = firstIndex; index <= lastIndex; ++index) {
        int64_t residual = 0;
        double strength = 0.0;
        const bool matched = NearestOnset(table, fit.phase + index * fit.period,
                                          tolerance, residual, strength);
        if (matched) {
            if (!open) {
                runFirst = index;
                runMatched = 0;
                open = true;
            }
            ++runMatched;
            gap = 0;
            if (runMatched > bestMatched) {
                bestMatched = runMatched;
                bestFirst = runFirst;
                bestLast = index;
            }
            continue;
        }
        if (!open) continue;
        if (++gap > gapLimit) {
            open = false;
            gap = 0;
        }
    }
    if (bestMatched > 0) {
        stableFirst = bestFirst;
        stableLast = bestLast;
    }
}

OnsetTable OnsetsWithin(const OnsetTable& table, int64_t first, int64_t last) {
    OnsetTable subset;
    for (size_t i = 0; i < table.size(); ++i) {
        if (table.sample[i] < first || table.sample[i] > last) continue;
        subset.sample.push_back(table.sample[i]);
        subset.strength.push_back(table.strength[i]);
    }
    return subset;
}

int64_t Median(std::vector<int64_t>& values) {
    if (values.empty()) return 0;
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

int64_t Percentile(std::vector<int64_t>& sorted, int percent) {
    if (sorted.empty()) return 0;
    const size_t index = std::min<size_t>(
        sorted.size() - 1, static_cast<size_t>(sorted.size() * percent / 100));
    return sorted[index];
}

// Position of a source time on the grid: the beat index below it and how far
// past that beat it sits, in samples. Exact: the subtraction happens in the
// common rate of the two times, and only the remainder is floored to a
// sample, which is the unit residuals are published in.
void GridPositionOf(const TempoGrid& grid, const RationalTime& sourceTime,
                    int64_t& index, int64_t& remainderSamples) {
    index = 0;
    remainderSamples = 0;
    if (grid.period_samples <= 0 || grid.sample_rate <= 0) return;
    const RationalTime phase(grid.phase_samples, grid.sample_rate);
    const RationalTime distance = sourceTime.sub(phase);
    const int64_t scale = distance.rate / grid.sample_rate;
    if (scale <= 0) return;
    const int64_t period = grid.period_samples * scale;
    index = FloorDivide(distance.value, period);
    remainderSamples = (distance.value - index * period) / scale;
}

}  // namespace

const char* TempoGridStatusName(TempoGridStatus status) {
    switch (status) {
        case TempoGridStatus::Fitted:
            return "fitted";
        case TempoGridStatus::TooFewOnsets:
            return "too_few_onsets";
        case TempoGridStatus::NoPeriodFound:
            return "no_period_found";
        case TempoGridStatus::FreeTempo:
            return "free_tempo";
    }
    return "free_tempo";
}

double TempoGrid::bpm() const { return BpmOf(period_samples, sample_rate); }

TempoGrid FitTempoGrid(const std::vector<AudioOnset>& onsets,
                       const RationalTime& windowIn,
                       const RationalTime& windowOut,
                       const TempoGridSettings& settings,
                       const TempoBarSettings& bars) {
    TempoGrid grid;
    if (onsets.empty()) {
        grid.status = TempoGridStatus::TooFewOnsets;
        grid.refusal = "the source has no cached onsets";
        return grid;
    }
    const int32_t rate = onsets.front().time.rate;
    if (rate <= 0 || settings.minimum_bpm <= 0.0 ||
        settings.maximum_bpm <= settings.minimum_bpm ||
        settings.match_tolerance_den <= 0 ||
        settings.match_tolerance_num <= 0 ||
        settings.maximum_residual_den <= 0 ||
        settings.maximum_residual_num <= 0) {
        grid.status = TempoGridStatus::NoPeriodFound;
        grid.refusal = "invalid tempo grid settings";
        return grid;
    }
    grid.sample_rate = rate;
    grid.window_first_sample =
        std::max<int64_t>(0, FloorSamples(windowIn, rate));
    grid.window_last_sample = FloorSamples(windowOut, rate);
    if (grid.window_last_sample <= grid.window_first_sample) {
        grid.status = TempoGridStatus::TooFewOnsets;
        grid.refusal = "the analysis window is empty";
        return grid;
    }

    OnsetTable window;
    for (const AudioOnset& onset : onsets) {
        if (onset.time.rate != rate) continue;
        const int64_t sample = onset.time.value;
        if (sample < grid.window_first_sample ||
            sample >= grid.window_last_sample) {
            continue;
        }
        window.sample.push_back(sample);
        window.strength.push_back(static_cast<double>(onset.strength));
    }
    if (static_cast<int32_t>(window.size()) < settings.minimum_onsets) {
        grid.status = TempoGridStatus::TooFewOnsets;
        grid.refusal = "fewer than " + std::to_string(settings.minimum_onsets) +
                       " onsets in the window";
        return grid;
    }

    // "The strong attacks lead" is carried by the weighting, not by a
    // threshold: every sum below is weighted by the onset's broadband
    // novelty, so a kick outvotes a decay peak without any onset being
    // dropped. Measured on 20 September 2026 against a hard split at the
    // window's median strength: on a click track whose novelty rises and
    // falls, that split hid half the beats, the stable stretch collapsed to
    // eight beats and the period came out 0.15 % wrong -- the same onsets
    // kept whole and weighted fit to 0.001 %.
    const int64_t minPeriod = std::max<int64_t>(
        1, static_cast<int64_t>(60.0 * rate / settings.maximum_bpm));
    const int64_t maxPeriod = std::max<int64_t>(
        minPeriod + 1,
        static_cast<int64_t>(60.0 * rate / settings.minimum_bpm));

    const std::vector<int64_t> candidates =
        CandidatePeriods(window, minPeriod, maxPeriod);
    if (candidates.empty()) {
        grid.status = TempoGridStatus::NoPeriodFound;
        grid.refusal = "no inter-onset interval inside the tempo range";
        return grid;
    }

    LevelFit best;
    double bestScore = -1.0;
    for (int64_t candidate : candidates) {
        const LevelFit fit =
            RefineLevel(window, candidate, minPeriod, maxPeriod, settings);
        if (fit.period <= 0) continue;
        const double score = CoherenceOf(window, fit.period) *
                             TempoPreference(BpmOf(fit.period, rate), settings);
        if (score > bestScore) {
            bestScore = score;
            best = fit;
        }
    }
    if (best.period <= 0) {
        grid.status = TempoGridStatus::NoPeriodFound;
        grid.refusal = "no candidate period survived refinement";
        return grid;
    }
    grid.candidate_period_samples = best.period;

    // Metrical arbitration. The attacks that fall between two candidate
    // beats decide, in both directions, and a move is never undone by its
    // own inverse on the next pass.
    int lastMove = 0;
    for (int pass = 0; pass < 3; ++pass) {
        const int64_t first = FirstIndexAtOrAfter(grid.window_first_sample,
                                                  best.phase, best.period);
        const int64_t last =
            LastIndexBefore(grid.window_last_sample, best.phase, best.period);
        if (last <= first) break;
        if (lastMove != -1 && best.period * 2 <= maxPeriod &&
            IsDoubleTimeReading(window, best, first, last, settings)) {
            const int64_t seed = best.period * 2;
            best = RefineLevel(window, seed, minPeriod, maxPeriod, settings);
            ++grid.doublings;
            lastMove = 1;
            continue;
        }
        if (lastMove != 1 && best.period / 2 >= minPeriod &&
            IsHalfTimeReading(window, best, first, last, settings)) {
            best = RefineLevel(window, best.period / 2, minPeriod, maxPeriod,
                               settings);
            ++grid.halvings;
            lastMove = -1;
            continue;
        }
        break;
    }

    // Period and phase are settled on the stable stretch only, then read
    // back over the whole window.
    {
        const int64_t first = FirstIndexAtOrAfter(grid.window_first_sample,
                                                  best.phase, best.period);
        const int64_t last =
            LastIndexBefore(grid.window_last_sample, best.phase, best.period);
        int64_t stableFirst = first;
        int64_t stableLast = last;
        StableSection(window, best, first, last, settings, stableFirst,
                      stableLast);
        const OnsetTable section =
            OnsetsWithin(window,
                         best.phase + stableFirst * best.period -
                             ToleranceOf(best.period, settings),
                         best.phase + stableLast * best.period +
                             ToleranceOf(best.period, settings));
        if (static_cast<int32_t>(section.size()) >= 8) {
            const LevelFit refitted = RefineLevel(
                section, best.period, minPeriod, maxPeriod, settings);
            if (refitted.period > 0) best = refitted;
        }
        grid.stable_first_index = stableFirst;
        grid.stable_last_index = stableLast;
    }

    grid.period_samples = best.period;
    grid.phase_samples = best.phase;
    grid.first_index =
        FirstIndexAtOrAfter(grid.window_first_sample, best.phase, best.period);
    grid.last_index =
        LastIndexBefore(grid.window_last_sample, best.phase, best.period);

    // Coherence at the subdivision the music is played in. Measuring on the
    // beat alone would refuse a perfectly gridded track whose eighths are
    // played, which is most of them.
    {
        double bestCoherence = 0.0;
        for (int32_t subdivision : {1, 2, 4}) {
            const int64_t period = best.period / subdivision;
            if (period < 2) continue;
            bestCoherence =
                std::max(bestCoherence, CoherenceOf(window, period));
        }
        grid.coherence = bestCoherence;
        grid.subdivision = 1;
        for (int32_t subdivision : {1, 2, 4}) {
            const int64_t period = best.period / subdivision;
            if (period < 2) continue;
            if (CoherenceOf(window, period) >= 0.98 * bestCoherence) {
                grid.subdivision = subdivision;
                break;
            }
        }
    }

    // Beats, residuals and support over the whole window.
    const int64_t tolerance = ToleranceOf(best.period, settings);
    std::vector<int64_t> absolutes;
    LevelSupport beatSupport;
    LevelSupport midpointSupport;
    std::vector<TempoGridBeat> beats;
    for (int64_t index = grid.first_index; index <= grid.last_index; ++index) {
        TempoGridBeat beat;
        beat.index = index;
        beat.sample = best.phase + index * best.period;
        int64_t residual = 0;
        double strength = 0.0;
        ++beatSupport.slots;
        if (NearestOnset(window, beat.sample, tolerance, residual, strength)) {
            beat.matched = true;
            beat.residual_samples = residual;
            absolutes.push_back(std::llabs(residual));
            ++beatSupport.matched;
            beatSupport.strength_sum += strength;
        }
        ++midpointSupport.slots;
        if (NearestOnset(window, beat.sample + best.period / 2, tolerance,
                         residual, strength)) {
            ++midpointSupport.matched;
            midpointSupport.strength_sum += strength;
        }
        beats.push_back(beat);
    }
    grid.beat_support_percent = beatSupport.percent();
    grid.subdivision_support_percent = midpointSupport.percent();
    grid.residuals.total_beats = beatSupport.slots;
    grid.residuals.matched_beats = beatSupport.matched;
    if (!absolutes.empty()) {
        std::sort(absolutes.begin(), absolutes.end());
        grid.residuals.median_absolute_samples = Median(absolutes);
        grid.residuals.percentile90_absolute_samples =
            Percentile(absolutes, 90);
        grid.residuals.maximum_absolute_samples = absolutes.back();
    }

    // Refusal. A free-tempo take is reported with everything that was
    // measured on it, and with no beats: it is not gridded by force.
    const int64_t maximumResidual =
        std::max<int64_t>(1, best.period * settings.maximum_residual_num /
                                 settings.maximum_residual_den);
    if (grid.coherence < settings.minimum_coherence) {
        grid.status = TempoGridStatus::FreeTempo;
        grid.refusal = "onsets are not on a grid (coherence below threshold)";
        return grid;
    }
    if (grid.beat_support_percent < settings.minimum_beat_support_percent) {
        grid.status = TempoGridStatus::FreeTempo;
        grid.refusal = "too few beats of the window carry an attack";
        return grid;
    }
    if (grid.residuals.median_absolute_samples > maximumResidual) {
        grid.status = TempoGridStatus::FreeTempo;
        grid.refusal = "median residual exceeds the grid tolerance";
        return grid;
    }

    // Bars, only once the author has declared them (garde-fou 4).
    grid.beats_per_bar = std::max<int32_t>(0, bars.beats_per_bar);
    grid.has_origin = bars.has_origin;
    if (bars.has_origin) {
        const int64_t originSample = RoundSamples(bars.origin, rate);
        grid.origin_index = NearestIndex(originSample, best.phase, best.period);
        grid.origin_snap_samples =
            originSample - (best.phase + grid.origin_index * best.period);
    }
    if (grid.beats_per_bar > 0 && grid.has_origin) {
        for (TempoGridBeat& beat : beats) {
            const int64_t offset = beat.index - grid.origin_index;
            const int64_t bar = FloorDivide(offset, grid.beats_per_bar);
            beat.bar = static_cast<int32_t>(bar + 1);
            beat.beat_in_bar =
                static_cast<int32_t>(offset - bar * grid.beats_per_bar) + 1;
        }
    }
    grid.beats = std::move(beats);
    grid.status = TempoGridStatus::Fitted;
    return grid;
}

bool TempoBeatToSequenceFrame(const RationalTime& timelineTime,
                              const MediaRate& rate, int64_t& frame,
                              RationalTime& offset, std::string& error) {
    if (rate.num <= 0 || rate.den <= 0) {
        error = "sequence frame rate must be positive";
        return false;
    }
    // The single rounding point of R7: a beat is an exact time until here.
    frame = timelineTime.to_frames(rate.num, rate.den);
    const RationalTime start(frame * rate.den, rate.num);
    offset = timelineTime.sub(start);
    return true;
}

RationalTime TempoSourceToTimeline(const DocumentClip& clip,
                                   const RationalTime& sourceTime) {
    // R3 -- vitesse de plan. When DocumentClip carries an exact speed
    // fraction, this elapsed source time is what gets divided by it; nothing
    // else in R7 touches the source-to-timeline mapping. No speed exists in
    // the document today, so the mapping is one to one.
    return clip.timeline_in.add(sourceTime.sub(clip.source_in));
}

bool TempoBeatFramesOnTimeline(const Document& document, const TempoGrid& grid,
                               const Ulid& mediaId, int64_t beatIndex,
                               std::vector<int64_t>& frames,
                               std::string& error) {
    frames.clear();
    error.clear();
    if (!grid.accepted()) {
        error = "the tempo grid was refused: " + grid.refusal;
        return false;
    }
    const bool held = std::any_of(
        grid.beats.begin(), grid.beats.end(),
        [&](const TempoGridBeat& beat) { return beat.index == beatIndex; });
    if (!held) {
        error = "beat " + std::to_string(beatIndex) +
                " is not in the grid's window";
        return false;
    }
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.muted) continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip plays no beat (ClipEnabled.h).
            if (clip.source_id != mediaId || !ClipIsHeard(clip)) continue;
            TempoGridClipBeats placed;
            if (!ProjectTempoGridOntoClip(
                    grid, clip, document.sequence.frame_rate, placed, error))
                return false;
            for (const TempoGridSequenceBeat& beat : placed.beats)
                if (beat.index == beatIndex) frames.push_back(beat.frame);
        }
    }
    std::sort(frames.begin(), frames.end());
    frames.erase(std::unique(frames.begin(), frames.end()), frames.end());
    return true;
}

bool ProjectTempoGridOntoClip(const TempoGrid& grid, const DocumentClip& clip,
                              const MediaRate& sequenceRate,
                              TempoGridClipBeats& out, std::string& error) {
    error.clear();
    if (!grid.accepted()) {
        error = "the tempo grid was refused: " + grid.refusal;
        return false;
    }
    if (sequenceRate.num <= 0 || sequenceRate.den <= 0) {
        error = "sequence frame rate must be positive";
        return false;
    }
    if (clip.source_id.empty()) {
        error = "clip '" + clip.id + "' reads a composition, not a media";
        return false;
    }
    out = TempoGridClipBeats{};
    out.clip_id = clip.id;
    out.source_id = clip.source_id;
    out.source_in = clip.source_in;
    out.timeline_in = clip.timeline_in;
    out.duration = clip.duration;
    // R3 -- with a speed, the source range a clip shows is its duration
    // multiplied by that speed; today the two are the same length.
    const RationalTime sourceOut = clip.source_in.add(clip.duration);
    for (const TempoGridBeat& beat : grid.beats) {
        const RationalTime sourceTime(beat.sample, grid.sample_rate);
        if (sourceTime < clip.source_in || !(sourceTime < sourceOut)) continue;
        TempoGridSequenceBeat placed;
        placed.index = beat.index;
        placed.bar = beat.bar;
        placed.beat_in_bar = beat.beat_in_bar;
        placed.source_time = sourceTime;
        placed.timeline_time = TempoSourceToTimeline(clip, sourceTime);
        if (!TempoBeatToSequenceFrame(placed.timeline_time, sequenceRate,
                                      placed.frame, placed.frame_offset,
                                      error)) {
            return false;
        }
        placed.frame_offset_samples =
            FloorSamples(placed.frame_offset, grid.sample_rate);
        out.beats.push_back(placed);
    }
    return true;
}

std::vector<TempoGridJunction> TempoGridJunctions(
    const TempoGrid& grid, const std::vector<DocumentClip>& clips) {
    std::vector<TempoGridJunction> junctions;
    if (!grid.accepted() || grid.period_samples <= 0) return junctions;
    for (size_t index = 1; index < clips.size(); ++index) {
        const DocumentClip& before = clips[index - 1];
        const DocumentClip& after = clips[index];
        if (before.source_id.empty() || before.source_id != after.source_id) {
            continue;
        }
        if (before.timeline_in.add(before.duration)
                .compare(after.timeline_in) != 0) {
            continue;  // Not a shared cut: nothing joins here.
        }
        int64_t beforeIndex = 0;
        int64_t beforeRemainder = 0;
        int64_t afterIndex = 0;
        int64_t afterRemainder = 0;
        GridPositionOf(grid, before.source_in.add(before.duration), beforeIndex,
                       beforeRemainder);
        GridPositionOf(grid, after.source_in, afterIndex, afterRemainder);

        TempoGridJunction junction;
        junction.before_clip_id = before.id;
        junction.after_clip_id = after.id;
        // The grid's phase on the timeline jumps by the difference of the
        // two source remainders, reduced to the half-period on either side.
        int64_t offset =
            (beforeRemainder - afterRemainder) % grid.period_samples;
        if (offset * 2 > grid.period_samples) offset -= grid.period_samples;
        if (offset * 2 <= -grid.period_samples) offset += grid.period_samples;
        junction.sample_offset = offset;
        junction.continuous = offset == 0;
        const int64_t beforeBeat =
            beforeIndex + (beforeRemainder * 2 >= grid.period_samples ? 1 : 0);
        const int64_t afterBeat =
            afterIndex + (afterRemainder * 2 >= grid.period_samples ? 1 : 0);
        junction.beat_offset = afterBeat - beforeBeat;
        if (grid.beats_per_bar > 0) {
            const int64_t phase =
                junction.beat_offset -
                FloorDivide(junction.beat_offset, grid.beats_per_bar) *
                    grid.beats_per_bar;
            junction.bar_phase_offset = static_cast<int32_t>(phase);
        }
        junctions.push_back(junction);
    }
    return junctions;
}
