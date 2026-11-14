#include "TempoGrid.h"

#include "BeatDetection.h"
#include "Document.h"
#include "RationalTime.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

// R7 -- docs/roadmap/REEL-2026-09.md. Everything here is plain C++: no
// AppKit, no Metal, no FFmpeg, and no audio is decoded. The ticket says the
// grid is derived from the onsets already cached, so the fixtures are onset
// lists, built in the test, exactly as DetectAudioBeats would have handed
// them over.

namespace {

int gFailures = 0;

void Check(bool condition, const std::string& what) {
    if (condition) return;
    std::cerr << "FAIL: " << what << '\n';
    ++gFailures;
}

constexpr int32_t kRate = 48000;
constexpr double kBpm = 121.0;

// Exact period of a 121.0 BPM track at 48 kHz: 23801.65... samples. The
// engine must land on an integer sample period within 0.05 % of it.
double TruePeriod(double bpm) {
    return 60.0 * static_cast<double>(kRate) / bpm;
}

// A reproducible jitter, so the fixture is not a perfectly clean grid and
// the test is still identical from one run to the next.
struct Jitter {
    uint64_t state = 0x9e3779b97f4a7c15ULL;

    int64_t Next(int64_t amplitude) {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        const uint64_t bits = (state >> 33);
        return static_cast<int64_t>(bits % (2 * amplitude + 1)) - amplitude;
    }
};

AudioOnset MakeOnset(int64_t sample, float strength) {
    AudioOnset onset;
    onset.time = RationalTime(sample, kRate);
    onset.strength = strength;
    return onset;
}

void SortOnsets(std::vector<AudioOnset>& onsets) {
    std::sort(onsets.begin(), onsets.end(),
              [](const AudioOnset& left, const AudioOnset& right) {
                  return left.time.value < right.time.value;
              });
}

// Criterion 1's fixture: a click on every beat, a bass note on the off-beat
// for the first two thirds, and one section played in half-time (beats 1 and
// 3 only, and no bass).
std::vector<AudioOnset> ClickTrack(int beats, int64_t phase) {
    std::vector<AudioOnset> onsets;
    Jitter jitter;
    const double period = TruePeriod(kBpm);
    const int64_t amplitude = kRate / 1000;  // +/- 1 ms.
    for (int beat = 0; beat < beats; ++beat) {
        const int64_t exact =
            phase + static_cast<int64_t>(std::llround(beat * period));
        const bool halfTimeSection = beat >= 160 && beat < 200;
        if (halfTimeSection && (beat % 2) != 0) continue;
        onsets.push_back(MakeOnset(exact + jitter.Next(amplitude), 1.0f));
        if (halfTimeSection || beat >= 160) continue;
        const int64_t offBeat =
            phase + static_cast<int64_t>(std::llround((beat + 0.5) * period));
        onsets.push_back(MakeOnset(offBeat + jitter.Next(amplitude), 0.45f));
    }
    SortOnsets(onsets);
    return onsets;
}

// A canonical, integer-only rendering of a grid. Two fits of the same onsets
// must produce the same characters: the ticket asks for byte-identical
// results from one pass to the next, and this is what is compared.
std::string CanonicalGrid(const TempoGrid& grid) {
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << TempoGridStatusName(grid.status) << '|' << grid.sample_rate << '|'
        << grid.period_samples << '|' << grid.phase_samples << '|'
        << grid.first_index << '|' << grid.last_index << '|'
        << grid.stable_first_index << '|' << grid.stable_last_index << '|'
        << grid.subdivision << '|' << grid.beat_support_percent << '|'
        << grid.subdivision_support_percent << '|' << grid.doublings << '|'
        << grid.halvings << '|' << grid.candidate_period_samples << '|'
        << grid.residuals.total_beats << '|' << grid.residuals.matched_beats
        << '|' << grid.residuals.median_absolute_samples << '|'
        << grid.residuals.percentile90_absolute_samples << '|'
        << grid.residuals.maximum_absolute_samples << '|' << grid.origin_index
        << '|' << grid.origin_snap_samples;
    for (const TempoGridBeat& beat : grid.beats) {
        out << ';' << beat.index << ',' << beat.sample << ','
            << (beat.matched ? 1 : 0) << ',' << beat.residual_samples << ','
            << beat.bar << ',' << beat.beat_in_bar;
    }
    return out.str();
}

double Milliseconds(int64_t samples) {
    return 1000.0 * static_cast<double>(samples) / kRate;
}

// ---------------------------------------------------------------------
// Criterion 1 -- a 121 BPM track, never read in half-time.
// ---------------------------------------------------------------------
void RunSyntheticTrack() {
    const int64_t phase = 7919;  // Not a round number, and not on a frame.
    const std::vector<AudioOnset> onsets = ClickTrack(256, phase);
    const TempoGridSettings settings;
    TempoBarSettings bars;
    bars.beats_per_bar = 4;
    bars.has_origin = true;
    bars.origin = RationalTime(phase, kRate);

    const RationalTime windowIn(0, kRate);
    const RationalTime windowOut(onsets.back().time.value + kRate, kRate);
    const TempoGrid grid =
        FitTempoGrid(onsets, windowIn, windowOut, settings, bars);

    Check(grid.accepted(), std::string("121 BPM click track is accepted: ") +
                               TempoGridStatusName(grid.status) + ' ' +
                               grid.refusal);
    if (!grid.accepted()) return;

    const double truePeriod = TruePeriod(kBpm);
    const double error =
        std::fabs(static_cast<double>(grid.period_samples) - truePeriod) /
        truePeriod;
    Check(error <= 0.0005,
          "period within 0.05 % (" + std::to_string(grid.period_samples) +
              " samples, error " + std::to_string(error * 100.0) + " %)");

    // Never the half-time reading, and never the off-beat subdivision.
    Check(std::fabs(grid.bpm() - kBpm) < 1.0,
          "tempo is 121 BPM, not its half or its double: " +
              std::to_string(grid.bpm()));

    // Phase: the distance from the fitted grid to the true click grid, taken
    // at the beat nearest the middle of the track so any period error shows.
    const int64_t middle = (grid.first_index + grid.last_index) / 2;
    const int64_t fitted = grid.phase_samples + middle * grid.period_samples;
    const int64_t truth =
        phase + static_cast<int64_t>(std::llround(middle * truePeriod));
    Check(std::llabs(fitted - truth) <= 5 * kRate / 1000,
          "phase within 5 ms of the click grid (" +
              std::to_string(Milliseconds(fitted - truth)) + " ms)");

    // The off-beat bass never moved the metrical level, and the engine says
    // the music is played in eighths rather than pretending it is not.
    Check(grid.halvings == 0 && grid.doublings == 0,
          "an off-beat bass needs no arbitration to be left alone");
    Check(grid.subdivision == 2, "the played subdivision is reported: " +
                                     std::to_string(grid.subdivision));
    Check(grid.coherence > 0.9,
          "the grid is coherent: " + std::to_string(grid.coherence));

    Check(grid.residuals.matched_beats > 200,
          "most beats carry an attack: " +
              std::to_string(grid.residuals.matched_beats) + '/' +
              std::to_string(grid.residuals.total_beats));
    Check(Milliseconds(grid.residuals.median_absolute_samples) <= 10.0,
          "median residual under 10 ms: " +
              std::to_string(
                  Milliseconds(grid.residuals.median_absolute_samples)) +
              " ms");

    // The author declared the downbeat and the bar length; the engine
    // numbered from there and guessed nothing.
    Check(grid.beats_per_bar == 4 && grid.has_origin,
          "the declared bar reaches the grid");
    const TempoGridBeat* origin = nullptr;
    for (const TempoGridBeat& beat : grid.beats) {
        if (beat.index == grid.origin_index) origin = &beat;
    }
    Check(origin != nullptr && origin->bar == 1 && origin->beat_in_bar == 1,
          "the declared origin is bar 1 beat 1");
    for (const TempoGridBeat& beat : grid.beats) {
        Check(beat.beat_in_bar >= 1 && beat.beat_in_bar <= 4,
              "every beat is numbered inside its bar");
        if (beat.beat_in_bar < 1 || beat.beat_in_bar > 4) break;
    }

    // Byte-for-byte determinism: a second fit of the same onsets.
    const TempoGrid again =
        FitTempoGrid(onsets, windowIn, windowOut, settings, bars);
    Check(CanonicalGrid(grid) == CanonicalGrid(again),
          "two passes over the same onsets write the same grid");

    // And the same when the onsets are handed over as a fresh copy, so the
    // result cannot depend on an address or on iteration order.
    const std::vector<AudioOnset> copy = onsets;
    const TempoGrid third =
        FitTempoGrid(copy, windowIn, windowOut, settings, bars);
    Check(CanonicalGrid(grid) == CanonicalGrid(third),
          "a copy of the onsets fits to the same grid");

    // Without a declared origin, no bar is numbered.
    const TempoGrid unnumbered =
        FitTempoGrid(onsets, windowIn, windowOut, settings, TempoBarSettings{});
    Check(unnumbered.accepted() && unnumbered.beats_per_bar == 0,
          "the engine numbers no bar the author did not declare");
    bool numbered = false;
    for (const TempoGridBeat& beat : unnumbered.beats) {
        if (beat.bar != 0 || beat.beat_in_bar != 0) numbered = true;
    }
    Check(!numbered, "an undeclared bar leaves bar and beat_in_bar at zero");
}

// ---------------------------------------------------------------------
// The arbitration itself: the attacks between two candidate beats decide.
// Both directions are provoked by pinning the tempo preference on the wrong
// metrical level, which is the only thing that can put a candidate there.
// ---------------------------------------------------------------------
void RunHalfAndDoubleTimeArbitration() {
    const int64_t phase = 4001;
    const double period = TruePeriod(kBpm);

    // Double-time: click on the beat, bass between. Preference pinned on the
    // off-beat subdivision (242 BPM). The bass is weaker than the click, so
    // the level it fills is a subdivision, not the pulse.
    {
        std::vector<AudioOnset> onsets;
        for (int beat = 0; beat < 240; ++beat) {
            const int64_t exact =
                phase + static_cast<int64_t>(std::llround(beat * period));
            onsets.push_back(MakeOnset(exact, 1.0f));
            onsets.push_back(MakeOnset(
                phase +
                    static_cast<int64_t>(std::llround((beat + 0.5) * period)),
                0.40f));
        }
        SortOnsets(onsets);
        TempoGridSettings settings;
        settings.maximum_bpm = 300.0;
        settings.preferred_bpm = 2.0 * kBpm;
        const TempoGrid grid =
            FitTempoGrid(onsets, RationalTime(0, kRate),
                         RationalTime(onsets.back().time.value + kRate, kRate),
                         settings, TempoBarSettings{});
        Check(grid.accepted() && std::fabs(grid.bpm() - kBpm) < 1.0,
              "an off-beat bass does not become the pulse: " +
                  std::to_string(grid.bpm()) + " BPM, " +
                  TempoGridStatusName(grid.status));
        Check(grid.doublings >= 1, "the period was doubled back onto the beat");
    }

    // Half-time: every beat played, at nearly the same strength. Preference
    // pinned hard on 60.5 BPM, which is the failure the ticket reports. The
    // midpoints of that candidate are played just as hard, so it is halved.
    {
        std::vector<AudioOnset> onsets;
        for (int beat = 0; beat < 240; ++beat) {
            const int64_t exact =
                phase + static_cast<int64_t>(std::llround(beat * period));
            onsets.push_back(MakeOnset(exact, beat % 2 == 0 ? 1.0f : 0.98f));
        }
        TempoGridSettings settings;
        settings.minimum_bpm = 50.0;
        settings.preferred_bpm = kBpm / 2.0;
        settings.preference_octaves = 0.05;
        const TempoGrid grid =
            FitTempoGrid(onsets, RationalTime(0, kRate),
                         RationalTime(onsets.back().time.value + kRate, kRate),
                         settings, TempoBarSettings{});
        Check(
            grid.accepted() && std::fabs(grid.bpm() - kBpm) < 1.0,
            "the half-time reading is refused: " + std::to_string(grid.bpm()) +
                " BPM, " + TempoGridStatusName(grid.status));
        Check(grid.halvings >= 1, "the period was halved back onto the beat");
    }
}

// ---------------------------------------------------------------------
// Criterion 3 -- a free-tempo take is refused with its residuals.
// ---------------------------------------------------------------------
void RunFreeTempoIsRefused() {
    std::vector<AudioOnset> onsets;
    Jitter jitter;
    double position = 12000.0;
    // A continuous accelerando from 60 to 120 BPM over 200 attacks: never a
    // period, never a phase, and nothing a grid can be forced onto.
    for (int beat = 0; beat < 200; ++beat) {
        const double bpm = 60.0 + 60.0 * beat / 200.0;
        onsets.push_back(
            MakeOnset(static_cast<int64_t>(std::llround(position)), 0.9f));
        position += TruePeriod(bpm) + static_cast<double>(jitter.Next(400));
    }
    SortOnsets(onsets);
    const TempoGrid grid =
        FitTempoGrid(onsets, RationalTime(0, kRate),
                     RationalTime(onsets.back().time.value + kRate, kRate),
                     TempoGridSettings{}, TempoBarSettings{});
    Check(!grid.accepted(), "a free-tempo take is not gridded");
    Check(grid.status == TempoGridStatus::FreeTempo,
          std::string("refused as free tempo, not as a missing period: ") +
              TempoGridStatusName(grid.status));
    Check(!grid.refusal.empty(), "the refusal says why");
    Check(grid.residuals.total_beats > 0,
          "the refusal carries the residuals it was measured on");
    Check(grid.beats.empty(), "a refused grid publishes no beats");

    std::string error;
    DocumentClip clip;
    clip.source_id = "01K30000000000000000000001";
    TempoGridClipBeats placed;
    Check(
        !ProjectTempoGridOntoClip(grid, clip, MediaRate{25, 1}, placed, error),
        "a refused grid cannot be projected onto a clip");
    Check(error.find("refused") != std::string::npos,
          "the projection repeats the refusal: " + error);
}

// ---------------------------------------------------------------------
// The grid in the sequence: one rounding point, and the gap published.
// ---------------------------------------------------------------------
TempoGrid FittedTrack(int64_t phase, int beats) {
    const std::vector<AudioOnset> onsets = ClickTrack(beats, phase);
    TempoBarSettings bars;
    bars.beats_per_bar = 4;
    bars.has_origin = true;
    bars.origin = RationalTime(phase, kRate);
    return FitTempoGrid(onsets, RationalTime(0, kRate),
                        RationalTime(onsets.back().time.value + kRate, kRate),
                        TempoGridSettings{}, bars);
}

void RunSequenceProjection() {
    const TempoGrid grid = FittedTrack(7919, 256);
    Check(grid.accepted(), "the projection fixture fits");
    if (!grid.accepted()) return;

    DocumentClip clip;
    clip.id = "01K30000000000000000000011";
    clip.source_id = "01K30000000000000000000001";
    clip.source_in = RationalTime(100, 25);   // 4 s into the track.
    clip.duration = RationalTime(250, 25);    // 10 s long.
    clip.timeline_in = RationalTime(50, 25);  // Posed at 2 s.
    const MediaRate sequenceRate{25, 1};

    TempoGridClipBeats placed;
    std::string error;
    Check(ProjectTempoGridOntoClip(grid, clip, sequenceRate, placed, error),
          "the grid projects onto the clip: " + error);
    Check(!placed.beats.empty(), "the clip shows some beats");

    const RationalTime sourceOut = clip.source_in.add(clip.duration);
    const RationalTime timelineOut = clip.timeline_in.add(clip.duration);
    for (const TempoGridSequenceBeat& beat : placed.beats) {
        Check(!(beat.source_time < clip.source_in) &&
                  beat.source_time < sourceOut,
              "a projected beat is inside the clip's source range");
        Check(!(beat.timeline_time < clip.timeline_in) &&
                  beat.timeline_time < timelineOut,
              "a projected beat is inside the clip on the timeline");
        // The frame is the one that contains the beat: the beat sits at or
        // after its start and strictly before the next.
        const RationalTime start(beat.frame * sequenceRate.den,
                                 sequenceRate.num);
        const RationalTime next((beat.frame + 1) * sequenceRate.den,
                                sequenceRate.num);
        Check(!(beat.timeline_time < start) && beat.timeline_time < next,
              "frame " + std::to_string(beat.frame) + " contains its beat");
        Check(beat.frame_offset.compare(RationalTime(0, 1)) >= 0 &&
                  beat.frame_offset <
                      RationalTime(sequenceRate.den, sequenceRate.num),
              "the published gap is inside one frame");
        Check(beat.frame_offset_samples >= 0 &&
                  beat.frame_offset_samples <
                      kRate * sequenceRate.den / sequenceRate.num,
              "the gap in samples agrees with the gap in exact time");
    }

    // The mapping and the rounding are the same two calls the projection
    // makes, so a caller that wants one beat gets the same frame.
    const TempoGridSequenceBeat& first = placed.beats.front();
    const RationalTime mapped = TempoSourceToTimeline(clip, first.source_time);
    int64_t frame = 0;
    RationalTime offset{0, 1};
    Check(
        TempoBeatToSequenceFrame(mapped, sequenceRate, frame, offset, error) &&
            frame == first.frame && offset.compare(first.frame_offset) == 0,
        "the single rounding point answers the same frame");

    Check(!TempoBeatToSequenceFrame(mapped, MediaRate{0, 1}, frame, offset,
                                    error),
          "an impossible frame rate is refused, not rounded");

    // A sequence at 30000/1001 rounds through the same one place.
    TempoGridClipBeats ntsc;
    Check(ProjectTempoGridOntoClip(grid, clip, MediaRate{30000, 1001}, ntsc,
                                   error),
          "the grid projects into a 30000/1001 sequence: " + error);
    Check(ntsc.beats.size() == placed.beats.size(),
          "the same beats are shown whatever the sequence rate");
    for (const TempoGridSequenceBeat& beat : ntsc.beats) {
        const RationalTime start(beat.frame * 1001, 30000);
        const RationalTime next((beat.frame + 1) * 1001, 30000);
        Check(!(beat.timeline_time < start) && beat.timeline_time < next,
              "a 30000/1001 frame contains its beat");
    }
}

// ---------------------------------------------------------------------
// Criterion 4 -- two clips of one track cut together.
// ---------------------------------------------------------------------
void RunJunctionContinuity() {
    const TempoGrid grid = FittedTrack(4409, 512);
    Check(grid.accepted(), "the junction fixture fits");
    if (!grid.accepted()) return;

    const int64_t period = grid.period_samples;
    const int64_t phase = grid.phase_samples;
    const auto beatTime = [&](int64_t index) {
        return RationalTime(phase + index * period, kRate);
    };

    DocumentClip before;
    before.id = "01K30000000000000000000021";
    before.source_id = "01K30000000000000000000001";
    before.source_in = beatTime(40);
    before.duration = RationalTime(64 * period, kRate);
    before.timeline_in = RationalTime(0, kRate);

    DocumentClip after = before;
    after.id = "01K30000000000000000000022";
    after.source_in = beatTime(200);  // 96 beats further into the track.
    after.duration = RationalTime(32 * period, kRate);
    after.timeline_in = before.timeline_in.add(before.duration);

    std::vector<TempoGridJunction> junctions =
        TempoGridJunctions(grid, {before, after});
    Check(junctions.size() == 1, "one junction between two adjacent clips");
    if (junctions.size() == 1) {
        Check(junctions[0].continuous,
              "two clips cut at the same position of the phrase run "
              "continuously");
        Check(junctions[0].sample_offset == 0,
              "a continuous junction names no offset");
        Check(junctions[0].beat_offset == 96,
              "the junction says how many beats the track skipped: " +
                  std::to_string(junctions[0].beat_offset));
        Check(junctions[0].bar_phase_offset == 0,
              "96 beats of a 4-beat bar land on the same beat of the bar");
    }

    // The same cut, three thousand samples out: the offset is named.
    DocumentClip drifted = after;
    drifted.source_in = RationalTime(after.source_in.value + 3000, kRate);
    junctions = TempoGridJunctions(grid, {before, drifted});
    Check(junctions.size() == 1, "a drifted junction is still a junction");
    if (junctions.size() == 1) {
        Check(!junctions[0].continuous,
              "the drifted junction is not continuous");
        Check(junctions[0].sample_offset == -3000,
              "the drift is named in samples: " +
                  std::to_string(junctions[0].sample_offset));
    }

    // Half a bar out: the phrase does not line up either.
    DocumentClip halfBar = after;
    halfBar.source_in = beatTime(202);
    junctions = TempoGridJunctions(grid, {before, halfBar});
    Check(junctions.size() == 1 && junctions[0].continuous &&
              junctions[0].bar_phase_offset == 2,
          "a cut two beats off keeps the pulse but moves inside the bar");

    // Clips that do not share a cut produce no junction at all.
    DocumentClip apart = after;
    apart.timeline_in = after.timeline_in.add(RationalTime(1000, kRate));
    Check(TempoGridJunctions(grid, {before, apart}).empty(),
          "clips that do not share a cut have no junction");
}

// ---------------------------------------------------------------------
// Refusals that are not about tempo.
// ---------------------------------------------------------------------
void RunEmptyAndShortInputs() {
    const TempoGridSettings settings;
    const TempoBarSettings bars;
    const TempoGrid none = FitTempoGrid(
        {}, RationalTime(0, kRate), RationalTime(kRate, kRate), settings, bars);
    Check(none.status == TempoGridStatus::TooFewOnsets && !none.refusal.empty(),
          "no onsets is refused, not fitted");

    std::vector<AudioOnset> few;
    for (int beat = 0; beat < 5; ++beat) {
        few.push_back(MakeOnset(beat * 24000, 1.0f));
    }
    const TempoGrid sparse =
        FitTempoGrid(few, RationalTime(0, kRate),
                     RationalTime(10 * kRate, kRate), settings, bars);
    Check(sparse.status == TempoGridStatus::TooFewOnsets,
          "five onsets are too few to fit a grid");

    const std::vector<AudioOnset> onsets = ClickTrack(256, 1000);
    const TempoGrid empty =
        FitTempoGrid(onsets, RationalTime(5 * kRate, kRate),
                     RationalTime(5 * kRate, kRate), settings, bars);
    Check(empty.status == TempoGridStatus::TooFewOnsets,
          "an empty window is refused");
}

// ---------------------------------------------------------------------
// A window, and the beat numbering that has to survive it.
// ---------------------------------------------------------------------
void RunWindowedFit() {
    const int64_t phase = 7919;
    const std::vector<AudioOnset> onsets = ClickTrack(256, phase);
    TempoGridSettings settings;
    const TempoGrid whole =
        FitTempoGrid(onsets, RationalTime(0, kRate),
                     RationalTime(onsets.back().time.value + kRate, kRate),
                     settings, TempoBarSettings{});
    const TempoGrid part = FitTempoGrid(onsets, RationalTime(20 * kRate, kRate),
                                        RationalTime(70 * kRate, kRate),
                                        settings, TempoBarSettings{});
    Check(whole.accepted() && part.accepted(),
          "a passage of the same track fits on its own");
    if (!whole.accepted() || !part.accepted()) return;
    Check(std::llabs(whole.period_samples - part.period_samples) <= 8,
          "a passage finds the same period as the whole track");
    // Beat indices count from sample 0, so the two fits number the same
    // attack the same way instead of restarting at the window.
    const int64_t wholeBeat =
        whole.phase_samples + part.first_index * whole.period_samples;
    const int64_t partBeat =
        part.phase_samples + part.first_index * part.period_samples;
    Check(std::llabs(wholeBeat - partBeat) <= 5 * kRate / 1000,
          "the same beat index names the same attack in both fits");
}

}  // namespace

// A title placed on a beat: the engine names the frame, from the beat's
// index alone (TempoBeatFramesOnTimeline).
void RunBeatOnTimeline() {
    const TempoGrid grid = FittedTrack(7919, 256);
    if (!grid.accepted()) return;
    const Ulid music = "01K30000000000000000000001";
    DocumentClip sound;
    sound.id = "01K30000000000000000000021";
    sound.source_id = music;
    sound.source_in = RationalTime(100, 25);
    sound.duration = RationalTime(250, 25);
    sound.timeline_in = RationalTime(50, 25);
    Document document;
    document.sequence.frame_rate = {25, 1};
    DocumentTrack audio{"01K30000000000000000000031", "audio", 1, {sound}};
    DocumentClip picture = sound;
    picture.id = "01K30000000000000000000022";
    DocumentTrack video{"01K30000000000000000000032", "video", 0, {picture}};
    document.sequence.tracks = {video, audio};

    TempoGridClipBeats placed;
    std::string error;
    ProjectTempoGridOntoClip(grid, sound, document.sequence.frame_rate, placed,
                             error);
    if (placed.beats.size() < 3) {
        Check(false, "the beat fixture shows beats");
        return;
    }
    const TempoGridSequenceBeat& third = placed.beats[2];
    std::vector<int64_t> frames;
    Check(TempoBeatFramesOnTimeline(document, grid, music, third.index, frames,
                                    error) &&
              frames == std::vector<int64_t>{third.frame},
          "a beat heard by a picture and its sound is one frame, the one "
          "the projection names: " +
              error);

    Document twice = document;
    DocumentClip again = sound;
    again.id = "01K30000000000000000000023";
    again.timeline_in = RationalTime(400, 25);
    twice.sequence.tracks[1].clips.push_back(again);
    Check(TempoBeatFramesOnTimeline(twice, grid, music, third.index, frames,
                                    error) &&
              frames.size() == 2 && frames[0] == third.frame &&
              frames[1] == third.frame + 350,
          "the music played twice hears the beat on two frames");

    Document muted = document;
    muted.sequence.tracks[0].muted = true;
    muted.sequence.tracks[1].muted = true;
    Check(TempoBeatFramesOnTimeline(muted, grid, music, third.index, frames,
                                    error) &&
              frames.empty(),
          "a muted track hears no beat");
    Check(!TempoBeatFramesOnTimeline(document, grid, music, 1000000, frames,
                                     error) &&
              error.find("not in the grid") != std::string::npos,
          "a beat the grid does not hold is refused: " + error);
}

int main() {
    try {
        RunBeatOnTimeline();
        RunSyntheticTrack();
        RunHalfAndDoubleTimeArbitration();
        RunFreeTempoIsRefused();
        RunSequenceProjection();
        RunJunctionContinuity();
        RunEmptyAndShortInputs();
        RunWindowedFit();
    } catch (const std::exception& failure) {
        std::cerr << "FAIL: tempo grid test threw: " << failure.what() << '\n';
        return 1;
    }
    if (gFailures != 0) {
        std::cerr << gFailures << " tempo grid check(s) failed\n";
        return 1;
    }
    std::cout << "PASS: tempo grid fitted, arbitrated, refused and projected\n";
    return 0;
}
