#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The timeline
// under the hand (TimelineEventTarget): the mouse, the scroll and the
// pinch, the keys, the drops of media and effects, the context menu, and
// the tool and its cursor. TimelineView.h says what a point hits; these
// methods turn each gesture into an edit or a move of the playhead. Moved
// out of main.mm unchanged.

@interface AppDelegate (TimelineInput) <TimelineEventTarget>
- (NSPoint)timelinePointForEvent:(NSEvent*)event;
- (TimelineTool)effectiveTool;
- (TimelinePointerTarget)timelineTargetAt:(NSPoint)point event:(NSEvent*)event;
- (void)applyToolCursor;
- (void)setTimelineTool:(TimelineTool)tool;
- (void)zoomTimelineAtPlayheadOut:(BOOL)zoomOut;
- (void)stepPlayheadSeconds:(int64_t)direction;
- (void)stepPlayheadFrames:(int64_t)amount;
- (BOOL)timelineDropMedia:(NSString*)mediaId atViewPoint:(NSPoint)point;
- (NSDragOperation)timelineDragEffectId:(NSString*)effectId
                            atViewPoint:(NSPoint)point;
- (void)timelineEffectDragEnded;
- (BOOL)timelineDropEffectId:(NSString*)effectId atViewPoint:(NSPoint)point;
- (BOOL)timelineDropMediaIds:(NSArray<NSString*>*)mediaIds
                 atViewPoint:(NSPoint)point;
- (void)timelineMouseDown:(NSEvent*)event;
- (void)timelineMouseDragged:(NSEvent*)event;
- (void)timelineMouseMoved:(NSEvent*)event;
- (void)timelineMouseUp:(NSEvent*)event;
- (void)timelineScroll:(NSEvent*)event;
- (void)timelineMagnify:(NSEvent*)event;
- (BOOL)timelineKeyDown:(NSEvent*)event;
- (void)timelineKeyUp:(NSEvent*)event;
- (BOOL)textEditKeyDown:(NSEvent*)event inTextView:(NSTextView*)text;
- (NSMenu*)timelineMenuForEvent:(NSEvent*)event;
@end
