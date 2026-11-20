#pragma once

// MOTION-2026-09 M4 -- ROADMAP.md. The MCP tools of motion clips: pose a
// composition on the timeline as a clip, set the props of that instance,
// and look at what a motion clip shows at an instant of the timeline. Kept
// apart from McpMotionTools.cc, whose tools edit the compositions
// themselves, so that each file reads as one vocabulary.

#include "McpMotionTools.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <string>
#include <vector>

void RegisterMotionClipTools(const McpToolAdder& add);

// The tools that change a motion clip; they join the timeline's history
// like every other timeline edit.
bool IsMotionClipEditingTool(const std::string& name);

// What motion clip `clipId` of the active timeline shows at `timelineTime`:
// the PNG and, as text, the geometry of the same evaluation, as
// render_motion_frame returns a composition's frame. `fontDirectories` are
// registered for the process first.
bool RenderMotionClipFrame(McpBackend& backend, const Ulid& clipId,
                           const RationalTime& timelineTime,
                           const std::vector<std::string>& fontDirectories,
                           std::string& resultJson, std::string& errorName,
                           std::string& message);
