#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. What the
// Record monitor shows beside the picture (GRADE-2026-09 G6, PRD37): the
// reference still and the comparison split, the grade bypass, and the video
// scope's panel. MonitorCompare.h and VideoScopes.h decide; these methods
// relay. Moved out of main.mm unchanged.

@interface AppDelegate (MonitorCompare)
- (void)programVideoScopeChanged:(NSPopUpButton*)sender;
- (double)programScopeBottomInset;
- (NSURL*)referenceStillURL;
- (void)loadMonitorCompare;
- (void)saveMonitorCompare;
- (BOOL)referenceStillFits;
- (void)refreshCompareBadge;
- (void)toggleGradeBypass:(id)sender;
- (void)toggleReferenceSplit:(id)sender;
- (void)grabReferenceStill:(id)sender;
- (void)forgetReferenceStill:(id)sender;
- (BOOL)keepReferenceStill:(ReferenceStill)still;
@end
