#pragma once

// REMOTION-PARITY-2026-09 REM5 -- docs/roadmap/REMOTION-PARITY-2026-09.md.
// AppKit-only. The Inspector of the node selected on the Record monitor:
// one row per MotionField (MotionCanvas.h) -- the value at the frame shown,
// a key toggle at that frame, the curve of a keyed property, a spring's
// settings -- grouped as the fields say. main.mm installs it in the
// Inspector slot while a node is selected, and puts the clip Inspector
// back when none is.
//
// Like CMInspectorView, it holds no edit state: every commit goes to the
// delegate as what was typed or chosen, and main.mm turns it into one
// operation (MotionCanvas.h), applies it, and reloads the rows. Not yet
// seen on screen (VISUAL_QA_CHECKLIST.md).

#import <AppKit/AppKit.h>

#include "MotionCanvas.h"
#include "MotionOutline.h"

#include <vector>

@class CMMotionInspectorView;

@protocol CMMotionInspectorViewDelegate <NSObject>

// A field committed (return, or focus leaving it): its text as typed, or
// the choice made. An empty text asks for the property's default.
- (void)motionInspector:(CMMotionInspectorView* _Nonnull)view
            commitField:(NSString* _Nonnull)property
                   text:(NSString* _Nonnull)text;

// The key toggle of a property at the frame shown.
- (void)motionInspector:(CMMotionInspectorView* _Nonnull)view
              toggleKey:(NSString* _Nonnull)property;

// The curve of a keyed property.
- (void)motionInspector:(CMMotionInspectorView* _Nonnull)view
                 easing:(NSString* _Nonnull)easing
            forProperty:(NSString* _Nonnull)property;

// A spring set (its four settings) or removed.
- (void)motionInspector:(CMMotionInspectorView* _Nonnull)view
                 spring:(NSString* _Nonnull)property
                     on:(BOOL)on
                   from:(double)from
                     to:(double)to
                  delay:(NSInteger)delay
                damping:(double)damping;

// MA6 -- a layer chosen in the composition's list: select it, as a click
// on the Record monitor does.
- (void)motionInspector:(CMMotionInspectorView* _Nonnull)view
             selectNode:(NSString* _Nonnull)uid;

// "Clip" -- back to the clip's own Inspector.
- (void)motionInspectorDidClose:(CMMotionInspectorView* _Nonnull)view;

@end

@interface CMMotionInspectorView : NSView

@property(nonatomic, weak, nullable) id<CMMotionInspectorViewDelegate> delegate;

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. The composition's layers
// first, named and opened as MotionOutlineShown says (`shown`, indices into
// `outline`), the selected one marked; then the selected node's fields.
- (void)showTitle:(NSString* _Nonnull)title
          outline:(const std::vector<MotionOutlineRow>&)outline
            shown:(const std::vector<size_t>&)shown
         selected:(const std::string&)selected
           fields:(const std::vector<MotionField>&)fields;

// The layers listed, for the smoke test.
@property(nonatomic, readonly) NSInteger layerCount;

@end
