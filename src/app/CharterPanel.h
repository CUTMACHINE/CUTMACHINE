#pragma once

// CH6 -- docs/roadmap/CHARTE-2026-11.md. The Charte panel: a floating
// window, opened on demand (Affichage > Charte... and the shortcut
// panel.charter, DEC63), where a brand's charter (kits/<name>.json,
// brand-kit.v1) is set with its pieces in sight. The charters of the
// library on the left, with their versions; the logo, the name and a
// subtitle dressed by the charter in 9:16, 16:9 and 1:1 in the middle,
// where they are dragged; the charter's values on the right, with the file
// as the engine writes it.
//
// Every validated gesture -- a release of the mouse, a field validated, a
// version restored -- is one write of the library (EditMotionKitVersion,
// RestoreMotionKitVersion), carrying the version the panel shows, and the
// engine rewrites kits/<name>.json (DEC60, DEC61). The file is watched: a
// hand edit, the agent's write, is read again. Nothing here decides a
// value or a geometry: the panel calls CharterPreview.h, CharterCanvas.h,
// CharterPanelModel.h and CharterWindow.h, and the engine
// (MotionLibraryBase.h), as the Inspector applies operations.
//
// The library is read and written on a serial queue of the panel's own,
// never on the main thread (a write takes the base's lock, which the agent
// may hold); the previews are rendered off the main thread too, and a
// result older than the last request is dropped.
//
// Undo is the panel's own (DEC61): while the panel is the key window,
// Édition > Annuler and Rétablir restore versions of the charter shown, and
// never reach the timeline's EditLog (main.mm routes menuUndo: here). The
// montage's other commands of the menu bar are greyed meanwhile (CH6
// review): from the preview or a list, Cmd+V pasted clips into a timeline
// the panel's Cmd+Z could not take them back from.
//
// A write carries the charter and the version its gesture began on (a
// press on the preview, the first key typed in a field, a colour well
// opened), not those the panel shows when it ends; and none is sent while
// the list's choice is not yet the charter shown (CH6 review).

#import <AppKit/AppKit.h>

#include "CharterCanvas.h"
#include "MotionLibraryBase.h"

#include <string>

@interface CMCharterPanel : NSObject

// The editor window, which the panel opens over the first time.
- (instancetype)initWithEditorWindow:(NSWindow*)editor;

@property(nonatomic, readonly) NSPanel* window;
// Where the panel was left is kept in the preferences (UiPreferences.h);
// the window test turns it off.
@property(nonatomic) BOOL rememberFrame;
@property(nonatomic, readonly) BOOL shown;

- (void)toggle;
// Shown (and the library read again), the keyboard given to it.
- (void)showPanel;
- (void)hidePanel;

// Édition > Annuler / Rétablir while the panel is the key window: a field
// being typed in undoes its typing; otherwise the charter's last gesture,
// restored as a new version (CharterPanelModel.h, History).
- (BOOL)ownsUndo;
// The panel or a sheet of it (Nouvelle charte, an alert) is the key window.
// The montage's commands are then not for it (main.mm greys them).
- (BOOL)holdsKeyboard;
// The text being typed in, in the panel or its sheet: a text view that
// takes typing. The file's JSON, which only reads, is not one: Cmd+Z there
// undoes the charter's last gesture.
- (NSTextView*)typingTextView;
- (BOOL)canUndo;
- (BOOL)canRedo;
- (void)undo;
- (void)redo;

// -- For the window test (UiSmokeHarness.mm), which drives the panel as a
// hand does, through its controls and its gestures.

// Spins the run loop until no read, write or render of the panel is
// pending; NO when `timeout` passed first.
- (BOOL)waitUntilIdle:(NSTimeInterval)timeout;
// The charter of that name chosen in the list, read; NO when it is not
// listed.
- (BOOL)selectCharterNamed:(NSString*)name;
@property(nonatomic, readonly) NSString* selectedName;
// The latest version of the charter shown; 0 for none.
@property(nonatomic, readonly) int64_t shownVersion;
// What the notice line says now: a refusal, a diverged file, the last
// write.
@property(nonatomic, readonly) NSString* noticeText;
// Whether a write would be sent (a charter that reads, a file that does
// not diverge).
@property(nonatomic, readonly) BOOL writable;
// The control of the field that writes `path` (a colour well for a
// colour, a field, a pop-up...), nil when no field writes it.
- (NSControl*)controlForPath:(NSString*)path;
// The frame view of PreviewFrames()[index]; the frame shown, which the
// fields follow, as the selector chooses it (3: the three side by side).
- (NSView*)frameViewAtIndex:(NSInteger)index;
- (void)selectFrameIndex:(NSInteger)index;
// Where the anchor of `piece` (logo, nom) was drawn in that frame, in
// composition pixels; NO when it was not.
- (BOOL)anchorOfPiece:(NSString*)piece
              inFrame:(NSInteger)index
                 quad:(ui::charter::Quad&)quad;

@end
