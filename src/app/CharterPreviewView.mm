// CH6 -- docs/roadmap/CHARTE-2026-11.md. One frame of the Charte panel's
// preview, drawn (CharterPreviewView.h). Every colour is a token of
// UiTheme.h or a colour the charter names; the outlines are drawn here, in
// AppKit, over the pictures, as the motion canvas draws REM5's over Record.

#import "CharterPreviewView.h"

#include "UiTheme.h"
#include "UiThemeAppKit.h"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace {

// The handles' side, in points, as REM5's (MotionCanvasView.mm).
constexpr CGFloat kHandleSide = 7.0;
// The checker's square, in points.
constexpr CGFloat kCheckerSide = 8.0;

}  // namespace

@implementation CMCharterFrameView {
    NSArray* _pictures;
    std::vector<ui::charter::Point> _hover;
    std::vector<ui::charter::Point> _handles;
    std::vector<ui::charter::Point> _drag;
    std::vector<ui::charter::Point> _zone;
    NSTrackingArea* _tracking;
}

- (BOOL)acceptsFirstMouse:(NSEvent*)event {
    (void)event;
    return YES;
}

- (BOOL)isOpaque {
    return YES;
}

- (void)setPictures:(NSArray*)pictures {
    _pictures = [pictures copy];
    self.needsDisplay = YES;
}

- (void)setHover:(const std::vector<ui::charter::Point>&)quad
         handles:(const std::vector<ui::charter::Point>&)handles {
    _hover = quad;
    _handles = handles;
    self.needsDisplay = YES;
}

- (void)setDragOutline:(const std::vector<ui::charter::Point>&)quad
              safeZone:(const std::vector<ui::charter::Point>&)zone {
    _drag = quad;
    _zone = zone;
    self.needsDisplay = YES;
}

- (void)setMessage:(NSString*)message {
    _message = [message copy];
    self.needsDisplay = YES;
}

- (void)setSelected:(BOOL)selected {
    _selected = selected;
    self.needsDisplay = YES;
}

- (void)setBackground:(CMCharterBackground)background {
    _background = background;
    self.needsDisplay = YES;
}

- (double)pointsPerPixel {
    const double width = self.previewFrame.width;
    return width > 0.0 ? self.bounds.size.width / width : 0.0;
}

// Rows from the top, as the motion frame gives them, to AppKit's rows from
// the bottom.
- (NSPoint)viewPointFor:(ui::charter::Point)point {
    const double scale = [self pointsPerPixel];
    return NSMakePoint(point.first * scale,
                       self.bounds.size.height - point.second * scale);
}

- (ui::charter::Point)compositionPointFor:(NSPoint)point {
    const double scale = [self pointsPerPixel];
    if (scale <= 0.0) return {0.0, 0.0};
    return {point.x / scale, (self.bounds.size.height - point.y) / scale};
}

- (NSBezierPath*)cmPath:(const std::vector<ui::charter::Point>&)quad {
    NSBezierPath* path = [NSBezierPath bezierPath];
    for (size_t index = 0; index < quad.size(); ++index) {
        const NSPoint point = [self viewPointFor:quad[index]];
        if (index == 0)
            [path moveToPoint:point];
        else
            [path lineToPoint:point];
    }
    [path closePath];
    return path;
}

- (void)cmDrawBackground {
    const NSRect bounds = self.bounds;
    NSColor* flat = nil;
    if (self.background == CMCharterBackgroundFond) flat = self.fondColor;
    if (self.background == CMCharterBackgroundAccent) flat = self.accentColor;
    if (flat != nil) {
        [flat setFill];
        NSRectFill(bounds);
        return;
    }
    if (self.background == CMCharterBackgroundChecker) {
        [CMThemeColor(ui::theme::kSurfaceControl) setFill];
        NSRectFill(bounds);
        [CMThemeColor(ui::theme::kSurfaceControlHi) setFill];
        const auto columns =
            static_cast<NSInteger>(std::ceil(bounds.size.width / kCheckerSide));
        const auto rows = static_cast<NSInteger>(
            std::ceil(bounds.size.height / kCheckerSide));
        for (NSInteger row = 0; row < rows; ++row)
            for (NSInteger column = row % 2; column < columns; column += 2)
                NSRectFill(NSMakeRect(column * kCheckerSide, row * kCheckerSide,
                                      kCheckerSide, kCheckerSide));
        return;
    }
    // The plan: the theme's raised greys, lighter at the top, as a lit set.
    NSGradient* plan = [[NSGradient alloc]
        initWithStartingColor:CMThemeColor(ui::theme::kSurfaceControlHi)
                  endingColor:CMThemeColor(ui::theme::kSurfaceRaised)];
    [plan drawInRect:bounds angle:-90.0];
}

- (void)drawRect:(NSRect)dirtyRect {
    (void)dirtyRect;
    const NSRect bounds = self.bounds;
    [self cmDrawBackground];
    CGContextRef context = NSGraphicsContext.currentContext.CGContext;
    for (id picture in _pictures) {
        CGImageRef image = (__bridge CGImageRef)picture;
        if (image != nullptr)
            CGContextDrawImage(context, NSRectToCGRect(bounds), image);
    }
    if (self.message.length > 0) {
        NSMutableParagraphStyle* centred = [NSMutableParagraphStyle new];
        centred.alignment = NSTextAlignmentCenter;
        centred.lineBreakMode = NSLineBreakByWordWrapping;
        NSDictionary* attributes = @{
            NSFontAttributeName :
                CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular),
            NSForegroundColorAttributeName : CMTextPrimaryColor(),
            NSParagraphStyleAttributeName : centred,
        };
        const CGFloat inset = ui::theme::kSpaceS;
        const NSRect box = NSInsetRect(bounds, inset, inset);
        const NSRect needed = [self.message
            boundingRectWithSize:box.size
                         options:NSStringDrawingUsesLineFragmentOrigin
                      attributes:attributes];
        const CGFloat height = std::min(box.size.height, needed.size.height);
        [self.message
            drawWithRect:NSMakeRect(box.origin.x, NSMidY(box) - height / 2.0,
                                    box.size.width, height)
                 options:NSStringDrawingUsesLineFragmentOrigin
              attributes:attributes];
    }
    if (_zone.size() == 4) {
        NSBezierPath* zone = [self cmPath:_zone];
        const CGFloat dashes[] = {4.0, 3.0};
        [zone setLineDash:dashes count:2 phase:0.0];
        zone.lineWidth = 1.0;
        [CMThemeColor(ui::theme::kTextSecondary) setStroke];
        [zone stroke];
    }
    if (_hover.size() == 4 && _drag.size() != 4) {
        NSBezierPath* hover = [self cmPath:_hover];
        hover.lineWidth = 1.0;
        [CMThemeColor(ui::theme::kAccentHi) setStroke];
        [hover stroke];
        [CMThemeColor(ui::theme::kAccent) setFill];
        for (const ui::charter::Point& handle : _handles) {
            const NSPoint at = [self viewPointFor:handle];
            NSRectFill(NSMakeRect(at.x - kHandleSide / 2.0,
                                  at.y - kHandleSide / 2.0, kHandleSide,
                                  kHandleSide));
        }
    }
    if (_drag.size() == 4) {
        NSBezierPath* outline = [self cmPath:_drag];
        outline.lineWidth = 2.0;
        [CMThemeColor(ui::theme::kAccent) setStroke];
        [outline stroke];
    }
    NSBezierPath* edge =
        [NSBezierPath bezierPathWithRect:NSInsetRect(bounds, 0.5, 0.5)];
    edge.lineWidth = self.selected ? 2.0 : 1.0;
    [(self.selected ? CMAccentColor()
                    : CMThemeColor(ui::theme::kBorderStrong)) setStroke];
    [edge stroke];
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_tracking != nil) [self removeTrackingArea:_tracking];
    _tracking = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingMouseMoved | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveAlways | NSTrackingInVisibleRect
               owner:self
            userInfo:nil];
    [self addTrackingArea:_tracking];
}

- (ui::charter::Point)cmPointOf:(NSEvent*)event {
    return [self compositionPointFor:[self convertPoint:event.locationInWindow
                                               fromView:nil]];
}

// It takes the keyboard when clicked: a field being typed in is validated,
// and Cmd+Z then undoes the gestures of the panel, not that typing.
- (BOOL)acceptsFirstResponder {
    return YES;
}

- (void)mouseDown:(NSEvent*)event {
    [self.window makeFirstResponder:self];
    [self.delegate charterFrame:self
                    mouseDownAt:[self cmPointOf:event]
                      modifiers:event.modifierFlags];
}

- (void)mouseDragged:(NSEvent*)event {
    [self.delegate charterFrame:self
                 mouseDraggedTo:[self cmPointOf:event]
                      modifiers:event.modifierFlags];
}

- (void)mouseUp:(NSEvent*)event {
    [self.delegate charterFrame:self
                      mouseUpAt:[self cmPointOf:event]
                      modifiers:event.modifierFlags];
}

- (void)mouseMoved:(NSEvent*)event {
    [self.delegate charterFrame:self mouseMovedTo:[self cmPointOf:event]];
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    [self.delegate charterFrameMouseExited:self];
}

@end
