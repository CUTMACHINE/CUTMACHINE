#import "FormPanel.h"

#include "FormPanelLayout.h"

#import "UiThemeAppKit.h"

#include <algorithm>
#include <cmath>
#include <vector>

// PRD26 -- docs/roadmap/PRODUIT-2026-10b.md. See FormPanel.h. What goes
// where is FormPanelLayout.h's; this measures what AppKit draws (the texts
// at their width, the buttons at their title) and applies the frames.

namespace {

namespace panel = ui::form_panel;

panel::ButtonRole RoleOf(CMFormButtonRole role) {
    switch (role) {
        case CMFormButtonRoleAccept:
            return panel::ButtonRole::kAccept;
        case CMFormButtonRoleCancel:
            return panel::ButtonRole::kCancel;
        case CMFormButtonRoleAlternate:
            return panel::ButtonRole::kAlternate;
        case CMFormButtonRoleAction:
            return panel::ButtonRole::kAction;
    }
    return panel::ButtonRole::kAction;
}

NSRect FrameOf(const panel::Rect& rect) {
    return NSMakeRect(rect.x, rect.y, rect.width, rect.height);
}

// The height `field` needs to show its whole text at `width`; 0 when it
// says nothing.
double WrappedHeight(NSTextField* field, double width) {
    if (field.stringValue.length == 0 || width <= 0.0) return 0.0;
    const NSSize needed =
        [field.cell cellSizeForBounds:NSMakeRect(0, 0, width, CGFLOAT_MAX)];
    return std::ceil(needed.height);
}

// One line of `field`'s font, as its cell draws it.
double LineHeight(NSTextField* field) {
    NSTextFieldCell* probe = [field.cell copy];
    probe.stringValue = @"Ag";
    return std::ceil(
        [probe cellSizeForBounds:NSMakeRect(0, 0, 1000.0, CGFLOAT_MAX)].height);
}

}  // namespace

@implementation CMFormPanel {
    NSMutableArray<NSButton*>* _buttonList;
    std::vector<panel::ButtonRole> _roles;
    // The form's size as its caller gave it: the panel never shrinks it.
    NSSize _formSize;
    BOOL _laidOut;
    BOOL _modal;
    __weak NSWindow* _parent;
    void (^_completion)(NSModalResponse);
}

- (instancetype)initWithTitle:(NSString*)title
                      message:(NSString*)message
                         form:(NSView*)form {
    const NSRect initial = NSMakeRect(0, 0, panel::kMinWidth, 200.0);
    if (!(self = [super initWithContentRect:initial
                                  styleMask:NSWindowStyleMaskTitled
                                    backing:NSBackingStoreBuffered
                                      defer:YES]))
        return nil;
    // Read by VoiceOver and the Window menu; the panel shows its own title,
    // and a modal one keeps an empty bar rather than the same words twice.
    self.title = title;
    self.titleVisibility = NSWindowTitleHidden;
    self.titlebarAppearsTransparent = YES;
    self.releasedWhenClosed = NO;
    // A modal panel that hid when another application came in front would
    // leave CUTMACHINE waiting on nothing visible.
    self.hidesOnDeactivate = NO;
    self.autorecalculatesKeyViewLoop = YES;
    // The theme's surface is dark whatever the system's appearance: the
    // form's own controls and labels, AppKit's, take the dark appearance
    // to read on it.
    self.appearance = [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua];
    self.backgroundColor = CMThemeColor(panel::kSurface);

    NSView* content = [[NSView alloc] initWithFrame:initial];
    content.autoresizesSubviews = YES;
    self.contentView = content;

    _titleLabel = [NSTextField labelWithString:title];
    _titleLabel.identifier = @"form_panel.title";
    _titleLabel.font = CMFont(panel::kTitleFontSize, NSFontWeightSemibold);
    _titleLabel.textColor = CMThemeColor(panel::kTitleText);
    _titleLabel.lineBreakMode = NSLineBreakByTruncatingTail;
    _titleLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_titleLabel];

    _messageLabel = [NSTextField wrappingLabelWithString:message ?: @""];
    _messageLabel.identifier = @"form_panel.message";
    _messageLabel.font = CMFont(panel::kBodyFontSize, NSFontWeightRegular);
    _messageLabel.textColor = CMThemeColor(panel::kMessageText);
    _messageLabel.selectable = NO;
    _messageLabel.autoresizingMask = NSViewWidthSizable | NSViewMinYMargin;
    [content addSubview:_messageLabel];

    _form = form;
    [form removeFromSuperview];
    _formSize = form.frame.size;
    form.autoresizingMask = NSViewMaxXMargin | NSViewMinYMargin;
    [content addSubview:form];

    NSImage* warning =
        [NSImage imageWithSystemSymbolName:@"exclamationmark.triangle.fill"
                  accessibilityDescription:@"Erreur"];
    warning =
        [warning imageWithSymbolConfiguration:
                     [NSImageSymbolConfiguration
                         configurationWithPointSize:panel::kErrorIconPointSize
                                             weight:NSFontWeightMedium]];
    _errorIcon = [[NSImageView alloc] initWithFrame:NSZeroRect];
    _errorIcon.identifier = @"form_panel.error.icon";
    _errorIcon.image = warning;
    _errorIcon.imageScaling = NSImageScaleProportionallyDown;
    _errorIcon.contentTintColor = CMThemeColor(panel::kErrorIcon);
    _errorIcon.hidden = YES;
    _errorIcon.autoresizingMask = NSViewMaxXMargin | NSViewMaxYMargin;
    [content addSubview:_errorIcon];

    _errorLabel = [NSTextField wrappingLabelWithString:@""];
    _errorLabel.identifier = @"form_panel.error";
    _errorLabel.font = CMFont(panel::kBodyFontSize, NSFontWeightRegular);
    _errorLabel.textColor = CMThemeColor(panel::kErrorText);
    // An engine's refusal can be long and worth pasting into a report.
    _errorLabel.selectable = YES;
    _errorLabel.autoresizingMask = NSViewWidthSizable | NSViewMaxYMargin;
    [content addSubview:_errorLabel];

    _buttonList = [NSMutableArray array];
    [self layoutForm];
    return self;
}

- (NSArray<NSButton*>*)buttons {
    return [_buttonList copy];
}

- (NSButton*)addButtonWithTitle:(NSString*)title role:(CMFormButtonRole)role {
    const panel::ButtonRole kind = RoleOf(role);
    NSButton* button = [NSButton buttonWithTitle:title target:nil action:nil];
    button.keyEquivalent = @(panel::KeyEquivalent(kind));
    if (panel::Closes(kind)) {
        button.target = self;
        button.action = @selector(cmButtonPressed:);
    }
    button.autoresizingMask =
        (panel::Trailing(kind) ? NSViewMinXMargin : NSViewMaxXMargin) |
        NSViewMaxYMargin;
    [self.contentView addSubview:button];
    // The window's default button: Return, drawn in the accent colour. While
    // the panel is not key, AppKit holds its key back (measured: the button
    // then reads no key equivalent).
    if (kind == panel::ButtonRole::kAccept &&
        [button.cell isKindOfClass:NSButtonCell.class])
        self.defaultButtonCell = (NSButtonCell*)button.cell;
    [_buttonList addObject:button];
    _roles.push_back(kind);
    [self layoutForm];
    return button;
}

- (void)setFormResizable:(BOOL)formResizable {
    _formResizable = formResizable;
    self.styleMask = formResizable
                         ? (self.styleMask | NSWindowStyleMaskResizable)
                         : (self.styleMask & ~NSWindowStyleMaskResizable);
    _form.autoresizingMask = formResizable
                                 ? NSViewWidthSizable | NSViewHeightSizable
                                 : NSViewMaxXMargin | NSViewMinYMargin;
    [self layoutForm];
}

- (void)setMessage:(NSString*)message {
    NSString* text = message ?: @"";
    if ([text isEqualToString:_messageLabel.stringValue]) return;
    _messageLabel.stringValue = text;
    [self layoutForm];
}

- (void)showError:(NSString*)error {
    NSString* text = error ?: @"";
    if ([text isEqualToString:_errorLabel.stringValue]) return;
    _errorLabel.stringValue = text;
    _errorIcon.hidden = text.length == 0;
    [self layoutForm];
    // Said aloud too: the person may not be looking at the error line.
    if (text.length > 0)
        NSAccessibilityPostNotificationWithUserInfo(
            _errorLabel, NSAccessibilityAnnouncementRequestedNotification, @{
                NSAccessibilityAnnouncementKey : text,
                NSAccessibilityPriorityKey : @(NSAccessibilityPriorityHigh)
            });
}

// What FormPanelLayout.h needs measured. `formSize` is the form's natural
// size, or the one the person gave it by resizing.
- (panel::Input)cmInputForForm:(NSSize)formSize width:(double)width {
    panel::Input input;
    input.content = {formSize.width, formSize.height};
    input.width = width;
    input.stretch_content = self.formResizable;
    const NSSize title = _titleLabel.cell.cellSize;
    input.title_width = std::ceil(title.width);
    input.title_height = std::ceil(title.height);
    for (size_t index = 0; index < _roles.size(); ++index) {
        NSButton* button = _buttonList[index];
        [button sizeToFit];
        input.buttons.push_back({_roles[index], NSWidth(button.frame)});
        input.button_height =
            std::max(input.button_height, (double)NSHeight(button.frame));
    }
    input.error_line_height = LineHeight(_errorLabel);
    const double panelWidth = panel::PanelWidth(input);
    input.message_height =
        WrappedHeight(_messageLabel, panel::TextWidth(panelWidth));
    input.error_height =
        WrappedHeight(_errorLabel, panel::ErrorTextWidth(panelWidth));
    return input;
}

- (void)layoutForm {
    NSView* content = self.contentView;
    // Never smaller than the form as its caller gave it. Measured first:
    // measuring fits every button to its title, which the frames below then
    // widen.
    const panel::Frames least = panel::Compute([self cmInputForForm:_formSize
                                                              width:0.0]);
    // A resizable panel the person may have resized keeps its size; the
    // form took the room.
    const BOOL keepSize = self.formResizable && _laidOut;
    const panel::Frames frames =
        keepSize ? panel::Compute([self cmInputForForm:_form.frame.size
                                                 width:NSWidth(content.bounds)])
                 : least;

    const NSSize size = NSMakeSize(frames.panel.width, frames.panel.height);
    if (!NSEqualSizes(content.bounds.size, size)) {
        // Its top stays where it is: a sheet hangs from it.
        const NSRect wanted = [self
            frameRectForContentRect:NSMakeRect(0, 0, size.width, size.height)];
        NSRect frame = self.frame;
        frame.origin.y += NSHeight(frame) - NSHeight(wanted);
        frame.size = wanted.size;
        [self setFrame:frame display:self.visible];
    }
    _titleLabel.frame = FrameOf(frames.title);
    _messageLabel.frame = FrameOf(frames.message);
    _messageLabel.hidden = frames.message.height <= 0.0;
    _form.frame = FrameOf(frames.content);
    _errorIcon.frame = FrameOf(frames.error_icon);
    _errorLabel.frame = FrameOf(frames.error_text);
    for (size_t index = 0; index < frames.buttons.size(); ++index)
        _buttonList[index].frame = FrameOf(frames.buttons[index]);

    if (self.formResizable)
        self.contentMinSize = NSMakeSize(
            least.panel.width, least.panel.height + (frames.error_text.height -
                                                     least.error_text.height));
    _laidOut = YES;
}

// A form: a panel with an accepting button, which a person fills in -- not
// a progress, which only counts.
- (BOOL)cmIsForm {
    return std::find(_roles.begin(), _roles.end(),
                     panel::ButtonRole::kAccept) != _roles.end();
}

+ (BOOL)formShownInWindow:(NSWindow*)window {
    for (NSWindow* sheet in window.sheets)
        if ([sheet isKindOfClass:CMFormPanel.class] &&
            ((CMFormPanel*)sheet).presented && [(CMFormPanel*)sheet cmIsForm])
            return YES;
    return NO;
}

- (void)presentInWindow:(NSWindow*)window
             completion:(void (^)(NSModalResponse))completion {
    if (_presented) return;
    _completion = [completion copy];
    [self layoutForm];
    if (window == nil) {
        // No window to hang from: the welcome screen.
        CMFormPanel* keep = self;
        _modal = YES;
        _presented = YES;
        [self center];
        const NSModalResponse response = [NSApp runModalForWindow:self];
        [self orderOut:nil];
        _modal = NO;
        _presented = NO;
        [keep cmFinish:response];
        return;
    }
    NSWindow* attached = window.attachedSheet;
    CMFormPanel* attachedPanel = [attached isKindOfClass:CMFormPanel.class]
                                     ? (CMFormPanel*)attached
                                     : nil;
    // One closing already: AppKit shows the next sheet once it has gone.
    if (attachedPanel != nil && !attachedPanel.presented) attached = nil;
    if (attached != nil && [self cmIsForm]) {
        // The menus stay live under a sheet. A second form asked while one
        // is open is refused, as the menus were greyed under an alert; over
        // anything else -- the export's progress, the save panel -- it shows
        // at once, rather than queued behind a progress that lasts minutes.
        if ([CMFormPanel formShownInWindow:window]) {
            NSBeep();
            [self cmFinish:NSModalResponseCancel];
            return;
        }
        _parent = window;
        _presented = YES;
        [window beginCriticalSheet:self completionHandler:nil];
        return;
    }
    // A progress opened from the save panel's answer waits for the save
    // panel to go: AppKit queues sheets.
    _parent = window;
    _presented = YES;
    [window beginSheet:self completionHandler:nil];
}

- (void)endWithResponse:(NSModalResponse)response {
    if (_modal) {
        [NSApp stopModalWithCode:response];
        // Outside an event (a block on the main queue), a modal loop only
        // sees the stop on its next event.
        [NSApp
            postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                         location:NSZeroPoint
                                    modifierFlags:0
                                        timestamp:0
                                     windowNumber:0
                                          context:nil
                                          subtype:0
                                            data1:0
                                            data2:0]
              atStart:NO];
        return;
    }
    // Closed already: the person pressed « Annuler » meanwhile.
    if (!_presented) return;
    [self cmCloseWithResponse:response];
}

- (void)cmButtonPressed:(NSButton*)sender {
    const NSUInteger index = [_buttonList indexOfObjectIdenticalTo:sender];
    if (index == NSNotFound || index >= _roles.size()) return;
    const panel::ButtonRole role = _roles[index];
    if (!panel::Closes(role)) return;
    const NSModalResponse response =
        NSAlertFirstButtonReturn + static_cast<NSModalResponse>(index);
    if (panel::Validates(role) && self.check) {
        NSString* refusal = self.check(response);
        if (refusal.length > 0) {
            [self showError:refusal];
            return;
        }
    }
    [self cmCloseWithResponse:response];
}

- (void)cmCloseWithResponse:(NSModalResponse)response {
    CMFormPanel* keep = self;
    if (_modal) {
        [NSApp stopModalWithCode:response];
        return;
    }
    if (_presented) {
        _presented = NO;
        NSWindow* parent = self.sheetParent ?: _parent;
        if (parent != nil) [parent endSheet:self returnCode:response];
        [self orderOut:nil];
    }
    [keep cmFinish:response];
}

// The caller's completion, once; the check goes with it, which may hold the
// panel's own controls.
- (void)cmFinish:(NSModalResponse)response {
    void (^completion)(NSModalResponse) = _completion;
    _completion = nil;
    _check = nil;
    if (completion) completion(response);
}

// Return on a text to read, not to type in -- the diagnosis' report: the
// text view takes the key for a line it cannot insert, and the panel's
// default button never heard it (QA pass of 8 October 2026, a Return the
// window test posted as the keyboard posts one). It is the button's.
- (void)sendEvent:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    NSResponder* responder = self.firstResponder;
    if (event.type == NSEventTypeKeyDown && modifiers == 0 &&
        (event.keyCode == 36 || event.keyCode == 76) &&
        [responder isKindOfClass:NSTextView.class] &&
        !((NSTextView*)responder).editable) {
        for (size_t index = 0; index < _roles.size(); ++index) {
            if (_roles[index] != panel::ButtonRole::kAccept) continue;
            [_buttonList[index] performClick:self];
            return;
        }
    }
    [super sendEvent:event];
}

// Escape on a panel whose one closing button answers Return -- the
// diagnosis' « Fermer » -- closes it too (FormPanelLayout.h).
- (BOOL)performKeyEquivalent:(NSEvent*)event {
    const NSEventModifierFlags modifiers =
        event.modifierFlags & NSEventModifierFlagDeviceIndependentFlagsMask;
    if (event.type == NSEventTypeKeyDown && modifiers == 0 &&
        [event.charactersIgnoringModifiers isEqualToString:@"\x1b"]) {
        const int index = panel::EscapeButton(_roles);
        if (index >= 0 && _roles[index] != panel::ButtonRole::kCancel) {
            [_buttonList[index] performClick:self];
            return YES;
        }
    }
    return [super performKeyEquivalent:event];
}

- (void)cancelOperation:(id)sender {
    (void)sender;
    const int index = panel::EscapeButton(_roles);
    if (index >= 0) [_buttonList[index] performClick:self];
}

@end
