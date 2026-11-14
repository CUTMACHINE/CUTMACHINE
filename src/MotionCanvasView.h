#pragma once

// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// AppKit-only. The selection of the visual editor, drawn over the Record
// monitor: the node under the mouse, the node selected and its eight
// handles, as quadrilaterals -- a node turned or in perspective is not a
// rectangle. Every position is decided by MotionCanvas.h and mapped by
// main.mm; this view only strokes what it is handed, in view points with
// rows from the top. It takes no event: the monitor under it does.
//
// Drawn by AppKit over the Metal surface, as the Record monitor's captions
// already are (CaptionOverlayLayout.h): Metal's three primitives have no
// stroke of any angle. Not yet seen on screen (VISUAL_QA_CHECKLIST.md).

#import <AppKit/AppKit.h>

#include <utility>
#include <vector>

@interface CMMotionCanvasView : NSView

- (void)setHover:(const std::vector<std::pair<double, double>>&)hover
       selection:(const std::vector<std::pair<double, double>>&)selection
         handles:(const std::vector<std::pair<double, double>>&)handles;

@end
