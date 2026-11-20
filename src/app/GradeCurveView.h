#pragma once

// GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. AppKit-only. The curve
// editor of the Inspector: the four channels of the clip's color.curves
// stage over one graph, the points of the channel being edited, and a bar to
// choose it. Every position and every gesture is decided by
// GradeCurveModel.h (portable, tested); this view draws what it says, with
// Core Graphics, and turns mouse events into calls to it. See
// GradeWheelView.h, its sibling, for the host it reports to and for why
// AppKit draws it.
//
// The curve stroked is the curve rendered: the model samples it through the
// renderer's own evaluation. Not yet seen on screen beyond the captures of
// the QA mode (VISUAL_QA_CHECKLIST.md).

#import <AppKit/AppKit.h>

#include "GradeCurveModel.h"
#include "VideoScopes.h"

#import "GradeWheelView.h"

#include <string>

@interface CMGradeCurveView : NSView

@property(nonatomic, weak) id<CMGradeEditingHost> host;

// The channel the gestures edit: "master", "r", "g" or "b"
// (kColorCurveChannels). The others are drawn behind it.
@property(nonatomic, readonly) std::string channel;
- (void)selectChannel:(const std::string&)channel;

// Why the last gesture was refused, as the graph shows it for a moment;
// empty when it was not.
@property(nonatomic, readonly) NSString* notice;

// Reads the host's stack again.
- (void)reload;

// DEC42 -- the successful Record present's display signal, already binned
// by its renderer. The selected channel chooses which distribution shows.
- (void)showHistogram:(const VideoHistogram&)histogram;

@end
