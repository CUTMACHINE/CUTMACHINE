#pragma once

// EF1 -- docs/roadmap/EFFETS-2026-10.md. The « Effets » tab of the media
// panel: the effects library (EffectsLibrary.h), grouped by category, each
// row dragged onto a cut of the timeline. It draws the list and starts the
// drag; where an effect lands and what it does are EffectsLibrary.h's, and
// the timeline applies it. A double click applies it to the cut nearest the
// playhead, so the library is not reachable by the mouse alone.

#import <Cocoa/Cocoa.h>

// What a dragged effect carries: its stable id (ui::effects::Effect::id).
extern NSPasteboardType const kCutmachineEffectPasteboardType;

@class CMEffectsPanelView;

@protocol CMEffectsPanelDelegate <NSObject>
// The effect double-clicked in the list, for the cut nearest the playhead.
- (void)effectsPanel:(CMEffectsPanelView*)panel
    applyEffectWithId:(NSString*)effectId;
@end

@interface CMEffectsPanelView
    : NSView <NSTableViewDataSource, NSTableViewDelegate>
@property(nonatomic, weak) id<CMEffectsPanelDelegate> delegate;
@end
