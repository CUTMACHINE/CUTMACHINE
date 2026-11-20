#pragma once

#include "Document.h"
#include "Operations.h"
#include "SpeechOnset.h"

#include <cstdint>
#include <string>
#include <vector>

// B4 -- ROADMAP.md. Boundary silence is measured on B1's envelope, never
// inferred from word timestamps. The defaults preserve three source frames
// around an attack/decay and ignore a boundary shorter than an ordinary
// breath.
//
// AGENT-2026-09 A9 -- the voice is the clip's own speech level times
// `speech_thresholds.speech_ratio_percent`, the line tighten_pauses and
// list_source_words read; any voiced window ends the air, since a short
// burst may be a short word. The persisted speech groups this read at first
// sit 6 dB above the noise floor, on purpose (a quiet question must stay
// visible to the onset tools), and heard a breath or the room as speech:
// the junction tools found no air where tighten_pauses measured some, on
// every junction of a real montage.
struct BoundaryAirSettings {
    int64_t keep_frames = 3;
    int64_t minimum_air_milliseconds = 300;
    SpeechOnsetThresholds speech_thresholds;
};

struct BoundaryAirReport {
    RationalTime head_air{0, 1};
    RationalTime tail_air{0, 1};
    RationalTime head_removed{0, 1};
    RationalTime tail_removed{0, 1};
};

struct JunctionAirReport {
    RationalTime left_tail_air{0, 1};
    RationalTime right_head_air{0, 1};
    RationalTime combined_air{0, 1};
    RationalTime left_removed{0, 1};
    RationalTime right_removed{0, 1};
};

// Resolves both boundaries of one clip into one atomic operation. A boundary
// under minimum_air_milliseconds, or already no longer than keep_frames,
// remains represented in the report but produces no trim.
bool ResolveBoundaryAir(const DocumentClip& clip,
                        const SpeechOnsetReport& envelope,
                        const BoundaryAirSettings& settings,
                        const MediaRate& sourceRate,
                        const std::vector<Ulid>& syncTrackIds,
                        TrimBoundaryAirOperation& operation,
                        BoundaryAirReport& report, std::string& error);

// Resolves the outgoing tail and incoming head together. The minimum applies
// to their sum, because that is the silence the listener hears across the
// splice. Both deltas are emitted together, which makes a second call a no-op
// instead of moving the junction back and forth over successive passes.
bool ResolveJunctionAir(
    const DocumentClip& left, const SpeechOnsetReport& leftEnvelope,
    const MediaRate& leftSourceRate, const DocumentClip& right,
    const SpeechOnsetReport& rightEnvelope, const MediaRate& rightSourceRate,
    const BoundaryAirSettings& settings, const std::vector<Ulid>& syncTrackIds,
    TrimBoundaryAirOperation& operation, JunctionAirReport& report,
    std::string& error);

// Editing default shared by CLI and MCP: every other sync-locked track
// ripples by the same total.
//
// BENCH-2026-09 -- the members that follow each trim are no longer the
// whole link group but LinkedEdgePartnerIds (Operations.h), the rule
// ripple_trim already used. Once a pair has been cut into fragments sharing
// one link id, the whole group is every fragment of the block: a 5-frame
// head trim on one fragment shortened all of them and left holes in
// picture and sound.
std::vector<Ulid> BoundarySyncTrackIds(
    const Document& document, const std::vector<BoundaryAirTrim>& trims);

std::string SerializeBoundaryAirReport(const BoundaryAirReport& report);
std::string SerializeJunctionAirReport(const JunctionAirReport& report);
