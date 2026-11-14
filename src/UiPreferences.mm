#import "UiPreferences.h"

#import <dispatch/dispatch.h>

@interface CMUiPreferences ()
- (NSString*)defaultsKeyForDock:(ui::PanelDock)dock;
@end

@implementation CMUiPreferences

+ (instancetype)sharedPreferences {
    static CMUiPreferences* instance = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
      instance = [[CMUiPreferences alloc] init];
    });
    return instance;
}

- (NSString*)defaultsKeyForDock:(ui::PanelDock)dock {
    const std::string key = ui::LastActiveTabPreferenceKey(dock);
    return [NSString stringWithUTF8String:key.c_str()];
}

- (void)setLastActiveIdentifier:(NSString*)identifier
                        forDock:(ui::PanelDock)dock {
    [NSUserDefaults.standardUserDefaults
        setObject:identifier
           forKey:[self defaultsKeyForDock:dock]];
}

- (nullable NSString*)lastActiveIdentifierForDock:(ui::PanelDock)dock {
    return [NSUserDefaults.standardUserDefaults
        stringForKey:[self defaultsKeyForDock:dock]];
}

- (void)setLastMediaPanelTab:(NSString* _Nonnull)identifier {
    [NSUserDefaults.standardUserDefaults setObject:identifier
                                            forKey:@"ui.mediaPanel.lastTab"];
}

- (nullable NSString*)lastMediaPanelTab {
    return [NSUserDefaults.standardUserDefaults
        stringForKey:@"ui.mediaPanel.lastTab"];
}

- (BOOL)requiresExplicitTimelineForAgentEdits {
    return [NSUserDefaults.standardUserDefaults
        boolForKey:@"CMRequireExplicitTimeline"];
}

- (MonitorCompare)monitorCompare {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    MonitorCompare compare;
    compare.bypass_grade = [defaults boolForKey:@"ui.monitor.bypassGrade"];
    compare.split = [defaults boolForKey:@"ui.monitor.referenceSplit"];
    compare.split_position =
        [defaults objectForKey:@"ui.monitor.referenceSplitPosition"] != nil
            ? ClampSplitPosition(
                  [defaults doubleForKey:@"ui.monitor.referenceSplitPosition"])
            : 0.5;
    return compare;
}

- (void)setMonitorCompare:(const MonitorCompare&)compare {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    [defaults setBool:compare.bypass_grade forKey:@"ui.monitor.bypassGrade"];
    [defaults setBool:compare.split forKey:@"ui.monitor.referenceSplit"];
    [defaults setDouble:ClampSplitPosition(compare.split_position)
                 forKey:@"ui.monitor.referenceSplitPosition"];
}

- (nullable NSString*)referenceStillPath {
    return [NSUserDefaults.standardUserDefaults
        stringForKey:@"ui.monitor.referenceStill"];
}

- (void)setReferenceStillPath:(nullable NSString*)path {
    if (path == nil) {
        [NSUserDefaults.standardUserDefaults
            removeObjectForKey:@"ui.monitor.referenceStill"];
        return;
    }
    [NSUserDefaults.standardUserDefaults
        setObject:path
           forKey:@"ui.monitor.referenceStill"];
}

// DEC42 -- one key per fold, local to this machine like every key here.
static NSString* CMInspectorSlidersKey(NSString* group) {
    return
        [NSString stringWithFormat:@"ui.inspector.sliders.%@.unfolded", group];
}

- (BOOL)inspectorSlidersUnfolded:(NSString*)group {
    return [NSUserDefaults.standardUserDefaults
        boolForKey:CMInspectorSlidersKey(group)];
}

- (void)setInspectorSliders:(NSString*)group unfolded:(BOOL)unfolded {
    [NSUserDefaults.standardUserDefaults setBool:unfolded
                                          forKey:CMInspectorSlidersKey(group)];
}

static NSString* CMWorkspaceKey(ui::workspace::Page page, NSString* what) {
    return [NSString
        stringWithFormat:@"ui.workspace.%@.%@",
                         @(ui::workspace::Describe(page).identifier), what];
}

- (ui::workspace::Page)workspacePage {
    NSString* stored =
        [NSUserDefaults.standardUserDefaults stringForKey:@"ui.workspace.page"];
    return ui::workspace::PageNamed(stored.UTF8String ?: "")
        .value_or(ui::workspace::kDefaultPage);
}

- (void)setWorkspacePage:(ui::workspace::Page)page {
    [NSUserDefaults.standardUserDefaults
        setObject:@(ui::workspace::Describe(page).identifier)
           forKey:@"ui.workspace.page"];
}

- (ui::workspace::PanelSet)panelsForPage:(ui::workspace::Page)page {
    ui::workspace::PanelSet panels = ui::workspace::DefaultPanels(page);
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:CMWorkspaceKey(page, @"panels")];
    if ([stored[@"media"] isKindOfClass:NSNumber.class])
        panels.media = [stored[@"media"] boolValue];
    if ([stored[@"inspector"] isKindOfClass:NSNumber.class])
        panels.inspector = [stored[@"inspector"] boolValue];
    if ([stored[@"source"] isKindOfClass:NSNumber.class])
        panels.source = [stored[@"source"] boolValue];
    return panels;
}

- (void)setPanels:(const ui::workspace::PanelSet&)panels
          forPage:(ui::workspace::Page)page {
    [NSUserDefaults.standardUserDefaults
        setObject:@{
            @"media" : @(panels.media),
            @"inspector" : @(panels.inspector),
            @"source" : @(panels.source)
        }
           forKey:CMWorkspaceKey(page, @"panels")];
}

- (ui::workspace::PanelWidths)panelWidthsForPage:(ui::workspace::Page)page {
    ui::workspace::PanelWidths widths;
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:CMWorkspaceKey(page, @"widths")];
    if ([stored[@"media"] isKindOfClass:NSNumber.class])
        widths.media = [stored[@"media"] doubleValue];
    if ([stored[@"inspector"] isKindOfClass:NSNumber.class])
        widths.inspector = [stored[@"inspector"] doubleValue];
    return widths;
}

- (void)setPanelWidths:(const ui::workspace::PanelWidths&)widths
               forPage:(ui::workspace::Page)page {
    [NSUserDefaults.standardUserDefaults
        setObject:@{
            @"media" : @(widths.media),
            @"inspector" : @(widths.inspector)
        }
           forKey:CMWorkspaceKey(page, @"widths")];
}

- (std::optional<ui::agent_window::Rect>)agentWindowFrame {
    NSDictionary* stored = [NSUserDefaults.standardUserDefaults
        dictionaryForKey:@"ui.agentWindow.frame"];
    ui::agent_window::Rect frame;
    double* fields[] = {&frame.x, &frame.y, &frame.width, &frame.height};
    NSString* names[] = {@"x", @"y", @"width", @"height"};
    for (size_t index = 0; index < 4; ++index) {
        if (![stored[names[index]] isKindOfClass:NSNumber.class])
            return std::nullopt;
        *fields[index] = [stored[names[index]] doubleValue];
    }
    return frame;
}

- (void)setAgentWindowFrame:(const ui::agent_window::Rect&)frame {
    [NSUserDefaults.standardUserDefaults setObject:@{
        @"x" : @(frame.x),
        @"y" : @(frame.y),
        @"width" : @(frame.width),
        @"height" : @(frame.height)
    }
                                            forKey:@"ui.agentWindow.frame"];
}

- (BOOL)agentWindowShown {
    return [NSUserDefaults.standardUserDefaults
        boolForKey:@"ui.agentWindow.shown"];
}

- (void)setAgentWindowShown:(BOOL)shown {
    [NSUserDefaults.standardUserDefaults setBool:shown
                                          forKey:@"ui.agentWindow.shown"];
}

- (nullable NSString*)videoOutputScreen {
    return [NSUserDefaults.standardUserDefaults
        stringForKey:@"ui.videoOutput.screen"];
}

- (void)setVideoOutputScreen:(nullable NSString*)screen {
    NSUserDefaults* defaults = NSUserDefaults.standardUserDefaults;
    if (screen.length > 0) {
        [defaults setObject:screen forKey:@"ui.videoOutput.screen"];
    } else {
        [defaults removeObjectForKey:@"ui.videoOutput.screen"];
    }
}

@end
