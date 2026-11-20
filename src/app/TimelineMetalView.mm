// DC3 -- docs/roadmap/DECOUPAGE-2026-10.md. The Metal view of the timeline
// and of both monitors: it relays mouse, keys and drops to its event target
// (the editor, AppDelegate) and draws nothing itself. Moved out of main.mm
// unchanged.

#include "AppDelegatePrivate.h"

@implementation TimelineMetalView {
    NSTrackingArea* _trackingArea;
}
- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame]))
        [self registerForDraggedTypes:@[
            kCutmachineMediaPasteboardType,
            kCutmachineMediaSelectionPasteboardType,
            kCutmachineEffectPasteboardType
        ]];
    return self;
}
- (id)accessibilityValue {
    return self.accessibilityValueProvider ? self.accessibilityValueProvider()
                                           : [super accessibilityValue];
}
- (BOOL)acceptsFirstResponder {
    return YES;
}
- (void)setFrameSize:(NSSize)newSize {
    const BOOL changed = !NSEqualSizes(self.frame.size, newSize);
    [super setFrameSize:newSize];
    if (changed) [self.resizeTarget timelineMetalViewDidResize:self];
}
- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingMouseMoved | NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}
- (NSView*)hitTest:(NSPoint)point {
    // Monitor controls remain interactive while passive labels stay visual
    // overlays and editing gestures remain owned by the Metal surface.
    NSView* child = [super hitTest:point];
    // Modern AppKit controls can return a private content subview from
    // -hitTest:. Walk back to the public control instead of handing the click
    // to the Metal view, which otherwise swallows Insert/Overwrite actions.
    for (NSView* candidate = child; candidate && candidate != self;
         candidate = candidate.superview) {
        if ([candidate isKindOfClass:NSPopUpButton.class] ||
            [candidate isKindOfClass:NSButton.class])
            return candidate;
    }
    // `point` is expressed in the superview's coordinates. Comparing it to
    // `bounds` only happened to work for monitor views placed at (0, 0); the
    // timeline has a non-zero frame origin, so every click and drag was
    // rejected. Let NSView's hit test decide whether the point is inside and
    // redirect passive overlay labels back to the Metal interaction surface.
    return child ? self : nil;
}
- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.eventTarget timelineMouseDown:event];
}
- (void)mouseDragged:(NSEvent*)event {
    [self.eventTarget timelineMouseDragged:event];
}
- (void)mouseUp:(NSEvent*)event {
    [self.eventTarget timelineMouseUp:event];
}
- (void)mouseMoved:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)mouseExited:(NSEvent*)event {
    [self.eventTarget timelineMouseMoved:event];
}
- (void)scrollWheel:(NSEvent*)event {
    [self.eventTarget timelineScroll:event];
}
- (void)magnifyWithEvent:(NSEvent*)event {
    [self.eventTarget timelineMagnify:event];
}
- (void)keyDown:(NSEvent*)event {
    if (![self.eventTarget timelineKeyDown:event]) [super keyDown:event];
}
- (void)keyUp:(NSEvent*)event {
    [self.eventTarget timelineKeyUp:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    return [self.eventTarget timelineMenuForEvent:event];
}
// EF1 -- the effect a drag carries, on the timeline only.
- (NSString*)cmDraggedEffectId:(id<NSDraggingInfo>)sender {
    if (!self.acceptsEffects) return nil;
    return [sender.draggingPasteboard
        stringForType:kCutmachineEffectPasteboardType];
}
- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDragEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    return [[sender draggingPasteboard] availableTypeFromArray:@[
        kCutmachineMediaSelectionPasteboardType, kCutmachineMediaPasteboardType
    ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDragEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    return [[sender draggingPasteboard] availableTypeFromArray:@[
        kCutmachineMediaSelectionPasteboardType, kCutmachineMediaPasteboardType
    ]]
               ? NSDragOperationCopy
               : NSDragOperationNone;
}
- (void)draggingExited:(id<NSDraggingInfo>)sender {
    if ([self cmDraggedEffectId:sender])
        [self.eventTarget timelineEffectDragEnded];
}
- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender {
    if (NSString* effectId = [self cmDraggedEffectId:sender])
        return [self.eventTarget
            timelineDropEffectId:effectId
                     atViewPoint:[self convertPoint:sender.draggingLocation
                                           fromView:nil]];
    NSPasteboard* pasteboard = sender.draggingPasteboard;
    NSArray<NSString*>* mediaIds = [pasteboard
        propertyListForType:kCutmachineMediaSelectionPasteboardType];
    if (![mediaIds isKindOfClass:NSArray.class] || mediaIds.count == 0) {
        NSString* mediaId =
            [pasteboard stringForType:kCutmachineMediaPasteboardType];
        mediaIds = mediaId ? @[ mediaId ] : @[];
    }
    if (mediaIds.count == 0) return NO;
    const NSPoint point = [self convertPoint:sender.draggingLocation
                                    fromView:nil];
    return [self.eventTarget timelineDropMediaIds:mediaIds atViewPoint:point];
}
@end
