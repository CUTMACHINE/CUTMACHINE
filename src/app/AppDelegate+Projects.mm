// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. Projects from
// the window: the startup window and its recent projects, creating,
// opening and saving a project, and switching to another one (DEC39,
// ProjectSwitch.h). Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"
#include "ProjectSwitch.h"

// The name typed in the new project's form, or « Nouveau projet ».
static NSString* CMNewProjectName(CMFormPanel* panel) {
    NSString* projectName =
        CMTrimmedName(((NSTextField*)panel.form).stringValue);
    return projectName.length > 0 ? projectName : @"Nouveau projet";
}

@implementation AppDelegate (Projects)

- (void)saveProjectPressed:(id)sender {
    (void)sender;
    std::string message;
    if ([self persistEdits:message]) {
        self.infoLabel.stringValue = @"Projet enregistré";
    } else {
        self.infoLabel.stringValue =
            [NSString stringWithFormat:@"Échec de l’enregistrement : %@",
                                       CMStringFromUtf8(message)];
        NSBeep();
    }
}

- (NSArray<NSString*>*)recentProjectPaths {
    NSArray* stored = [NSUserDefaults.standardUserDefaults
        arrayForKey:@"RecentProjectPaths.v2"];
    NSMutableArray<NSString*>* available = [NSMutableArray array];
    for (id value in stored) {
        if (![value isKindOfClass:NSString.class]) continue;
        BOOL directory = NO;
        if ([NSFileManager.defaultManager fileExistsAtPath:value
                                               isDirectory:&directory] &&
            !directory)
            [available addObject:value];
        if (available.count == 8) break;
    }
    return available;
}

- (void)recordRecentProject:(NSString*)path {
    if (path.length == 0) return;
    NSString* standardized = path.stringByStandardizingPath;
    NSMutableArray<NSString*>* recent =
        [NSMutableArray arrayWithObject:standardized];
    for (NSString* candidate in [self recentProjectPaths]) {
        if (![candidate isEqualToString:standardized])
            [recent addObject:candidate];
        if (recent.count == 8) break;
    }
    [NSUserDefaults.standardUserDefaults setObject:recent
                                            forKey:@"RecentProjectPaths.v2"];
}

- (void)startupNewPressed:(id)sender {
    (void)sender;
    self.startupChoice = 1;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupOpenPressed:(id)sender {
    (void)sender;
    self.startupChoice = 2;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupRecentPressed:(id)sender {
    (void)sender;
    NSString* path = self.recentProjectsPopup.selectedItem.representedObject;
    if (path.length == 0) return;
    self.startupSelectedPath = path;
    self.startupChoice = 3;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)startupQuitPressed:(id)sender {
    (void)sender;
    self.startupChoice = 0;
    [NSApp stopModal];
    [self.startupWindow orderOut:nil];
}

- (void)showStartupWindow {
    [self buildStartupWindow];
    [self.startupWindow makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp runModalForWindow:self.startupWindow];
}

// PRD21 (window) -- built apart from its modal session, so the window test
// builds and reads the screen the way it is shown, without a session that
// would hold the run until someone answers it.
- (void)buildStartupWindow {
    const NSRect frame = NSMakeRect(0, 0, 680, 430);
    self.startupWindow =
        [[NSWindow alloc] initWithContentRect:frame
                                    styleMask:NSWindowStyleMaskTitled
                                      backing:NSBackingStoreBuffered
                                        defer:NO];
    self.startupWindow.title = @"Bienvenue dans CUTMACHINE";
    self.startupWindow.releasedWhenClosed = NO;
    [self.startupWindow center];

    NSView* content = [[NSView alloc] initWithFrame:frame];
    content.wantsLayer = YES;
    content.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfacePanel).CGColor;
    self.startupWindow.contentView = content;

    NSImageView* icon =
        [[NSImageView alloc] initWithFrame:NSMakeRect(54, 278, 92, 92)];
    icon.image = NSApp.applicationIconImage
                     ?: SystemSymbol(@"scissors", @"CUTMACHINE", 64.0);
    icon.imageScaling = NSImageScaleProportionallyUpOrDown;
    [content addSubview:icon];

    NSTextField* title = [NSTextField labelWithString:@"CUTMACHINE"];
    title.frame = NSMakeRect(170, 325, 455, 42);
    title.font = [NSFont systemFontOfSize:30.0 weight:NSFontWeightBold];
    title.textColor = NSColor.labelColor;
    [content addSubview:title];
    NSTextField* subtitle = [NSTextField
        labelWithString:
            @"Commencez un nouveau montage ou reprenez un projet existant."];
    subtitle.frame = NSMakeRect(172, 294, 445, 24);
    subtitle.font = [NSFont systemFontOfSize:14.0];
    subtitle.textColor = NSColor.secondaryLabelColor;
    [content addSubview:subtitle];

    NSButton* create = [NSButton buttonWithTitle:@"Nouveau projet"
                                          target:self
                                          action:@selector(startupNewPressed:)];
    create.frame = NSMakeRect(54, 211, 274, 56);
    create.bezelStyle = NSBezelStyleTexturedRounded;
    create.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    create.image = SystemSymbol(@"plus.square", @"Nouveau projet", 20.0);
    create.imagePosition = NSImageLeading;
    create.keyEquivalent = @"\r";
    [content addSubview:create];

    NSButton* open = [NSButton buttonWithTitle:@"Ouvrir un projet…"
                                        target:self
                                        action:@selector(startupOpenPressed:)];
    open.frame = NSMakeRect(352, 211, 274, 56);
    open.bezelStyle = NSBezelStyleTexturedRounded;
    open.font = [NSFont systemFontOfSize:15.0 weight:NSFontWeightSemibold];
    open.image = SystemSymbol(@"folder", @"Ouvrir un projet", 20.0);
    open.imagePosition = NSImageLeading;
    [content addSubview:open];

    NSTextField* recentLabel = [NSTextField labelWithString:@"Projets récents"];
    recentLabel.frame = NSMakeRect(54, 158, 160, 22);
    recentLabel.font = [NSFont systemFontOfSize:12.0
                                         weight:NSFontWeightSemibold];
    recentLabel.textColor = NSColor.secondaryLabelColor;
    [content addSubview:recentLabel];
    self.recentProjectsPopup =
        [[NSPopUpButton alloc] initWithFrame:NSMakeRect(54, 116, 460, 34)];
    NSArray<NSString*>* recentPaths = [self recentProjectPaths];
    if (recentPaths.count == 0) {
        [self.recentProjectsPopup addItemWithTitle:@"Aucun projet récent"];
        self.recentProjectsPopup.enabled = NO;
    } else {
        for (NSString* path in recentPaths) {
            NSString* title =
                path.lastPathComponent.stringByDeletingPathExtension;
            if ([title.pathExtension.lowercaseString
                    isEqualToString:@"cutmachine"])
                title = title.stringByDeletingPathExtension;
            [self.recentProjectsPopup addItemWithTitle:title];
            self.recentProjectsPopup.lastItem.representedObject = path;
            self.recentProjectsPopup.lastItem.toolTip = path;
        }
    }
    [content addSubview:self.recentProjectsPopup];
    NSButton* recent =
        [NSButton buttonWithTitle:@"Ouvrir"
                           target:self
                           action:@selector(startupRecentPressed:)];
    recent.frame = NSMakeRect(526, 116, 100, 34);
    recent.enabled = recentPaths.count > 0;
    [content addSubview:recent];

    NSButton* quit = [NSButton buttonWithTitle:@"Quitter"
                                        target:self
                                        action:@selector(startupQuitPressed:)];
    quit.frame = NSMakeRect(526, 38, 100, 30);
    quit.keyEquivalent = @"\e";
    [content addSubview:quit];

    // PRD21 (window) -- at the foot, left of « Quitter », from the screen's
    // left margin.
    [self installStartupSetupNoticeIn:content left:54.0 beside:quit.frame];
}

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. An app opened from the
// Finder gets launchd's PATH, where FFmpeg is not, and nothing said so: the
// welcome screen now says what this machine lacks to edit and export
// (SetupNotice.h), with « Diagnostic… » for the rest. The row stays hidden
// until the report comes back from its thread: neither a slow disk nor an
// FFmpeg that does not answer delays the screen.
- (void)installStartupSetupNoticeIn:(NSView*)content
                               left:(double)left
                             beside:(NSRect)quit {
    using namespace ui::theme;
    const NSRect rowFrame =
        NSMakeRect(left, NSMinY(quit) - kSpaceXs, NSMinX(quit) - kSpaceM - left,
                   NSHeight(quit) + 2.0 * kSpaceXs);
    NSView* row = [[NSView alloc] initWithFrame:rowFrame];
    row.identifier = @"startup.setup_notice";
    row.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    row.hidden = YES;

    constexpr double kIconSize = 16.0;
    NSImageView* icon = [[NSImageView alloc]
        initWithFrame:NSMakeRect(
                          0, std::floor((NSHeight(rowFrame) - kIconSize) / 2.0),
                          kIconSize, kIconSize)];
    icon.identifier = @"startup.setup_notice.icon";
    icon.image = SystemSymbol(@"exclamationmark.triangle.fill",
                              @"Installation incomplète", kFontSizeSection);
    icon.imageScaling = NSImageScaleProportionallyDown;
    icon.contentTintColor = CMThemeColor(kError);
    icon.autoresizingMask = NSViewMaxXMargin;
    [row addSubview:icon];

    NSButton* diagnose =
        [NSButton buttonWithTitle:@"Diagnostic…"
                           target:self
                           action:@selector(showSetupDiagnostic:)];
    diagnose.identifier = @"startup.setup_notice.diagnose";
    [diagnose sizeToFit];
    const double buttonWidth = std::max(NSWidth(quit), NSWidth(diagnose.frame));
    diagnose.frame = NSMakeRect(NSWidth(rowFrame) - buttonWidth, kSpaceXs,
                                buttonWidth, NSHeight(quit));
    diagnose.autoresizingMask = NSViewMinXMargin;
    [row addSubview:diagnose];

    const double labelX = kIconSize + kSpaceS;
    NSTextField* label = [NSTextField wrappingLabelWithString:@""];
    label.identifier = @"startup.setup_notice.text";
    label.frame =
        NSMakeRect(labelX, 0, NSMinX(diagnose.frame) - kSpaceM - labelX,
                   NSHeight(rowFrame));
    label.font = CMFont(kFontSizeBody, NSFontWeightRegular);
    label.textColor = CMTextPrimaryColor();
    label.maximumNumberOfLines = 2;
    label.cell.truncatesLastVisibleLine = YES;
    label.autoresizingMask = NSViewWidthSizable;
    [row addSubview:label];
    [content addSubview:row];
    self.startupSetupNotice = row;
    self.startupSetupNoticeLabel = label;
    self.startupSetupNoticeChecked = NO;

    __weak AppDelegate* weakSelf = self;
    __weak NSWindow* window = self.startupWindow;
    [self gatherSetupNotice:^(const SetupNoticeResult& result) {
      AppDelegate* strongSelf = weakSelf;
      // A screen built again meanwhile has its own report on the way.
      if (!strongSelf || !window || strongSelf.startupWindow != window) return;
      [strongSelf
          applyStartupSetupNotice:setup_notice::WelcomeLine(result.report)];
    }];
}

- (void)applyStartupSetupNotice:(const std::string&)line {
    NSView* row = self.startupSetupNotice;
    NSTextField* label = self.startupSetupNoticeLabel;
    label.stringValue = CMStringFromUtf8(line);
    // Two lines at most, centred on the row.
    NSRect frame = label.frame;
    const NSSize needed = [label.cell
        cellSizeForBounds:NSMakeRect(0, 0, NSWidth(frame), CGFLOAT_MAX)];
    frame.size.height =
        std::min(std::ceil(needed.height), NSHeight(row.bounds));
    frame.origin.y = std::floor((NSHeight(row.bounds) - NSHeight(frame)) / 2.0);
    label.frame = frame;
    row.hidden = line.empty();
    self.startupSetupNoticeChecked = YES;
}

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. The new project's name, asked
// on the form panel: modal, the welcome screen having no window to hang a
// sheet from. Built apart, so the window test reads it without its modal
// loop.
- (CMFormPanel*)newProjectNamePanel {
    NSTextField* name =
        [[NSTextField alloc] initWithFrame:NSMakeRect(0, 0, 380, 26)];
    name.identifier = @"project.new.name";
    name.stringValue = @"Nouveau projet";
    name.placeholderString = @"Nom du projet";
    CMFormPanel* panel =
        [[CMFormPanel alloc] initWithTitle:@"Créer un nouveau projet"
                                   message:@"Donnez un nom au projet."
                                      form:name];
    [panel addButtonWithTitle:@"Continuer" role:CMFormButtonRoleAccept];
    [panel addButtonWithTitle:@"Annuler" role:CMFormButtonRoleCancel];
    panel.initialFirstResponder = name;
    return panel;
}

// Where the new project goes: the welcome screen runs it modal, the editor
// as a sheet (DEC39).
- (NSSavePanel*)newProjectSavePanelNamed:(NSString*)projectName {
    NSSavePanel* save = [NSSavePanel savePanel];
    save.title = @"Enregistrer le nouveau projet";
    save.prompt = @"Créer";
    save.nameFieldStringValue =
        [projectName stringByAppendingString:@".cutmachine-project"];
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) save.allowedContentTypes = @[ collectionType ];
    save.canCreateDirectories = YES;
    return save;
}

// The engine's package, made as create_project makes it: the project file
// written, or nil and why.
- (NSString*)createProjectNamed:(NSString*)projectName
                             at:(NSURL*)destination
                          error:(NSString**)failure {
    Project project(projectName.UTF8String ?: "Nouveau projet");
    project.timelines.front().name = "Timeline 1";
    std::string error;
    std::string path;
    if (!CreatePortableProject(destination.fileSystemRepresentation ?: "",
                               project, path, error)) {
        if (failure) *failure = CMStringFromUtf8(error);
        return nil;
    }
    return CMStringFromUtf8(path);
}

- (BOOL)createStartupProject {
    CMFormPanel* panel = [self newProjectNamePanel];
    __block NSModalResponse answer = NSModalResponseCancel;
    [panel presentInWindow:nil
                completion:^(NSModalResponse response) {
                  answer = response;
                }];
    if (answer != NSAlertFirstButtonReturn) return NO;
    NSString* projectName = CMNewProjectName(panel);

    NSSavePanel* save = [self newProjectSavePanelNamed:projectName];
    if ([save runModal] != NSModalResponseOK || !save.URL) return NO;

    NSString* failure = nil;
    NSString* path = [self createProjectNamed:projectName
                                           at:save.URL
                                        error:&failure];
    if (!path) {
        NSAlert* alert = [NSAlert new];
        alert.alertStyle = NSAlertStyleCritical;
        alert.messageText = @"Impossible de créer le projet";
        alert.informativeText = failure;
        [alert runModal];
        return NO;
    }
    self.documentPath = path;
    return YES;
}

- (NSString*)resolvedProjectPath:(NSString*)selection {
    NSString* selected = selection.stringByStandardizingPath;
    BOOL directory = NO;
    if ([NSFileManager.defaultManager fileExistsAtPath:selected
                                           isDirectory:&directory] &&
        directory)
        selected = [selected
            stringByAppendingPathComponent:@"project.cutmachine.json"];
    return selected;
}

// The welcome screen runs it modal, the editor as a sheet (DEC39).
- (NSOpenPanel*)projectOpenPanel {
    NSOpenPanel* open = [NSOpenPanel openPanel];
    open.title = @"Ouvrir un projet CUTMACHINE";
    open.prompt = @"Ouvrir";
    UTType* collectionType =
        [UTType typeWithIdentifier:@"com.cutmachine.project-collection"];
    if (collectionType) open.allowedContentTypes = @[ collectionType ];
    open.allowsMultipleSelection = NO;
    open.canChooseFiles = YES;
    open.canChooseDirectories = YES;
    return open;
}

- (BOOL)openStartupProject {
    NSOpenPanel* open = [self projectOpenPanel];
    if ([open runModal] != NSModalResponseOK || !open.URL) return NO;
    self.documentPath = [self resolvedProjectPath:open.URL.path];
    return YES;
}

// Why the window cannot change project now, said on a sheet; NO then.
- (BOOL)projectSwitchAllowed {
    if (!self.state) return NO;
    size_t running = 0;
    if (self.state->mediaTasks) {
        for (const MediaTaskSnapshot& task : self.state->mediaTasks->Snapshot())
            if (task.state == MediaTaskState::Running ||
                task.state == MediaTaskState::Queued)
                ++running;
    }
    const std::string refusal =
        project_switch::BusyRefusal(self.state->exportRunning, running);
    if (refusal.empty()) return YES;
    [self showProjectSwitchRefusal:CMStringFromUtf8(refusal)];
    return NO;
}

- (void)showProjectSwitchRefusal:(NSString*)refusal {
    self.infoLabel.stringValue = refusal;
    NSAlert* alert = [NSAlert new];
    alert.alertStyle = NSAlertStyleWarning;
    alert.messageText = @"Changement de projet impossible";
    alert.informativeText = refusal;
    if (self.window.attachedSheet == nil)
        [alert beginSheetModalForWindow:self.window completionHandler:nil];
    else
        NSBeep();
}

- (void)newProjectFromEditor:(id)sender {
    (void)sender;
    if ([CMFormPanel formShownInWindow:self.window]) {
        NSBeep();
        return;
    }
    if (![self projectSwitchAllowed]) return;
    CMFormPanel* panel = [self newProjectNamePanel];
    __weak AppDelegate* weakSelf = self;
    [panel presentInWindow:self.window
                completion:^(NSModalResponse response) {
                  if (response != NSAlertFirstButtonReturn) return;
                  NSString* projectName = CMNewProjectName(panel);
                  // After this sheet is gone: the save panel is a sheet too.
                  dispatch_async(dispatch_get_main_queue(), ^{
                    [weakSelf chooseNewProjectLocationNamed:projectName];
                  });
                }];
}

- (void)chooseNewProjectLocationNamed:(NSString*)projectName {
    NSSavePanel* save = [self newProjectSavePanelNamed:projectName];
    [save beginSheetModalForWindow:self.window
                 completionHandler:^(NSModalResponse response) {
                   NSURL* chosen = CMSaveSheetAnswer(save, response);
                   if (!chosen) return;
                   NSString* failure = nil;
                   NSString* path = [self createProjectNamed:projectName
                                                          at:chosen
                                                       error:&failure];
                   if (!path) {
                       dispatch_async(dispatch_get_main_queue(), ^{
                         [self showProjectSwitchRefusal:
                                   [@"Impossible de créer le projet : "
                                       stringByAppendingString:failure]];
                       });
                       return;
                   }
                   dispatch_async(dispatch_get_main_queue(), ^{
                     [self switchToProjectAtPath:path];
                   });
                 }];
}

- (void)openProjectFromEditor:(id)sender {
    (void)sender;
    if (self.window.attachedSheet != nil) {
        NSBeep();
        return;
    }
    if (![self projectSwitchAllowed]) return;
    NSOpenPanel* open = [self projectOpenPanel];
    [open beginSheetModalForWindow:self.window
                 completionHandler:^(NSModalResponse response) {
                   if (response != NSModalResponseOK || !open.URL) return;
                   NSString* chosen = open.URL.path;
                   dispatch_async(dispatch_get_main_queue(), ^{
                     [self switchToProjectAtPath:chosen];
                   });
                 }];
}

// Checked again here, the panels having taken their time: busy, the same
// project, one that cannot be opened. Then what this project holds is
// written, and the editor quits to reopen on `chosen`. YES when it does
// (in the window test, when it would: the path is left in
// pendingProjectSwitchPath and nothing quits).
- (BOOL)switchToProjectAtPath:(NSString*)chosen {
    if (![self projectSwitchAllowed]) return NO;
    const project_switch::Plan plan = project_switch::PlanProjectSwitch(
        self.documentPath.fileSystemRepresentation ?: "",
        chosen.fileSystemRepresentation ?: "");
    if (plan.outcome == project_switch::Outcome::kAlreadyOpen) {
        self.infoLabel.stringValue = @"Ce projet est déjà ouvert";
        return NO;
    }
    if (plan.outcome == project_switch::Outcome::kRefused) {
        [self showProjectSwitchRefusal:CMStringFromUtf8(plan.refusal)];
        return NO;
    }
    NSString* executable = NSBundle.mainBundle.executablePath;
    if (executable.length == 0 ||
        access(executable.fileSystemRepresentation, X_OK) != 0) {
        [self showProjectSwitchRefusal:@"CUTMACHINE ne retrouve pas son "
                                       @"propre programme pour se rouvrir "
                                       @"sur l’autre projet."];
        return NO;
    }
    std::string message;
    if (![self persistEdits:message]) {
        [self showProjectSwitchRefusal:
                  [NSString stringWithFormat:@"Ce projet n’a pas pu être "
                                             @"enregistré avant de le "
                                             @"fermer : %@",
                                             CMStringFromUtf8(message)]];
        return NO;
    }
    self.pendingProjectSwitchPath = CMStringFromUtf8(plan.project_path);
    // The window test, which installs its hooks, stays open.
    if (TestHooks() == nullptr) {
        std::fprintf(stderr, "Project switch: reopening on %s\n",
                     plan.project_path.c_str());
        [NSApp terminate:nil];
    }
    return YES;
}

- (BOOL)chooseStartupProject {
    while (self.documentPath.length == 0) {
        self.startupChoice = 0;
        self.startupSelectedPath = nil;
        [self showStartupWindow];
        if (self.startupChoice == 0) return NO;
        if (self.startupChoice == 1) {
            if ([self createStartupProject]) return YES;
        } else if (self.startupChoice == 2) {
            if ([self openStartupProject]) return YES;
        } else if (self.startupChoice == 3 &&
                   self.startupSelectedPath.length > 0) {
            self.documentPath = self.startupSelectedPath;
            return YES;
        }
    }
    return YES;
}

@end
