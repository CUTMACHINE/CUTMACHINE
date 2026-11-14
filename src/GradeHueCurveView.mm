#import "GradeHueCurveView.h"

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

using ui::inspector::CurveEdit;
using ui::inspector::GradeHueCurveLayout;
using ui::inspector::GradeRect;
using ui::inspector::HueCurveSpec;

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSRect CMRect(const GradeRect& rect) {
    return NSMakeRect(CGF(rect.x), CGF(rect.y), CGF(rect.width),
                      CGF(rect.height));
}

// As GradeCurveView.mm's: a point is held from further than it is drawn.
constexpr double kGrabReach = 7.0;
constexpr CGFloat kDisabledAlpha = 0.35;
// How much of each hue shows through the graph's ground: enough to tell
// which colour a point is over, not enough to compete with the curve.
constexpr CGFloat kHueGroundAlpha = 0.38;
constexpr int kHueGroundStops = 25;

NSColor* CMColorOf(const std::array<float, 3>& color, CGFloat alpha) {
    return [NSColor colorWithSRGBRed:color[0]
                               green:color[1]
                                blue:color[2]
                               alpha:alpha];
}

}  // namespace

@implementation CMGradeHueCurveView {
    std::string _curve;
    NSString* _notice;
    CMTabStripView* _curves;
    NSButton* _reset;

    BOOL _dragging;
    int32_t _held;
    double _grabX;
    double _grabY;
    // As GradeCurveView.mm: the stack the gesture moves its point in, and
    // the stack it is at.
    std::vector<ClipEffect> _dragBase;
    std::vector<ClipEffect> _dragCurrent;
}

@synthesize notice = _notice;

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        // Hue against saturation first: the one reached for most.
        _curve = kHueCurveNames.at(1);
        _notice = @"";
        _held = -1;
        NSMutableArray<NSString*>* titles = [NSMutableArray array];
        for (const HueCurveSpec& spec : ui::inspector::HueCurveSpecs()) {
            [titles addObject:[NSString stringWithUTF8String:spec.tab] ?: @""];
        }
        _curves = [[CMTabStripView alloc] initWithFrame:NSZeroRect
                                                 titles:titles];
        _curves.target = self;
        _curves.action = @selector(cmCurveChosen:);
        _curves.toolTip = @"Teinte, Saturation, Luma : la première se lit le "
                          @"long de l'axe, la seconde est ce que la courbe "
                          @"change";
        [self addSubview:_curves];
        [_curves selectIndex:1];
        _reset = CMMakeStyledButton(@"↺", self, @selector(cmResetCurve:));
        _reset.toolTip = @"Remettre cette courbe au neutre";
        // PRD27 -- VoiceOver would read the arrow's glyph.
        _reset.accessibilityLabel = @"Remettre cette courbe au neutre";
        [self addSubview:_reset];
        [self cmLayoutSubviews];
        [self reload];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (std::string)curve {
    return _curve;
}

- (GradeHueCurveLayout)cmLayout {
    return ui::inspector::LayoutGradeHueCurve(self.bounds.size.width);
}

- (void)cmLayoutSubviews {
    const GradeHueCurveLayout layout = [self cmLayout];
    _curves.frame = CMRect(layout.bar);
    _reset.frame = CMRect(layout.reset);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutSubviews];
    self.needsDisplay = YES;
}

- (BOOL)cmEditable {
    return self.host != nil && [self.host gradeEditable];
}

- (BOOL)cmPeriodic {
    return IsHueIndexedCurve(_curve);
}

- (std::vector<ClipEffect>)cmShownEffects {
    if (_dragging) return _dragCurrent;
    return self.host != nil ? [self.host gradeEffects]
                            : std::vector<ClipEffect>{};
}

- (void)reload {
    _reset.enabled =
        [self cmEditable] &&
        !ui::inspector::HueCurvePoints([self cmShownEffects], _curve).empty();
    self.needsDisplay = YES;
}

- (void)selectCurve:(const std::string&)curve {
    for (size_t index = 0; index < kHueCurveNames.size(); ++index) {
        if (curve == kHueCurveNames.at(index)) {
            _curve = curve;
            if (_curves.selectedIndex != static_cast<NSInteger>(index)) {
                [_curves selectIndex:static_cast<NSInteger>(index)];
            }
            [self reload];
            return;
        }
    }
}

- (void)cmCurveChosen:(id)sender {
    (void)sender;
    const NSInteger index = _curves.selectedIndex;
    if (index < 0 || static_cast<size_t>(index) >= kHueCurveNames.size()) {
        return;
    }
    _curve = kHueCurveNames.at(static_cast<size_t>(index));
    [self cmSetNotice:@""];
    [self reload];
}

- (void)cmSetNotice:(NSString*)notice {
    _notice = [notice copy] ?: @"";
    self.needsDisplay = YES;
}

#pragma mark - Drawing

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const GradeHueCurveLayout layout = [self cmLayout];
    const GradeRect graph = layout.graph;
    const NSRect frame = CMRect(graph);
    const BOOL periodic = [self cmPeriodic];
    const CGFloat alpha = [self cmEditable] ? 1.0 : kDisabledAlpha;
    const std::vector<ClipEffect> effects = [self cmShownEffects];

    // The six patches: live along the hue, dimmed otherwise.
    const auto& vectors = ui::inspector::HueCurveVectors();
    for (size_t index = 0; index < vectors.size(); ++index) {
        const NSRect patch = CMRect(layout.patches.at(index));
        [CMColorOf(vectors.at(index).color,
                   periodic ? alpha : alpha * kDisabledAlpha) setFill];
        [[NSBezierPath bezierPathWithOvalInRect:NSInsetRect(patch, 2.0, 2.0)]
            fill];
    }

    // The ground: the hues themselves under a curve read along them.
    [CMThemeColor(ui::theme::kSurfaceSunken) setFill];
    NSRectFill(frame);
    if (periodic) {
        NSMutableArray<NSColor*>* colors = [NSMutableArray array];
        std::vector<CGFloat> locations;
        for (int stop = 0; stop < kHueGroundStops; ++stop) {
            const double turn =
                static_cast<double>(stop) / (kHueGroundStops - 1);
            [colors
                addObject:CMColorOf(ui::inspector::WheelRimColor(
                                        turn * 2.0 * 3.14159265358979323846),
                                    kHueGroundAlpha * alpha)];
            locations.push_back(CGF(turn));
        }
        NSGradient* ground =
            [[NSGradient alloc] initWithColors:colors
                                   atLocations:locations.data()
                                    colorSpace:NSColorSpace.sRGBColorSpace];
        [ground drawInRect:frame angle:0.0];
    }

    // The quarters along x, then the neutral line.
    NSBezierPath* grid = [NSBezierPath bezierPath];
    for (int quarter = 1; quarter < 4; ++quarter) {
        const std::array<double, 2> low =
            ui::inspector::CurvePointInGraph(graph, quarter / 4.0, 0.0);
        const CGFloat x = std::floor(CGF(low[0])) + 0.5;
        [grid moveToPoint:NSMakePoint(x, NSMinY(frame))];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(frame))];
    }
    grid.lineWidth = 1.0;
    [CMThemeColor(ui::theme::kBorderSubtle) setStroke];
    [grid stroke];
    const std::array<double, 2> neutralAt =
        ui::inspector::CurvePointInGraph(graph, 0.0, 0.5);
    const CGFloat neutralY = std::floor(CGF(neutralAt[1])) + 0.5;
    NSBezierPath* neutral = [NSBezierPath bezierPath];
    [neutral moveToPoint:NSMakePoint(NSMinX(frame), neutralY)];
    [neutral lineToPoint:NSMakePoint(NSMaxX(frame), neutralY)];
    neutral.lineWidth = 1.0;
    [CMThemeColor(ui::theme::kBorderStrong) setStroke];
    [neutral stroke];

    // The curve, as rendered.
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:frame] addClip];
    const auto count =
        static_cast<int32_t>(std::max<CGFloat>(2.0, frame.size.width));
    const std::vector<double> samples =
        ui::inspector::SampleHueCurve(effects, _curve, count);
    NSBezierPath* stroke = [NSBezierPath bezierPath];
    for (size_t index = 0; index < samples.size(); ++index) {
        const std::array<double, 2> point = ui::inspector::CurvePointInGraph(
            graph, static_cast<double>(index) / (count - 1), samples[index]);
        const NSPoint at = NSMakePoint(CGF(point[0]), CGF(point[1]));
        if (index == 0) {
            [stroke moveToPoint:at];
        } else {
            [stroke lineToPoint:at];
        }
    }
    stroke.lineWidth = 1.5;
    stroke.lineJoinStyle = NSLineJoinStyleRound;
    [[CMThemeColor(ui::theme::kChannelMaster) colorWithAlphaComponent:alpha]
        setStroke];
    [stroke stroke];
    [NSGraphicsContext restoreGraphicsState];

    const std::vector<ColorCurvePoint> points =
        ui::inspector::HueCurvePoints(effects, _curve);
    const CGFloat radius = CGF(ui::theme::kGradeCurvePointRadius);
    for (size_t index = 0; index < points.size(); ++index) {
        const std::array<double, 2> at = ui::inspector::CurvePointInGraph(
            graph, ui::inspector::CurveCoordinate(points[index].x),
            ui::inspector::CurveCoordinate(points[index].y));
        NSBezierPath* dot = [NSBezierPath
            bezierPathWithOvalInRect:NSMakeRect(CGF(at[0]) - radius,
                                                CGF(at[1]) - radius,
                                                radius * 2.0, radius * 2.0)];
        const BOOL held = _dragging && static_cast<int32_t>(index) == _held;
        [[CMThemeColor(held ? ui::theme::kAccent : ui::theme::kSurfaceBase)
            colorWithAlphaComponent:alpha] setFill];
        [dot fill];
        dot.lineWidth = 1.5;
        [[CMThemeColor(held ? ui::theme::kAccent : ui::theme::kChannelMaster)
            colorWithAlphaComponent:alpha] setStroke];
        [dot stroke];
    }

    [CMThemeColor(ui::theme::kBorderSubtle) setFill];
    NSFrameRectWithWidth(frame, 1.0);

    if (_notice.length > 0) {
        NSDictionary* attributes = @{
            NSFontAttributeName :
                CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular),
            NSForegroundColorAttributeName : CMTextPrimaryColor(),
        };
        const CGFloat inset = CGF(ui::theme::kSpaceXs);
        [_notice drawWithRect:NSMakeRect(NSMinX(frame) + inset,
                                         NSMinY(frame) + inset,
                                         frame.size.width - 2.0 * inset,
                                         frame.size.height - 2.0 * inset)
                      options:NSStringDrawingUsesLineFragmentOrigin
                   attributes:attributes
                      context:nil];
    }
}

#pragma mark - Gestures

- (NSPoint)cmPoint:(NSEvent*)event {
    return [self convertPoint:event.locationInWindow fromView:nil];
}

- (int32_t)cmHit:(NSPoint)point
         effects:(const std::vector<ClipEffect>&)effects {
    const GradeRect graph = [self cmLayout].graph;
    if (graph.width <= 0.0 || graph.height <= 0.0) return -1;
    const std::array<double, 2> unit =
        ui::inspector::CurvePointInUnits(graph, point.x, point.y);
    return ui::inspector::HitHueCurvePoint(effects, _curve, unit[0], unit[1],
                                           kGrabReach / graph.width,
                                           kGrabReach / graph.height);
}

- (BOOL)cmGraphTakes:(NSPoint)point {
    const NSRect frame = CMRect([self cmLayout].graph);
    return NSPointInRect(
        point, NSInsetRect(frame, -CGF(kGrabReach), -CGF(kGrabReach)));
}

// The colour patch under `point`, or -1.
- (NSInteger)cmPatchAt:(NSPoint)point {
    const GradeHueCurveLayout layout = [self cmLayout];
    for (size_t index = 0; index < layout.patches.size(); ++index) {
        if (layout.patches.at(index).Contains(point.x, point.y)) {
            return static_cast<NSInteger>(index);
        }
    }
    return -1;
}

- (void)cmRefuse:(const std::string&)refusal {
    NSBeep();
    [self cmSetNotice:[NSString stringWithUTF8String:refusal.c_str()] ?: @""];
}

- (void)mouseDown:(NSEvent*)event {
    const NSPoint point = [self cmPoint:event];
    if (![self cmEditable]) {
        [super mouseDown:event];
        return;
    }
    const NSInteger patch = [self cmPatchAt:point];
    if (patch >= 0) {
        // A patch: three points around its colour, on the curve as it is.
        if (![self cmPeriodic]) return;
        [self.window makeFirstResponder:nil];
        const CurveEdit placed = ui::inspector::WithHueCurveVector(
            [self.host gradeEffects], _curve, static_cast<size_t>(patch));
        if (!placed.refusal.empty()) {
            [self cmRefuse:placed.refusal];
            return;
        }
        [self cmSetNotice:@""];
        [self cmCommit:placed.effects];
        return;
    }
    if (![self cmGraphTakes:point]) {
        [super mouseDown:event];
        return;
    }
    [self.window makeFirstResponder:nil];
    const GradeRect graph = [self cmLayout].graph;
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    const std::array<double, 2> unit =
        ui::inspector::CurvePointInUnits(graph, point.x, point.y);
    [self cmSetNotice:@""];
    int32_t held = [self cmHit:point effects:effects];
    _dragBase = effects;
    if (held < 0) {
        const CurveEdit added = ui::inspector::WithHueCurvePointAdded(
            effects, _curve, unit[0], unit[1],
            (event.modifierFlags & NSEventModifierFlagShift) != 0);
        if (!added.refusal.empty() || added.index < 0) {
            [self cmRefuse:added.refusal];
            return;
        }
        _dragBase = added.effects;
        held = added.index;
    }
    const std::vector<ColorCurvePoint> points =
        ui::inspector::HueCurvePoints(_dragBase, _curve);
    if (static_cast<size_t>(held) >= points.size()) return;
    _held = held;
    _grabX =
        ui::inspector::CurveCoordinate(points[static_cast<size_t>(held)].x) -
        unit[0];
    _grabY =
        ui::inspector::CurveCoordinate(points[static_cast<size_t>(held)].y) -
        unit[1];
    _dragging = YES;
    _dragCurrent = _dragBase;
    [self.host gradePreviewEffects:_dragCurrent];
    [self reload];
}

- (void)mouseDragged:(NSEvent*)event {
    if (!_dragging) return;
    const NSPoint point = [self cmPoint:event];
    const std::array<double, 2> unit = ui::inspector::CurvePointInUnits(
        [self cmLayout].graph, point.x, point.y);
    _dragCurrent =
        ui::inspector::WithHueCurvePointMoved(
            _dragBase, _curve, _held, unit[0] + _grabX, unit[1] + _grabY,
            (event.modifierFlags & NSEventModifierFlagOption) != 0)
            .effects;
    [self.host gradePreviewEffects:_dragCurrent];
    [self reload];
}

- (void)mouseUp:(NSEvent*)event {
    (void)event;
    if (!_dragging) return;
    const std::vector<ClipEffect> result = _dragCurrent;
    _dragging = NO;
    _held = -1;
    _dragBase.clear();
    _dragCurrent.clear();
    [self cmCommit:result];
    [self reload];
}

- (void)rightMouseDown:(NSEvent*)event {
    const NSPoint point = [self cmPoint:event];
    if (![self cmEditable] || ![self cmGraphTakes:point]) {
        [super rightMouseDown:event];
        return;
    }
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    const int32_t hit = [self cmHit:point effects:effects];
    if (hit < 0) return;
    [self cmSetNotice:@""];
    [self cmCommit:ui::inspector::WithHueCurvePointRemoved(effects, _curve, hit)
                       .effects];
}

- (void)cmCommit:(const std::vector<ClipEffect>&)effects {
    if (self.host == nil ||
        ui::inspector::SameGradeStack(effects, [self.host gradeEffects])) {
        return;
    }
    [self.host gradeCommitEffects:effects];
}

- (void)cmResetCurve:(id)sender {
    (void)sender;
    if (![self cmEditable]) return;
    [self cmSetNotice:@""];
    [self cmCommit:ui::inspector::WithHueCurveReset([self.host gradeEffects],
                                                    _curve)];
}

@end
