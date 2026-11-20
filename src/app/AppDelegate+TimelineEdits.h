#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Edits of the
// timeline asked from the window: paste and delete (ripple or not), the
// range, markers and their notes, a carton's text, the Source insert,
// multicam angles, the waveform alignment of a sound, retiming, tracks
// added and removed, the context menu's actions, and the effects dropped
// on a cut. Each is an operation (Operations.h). Moved out of main.mm
// unchanged.

@interface AppDelegate (TimelineEdits) <CMEffectsPanelDelegate>
- (ui::effects::DropTarget)effectDropTargetForId:(NSString*)effectId
                                     atViewPoint:(NSPoint)point;
- (BOOL)applyEffectWithId:(NSString*)effectId
                 toTarget:(const ui::effects::DropTarget&)target;
- (void)effectsPanel:(CMEffectsPanelView*)panel
    applyEffectWithId:(NSString*)effectId;
- (void)addTrack:(BOOL)audio;
- (BOOL)applyTimelinePaste:(PasteClipsOperation)operation
                     label:(NSString*)label;
- (BOOL)hasValidTimelineRange;
- (BOOL)deleteTimelineRangeRipple:(BOOL)ripple;
- (BOOL)deleteCurrentTimelineSelectionRipple:(BOOL)ripple;
- (BOOL)deleteCurrentTimelineSelection;
- (BOOL)applyMarkerOperation:(Operation)operation;
- (BOOL)markersBelongToTimeline;
- (void)showMarkerInfo:(const DocumentMarker&)marker;
- (void)askMarkerNote:(NSString*)note
                 time:(RationalTime)time
             duration:(RationalTime)duration
            removable:(BOOL)removable
               answer:
                   (void (^)(NSModalResponse response, NSString* typed))answer;
- (void)editMarkerNote:(const Ulid&)markerId;
- (void)goToAdjacentMarkerForward:(BOOL)forward;
- (void)showCartonInfo:(const Ulid&)clipId;
- (BOOL)setCartonText:(const std::string&)typed forClip:(const Ulid&)clipId;
- (void)editCartonText:(const Ulid&)clipId;
- (BOOL)performSourceEditInsert:(BOOL)insert;
- (BOOL)multicamSwitchForSelection:(BOOL)clear
                         operation:(SetMulticamActiveAngleOperation&)operation;
- (void)applyMulticamSwitch:(BOOL)clear;
- (NSMenuItem*)autoAlignMenuItem;
- (BOOL)alignSelectedSoundByWaveform;
- (void)finishSoundAlignment:(ui::separate_sound::SoundPair)pair
                    measured:(const std::string&)output;
- (void)toggleRetimeControls;
- (void)removeContextTrackPressed:(id)sender;
- (void)removeEmptyTracksPressed:(id)sender;
- (void)contextOpenClipSource:(id)sender;
- (void)contextCutClip:(id)sender;
- (void)contextDeleteSelection:(id)sender;
- (void)contextCloseGap:(id)sender;
@end
