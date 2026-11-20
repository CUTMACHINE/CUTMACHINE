#pragma once

// MOTION-2026-09 M3 -- ROADMAP.md. The motion tools of the MCP catalog: list
// and describe the project's compositions, add, remove and edit one through
// the motion operations, and render one frame of it as a picture with the
// boxes the engine computed. Kept apart from McpTools.cc, which registers
// them, so that the vocabulary of compositions reads in one place.

#include "McpTools.h"

#include <functional>
#include <string>

using McpToolAdder =
    std::function<void(std::string name, std::string description,
                       std::string schema, McpDispatchFn fn)>;

void RegisterMotionTools(const McpToolAdder& add);

// The tools that change a composition. Their edits join the history of the
// timeline they are made from, as bin edits do: a composition belongs to the
// project, the undo step to the timeline being worked on.
bool IsMotionEditingTool(const std::string& name);
