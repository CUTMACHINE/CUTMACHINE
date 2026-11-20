#pragma once

// CH6 -- docs/roadmap/CHARTE-2026-11.md. One frame of the Charte panel's
// preview (9:16, 16:9 or 1:1): the background chosen, the pieces and the
// subtitle as the renderers drew them, and the outlines of what can be
// grabbed. AppKit only, and no policy: what a click grabs, the outline of a
// drag and the edits of a release are CharterCanvas.h's; this view draws
// what it is handed and turns the mouse into composition pixels (origin at
// the top left, as the motion frame has it) for its delegate.
//
// The pictures are CGImages the size of the frame at the scale they were
// rendered at, laid over the whole view: the view keeps the frame's aspect,
// so a composition pixel is bounds.width / frame.width points wide.

#import <AppKit/AppKit.h>

#include "CharterCanvas.h"
#include "CharterPreview.h"

#include <vector>

@class CMCharterFrameView;

@protocol CMCharterFrameViewDelegate <NSObject>
- (void)charterFrame:(CMCharterFrameView*)view
         mouseDownAt:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers;
- (void)charterFrame:(CMCharterFrameView*)view
      mouseDraggedTo:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers;
- (void)charterFrame:(CMCharterFrameView*)view
           mouseUpAt:(ui::charter::Point)point
           modifiers:(NSEventModifierFlags)modifiers;
- (void)charterFrame:(CMCharterFrameView*)view
        mouseMovedTo:(ui::charter::Point)point;
- (void)charterFrameMouseExited:(CMCharterFrameView*)view;
@end

// What the frame is shown over.
typedef NS_ENUM(NSInteger, CMCharterBackground) {
    // A neutral plan, from the theme's greys: the film the pieces dress.
    CMCharterBackgroundPlan = 0,
    // The charter's `fond` colour.
    CMCharterBackgroundFond,
    // The charter's `accent` colour.
    CMCharterBackgroundAccent,
    // A checker of the theme's surfaces: what the pieces leave transparent.
    CMCharterBackgroundChecker,
};

@interface CMCharterFrameView : NSView

@property(nonatomic) NSInteger frameIndex;
@property(nonatomic) ui::charter::PreviewFrame previewFrame;
@property(nonatomic, weak) id<CMCharterFrameViewDelegate> delegate;
@property(nonatomic) CMCharterBackground background;
// The charter's colours for those backgrounds; nil draws the plan.
@property(nonatomic, strong) NSColor* fondColor;
@property(nonatomic, strong) NSColor* accentColor;
// Said in place of the pictures (a refusal, nothing to show).
@property(nonatomic, copy) NSString* message;
// The frame the fields follow: its edge in the accent colour.
@property(nonatomic) BOOL selected;

// The pictures, bottom first, each a CGImageRef (bridged), laid over the
// whole frame.
- (void)setPictures:(NSArray*)pictures;
// The outlines of what the pointer is over (composition pixels): its quad
// and the handles of the logo, when it is the logo.
- (void)setHover:(const std::vector<ui::charter::Point>&)quad
         handles:(const std::vector<ui::charter::Point>&)handles;
// The box the dragged thing would land in now (DragOutline), and the safe
// zone it snaps to; empty quad: no drag.
- (void)setDragOutline:(const std::vector<ui::charter::Point>&)quad
              safeZone:(const std::vector<ui::charter::Point>&)zone;

// View points per composition pixel.
- (double)pointsPerPixel;
// A composition pixel's point in this view, and back.
- (NSPoint)viewPointFor:(ui::charter::Point)point;
- (ui::charter::Point)compositionPointFor:(NSPoint)point;

@end
