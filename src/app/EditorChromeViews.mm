// DC3 -- docs/roadmap/DECOUPAGE-2026-10.md. The editor window's own pieces:
// the split view's dividers, the full-screen video window, and the cartons
// laid over Record. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation CutmachineSplitView
- (CGFloat)dividerThickness {
    return 7.0;
}
- (void)drawDividerInRect:(NSRect)rect {
    [CMThemeColor(ui::theme::kSurfaceBase) setFill];
    NSRectFill(rect);
    [CMThemeColor(ui::theme::kBorderStrong) setFill];
    if (self.isVertical) {
        const CGFloat x = NSMidX(rect) - 0.5;
        NSRectFill(NSMakeRect(x, NSMidY(rect) - 18.0, 1.0, 36.0));
    } else {
        const CGFloat y = NSMidY(rect) - 0.5;
        NSRectFill(NSMakeRect(NSMidX(rect) - 24.0, y, 48.0, 1.0));
    }
}
@end

@implementation VideoFullscreenWindow
- (BOOL)canBecomeKeyWindow {
    return YES;
}
- (BOOL)canBecomeMainWindow {
    return YES;
}
@end

// LG5 -- docs/roadmap/LEGENDES-2026-10.md.
@implementation CMCaptionView
- (instancetype)initWithFrame:(NSRect)frame {
    self = [super initWithFrame:frame];
    if (self) {
        // Layer-hosting: AppKit never redraws over the picture set here.
        CALayer* layer = [CALayer layer];
        // Nor animates it: a hosted layer keeps Core Animation's implicit
        // actions, and a new carton faded in over a quarter of a second
        // with the old one still showing through (window test, ITW4, 9
        // October 2026). A carton changes on its frame, as in the file.
        NSNull* none = NSNull.null;
        layer.actions = @{
            @"contents" : none,
            @"bounds" : none,
            @"position" : none,
            @"hidden" : none,
            @"onOrderIn" : none,
            @"onOrderOut" : none,
        };
        layer.contentsGravity = kCAGravityResize;
        layer.magnificationFilter = kCAFilterLinear;
        layer.minificationFilter = kCAFilterLinear;
        self.layer = layer;
        self.wantsLayer = YES;
        _captionText = @"";
        _drawnKey = @"";
    }
    return self;
}
- (NSView*)hitTest:(NSPoint)point {
    return nil;
}
- (BOOL)isAccessibilityElement {
    return !self.hidden && self.captionText.length > 0;
}
- (NSAccessibilityRole)accessibilityRole {
    return NSAccessibilityStaticTextRole;
}
- (id)accessibilityValue {
    return self.captionText;
}
@end
