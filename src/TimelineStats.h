#pragma once

#include "Document.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// QC-2026-09 (A8) -- editorial density is a property of the composited
// timeline, not of the number of rectangles in its tracks.  Keep this pure so
// the CLI, MCP and a future UI all ask the same question of the document.
struct TimelineStatsRatio {
    int64_t numerator = 0;
    int64_t denominator = 1;
};

struct TimelineStats {
    RationalTime duration;
    int64_t visible_shots = 0;
    int64_t visible_cuts = 0;
    TimelineStatsRatio plans_per_minute;
    int64_t cutaway_plans = 0;
    TimelineStatsRatio cutaway_share;
    // The first visible cutaway, not merely the first edit on the primary
    // track. This is the editorial "première coupe" measured by A8.
    std::optional<RationalTime> first_cut;
    // MOTION-2026-09 M4 -- visible motion clips (titles, names on screen):
    // drawn over the shots, so neither shots nor cuts above.
    int64_t motion_clips = 0;
    // AGENT-2026-09 A5 -- ROADMAP.md. Picture and sound of one link group
    // that overlap on the timeline must read their common source at the
    // same offset (source_in minus timeline_in); a pair that does not is
    // out of sync by the difference. The bench measured this by hand and
    // every run's brief asks for it; here the engine counts it.
    struct SyncDrift {
        Ulid video_clip_id;
        Ulid audio_clip_id;
        // Video offset minus audio offset: positive when the picture runs
        // ahead of its sound.
        RationalTime drift;
    };
    int64_t linked_pairs = 0;
    std::vector<SyncDrift> out_of_sync;
};

// Computes statistics from the active timeline.  Video is composited from
// low to high track index, so an interval covered by a higher visible track
// is attributed only to that top clip.  Invalid clip times are refused with a
// stable message rather than being repaired or rounded.
bool CalculateTimelineStats(const Document& document, TimelineStats& output,
                            std::string& error);

// Compact canonical JSON used by both headless surfaces.
std::string SerializeTimelineStats(const TimelineStats& stats);
