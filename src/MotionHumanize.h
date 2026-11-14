#pragma once

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. A composition written by a
// machine, rewritten at the level a person would have written it: the teaser
// v6 spelled its timecode as 112 strips of ten digits scrolled a frame at a
// time, and wrapped each letter in a box that only placed it. After the pass
// the Inspector, the JSON and the After export show a timecode and texts.
//
// The lot admits a rewrite only when the picture stays the same, and
// decided on 29 September 2026 to keep that exact: no rule guesses an
// intention, and one that would move a pixel is not applied. The pass
// proposes; CompareMotionRenders proves, by rendering every frame of both
// definitions, and a surface that applies the result refuses it when one
// frame differs. The result is a definition an UpdateMotionComposition
// carries, so one undo takes it back.
//
// The rules --
//   timecode   a row of fixed-width columns, digit strips stepped by
//              translateY and static characters, whose digits read the
//              timecode of every frame the row is shown, becomes one text
//              bound to that timecode, spaced to the columns' width;
//   wrapper    a box that only places one text (position: absolute, left,
//              top) folds into the text, which takes the place and the
//              box's animations;
//   letters    sibling texts of one character each, same style and line,
//              each where the one before it ends by the engine's own
//              advances, animated alike up to a delay per rank, become one
//              text cut into characters, the delay a stagger. Letters
//              placed by other metrics (the v6's, measured with FreeType)
//              land a fraction of a pixel away and stay apart.
// A group that only holds others is not flattened: in the v6 the two such
// groups lay their children out in the flow, which flattening would move.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"
#include "MotionFrameRenderer.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

struct MotionHumanizeReport {
    int64_t nodes_before = 0;
    int64_t nodes_after = 0;
    // Rows of digit strips that now read the timecode.
    int64_t timecodes = 0;
    // Boxes that only placed one text, folded into it.
    int64_t wrappers = 0;
    // Texts made of letters placed one by one, and the letters they took.
    int64_t words = 0;
    int64_t letters = 0;
    // "path: why" for what looked foldable and stays as it is.
    std::vector<std::string> notes;
};

// `definition` (a composition as the document stores it) rewritten by the
// rules above. Text is measured by `measurer`, the style a text resolves to
// read from the evaluated frame. Nodes keep their identity (`uid`) where
// one survives: a folded text keeps its own, a new timecode text has none
// until the operation gives it one.
bool HumanizeMotionDefinition(const mcp_json::Value& definition,
                              MotionMeasurer& measurer,
                              const MotionEvaluationContext& context,
                              mcp_json::Value& output,
                              MotionHumanizeReport& report, MotionError& error);

// What a surface of the model hands the pass: the definition, the project's
// reach (media and the compositions an instance may show), and where
// relative sources and fonts are.
struct MotionHumanizeRequest {
    const mcp_json::Value* definition = nullptr;
    MotionReach reach;
    std::string media_directory;
    std::vector<std::string> font_directories;
};

// Measuring text needs Core Text, which the model does not link: the
// executable installs the pass with its measurer beside the motion
// renderers, as it installs the After describer (MotionAfterExport.h); null
// in a process that links the model alone.
using MotionHumanizer = bool (*)(const MotionHumanizeRequest& request,
                                 mcp_json::Value& output,
                                 MotionHumanizeReport& report,
                                 MotionError& error);

namespace motion_humanizer {
inline std::atomic<MotionHumanizer>& Slot() {
    static std::atomic<MotionHumanizer> humanizer{nullptr};
    return humanizer;
}
}  // namespace motion_humanizer

inline void SetMotionHumanizer(MotionHumanizer humanizer) {
    motion_humanizer::Slot().store(humanizer);
}

inline MotionHumanizer CurrentMotionHumanizer() {
    return motion_humanizer::Slot().load();
}

// The report as the CLI and humanize_motion_composition give it.
void MotionHumanizeReportFields(const MotionHumanizeReport& report,
                                mcp_json::Value& result);

// The proof: every frame of `before` and `after` rendered by the installed
// renderer (MotionFrameRenderer.h) with `base`'s settings, on every core,
// and the frames whose pixels differ listed in order. False when no
// renderer is installed or a frame cannot be rendered.
bool CompareMotionRenders(const MotionFrameRequest& base,
                          const MotionComposition& before,
                          const MotionComposition& after,
                          std::vector<int64_t>& differing, MotionError& error);
