#import "InspectorView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "EffectsLibrary.h"
#include "GradeCurveModel.h"
#include "GradeWheelModel.h"
#include "GradeWorkspaceLayout.h"
#include "InspectorGradeControls.h"
#include "InspectorKeys.h"
#include "InspectorSound.h"
#include "MediaPanelModel.h"
#include "TimelineView.h"
#include "UiPreferences.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"
#include "Ulid.h"

#import "GradeCurveView.h"
#import "GradeHueCurveView.h"
#import "GradeWheelView.h"
#import "UiComponents.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

// Section headers (CMMakeSectionHeader) get a slightly taller slot than a
// CMControlRowView row so the caption-sized text has breathing room above
// and below it.
constexpr CGFloat kSectionHeaderHeight = 20.0;

// A grading slider commits at most this often after the user stops
// dragging -- "reasonable UX: don't fire one operation per pixel of slider
// drag" (F2.2 scope). Every intermediate drag position still updates the
// row's value label immediately, so the number the user sees is never
// stale; only the EditLog::Apply call (and the undo entry it creates) is
// debounced.
constexpr NSTimeInterval kGradeCommitDebounceSeconds = 0.12;

// D7 -- four lines of body text: a carton holds two in practice, and a
// long one read from an SRT still shows whole.
constexpr CGFloat kCaptionFieldHeight = 66.0;

// GR1 -- two lines of caption text: the note under the reset button when
// the clip's stack is not in the order the panel adds stages in.
constexpr CGFloat kOrderNoteHeight = 28.0;

// Strips a filesystem path down to its final component for a compact "Nom"
// row. This app targets macOS only, so '/' is the only separator that
// matters.
NSString* CMBasename(const std::string& path) {
    const size_t slash = path.find_last_of('/');
    const std::string base =
        slash == std::string::npos ? path : path.substr(slash + 1);
    return base.empty() ? @"(sans nom)"
                        : [NSString stringWithUTF8String:base.c_str()];
}

// A small local timecode formatter. This duplicates the handful of lines
// main.mm's own file-local (anonymous-namespace) TimelineTimecode already
// computes -- main.mm does not export it, and F2.2's instructions ask for
// as small a main.mm diff as possible, so a few duplicated lines here beat
// widening main.mm's surface just to share one formatter with a second
// translation unit.
NSString* CMInspectorTimecodeString(const RationalTime& time, MediaRate rate) {
    if (rate.num <= 0 || rate.den <= 0) return @"00:00:00:00";
    const int64_t absoluteFrames =
        std::max<int64_t>(0, time.to_frames(rate.num, rate.den));
    const int64_t nominalFps =
        std::max<int64_t>(1, (rate.num + rate.den - 1) / rate.den);
    const int64_t frames = absoluteFrames % nominalFps;
    const int64_t totalSeconds = absoluteFrames / nominalFps;
    return [NSString
        stringWithFormat:@"%02lld:%02lld:%02lld:%02lld", totalSeconds / 3600,
                         (totalSeconds / 60) % 60, totalSeconds % 60, frames];
}

// Temperature reads as whole Kelvin (matching its GradeControlSpec's
// precision_den == 1); every other knob is a signed +/- amount.
NSString* CMFormatGradeValue(const ui::inspector::GradeControlSpec& spec,
                             float value) {
    if (std::string(spec.type) == "color.temperature")
        return [NSString stringWithFormat:@"%.0f K", value];
    // GR6 -- the hue rotation reads in degrees, the pivot as the value it
    // is: neither is a signed amount about zero.
    if (std::string(spec.type) == "color.hue")
        return [NSString stringWithFormat:@"%+.0f°", value];
    if (std::string(spec.type) == "color.contrast" && spec.param != nullptr)
        return [NSString stringWithFormat:@"%.3f", value];
    return [NSString stringWithFormat:@"%+.2f", value];
}

}  // namespace

// F1.3 -- wheels and curves took the panel from 8 grading sliders to 32,
// more than the dock's height holds, so the rows now sit in a scroll view.
// Flipped, so the frame layout below runs from the top down and the
// panel opens scrolled to its first row.
@interface CMInspectorBodyView : NSView
@end

@implementation CMInspectorBodyView
- (BOOL)isFlipped {
    return YES;
}
@end

@interface CMInspectorView () <NSTextFieldDelegate, CMGradeEditingHost>
- (CMControlRowView*)cmAddPropertyRowWithLabel:(NSString*)labelText
                                    valueField:
                                        (NSTextField* __strong*)outValueField;
- (CMControlRowView*)cmAddGradeRowForSpec:
                         (const ui::inspector::GradeControlSpec&)spec
                                    index:(NSInteger)index
                                   slider:(NSSlider* __strong*)outSlider
                               valueLabel:(NSTextField* __strong*)outValueLabel;
- (void)cmLayoutForSize:(NSSize)size;
- (void)cmPlacePropertyValues;
- (void)cmGradeSliderChanged:(NSSlider*)sender;
- (void)cmOpacitySliderChanged:(NSSlider*)sender;
- (void)cmCommitOpacity:(float)value;
- (void)cmBlurSliderChanged:(NSSlider*)sender;
- (void)cmCommitBlur:(float)value;
- (void)cmShowBlur;
- (void)cmScheduleGradeCommitAtIndex:(NSInteger)index value:(float)value;
- (void)cmCommitGradeAtIndex:(NSInteger)index value:(float)value;
- (void)cmInvalidateAllGradeTimers;
- (void)cmShowOpacity;
- (void)cmShowFraming;
- (void)cmShowFramingForClip:(const DocumentClip&)clip;
- (NSString*)cmFramingText:(ui::inspector::FramingControl)control
                     value:(float)value;
- (void)cmFramingSliderChanged:(CMTrackedSlider*)sender;
- (void)cmResetFramingSlider:(NSSlider*)sender;
- (void)cmFramingModeChanged:(NSPopUpButton*)sender;
- (void)cmCommitFraming:(const ClipFraming&)framing;
- (void)cmPlaceKeyButtons;
- (void)cmShowKeyedRows;
- (void)cmCommitKeyedIndex:(NSUInteger)index slider:(float)value;
- (void)cmToggleKey:(NSButton*)sender;
- (void)cmPreviousKey:(NSButton*)sender;
- (void)cmNextKey:(NSButton*)sender;
- (void)cmShowGradeControlAtIndex:(size_t)index;
- (void)cmToggleWheelSliders:(id)sender;
- (void)cmToggleCurveSliders:(id)sender;
- (void)cmShowCaptionSection:(BOOL)show;
- (void)cmEndCaptionPreview;
- (BOOL)cmCommitCaptionText;
- (BOOL)cmGoToAdjacentCarton:(BOOL)forward;
- (void)cmReloadCaptionStyleFrom:(const Document&)document
                            clip:(const DocumentClip&)clip;
- (void)cmShowCaptionStyle;
- (void)cmPlaceStyleControls;
- (void)cmCaptionStyleChanged:(id)sender;
- (void)cmReloadSoundFrom:(const Document&)document;
- (void)cmShowSound;
- (void)cmPlaceSoundControls;
- (void)cmGainSliderChanged:(CMTrackedSlider*)sender;
- (void)cmResetGain:(id)sender;
- (void)cmGainTyped:(NSTextField*)sender;
- (void)cmFadeTyped:(NSTextField*)sender;
- (void)cmFadeStepped:(NSStepper*)sender;
- (void)cmCommitSound:(const SetClipAudioOperation&)operation;
- (void)cmRefuseSound:(const std::string&)refusal;
@end

// Visual QA of 19 September 2026 -- a double click brings a grade slider
// back to its neutral value, as in the colour tools the app is used next to.
@interface CMResettableSlider : NSSlider
@property(nonatomic) SEL resetAction;
@end

@implementation CMTrackedSlider
// NSSlider's own tracking loop runs inside -mouseDown: and returns at the
// release: every action it sends meanwhile is a tick.
- (void)mouseDown:(NSEvent*)event {
    self.tracking = YES;
    [super mouseDown:event];
    self.tracking = NO;
    [self sendAction:self.action to:self.target];
}
@end

// The author, 29 October 2026 -- a framing slider says its release, so the
// edit lands once per drag (the monitor previews the ticks), and a double
// click brings it back to zoom 1, centred.
@interface CMFramingSlider : CMTrackedSlider
@property(nonatomic) SEL resetAction;
@end

@implementation CMFramingSlider
- (void)mouseDown:(NSEvent*)event {
    if (event.clickCount == 2 && self.enabled && self.resetAction) {
        [NSApp sendAction:self.resetAction to:self.target from:self];
        return;
    }
    [super mouseDown:event];
}
@end

@implementation CMResettableSlider
- (void)mouseDown:(NSEvent*)event {
    if (event.clickCount == 2 && self.enabled && self.resetAction) {
        [NSApp sendAction:self.resetAction to:self.target from:self];
        return;
    }
    [super mouseDown:event];
}
@end

@implementation CMInspectorView {
    Ulid _clipId;
    // G7 -- the shown clip's as-shot white balance: where the temperature
    // slider sits without a stage, and where a reset brings it.
    int32_t _neutralKelvin;
    // The selected clip's effects as of the last reload/commit. Every
    // grading commit is built from this snapshot via
    // ui::inspector::WithGradeControlValue (InspectorGradeControls.h),
    // never a direct mutation -- see the file comment in InspectorView.h.
    std::vector<ClipEffect> _effects;
    EffectParamValue _opacity;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. The clip's blur, in sequence
    // pixels, as of the last reload or commit.
    EffectParamValue _blur;

    NSTextField* _placeholderLabel;
    // Everything else, scrolled by _scrollView; hidden while
    // _placeholderLabel shows.
    NSScrollView* _scrollView;
    CMInspectorBodyView* _body;

    // D7 -- docs/roadmap/DIALOGUE-2026-10.md. Shown for a carton only, at
    // the top: its words are what one comes to this panel for.
    NSTextField* _captionHeader;
    NSTextField* _captionField;
    NSView* _captionNav;
    NSButton* _captionPrevious;
    NSButton* _captionNext;
    BOOL _isCarton;
    // The carton's text as the document held it at the last reload: what
    // the field goes back to, and how a reload tells a text that changed
    // under the field from one being typed.
    std::string _captionLoadedText;
    std::vector<std::pair<NSView*, CGFloat>> _captionItems;

    // F2.3 -- ROADMAP.md. The style the carton wears, under its words, as
    // Resolve puts the Track Style under a subtitle's caption: font, size
    // and colour, then Stroke, Drop Shadow, Background and Position. A
    // number is typed in the value column or stepped beside it; a colour
    // well follows; the height is a slider, written once at its release.
    NSTextField* _styleHeader;
    NSTextField* _styleNote;
    CMControlRowView* _styleFontRow;
    NSPopUpButton* _styleFontPopup;
    CMControlRowView* _styleSizeRow;
    NSTextField* _styleSizeField;
    NSStepper* _styleSizeStepper;
    CMControlRowView* _styleColorRow;
    NSColorWell* _styleColorWell;
    CMControlRowView* _styleOutlineRow;
    NSTextField* _styleOutlineField;
    NSStepper* _styleOutlineStepper;
    NSColorWell* _styleOutlineWell;
    CMControlRowView* _styleShadowRow;
    NSTextField* _styleShadowField;
    NSStepper* _styleShadowStepper;
    NSColorWell* _styleShadowWell;
    CMControlRowView* _styleBoxRow;
    NSButton* _styleBoxCheck;
    NSColorWell* _styleBoxWell;
    CMControlRowView* _stylePaddingRow;
    NSTextField* _stylePaddingField;
    NSStepper* _stylePaddingStepper;
    CMControlRowView* _stylePositionRow;
    NSPopUpButton* _stylePositionPopup;
    CMControlRowView* _styleHeightRow;
    CMTrackedSlider* _styleHeightSlider;
    // The style as the document held it at the last reload: what every
    // gesture starts from, and what the controls go back to on a refusal.
    std::optional<CaptionStyle> _style;
    std::vector<std::pair<NSView*, CGFloat>> _styleItems;

    // Out of lot -- the author, 18 November 2026 (GitHub #330). The « Son »
    // section: the gain, a slider and its typed value, then the two fades,
    // typed or stepped a frame at a time, of the clip InspectorSound.h
    // names for the one shown. It takes no room for a clip without sound.
    NSTextField* _soundHeader;
    CMControlRowView* _gainRow;
    CMTrackedSlider* _gainSlider;
    NSTextField* _gainField;
    CMControlRowView* _fadeInRow;
    NSTextField* _fadeInField;
    NSStepper* _fadeInStepper;
    CMControlRowView* _fadeOutRow;
    NSTextField* _fadeOutField;
    NSStepper* _fadeOutStepper;
    // A refusal, in words, on its own line; no room when there is none.
    NSTextField* _soundNote;
    NSView* _soundSpacer;
    std::optional<ui::inspector::SoundTarget> _soundTarget;
    // The target as the document held it at the last reload: what every
    // gesture starts from, and what the controls go back to on a refusal.
    DocumentClip _soundClip;
    MediaRate _soundRate;
    // What each field -- gain, fade in, fade out -- was last given.
    std::array<std::string, 3> _soundShown;

    CMControlRowView* _nameRow;
    CMControlRowView* _trackRow;
    CMControlRowView* _startRow;
    CMControlRowView* _durationRow;
    // DEC42 -- the dips through a colour touching the clip (case 123), and
    // the sequence's look (case 134).
    CMControlRowView* _dipRow;
    CMControlRowView* _lookRow;
    NSTextField* _nameValueField;
    NSTextField* _trackValueField;
    NSTextField* _startValueField;
    NSTextField* _durationValueField;
    NSTextField* _dipValueField;
    NSTextField* _lookValueField;
    CMControlRowView* _opacityRow;
    NSSlider* _opacitySlider;
    NSTextField* _opacityValueLabel;
    NSTimer* _opacityDebounceTimer;
    // The author, 29 October 2026 -- zoom and position (ClipFraming), one
    // slider per FramingControl in its order, and fit or fill.
    ClipFraming _framing;
    BOOL _framable;
    int32_t _sequenceWidth;
    int32_t _sequenceHeight;
    CMControlRowView* _framingModeRow;
    NSPopUpButton* _framingModePopup;
    NSMutableArray<CMControlRowView*>* _framingRows;
    NSMutableArray<CMFramingSlider*>* _framingSliders;
    // R2c -- docs/roadmap/REEL-2026-09.md. The keyed rows, the opacity then
    // the three framing sliders, in KeyedControl's order (InspectorKeys.h):
    // a diamond on each, and arrows while the parameter has keys. What a
    // diamond reads: the clip the last reload kept, its cadence and the
    // playhead.
    NSMutableArray<NSButton*>* _keyButtons;
    NSMutableArray<NSButton*>* _previousKeyButtons;
    NSMutableArray<NSButton*>* _nextKeyButtons;
    std::array<BOOL, 4> _keyedAnimated;
    DocumentClip _keyedClip;
    BOOL _hasKeyedClip;
    BOOL _opacityEditable;
    MediaRate _rate;
    RationalTime _playhead;
    // The clip frame the rows last showed: playback calls -showPlayheadAt:
    // on every frame, and a frame already shown is not read again.
    std::optional<int64_t> _keyedFrameShown;
    // EF3 -- the « Flou » row, committed like the opacity: once the hand
    // rests, one SetClipBlurOperation.
    CMControlRowView* _blurRow;
    NSSlider* _blurSlider;
    NSTextField* _blurValueLabel;
    NSTimer* _blurDebounceTimer;

    NSButton* _resetGradeButton;
    BOOL _gradable;
    NSMutableArray<NSSlider*>* _gradeSliders;
    NSMutableArray<NSTextField*>* _gradeValueLabels;
    // One slot per GradeControls() entry: an NSTimer while a commit is
    // pending, NSNull otherwise (NSMutableArray cannot hold nil).
    NSMutableArray* _gradeDebounceTimers;

    // GR4, GR5 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The wheels and
    // the curve editor, each at the head of its section. The sliders of
    // those two sections -- one per knob, the bars of the application the
    // timelines go to -- stay, folded away until asked for.
    CMGradeWheelGridView* _wheelGrid;
    CMGradeCurveView* _curveView;
    // GR8 -- the hue curves, a section of their own after the curves': they
    // have an editor and no slider.
    CMGradeHueCurveView* _hueCurveView;
    NSButton* _wheelSlidersToggle;
    NSButton* _curveSlidersToggle;
    NSMutableArray<NSView*>* _wheelSliderRows;
    NSMutableArray<NSView*>* _curveSliderRows;
    // GR1 -- shown while the clip's stack is not in the canonical order.
    NSTextField* _orderNote;
    // The rows of _layoutItems that take no room at the moment.
    NSMutableSet<NSView*>* _collapsed;

    // Top-to-bottom layout order: (view, slot height). A nil view is a
    // plain spacer (the gap between the properties and grading sections).
    // Built once in -initWithFrame:, replayed by
    // -cmLayoutForSize: on every resize -- same frame-based approach as
    // CMControlRowView/CMPanelChromeView (UiComponents.mm), no Auto
    // Layout anywhere in this project.
    std::vector<std::pair<NSView*, CGFloat>> _layoutItems;
    // ON4 -- views shared with the normal Inspector, regrouped on Colour.
    std::vector<std::pair<NSView*, CGFloat>> _gradeItems;
    size_t _gradeStart;
    size_t _wheelGradeStart;
    size_t _curveGradeStart;
    BOOL _colorWorkspace;
}

@synthesize captionTextField = _captionField;
@synthesize captionStyleNote = _styleNote;
@synthesize captionStyleFontPopup = _styleFontPopup;
@synthesize captionStyleSizeField = _styleSizeField;
@synthesize captionStyleColorWell = _styleColorWell;
@synthesize captionStyleOutlineField = _styleOutlineField;
@synthesize captionStyleOutlineStepper = _styleOutlineStepper;
@synthesize captionStyleOutlineWell = _styleOutlineWell;
@synthesize captionStyleShadowField = _styleShadowField;
@synthesize captionStyleShadowWell = _styleShadowWell;
@synthesize captionStyleBoxCheck = _styleBoxCheck;
@synthesize captionStyleBoxWell = _styleBoxWell;
@synthesize captionStylePaddingField = _stylePaddingField;
@synthesize captionStylePositionPopup = _stylePositionPopup;
@synthesize captionStyleHeightSlider = _styleHeightSlider;
@synthesize soundHeader = _soundHeader;
@synthesize soundGainSlider = _gainSlider;
@synthesize soundGainField = _gainField;
@synthesize soundFadeInField = _fadeInField;
@synthesize soundFadeInStepper = _fadeInStepper;
@synthesize soundFadeOutField = _fadeOutField;
@synthesize soundFadeOutStepper = _fadeOutStepper;
@synthesize soundNote = _soundNote;

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _clipId = Ulid{};
        _opacity = {1, 1};
        _blur = {0, 1};
        _neutralKelvin = kNeutralKelvin;

        _placeholderLabel =
            [NSTextField labelWithString:@"Aucun clip sélectionné"];
        _placeholderLabel.font =
            CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
        _placeholderLabel.textColor = CMThemeColor(ui::theme::kTextTertiary);
        _placeholderLabel.alignment = NSTextAlignmentCenter;
        _placeholderLabel.lineBreakMode = NSLineBreakByWordWrapping;
        [self addSubview:_placeholderLabel];

        _body = [[CMInspectorBodyView alloc] initWithFrame:NSZeroRect];
        _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scrollView.hasVerticalScroller = YES;
        _scrollView.hasHorizontalScroller = NO;
        _scrollView.autohidesScrollers = YES;
        _scrollView.borderType = NSNoBorder;
        // The panel chrome draws the background, as before the scroll view.
        _scrollView.drawsBackground = NO;
        _scrollView.documentView = _body;
        [self addSubview:_scrollView];

        _captionHeader = CMMakeSectionHeader(@"Sous-titre");
        [_body addSubview:_captionHeader];
        _captionField = [NSTextField textFieldWithString:@""];
        _captionField.font =
            CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        _captionField.textColor = CMTextPrimaryColor();
        _captionField.bezeled = YES;
        _captionField.bezelStyle = NSTextFieldSquareBezel;
        _captionField.usesSingleLineMode = NO;
        _captionField.cell.wraps = YES;
        _captionField.cell.scrollable = NO;
        _captionField.target = self;
        _captionField.action = @selector(cmCaptionTextCommitted:);
        // Leaving the field commits.
        _captionField.cell.sendsActionOnEndEditing = YES;
        _captionField.delegate = self;
        _captionField.toolTip =
            @"Entrée passe à la ligne · ⌘ Entrée enregistre · Tab : carton "
            @"suivant · Échap annule";
        [_body addSubview:_captionField];
        _captionNav = [[NSView alloc] initWithFrame:NSZeroRect];
        _captionPrevious = CMMakeStyledButton(@"◀ Précédent", self,
                                              @selector(cmPreviousCarton:));
        _captionNext =
            CMMakeStyledButton(@"Suivant ▶", self, @selector(cmNextCarton:));
        [_captionNav addSubview:_captionPrevious];
        [_captionNav addSubview:_captionNext];
        [_body addSubview:_captionNav];
        _captionItems.push_back({_captionHeader, kSectionHeaderHeight});
        _captionItems.push_back({_captionField, kCaptionFieldHeight});
        _captionItems.push_back(
            {_captionNav, CGF(ui::theme::kControlRowHeight)});
        _captionItems.push_back({nil, CGF(ui::theme::kSpaceM)});  // spacer
        for (const auto& item : _captionItems) item.first.hidden = YES;

        // F2.3 -- the carton's style. Bounds are the engine's
        // (Document.h) and the tab's for the size (MediaPanelModel.h).
        _styleHeader = CMMakeSectionHeader(@"Style de légende");
        [_body addSubview:_styleHeader];
        _styleNote = [NSTextField wrappingLabelWithString:@""];
        _styleNote.font =
            CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _styleNote.textColor = CMThemeColor(ui::theme::kTextTertiary);
        [_body addSubview:_styleNote];
        const auto row = [&](NSString* title) {
            CMControlRowView* made =
                [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                              labelText:title];
            [_body addSubview:made];
            return made;
        };
        // No bounds on the field itself: a number past them is typed,
        // then refused by the policy in words (MediaPanelModel.h), where
        // a formatter's would only beep.
        const auto number = [&](CMControlRowView* owner, NSString* help) {
            NSTextField* field = [NSTextField textFieldWithString:@""];
            field.font = [NSFont
                monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
                                         weight:NSFontWeightRegular];
            field.alignment = NSTextAlignmentRight;
            field.controlSize = NSControlSizeSmall;
            NSNumberFormatter* format = [[NSNumberFormatter alloc] init];
            format.numberStyle = NSNumberFormatterNoStyle;
            format.allowsFloats = NO;
            field.formatter = format;
            field.target = self;
            field.action = @selector(cmCaptionStyleChanged:);
            field.toolTip = help;
            field.accessibilityLabel = owner.label.stringValue;
            // The value column holds the number (-cmPlaceStyleControls).
            owner.valueLabel.hidden = YES;
            [owner addSubview:field];
            return field;
        };
        const auto stepper = [&](CMControlRowView* owner, int32_t minimum,
                                 int32_t maximum, double increment) {
            NSStepper* made = [[NSStepper alloc] initWithFrame:NSZeroRect];
            made.controlSize = NSControlSizeSmall;
            made.minValue = minimum;
            made.maxValue = maximum;
            made.increment = increment;
            made.valueWraps = NO;
            // One click, one edit: held down, it would write one per step.
            made.autorepeat = NO;
            made.target = self;
            made.action = @selector(cmCaptionStyleChanged:);
            made.accessibilityLabel = owner.label.stringValue;
            [owner.controlContainer addSubview:made];
            return made;
        };
        const auto well = [&](CMControlRowView* owner, NSString* help) {
            NSColorWell* made = [[NSColorWell alloc] initWithFrame:NSZeroRect];
            // One edit when the colour is chosen, not one per step of the
            // panel's drag.
            made.continuous = NO;
            made.target = self;
            made.action = @selector(cmCaptionStyleChanged:);
            made.toolTip = help;
            made.accessibilityLabel = help;
            [owner.controlContainer addSubview:made];
            return made;
        };
        const auto popup = [&](CMControlRowView* owner) {
            NSPopUpButton* made =
                [[NSPopUpButton alloc] initWithFrame:NSZeroRect pullsDown:NO];
            made.controlSize = NSControlSizeSmall;
            made.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
            made.target = self;
            made.action = @selector(cmCaptionStyleChanged:);
            made.accessibilityLabel = owner.label.stringValue;
            owner.valueLabel.hidden = YES;
            [owner addSubview:made];
            return made;
        };
        _styleFontRow = row(@"Police");
        _styleFontPopup = popup(_styleFontRow);
        CMFillCaptionFontPopup(_styleFontPopup);
        _styleSizeRow = row(@"Taille");
        _styleSizeField = number(_styleSizeRow, @"Taille en pixels à la "
                                                @"résolution de la séquence");
        _styleSizeStepper =
            stepper(_styleSizeRow, ui::media_panel::kCaptionTabMinimumFontSize,
                    ui::media_panel::kCaptionTabMaximumFontSize, 2.0);
        _styleColorRow = row(@"Couleur");
        _styleColorWell = well(_styleColorRow, @"Couleur du texte");
        _styleOutlineRow = row(@"Contour");
        _styleOutlineField = number(
            _styleOutlineRow, @"Épaisseur du contour en pixels ; 0 : aucun");
        _styleOutlineStepper =
            stepper(_styleOutlineRow, 0, kCaptionStyleMaxOutlineWidth, 1.0);
        _styleOutlineWell = well(_styleOutlineRow, @"Couleur du contour");
        _styleShadowRow = row(@"Ombre portée");
        _styleShadowField = number(
            _styleShadowRow, @"Décalage de l’ombre en pixels ; 0 : aucune");
        _styleShadowStepper =
            stepper(_styleShadowRow, 0, kCaptionStyleMaxShadowOffset, 1.0);
        _styleShadowWell = well(_styleShadowRow, @"Couleur de l’ombre");
        _styleBoxRow = row(@"Fond");
        // The box alone, its row saying what: the control column is some
        // eighty points wide, where the word beside it pushed the well out.
        _styleBoxCheck =
            [NSButton checkboxWithTitle:@""
                                 target:self
                                 action:@selector(cmCaptionStyleChanged:)];
        _styleBoxCheck.controlSize = NSControlSizeSmall;
        _styleBoxCheck.toolTip = @"Une boîte derrière le texte";
        _styleBoxCheck.accessibilityLabel =
            @"Fond : une boîte derrière le texte";
        [_styleBoxRow.controlContainer addSubview:_styleBoxCheck];
        _styleBoxWell =
            well(_styleBoxRow, @"Couleur du fond ; son opacité est gardée");
        _stylePaddingRow = row(@"Marge du fond");
        _stylePaddingField = number(
            _stylePaddingRow, @"Marge de la boîte autour du texte, en pixels");
        _stylePaddingStepper = stepper(_stylePaddingRow, 0,
                                       kCaptionStyleMaxBackgroundPadding, 2.0);
        _stylePositionRow = row(@"Position");
        _stylePositionPopup = popup(_stylePositionRow);
        // In CaptionPlacement's order.
        [_stylePositionPopup addItemsWithTitles:@[
            @"En bas", @"Au centre", @"En haut", @"Hauteur choisie"
        ]];
        _styleHeightRow = row(@"Hauteur");
        // CMMakeStyledSlider's styling, on the subclass that says a drag.
        _styleHeightSlider = [[CMTrackedSlider alloc] initWithFrame:NSZeroRect];
        _styleHeightSlider.minValue = ui::media_panel::kCaptionHeightPercentMin;
        _styleHeightSlider.maxValue = ui::media_panel::kCaptionHeightPercentMax;
        _styleHeightSlider.target = self;
        _styleHeightSlider.action = @selector(cmCaptionStyleChanged:);
        _styleHeightSlider.controlSize = NSControlSizeSmall;
        _styleHeightSlider.trackFillColor = CMThemeColor(ui::theme::kAccent);
        // Every tick moves the number; the edit waits for the release.
        _styleHeightSlider.continuous = YES;
        _styleHeightSlider.accessibilityLabel =
            _styleHeightRow.label.stringValue;
        _styleHeightSlider.toolTip =
            @"Centre des cartons, en pourcentage de la hauteur de l’image";
        [_styleHeightRow.controlContainer addSubview:_styleHeightSlider];
        const CGFloat rowHeight = CGF(ui::theme::kControlRowHeight);
        _styleItems = {{_styleHeader, kSectionHeaderHeight},
                       {_styleNote, kOrderNoteHeight},
                       {_styleFontRow, rowHeight},
                       {_styleSizeRow, rowHeight},
                       {_styleColorRow, rowHeight},
                       {_styleOutlineRow, rowHeight},
                       {_styleShadowRow, rowHeight},
                       {_styleBoxRow, rowHeight},
                       {_stylePaddingRow, rowHeight},
                       {_stylePositionRow, rowHeight},
                       {_styleHeightRow, rowHeight},
                       {nil, CGF(ui::theme::kSpaceM)}};
        for (const auto& item : _styleItems) item.first.hidden = YES;

        NSTextField* propertiesHeader = CMMakeSectionHeader(@"Propriétés");
        [_body addSubview:propertiesHeader];
        _nameRow = [self cmAddPropertyRowWithLabel:@"Nom"
                                        valueField:&_nameValueField];
        _trackRow = [self cmAddPropertyRowWithLabel:@"Piste"
                                         valueField:&_trackValueField];
        _startRow = [self cmAddPropertyRowWithLabel:@"Début"
                                         valueField:&_startValueField];
        _durationRow = [self cmAddPropertyRowWithLabel:@"Durée"
                                            valueField:&_durationValueField];
        _dipRow = [self cmAddPropertyRowWithLabel:@"Fondu"
                                       valueField:&_dipValueField];
        _lookRow = [self cmAddPropertyRowWithLabel:@"Look"
                                        valueField:&_lookValueField];

        // GitHub #330 -- the « Son » section. A field in the value column
        // (F2.3's number rows), a slider or a stepper in the control column.
        _soundHeader = CMMakeSectionHeader(@"Son");
        [_body addSubview:_soundHeader];
        const auto soundField = [&](CMControlRowView* owner, SEL action,
                                    NSString* help) {
            NSTextField* field = [NSTextField textFieldWithString:@""];
            field.font = [NSFont
                monospacedDigitSystemFontOfSize:CGF(ui::theme::kFontSizeBody)
                                         weight:NSFontWeightRegular];
            field.alignment = NSTextAlignmentRight;
            field.controlSize = NSControlSizeSmall;
            field.target = self;
            field.action = action;
            // Leaving the field commits, as Return does; a value left as
            // it was writes nothing (-cmCommitSound:).
            field.cell.sendsActionOnEndEditing = YES;
            field.toolTip = help;
            field.accessibilityLabel = owner.label.stringValue;
            owner.valueLabel.hidden = YES;
            [owner addSubview:field];
            return field;
        };
        const auto soundStepper = [&](CMControlRowView* owner) {
            NSStepper* made = [[NSStepper alloc] initWithFrame:NSZeroRect];
            made.controlSize = NSControlSizeSmall;
            made.minValue = 0;
            made.increment = 1;
            made.valueWraps = NO;
            // One click, one edit, as the carton style's steppers.
            made.autorepeat = NO;
            made.target = self;
            made.action = @selector(cmFadeStepped:);
            made.toolTip = @"Une image de plus ou de moins";
            made.accessibilityLabel = owner.label.stringValue;
            [owner.controlContainer addSubview:made];
            return made;
        };
        _gainRow = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                 labelText:@"Gain"];
        // The framing sliders' class: written once at the release, and a
        // double click brings the clip back to its own level.
        CMFramingSlider* gainSlider =
            [[CMFramingSlider alloc] initWithFrame:NSZeroRect];
        gainSlider.resetAction = @selector(cmResetGain:);
        _gainSlider = gainSlider;
        _gainSlider.minValue = ui::inspector::kInspectorGainSliderMin;
        _gainSlider.maxValue = ui::inspector::kInspectorGainSliderMax;
        _gainSlider.target = self;
        _gainSlider.action = @selector(cmGainSliderChanged:);
        _gainSlider.controlSize = NSControlSizeSmall;
        _gainSlider.trackFillColor = CMThemeColor(ui::theme::kAccent);
        _gainSlider.continuous = YES;
        _gainSlider.toolTip = @"De -60 à +12 dB · double-clic : 0 dB";
        _gainSlider.accessibilityLabel = _gainRow.label.stringValue;
        _gainSlider.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _gainSlider.frame = _gainRow.controlContainer.bounds;
        [_gainRow.controlContainer addSubview:_gainSlider];
        _gainField = soundField(_gainRow, @selector(cmGainTyped:),
                                @"Gain du plan en dB, de -120 à +48");
        [_body addSubview:_gainRow];
        NSString* const fadeHelp =
            @"En images (12), en secondes:images (1:05) ou en secondes "
            @"(0,5 s) ; 0 : aucun";
        _fadeInRow = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                   labelText:@"Fondu d’entrée"];
        _fadeInField =
            soundField(_fadeInRow, @selector(cmFadeTyped:), fadeHelp);
        _fadeInStepper = soundStepper(_fadeInRow);
        [_body addSubview:_fadeInRow];
        _fadeOutRow =
            [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                          labelText:@"Fondu de sortie"];
        _fadeOutField =
            soundField(_fadeOutRow, @selector(cmFadeTyped:), fadeHelp);
        _fadeOutStepper = soundStepper(_fadeOutRow);
        [_body addSubview:_fadeOutRow];
        _soundNote = [NSTextField wrappingLabelWithString:@""];
        _soundNote.font =
            CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _soundNote.textColor = CMThemeColor(ui::theme::kError);
        [_body addSubview:_soundNote];
        _soundSpacer = [[NSView alloc] initWithFrame:NSZeroRect];
        [_body addSubview:_soundSpacer];

        NSTextField* compositionHeader = CMMakeSectionHeader(@"Composition");
        [_body addSubview:compositionHeader];
        _opacityRow = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                    labelText:@"Opacité"];
        _opacitySlider = CMMakeStyledSlider(0.0, 1.0, self,
                                            @selector(cmOpacitySliderChanged:));
        _opacitySlider.continuous = YES;
        // PRD27 -- the row's label sits beside the slider, unknown to
        // VoiceOver, which read « curseur, 0,42 » and nothing of what for.
        _opacitySlider.accessibilityLabel = _opacityRow.label.stringValue;
        _opacitySlider.autoresizingMask =
            NSViewWidthSizable | NSViewHeightSizable;
        _opacitySlider.frame = _opacityRow.controlContainer.bounds;
        [_opacityRow.controlContainer addSubview:_opacitySlider];
        _opacityValueLabel = _opacityRow.valueLabel;
        [_body addSubview:_opacityRow];
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. The Gaussian blur, in
        // sequence pixels (InspectorGradeControls.h, QuantizeClipBlur).
        _blurRow = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                 labelText:@"Flou"];
        _blurSlider =
            CMMakeStyledSlider(0.0, ui::inspector::kInspectorBlurMaximum, self,
                               @selector(cmBlurSliderChanged:));
        _blurSlider.continuous = YES;
        _blurSlider.accessibilityLabel = _blurRow.label.stringValue;
        _blurSlider.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
        _blurSlider.frame = _blurRow.controlContainer.bounds;
        [_blurRow.controlContainer addSubview:_blurSlider];
        _blurValueLabel = _blurRow.valueLabel;
        [_body addSubview:_blurRow];

        _framingModeRow = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                        labelText:@"Cadrage"];
        _framingModePopup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                       pullsDown:NO];
        [_framingModePopup addItemsWithTitles:@[ @"Ajuster", @"Remplir" ]];
        [_framingModePopup itemAtIndex:0].tag =
            static_cast<NSInteger>(FramingMode::Fit);
        [_framingModePopup itemAtIndex:1].tag =
            static_cast<NSInteger>(FramingMode::Fill);
        _framingModePopup.controlSize = NSControlSizeSmall;
        _framingModePopup.font =
            CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
        _framingModePopup.target = self;
        _framingModePopup.action = @selector(cmFramingModeChanged:);
        _framingModePopup.accessibilityLabel =
            _framingModeRow.label.stringValue;
        _framingModePopup.toolTip =
            @"Ajuster : l’image entière dans le cadre ; Remplir : le cadre "
            @"entier couvert";
        _framingModePopup.autoresizingMask =
            NSViewWidthSizable | NSViewHeightSizable;
        _framingModePopup.frame = _framingModeRow.controlContainer.bounds;
        [_framingModeRow.controlContainer addSubview:_framingModePopup];
        _framingModeRow.valueLabel.stringValue = @"";
        [_body addSubview:_framingModeRow];

        _framingRows = [NSMutableArray arrayWithCapacity:3];
        _framingSliders = [NSMutableArray arrayWithCapacity:3];
        NSString* const framingLabels[] = {@"Zoom", @"Position X",
                                           @"Position Y"};
        NSString* const framingHelp[] = {
            @"Zoom sur le cadrage, de 1/8 à 8 · double-clic : 1",
            @"Pixels de la séquence depuis le centre · double-clic : 0",
            @"Pixels de la séquence depuis le centre, vers le haut comme "
            @"dans Resolve · double-clic : 0"};
        for (NSInteger index = 0; index < 3; ++index) {
            CMControlRowView* row =
                [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                              labelText:framingLabels[index]];
            CMFramingSlider* slider =
                [[CMFramingSlider alloc] initWithFrame:NSZeroRect];
            slider.minValue =
                index == 0 ? ui::inspector::kFramingScaleSliderMin : -1920.0;
            slider.maxValue =
                index == 0 ? ui::inspector::kFramingScaleSliderMax : 1920.0;
            slider.target = self;
            slider.action = @selector(cmFramingSliderChanged:);
            slider.resetAction = @selector(cmResetFramingSlider:);
            slider.controlSize = NSControlSizeSmall;
            slider.trackFillColor = CMThemeColor(ui::theme::kAccent);
            slider.continuous = YES;
            slider.tag = index;
            slider.toolTip = framingHelp[index];
            slider.accessibilityLabel = row.label.stringValue;
            slider.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            slider.frame = row.controlContainer.bounds;
            [row.controlContainer addSubview:slider];
            [_body addSubview:row];
            [_framingRows addObject:row];
            [_framingSliders addObject:slider];
        }
        // R2c -- the diamond of each keyed row, and its two arrows.
        _keyButtons = [NSMutableArray arrayWithCapacity:4];
        _previousKeyButtons = [NSMutableArray arrayWithCapacity:4];
        _nextKeyButtons = [NSMutableArray arrayWithCapacity:4];
        _keyedAnimated.fill(NO);
        NSArray<CMControlRowView*>* keyedRows =
            @[ _opacityRow, _framingRows[0], _framingRows[1], _framingRows[2] ];
        for (NSUInteger index = 0; index < keyedRows.count; ++index) {
            CMControlRowView* row = keyedRows[index];
            NSString* name = row.label.stringValue;
            NSButton* previous =
                CMMakeStyledButton(@"◂", self, @selector(cmPreviousKey:));
            NSButton* key =
                CMMakeStyledToggle(@"◇", self, @selector(cmToggleKey:));
            NSButton* next =
                CMMakeStyledButton(@"▸", self, @selector(cmNextKey:));
            previous.toolTip = @"Clé précédente";
            next.toolTip = @"Clé suivante";
            previous.accessibilityLabel =
                [NSString stringWithFormat:@"%@ : clé précédente", name];
            next.accessibilityLabel =
                [NSString stringWithFormat:@"%@ : clé suivante", name];
            for (NSButton* button : @[ previous, key, next ]) {
                button.tag = static_cast<NSInteger>(index);
                button.enabled = NO;
                button.autoresizingMask =
                    NSViewMinXMargin | NSViewHeightSizable;
                [row.controlContainer addSubview:button];
            }
            previous.hidden = YES;
            next.hidden = YES;
            [_keyButtons addObject:key];
            [_previousKeyButtons addObject:previous];
            [_nextKeyButtons addObject:next];
        }

        const std::vector<ui::inspector::GradeControlSpec>& controls =
            ui::inspector::GradeControls();
        _gradeSliders = [NSMutableArray arrayWithCapacity:controls.size()];
        _gradeValueLabels = [NSMutableArray arrayWithCapacity:controls.size()];
        _gradeDebounceTimers =
            [NSMutableArray arrayWithCapacity:controls.size()];
        // Grading rows in table order, a section header (and the spacer
        // before it) wherever the family changes: the eight scalar knobs,
        // then the wheels, then the curves (InspectorGradeControls.h).
        std::vector<std::pair<NSView*, CGFloat>> gradeItems;
        // Visual QA of 19 September 2026 -- the whole grade back to neutral
        // in one edit, one Undo.
        _resetGradeButton = CMMakeStyledButton(@"Réinitialiser l'étalonnage",
                                               self, @selector(cmResetGrade:));
        [_body addSubview:_resetGradeButton];
        gradeItems.push_back(
            {_resetGradeButton, CGF(ui::theme::kControlRowHeight)});
        _collapsed = [NSMutableSet set];
        _orderNote = [NSTextField
            wrappingLabelWithString:@"Pile écrite dans un autre ordre que "
                                    @"celui du panneau : éditée en place."];
        _orderNote.font =
            CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular);
        _orderNote.textColor = CMThemeColor(ui::theme::kTextTertiary);
        _orderNote.hidden = YES;
        [_body addSubview:_orderNote];
        [_collapsed addObject:_orderNote];
        gradeItems.push_back({_orderNote, kOrderNoteHeight});

        _wheelGrid = [[CMGradeWheelGridView alloc] initWithFrame:NSZeroRect];
        _wheelGrid.host = self;
        [_body addSubview:_wheelGrid];
        _curveView = [[CMGradeCurveView alloc] initWithFrame:NSZeroRect];
        _curveView.host = self;
        [_body addSubview:_curveView];
        _hueCurveView = [[CMGradeHueCurveView alloc] initWithFrame:NSZeroRect];
        _hueCurveView.host = self;
        [_body addSubview:_hueCurveView];
        _wheelSlidersToggle = CMMakeStyledButton(
            @"Curseurs par canal ▸", self, @selector(cmToggleWheelSliders:));
        [_body addSubview:_wheelSlidersToggle];
        _curveSlidersToggle =
            CMMakeStyledButton(@"Curseurs (écart à la diagonale) ▸", self,
                               @selector(cmToggleCurveSliders:));
        [_body addSubview:_curveSlidersToggle];
        _wheelSliderRows = [NSMutableArray array];
        _curveSliderRows = [NSMutableArray array];
        std::string section;
        for (size_t index = 0; index < controls.size(); ++index) {
            const std::string title =
                ui::inspector::GradeControlSectionTitle(controls[index]);
            if (title != section) {
                if (!section.empty()) {
                    gradeItems.push_back({nil, CGF(ui::theme::kSpaceM)});
                }
                NSTextField* header = CMMakeSectionHeader(
                    [NSString stringWithUTF8String:title.c_str()]);
                [_body addSubview:header];
                if (std::string(controls[index].type) == "color.wheels") {
                    _wheelGradeStart = gradeItems.size();
                }
                if (std::string(controls[index].type) == "color.curves") {
                    _curveGradeStart = gradeItems.size();
                }
                gradeItems.push_back({header, kSectionHeaderHeight});
                section = title;
                // GR4, GR5 -- the control a hand reaches for, then the
                // button that unfolds the section's sliders.
                const std::string type = controls[index].type;
                if (type == "color.wheels") {
                    gradeItems.push_back(
                        {_wheelGrid,
                         CGF(ui::inspector::LayoutGradeWheelGrid(0.0).height)});
                    gradeItems.push_back({_wheelSlidersToggle,
                                          CGF(ui::theme::kControlRowHeight)});
                } else if (type == "color.curves") {
                    gradeItems.push_back(
                        {_curveView,
                         CGF(ui::inspector::LayoutGradeCurve(0.0).height)});
                    gradeItems.push_back({_curveSlidersToggle,
                                          CGF(ui::theme::kControlRowHeight)});
                }
            }
            NSSlider* slider = nil;
            NSTextField* valueLabel = nil;
            CMControlRowView* row =
                [self cmAddGradeRowForSpec:controls[index]
                                     index:static_cast<NSInteger>(index)
                                    slider:&slider
                                valueLabel:&valueLabel];
            [_gradeSliders addObject:slider];
            [_gradeValueLabels addObject:valueLabel];
            [_gradeDebounceTimers addObject:NSNull.null];
            gradeItems.push_back({row, CGF(ui::theme::kControlRowHeight)});
            const std::string type = controls[index].type;
            if (type == "color.wheels" || type == "color.curves") {
                [(type == "color.wheels" ? _wheelSliderRows : _curveSliderRows)
                    addObject:row];
                [_collapsed addObject:row];
                row.hidden = YES;
            }
        }

        _layoutItems.push_back({propertiesHeader, kSectionHeaderHeight});
        _layoutItems.push_back({_nameRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_trackRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_startRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back(
            {_durationRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_dipRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_lookRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({nil, CGF(ui::theme::kSpaceM)});  // spacer
        // GitHub #330 -- the sound, before the picture's composition: for a
        // sound it is all the panel can change; collapsed for no sound.
        _layoutItems.push_back({_soundHeader, kSectionHeaderHeight});
        _layoutItems.push_back({_gainRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_fadeInRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back(
            {_fadeOutRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_soundNote, kOrderNoteHeight});
        _layoutItems.push_back({_soundSpacer, CGF(ui::theme::kSpaceM)});
        _layoutItems.push_back({compositionHeader, kSectionHeaderHeight});
        _layoutItems.push_back(
            {_opacityRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({_blurRow, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back(
            {_framingModeRow, CGF(ui::theme::kControlRowHeight)});
        for (CMControlRowView* row in _framingRows)
            _layoutItems.push_back({row, CGF(ui::theme::kControlRowHeight)});
        _layoutItems.push_back({nil, CGF(ui::theme::kSpaceM)});  // spacer
        // GR8 -- the hue curves close the grade.
        NSTextField* hueHeader = CMMakeSectionHeader(@"Courbes de teinte");
        [_body addSubview:hueHeader];
        gradeItems.push_back({nil, CGF(ui::theme::kSpaceM)});
        gradeItems.push_back({hueHeader, kSectionHeaderHeight});
        gradeItems.push_back(
            {_hueCurveView,
             CGF(ui::inspector::LayoutGradeHueCurve(0.0).height)});
        for (NSView* view in @[
                 _soundHeader, _gainRow, _fadeInRow, _fadeOutRow, _soundNote,
                 _soundSpacer
             ]) {
            [_collapsed addObject:view];
            view.hidden = YES;
        }
        _gradeStart = _layoutItems.size();
        _gradeItems = gradeItems;
        _layoutItems.insert(_layoutItems.end(), gradeItems.begin(),
                            gradeItems.end());

        // Nothing selected yet -- placeholder shows until the first
        // -reloadWithDocument:selectedClipId: call (main.mm makes one
        // right after installing this view).
        _placeholderLabel.hidden = NO;
        _scrollView.hidden = YES;

        // DEC42 -- a fold left open stays open from one launch to the
        // next (UiPreferences.h), a local preference, never the project's.
        CMUiPreferences* preferences = [CMUiPreferences sharedPreferences];
        if ([preferences inspectorSlidersUnfolded:@"wheels"])
            [self cmFold:_wheelSliderRows
                  toggle:_wheelSlidersToggle
                   title:@"Curseurs par canal"];
        if ([preferences inspectorSlidersUnfolded:@"curves"])
            [self cmFold:_curveSliderRows
                  toggle:_curveSlidersToggle
                   title:@"Curseurs (écart à la diagonale)"];

        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)dealloc {
    [self cmInvalidateAllGradeTimers];
}

- (CMControlRowView*)cmAddPropertyRowWithLabel:(NSString*)labelText
                                    valueField:
                                        (NSTextField* __strong*)outValueField {
    CMControlRowView* row = [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                                          labelText:labelText];
    // Property rows show a free-form string (a filename, a timecode) wider
    // than the fixed 56pt valueLabel column comfortably fits, so they leave
    // valueLabel empty/hidden -- exactly the case UiComponents.h's own doc
    // comment calls out. PRD37 -- ROADMAP.md: and the string takes that
    // column too, up to the row's end (-cmPlacePropertyValues). Inside the
    // control column alone it had some seventy points, and the QA of 3
    // October 2026 read « Sous-titres… » for a carton's track.
    row.valueLabel.hidden = YES;
    NSTextField* value = [NSTextField labelWithString:@""];
    value.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
    value.textColor = CMTextSecondaryColor();
    value.lineBreakMode = NSLineBreakByTruncatingTail;
    [row addSubview:value];
    *outValueField = value;
    [_body addSubview:row];
    return row;
}

- (CMControlRowView*)
    cmAddGradeRowForSpec:(const ui::inspector::GradeControlSpec&)spec
                   index:(NSInteger)index
                  slider:(NSSlider* __strong*)outSlider
              valueLabel:(NSTextField* __strong*)outValueLabel {
    CMControlRowView* row = [[CMControlRowView alloc]
        initWithFrame:NSZeroRect
            labelText:[NSString stringWithUTF8String:spec.label]];
    CMResettableSlider* slider =
        [[CMResettableSlider alloc] initWithFrame:NSZeroRect];
    // CMMakeStyledSlider's styling, on the subclass that resets.
    slider.minValue = spec.min_value;
    slider.maxValue = spec.max_value;
    slider.target = self;
    slider.action = @selector(cmGradeSliderChanged:);
    slider.resetAction = @selector(cmResetGradeSlider:);
    slider.controlSize = NSControlSizeSmall;
    slider.trackFillColor = CMThemeColor(ui::theme::kAccent);
    slider.toolTip = @"Double-clic : valeur neutre";
    // PRD27 -- named for VoiceOver as its row is on screen.
    slider.accessibilityLabel = row.label.stringValue;
    slider.tag = index;
    // Continuous: the action fires on every drag tick, not only on mouse
    // up. -cmGradeSliderChanged: uses that to keep the value label live
    // while only *scheduling* (debouncing) the actual committed edit --
    // see kGradeCommitDebounceSeconds above.
    slider.continuous = YES;
    slider.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    slider.frame = row.controlContainer.bounds;
    [row.controlContainer addSubview:slider];
    [_body addSubview:row];
    *outSlider = slider;
    *outValueLabel = row.valueLabel;
    return row;
}

- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    _placeholderLabel.frame =
        NSMakeRect(inset, 0.0, std::max<CGFloat>(0.0, size.width - 2 * inset),
                   size.height);
    _scrollView.frame = NSMakeRect(0.0, 0.0, size.width, size.height);
    const NSSize visible = _scrollView.contentSize;

    if (_colorWorkspace && !_isCarton) {
        std::array<std::vector<std::pair<NSView*, CGFloat>>, 4> groups;
        const auto add = [&](size_t group,
                             const std::pair<NSView*, CGFloat>& item) {
            if (item.first == nil || ![_collapsed containsObject:item.first]) {
                groups[group].push_back(item);
            }
        };
        for (size_t index = 0; index < _gradeItems.size(); ++index) {
            const size_t group = index < 2                  ? 0
                                 : index < _wheelGradeStart ? 3
                                 : index < _curveGradeStart ? 1
                                                            : 2;
            add(group, _gradeItems[index]);
        }
        for (size_t index = 0; index < _gradeStart; ++index) {
            add(3, _layoutItems[index]);
        }
        std::array<std::vector<double>, 4> heights;
        for (size_t group = 0; group < groups.size(); ++group) {
            for (const auto& item : groups[group]) {
                heights[group].push_back(item.second);
            }
        }
        const auto layout =
            ui::inspector::LayoutGradeWorkspace(visible.width, heights);
        _body.frame =
            NSMakeRect(0.0, 0.0, visible.width,
                       std::max<CGFloat>(layout.height, visible.height));
        for (size_t group = 0; group < groups.size(); ++group) {
            for (size_t index = 0; index < groups[group].size(); ++index) {
                NSView* view = groups[group][index].first;
                if (view == nil) {
                    continue;
                }
                const auto& rect = layout.groups[group][index];
                const CGFloat padding =
                    [view isKindOfClass:[CMControlRowView class]] ? 0.0 : inset;
                view.frame = NSMakeRect(
                    rect.x + padding, rect.y,
                    std::max<CGFloat>(0.0, rect.width - 2.0 * padding),
                    rect.height);
            }
        }
        [self cmPlacePropertyValues];
        [self cmPlaceKeyButtons];
        [self cmPlaceSoundControls];
        return;
    }

    // D7 -- a carton's text section sits above the rows every clip has,
    // and takes no room for any other clip.
    std::vector<std::pair<NSView*, CGFloat>> items;
    if (_isCarton) {
        items = _captionItems;
        // F2.3 -- its style follows its words.
        items.insert(items.end(), _styleItems.begin(), _styleItems.end());
    }
    // GR4, GR5 -- a folded row takes no room.
    for (const auto& item : _layoutItems) {
        if (item.first == nil || ![_collapsed containsObject:item.first]) {
            items.push_back(item);
        }
    }

    // The body is as tall as its rows, and never shorter than the visible
    // area, so a short panel still starts at the top.
    CGFloat contentHeight = 2 * inset;
    for (const auto& item : items) {
        contentHeight += item.second + CGF(ui::theme::kSpaceXxs);
    }
    _body.frame = NSMakeRect(0.0, 0.0, visible.width,
                             std::max(contentHeight, visible.height));
    size.width = visible.width;

    // _body is flipped: y runs from the top down.
    CGFloat y = inset;
    for (const auto& item : items) {
        NSView* view = item.first;
        const CGFloat height = item.second;
        if (view) {
            if ([view isKindOfClass:[CMControlRowView class]]) {
                // CMControlRowView insets its own subviews (UiComponents.mm),
                // so the row itself spans the full width.
                view.frame = NSMakeRect(0.0, y, size.width, height);
            } else {
                // A bare section header (CMMakeSectionHeader) has no
                // built-in inset of its own.
                view.frame = NSMakeRect(
                    inset, y, std::max<CGFloat>(0.0, size.width - 2 * inset),
                    height);
            }
        }
        y += height + CGF(ui::theme::kSpaceXxs);  // breathing room
    }
    [self cmPlacePropertyValues];
    [self cmPlaceKeyButtons];
    [self cmPlaceStyleControls];
    [self cmPlaceSoundControls];

    // The two buttons share their row, a gap between them.
    const NSSize nav = _captionNav.bounds.size;
    const CGFloat half =
        std::max<CGFloat>(0.0, (nav.width - CGF(ui::theme::kSpaceS)) / 2);
    _captionPrevious.frame = NSMakeRect(0.0, 0.0, half, nav.height);
    _captionNext.frame = NSMakeRect(nav.width - half, 0.0, half, nav.height);
}

// PRD37 -- a property's string from the row's value column to its end, the
// row's own layout (UiComponents.mm) having just placed both columns.
- (void)cmPlacePropertyValues {
    // A C array: before -initWithFrame: has built them, the fields are nil.
    NSTextField* const values[] = {_nameValueField,  _trackValueField,
                                   _startValueField, _durationValueField,
                                   _dipValueField,   _lookValueField};
    for (NSTextField* value : values) {
        if (value == nil) continue;
        CMControlRowView* row = (CMControlRowView*)value.superview;
        const CGFloat left = NSMinX(row.valueLabel.frame);
        value.frame = NSMakeRect(
            left, 0.0,
            std::max<CGFloat>(0.0, NSMaxX(row.controlContainer.frame) - left),
            row.bounds.size.height);
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (BOOL)colorWorkspace {
    return _colorWorkspace;
}

- (void)setColorWorkspace:(BOOL)colorWorkspace {
    if (_colorWorkspace == colorWorkspace) {
        return;
    }
    _colorWorkspace = colorWorkspace;
    [self cmLayoutForSize:self.bounds.size];
    [_scrollView.contentView scrollToPoint:NSZeroPoint];
    [_scrollView reflectScrolledClipView:_scrollView.contentView];
}

- (void)reloadWithDocument:(const Document&)document
            selectedClipId:(const Ulid&)clipId {
    // Any grading commit still debounced against the *previous* selection
    // must not land on whatever clip happens to be selected when its timer
    // fires.
    [self cmInvalidateAllGradeTimers];

    const DocumentClip* clip =
        clipId.empty() ? nullptr : document.FindClip(clipId);
    if (!clip) {
        _clipId = Ulid{};
        _gradable = NO;
        _effects.clear();
        _opacity = {1, 1};
        _framing = ClipFraming{};
        _framable = NO;
        _hasKeyedClip = NO;
        [self cmShowKeyedRows];
        _blur = {0, 1};
        _captionLoadedText.clear();
        _style.reset();
        [self cmShowCaptionSection:NO];
        [self cmReloadSoundFrom:document];
        _placeholderLabel.hidden = NO;
        _scrollView.hidden = YES;
        return;
    }

    const Ulid previousClipId = _clipId;
    _clipId = clipId;
    _effects = clip->effects;
    _neutralKelvin =
        ColorGradeNeutralKelvin(ColorGradeContextFor(document, *clip));
    _opacity = clip->opacity;
    _blur = clip->blur;
    _placeholderLabel.hidden = YES;
    _scrollView.hidden = NO;

    const DocumentSource* source = document.FindSource(clip->source_id);
    const DocumentTrack* track = document.FindTrackForClip(clip->id);
    // D7 -- a carton has no source to be named after, and its identifier,
    // shown here until its words had a section above, names nothing.
    _nameValueField.stringValue =
        source ? CMBasename(source->path)
        : track && track->kind == "caption"
            ? @"Carton"
            : [NSString stringWithUTF8String:clip->id.c_str()];

    if (track) {
        NSString* kind = track->kind == "audio"     ? @"Audio"
                         : track->kind == "caption" ? @"Sous-titres"
                                                    : @"Vidéo";
        _trackValueField.stringValue = [NSString
            stringWithFormat:@"%@ · %@", kind,
                             [NSString
                                 stringWithUTF8String:TrackDisplayName(
                                                          document, track->id)
                                                          .c_str()]];
    } else {
        _trackValueField.stringValue = @"—";
    }
    // D7 -- the carton's words. The field is rewritten when the panel turns
    // to another clip, or when the document's text changed under it (a
    // commit, an undo, an agent), and left alone otherwise: a reload that
    // changes nothing here must not take away what is being typed.
    const BOOL carton = track && track->kind == "caption";
    const BOOL wasCarton = _isCarton;
    [self cmShowCaptionSection:carton];
    if (carton) {
        if (!wasCarton || previousClipId != clipId ||
            clip->caption_text != _captionLoadedText) {
            // What was being typed goes with it, on the monitor too.
            [self cmEndCaptionPreview];
            _captionField.stringValue =
                [NSString stringWithUTF8String:clip->caption_text.c_str()]
                    ?: @"";
        }
        _captionLoadedText = clip->caption_text;
        // A locked track's cartons are read, not written.
        _captionField.editable = !track->locked;
        _captionPrevious.enabled =
            ui::media_panel::AdjacentCarton(document, clipId, false)
                .has_value();
        _captionNext.enabled =
            ui::media_panel::AdjacentCarton(document, clipId, true).has_value();
        [self cmReloadCaptionStyleFrom:document clip:*clip];
    } else {
        _captionLoadedText.clear();
        _style.reset();
    }
    _gradable = track && ui::inspector::IsGradableTrackKind(track->kind);
    // Enabled while the clip carries any grade the sliders drive.
    _resetGradeButton.enabled =
        ui::inspector::CanResetGrade(_gradable, _effects);
    _opacityEditable = track && track->kind == "video";
    _opacitySlider.enabled = _opacityEditable;
    [self cmShowOpacity];
    _framing = clip->framing;
    _framable = ui::inspector::CanFrameClip(*clip, track);
    _sequenceWidth = document.sequence.width;
    _sequenceHeight = document.sequence.height;
    [self cmShowFramingForClip:*clip];
    _keyedClip = *clip;
    _hasKeyedClip = YES;
    _rate = document.sequence.frame_rate;
    [self cmShowKeyedRows];
    // EF3 -- the clips the Effets tab's « Flou gaussien » takes: a picture
    // of a media on an unlocked video track whose framing does not move
    // (EffectsLibrary.h). The reason another clip takes none is the tab's,
    // shown on the row.
    if (const ui::effects::Effect* blurEffect =
            ui::effects::FindEffect("gaussian_blur")) {
        const ui::effects::DropTarget blurTarget =
            ui::effects::ClipEffectTarget(document, *blurEffect, clip->id);
        _blurSlider.enabled = blurTarget.valid;
        _blurSlider.toolTip =
            blurTarget.valid
                ? nil
                : [NSString stringWithUTF8String:blurTarget.refusal.c_str()];
    }
    [self cmShowBlur];
    [self cmReloadSoundFrom:document];

    const MediaRate rate = document.sequence.frame_rate;
    _startValueField.stringValue =
        CMInspectorTimecodeString(clip->timeline_in, rate);
    _durationValueField.stringValue =
        CMInspectorTimecodeString(clip->duration, rate);
    // DEC42 -- each dip on one line of the tooltip, all of them in the
    // row, « — » without one; the look by its .cube and its space.
    std::string dips;
    for (const std::string& line :
         ui::media_panel::ClipDipDescriptions(document, clip->id))
        dips += (dips.empty() ? "" : " ; ") + line;
    _dipValueField.stringValue =
        dips.empty() ? @"—" : [NSString stringWithUTF8String:dips.c_str()];
    _dipValueField.toolTip =
        dips.empty() ? nil : [NSString stringWithUTF8String:dips.c_str()];
    const std::string look =
        ui::media_panel::SequenceLookDescription(document.sequence);
    _lookValueField.stringValue = [NSString stringWithUTF8String:look.c_str()];
    _lookValueField.toolTip = _lookValueField.stringValue;

    for (size_t index = 0; index < ui::inspector::GradeControls().size();
         ++index)
        [self cmShowGradeControlAtIndex:index];
    [self cmReloadGradeControls];
}

- (void)cmShowOpacity {
    const float opacity = ui::inspector::CurrentClipOpacity(_opacity);
    _opacitySlider.floatValue = opacity;
    _opacityValueLabel.stringValue =
        [NSString stringWithFormat:@"%.0f %%", opacity * 100.0f];
}

// The zoom and position as the clip holds them. A keyframed one is shown
// and left to its animation (IsFramingControlAnimated).
- (void)cmShowFramingForClip:(const DocumentClip&)clip {
    [_framingModePopup selectItemWithTag:static_cast<NSInteger>(_framing.mode)];
    _framingModePopup.enabled = _framable;
    for (NSInteger index = 0; index < 3; ++index) {
        const auto control = static_cast<ui::inspector::FramingControl>(index);
        CMFramingSlider* slider = _framingSliders[index];
        if (index > 0) {
            const double bound = index == 1 ? _sequenceWidth : _sequenceHeight;
            slider.minValue = -std::max(bound, 1.0);
            slider.maxValue = std::max(bound, 1.0);
        }
        // R2c -- a keyed row shows its playhead's frame: cmShowKeyedRows.
        if (ui::inspector::IsFramingControlAnimated(clip, control)) continue;
        slider.enabled = _framable;
        slider.floatValue = ui::inspector::FramingSliderValue(
            _framing, control, _sequenceWidth, _sequenceHeight);
        _framingRows[index].valueLabel.stringValue =
            [self cmFramingText:control value:slider.floatValue];
    }
}

- (void)cmShowFraming {
    for (NSInteger index = 0; index < 3; ++index) {
        const auto control = static_cast<ui::inspector::FramingControl>(index);
        _framingSliders[index].floatValue = ui::inspector::FramingSliderValue(
            _framing, control, _sequenceWidth, _sequenceHeight);
        if (_framingSliders[index].enabled)
            _framingRows[index].valueLabel.stringValue =
                [self cmFramingText:control
                              value:_framingSliders[index].floatValue];
    }
    [self cmShowKeyedRows];
}

- (NSString*)cmFramingText:(ui::inspector::FramingControl)control
                     value:(float)value {
    if (control == ui::inspector::FramingControl::Scale) {
        const EffectParamValue scale =
            ui::inspector::FramingScaleFromSlider(value);
        return
            [NSString stringWithFormat:@"%.3f", static_cast<double>(scale.num) /
                                                    std::max(scale.den, 1)];
    }
    return [NSString stringWithFormat:@"%+.0f px", std::round(value)];
}

// EF3 -- the blur as the document holds it, in pixels of the sequence.
- (void)cmShowBlur {
    const float blur = ui::inspector::CurrentClipBlur(_blur);
    _blurSlider.floatValue = blur;
    _blurValueLabel.stringValue = [NSString stringWithFormat:@"%.1f px", blur];
}

// A slider and its value as the stack shows them.
- (void)cmShowGradeControlAtIndex:(size_t)index {
    const ui::inspector::GradeControlSpec& spec =
        ui::inspector::GradeControls()[index];
    const float value =
        ui::inspector::CurrentGradeControlValue(_effects, spec, _neutralKelvin);
    _gradeSliders[index].floatValue = value;
    // F1.3 -- a curve channel written off the Inspector's grid is shown
    // but not editable here (InspectorGradeControls.h).
    _gradeSliders[index].enabled =
        _gradable && ui::inspector::IsGradeControlEditable(_effects, spec);
    _gradeValueLabels[index].stringValue = CMFormatGradeValue(spec, value);
}

// PRD37 -- ROADMAP.md. After one of this panel's own edits, the stack the
// document holds now, without the full reload's dropping of every pending
// commit (InspectorGradeControls.h, RefreshAfterOwnEdit).
- (void)refreshAfterOwnEditWithDocument:(const Document&)document {
    const DocumentClip* clip =
        _clipId.empty() ? nullptr : document.FindClip(_clipId);
    // The clip went with the edit: the panel turns to the selection.
    if (clip == nullptr) {
        [self reloadWithDocument:document selectedClipId:_clipId];
        return;
    }
    _effects = clip->effects;
    _opacity = clip->opacity;
    _framing = clip->framing;
    _keyedClip = *clip;
    _rate = document.sequence.frame_rate;
    [self cmShowFramingForClip:*clip];
    _blur = clip->blur;
    std::vector<bool> pending(_gradeDebounceTimers.count, false);
    for (NSUInteger index = 0; index < _gradeDebounceTimers.count; ++index)
        pending[index] =
            [_gradeDebounceTimers[index] isKindOfClass:[NSTimer class]];
    const ui::inspector::OwnEditRefresh refresh =
        ui::inspector::RefreshAfterOwnEdit(_gradable, _effects,
                                           _opacityDebounceTimer != nil,
                                           pending, _blurDebounceTimer != nil);
    _resetGradeButton.enabled = refresh.reset_enabled;
    if (refresh.rewrite_opacity) [self cmShowOpacity];
    if (refresh.rewrite_blur) [self cmShowBlur];
    for (size_t index = 0; index < refresh.rewrite_sliders.size(); ++index)
        if (refresh.rewrite_sliders[index])
            [self cmShowGradeControlAtIndex:index];
    [self cmReloadGradeControls];
    [self cmShowKeyedRows];
    [self cmReloadSoundFrom:document];
}

#pragma mark - GR4, GR5 wheels and curves

// The wheels, the curve editor and the note on the stack's order, from the
// stack as it is now.
- (void)cmReloadGradeControls {
    [_wheelGrid reload];
    [_curveView reload];
    [_hueCurveView reload];
    // GR7 -- a stack longer than a clip's grade renders is the more urgent
    // thing to say: part of it is not in the picture.
    std::string overflow;
    const BOOL fits = ColorStackFits(_effects, overflow);
    _orderNote.stringValue =
        fits ? @"Pile écrite dans un autre ordre que celui du panneau : "
               @"éditée en place."
             : @"Pile trop longue : le moniteur et l'export n'en rendent "
               @"que le début.";
    const BOOL noted = !_clipId.empty() && _gradable &&
                       (!fits || !IsCanonicalColorStack(_effects));
    if (noted == [_collapsed containsObject:_orderNote]) {
        if (noted) {
            [_collapsed removeObject:_orderNote];
        } else {
            [_collapsed addObject:_orderNote];
        }
        _orderNote.hidden = !noted;
        [self cmLayoutForSize:self.bounds.size];
    }
}

- (void)cmFold:(NSArray<NSView*>*)rows
        toggle:(NSButton*)toggle
         title:(NSString*)title {
    const BOOL folded = [_collapsed containsObject:rows.firstObject];
    for (NSView* row in rows) {
        if (folded) {
            [_collapsed removeObject:row];
        } else {
            [_collapsed addObject:row];
        }
        row.hidden = !folded;
    }
    toggle.title =
        [NSString stringWithFormat:@"%@ %@", title, folded ? @"▾" : @"▸"];
    [self cmLayoutForSize:self.bounds.size];
}

- (void)cmToggleWheelSliders:(id)sender {
    (void)sender;
    [self cmFold:_wheelSliderRows
          toggle:_wheelSlidersToggle
           title:@"Curseurs par canal"];
    // DEC42 -- remembered for the next launch.
    [[CMUiPreferences sharedPreferences]
        setInspectorSliders:@"wheels"
                   unfolded:![_collapsed
                                containsObject:_wheelSliderRows.firstObject]];
}

- (void)cmToggleCurveSliders:(id)sender {
    (void)sender;
    [self cmFold:_curveSliderRows
          toggle:_curveSlidersToggle
           title:@"Curseurs (écart à la diagonale)"];
    [[CMUiPreferences sharedPreferences]
        setInspectorSliders:@"curves"
                   unfolded:![_collapsed
                                containsObject:_curveSliderRows.firstObject]];
}

// CMGradeEditingHost -- what the wheels and the curve editor read and
// report. A gesture is one edit: its stack goes out as one
// SetClipEffectsOperation, like a slider's, when it ends.
- (const std::vector<ClipEffect>&)gradeEffects {
    return _effects;
}

- (BOOL)wantsCurveHistogram {
    return _gradable && !self.hiddenOrHasHiddenAncestor &&
           !_curveView.hiddenOrHasHiddenAncestor;
}

- (void)showCurveHistogram:(const VideoHistogram&)histogram {
    [_curveView showHistogram:histogram];
}

- (BOOL)gradeEditable {
    return !_clipId.empty() && _gradable;
}

- (void)gradePreviewEffects:(const std::vector<ClipEffect>&)effects {
    if (![self gradeEditable]) return;
    if ([self.delegate respondsToSelector:@selector(inspectorView:
                                               previewClipEffects:forClip:)])
        [self.delegate inspectorView:self
                  previewClipEffects:effects
                             forClip:_clipId];
}

- (void)gradeCommitEffects:(const std::vector<ClipEffect>&)effects {
    if (![self gradeEditable] || !self.delegate) return;
    // A slider's pending commit was built on the stack this one replaces.
    [self cmInvalidateAllGradeTimers];
    _effects = effects;
    SetClipEffectsOperation operation;
    operation.clip_id = _clipId;
    operation.effects = _effects;
    [self.delegate inspectorView:self
            didCommitClipEffects:std::move(operation)];
}

#pragma mark - F2.3 caption style

// The value column of a number row holds its field, the popups span both
// columns, and the control column holds, left to right, a stepper, a
// checkbox, a well, or the slider across it.
- (void)cmPlaceStyleControls {
    if (_styleFontRow == nil) return;
    const CGFloat gap = CGF(ui::theme::kSpaceXs);
    const auto centred = [](NSView* view, CGFloat x, CGFloat width,
                            CGFloat height) {
        const CGFloat rowHeight = view.superview.bounds.size.height;
        view.frame = NSMakeRect(x, std::floor((rowHeight - height) / 2.0),
                                std::max<CGFloat>(0.0, width), height);
    };
    for (CMControlRowView* owner in @[ _styleFontRow, _stylePositionRow ]) {
        NSPopUpButton* control =
            owner == _styleFontRow ? _styleFontPopup : _stylePositionPopup;
        const CGFloat left = NSMinX(owner.valueLabel.frame);
        centred(control, left, NSMaxX(owner.controlContainer.frame) - left,
                22.0);
    }
    const struct {
        CMControlRowView* owner;
        NSTextField* field;
        NSStepper* stepper;
        NSColorWell* well;
    } numbers[] = {
        {_styleSizeRow, _styleSizeField, _styleSizeStepper, nil},
        {_styleOutlineRow, _styleOutlineField, _styleOutlineStepper,
         _styleOutlineWell},
        {_styleShadowRow, _styleShadowField, _styleShadowStepper,
         _styleShadowWell},
        {_stylePaddingRow, _stylePaddingField, _stylePaddingStepper, nil},
    };
    for (const auto& entry : numbers) {
        const NSRect value = entry.owner.valueLabel.frame;
        centred(entry.field, NSMinX(value), NSWidth(value), 20.0);
        centred(entry.stepper, 0.0, 15.0, 22.0);
        if (entry.well) centred(entry.well, 15.0 + gap, 40.0, 20.0);
    }
    // Every well in one column, after the steppers'.
    centred(_styleColorWell, 15.0 + gap, 40.0, 20.0);
    centred(_styleBoxCheck, 0.0, 18.0, 18.0);
    centred(_styleBoxWell, 15.0 + gap, 40.0, 20.0);
    centred(_styleHeightSlider, 0.0,
            _styleHeightRow.controlContainer.bounds.size.width, 18.0);
}

- (void)cmReloadCaptionStyleFrom:(const Document&)document
                            clip:(const DocumentClip&)clip {
    const CaptionStyle* style =
        clip.caption_group_id.empty()
            ? nullptr
            : document.FindCaptionStyle(clip.caption_group_id);
    if (style == nullptr) {
        _style.reset();
        _styleNote.textColor = CMThemeColor(ui::theme::kTextTertiary);
        _styleNote.stringValue =
            @"Aspect par défaut : donnez un style à ce carton dans l’onglet "
            @"Légendes pour le régler ici.";
        [self cmShowCaptionStyle];
        return;
    }
    _style = *style;
    int32_t cartons = 0;
    for (const auto& summary :
         ui::media_panel::SummarizeCaptionStyles(document.sequence))
        if (summary.style_id == style->id) cartons = summary.carton_count;
    _styleNote.textColor = CMThemeColor(ui::theme::kTextTertiary);
    _styleNote.stringValue =
        cartons <= 1 ? @"Ce style n’habille que ce carton."
                     : [NSString stringWithFormat:@"Ce style habille %d "
                                                  @"cartons : un réglage les "
                                                  @"change tous.",
                                                  cartons];
    [self cmShowCaptionStyle];
}

// Every control as `_style` holds it; all off without a style.
- (void)cmShowCaptionStyle {
    const BOOL styled = _style.has_value();
    for (NSControl* control in @[
             _styleFontPopup, _styleSizeField, _styleSizeStepper,
             _styleColorWell, _styleOutlineField, _styleOutlineStepper,
             _styleOutlineWell, _styleShadowField, _styleShadowStepper,
             _styleShadowWell, _styleBoxCheck, _styleBoxWell,
             _stylePaddingField, _stylePaddingStepper, _stylePositionPopup,
             _styleHeightSlider
         ])
        control.enabled = styled;
    const CaptionStyle shown = styled ? *_style : CaptionStyle{};
    CMSelectCaptionFont(_styleFontPopup, shown.font_family);
    const auto showNumber = [](NSTextField* field, NSStepper* stepper,
                               int32_t value) {
        field.integerValue = value;
        stepper.integerValue = value;
    };
    showNumber(_styleSizeField, _styleSizeStepper, shown.font_size);
    showNumber(_styleOutlineField, _styleOutlineStepper, shown.outline_width);
    showNumber(_styleShadowField, _styleShadowStepper, shown.shadow_offset);
    showNumber(_stylePaddingField, _stylePaddingStepper,
               shown.background_padding);
    _styleColorWell.color = CMCaptionColor(shown.color, NSColor.whiteColor);
    _styleOutlineWell.color =
        CMCaptionColor(shown.outline_color, NSColor.blackColor);
    _styleShadowWell.color =
        CMCaptionColor(shown.shadow_color, NSColor.blackColor);
    const bool boxed = !shown.background_color.empty();
    _styleBoxCheck.state =
        boxed ? NSControlStateValueOn : NSControlStateValueOff;
    _styleBoxWell.color = CMCaptionColor(
        boxed ? shown.background_color
              : std::string(ui::media_panel::kCaptionDefaultBackground),
        NSColor.blackColor);
    // The box's colour and margin mean nothing without the box.
    _styleBoxWell.enabled = styled && boxed;
    _stylePaddingField.enabled = styled && boxed;
    _stylePaddingStepper.enabled = styled && boxed;
    [_stylePositionPopup
        selectItemAtIndex:static_cast<NSInteger>(
                              ui::media_panel::CaptionPlacementOf(shown))];
    const int32_t height = ui::media_panel::CaptionHeightPercent(shown);
    _styleHeightSlider.integerValue = height;
    _styleHeightRow.valueLabel.stringValue =
        [NSString stringWithFormat:@"%d %%", height];
}

// One gesture, one UpdateCaptionStyleOperation: the stored style with what
// the control says, checked by the policy (RestyleCaptionStyleWhole), then
// handed to the delegate. A drag of the height is written at its release.
- (void)cmCaptionStyleChanged:(id)sender {
    if (!_style) return;
    CaptionStyle wanted = *_style;
    const auto stepped = [](NSTextField* field, NSStepper* stepper, id from) {
        if (from == stepper) field.integerValue = stepper.integerValue;
        return static_cast<int32_t>(field.integerValue);
    };
    if (sender == _styleHeightSlider) {
        const int32_t height =
            static_cast<int32_t>(std::lround(_styleHeightSlider.doubleValue));
        _styleHeightRow.valueLabel.stringValue =
            [NSString stringWithFormat:@"%d %%", height];
        // A tick of a drag: written at the release.
        if (_styleHeightSlider.tracking) return;
        wanted = ui::media_panel::WithCaptionPlacement(
            wanted, ui::media_panel::CaptionPlacement::Custom, height);
    } else if (sender == _stylePositionPopup) {
        wanted = ui::media_panel::WithCaptionPlacement(
            wanted,
            static_cast<ui::media_panel::CaptionPlacement>(
                _stylePositionPopup.indexOfSelectedItem),
            static_cast<int32_t>(std::lround(_styleHeightSlider.doubleValue)));
    } else if (sender == _styleFontPopup) {
        NSString* family = _styleFontPopup.selectedItem.representedObject;
        wanted.font_family = family.UTF8String ?: "";
    } else if (sender == _styleSizeField || sender == _styleSizeStepper) {
        wanted.font_size = stepped(_styleSizeField, _styleSizeStepper, sender);
    } else if (sender == _styleOutlineField || sender == _styleOutlineStepper) {
        wanted.outline_width =
            stepped(_styleOutlineField, _styleOutlineStepper, sender);
    } else if (sender == _styleShadowField || sender == _styleShadowStepper) {
        wanted.shadow_offset =
            stepped(_styleShadowField, _styleShadowStepper, sender);
    } else if (sender == _stylePaddingField || sender == _stylePaddingStepper) {
        wanted.background_padding =
            stepped(_stylePaddingField, _stylePaddingStepper, sender);
    } else if (sender == _styleBoxCheck) {
        wanted = ui::media_panel::WithCaptionBackground(
            wanted, _styleBoxCheck.state == NSControlStateValueOn);
    } else if ([sender isKindOfClass:NSColorWell.class]) {
        std::string* target =
            sender == _styleColorWell     ? &wanted.color
            : sender == _styleOutlineWell ? &wanted.outline_color
            : sender == _styleShadowWell  ? &wanted.shadow_color
                                          : &wanted.background_color;
        if (sender == _styleBoxWell)
            wanted = ui::media_panel::WithCaptionBackground(wanted, true);
        uint8_t red = 0;
        uint8_t green = 0;
        uint8_t blue = 0;
        if (CMChosenCaptionChannels([(NSColorWell*)sender color], *target, red,
                                    green, blue))
            *target = ui::media_panel::CaptionColorFromWell(*target, red, green,
                                                            blue);
    } else {
        return;
    }
    DocumentSequence sequence;
    sequence.caption_styles = {*_style};
    std::string refusal;
    const std::optional<UpdateCaptionStyleOperation> update =
        ui::media_panel::RestyleCaptionStyleWhole(sequence, wanted, refusal);
    if (!update) {
        if (!refusal.empty()) {
            _styleNote.textColor = CMThemeColor(ui::theme::kError);
            _styleNote.stringValue = [NSString
                stringWithFormat:@"Réglage refusé : %@.",
                                 [NSString stringWithUTF8String:refusal.c_str()]
                                     ?: @""];
        }
        [self cmShowCaptionStyle];
        return;
    }
    if ([self.delegate respondsToSelector:@selector(inspectorView:
                                              didCommitCaptionStyle:)])
        [self.delegate inspectorView:self didCommitCaptionStyle:*update];
}

#pragma mark - GitHub #330 sound

// The clip the section sets, as the document holds it now. The section
// takes no room without one; a refusal still on screen stays while the
// target and its settings do, and goes when either changes -- a handle
// dragged on the timeline, an agent's set_clip_audio, an undo.
- (void)cmReloadSoundFrom:(const Document&)document {
    const Ulid previous = _soundTarget ? _soundTarget->clip_id : Ulid{};
    const DocumentClip before = _soundClip;
    _soundTarget = ui::inspector::InspectorSoundTarget(document, _clipId);
    const DocumentClip* sound =
        _soundTarget ? document.FindClip(_soundTarget->clip_id) : nullptr;
    if (sound == nullptr) _soundTarget.reset();
    if (sound != nullptr) _soundClip = *sound;
    _soundRate = document.sequence.frame_rate;
    const BOOL shown = _soundTarget.has_value();
    const bool settingsMoved =
        shown && (!(before.audio_fade_in == _soundClip.audio_fade_in) ||
                  !(before.audio_fade_out == _soundClip.audio_fade_out) ||
                  static_cast<int64_t>(before.audio_gain_db.num) *
                          _soundClip.audio_gain_db.den !=
                      static_cast<int64_t>(_soundClip.audio_gain_db.num) *
                          before.audio_gain_db.den);
    if (!shown || _soundTarget->clip_id != previous || settingsMoved)
        _soundNote.stringValue = @"";
    // The sound of a linked picture says whose: « — SON · A1 ».
    NSString* title = @"Son";
    if (shown && _soundTarget->linked) {
        const std::string track =
            TrackDisplayName(document, _soundTarget->track_id);
        title = [NSString
            stringWithFormat:@"Son · %@",
                             [NSString stringWithUTF8String:track.c_str()]
                                 ?: @""];
    }
    _soundHeader.attributedStringValue =
        CMMakeSectionHeader(title).attributedStringValue;
    BOOL moved = NO;
    for (NSView* view in @[
             _soundHeader, _gainRow, _fadeInRow, _fadeOutRow, _soundSpacer,
             _soundNote
         ]) {
        const BOOL takesRoom =
            shown && (view != _soundNote || _soundNote.stringValue.length > 0);
        if (takesRoom == ![_collapsed containsObject:view]) continue;
        moved = YES;
        if (takesRoom)
            [_collapsed removeObject:view];
        else
            [_collapsed addObject:view];
        view.hidden = !takesRoom;
    }
    [self cmShowSound];
    if (moved) [self cmLayoutForSize:self.bounds.size];
}

// The controls as _soundClip has them. A field being typed in keeps its
// text while the value under it has not changed, so that a reload from
// elsewhere -- playback, an agent's edit of another clip -- does not take
// away what is being typed.
- (void)cmShowSound {
    const BOOL editable = _soundTarget.has_value() && !_soundTarget->locked;
    for (NSControl* control : @[
             _gainSlider, _gainField, _fadeInField, _fadeInStepper,
             _fadeOutField, _fadeOutStepper
         ])
        control.enabled = editable;
    if (!_soundTarget) return;
    const auto show = [](NSTextField* field, const std::string& text,
                         std::string& shown) {
        if (field.currentEditor != nil) {
            if (text == shown) return;
            // The document changed under the hand: it wins.
            [field abortEditing];
        }
        shown = text;
        field.stringValue = [NSString stringWithUTF8String:text.c_str()] ?: @"";
    };
    _gainSlider.floatValue =
        ui::inspector::CurrentClipGain(_soundClip.audio_gain_db);
    show(_gainField, ui::inspector::ClipGainText(_soundClip.audio_gain_db),
         _soundShown[0]);
    show(_fadeInField,
         ui::inspector::FadeLengthText(_soundClip.audio_fade_in, _soundRate),
         _soundShown[1]);
    show(_fadeOutField,
         ui::inspector::FadeLengthText(_soundClip.audio_fade_out, _soundRate),
         _soundShown[2]);
    const struct {
        NSStepper* stepper;
        bool fadeIn;
        RationalTime fade;
    } steps[] = {{_fadeInStepper, true, _soundClip.audio_fade_in},
                 {_fadeOutStepper, false, _soundClip.audio_fade_out}};
    for (const auto& step : steps) {
        step.stepper.maxValue = static_cast<double>(
            ui::inspector::FadeRoomFrames(_soundClip, step.fadeIn, _soundRate));
        step.stepper.integerValue = static_cast<NSInteger>(
            ui::inspector::FadeFramesOf(step.fade, _soundRate, true));
    }
}

// As the carton style's number rows: the field in the value column, the
// stepper at the head of the control column; the slider spans it.
- (void)cmPlaceSoundControls {
    if (_gainRow == nil) return;
    const auto centred = [](NSView* view, CGFloat x, CGFloat width,
                            CGFloat height) {
        const CGFloat rowHeight = view.superview.bounds.size.height;
        view.frame = NSMakeRect(x, std::floor((rowHeight - height) / 2.0),
                                std::max<CGFloat>(0.0, width), height);
    };
    const struct {
        CMControlRowView* owner;
        NSTextField* field;
        NSStepper* stepper;
    } rows[] = {{_gainRow, _gainField, nil},
                {_fadeInRow, _fadeInField, _fadeInStepper},
                {_fadeOutRow, _fadeOutField, _fadeOutStepper}};
    for (const auto& row : rows) {
        const NSRect value = row.owner.valueLabel.frame;
        centred(row.field, NSMinX(value), NSWidth(value), 20.0);
        if (row.stepper) centred(row.stepper, 0.0, 15.0, 22.0);
    }
}

- (void)cmGainSliderChanged:(CMTrackedSlider*)sender {
    if (!_soundTarget) return;
    const EffectParamValue gain =
        ui::inspector::QuantizeClipGain(sender.floatValue);
    _gainField.stringValue = [NSString
        stringWithUTF8String:ui::inspector::ClipGainText(gain).c_str()];
    // A tick of a drag: written at the release.
    if (sender.tracking) return;
    [self cmCommitSound:ui::inspector::WithClipGain(_soundClip, gain)];
}

- (void)cmResetGain:(id)sender {
    (void)sender;
    if (!_soundTarget) return;
    [self cmCommitSound:ui::inspector::WithClipGain(_soundClip, {0, 1})];
}

- (void)cmGainTyped:(NSTextField*)sender {
    if (!_soundTarget) return;
    EffectParamValue gain;
    std::string refusal;
    if (!ui::inspector::ParseClipGain(sender.stringValue.UTF8String ?: "", gain,
                                      refusal)) {
        [self cmRefuseSound:refusal];
        return;
    }
    [self cmCommitSound:ui::inspector::WithClipGain(_soundClip, gain)];
}

- (void)cmFadeTyped:(NSTextField*)sender {
    if (!_soundTarget) return;
    const bool fadeIn = sender == _fadeInField;
    RationalTime fade;
    std::string refusal;
    if (!ui::inspector::ParseFadeLength(sender.stringValue.UTF8String ?: "",
                                        _soundRate, fade, refusal)) {
        [self cmRefuseSound:refusal];
        return;
    }
    const std::optional<SetClipAudioOperation> operation =
        ui::inspector::WithClipFade(_soundClip, fadeIn, fade, _soundRate,
                                    refusal);
    if (!operation) {
        [self cmRefuseSound:refusal];
        return;
    }
    [self cmCommitSound:*operation];
}

- (void)cmFadeStepped:(NSStepper*)sender {
    if (!_soundTarget) return;
    const bool fadeIn = sender == _fadeInStepper;
    std::string refusal;
    const std::optional<SetClipAudioOperation> operation =
        ui::inspector::WithClipFade(
            _soundClip, fadeIn,
            ui::inspector::FadeOfFrames(sender.integerValue, _soundRate),
            _soundRate, refusal);
    if (!operation) {
        [self cmRefuseSound:refusal];
        return;
    }
    [self cmCommitSound:*operation];
}

// A gesture that changes nothing writes nothing: a field left as it was, a
// value typed as the document already holds it. Anything else goes to the
// delegate, whose refresh rewrites the controls.
- (void)cmCommitSound:(const SetClipAudioOperation&)operation {
    const bool sameGain = static_cast<int64_t>(operation.gain_db.num) *
                              _soundClip.audio_gain_db.den ==
                          static_cast<int64_t>(_soundClip.audio_gain_db.num) *
                              operation.gain_db.den;
    if (_soundNote.stringValue.length > 0) {
        _soundNote.stringValue = @"";
        [_collapsed addObject:_soundNote];
        _soundNote.hidden = YES;
        [self cmLayoutForSize:self.bounds.size];
    }
    if (sameGain && operation.fade_in == _soundClip.audio_fade_in &&
        operation.fade_out == _soundClip.audio_fade_out) {
        [self cmShowSound];
        return;
    }
    if ([self.delegate respondsToSelector:@selector(inspectorView:
                                               didCommitClipAudio:)])
        [self.delegate inspectorView:self didCommitClipAudio:operation];
}

// Said on its own line, under the fades; the controls go back to the
// document.
- (void)cmRefuseSound:(const std::string&)refusal {
    _soundNote.stringValue = [NSString
        stringWithFormat:@"Réglage refusé : %@.",
                         [NSString stringWithUTF8String:refusal.c_str()]
                             ?: @""];
    if ([_collapsed containsObject:_soundNote]) {
        [_collapsed removeObject:_soundNote];
        _soundNote.hidden = NO;
        [self cmLayoutForSize:self.bounds.size];
    }
    for (NSTextField* field : @[ _gainField, _fadeInField, _fadeOutField ])
        [field abortEditing];
    [self cmShowSound];
}

#pragma mark - D7 carton text

// D7 -- docs/roadmap/DIALOGUE-2026-10.md. The section shows for a carton
// and takes no room otherwise.
- (void)cmShowCaptionSection:(BOOL)show {
    if (show == _isCarton) return;
    // Hidden under the keyboard, the field would keep it; and nothing typed
    // there is shown any longer.
    if (!show) {
        if (_captionField.currentEditor) [_captionField abortEditing];
        [self cmEndCaptionPreview];
    }
    _isCarton = show;
    for (const auto& item : _captionItems) item.first.hidden = !show;
    for (const auto& item : _styleItems) item.first.hidden = !show;
    [self cmLayoutForSize:self.bounds.size];
}

// The monitor shows the document's words again.
- (void)cmEndCaptionPreview {
    if ([self.delegate respondsToSelector:@selector(inspectorView:
                                                previewCartonText:forClip:)])
        [self.delegate inspectorView:self
                   previewCartonText:std::string()
                             forClip:Ulid{}];
}

// Each keystroke: the monitor shows the carton with what the field holds.
- (void)controlTextDidChange:(NSNotification*)notification {
    if (notification.object != _captionField || !_isCarton || _clipId.empty() ||
        ![self.delegate respondsToSelector:@selector(inspectorView:
                                                 previewCartonText:forClip:)])
        return;
    [self.delegate inspectorView:self
               previewCartonText:(_captionField.stringValue.UTF8String
                                      ?: "")forClip:_clipId];
}

- (NSString*)cmLoadedCaptionText {
    return [NSString stringWithUTF8String:_captionLoadedText.c_str()] ?: @"";
}

// What the field holds, handed to the delegate. YES when the carton now
// holds it: the delegate's reload has then written the document's text
// back, tidied. Refused or unchanged -- an empty text, the same words, a
// locked track -- the field goes back to the document's text.
- (BOOL)cmCommitCaptionText {
    [self cmEndCaptionPreview];
    if (!_isCarton || _clipId.empty()) return NO;
    const std::string typed = _captionField.stringValue.UTF8String ?: "";
    if (typed == _captionLoadedText) return NO;
    // Copied: the delegate's reload may turn the panel to another clip.
    const Ulid clipId = _clipId;
    BOOL applied = NO;
    if ([self.delegate respondsToSelector:@selector(inspectorView:
                                              didCommitCartonText:forClip:)])
        applied = [self.delegate inspectorView:self
                           didCommitCartonText:typed
                                       forClip:clipId];
    if (!applied && clipId == _clipId)
        _captionField.stringValue = [self cmLoadedCaptionText];
    return applied;
}

- (void)cmCaptionTextCommitted:(id)sender {
    (void)sender;
    [self cmCommitCaptionText];
}

- (BOOL)cmGoToAdjacentCarton:(BOOL)forward {
    if (!_isCarton || _clipId.empty() ||
        ![self.delegate respondsToSelector:@selector
                        (inspectorView:selectCartonAdjacentTo:forward:)])
        return NO;
    const Ulid from = _clipId;
    return [self.delegate inspectorView:self
                 selectCartonAdjacentTo:from
                                forward:forward];
}

// A button pressed while the field is being typed in commits it first, and
// leaves the keyboard in the field of the carton arrived at.
- (void)cmWalkCartons:(BOOL)forward {
    const BOOL typing = _captionField.currentEditor != nil;
    [self cmCommitCaptionText];
    [self cmGoToAdjacentCarton:forward];
    if (typing) [self focusCaptionTextForClip:_clipId];
}

- (void)cmPreviousCarton:(id)sender {
    (void)sender;
    [self cmWalkCartons:NO];
}

- (void)cmNextCarton:(id)sender {
    (void)sender;
    [self cmWalkCartons:YES];
}

- (BOOL)focusCaptionTextForClip:(const Ulid&)clipId {
    if (!_isCarton || clipId.empty() || clipId != _clipId ||
        !_captionField.editable || !self.window ||
        self.hiddenOrHasHiddenAncestor)
        return NO;
    // The section is the panel's first: scrolled down to the grade, the
    // panel comes back to it.
    [_scrollView.contentView scrollToPoint:NSZeroPoint];
    [_scrollView reflectScrolledClipView:_scrollView.contentView];
    [_captionField selectText:nil];
    return _captionField.currentEditor != nil;
}

// Return breaks the line, as in the editor the timelines go to: a carton
// is two lines more often than one, and the break is chosen on the picture.
// Leaving the field commits, through its action. Tab and Shift+Tab commit
// and walk to the neighbour carton, the keyboard staying in the field: a
// pass over every carton without the mouse. Escape gives up what was typed
// and hands the keys back.
- (BOOL)control:(NSControl*)control
               textView:(NSTextView*)textView
    doCommandBySelector:(SEL)command {
    if (control != _captionField) return NO;
    if (command == @selector(insertNewline:)) {
        [textView insertNewlineIgnoringFieldEditor:nil];
        return YES;
    }
    if (command == @selector(insertTab:) || command == @selector
                                                (insertBacktab:)) {
        [self cmCommitCaptionText];
        [self cmGoToAdjacentCarton:command == @selector(insertTab:)];
        [self focusCaptionTextForClip:_clipId];
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        [self cmEndCaptionPreview];
        [_captionField abortEditing];
        _captionField.stringValue = [self cmLoadedCaptionText];
        if ([self.delegate
                respondsToSelector:@selector(inspectorViewDidLeaveCartonText:)])
            [self.delegate inspectorViewDidLeaveCartonText:self];
        return YES;
    }
    return NO;
}

#pragma mark - Grade

- (void)cmGradeSliderChanged:(NSSlider*)sender {
    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    const NSInteger index = sender.tag;
    if (index < 0 || static_cast<size_t>(index) >= controls.size()) return;

    const ui::inspector::GradeControlSpec& spec =
        controls[static_cast<size_t>(index)];
    const float value = sender.floatValue;
    // The number the user sees updates on every drag tick; only the
    // committed operation is debounced (see below).
    _gradeValueLabels[index].stringValue = CMFormatGradeValue(spec, value);
    if (!_clipId.empty() && _gradable &&
        ui::inspector::IsGradeControlEditable(_effects, spec) &&
        [self.delegate respondsToSelector:@selector(inspectorView:
                                               previewClipEffects:forClip:)])
        [self.delegate inspectorView:self
                  previewClipEffects:ui::inspector::WithGradeControlValue(
                                         _effects, spec, value)
                             forClip:_clipId];
    [self cmScheduleGradeCommitAtIndex:index value:value];
}

- (void)cmOpacitySliderChanged:(NSSlider*)sender {
    const float value = sender.floatValue;
    _opacityValueLabel.stringValue =
        [NSString stringWithFormat:@"%.0f %%", value * 100.0f];
    [_opacityDebounceTimer invalidate];
    __weak CMInspectorView* weakSelf = self;
    _opacityDebounceTimer =
        [NSTimer scheduledTimerWithTimeInterval:kGradeCommitDebounceSeconds
                                        repeats:NO
                                          block:^(NSTimer* _Nonnull timer) {
                                            (void)timer;
                                            [weakSelf cmCommitOpacity:value];
                                          }];
}

- (void)cmCommitOpacity:(float)value {
    _opacityDebounceTimer = nil;
    if (_clipId.empty() || !self.delegate) return;
    // R2c -- a keyed opacity keys the playhead's frame instead.
    if (_hasKeyedClip &&
        ClipAnimates(_keyedClip, ClipAnimatedParameter::Opacity)) {
        [self cmCommitKeyedIndex:0 slider:value];
        return;
    }
    _opacity = ui::inspector::QuantizeClipOpacity(value);
    [self.delegate inspectorView:self
            didCommitClipOpacity:SetClipOpacityOperation{_clipId, _opacity}];
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A blur committed as the opacity is:
// once the hand rests, one operation, one undo.
- (void)cmBlurSliderChanged:(NSSlider*)sender {
    const float value = sender.floatValue;
    _blurValueLabel.stringValue = [NSString stringWithFormat:@"%.1f px", value];
    [_blurDebounceTimer invalidate];
    __weak CMInspectorView* weakSelf = self;
    _blurDebounceTimer =
        [NSTimer scheduledTimerWithTimeInterval:kGradeCommitDebounceSeconds
                                        repeats:NO
                                          block:^(NSTimer* _Nonnull timer) {
                                            (void)timer;
                                            [weakSelf cmCommitBlur:value];
                                          }];
}

- (void)cmCommitBlur:(float)value {
    _blurDebounceTimer = nil;
    if (_clipId.empty() || !self.delegate) return;
    _blur = ui::inspector::QuantizeClipBlur(value);
    [self.delegate inspectorView:self
               didCommitClipBlur:SetClipBlurOperation{_clipId, _blur}];
}

- (void)cmScheduleGradeCommitAtIndex:(NSInteger)index value:(float)value {
    id previous = _gradeDebounceTimers[static_cast<NSUInteger>(index)];
    if ([previous isKindOfClass:[NSTimer class]])
        [(NSTimer*)previous invalidate];

    __weak CMInspectorView* weakSelf = self;
    NSTimer* timer = [NSTimer
        scheduledTimerWithTimeInterval:kGradeCommitDebounceSeconds
                               repeats:NO
                                 block:^(NSTimer* _Nonnull firedTimer) {
                                   (void)firedTimer;
                                   [weakSelf cmCommitGradeAtIndex:index
                                                            value:value];
                                 }];
    _gradeDebounceTimers[static_cast<NSUInteger>(index)] = timer;
}

- (void)cmCommitGradeAtIndex:(NSInteger)index value:(float)value {
    _gradeDebounceTimers[static_cast<NSUInteger>(index)] = NSNull.null;
    if (_clipId.empty() || !self.delegate) return;

    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    if (index < 0 || static_cast<size_t>(index) >= controls.size()) return;

    const ui::inspector::GradeControlSpec& spec =
        controls[static_cast<size_t>(index)];
    if (!_gradable || !ui::inspector::IsGradeControlEditable(_effects, spec)) {
        return;
    }
    // Committed from the exact fraction QuantizeGradeControlValue produces
    // -- never the raw slider float -- so what lands in the document is
    // the same num/den boundary crossing ColorEffects.h documents
    // (PHILOSOPHY.md principle 4).
    _effects = ui::inspector::WithGradeControlValue(_effects, spec, value);

    SetClipEffectsOperation operation;
    operation.clip_id = _clipId;
    operation.effects = _effects;
    [self.delegate inspectorView:self
            didCommitClipEffects:std::move(operation)];
}

- (void)cmResetGradeSlider:(NSSlider*)sender {
    const std::vector<ui::inspector::GradeControlSpec>& controls =
        ui::inspector::GradeControls();
    const NSInteger index = sender.tag;
    if (index < 0 || static_cast<size_t>(index) >= controls.size()) return;
    const ui::inspector::GradeControlSpec& spec =
        controls[static_cast<size_t>(index)];
    id pending = _gradeDebounceTimers[static_cast<NSUInteger>(index)];
    if ([pending isKindOfClass:[NSTimer class]]) [(NSTimer*)pending invalidate];
    const float neutral =
        ui::inspector::NeutralGradeControlValue(spec, _neutralKelvin);
    sender.floatValue = neutral;
    _gradeValueLabels[index].stringValue = CMFormatGradeValue(spec, neutral);
    [self cmCommitGradeAtIndex:index value:neutral];
}

#pragma mark - Framing

// Every tick moves the number and the monitor; the edit waits for the
// release, so one drag is one undo.
- (void)cmFramingSliderChanged:(CMTrackedSlider*)sender {
    const NSInteger index = sender.tag;
    if (index < 0 || index > 2 || _clipId.empty() || !_framable) return;
    const auto control = static_cast<ui::inspector::FramingControl>(index);
    // R2c -- a keyed row moves the key of the playhead's frame: the monitor
    // shows the keys it would commit, the release commits them.
    if (_hasKeyedClip &&
        ui::inspector::IsFramingControlAnimated(_keyedClip, control)) {
        _framingRows[index].valueLabel.stringValue =
            [self cmFramingText:control value:sender.floatValue];
        if (!sender.tracking) {
            [self cmCommitKeyedIndex:static_cast<NSUInteger>(index) + 1
                              slider:sender.floatValue];
            return;
        }
        const ui::inspector::KeyedControl keyed =
            ui::inspector::KeyedControlFor(control);
        std::vector<ClipParameterAnimation> animation;
        std::string error;
        if (ui::inspector::KeyedValueAnimation(
                _keyedClip, keyed,
                ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate),
                _rate,
                ui::inspector::KeyedValueFromSlider(
                    keyed, sender.floatValue, _sequenceWidth, _sequenceHeight),
                GenerateUlid(), animation, error) &&
            [self.delegate respondsToSelector:@selector
                           (inspectorView:previewClipAnimation:forClip:)])
            [self.delegate inspectorView:self
                    previewClipAnimation:animation
                                 forClip:_clipId];
        return;
    }
    const ClipFraming framing = ui::inspector::WithFramingControl(
        _framing, control, sender.floatValue, _sequenceWidth, _sequenceHeight);
    _framingRows[index].valueLabel.stringValue =
        [self cmFramingText:control value:sender.floatValue];
    if (sender.tracking) {
        if ([self.delegate respondsToSelector:@selector
                           (inspectorView:previewClipFraming:forClip:)])
            [self.delegate inspectorView:self
                      previewClipFraming:framing
                                 forClip:_clipId];
        return;
    }
    [self cmCommitFraming:framing];
}

- (void)cmResetFramingSlider:(NSSlider*)sender {
    const NSInteger index = sender.tag;
    if (index < 0 || index > 2 || _clipId.empty() || !_framable) return;
    const auto control = static_cast<ui::inspector::FramingControl>(index);
    const float neutral = ui::inspector::NeutralFramingSliderValue(control);
    sender.floatValue = neutral;
    if (_hasKeyedClip &&
        ui::inspector::IsFramingControlAnimated(_keyedClip, control)) {
        [self cmCommitKeyedIndex:static_cast<NSUInteger>(index) + 1
                          slider:neutral];
        return;
    }
    [self cmCommitFraming:ui::inspector::WithFramingControl(
                              _framing, control, neutral, _sequenceWidth,
                              _sequenceHeight)];
}

- (void)cmFramingModeChanged:(NSPopUpButton*)sender {
    if (_clipId.empty() || !_framable) return;
    ClipFraming framing = _framing;
    framing.mode = static_cast<FramingMode>(sender.selectedItem.tag);
    [self cmCommitFraming:ReduceClipFraming(framing)];
}

- (void)cmCommitFraming:(const ClipFraming&)framing {
    if (_clipId.empty() ||
        ![self.delegate respondsToSelector:@selector(inspectorView:
                                               didCommitClipFraming:)])
        return;
    // Nothing moved: no step in the history for a click that let go where
    // it took hold.
    if (ui::inspector::SameFraming(framing, _framing)) {
        [self cmShowFraming];
        return;
    }
    SetClipFramingOperation operation;
    operation.clips.push_back({_clipId, framing});
    [self.delegate inspectorView:self
            didCommitClipFraming:std::move(operation)];
}

#pragma mark - R2c keys

// The diamond and the arrows at the end of each keyed row, the slider
// before them. The arrows take room only while the parameter has keys: at
// the panel's default width the slider has 96 points to share.
- (void)cmPlaceKeyButtons {
    constexpr CGFloat kKeyWidth = 20.0;
    constexpr CGFloat kArrowWidth = 14.0;
    for (NSUInteger index = 0; index < _keyButtons.count; ++index) {
        NSSlider* slider =
            index == 0 ? _opacitySlider : (NSSlider*)_framingSliders[index - 1];
        const NSSize size = _keyButtons[index].superview.bounds.size;
        const BOOL arrows = _keyedAnimated[index];
        const CGFloat buttons = kKeyWidth + (arrows ? 2 * kArrowWidth : 0.0);
        slider.frame = NSMakeRect(
            0.0, 0.0, std::max<CGFloat>(0.0, size.width - buttons - 2.0),
            size.height);
        CGFloat x = size.width - buttons;
        _previousKeyButtons[index].frame =
            NSMakeRect(x, 0.0, arrows ? kArrowWidth : 0.0, size.height);
        x += arrows ? kArrowWidth : 0.0;
        _keyButtons[index].frame = NSMakeRect(x, 0.0, kKeyWidth, size.height);
        x += kKeyWidth;
        _nextKeyButtons[index].frame =
            NSMakeRect(x, 0.0, arrows ? kArrowWidth : 0.0, size.height);
        _previousKeyButtons[index].hidden = !arrows;
        _nextKeyButtons[index].hidden = !arrows;
    }
}

- (void)showPlayheadAt:(RationalTime)position {
    _playhead = position;
    if (_hasKeyedClip && _keyedFrameShown == ui::inspector::ClipLocalFrame(
                                                 _keyedClip, _playhead, _rate))
        return;
    [self cmShowKeyedRows];
}

// Each keyed row as InspectorKeys.h reads it on the playhead's frame: its
// diamond, its arrows, and for a keyed parameter the value of that frame,
// unless the hand holds the slider or its commit still waits.
- (void)cmShowKeyedRows {
    BOOL relayout = NO;
    _keyedFrameShown.reset();
    if (_hasKeyedClip)
        _keyedFrameShown =
            ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate);
    for (NSUInteger index = 0; index < _keyButtons.count; ++index) {
        const ui::inspector::KeyedControl control =
            ui::inspector::kKeyedControls[index];
        NSButton* key = _keyButtons[index];
        if (!_hasKeyedClip) {
            key.enabled = NO;
            key.state = NSControlStateValueOff;
            key.title = @"◇";
            relayout = relayout || _keyedAnimated[index];
            _keyedAnimated[index] = NO;
            continue;
        }
        const BOOL editable = control == ui::inspector::KeyedControl::Opacity
                                  ? _opacityEditable
                                  : _framable;
        const ui::inspector::KeyedRowState state =
            ui::inspector::KeyedRowStateAt(
                _keyedClip, control, editable,
                ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate),
                _rate, _sequenceWidth, _sequenceHeight);
        key.enabled = state.keyable;
        key.state =
            state.key_here ? NSControlStateValueOn : NSControlStateValueOff;
        NSString* title = state.key_here ? @"◆" : @"◇";
        if (![key.title isEqualToString:title]) key.title = title;
        key.toolTip = state.key_here ? @"Retirer la clé à cette image"
                                     : @"Poser une clé à cette image";
        NSString* name = (index == 0 ? _opacityRow : _framingRows[index - 1])
                             .label.stringValue;
        key.accessibilityLabel =
            [NSString stringWithFormat:@"%@ : %@", name, key.toolTip];
        _previousKeyButtons[index].enabled =
            editable && state.previous_frame.has_value();
        _nextKeyButtons[index].enabled =
            editable && state.next_frame.has_value();
        if (_keyedAnimated[index] != state.animated) {
            _keyedAnimated[index] = state.animated;
            relayout = YES;
        }
        if (!state.animated) continue;
        if (index == 0) {
            if (_opacityDebounceTimer != nil) continue;
            _opacitySlider.enabled = state.keyable;
            _opacitySlider.floatValue = state.slider_value;
            _opacityValueLabel.stringValue = [NSString
                stringWithFormat:@"%.0f %%", state.slider_value * 100.0f];
        } else {
            CMFramingSlider* slider = _framingSliders[index - 1];
            if (slider.tracking) continue;
            slider.enabled = state.keyable;
            slider.floatValue = state.slider_value;
            _framingRows[index - 1].valueLabel.stringValue =
                [self cmFramingText:ui::inspector::FramingControlFor(control)
                              value:state.slider_value];
        }
    }
    if (relayout) [self cmPlaceKeyButtons];
}

// A keyed row's slider let go: the key of the playhead's frame takes the
// value, or one is added there. A click that let go where it took hold
// commits nothing, as on an unkeyed row.
- (void)cmCommitKeyedIndex:(NSUInteger)index slider:(float)value {
    if (!_hasKeyedClip || _clipId.empty() || index >= _keyButtons.count ||
        ![self.delegate respondsToSelector:@selector(inspectorView:
                                                 didCommitClipKeys:label:)]) {
        [self cmShowKeyedRows];
        return;
    }
    const ui::inspector::KeyedControl control =
        ui::inspector::kKeyedControls[index];
    const int64_t frame =
        ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate);
    const ui::inspector::KeyedRowState state =
        ui::inspector::KeyedRowStateAt(_keyedClip, control, YES, frame, _rate,
                                       _sequenceWidth, _sequenceHeight);
    const float tolerance =
        control == ui::inspector::KeyedControl::Opacity ? 0.0005f
        : control == ui::inspector::KeyedControl::Scale ? 0.0001f
                                                        : 0.5f;
    std::vector<ClipParameterAnimation> animation;
    std::string error;
    if (!state.keyable || std::abs(value - state.slider_value) < tolerance ||
        !ui::inspector::KeyedValueAnimation(
            _keyedClip, control, frame, _rate,
            ui::inspector::KeyedValueFromSlider(control, value, _sequenceWidth,
                                                _sequenceHeight),
            GenerateUlid(), animation, error)) {
        [self cmShowKeyedRows];
        return;
    }
    SetClipAnimationOperation operation;
    operation.clips.push_back({_clipId, std::move(animation)});
    [self.delegate inspectorView:self
               didCommitClipKeys:Operation{std::move(operation)}
                           label:@"Clé réglée"];
}

- (void)cmToggleKey:(NSButton*)sender {
    const NSUInteger index = static_cast<NSUInteger>(sender.tag);
    if (!_hasKeyedClip || _clipId.empty() || index >= _keyButtons.count ||
        ![self.delegate respondsToSelector:@selector(inspectorView:
                                                 didCommitClipKeys:label:)]) {
        [self cmShowKeyedRows];
        return;
    }
    // An opacity still waiting for its commit lands first: the key holds
    // the value the slider shows.
    if (index == 0 && _opacityDebounceTimer != nil)
        [_opacityDebounceTimer fire];
    Operation operation;
    bool posed = false;
    std::string error;
    if (!ui::inspector::KeyToggleOperation(
            _keyedClip, ui::inspector::kKeyedControls[index],
            ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate), _rate,
            GenerateUlid(), operation, posed, error)) {
        [self cmShowKeyedRows];
        return;
    }
    [self.delegate inspectorView:self
               didCommitClipKeys:std::move(operation)
                           label:posed ? @"Clé posée" : @"Clé retirée"];
}

- (void)cmPreviousKey:(NSButton*)sender {
    const NSUInteger index = static_cast<NSUInteger>(sender.tag);
    if (!_hasKeyedClip || index >= _keyButtons.count) return;
    const std::optional<int64_t> frame = PreviousClipKeyframeFrame(
        _keyedClip,
        ui::inspector::KeyedParameter(ui::inspector::kKeyedControls[index]),
        ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate), _rate);
    if (frame && [self.delegate respondsToSelector:@selector
                                (inspectorView:moveToFrame:ofClip:)])
        [self.delegate inspectorView:self moveToFrame:*frame ofClip:_clipId];
}

- (void)cmNextKey:(NSButton*)sender {
    const NSUInteger index = static_cast<NSUInteger>(sender.tag);
    if (!_hasKeyedClip || index >= _keyButtons.count) return;
    const std::optional<int64_t> frame = NextClipKeyframeFrame(
        _keyedClip,
        ui::inspector::KeyedParameter(ui::inspector::kKeyedControls[index]),
        ui::inspector::ClipLocalFrame(_keyedClip, _playhead, _rate), _rate);
    if (frame && [self.delegate respondsToSelector:@selector
                                (inspectorView:moveToFrame:ofClip:)])
        [self.delegate inspectorView:self moveToFrame:*frame ofClip:_clipId];
}

- (void)cmResetGrade:(id)sender {
    (void)sender;
    [self cmInvalidateAllGradeTimers];
    if (_clipId.empty() || !self.delegate || !_gradable) return;
    _effects = ui::inspector::WithoutGrade(_effects);
    SetClipEffectsOperation operation;
    operation.clip_id = _clipId;
    operation.effects = _effects;
    [self.delegate inspectorView:self
            didCommitClipEffects:std::move(operation)];
}

- (void)cmInvalidateAllGradeTimers {
    [_opacityDebounceTimer invalidate];
    _opacityDebounceTimer = nil;
    [_blurDebounceTimer invalidate];
    _blurDebounceTimer = nil;
    for (NSUInteger index = 0; index < _gradeDebounceTimers.count; ++index) {
        id timer = _gradeDebounceTimers[index];
        if ([timer isKindOfClass:[NSTimer class]]) [(NSTimer*)timer invalidate];
        _gradeDebounceTimers[index] = NSNull.null;
    }
}

@end
