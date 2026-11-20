#pragma once

// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Projects from
// the window: the startup window and its recent projects, creating,
// opening and saving a project, and switching to another one (DEC39,
// ProjectSwitch.h). Moved out of main.mm unchanged.

@interface AppDelegate (Projects)
- (void)saveProjectPressed:(id)sender;
- (NSArray<NSString*>*)recentProjectPaths;
- (void)recordRecentProject:(NSString*)path;
- (void)startupNewPressed:(id)sender;
- (void)startupOpenPressed:(id)sender;
- (void)startupRecentPressed:(id)sender;
- (void)startupQuitPressed:(id)sender;
- (void)showStartupWindow;
- (void)buildStartupWindow;
- (void)installStartupSetupNoticeIn:(NSView*)content
                               left:(double)left
                             beside:(NSRect)quit;
- (void)applyStartupSetupNotice:(const std::string&)line;
- (CMFormPanel*)newProjectNamePanel;
- (NSSavePanel*)newProjectSavePanelNamed:(NSString*)projectName;
- (NSString*)createProjectNamed:(NSString*)projectName
                             at:(NSURL*)destination
                          error:(NSString**)failure;
- (BOOL)createStartupProject;
- (NSString*)resolvedProjectPath:(NSString*)selection;
- (NSOpenPanel*)projectOpenPanel;
- (BOOL)openStartupProject;
- (BOOL)projectSwitchAllowed;
- (void)showProjectSwitchRefusal:(NSString*)refusal;
- (void)newProjectFromEditor:(id)sender;
- (void)chooseNewProjectLocationNamed:(NSString*)projectName;
- (void)openProjectFromEditor:(id)sender;
- (BOOL)switchToProjectAtPath:(NSString*)chosen;
- (BOOL)chooseStartupProject;
@end
