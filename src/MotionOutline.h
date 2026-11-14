#pragma once

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. A composition as After shows
// it once exported: numbered, named scenes and named layers. The names are
// the export's own (MotionAfterExport.cc calls these functions), so the
// panel of CUTMACHINE and the timeline of After say the same thing -- the
// author's id before '#', what a text shows, the biggest text of a group.
// Portable: the editor draws the rows it is given and decides nothing.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// The id a person reads: what comes before '#' (ids are unique in a
// composition, so an author writes "titre#2" for a second "titre").
std::string MotionDisplayId(const std::string& id);

// One line of at most `most` bytes, cut on a character, with an ellipsis.
std::string MotionShortName(std::string text, size_t most = 32);

// A group named as After names its precomposition: its id, else the
// biggest text it shows in `evaluated` (null: none known), else "groupe".
std::string MotionGroupName(const std::string& id,
                            const MotionEvaluatedNode* evaluated);

// The number of a scene of the film, as its precomposition is named:
// "03 " for the third.
std::string MotionSceneNumber(int64_t index);

struct MotionOutlineRow {
    // The node's identity (empty in a composition read from a file that
    // names none) and its structural path.
    std::string uid;
    std::string path;
    int depth = 0;
    // What the row reads, and what the node is, in French.
    std::string name;
    std::string kind;
    // A scene of the film: numbered, with the frames it covers in the
    // composition.
    bool scene = false;
    int64_t start = 0;
    int64_t duration = 0;
};

// The rows the panel shows while `selected` is: every scene or top-level
// layer, and the children of each ancestor of the selected node and of the
// node itself -- the tree opened along the selection, as After's timeline
// shows the precomposition one opens. Indices into `rows`, in order.
std::vector<size_t> MotionOutlineShown(
    const std::vector<MotionOutlineRow>& rows, const std::string& selected);

// Every node under the root, depth first, in paint order. Texts and groups
// are named from the frame each is first shown at: the text a binding
// gives, the biggest text a group holds.
bool MotionOutlineRows(const MotionComposition& composition,
                       const mcp_json::Value& props, MotionMeasurer& measurer,
                       const MotionEvaluationContext& context,
                       std::vector<MotionOutlineRow>& rows, MotionError& error);
