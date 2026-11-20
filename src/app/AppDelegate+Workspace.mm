// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The window's
// pages and panels (ONGLETS ON1, ON2): the workspace bar, which page shows
// which panes, their layout and widths, and the split views' limits.
// WorkspacePages.h and PanelLayout.h decide; these methods apply. Moved out
// of main.mm unchanged.

#include "AppDelegatePrivate.h"
#include "WorkspaceActivity.h"

// The workspace is three panes -- media | editor | right dock -- so it has
// two dividers, and they cannot share one constraint. Divider 1 sits roughly
// a dock's width from the right edge, which is far past where divider 0 is
// allowed to go; answering divider 0's range for both is what collapsed the
// editor and the dock to zero width on launch, and a zero-width timeline is
// what made FitDuration throw. Every bound below is an absolute x in the
// split view, which is the coordinate space AppKit asks about.
static constexpr CGFloat kMediaPaneMinWidth = 220.0;
static constexpr CGFloat kEditorPaneMinWidth = 360.0;
static constexpr CGFloat kRightDockMinWidth = 260.0;

@implementation AppDelegate (Workspace)

// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. A page from the bar, the menu
// or its shortcut: the panels the editor left open on it.
- (void)showWorkspacePage:(ui::workspace::Page)page {
    [self applyWorkspacePage:page
                      panels:[CMUiPreferences.sharedPreferences
                                 panelsForPage:page]
                    remember:self.rememberWorkspace];
}

- (void)toggleWorkspacePanel:(ui::workspace::Side)side {
    ui::workspace::PanelSet panels = self.workspacePanels;
    if (side == ui::workspace::Side::Media)
        panels.media = !panels.media;
    else
        panels.inspector = !panels.inspector;
    [self applyWorkspacePage:self.workspacePage
                      panels:panels
                    remember:self.rememberWorkspace];
}

- (void)applyWorkspacePage:(ui::workspace::Page)page
                    panels:(const ui::workspace::PanelSet&)requestedPanels
                  remember:(BOOL)remember {
    ui::workspace::PanelSet panels = requestedPanels;
    if (page == ui::workspace::Page::Media) {
        panels.source = true;
    }
    self.workspacePage = page;
    self.workspacePanels = panels;
    if (remember) {
        [CMUiPreferences.sharedPreferences setWorkspacePage:page];
        [CMUiPreferences.sharedPreferences setPanels:panels forPage:page];
    }
    self.applyingWorkspace = YES;
    [self layOutWorkspacePanes];
    if (self.state && self.state->sourceMonitorVisible != panels.source &&
        self.sourceMonitorToggleButton)
        [self toggleSourceMonitor:self.sourceMonitorToggleButton];
    [self layOutPageContents];
    self.inspectorView.colorWorkspace = page == ui::workspace::Page::Color;
    // Couleur is the grade: its dock opens on the Inspector.
    if (panels.inspector && page == ui::workspace::Page::Color)
        [self.rightDockPanel selectSlot:ui::PanelSlot::Inspector];
    self.applyingWorkspace = NO;
    [self.workspaceBar showPage:page panels:panels];
    if (self.state) {
        [self refreshTimelineChrome];
        self.state->overlayDirty = true;
    }
}

// The panes the page shows, in their order, each removed from the split or
// put back rather than hidden -- a hidden pane leaves NSSplitView free to
// give it back its old divider (see -toggleSourceMonitor:) -- then the
// dividers where WorkspacePages.h puts them.
- (void)layOutWorkspacePanes {
    NSSplitView* split = self.workspaceSplitView;
    if (!split || !self.mediaPanel || !self.editorSplitView ||
        !self.rightDockPanel)
        return;
    const ui::workspace::PanelSet panels = self.workspacePanels;
    NSMutableArray<NSView*>* wanted = [NSMutableArray array];
    if (panels.media) [wanted addObject:self.mediaPanel];
    [wanted addObject:self.editorSplitView];
    if (panels.inspector) [wanted addObject:self.rightDockPanel];
    for (NSView* pane in [split.arrangedSubviews copy]) {
        if ([wanted containsObject:pane]) continue;
        [split removeArrangedSubview:pane];
        [pane removeFromSuperview];
    }
    for (NSUInteger index = 0; index < wanted.count; ++index) {
        if ([split.arrangedSubviews containsObject:wanted[index]]) continue;
        [split insertArrangedSubview:wanted[index] atIndex:index];
    }
    // The centre gives way when the window is resized; a side panel keeps
    // the width it was given.
    for (NSUInteger index = 0; index < split.arrangedSubviews.count; ++index)
        [split setHoldingPriority:split.arrangedSubviews[index] ==
                                          self.editorSplitView
                                      ? NSLayoutPriorityDefaultLow
                                      : NSLayoutPriorityDefaultHigh
                forSubviewAtIndex:static_cast<NSInteger>(index)];
    [split adjustSubviews];
    const CGFloat width = split.bounds.size.width;
    const CGFloat divider = split.dividerThickness;
    // The widths the hand gave this page -- not during the smoke run, whose
    // layout must not depend on the editor's preferences.
    const ui::workspace::PanelWidths chosen =
        self.workspaceWidthsFixed ? self.workspaceFixedWidths
        : self.rememberWorkspace  ? [CMUiPreferences.sharedPreferences
                                       panelWidthsForPage:self.workspacePage]
                                  : ui::workspace::PanelWidths{};
    const ui::workspace::PanelWidths widths = ui::workspace::SidePanelWidths(
        self.workspacePage, panels, width, divider, chosen);
    NSInteger dividerIndex = 0;
    if (panels.media) {
        [split setPosition:widths.media ofDividerAtIndex:dividerIndex];
        ++dividerIndex;
    }
    if (panels.inspector)
        [split setPosition:width - widths.inspector - divider
            ofDividerAtIndex:dividerIndex];
    [self layOutMediaBrowser];
}

// ON5 -- a broad media page gives the bins readable names as well as a
// grid. Keep the compact browser's frames on the other pages.
- (void)layOutMediaBrowser {
    if (self.binSidebarScroll == nil) {
        return;
    }
    const double width = self.mediaTabContentMedia.bounds.size.width;
    const double sidebar =
        ui::workspace::BinSidebarWidth(self.workspacePage, width);
    NSRect frame = self.binSidebarScroll.frame;
    frame.size.width = sidebar;
    self.binSidebarScroll.frame = frame;
    self.binOutline.outlineTableColumn.width = sidebar;
    const CGFloat x = NSMaxX(frame) + ui::theme::kSpaceS;
    for (NSView* browser in @[ self.mediaListScroll, self.mediaIconScroll ]) {
        frame = browser.frame;
        frame.origin.x = x;
        frame.size.width = std::max<CGFloat>(0, width - x - ui::theme::kSpaceS);
        browser.frame = frame;
    }
    frame = self.mediaSearchField.frame;
    frame.origin.x = x;
    frame.size.width = std::max<CGFloat>(0, width - x - 80.0);
    self.mediaSearchField.frame = frame;
    [self.mediaCollection.collectionViewLayout invalidateLayout];
}

// ON4, ON5, ON7 -- re-use the same live views; a page changes only where
// they are placed. In particular no renderer or clean feed is replaced.
- (void)layOutPageContents {
    if (self.workspaceActivityScroll == nil) {
        return;
    }
    const auto page = self.workspacePage;
    const bool reference = self.workspaceWidthsFixed;
    const bool timeline = reference || ui::workspace::ShowsTimeline(page);
    const bool record = reference || ui::workspace::ShowsRecord(page);
    const bool activity = !reference && ui::workspace::ShowsActivity(page);
    NSSplitView* editor = self.editorSplitView;
    if (!timeline && [editor.arrangedSubviews containsObject:self.metalView]) {
        self.workspaceMonitorHeight = self.monitorSplitView.frame.size.height;
        [editor removeArrangedSubview:self.metalView];
        [self.metalView removeFromSuperview];
    } else if (timeline &&
               ![editor.arrangedSubviews containsObject:self.metalView]) {
        [editor addArrangedSubview:self.metalView];
        [editor adjustSubviews];
        [editor setPosition:self.workspaceMonitorHeight ofDividerAtIndex:0];
    }
    NSMutableArray<NSView*>* wanted = [NSMutableArray array];
    if (self.state->sourceMonitorVisible) {
        [wanted addObject:self.sourceMonitorPanel];
    }
    if (record) {
        [wanted addObject:self.programMonitorPanel];
    }
    if (activity) {
        [wanted addObject:self.workspaceActivityScroll];
    }
    NSSplitView* monitors = self.monitorSplitView;
    for (NSView* view in [monitors.arrangedSubviews copy]) {
        if ([wanted containsObject:view]) {
            continue;
        }
        [monitors removeArrangedSubview:view];
        [view removeFromSuperview];
    }
    for (NSUInteger index = 0; index < wanted.count; ++index) {
        if (![monitors.arrangedSubviews containsObject:wanted[index]]) {
            [monitors insertArrangedSubview:wanted[index] atIndex:index];
        }
    }
    [editor adjustSubviews];
    [monitors adjustSubviews];
    if (activity) {
        const CGFloat edge = monitors.bounds.size.width -
                             ui::workspace::kActivityWidth -
                             monitors.dividerThickness;
        [monitors setPosition:std::max<CGFloat>(0, edge)
             ofDividerAtIndex:static_cast<NSInteger>(wanted.count) - 2];
    }
    if (record && self.state->sourceMonitorVisible) {
        const CGFloat available = monitors.bounds.size.width -
                                  (activity ? ui::workspace::kActivityWidth +
                                                  monitors.dividerThickness
                                            : 0);
        [monitors setPosition:(available - monitors.dividerThickness) * 0.5
             ofDividerAtIndex:0];
    }
    [self refreshWorkspaceActivity];
}

- (void)refreshWorkspaceActivity {
    if (self.workspaceActivityText == nil || self.state == nullptr) {
        return;
    }
    NSMutableString* text = [NSMutableString stringWithString:@"MARQUEURS\n\n"];
    for (const DocumentMarker& marker : self.state->document.sequence.markers) {
        [text appendFormat:@"%@ · %@\n",
                           TimelineTimecode(marker.time,
                                            [self playheadFrameRate]),
                           CMStringFromUtf8(marker.name)];
    }
    if (self.state->document.sequence.markers.empty()) {
        [text appendString:@"Aucun marqueur\n"];
    }
    [text appendString:@"\nMODIFICATIONS RÉCENTES\n\n"];
    const auto changes = ui::workspace::RecentChanges(self.state->editLog);
    for (const std::string& change : changes) {
        [text appendFormat:@"%@\n\n", CMStringFromUtf8(change)];
    }
    if (changes.empty()) {
        [text appendString:@"Aucune modification\n"];
    }
    // Selection and playhead changes do not replace the field, so reading
    // and copying the trace keeps the selection and scroll position.
    if (![self.workspaceActivityText.string isEqualToString:text]) {
        self.workspaceActivityText.string = text;
    }
}

- (void)workspaceBar:(CMWorkspaceBarView*)bar
       didChoosePage:(ui::workspace::Page)page {
    (void)bar;
    [self showWorkspacePage:page];
}

- (void)workspaceBar:(CMWorkspaceBarView*)bar
      didTogglePanel:(ui::workspace::Side)side {
    (void)bar;
    [self toggleWorkspacePanel:side];
}

- (void)showMediaPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Media];
}

- (void)showEditPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Edit];
}

- (void)showColorPage:(id)sender {
    (void)sender;
    [self showWorkspacePage:ui::workspace::Page::Color];
}

- (void)toggleMediaPanel:(id)sender {
    (void)sender;
    [self toggleWorkspacePanel:ui::workspace::Side::Media];
}

- (void)toggleInspectorPanel:(id)sender {
    (void)sender;
    [self toggleWorkspacePanel:ui::workspace::Side::Inspector];
}

- (void)workspaceBarDidToggleAgent:(CMWorkspaceBarView*)bar {
    (void)bar;
    [self toggleAgentWindow:nil];
}

// ON1 -- docs/roadmap/ONGLETS-2026-10.md. A page shows one, two or three of
// the workspace's panes, so a divider's bounds are read off the panes
// actually around it: the pane before it keeps its own minimum from where it
// starts, the panes after it keep theirs up to the right edge.
- (CGFloat)workspacePaneMinimum:(NSView*)pane {
    if (pane == self.mediaPanel) return kMediaPaneMinWidth;
    if (pane == self.rightDockPanel) return kRightDockMinWidth;
    return kEditorPaneMinWidth;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainSplitPosition:(CGFloat)proposedPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (splitView == self.monitorSplitView && self.state &&
        self.state->sourceMonitorVisible && dividerIndex == 0) {
        CGFloat width = splitView.bounds.size.width;
        if ([splitView.arrangedSubviews
                containsObject:self.workspaceActivityScroll]) {
            width -= self.workspaceActivityScroll.frame.size.width +
                     splitView.dividerThickness;
        }
        return (width - splitView.dividerThickness) * 0.5;
    }
    return proposedPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMinCoordinate:(CGFloat)proposedMinimumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    if (splitView == self.workspaceSplitView) {
        NSArray<NSView*>* panes = splitView.arrangedSubviews;
        if (dividerIndex < 0 ||
            dividerIndex >= static_cast<NSInteger>(panes.count))
            return proposedMinimumPosition;
        NSView* before = panes[static_cast<NSUInteger>(dividerIndex)];
        return NSMinX(before.frame) + [self workspacePaneMinimum:before];
    }
    if (splitView == self.editorSplitView) return 180.0;
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible ? 0.0 : 320.0;
    return proposedMinimumPosition;
}

- (CGFloat)splitView:(NSSplitView*)splitView
    constrainMaxCoordinate:(CGFloat)proposedMaximumPosition
               ofSubviewAt:(NSInteger)dividerIndex {
    const CGFloat divider = splitView.dividerThickness;
    if (splitView == self.workspaceSplitView) {
        NSArray<NSView*>* panes = splitView.arrangedSubviews;
        if (dividerIndex < 0 ||
            dividerIndex + 1 >= static_cast<NSInteger>(panes.count))
            return proposedMaximumPosition;
        CGFloat after = 0.0;
        for (NSUInteger index = static_cast<NSUInteger>(dividerIndex) + 1;
             index < panes.count; ++index)
            after += [self workspacePaneMinimum:panes[index]] + divider;
        NSView* before = panes[static_cast<NSUInteger>(dividerIndex)];
        return std::max<CGFloat>(
            NSMinX(before.frame) + [self workspacePaneMinimum:before],
            splitView.bounds.size.width - after);
    }
    if (splitView == self.editorSplitView)
        return std::max<CGFloat>(
            180.0, splitView.bounds.size.height - 200.0 - divider);
    if (splitView == self.monitorSplitView)
        return self.state && !self.state->sourceMonitorVisible
                   ? splitView.bounds.size.width
                   : std::max<CGFloat>(320.0,
                                       splitView.bounds.size.width - 320.0);
    return proposedMaximumPosition;
}

- (BOOL)splitView:(NSSplitView*)splitView canCollapseSubview:(NSView*)subview {
    return splitView == self.monitorSplitView &&
           subview == self.sourceMonitorPanel;
}

- (void)splitViewDidResizeSubviews:(NSNotification*)notification {
    if (notification.object == self.workspaceSplitView) {
        [self layOutMediaBrowser];
    }
    // ON2 -- a side panel dragged to a width keeps it on this page.
    if (notification.object == self.workspaceSplitView &&
        !self.applyingWorkspace && self.rememberWorkspace &&
        NSApp.currentEvent.type == NSEventTypeLeftMouseDragged) {
        NSArray<NSView*>* panes = self.workspaceSplitView.arrangedSubviews;
        ui::workspace::PanelWidths widths = [CMUiPreferences.sharedPreferences
            panelWidthsForPage:self.workspacePage];
        if ([panes containsObject:self.mediaPanel])
            widths.media = self.mediaPanel.frame.size.width;
        if ([panes containsObject:self.rightDockPanel])
            widths.inspector = self.rightDockPanel.frame.size.width;
        [CMUiPreferences.sharedPreferences setPanelWidths:widths
                                                  forPage:self.workspacePage];
    }
    if (!self.state) return;
    [self refreshTimelineChrome];
    self.state->overlayDirty = true;
}

@end
