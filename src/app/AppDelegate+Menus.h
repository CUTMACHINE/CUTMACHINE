#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The menu bar:
// its construction, the validation of each item, what each item does, and
// the keyboard shortcuts a person can rebind (KeyboardShortcuts.h decides
// the bindings). Moved out of main.mm unchanged.

@interface AppDelegate (Menus) <NSMenuItemValidation>
- (NSArray<NSDictionary<NSString*, NSString*>*>*)shortcutDefinitions;
- (void)loadShortcutBindings;
- (BOOL)event:(NSEvent*)event matchesShortcut:(NSString*)identifier;
- (void)applyShortcutBindingsToMenus;
- (NSMenuItem*)shortcutMenuItem:(NSString*)title
                         action:(SEL)action
                        command:(NSString*)identifier;
- (void)editKeyboardShortcuts:(id)sender;
- (void)resetKeyboardShortcutFields:(id)sender;
- (NSMenuItem*)menuItem:(NSString*)title action:(SEL)action key:(NSString*)key;
- (void)installApplicationMenus;
- (BOOL)validateMenuItem:(NSMenuItem*)item;
- (void)menuToggleProxies:(id)sender;
- (void)menuToggleAutomaticProxies:(id)sender;
- (void)menuCopyTimelineClips:(id)sender;
- (void)menuPasteTimelineClips:(id)sender;
- (void)menuUndo:(id)sender;
- (void)menuRedo:(id)sender;
- (void)menuDeleteSelection:(id)sender;
- (void)menuRippleDeleteSelection:(id)sender;
- (void)menuMarkTimelineIn:(id)sender;
- (void)menuMarkTimelineOut:(id)sender;
- (void)menuClearTimelineRange:(id)sender;
- (void)menuAddMarker:(id)sender;
- (void)menuAnnotateMarker:(id)sender;
- (void)menuRemoveMarker:(id)sender;
- (void)menuNextMarker:(id)sender;
- (void)menuPreviousMarker:(id)sender;
- (void)menuEditCartonText:(id)sender;
- (void)menuInsertSource:(id)sender;
- (void)menuOverwriteSource:(id)sender;
- (void)menuCutSelectedAtPlayhead:(id)sender;
- (void)menuAddCrossDissolve:(id)sender;
- (void)menuRemoveCrossDissolve:(id)sender;
- (void)menuActivateMulticamAngle:(id)sender;
- (void)menuClearMulticamAngle:(id)sender;
- (void)menuAutoAlignByWaveform:(id)sender;
- (void)menuSelectTool:(id)sender;
- (void)menuHandTool:(id)sender;
- (void)menuZoomTool:(id)sender;
- (void)menuCutTool:(id)sender;
- (void)menuRetimeControls:(id)sender;
- (void)menuToggleSnapping:(id)sender;
- (void)menuToggleLinkedSelection:(id)sender;
- (void)menuSelectClipsAfter:(id)sender;
- (void)menuToggleClipsEnabled:(id)sender;
- (void)menuFitTimeline:(id)sender;
- (void)menuAddVideoTrack:(id)sender;
- (void)menuAddAudioTrack:(id)sender;
- (void)menuPlayPause:(id)sender;
- (void)menuPlayReverse:(id)sender;
- (void)menuStop:(id)sender;
- (void)menuPlayForward:(id)sender;
- (void)keyboardKeyPressed:(NSButton*)sender;
- (NSButton*)keyboardModifierButton:(NSString*)title frame:(NSRect)frame;
- (NSView*)keyboardViewForDefinitions:(NSArray*)definitions
                               fields:(NSMutableDictionary*)fields;
@end
