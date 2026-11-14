#pragma once

// ON8 -- docs/roadmap/ONGLETS-2026-10.md. The trim bar, in the timeline's
// tool row between the transport letters and the timecode. It shows what
// TrimBar.h describes and hands back the action of the button pressed; it
// trims nothing. Its buttons never take the keyboard, so that the trim keys
// keep reaching the timeline after a click, and a click beside them falls
// through to the row under it.

#import <AppKit/AppKit.h>

#include "TrimBar.h"

@class CMTrimBarView;

@protocol CMTrimBarDelegate <NSObject>
- (void)trimBar:(CMTrimBarView* _Nonnull)bar
       didPress:(NSString* _Nonnull)action;
@end

@interface CMTrimBarView : NSView
@property(nonatomic, weak, nullable) id<CMTrimBarDelegate> delegate;
- (void)showModel:(const trim::TrimBarModel&)model;
// The button that sends `action`, if shown: the smoke run presses it.
- (nullable NSButton*)buttonForAction:(NSString* _Nonnull)action;
@end
