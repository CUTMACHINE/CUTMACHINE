#pragma once

// Where a track's energy moves, beat by beat: the drop an edit waits for,
// the break where it breathes. Measured, never judged -- the same line
// TempoGrid.h holds for beats and IsolatedFrames.h for flashes: the engine
// says where the bass comes in and by how much, the author decides whether
// to cut there.
//
// Why. Cutting a 78 s fashion reel on a 124 BPM track on 24 September 2026,
// the agent had the beats (R7) but nothing that said where the track opens
// up: it could not hear the drop, and cut the same pace through the intro
// and the chorus. Low-band energy is what moves at a drop -- the kick and
// the bass come in -- so that is the band events are read on; the high and
// the full band are reported beside it.
//
// It is NOT used to place beats. TempoGrid.h explains why: the steepest
// slope of the bass arrives about 88 ms after the audible attack. Here the
// energy is averaged over whole beats of a grid fitted on broadband onsets,
// so that lag moves nothing.
//
// Levels are hundredths of a decibel, relative to the loudest beat of the
// window in the same band: 0 is the loudest, -1200 is 12 dB under it.
// Integers, so two runs give the same bytes. Positions are the grid's own
// exact beat samples.
//
// Portable: no AppKit, no FFmpeg (tests/music_structure_tests.cc). The
// envelope comes from BeatDetection.cc's analysis, which already decodes
// and transforms the track for its onsets.

#include "BeatDetection.h"
#include "TempoGrid.h"

#include <cstdint>
#include <string>
#include <vector>

struct MusicBeatEnergy {
    int64_t index = 0;   // TempoGridBeat::index.
    int64_t sample = 0;  // At TempoGrid::sample_rate.
    int32_t bar = 0;     // Zero when the author declared no bars.
    int32_t beat_in_bar = 0;
    int32_t low_centi_db = 0;
    int32_t high_centi_db = 0;
    int32_t full_centi_db = 0;
};

struct MusicEnergySettings {
    // A change is read between the beats before and the beats after: two
    // bars of four before, so a fill does not count as the level it leaves,
    // and one bar after, so the level it reaches is the one that holds.
    int32_t beats_before = 8;
    int32_t beats_after = 4;
    // 6 dB: twice the power. On the reel's track (124 BPM), the bass came
    // in at 15.2 s by +20.3 dB and left for the break by -21.0 dB; a
    // phrase that only thickens moves it by a few decibels.
    int32_t threshold_centi_db = 600;
};

struct MusicEnergyChange {
    // "rise": the low band comes in (a drop); "fall": it goes (a break).
    std::string kind;
    // The first beat at the new level.
    int64_t index = 0;
    int64_t sample = 0;
    int32_t bar = 0;
    int32_t beat_in_bar = 0;
    // After minus before, over the windows above.
    int32_t low_change_centi_db = 0;
    int32_t full_change_centi_db = 0;
};

// Averages the envelope over each beat of `grid` -- the frames whose window
// centre falls on or after the beat and before the next one -- and puts the
// levels relative to the loudest beat. Fails when the grid was refused, or
// the envelope is at another rate than the grid.
bool MeasureBeatEnergies(const AudioBandEnvelope& bands, const TempoGrid& grid,
                         std::vector<MusicBeatEnergy>& beats,
                         std::string& error);

// Beats where the low band steps up or down by the threshold, keeping only
// the strongest of the beats a single change spreads over.
std::vector<MusicEnergyChange> FindEnergyChanges(
    const std::vector<MusicBeatEnergy>& beats,
    const MusicEnergySettings& settings);
