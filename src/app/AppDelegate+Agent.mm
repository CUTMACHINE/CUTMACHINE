// STRUCTURE-2026-11 ST4 -- docs/roadmap/STRUCTURE-2026-11.md. The agent from
// the window: its floating window and toggle, the endpoint an outside agent
// reaches the open project through (AX-2026-09 AX1), and the journal of
// what it did, with its marks (PRD25, AgentJournal.h). Moved out of main.mm
// unchanged.

#include "AppDelegatePrivate.h"

static ui::agent_window::Rect CMAgentRect(NSRect rect) {
    return {rect.origin.x, rect.origin.y, rect.size.width, rect.size.height};
}

@implementation AppDelegate (Agent)

// ON3 -- docs/roadmap/ONGLETS-2026-10.md. The Agent's own window, floating
// over the editor (AgentWindow.h), where it was the right dock's second tab:
// shown and hidden from any page by the bar's toggle, the Affichage menu or
// its key, and opened where it was left. A palette: it hides with the
// application, and the film shown full screen, whose window is above every
// floating one, covers it. Its buttons and its thread leave the keyboard
// where it was; its field takes it.
- (void)buildAgentWindow {
    NSPanel* panel = [[NSPanel alloc]
        initWithContentRect:NSMakeRect(0.0, 0.0,
                                       ui::agent_window::kDefaultWidth,
                                       ui::agent_window::kDefaultHeight)
                  styleMask:NSWindowStyleMaskTitled |
                            NSWindowStyleMaskClosable |
                            NSWindowStyleMaskResizable |
                            NSWindowStyleMaskUtilityWindow
                    backing:NSBackingStoreBuffered
                      defer:YES];
    panel.title = @"Agent";
    panel.floatingPanel = YES;
    panel.hidesOnDeactivate = YES;
    panel.becomesKeyOnlyIfNeeded = YES;
    panel.releasedWhenClosed = NO;
    panel.minSize = NSMakeSize(ui::agent_window::kMinimumWidth,
                               ui::agent_window::kMinimumHeight);
    panel.collectionBehavior = NSWindowCollectionBehaviorFullScreenAuxiliary;
    panel.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    panel.backgroundColor = CMSurfacePanelColor();
    panel.delegate = self;
    self.chatPanelView.autoresizingMask =
        NSViewWidthSizable | NSViewHeightSizable;
    panel.contentView = self.chatPanelView;
    // Its forms are the editor's, sheets on the editor's window.
    self.chatPanelView.formWindow = self.window;
    __weak AppDelegate* weakSelf = self;
    self.chatPanelView.activityHandler = ^{
      [weakSelf agentDidWrite];
    };
    self.agentWindow = panel;
}

- (void)toggleAgentWindow:(id)sender {
    (void)sender;
    [self showAgentWindow:!self.agentShown focus:YES];
}

// Shown where AgentWindow.h places it: where it was left this session, or
// at the last launch, while that can be reached on a screen. `focus` puts
// the keyboard in its field, for a request; hidden, the keyboard it had
// goes back to the editor.
- (void)showAgentWindow:(BOOL)shown focus:(BOOL)focus {
    NSPanel* panel = self.agentWindow;
    if (!panel) return;
    CMUiPreferences* preferences = CMUiPreferences.sharedPreferences;
    if (shown) {
        std::optional<ui::agent_window::Rect> remembered;
        if (self.agentPlaced)
            remembered = CMAgentRect(panel.frame);
        else if (self.rememberWorkspace)
            remembered = [preferences agentWindowFrame];
        std::vector<ui::agent_window::Rect> screens;
        for (NSScreen* screen in NSScreen.screens)
            screens.push_back(CMAgentRect(screen.visibleFrame));
        const ui::agent_window::Rect place = ui::agent_window::PlaceAgentWindow(
            remembered, CMAgentRect(self.window.frame), screens);
        self.agentPlacing = YES;
        [panel setFrame:NSMakeRect(place.x, place.y, place.width, place.height)
                display:NO];
        self.agentPlacing = NO;
        self.agentPlaced = YES;
        self.agentShown = YES;
        self.agentUnread = NO;
        if (focus) {
            [panel makeKeyAndOrderFront:nil];
            [self.chatPanelView focusInput];
        } else {
            [panel orderFront:nil];
        }
    } else {
        const BOOL hadKeyboard = panel.isKeyWindow;
        self.agentShown = NO;
        [panel orderOut:nil];
        if (hadKeyboard) [self.window makeKeyWindow];
    }
    if (self.rememberWorkspace) {
        [preferences setAgentWindowShown:self.agentShown];
        if (self.agentPlaced)
            [preferences setAgentWindowFrame:CMAgentRect(panel.frame)];
    }
    [self refreshAgentToggle];
}

// A row added to the thread while the window is hidden marks the toggle,
// until the window is shown again.
- (void)agentDidWrite {
    if (self.agentShown || self.agentUnread) return;
    self.agentUnread = YES;
    [self refreshAgentToggle];
}

- (void)refreshAgentToggle {
    NSString* spec = self.shortcutBindings[@"panel.agent"];
    NSString* key = spec ? CMStringFromUtf8(ui::shortcuts::DisplaySpec(
                               spec.UTF8String ?: ""))
                         : @"";
    [self.workspaceBar showAgentShown:self.agentShown
                               unread:self.agentUnread
                                  key:key];
}

// Moved or resized by the hand: where it opens next time.
- (void)agentWindowDidChangeFrame {
    if (self.agentPlacing || !self.agentShown || !self.rememberWorkspace)
        return;
    [CMUiPreferences.sharedPreferences
        setAgentWindowFrame:CMAgentRect(self.agentWindow.frame)];
}

// D7 -- the keys of a text being typed. The Edit menu gives Cmd+Z, Cmd+C
// and Cmd+V to the montage and has neither Cut nor Select All, so in a
// field of this window copying a word copied the selected clip, pasting it
// pasted a clip, and undoing took back the last edit of the film. While a
// text view holds the keyboard these five are its own. Cmd+Z with nothing
// typed to undo does nothing rather than reach the montage: nobody means
// the film by it from inside a field.
// ON3 -- a key typed in the Agent's window: its field keeps the same five
// keys, which the menus would otherwise take for the montage.
- (BOOL)agentWindowTakesKey:(NSEvent*)event {
    NSWindow* keyed = event.window ?: NSApp.keyWindow;
    if (keyed == nil || keyed != self.agentWindow) return NO;
    NSResponder* typing = keyed.firstResponder;
    return [typing isKindOfClass:NSTextView.class] &&
           [self textEditKeyDown:event inTextView:(NSTextView*)typing];
}

// CH6 -- a key typed in a field of the Charte panel, or of its sheet
// (Nouvelle charte): the same five keys stay the field's, which the menus
// would otherwise give the montage. CH6 (review) -- a text that only reads
// (the file's JSON) keeps Cmd+C and Cmd+A for its selection, but Cmd+Z and
// Cmd+Shift+Z go on to Édition, which gives them to the charter's versions:
// there is no typing there to undo.
- (BOOL)charterWindowTakesKey:(NSEvent*)event {
    NSWindow* keyed = event.window ?: NSApp.keyWindow;
    NSWindow* panel = self.charterPanel.window;
    if (keyed == nil || panel == nil ||
        (keyed != panel && keyed.sheetParent != panel))
        return NO;
    NSResponder* responder = keyed.firstResponder;
    if (![responder isKindOfClass:NSTextView.class]) return NO;
    NSTextView* text = (NSTextView*)responder;
    if (text != [self.charterPanel typingTextView] &&
        [event.charactersIgnoringModifiers.lowercaseString
            isEqualToString:@"z"])
        return NO;
    return [self textEditKeyDown:event inTextView:text];
}

// CH6 (review) -- a montage command of the menu bar while the Charte panel,
// or a sheet of it, holds the keyboard: refused with a beep.
// -validateMenuItem: greys it; this holds on a path that did not ask. A
// context menu of the editor window goes through: it was clicked on the
// montage.
- (BOOL)charterRefusesMontageCommand:(id)sender {
    if (![self.charterPanel holdsKeyboard]) return NO;
    if ([sender isKindOfClass:NSMenuItem.class] &&
        !IsMenuBarItem((NSMenuItem*)sender))
        return NO;
    NSBeep();
    return YES;
}

// AX1 -- docs/roadmap/AX-2026-09.md. The MCP server Claude Code starts from
// `.mcp.json` relays to this endpoint when it is asked to open this project
// (McpEndpoint.h): the agent edits the document on screen, through the
// chat panel's live backend, one journal, one Undo. Announced in the
// project's lock directory, so only while this window holds the project.
- (void)startAgentEndpoint {
    if (self.agentEndpoint || !self.chatBackend ||
        self.documentPath.length == 0 || !self.state ||
        !self.state->projectLock || !self.state->projectLock->Held())
        return;
    std::string socketPath;
    std::string error;
    if (!PrepareMcpSocketPath(GenerateUlid(), socketPath, error)) {
        std::fprintf(stderr, "Agent endpoint not started: %s\n", error.c_str());
        return;
    }
    self.agentEndpointBackend = new MainThreadBackend(*self.chatBackend);
    McpEndpoint* endpoint = new McpEndpoint(*self.agentEndpointBackend);
    // DEC10 -- docs/roadmap/AX-2026-09.md (AX7). The relayed agent's tasks
    // (exports, analyses asked with `async: true`) run on the editor's
    // manager: the task bar shows them and cancels them, as it does
    // ingest_media. The manager outlives the endpoint, which is abandoned,
    // never destroyed, as the app quits.
    if (self.state->mediaTasks)
        endpoint->Server().Session().SetTaskManager(
            self.state->mediaTasks.get(), "Agent · ");
    __weak AppDelegate* weakSelf = self;
    endpoint->SetObserver([weakSelf](const McpEndpoint::Event& event) {
        const McpEndpoint::Event copy = event;
        dispatch_async(dispatch_get_main_queue(), ^{
          AppDelegate* strongSelf = weakSelf;
          if (strongSelf) [strongSelf showAgentEndpointEvent:copy];
        });
    });
    if (!endpoint->Start(socketPath, error) ||
        !endpoint->Announce(self.documentPath.UTF8String, error)) {
        std::fprintf(stderr, "Agent endpoint not started: %s\n", error.c_str());
        endpoint->Stop();
        return;
    }
    self.agentEndpoint = endpoint;
}

- (void)showAgentEndpointEvent:(const McpEndpoint::Event&)event {
    switch (event.kind) {
        case McpEndpoint::Event::Kind::Connected:
            self.infoLabel.stringValue = @"Agent connecté au projet";
            break;
        case McpEndpoint::Event::Kind::Disconnected:
            self.infoLabel.stringValue = @"Agent déconnecté";
            break;
        case McpEndpoint::Event::Kind::ToolCall:
            self.infoLabel.stringValue = [NSString
                stringWithFormat:@"Agent : %@%@", CMStringFromUtf8(event.tool),
                                 event.ok ? @"" : @" (refusé)"];
            break;
    }
    // PRD25 -- docs/roadmap/DIALOGUE-2026-10.md (D3). The status line keeps
    // the last call; the journal keeps what the agent did (AgentJournal.h),
    // in the Agent panel whether it is shown or not, and the timeline marks
    // the clips its last call touched.
    AgentJournalContext context;
    context.time = [self agentJournalTime];
    AppState* state = self.state;
    context.timeline_name = [state](const std::string& timelineId) {
        if (timelineId == state->activeTimelineId)
            return state->document.sequence.name;
        const DocumentSequence* timeline =
            state->project.FindTimeline(timelineId);
        return timeline != nullptr ? timeline->name : std::string();
    };
    if (state->agentJournal.Record(event, context)) {
        [self.chatPanelView
            showAgentJournal:state->agentJournal.TranscriptEntries()];
        state->overlayDirty = true;
    }
    if (AppTestHooks* hooks = TestHooks()) hooks->AgentEventRecorded();
}

// PRD25 -- the local time a journal line is written at, "HH:MM:SS": read
// here, never in the journal's policy.
- (std::string)agentJournalTime {
    if (AppTestHooks* hooks = TestHooks()) {
        std::string time = hooks->AgentJournalTime();
        if (!time.empty()) return time;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char text[16];
    std::strftime(text, sizeof text, "%H:%M:%S", &local);
    return text;
}

// PRD25 -- the person acted in the timeline (a click, a key, an undo): the
// marks of the agent's last call go, the journal stays.
- (void)dismissAgentMarks {
    if (self.state && self.state->agentJournal.DismissMarks())
        self.state->overlayDirty = true;
}

@end
