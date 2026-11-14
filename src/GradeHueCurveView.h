#pragma once

// GR8 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. AppKit-only. The hue curves
// of the Inspector: the six curves of the clip's color.hue_curves stage, one
// at a time, over a graph whose x is the hue, the luma or the saturation.
// Every position and every gesture is decided by GradeHueCurveModel.h
// (portable, tested); this view draws what it says and turns mouse events
// into calls to it. See GradeWheelView.h for the host it reports to and for
// why AppKit draws it, and GradeCurveView.h, whose gestures these are.
//
// Under a curve along the hue the graph is painted with the hues themselves,
// by the angle the model gives them: what is under a point is the colour it
// changes. Not yet seen on screen beyond the captures of the QA mode
// (VISUAL_QA_CHECKLIST.md).

#import <AppKit/AppKit.h>

#include "GradeHueCurveModel.h"

#import "GradeWheelView.h"

#include <string>

@interface CMGradeHueCurveView : NSView

@property(nonatomic, weak) id<CMGradeEditingHost> host;

// The curve the gestures edit, one of kHueCurveNames.
@property(nonatomic, readonly) std::string curve;
- (void)selectCurve:(const std::string&)curve;

// Why the last gesture was refused, as the graph shows it until the next;
// empty when it was not.
@property(nonatomic, readonly) NSString* notice;

// Reads the host's stack again.
- (void)reload;

@end
