#import "GradeCurveView.h"

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
using ui::inspector::GradeCurveLayout;
using ui::inspector::GradeRect;

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSRect CMRect(const GradeRect& rect) {
    return NSMakeRect(CGF(rect.x), CGF(rect.y), CGF(rect.width),
                      CGF(rect.height));
}

// A point is held from this far, in points: more than it is drawn, the hand
// being less exact than the eye.
constexpr double kGrabReach = 7.0;

constexpr CGFloat kDisabledAlpha = 0.35;
// A channel that is not the one edited stays readable behind it.
constexpr CGFloat kBehindAlpha = 0.45;

// The bar's buttons, right to left: the reset, the menu, the anchors.
constexpr CGFloat kSmallButtonWidth = 22.0;
constexpr CGFloat kAnchorsButtonWidth = 52.0;

ui::theme::Color CMCurveColor(const std::string& channel) {
    if (channel == "r") return ui::theme::kChannelRed;
    if (channel == "g") return ui::theme::kChannelGreen;
    if (channel == "b") return ui::theme::kChannelBlue;
    return ui::theme::kChannelMaster;
}

NSString* CMChannelTitle(const std::string& channel) {
    if (channel == "r") return @"Rouge";
    if (channel == "g") return @"Vert";
    if (channel == "b") return @"Bleu";
    return @"RVB";
}

}  // namespace

@implementation CMGradeCurveView {
    std::string _channel;
    NSString* _notice;
    CMTabStripView* _channels;
    NSButton* _anchors;
    NSButton* _more;
    NSButton* _reset;
    VideoHistogram _histogram;

    BOOL _dragging;
    // The point the gesture holds, in the edited channel.
    int32_t _held;
    // How far the point sat from the pointer when it was taken, in the
    // curve's units: it does not jump under the pointer.
    double _grabX;
    double _grabY;
    // The stack the gesture moves its point in: the clip's, or the clip's
    // with the point the press added. Every step is computed from it.
    std::vector<ClipEffect> _dragBase;
    std::vector<ClipEffect> _dragCurrent;
}

@synthesize notice = _notice;

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _channel = kColorCurveChannels.front();
        _histogram = {};
        _notice = @"";
        _held = -1;
        NSMutableArray<NSString*>* titles = [NSMutableArray array];
        for (const char* channel : kColorCurveChannels) {
            // The strip is narrow: a letter per colour channel.
            const std::string name = channel;
            [titles addObject:name == "master"
                                  ? @"RVB"
                                  : [CMChannelTitle(name) substringToIndex:1]];
        }
        _channels = [[CMTabStripView alloc] initWithFrame:NSZeroRect
                                                   titles:titles];
        _channels.target = self;
        _channels.action = @selector(cmChannelChosen:);
        [self addSubview:_channels];

        _anchors = CMMakeStyledButton(@"Ancres", self, @selector(cmAnchors:));
        _anchors.toolTip = @"Poser trois points sur la courbe, aux quarts";
        [self addSubview:_anchors];
        _more = CMMakeStyledButton(@"⋯", self, @selector(cmMore:));
        _more.toolTip = @"Copier ce canal, réinitialiser toutes les courbes";
        // PRD27 -- VoiceOver would read these glyphs, not what they do.
        _more.accessibilityLabel = @"Autres actions sur les courbes";
        [self addSubview:_more];
        _reset = CMMakeStyledButton(@"↺", self, @selector(cmResetChannel:));
        _reset.toolTip = @"Remettre ce canal à la diagonale";
        _reset.accessibilityLabel = @"Remettre ce canal à la diagonale";
        [self addSubview:_reset];

        [self cmLayoutSubviews];
        [self reload];
    }
    return self;
}

- (BOOL)isFlipped {
    return YES;
}

- (std::string)channel {
    return _channel;
}

- (GradeCurveLayout)cmLayout {
    return ui::inspector::LayoutGradeCurve(self.bounds.size.width);
}

- (void)cmLayoutSubviews {
    const NSRect bar = CMRect([self cmLayout].bar);
    const CGFloat gap = CGF(ui::theme::kSpaceXs);
    CGFloat right = NSMaxX(bar);
    _reset.frame = NSMakeRect(right - kSmallButtonWidth, NSMinY(bar),
                              kSmallButtonWidth, bar.size.height);
    right -= kSmallButtonWidth + gap;
    _more.frame = NSMakeRect(right - kSmallButtonWidth, NSMinY(bar),
                             kSmallButtonWidth, bar.size.height);
    right -= kSmallButtonWidth + gap;
    _anchors.frame = NSMakeRect(right - kAnchorsButtonWidth, NSMinY(bar),
                                kAnchorsButtonWidth, bar.size.height);
    right -= kAnchorsButtonWidth + gap;
    _channels.frame = NSMakeRect(NSMinX(bar), NSMinY(bar),
                                 std::max<CGFloat>(0.0, right - NSMinX(bar)),
                                 bar.size.height);
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutSubviews];
    self.needsDisplay = YES;
}

- (BOOL)cmEditable {
    return self.host != nil && [self.host gradeEditable];
}

- (std::vector<ClipEffect>)cmShownEffects {
    if (_dragging) return _dragCurrent;
    return self.host != nil ? [self.host gradeEffects]
                            : std::vector<ClipEffect>{};
}

- (void)reload {
    const BOOL editable = [self cmEditable];
    _anchors.enabled = editable;
    _more.enabled = editable;
    _reset.enabled = editable && ui::inspector::HasCurvePoints(
                                     [self cmShownEffects], _channel);
    self.needsDisplay = YES;
}

- (void)showHistogram:(const VideoHistogram&)histogram {
    if (_histogram == histogram) {
        return;
    }
    _histogram = histogram;
    self.needsDisplay = YES;
}

- (void)selectChannel:(const std::string&)channel {
    for (size_t index = 0; index < kColorCurveChannels.size(); ++index) {
        if (channel == kColorCurveChannels.at(index)) {
            _channel = channel;
            if (_channels.selectedIndex != static_cast<NSInteger>(index)) {
                [_channels selectIndex:static_cast<NSInteger>(index)];
            }
            [self reload];
            return;
        }
    }
}

- (void)cmChannelChosen:(id)sender {
    (void)sender;
    const NSInteger index = _channels.selectedIndex;
    if (index < 0 || static_cast<size_t>(index) >= kColorCurveChannels.size()) {
        return;
    }
    _channel = kColorCurveChannels.at(static_cast<size_t>(index));
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
    const GradeRect graph = [self cmLayout].graph;
    const NSRect frame = CMRect(graph);
    const CGFloat alpha = [self cmEditable] ? 1.0 : kDisabledAlpha;
    const std::vector<ClipEffect> effects = [self cmShownEffects];

    [CMThemeColor(ui::theme::kSurfaceSunken) setFill];
    NSRectFill(frame);

    // DEC42 -- behind the grid and the curves. RGB shows the three
    // distributions together; an individual channel shows its own.
    const std::array<std::string, 3> histogramChannels = {"r", "g", "b"};
    for (size_t channel = 0; channel < histogramChannels.size(); ++channel) {
        const std::string& name = histogramChannels[channel];
        if (_channel != "master" && _channel != name) {
            continue;
        }
        const auto levels = NormalizeVideoHistogramChannel(_histogram, channel);
        NSBezierPath* area = [NSBezierPath bezierPath];
        [area moveToPoint:NSMakePoint(NSMinX(frame), NSMaxY(frame))];
        for (size_t bin = 0; bin < levels.size(); ++bin) {
            const CGFloat left =
                NSMinX(frame) + frame.size.width * bin / levels.size();
            const CGFloat right =
                NSMinX(frame) + frame.size.width * (bin + 1) / levels.size();
            const CGFloat top = NSMaxY(frame) - frame.size.height * levels[bin];
            [area lineToPoint:NSMakePoint(left, top)];
            [area lineToPoint:NSMakePoint(right, top)];
        }
        [area lineToPoint:NSMakePoint(NSMaxX(frame), NSMaxY(frame))];
        [area closePath];
        [[CMThemeColor(CMCurveColor(name))
            colorWithAlphaComponent:alpha * kBehindAlpha] setFill];
        [area fill];
    }

    // The quarters, then the neutral diagonal.
    NSBezierPath* grid = [NSBezierPath bezierPath];
    for (int quarter = 1; quarter < 4; ++quarter) {
        const std::array<double, 2> low =
            ui::inspector::CurvePointInGraph(graph, quarter / 4.0, 0.0);
        const std::array<double, 2> left =
            ui::inspector::CurvePointInGraph(graph, 0.0, quarter / 4.0);
        // On the half pixel, so that a one-point line is one point.
        const CGFloat x = std::floor(CGF(low[0])) + 0.5;
        const CGFloat y = std::floor(CGF(left[1])) + 0.5;
        [grid moveToPoint:NSMakePoint(x, NSMinY(frame))];
        [grid lineToPoint:NSMakePoint(x, NSMaxY(frame))];
        [grid moveToPoint:NSMakePoint(NSMinX(frame), y)];
        [grid lineToPoint:NSMakePoint(NSMaxX(frame), y)];
    }
    grid.lineWidth = 1.0;
    [CMThemeColor(ui::theme::kBorderSubtle) setStroke];
    [grid stroke];
    NSBezierPath* diagonal = [NSBezierPath bezierPath];
    [diagonal moveToPoint:NSMakePoint(NSMinX(frame), NSMaxY(frame))];
    [diagonal lineToPoint:NSMakePoint(NSMaxX(frame), NSMinY(frame))];
    diagonal.lineWidth = 1.0;
    [CMThemeColor(ui::theme::kBorderStrong) setStroke];
    [diagonal stroke];

    // The four curves, the one edited last and over the others. A channel
    // without a point is the diagonal already drawn.
    [NSGraphicsContext saveGraphicsState];
    [[NSBezierPath bezierPathWithRect:frame] addClip];
    const auto count =
        static_cast<int32_t>(std::max<CGFloat>(2.0, frame.size.width));
    std::vector<std::string> order;
    for (const char* channel : kColorCurveChannels) {
        if (_channel != channel) order.emplace_back(channel);
    }
    order.push_back(_channel);
    for (const std::string& channel : order) {
        const bool edited = channel == _channel;
        if (!edited && !ui::inspector::HasCurvePoints(effects, channel)) {
            continue;
        }
        const std::vector<double> samples =
            ui::inspector::SampleCurve(effects, channel, count);
        NSBezierPath* curve = [NSBezierPath bezierPath];
        for (size_t index = 0; index < samples.size(); ++index) {
            const std::array<double, 2> point =
                ui::inspector::CurvePointInGraph(
                    graph, static_cast<double>(index) / (count - 1),
                    samples[index]);
            const NSPoint at = NSMakePoint(CGF(point[0]), CGF(point[1]));
            if (index == 0) {
                [curve moveToPoint:at];
            } else {
                [curve lineToPoint:at];
            }
        }
        curve.lineWidth = edited ? 1.5 : 1.0;
        curve.lineJoinStyle = NSLineJoinStyleRound;
        [[CMThemeColor(CMCurveColor(channel))
            colorWithAlphaComponent:alpha * (edited ? 1.0 : kBehindAlpha)]
            setStroke];
        [curve stroke];
    }
    [NSGraphicsContext restoreGraphicsState];

    // The points of the channel edited, the one held in the accent.
    const std::vector<ColorCurvePoint> points =
        ui::inspector::ShownCurvePoints(effects, _channel);
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
        [[CMThemeColor(held ? ui::theme::kAccent : CMCurveColor(_channel))
            colorWithAlphaComponent:alpha] setStroke];
        [dot stroke];
    }

    [CMThemeColor(ui::theme::kBorderSubtle) setFill];
    NSFrameRectWithWidth(frame, 1.0);

    // Why a gesture was refused, until the next one.
    if (_notice.length > 0) {
        NSDictionary* attributes = @{
            NSFontAttributeName :
                CMFont(ui::theme::kFontSizeCaption, NSFontWeightRegular),
            NSForegroundColorAttributeName : CMTextSecondaryColor(),
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

// The point of the edited channel under `point`, or -1.
- (int32_t)cmHit:(NSPoint)point
         effects:(const std::vector<ClipEffect>&)effects {
    const GradeRect graph = [self cmLayout].graph;
    if (graph.width <= 0.0 || graph.height <= 0.0) return -1;
    const std::array<double, 2> unit =
        ui::inspector::CurvePointInUnits(graph, point.x, point.y);
    return ui::inspector::HitCurvePoint(effects, _channel, unit[0], unit[1],
                                        kGrabReach / graph.width,
                                        kGrabReach / graph.height);
}

// The graph takes a press a little past its edges: its end points sit on
// them.
- (BOOL)cmGraphTakes:(NSPoint)point {
    const NSRect frame = CMRect([self cmLayout].graph);
    return NSPointInRect(
        point, NSInsetRect(frame, -CGF(kGrabReach), -CGF(kGrabReach)));
}

- (void)mouseDown:(NSEvent*)event {
    const NSPoint point = [self cmPoint:event];
    if (![self cmEditable] || ![self cmGraphTakes:point]) {
        [super mouseDown:event];
        return;
    }
    // What is typed in a field of the panel is committed first.
    [self.window makeFirstResponder:nil];
    const GradeRect graph = [self cmLayout].graph;
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    const std::array<double, 2> unit =
        ui::inspector::CurvePointInUnits(graph, point.x, point.y);
    [self cmSetNotice:@""];
    int32_t held = [self cmHit:point effects:effects];
    _dragBase = effects;
    if (held < 0) {
        // A click adds a point where it lands; with Shift, on the curve.
        const CurveEdit added = ui::inspector::WithCurvePointAdded(
            effects, _channel, unit[0], unit[1],
            (event.modifierFlags & NSEventModifierFlagShift) != 0);
        if (!added.refusal.empty() || added.index < 0) {
            NSBeep();
            [self cmSetNotice:[NSString
                                  stringWithUTF8String:added.refusal.c_str()]];
            return;
        }
        _dragBase = added.effects;
        held = added.index;
    }
    const std::vector<ColorCurvePoint> points =
        ui::inspector::ShownCurvePoints(_dragBase, _channel);
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
    [self cmDragTo:[self cmPoint:event]
              snap:(event.modifierFlags & NSEventModifierFlagOption) != 0];
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

// One step of the gesture: the held point where the pointer is, from the
// stack the gesture started in.
- (void)cmDragTo:(NSPoint)point snap:(BOOL)snap {
    const std::array<double, 2> unit = ui::inspector::CurvePointInUnits(
        [self cmLayout].graph, point.x, point.y);
    _dragCurrent = ui::inspector::WithCurvePointMoved(_dragBase, _channel,
                                                      _held, unit[0] + _grabX,
                                                      unit[1] + _grabY, snap)
                       .effects;
    [self.host gradePreviewEffects:_dragCurrent];
    [self reload];
}

// A right click on a point removes it.
- (void)rightMouseDown:(NSEvent*)event {
    const NSPoint point = [self cmPoint:event];
    if (![self cmEditable] || ![self cmGraphTakes:point]) {
        [super rightMouseDown:event];
        return;
    }
    const std::vector<ClipEffect> effects = [self.host gradeEffects];
    const int32_t hit = [self cmHit:point effects:effects];
    if (hit < 0) return;
    const CurveEdit removed =
        ui::inspector::WithCurvePointRemoved(effects, _channel, hit);
    if (!removed.refusal.empty()) {
        NSBeep();
        [self cmSetNotice:[NSString
                              stringWithUTF8String:removed.refusal.c_str()]];
        return;
    }
    [self cmSetNotice:@""];
    [self cmCommit:removed.effects];
}

// A gesture that changed nothing commits nothing.
- (void)cmCommit:(const std::vector<ClipEffect>&)effects {
    if (self.host == nil ||
        ui::inspector::SameGradeStack(effects, [self.host gradeEffects])) {
        return;
    }
    [self.host gradeCommitEffects:effects];
}

#pragma mark - The bar

- (void)cmAnchors:(id)sender {
    (void)sender;
    if (![self cmEditable]) return;
    const CurveEdit anchored =
        ui::inspector::WithCurveAnchors([self.host gradeEffects], _channel);
    if (!anchored.refusal.empty()) {
        NSBeep();
        [self cmSetNotice:[NSString
                              stringWithUTF8String:anchored.refusal.c_str()]];
        return;
    }
    [self cmSetNotice:@""];
    [self cmCommit:anchored.effects];
}

- (void)cmResetChannel:(id)sender {
    (void)sender;
    if (![self cmEditable]) return;
    [self cmSetNotice:@""];
    [self cmCommit:ui::inspector::WithCurveChannelReset(
                       [self.host gradeEffects], _channel)];
}

- (void)cmMore:(NSButton*)sender {
    if (![self cmEditable]) return;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    for (size_t index = 0; index < kColorCurveChannels.size(); ++index) {
        const std::string channel = kColorCurveChannels.at(index);
        if (channel == _channel) continue;
        NSMenuItem* item = [menu
            addItemWithTitle:[NSString stringWithFormat:@"Copier vers %@",
                                                        CMChannelTitle(channel)]
                      action:@selector(cmCopyChannel:)
               keyEquivalent:@""];
        item.target = self;
        item.tag = static_cast<NSInteger>(index);
    }
    [menu addItem:NSMenuItem.separatorItem];
    NSMenuItem* all = [menu addItemWithTitle:@"Réinitialiser toutes les courbes"
                                      action:@selector(cmResetAll:)
                               keyEquivalent:@""];
    all.target = self;
    [menu popUpMenuPositioningItem:nil
                        atLocation:NSMakePoint(0.0, NSMaxY(sender.bounds))
                            inView:sender];
}

- (void)cmCopyChannel:(NSMenuItem*)item {
    if (![self cmEditable] || item.tag < 0 ||
        static_cast<size_t>(item.tag) >= kColorCurveChannels.size()) {
        return;
    }
    [self cmCommit:ui::inspector::WithCurveChannelCopied(
                       [self.host gradeEffects], _channel,
                       kColorCurveChannels.at(static_cast<size_t>(item.tag)))];
}

- (void)cmResetAll:(id)sender {
    (void)sender;
    if (![self cmEditable]) return;
    [self cmSetNotice:@""];
    [self cmCommit:ui::inspector::WithCurvesReset([self.host gradeEffects])];
}

@end
