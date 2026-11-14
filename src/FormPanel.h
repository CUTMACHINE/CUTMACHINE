#pragma once

// AppKit-only. PRD26 -- docs/roadmap/PRODUIT-2026-10b.md.
//
// The window's forms used to live in the accessory view of an NSAlert: a
// warning icon over a centred title, the whole application blocked, no
// resizing, a « Enregistrer » that accepted a wrong form and a refusal that
// came once the alert had closed, in a second alert. CMFormPanel is the
// panel they share instead: a sheet on the window -- or, with no window to
// attach to (the welcome screen), a panel modal to the application -- that
// holds a title, an optional explanation, the caller's form as it was, an
// error line under it and its buttons at the bottom. Return presses the
// accepting button, Escape the cancelling one.
//
// The caller's check runs before the panel closes: while it refuses, the
// panel stays open and says why on its error line. FormPanelLayout.h
// decides the frames, the keys and the colours; this only measures and
// applies them (frames and autoresizing masks, as everywhere in the app).

#import <AppKit/AppKit.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(NSInteger, CMFormButtonRole) {
    // Return; the check runs, then the panel closes.
    CMFormButtonRoleAccept = 0,
    // Escape; closes, nothing checked.
    CMFormButtonRoleCancel,
    // Closes with its own answer, after the check.
    CMFormButtonRoleAlternate,
    // The caller's target and action; the panel stays open.
    CMFormButtonRoleAction,
};

// The check run before the panel closes on `response`, the answer of an
// accepting or an alternate button: nil to close, or what the error line
// says. It may also do the work that can be refused -- store a key, apply
// an operation -- the panel staying open on what it says.
typedef NSString* _Nullable (^CMFormPanelCheck)(NSModalResponse response);

@interface CMFormPanel : NSPanel

// `form` is the caller's view, taken as it is: its size is the form's.
- (instancetype)initWithTitle:(NSString*)title
                      message:(nullable NSString*)message
                         form:(NSView*)form;

@property(nonatomic, readonly) NSView* form;
@property(nonatomic, readonly) NSTextField* titleLabel;
@property(nonatomic, readonly) NSTextField* messageLabel;
@property(nonatomic, readonly) NSTextField* errorLabel;
@property(nonatomic, readonly) NSImageView* errorIcon;
@property(nonatomic, readonly) NSArray<NSButton*>* buttons;
@property(nonatomic, copy, nullable) CMFormPanelCheck check;
// A form that can use more room (a list, a long text): the person can
// resize the panel, never under its first size, and the form takes the
// room.
@property(nonatomic) BOOL formResizable;
// Whether it is on screen, as a sheet or modal.
@property(nonatomic, readonly) BOOL presented;

// A button at the bottom. Its answer is NSAlertFirstButtonReturn plus its
// index, as an alert's: the callers keep their tests. An action button's
// target and action are the caller's to set.
- (NSButton*)addButtonWithTitle:(NSString*)title role:(CMFormButtonRole)role;

// The explanation, changed while the panel is shown (a progress count).
- (void)setMessage:(nullable NSString*)message;
// What the error line says; nil or empty clears it. The panel grows when
// it needs more than one line.
- (void)showError:(nullable NSString*)error;

// A sheet on `window`; with no window, a panel modal to the application,
// centred, run before this returns. `completion` gets the answer once the
// panel has closed. The menus stay live under a sheet: a form asked while
// another form is open is refused with a beep (its completion gets
// NSModalResponseCancel); over another sheet -- the export's progress -- a
// form shows at once, on top, and a progress waits its turn.
- (void)presentInWindow:(nullable NSWindow*)window
             completion:(nullable void (^)(NSModalResponse response))completion;

// Closes it as `response`, without a button: a progress panel whose work
// is done. The completion runs.
- (void)endWithResponse:(NSModalResponse)response;

// Whether `window` shows a form panel now, which would refuse another form:
// a caller whose building has effects -- it keeps the fields it builds --
// asks first.
+ (BOOL)formShownInWindow:(nullable NSWindow*)window;

// Lays the panel out for what it holds now. Every change above does it;
// the window test calls it on a panel it builds without showing.
- (void)layoutForm;

@end

NS_ASSUME_NONNULL_END
