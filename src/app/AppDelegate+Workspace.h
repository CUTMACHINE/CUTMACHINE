#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The window's
// pages and panels (ONGLETS ON1, ON2): the workspace bar, which page shows
// which panes, their layout and widths, and the split views' limits.
// WorkspacePages.h and PanelLayout.h decide; these methods apply. Moved out
// of main.mm unchanged.

@interface AppDelegate (Workspace) <NSSplitViewDelegate, CMWorkspaceBarDelegate>
- (void)showWorkspacePage:(ui::workspace::Page)page;
- (void)toggleWorkspacePanel:(ui::workspace::Side)side;
- (void)applyWorkspacePage:(ui::workspace::Page)page
                    panels:(const ui::workspace::PanelSet&)requestedPanels
                  remember:(BOOL)remember;
- (void)layOutWorkspacePanes;
- (void)layOutMediaBrowser;
- (void)layOutPageContents;
- (void)refreshWorkspaceActivity;
- (void)workspaceBar:(CMWorkspaceBarView*)bar
       didChoosePage:(ui::workspace::Page)page;
- (void)workspaceBar:(CMWorkspaceBarView*)bar
      didTogglePanel:(ui::workspace::Side)side;
- (void)showMediaPage:(id)sender;
- (void)showEditPage:(id)sender;
- (void)showColorPage:(id)sender;
- (void)toggleMediaPanel:(id)sender;
- (void)toggleInspectorPanel:(id)sender;
- (void)workspaceBarDidToggleAgent:(CMWorkspaceBarView*)bar;
- (CGFloat)workspacePaneMinimum:(NSView*)pane;
- (CGFloat)splitView:(NSSplitView*)splitView
    constrainSplitPosition:(CGFloat)proposedPosition
               ofSubviewAt:(NSInteger)dividerIndex;
- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex;
- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex;
- (BOOL)splitView:(NSSplitView*)splitView canCollapseSubview:(NSView*)subview;
- (void)splitViewDidResizeSubviews:(NSNotification*)notification;
@end
