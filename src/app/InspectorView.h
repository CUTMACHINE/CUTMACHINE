#pragma once

// AppKit-only. This file (and InspectorView.mm) compiles against a real
// macOS SDK but has not been exercised at runtime -- same caveat as
// UiComponents.h/PanelHostView.h carry for F2.1. See
// VISUAL_QA_CHECKLIST.md for what still needs walking through.
//
// F2.2 -- ROADMAP.md: the Inspector panel's content view, installed into
// PanelSlot::Inspector via CMPanelHostView's -setContentView:forSlot: (see
// main.mm's -applicationDidFinishLaunching). Shows the currently selected
// clip's properties and its color.* grading stack (F1.3, ColorEffects.h)
// as a set of sliders built on UiComponents.h's CMControlRowView -- the
// exact pairing UiComponents.h's own file comment already anticipates
// ("e.g. an Inspector property row showing 'Exposition' / a slider /
// '+0.4'").
//
// This view holds no EditLog of its own: main.mm remains the single owner
// of edit state, matching every other in-place editor already in main.mm.
// Two entry points connect it to that state:
//
//   - -reloadWithDocument:selectedClipId: refreshes every row from a
//     Document snapshot the caller passes in. main.mm calls this from
//     -updateSelectionInfo, the hook already fired after every selection
//     change and every edit (see main.mm's existing call sites).
//   - -delegate is notified with a freshly built SetClipEffectsOperation
//     whenever a grading control commits an edit (drag-end or short
//     debounce, never one operation per pixel of drag -- see
//     InspectorView.mm). CMInspectorViewDelegate matches this codebase's
//     existing delegate-protocol convention (AppDelegate already conforms
//     to NSWindowDelegate/NSSplitViewDelegate) rather than introducing a
//     block-based callback, which nothing else in this project uses. The
//     delegate is responsible for actually calling EditLog::Apply with the
//     operation and for any follow-up (persistEdits, marking the frame
//     dirty for re-render) -- this view never mutates Document/DocumentClip
//     directly (PHILOSOPHY.md principle 2/3).

#import <AppKit/AppKit.h>

#include "Document.h"
#include "Operations.h"
#include "VideoScopes.h"

@class CMInspectorView;

// F2.3 -- ROADMAP.md. A slider that says when a hand holds it: its action
// fires on every tick of a drag with `tracking` on, then once more, with
// `tracking` off, when the hand lets go. A move from the keyboard comes
// with `tracking` off. What reads it writes a drag once, at its release.
@interface CMTrackedSlider : NSSlider
@property(nonatomic) BOOL tracking;
@end

@protocol CMInspectorViewDelegate <NSObject>

// `operation` is ready to submit to EditLog::Apply as-is (clip_id and the
// clip's complete new effects vector already set) -- see the file comment
// above.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipEffects:(SetClipEffectsOperation)operation;

- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipOpacity:(SetClipOpacityOperation)operation;

// EF3 -- docs/roadmap/EFFETS-2026-10.md. The « Flou » row, committed as the
// opacity is.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipBlur:(SetClipBlurOperation)operation;

@optional
// The author, 29 October 2026 -- a clip's zoom, position and fit or fill, one
// operation per released drag; while it moves, the framing it would commit
// is handed over for the monitor to show, nothing applied.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipFraming:(SetClipFramingOperation)operation;
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    previewClipFraming:(const ClipFraming&)framing
               forClip:(const Ulid&)clipId;
// R2c -- docs/roadmap/REEL-2026-09.md. A key posed or removed by a row's
// diamond, or a keyed row's slider let go: a SetClipAnimationOperation, or a
// batch that also writes the last key's value into the field
// (InspectorKeys.h). `label` says which, for the status line.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipKeys:(Operation)operation
                label:(NSString* _Nonnull)label;
// While a keyed row's slider moves, the keys it would commit, for the
// monitor to show; nothing is applied.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    previewClipAnimation:(const std::vector<ClipParameterAnimation>&)animation
                 forClip:(const Ulid&)clipId;
// A row's arrows: the playhead goes to frame `localFrame` of `clipId`, where
// the key before or after it is.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
          moveToFrame:(int64_t)localFrame
               ofClip:(const Ulid&)clipId;

// Visual QA of 19 September 2026 -- the image only changed on release: the
// commit waits for the drag to end, so that one undo gives back the state
// before it. While a grading slider moves, this hands over the effects it
// would commit, for the monitor to show; nothing is applied. The next
// commit or reload supersedes it.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    previewClipEffects:(const std::vector<ClipEffect>&)effects
               forClip:(const Ulid&)clipId;

// D7 -- docs/roadmap/DIALOGUE-2026-10.md. The words of a carton, typed in
// the panel as in the editor the timelines go to. The text is handed over
// as typed: what may be written, and the operation that keeps the carton's
// style, are the delegate's (MediaPanelModel.h, EditCartonText). YES when
// the carton now holds it; NO leaves the document as it was, and the field
// goes back to the document's text.
- (BOOL)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitCartonText:(const std::string&)typed
                forClip:(const Ulid&)clipId;

// The carton before or after `clipId` on its track becomes the selection,
// and the playhead goes to it. NO at either end of the track.
- (BOOL)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    selectCartonAdjacentTo:(const Ulid&)clipId
                   forward:(BOOL)forward;

// While the field is typed in, the text it holds, for the monitor to show
// in place of the carton's own; nothing is applied. An empty `clipId` ends
// it: the text was committed or given up.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    previewCartonText:(const std::string&)typed
              forClip:(const Ulid&)clipId;

// Escape in the field: the keys go back to the timeline.
- (void)inspectorViewDidLeaveCartonText:
    (CMInspectorView* _Nonnull)inspectorView;

// Out of lot -- the author, 18 November 2026 (GitHub #330). The « Son »
// section: the gain or a fade of the clip InspectorSound.h names for the one
// shown -- itself on an audio track, or the sound of a linked picture. One
// complete SetClipAudioOperation per gesture (a typed value, a step, a released
// drag of the gain), checked by InspectorSound.h; the delegate applies it.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitClipAudio:(SetClipAudioOperation)operation;

// F2.3 -- ROADMAP.md. The style the shown carton wears, set in the panel as
// Resolve sets the Track Style of a subtitle track (DaVinci Resolve 21.1
// manual, chapter 59, page 1237): font, size, colour, stroke, drop shadow,
// background and position. `operation` is MediaPanelModel.h's
// RestyleCaptionStyleWhole, one per gesture -- a slider's drag is one, at
// its release. The delegate applies it; every carton of the style follows.
- (void)inspectorView:(CMInspectorView* _Nonnull)inspectorView
    didCommitCaptionStyle:(UpdateCaptionStyleOperation)operation;

@end

@interface CMInspectorView : NSView

@property(nonatomic, weak, nullable) id<CMInspectorViewDelegate> delegate;

// ON4 -- the Colour page presents its grading tools first, with wheels
// and curves side by side when there is room (GradeWorkspaceLayout.h).
@property(nonatomic) BOOL colorWorkspace;

// Refreshes every property row and grading slider from `document`'s
// current state for `clipId`. Pass an empty Ulid (or the id of a clip the
// document no longer has) to show the "no selection" placeholder instead.
- (void)reloadWithDocument:(const Document&)document
            selectedClipId:(const Ulid&)clipId;
// R2c -- where the playhead is on the timeline: the diamonds say whether
// the shown clip has a key on that frame, and a keyed row shows the value
// of that frame. Cheap: it reads the clip the last reload kept.
- (void)showPlayheadAt:(RationalTime)position;

// DEC42 -- only a visible curve editor needs a GPU histogram.
- (BOOL)wantsCurveHistogram;
- (void)showCurveHistogram:(const VideoHistogram&)histogram;

// PRD37 -- ROADMAP.md. The delegate calls this once it has applied, or
// refused, an edit this panel committed: every control shows the clip's
// stack again -- the reset button, the wheels, the curves, the sliders --
// except one whose own commit is still pending, and nothing pending is
// dropped (InspectorGradeControls.h, RefreshAfterOwnEdit).
- (void)refreshAfterOwnEditWithDocument:(const Document&)document;

// D7 -- the field holding the shown carton's text; hidden for any other
// clip. As in the editor the timelines go to, Return breaks the line and
// the text is committed when the field is left. Tab and Shift+Tab commit
// and go to the next and the previous carton, Escape gives up.
@property(nonatomic, readonly, nonnull) NSTextField* captionTextField;

// Puts the keyboard in that field, its text selected. NO when the panel
// does not show `clipId` as a carton it may rewrite.
- (BOOL)focusCaptionTextForClip:(const Ulid&)clipId;

// F2.3 -- the style section's controls, which the window test drives as
// AppKit drives them. Off when the carton wears no style.
@property(nonatomic, readonly, nonnull) NSTextField* captionStyleNote;
@property(nonatomic, readonly, nonnull) NSPopUpButton* captionStyleFontPopup;
@property(nonatomic, readonly, nonnull) NSTextField* captionStyleSizeField;
@property(nonatomic, readonly, nonnull) NSColorWell* captionStyleColorWell;
@property(nonatomic, readonly, nonnull) NSTextField* captionStyleOutlineField;
@property(nonatomic, readonly, nonnull) NSStepper* captionStyleOutlineStepper;
@property(nonatomic, readonly, nonnull) NSColorWell* captionStyleOutlineWell;
@property(nonatomic, readonly, nonnull) NSTextField* captionStyleShadowField;
@property(nonatomic, readonly, nonnull) NSColorWell* captionStyleShadowWell;
@property(nonatomic, readonly, nonnull) NSButton* captionStyleBoxCheck;
@property(nonatomic, readonly, nonnull) NSColorWell* captionStyleBoxWell;
@property(nonatomic, readonly, nonnull) NSTextField* captionStylePaddingField;
@property(nonatomic, readonly, nonnull)
    NSPopUpButton* captionStylePositionPopup;
@property(nonatomic, readonly, nonnull)
    CMTrackedSlider* captionStyleHeightSlider;

// GitHub #330 -- the « Son » section's controls, for the window test.
@property(nonatomic, readonly, nonnull) NSTextField* soundHeader;
@property(nonatomic, readonly, nonnull) CMTrackedSlider* soundGainSlider;
@property(nonatomic, readonly, nonnull) NSTextField* soundGainField;
@property(nonatomic, readonly, nonnull) NSTextField* soundFadeInField;
@property(nonatomic, readonly, nonnull) NSStepper* soundFadeInStepper;
@property(nonatomic, readonly, nonnull) NSTextField* soundFadeOutField;
@property(nonatomic, readonly, nonnull) NSStepper* soundFadeOutStepper;
@property(nonatomic, readonly, nonnull) NSTextField* soundNote;

@end
