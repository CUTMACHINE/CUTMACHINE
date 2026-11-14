#import "MotionCanvasView.h"

#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

// The handles' side, in points: big enough to grab, small enough not to
// hide the corner they stand for.
constexpr CGFloat kHandleSide = 7.0;

}  // namespace

@implementation CMMotionCanvasView {
    std::vector<std::pair<double, double>> _hover;
    std::vector<std::pair<double, double>> _selection;
    std::vector<std::pair<double, double>> _handles;
}

- (NSView*)hitTest:(NSPoint)point {
    (void)point;
    return nil;
}

- (void)setHover:(const std::vector<std::pair<double, double>>&)hover
       selection:(const std::vector<std::pair<double, double>>&)selection
         handles:(const std::vector<std::pair<double, double>>&)handles {
    _hover = hover;
    _selection = selection;
    _handles = handles;
    // Hidden when there is nothing to stroke: a view over Record is taken
    // for a control covering the picture (the visual QA leaves the cells
    // it covers out of its comparisons), and an empty overlay covers none.
    self.hidden = _hover.empty() && _selection.empty() && _handles.empty();
    self.needsDisplay = YES;
}

// Rows from the top, as MotionCanvas.h gives them, to AppKit's rows from
// the bottom.
- (NSPoint)cmPoint:(const std::pair<double, double>&)point {
    return NSMakePoint(point.first, self.bounds.size.height - point.second);
}

- (NSBezierPath*)cmQuad:(const std::vector<std::pair<double, double>>&)quad {
    NSBezierPath* path = [NSBezierPath bezierPath];
    for (size_t index = 0; index < quad.size(); ++index) {
        const NSPoint point = [self cmPoint:quad[index]];
        if (index == 0) {
            [path moveToPoint:point];
        } else {
            [path lineToPoint:point];
        }
    }
    [path closePath];
    return path;
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    if (_hover.size() == 4) {
        NSBezierPath* hover = [self cmQuad:_hover];
        hover.lineWidth = 1.0;
        [[CMThemeColor(ui::theme::kAccent) colorWithAlphaComponent:0.55]
            setStroke];
        [hover stroke];
    }
    if (_selection.size() == 4) {
        NSBezierPath* selection = [self cmQuad:_selection];
        selection.lineWidth = 1.5;
        [CMThemeColor(ui::theme::kAccent) setStroke];
        [selection stroke];
    }
    for (const auto& handle : _handles) {
        const NSPoint center = [self cmPoint:handle];
        const NSRect square =
            NSMakeRect(center.x - kHandleSide / 2.0,
                       center.y - kHandleSide / 2.0, kHandleSide, kHandleSide);
        [CMThemeColor(ui::theme::kTextPrimary) setFill];
        NSRectFill(square);
        // NSFrameRectWithWidth draws with the fill colour.
        [CMThemeColor(ui::theme::kAccent) setFill];
        NSFrameRectWithWidth(square, 1.0);
    }
}

@end
