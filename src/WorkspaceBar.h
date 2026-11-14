#pragma once

// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. The strip at the bottom of
// the window: the status line on the left, the pages in the middle where
// Resolve has them, and the panels a page can call on the right; at the
// right edge, apart, the Agent's window, which belongs to no page (ON3). It
// decides nothing: what a page opens is WorkspacePages.h's, applied by its
// delegate, and it only shows the state it is handed.

#import <AppKit/AppKit.h>

#include "WorkspacePages.h"

@class CMWorkspaceBarView;

@protocol CMWorkspaceBarDelegate <NSObject>
- (void)workspaceBar:(CMWorkspaceBarView* _Nonnull)bar
       didChoosePage:(ui::workspace::Page)page;
- (void)workspaceBar:(CMWorkspaceBarView* _Nonnull)bar
      didTogglePanel:(ui::workspace::Side)side;
- (void)workspaceBarDidToggleAgent:(CMWorkspaceBarView* _Nonnull)bar;
@end

@interface CMWorkspaceBarView : NSView
@property(nonatomic, weak, nullable) id<CMWorkspaceBarDelegate> delegate;
// The status line, laid out from the left edge to the pages.
@property(nonatomic, strong, nullable) NSTextField* statusLabel;
// The page chosen and the panels open on it, as the window shows them.
- (void)showPage:(ui::workspace::Page)page
          panels:(const ui::workspace::PanelSet&)panels;
// Whether the Agent's window is shown, and whether its thread has new rows
// it has not shown: a dot on the toggle while it is hidden. `key` is the one
// bound to the toggle as the menus write it ("⌥⌘A"), empty when none.
- (void)showAgentShown:(BOOL)shown
                unread:(BOOL)unread
                   key:(NSString* _Nonnull)key;
// The Agent's toggle, for the window test.
@property(nonatomic, readonly, nonnull) NSButton* agentToggle;
@end
