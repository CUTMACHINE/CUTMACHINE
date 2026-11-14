#pragma once

// GR4 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. AppKit-only. The colour
// wheels of the Inspector: lift, gamma, gain and offset, each a ring with
// its puck, four fields and a master bar. Every position and every gesture
// is decided by GradeWheelModel.h (portable, tested); these views draw what
// it says, with Core Graphics, and turn mouse events into calls to it.
//
// Drawn by AppKit, like the selection of the visual editor of motion
// (MotionCanvasView.h): Metal's three primitives have no circle. Laid out in
// frames, no Auto Layout. Not yet seen on screen beyond the captures of the
// QA mode (VISUAL_QA_CHECKLIST.md).
//
// A view holds no stack of its own. It reads the clip's from its host at
// every reload, shows the stack of a gesture while it lasts, and hands the
// host one stack when the gesture ends: one gesture, one edit, one undo.

#import <AppKit/AppKit.h>

#include "Document.h"
#include "GradeWheelModel.h"

#include <vector>

// What the grading controls need of the panel they sit in. CMInspectorView
// is the one host: it owns the clip's stack and submits the operation.
@protocol CMGradeEditingHost <NSObject>

// The stack of the clip shown, as of its last reload or commit.
- (const std::vector<ClipEffect>&)gradeEffects;

// Whether the clip shown takes a grade: a picture, on a video track.
- (BOOL)gradeEditable;

// While a gesture lasts: the stack it would commit, for the monitor to
// show. Nothing is applied.
- (void)gradePreviewEffects:(const std::vector<ClipEffect>&)effects;

// When it ends: the stack to commit, in one edit.
- (void)gradeCommitEffects:(const std::vector<ClipEffect>&)effects;

@end

@interface CMGradeWheelView : NSView

- (instancetype)initWithFrame:(NSRect)frame
                        wheel:(ui::inspector::GradeWheel)wheel;

@property(nonatomic, weak) id<CMGradeEditingHost> host;
@property(nonatomic, readonly) ui::inspector::GradeWheel wheel;

// The four fields under the ring: r, g, b, master.
@property(nonatomic, readonly) NSArray<NSTextField*>* fields;
@property(nonatomic, readonly) NSButton* resetButton;

// Reads the host's stack again: the puck, the fields, the master bar.
- (void)reload;

@end

// The four wheels, two rows of two (LayoutGradeWheelGrid).
@interface CMGradeWheelGridView : NSView

@property(nonatomic, weak) id<CMGradeEditingHost> host;
@property(nonatomic, readonly) NSArray<CMGradeWheelView*>* wheelViews;

- (void)reload;

@end
