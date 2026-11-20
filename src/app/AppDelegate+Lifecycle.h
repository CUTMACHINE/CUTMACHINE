#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The editor's
// life: the application's and the window's delegate, the launch that builds
// the window and loads the project under its lock, the files opened from
// the Finder, full screen, the window drawn off screen, and the end, which
// writes what is held and releases the lock. Moved out of main.mm
// unchanged.

@interface AppDelegate (Lifecycle) <NSApplicationDelegate,
                                    NSWindowDelegate,
                                    TimelineMetalViewResizeTarget>
- (void)toggleFullscreen:(id)sender;
- (BOOL)loadDocumentAndSources;
- (BOOL)loadDocumentAndSourcesHoldingLock;
- (BOOL)separateEmbeddedAudioByDefault:(std::string&)message;
- (void)applicationDidFinishLaunching:(NSNotification*)notification;
- (void)application:(NSApplication*)application
          openFiles:(NSArray<NSString*>*)filenames;
- (void)windowDidMove:(NSNotification*)notification;
- (void)windowWillClose:(NSNotification*)notification;
- (void)windowDidResize:(NSNotification*)notification;
- (NSBitmapImageRep*)drawEditorWindowOffscreen:(std::string&)error;
// CH6 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel, from the menu or
// its key; drawn without the screen for capture_editor_window's "charter"
// region.
- (void)toggleCharterPanel:(id)sender;
- (NSBitmapImageRep*)drawCharterWindowOffscreen:(std::string&)error;
- (void)timelineMetalViewDidResize:(TimelineMetalView*)view;
- (void)applicationWillTerminate:(NSNotification*)notification;
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication*)sender;
@end
