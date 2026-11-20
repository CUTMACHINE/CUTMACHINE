#pragma once

// LG1 -- docs/roadmap/LEGENDES-2026-10.md. The Core Text font a motion text
// style resolves to, for the caption renderer (CaptionRender.mm): a caption
// family has to be found exactly as a composition finds it -- the same exact
// family match, the same regular face, a registered font directory winning
// over an installed copy -- or a carton and a title in one font would not
// be drawn in the same file. It exposes a Core Text type, so only the .mm
// files of cutmachine_motion_render include it; MotionRender.h stays plain
// C++.

#include "MotionRender.h"

#include <CoreText/CoreText.h>

// Owned by `measurer`, valid while it lives. Null with FontMissing when the
// family has no such face.
CTFontRef MotionCoreTextFont(MotionCoreTextMeasurer& measurer,
                             const MotionTextStyle& style, MotionError& error);
