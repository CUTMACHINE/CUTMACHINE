#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Trim mode
// (FILMCRAFT FC4, ONGLETS ON8 and ON9): the Trim Monitor's two frames and
// their gestures, the trim bar, the side and the nudges, dynamic trim, play
// around, and their menu items. TrimSession.h decides; these methods relay.
// Moved out of main.mm unchanged.

@interface AppDelegate (Trim) <CMTrimBarDelegate>
- (void)layOutTrimMonitor:(TimelineRenderData&)programData;
- (BOOL)resolveTrimMonitorSlots;
- (void)syncTrimStyle;
- (const Document&)trimShownDocument;
- (void)showTrimStatus;
- (void)showTrimRefusal:(trim::TrimRefusal)refusal;
- (void)leaveTrimMode;
- (NSCursor*)edgeCursorFor:(const TimelinePointerTarget&)target;
- (void)layOutTrimBar;
- (void)refreshTrimBar;
- (void)playAroundSelection;
- (void)checkPlayAround;
- (BOOL)trimMonitorPressAt:(NSPoint)point;
- (int64_t)trimMonitorPreviewFrames:(int64_t)wanted document:(Document&)preview;
- (void)trimMonitorDragTo:(NSPoint)point;
- (void)trimMonitorRelease;
- (void)menuPlayAround:(id)sender;
- (void)menuToggleLoop:(id)sender;
- (void)trimBar:(CMTrimBarView*)bar didPress:(NSString*)action;
- (void)setTrimSide:(trim::TrimSide)side;
- (BOOL)applyTrimEdit:(const trim::TrimEdit&)edit;
- (void)trimSelectNearest;
- (void)trimCycleSide;
- (void)trimNudge:(int64_t)frames;
- (void)trimExtendToPlayhead;
- (RationalTime)trimClockNow;
- (void)toggleDynamicTrim;
- (BOOL)dynamicTrimTransport:(int)direction;
- (void)refreshDynamicTrim;
- (trim::TrimRefusal)selectPressedEdgePoint;
- (void)menuTrimTool:(id)sender;
- (void)menuTrimSelectNearest:(id)sender;
- (void)menuTrimCycleSide:(id)sender;
- (void)menuTrimNudgeBack:(id)sender;
- (void)menuTrimNudgeForward:(id)sender;
- (void)menuTrimNudgeBackMany:(id)sender;
- (void)menuTrimNudgeForwardMany:(id)sender;
- (void)menuTrimExtend:(id)sender;
- (void)menuTrimDynamic:(id)sender;
- (void)menuLeaveTrimMode:(id)sender;
- (void)trimDecoderPool;
@end
