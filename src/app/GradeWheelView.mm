#import "GradeWheelView.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "UiTheme.h"
#include "UiThemeAppKit.h"

#import "UiComponents.h"

namespace {

using ui::inspector::GradeRect;
using ui::inspector::GradeWheelLayout;
using ui::inspector::GradeWheelSpec;
using ui::inspector::GradeWheelState;

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSRect CMRect(const GradeRect& rect) {
    return NSMakeRect(CGF(rect.x), CGF(rect.y), CGF(rect.width),
                      CGF(rect.height));
}

// What a press in the wheel started.
enum class WheelDrag {
    None,
    // In the ring: the puck follows the pointer's travel, geared down.
    Relative,
    // In the ring, with Shift: the puck is under the pointer.
    Absolute,
    // In the ring with Command, or on the master bar: the master knob.
    Master,
};

// A disabled wheel is still read, not grabbed.
constexpr CGFloat kDisabledAlpha = 0.35;

// The ring and the disc it bounds, for a diameter in points at a backing
// scale: the rim painted with what the puck pushes toward at each angle
// (WheelRimColor), the disc with a trace of it that fades to the centre,
// where nothing is pushed. Computed once per size; the colours are the
// model's, none is written here.
NSImage* CMWheelRingImage(CGFloat diameter, CGFloat scale) {
    static NSMutableDictionary<NSString*, NSImage*>* cache =
        [NSMutableDictionary dictionary];
    NSString* key = [NSString stringWithFormat:@"%.1f@%.1f", diameter, scale];
    NSImage* cached = cache[key];
    if (cached != nil) return cached;

    const NSInteger pixels =
        std::max<NSInteger>(1, std::lround(diameter * scale));
    NSBitmapImageRep* rep =
        [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                pixelsWide:pixels
                                                pixelsHigh:pixels
                                             bitsPerSample:8
                                           samplesPerPixel:4
                                                  hasAlpha:YES
                                                  isPlanar:NO
                                            colorSpaceName:NSDeviceRGBColorSpace
                                               bytesPerRow:pixels * 4
                                              bitsPerPixel:32];
    if (rep == nil) return nil;
    const ui::theme::Color disc = ui::theme::kSurfaceSunken;
    const double outer = diameter / 2.0;
    const double inner = outer - ui::theme::kGradeWheelRingWidth;
    const double centre = static_cast<double>(pixels) / 2.0;
    unsigned char* data = rep.bitmapData;
    if (data == nullptr) return nil;
    for (NSInteger row = 0; row < pixels; ++row) {
        for (NSInteger column = 0; column < pixels; ++column) {
            // Row 0 is the top row; the wheel's y points up.
            const double dx = (column + 0.5 - centre) / scale;
            const double dy = (centre - (row + 0.5)) / scale;
            const double distance = std::hypot(dx, dy);
            // Coverage of the disc's edge and of the ring's inner edge,
            // one pixel wide each.
            const double insideOuter =
                std::clamp((outer - distance) * scale + 0.5, 0.0, 1.0);
            const double outsideInner =
                std::clamp((distance - inner) * scale + 0.5, 0.0, 1.0);
            const std::array<float, 3> rim =
                ui::inspector::WheelRimColor(std::atan2(dy, dx));
            const double trace =
                inner > 0.0 ? 0.20 * std::min(1.0, distance / inner) : 0.0;
            const std::array<double, 3> surface = {
                disc.r + (rim[0] - disc.r) * trace,
                disc.g + (rim[1] - disc.g) * trace,
                disc.b + (rim[2] - disc.b) * trace};
            unsigned char* pixel = data + (row * pixels + column) * 4;
            for (int channel = 0; channel < 3; ++channel) {
                const double colour = surface[static_cast<size_t>(channel)] +
                                      (rim[static_cast<size_t>(channel)] -
                                       surface[static_cast<size_t>(channel)]) *
                                          outsideInner;
                // Premultiplied by the disc's coverage.
                pixel[channel] = static_cast<unsigned char>(std::lround(
                    std::clamp(colour * insideOuter, 0.0, 1.0) * 255.0));
            }
            pixel[3] =
                static_cast<unsigned char>(std::lround(insideOuter * 255.0));
        }
    }
    NSBitmapImageRep* tagged =
        [rep
            bitmapImageRepByRetaggingWithColorSpace:NSColorSpace.sRGBColorSpace]
            ?: rep;
    tagged.size = NSMakeSize(diameter, diameter);
    NSImage* image =
        [[NSImage alloc] initWithSize:NSMakeSize(diameter, diameter)];
    [image addRepresentation:tagged];
    cache[key] = image;
    return image;
}

NSColor* CMChannelColor(size_t channel) {
    switch (channel) {
        case 0:
            return CMThemeColor(ui::theme::kChannelRed);
        case 1:
            return CMThemeColor(ui::theme::kChannelGreen);
        case 2:
            return CMThemeColor(ui::theme::kChannelBlue);
        default:
            return CMThemeColor(ui::theme::kChannelMaster);
    }
}

}  // namespace

@interface CMGradeWheelView () <NSTextFieldDelegate>
@end

@implementation CMGradeWheelView {
    ui::inspector::GradeWheel _wheel;
    NSTextField* _title;
    NSButton* _reset;
    NSArray<NSTextField*>* _fields;

    WheelDrag _drag;
    NSPoint _dragOrigin;
    // The stack the gesture started from: every step of it is computed
    // from this one, so that a drag accumulates no rounding.
    std::vector<ClipEffect> _dragStart;
    // The stack the gesture is at, shown in place of the host's while it
    // lasts.
    std::vector<ClipEffect> _dragCurrent;
}

@synthesize wheel = _wheel;
@synthesize fields = _fields;
@synthesize resetButton = _reset;

- (instancetype)initWithFrame:(NSRect)frame
                        wheel:(ui::inspector::GradeWheel)wheel {
    if ((self = [super initWithFrame:frame])) {
        _wheel = wheel;
        _drag = WheelDrag::None;
        const GradeWheelSpec& spec = [self cmSpec];

        // The label is read as UTF-8: a %s in a format would not.
        NSString* label = [NSString stringWithUTF8String:spec.label] ?: @"";
        _title = [NSTextField labelWithString:label];
        _title.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
        _title.textColor = CMTextSecondaryColor();
        [self addSubview:_title];

        _reset = CMMakeStyledButton(@"↺", self, @selector(cmReset:));
        _reset.toolTip = @"Remettre la roue au neutre, couleur et maître";
        // PRD27 -- VoiceOver would read the arrow's glyph; four of them sit
        // side by side, so the label names its wheel.
        _reset.accessibilityLabel =
            [NSString stringWithFormat:@"Remettre la roue %@ au neutre", label];
        [self addSubview:_reset];

        NSMutableArray<NSTextField*>* fields = [NSMutableArray array];
        NSArray<NSString*>* names = @[ @"rouge", @"vert", @"bleu", @"maître" ];
        for (NSInteger channel = 0; channel < 4; ++channel) {
            NSTextField* field = [NSTextField textFieldWithString:@""];
            field.font = [NSFont
                monospacedDigitSystemFontOfSize:ui::theme::kFontSizeCaption
                                         weight:NSFontWeightRegular];
            field.textColor = CMTextPrimaryColor();
            field.alignment = NSTextAlignmentCenter;
            field.bezeled = NO;
            field.drawsBackground = YES;
            field.backgroundColor = CMThemeColor(ui::theme::kSurfaceSunken);
            field.focusRingType = NSFocusRingTypeNone;
            field.usesSingleLineMode = YES;
            field.tag = channel;
            field.target = self;
            field.action = @selector(cmFieldCommitted:);
            // Leaving the field commits, as Return does.
            field.cell.sendsActionOnEndEditing = YES;
            field.delegate = self;
            field.toolTip =
                [NSString stringWithFormat:@"%@ %@", label, names[channel]];
            [self addSubview:field];
            [fields addObject:field];
        }
        _fields = fields;
        [self cmLayoutSubviews];
        [self reload];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (const GradeWheelSpec&)cmSpec {
    return ui::inspector::GradeWheels().at(static_cast<size_t>(_wheel));
}

- (GradeWheelLayout)cmLayout {
    return ui::inspector::LayoutGradeWheel(self.bounds.size.width);
}

- (void)cmLayoutSubviews {
    const GradeWheelLayout layout = [self cmLayout];
    _title.frame = CMRect(layout.title);
    _reset.frame = CMRect(layout.reset);
    for (NSUInteger channel = 0; channel < _fields.count; ++channel) {
        // One point is left under each field for its channel's line.
        NSRect frame = CMRect(layout.fields.at(channel));
        frame.size.height = std::max<CGFloat>(0.0, frame.size.height - 2.0);
        _fields[channel].frame = frame;
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutSubviews];
    self.needsDisplay = YES;
}

- (BOOL)cmEditable {
    return self.host != nil && [self.host gradeEditable];
}

// The stack shown: the gesture's while one lasts, the host's otherwise.
- (std::vector<ClipEffect>)cmShownEffects {
    if (_drag != WheelDrag::None) return _dragCurrent;
    return self.host != nil ? [self.host gradeEffects]
                            : std::vector<ClipEffect>{};
}

- (void)reload {
    const BOOL editable = [self cmEditable];
    const GradeWheelSpec& spec = [self cmSpec];
    const GradeWheelState state =
        ui::inspector::ReadGradeWheel([self cmShownEffects], spec);
    for (NSUInteger channel = 0; channel < _fields.count; ++channel) {
        NSTextField* field = _fields[channel];
        field.enabled = editable;
        field.editable = editable;
        // A field being typed in keeps what is typed.
        if (field.currentEditor != nil) continue;
        field.stringValue = [self cmFormat:state.knobs.at(channel)];
    }
    _reset.enabled = editable;
    self.needsDisplay = YES;
}

- (NSString*)cmFormat:(double)knob {
    return [NSString stringWithFormat:@"%.2f", ui::inspector::WheelShownValue(
                                                   [self cmSpec], knob)];
}

#pragma mark - Drawing

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const GradeWheelLayout layout = [self cmLayout];
    const GradeWheelSpec& spec = [self cmSpec];
    const BOOL editable = [self cmEditable];
    const CGFloat alpha = editable ? 1.0 : kDisabledAlpha;
    const GradeWheelState state =
        ui::inspector::ReadGradeWheel([self cmShownEffects], spec);

    // The ring and its disc.
    const CGFloat scale = self.window != nil
                              ? self.window.backingScaleFactor
                              : NSScreen.mainScreen.backingScaleFactor;
    NSImage* ring =
        CMWheelRingImage(CGF(layout.ring.width), scale > 0.0 ? scale : 2.0);
    [ring drawInRect:CMRect(layout.ring)
              fromRect:NSZeroRect
             operation:NSCompositingOperationSourceOver
              fraction:alpha
        respectFlipped:YES
                 hints:nil];

    // The cross: the two axes of the puck's plane, to the rim.
    const std::array<double, 2> centre =
        ui::inspector::WheelPointInRing(layout.ring, 0.0, 0.0);
    const std::array<double, 2> right =
        ui::inspector::WheelPointInRing(layout.ring, 1.0, 0.0);
    const std::array<double, 2> top =
        ui::inspector::WheelPointInRing(layout.ring, 0.0, 1.0);
    const CGFloat reach = CGF(right[0] - centre[0]);
    NSBezierPath* cross = [NSBezierPath bezierPath];
    [cross moveToPoint:NSMakePoint(CGF(centre[0]) - reach, CGF(centre[1]))];
    [cross lineToPoint:NSMakePoint(CGF(centre[0]) + reach, CGF(centre[1]))];
    [cross moveToPoint:NSMakePoint(CGF(centre[0]), CGF(top[1]))];
    [cross lineToPoint:NSMakePoint(CGF(centre[0]),
                                   CGF(centre[1] + (centre[1] - top[1])))];
    cross.lineWidth = 1.0;
    [[CMThemeColor(ui::theme::kBorderStrong) colorWithAlphaComponent:alpha]
        setStroke];
    [cross stroke];

    // The puck: at the centre on a neutral wheel, in the accent while it is
    // held.
    const std::array<double, 2> puck =
        ui::inspector::WheelPointInRing(layout.ring, state.x, state.y);
    const CGFloat radius = CGF(ui::theme::kGradeWheelPuckRadius);
    NSBezierPath* dot = [NSBezierPath
        bezierPathWithOvalInRect:NSMakeRect(CGF(puck[0]) - radius,
                                            CGF(puck[1]) - radius, radius * 2.0,
                                            radius * 2.0)];
    const BOOL held =
        _drag == WheelDrag::Relative || _drag == WheelDrag::Absolute;
    const ui::theme::Color stroke = held ? ui::theme::kAccent
                                    : state.reach > 0.0
                                        ? ui::theme::kTextPrimary
                                        : ui::theme::kTextSecondary;
    [[CMThemeColor(ui::theme::kSurfaceBase) colorWithAlphaComponent:alpha]
        setFill];
    [dot fill];
    dot.lineWidth = 1.5;
    [[CMThemeColor(stroke) colorWithAlphaComponent:alpha] setStroke];
    [dot stroke];

    // A line under each field, in its channel's colour.
    for (size_t channel = 0; channel < 4; ++channel) {
        const GradeRect& field = layout.fields.at(channel);
        [[CMChannelColor(channel) colorWithAlphaComponent:alpha] setFill];
        NSRectFill(NSMakeRect(CGF(field.x), CGF(field.y + field.height) - 1.0,
                              CGF(field.width), 1.0));
    }

    // The master bar: a track, the notches of a jog wheel, and the mark of
    // the master knob from the middle.
    const NSRect bar = CMRect(layout.master);
    NSBezierPath* track =
        [NSBezierPath bezierPathWithRoundedRect:NSInsetRect(bar, 0.5, 0.5)
                                        xRadius:bar.size.height / 2.0
                                        yRadius:bar.size.height / 2.0];
    [[CMThemeColor(ui::theme::kSurfaceSunken) colorWithAlphaComponent:alpha]
        setFill];
    [track fill];
    [[CMThemeColor(ui::theme::kBorderSubtle) colorWithAlphaComponent:alpha]
        setStroke];
    track.lineWidth = 1.0;
    [track stroke];
    [NSGraphicsContext saveGraphicsState];
    [track addClip];
    [[CMThemeColor(ui::theme::kBorderStrong) colorWithAlphaComponent:alpha]
        setFill];
    const CGFloat notch = CGF(ui::theme::kSpaceXs);
    const auto notches = static_cast<NSInteger>(bar.size.width / notch);
    for (NSInteger index = 1; index < notches; ++index) {
        NSRectFill(NSMakeRect(NSMinX(bar) + (index * notch), NSMinY(bar) + 3.0,
                              1.0, bar.size.height - 6.0));
    }
    const CGFloat middle = NSMidX(bar);
    const CGFloat half = std::max<CGFloat>(0.0, bar.size.width / 2.0 - 3.0);
    const CGFloat mark =
        middle +
        CGF(ui::inspector::WheelMasterFraction(spec, state.knobs[3])) * half;
    const ui::theme::Color fill = _drag == WheelDrag::Master
                                      ? ui::theme::kAccent
                                      : ui::theme::kTextSecondary;
    [[CMThemeColor(fill) colorWithAlphaComponent:alpha * 0.55] setFill];
    NSRectFill(NSMakeRect(std::min(middle, mark), NSMinY(bar) + 2.0,
                          std::abs(mark - middle), bar.size.height - 4.0));
    [[CMThemeColor(state.knobs[3] != 0.0 || _drag == WheelDrag::Master
                       ? ui::theme::kTextPrimary
                       : ui::theme::kTextSecondary)
        colorWithAlphaComponent:alpha] setFill];
    NSRectFill(
        NSMakeRect(mark - 1.0, NSMinY(bar) + 1.0, 2.0, bar.size.height - 2.0));
    [NSGraphicsContext restoreGraphicsState];
}

#pragma mark - Gestures

- (NSPoint)cmPoint:(NSEvent*)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)mouseDown:(NSEvent*)event {
    if (![self cmEditable]) return;
    const GradeWheelLayout layout = [self cmLayout];
    const GradeWheelSpec& spec = [self cmSpec];
    const NSPoint point = [self cmPoint:event];
    const BOOL inDisc =
        ui::inspector::WheelDiscContains(layout.ring, point.x, point.y);
    const BOOL onBar = layout.master.Contains(point.x, point.y);
    if (!inDisc && !onBar) {
        [super mouseDown:event];
        return;
    }
    // What is typed in a field is committed before the wheel is moved, and
    // the gesture starts from the stack that leaves.
    [self.window makeFirstResponder:nil];
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    if (event.clickCount == 2) {
        // In the ring, the colour and not the master; on the bar, the
        // master and not the colour.
        [self
            cmCommit:inDisc
                         ? ui::inspector::WithWheelColourReset(effects, spec)
                         : ui::inspector::WithWheelKnob(effects, spec, 3, 0.0)];
        return;
    }
    _dragStart = effects;
    _dragCurrent = effects;
    _dragOrigin = point;
    if (onBar || (event.modifierFlags & NSEventModifierFlagCommand) != 0) {
        _drag = WheelDrag::Master;
    } else if ((event.modifierFlags & NSEventModifierFlagShift) != 0) {
        _drag = WheelDrag::Absolute;
        [self cmDragTo:point];
    } else {
        _drag = WheelDrag::Relative;
    }
    self.needsDisplay = YES;
}

- (void)mouseDragged:(NSEvent*)event {
    if (_drag == WheelDrag::None) return;
    [self cmDragTo:[self cmPoint:event]];
}

- (void)mouseUp:(NSEvent*)event {
    if (_drag == WheelDrag::None) return;
    [self cmDragTo:[self cmPoint:event]];
    const std::vector<ClipEffect> result = _dragCurrent;
    _drag = WheelDrag::None;
    _dragStart.clear();
    _dragCurrent.clear();
    [self cmCommit:result];
    [self reload];
}

// One step of the gesture: the stack it gives from where it started, shown
// here and on the monitor.
- (void)cmDragTo:(NSPoint)point {
    const GradeWheelLayout layout = [self cmLayout];
    const GradeWheelSpec& spec = [self cmSpec];
    switch (_drag) {
        case WheelDrag::None:
            return;
        case WheelDrag::Relative: {
            const double travel = ui::inspector::WheelTravelRadius(layout.ring);
            // The view's rows run down; the wheel's y runs up.
            _dragCurrent = ui::inspector::WithWheelPuckMoved(
                _dragStart, spec,
                ui::inspector::WheelDragDelta(point.x - _dragOrigin.x, travel),
                ui::inspector::WheelDragDelta(_dragOrigin.y - point.y, travel));
            break;
        }
        case WheelDrag::Absolute: {
            const std::array<double, 2> rims =
                ui::inspector::WheelPointInRims(layout.ring, point.x, point.y);
            _dragCurrent = ui::inspector::WithWheelPuck(_dragStart, spec,
                                                        rims[0], rims[1]);
            break;
        }
        case WheelDrag::Master:
            _dragCurrent = ui::inspector::WithWheelMasterMoved(
                _dragStart, spec, (point.x - _dragOrigin.x) * spec.master_step);
            break;
    }
    [self.host gradePreviewEffects:_dragCurrent];
    [self reload];
}

// A gesture that changed nothing commits nothing: no empty step in the
// journal.
- (void)cmCommit:(const std::vector<ClipEffect>&)effects {
    if (self.host == nil ||
        ui::inspector::SameGradeStack(effects, [self.host gradeEffects])) {
        return;
    }
    [self.host gradeCommitEffects:effects];
}

- (void)cmReset:(id)sender {
    (void)sender;
    if (![self cmEditable]) return;
    [self cmCommit:ui::inspector::WithWheelReset([self.host gradeEffects],
                                                 [self cmSpec])];
}

#pragma mark - Fields

- (void)cmFieldCommitted:(NSTextField*)field {
    if (![self cmEditable]) return;
    const GradeWheelSpec& spec = [self cmSpec];
    const auto channel = static_cast<size_t>(field.tag);
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    const double stored =
        ui::inspector::ReadWheelKnobs(effects, spec).at(channel);
    // Left as it was shown: nothing was typed.
    if ([field.stringValue isEqualToString:[self cmFormat:stored]]) return;
    // A decimal comma is a decimal point.
    NSString* text = [[field.stringValue
        stringByReplacingOccurrencesOfString:@","
                                  withString:@"."]
        stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
    NSScanner* scanner = [NSScanner scannerWithString:text];
    scanner.locale = [NSLocale localeWithLocaleIdentifier:@"en_US_POSIX"];
    double shown = 0.0;
    double knob = 0.0;
    if (![scanner scanDouble:&shown] || !scanner.atEnd ||
        !ui::inspector::WheelKnobForShown(spec, shown, knob)) {
        // Not a value this knob takes: the field goes back to the knob's.
        NSBeep();
        field.stringValue = [self cmFormat:stored];
        return;
    }
    [self cmCommit:ui::inspector::WithWheelKnob(effects, spec, channel, knob)];
    // What the knob holds now, clamped and at its step.
    field.stringValue = [self
        cmFormat:ui::inspector::ReadWheelKnobs([self.host gradeEffects], spec)
                     .at(channel)];
}

@end

@implementation CMGradeWheelGridView {
    NSArray<CMGradeWheelView*>* _wheelViews;
}

@synthesize wheelViews = _wheelViews;

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        NSMutableArray<CMGradeWheelView*>* views = [NSMutableArray array];
        for (const GradeWheelSpec& spec : ui::inspector::GradeWheels()) {
            CMGradeWheelView* view =
                [[CMGradeWheelView alloc] initWithFrame:NSZeroRect
                                                  wheel:spec.wheel];
            [self addSubview:view];
            [views addObject:view];
        }
        _wheelViews = views;
        [self cmLayoutWheels];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (void)setHost:(id<CMGradeEditingHost>)host {
    _host = host;
    for (CMGradeWheelView* view in _wheelViews) view.host = host;
}

- (void)cmLayoutWheels {
    const ui::inspector::GradeWheelGridLayout layout =
        ui::inspector::LayoutGradeWheelGrid(self.bounds.size.width);
    for (NSUInteger index = 0; index < _wheelViews.count; ++index) {
        _wheelViews[index].frame = CMRect(layout.cells.at(index));
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutWheels];
}

- (void)reload {
    for (CMGradeWheelView* view in _wheelViews) [view reload];
}

@end
