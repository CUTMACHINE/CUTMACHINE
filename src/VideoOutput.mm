#import "AppDelegatePrivate.h"

// CF1 -- docs/roadmap/CLEAN-FEED-2026-10.md. A passive window keeps the
// editor's focus and consumes the very frames Record presents. Screen
// changes suspend output rather than moving it onto another display.

@interface CMCleanFeedWindow : NSWindow
@end

@implementation CMCleanFeedWindow
- (BOOL)canBecomeKeyWindow {
    return NO;
}
- (BOOL)canBecomeMainWindow {
    return NO;
}
@end

namespace {

std::string Utf8(NSString* _Nullable value) {
    const char* text = value.UTF8String;
    return text != nullptr ? text : "";
}

NSScreen* _Nullable EditorScreen(AppDelegate* editor) {
    NSScreen* screen = editor.window.screen;
    return screen != nil ? screen : NSScreen.mainScreen;
}

NSString* ScreenIdentifier(NSScreen* _Nullable screen) {
    NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
    if (number == nil) {
        return @"";
    }
    CFUUIDRef uuid = CGDisplayCreateUUIDFromDisplayID(number.unsignedIntValue);
    if (uuid == nullptr) {
        return @"";
    }
    NSString* identifier = CFBridgingRelease(CFUUIDCreateString(nullptr, uuid));
    CFRelease(uuid);
    return identifier != nil ? identifier : @"";
}

VideoOutputState OutputState(AppDelegate* editor) {
    VideoOutputState state;
    state.selected_screen_id = Utf8(editor.videoOutputSelection);
    state.active_screen_id = Utf8(editor.videoOutputActiveScreen);
    NSString* editorScreen = ScreenIdentifier(EditorScreen(editor));
    for (NSScreen* screen in NSScreen.screens) {
        NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
        if (number == nil) {
            continue;
        }
        const CGDirectDisplayID display = number.unsignedIntValue;
        state.screens.push_back(
            {Utf8(ScreenIdentifier(screen)), Utf8(screen.localizedName),
             static_cast<int64_t>(CGDisplayPixelsWide(display)),
             static_cast<int64_t>(CGDisplayPixelsHigh(display)),
             [ScreenIdentifier(screen) isEqualToString:editorScreen] != NO});
    }
    return state;
}

}  // namespace

@implementation AppDelegate (VideoOutput)

- (void)startVideoOutput {
#if defined(CUTMACHINE_UI_SMOKE_TEST)
    self.videoOutputSelection = @"";
#else
    NSString* saved = [CMUiPreferences.sharedPreferences videoOutputScreen];
    self.videoOutputSelection = saved != nil ? saved : @"";
#endif
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    [center addObserver:self
               selector:@selector(videoOutputScreensChanged:)
                   name:NSApplicationDidChangeScreenParametersNotification
                 object:nil];
    [center addObserver:self
               selector:@selector(videoOutputScreensChanged:)
                   name:NSWindowDidChangeScreenNotification
                 object:self.window];
    [center addObserver:self
               selector:@selector(videoOutputEditorWillClose:)
                   name:NSWindowWillCloseNotification
                 object:self.window];
    [self videoOutputScreensChanged:nil];
}

- (void)stopVideoOutput {
    [self.videoOutputWindow close];
    self.videoOutputWindow = nil;
    self.videoOutputView = nil;
    self.videoOutputCaptionView = nil;
    self.videoOutputActiveScreen = @"";
    if (self.state != nullptr) {
        self.state->videoOutputRenderer.reset();
        self.state->videoOutputNeedsPresent = false;
    }
}

- (BOOL)showVideoOutputOnScreen:(NSScreen* _Nullable)screen
                        message:(std::string&)message {
    if (screen == nil) {
        message = "the selected screen is not connected";
        return NO;
    }
    if (self.state == nullptr || !self.state->programRenderer) {
        message = "video output requires an open editor window";
        return NO;
    }
    if (self.videoOutputWindow != nil &&
        [self.videoOutputActiveScreen
            isEqualToString:ScreenIdentifier(screen)] &&
        NSEqualRects(self.videoOutputWindow.frame, screen.frame) &&
        self.videoOutputView.layer.contentsScale == screen.backingScaleFactor) {
        return YES;
    }
    NSWindow* window = [[CMCleanFeedWindow alloc]
        initWithContentRect:screen.frame
                  styleMask:NSWindowStyleMaskBorderless
                    backing:NSBackingStoreBuffered
                      defer:NO
                     screen:screen];
    window.releasedWhenClosed = NO;
    window.title = @"CUTMACHINE — Sortie vidéo";
    window.backgroundColor = CMThemeColor(ui::theme::kSurfaceSunken);
    window.opaque = YES;
    window.ignoresMouseEvents = YES;
    window.level = NSMainMenuWindowLevel + 1;
    window.animationBehavior = NSWindowAnimationBehaviorNone;
    window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces |
                                NSWindowCollectionBehaviorFullScreenAuxiliary;
    TimelineMetalView* view =
        [[TimelineMetalView alloc] initWithFrame:window.contentView.bounds];
    view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [window.contentView addSubview:view];
    auto renderer = std::make_unique<Renderer>();
    if (!renderer->Initialize(view)) {
        message = "unable to initialize the video output's Metal renderer";
        return NO;
    }
    CMCaptionView* captions = [[CMCaptionView alloc] initWithFrame:NSZeroRect];
    captions.hidden = YES;
    [view addSubview:captions];
    [self stopVideoOutput];
    self.videoOutputWindow = window;
    self.videoOutputView = view;
    self.videoOutputCaptionView = captions;
    self.videoOutputActiveScreen = ScreenIdentifier(screen);
    self.state->videoOutputRenderer = std::move(renderer);
    self.state->videoOutputNeedsPresent = true;
    view.resizeTarget = self;
    self.state->overlayDirty = true;
    // No makeKey: the timeline retains its shortcuts, mouse and focus.
    [window orderFront:nil];
    return YES;
}

- (void)videoOutputEditorWillClose:(NSNotification*)notification {
    (void)notification;
    [self stopVideoOutput];
}

- (void)videoOutputScreensChanged:(NSNotification*)notification {
    (void)notification;
    NSScreen* target = nil;
    NSString* editorScreen = ScreenIdentifier(EditorScreen(self));
    for (NSScreen* screen in NSScreen.screens) {
        if (![ScreenIdentifier(screen) isEqualToString:editorScreen] &&
            [ScreenIdentifier(screen)
                isEqualToString:self.videoOutputSelection]) {
            target = screen;
        }
    }
    if (target != nil && self.state != nullptr && self.state->programRenderer) {
        std::string error;
        if (![self showVideoOutputOnScreen:target message:error]) {
            self.infoLabel.stringValue =
                @"La sortie vidéo n’a pas pu démarrer.";
            std::fprintf(stderr, "Video output: %s\n", error.c_str());
        }
    } else {
        [self stopVideoOutput];
    }
    [self refreshVideoOutputMenu];
}

- (void)refreshVideoOutputMenu {
    NSMenu* menu = self.videoOutputMenu;
    if (menu == nil) {
        return;
    }
    [menu removeAllItems];
    NSMenuItem* off =
        [[NSMenuItem alloc] initWithTitle:@"Désactivée"
                                   action:@selector(selectVideoOutput:)
                            keyEquivalent:@""];
    off.target = self;
    off.representedObject = @"";
    off.state = self.videoOutputSelection.length == 0 ? NSControlStateValueOn
                                                      : NSControlStateValueOff;
    [menu addItem:off];
    [menu addItem:NSMenuItem.separatorItem];
    const VideoOutputState state = OutputState(self);
    bool selectedFound = state.selected_screen_id.empty();
    for (const VideoOutputScreen& screen : state.screens) {
        NSString* title = CMStringFromUtf8(screen.name);
        if (screen.editor_screen) {
            title = [title stringByAppendingString:@" — écran de l’éditeur"];
        }
        NSMenuItem* item =
            [[NSMenuItem alloc] initWithTitle:title
                                       action:@selector(selectVideoOutput:)
                                keyEquivalent:@""];
        item.target = self;
        item.representedObject = CMStringFromUtf8(screen.id);
        item.enabled = !screen.editor_screen;
        const bool selected = screen.id == state.selected_screen_id;
        selectedFound |= selected;
        item.state = selected ? NSControlStateValueOn : NSControlStateValueOff;
        [menu addItem:item];
    }
    if (!selectedFound) {
        NSMenuItem* absent = [[NSMenuItem alloc]
            initWithTitle:@"Écran choisi déconnecté — sortie suspendue"
                   action:nil
            keyEquivalent:@""];
        absent.enabled = NO;
        [menu addItem:absent];
    }
}

- (BOOL)accessVideoOutput:(const std::optional<std::string>&)screen
                    state:(VideoOutputState&)state
                errorName:(std::string&)errorName
                  message:(std::string&)message {
    state = OutputState(self);
    if (!screen) {
        return YES;
    }
    if (!ValidateVideoOutputScreen(*screen, state, message)) {
        errorName = "ValidationFailed";
        return NO;
    }
    if (!screen->empty()) {
        bool found = false;
        for (NSScreen* candidate in NSScreen.screens) {
            if (*screen != Utf8(ScreenIdentifier(candidate))) {
                continue;
            }
            if (![self showVideoOutputOnScreen:candidate message:message]) {
                errorName = "IoError";
                return NO;
            }
            found = true;
            break;
        }
        if (!found) {
            errorName = "ValidationFailed";
            message = "the selected screen was disconnected";
            return NO;
        }
    } else {
        [self stopVideoOutput];
    }
    self.videoOutputSelection = CMStringFromUtf8(*screen);
#if !defined(CUTMACHINE_UI_SMOKE_TEST)
    [CMUiPreferences.sharedPreferences
        setVideoOutputScreen:self.videoOutputSelection];
#endif
    [self refreshVideoOutputMenu];
    state = OutputState(self);
    return YES;
}

- (void)selectVideoOutput:(NSMenuItem*)sender {
    const std::optional<std::string> screen{Utf8(sender.representedObject)};
    VideoOutputState state;
    std::string name;
    std::string message;
    if (![self accessVideoOutput:screen
                           state:state
                       errorName:name
                         message:message]) {
        self.infoLabel.stringValue =
            name == "ValidationFailed"
                ? @"Écran indisponible pour la sortie vidéo."
                : @"La sortie vidéo n’a pas pu démarrer.";
        std::fprintf(stderr, "Video output: %s\n", message.c_str());
        NSBeep();
    }
}

@end
