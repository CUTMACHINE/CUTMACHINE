#import "TrimCursors.h"

namespace {

constexpr CGFloat kSize = 24.0;

// A bracket opening toward `direction` (-1 left, +1 right) at `x`, its
// serifs four points long, from y 5 to 19.
void AddBracket(NSBezierPath* path, CGFloat x, CGFloat direction) {
    [path moveToPoint:NSMakePoint(x + 4.0 * direction, 5.0)];
    [path lineToPoint:NSMakePoint(x, 5.0)];
    [path lineToPoint:NSMakePoint(x, 19.0)];
    [path lineToPoint:NSMakePoint(x + 4.0 * direction, 19.0)];
}

// A double arrow across the cursor, at mid height.
void AddArrow(NSBezierPath* path) {
    [path moveToPoint:NSMakePoint(2.0, 12.0)];
    [path lineToPoint:NSMakePoint(22.0, 12.0)];
    [path moveToPoint:NSMakePoint(5.0, 9.0)];
    [path lineToPoint:NSMakePoint(2.0, 12.0)];
    [path lineToPoint:NSMakePoint(5.0, 15.0)];
    [path moveToPoint:NSMakePoint(19.0, 9.0)];
    [path lineToPoint:NSMakePoint(22.0, 12.0)];
    [path lineToPoint:NSMakePoint(19.0, 15.0)];
}

NSCursor* MakeCursor(CMTrimCursorKind kind) {
    NSImage* image = [NSImage imageWithSize:NSMakeSize(kSize, kSize)
                                    flipped:NO
                             drawingHandler:^BOOL(NSRect rect) {
                               (void)rect;
                               NSBezierPath* path = [NSBezierPath bezierPath];
                               switch (kind) {
                                   case CMTrimCursorOut:
                                       AddBracket(path, 12.0, -1.0);
                                       break;
                                   case CMTrimCursorIn:
                                       AddBracket(path, 12.0, 1.0);
                                       break;
                                   case CMTrimCursorRoll:
                                       AddBracket(path, 10.0, -1.0);
                                       AddBracket(path, 14.0, 1.0);
                                       break;
                               }
                               AddArrow(path);
                               path.lineCapStyle = NSLineCapStyleRound;
                               path.lineJoinStyle = NSLineJoinStyleRound;
                               // A dark outline under a white stroke: legible
                               // over the dark timeline and over a light
                               // picture alike.
                               [NSColor.blackColor setStroke];
                               path.lineWidth = 3.5;
                               [path stroke];
                               [NSColor.whiteColor setStroke];
                               path.lineWidth = 1.5;
                               [path stroke];
                               return YES;
                             }];
    return
        [[NSCursor alloc] initWithImage:image
                                hotSpot:NSMakePoint(kSize / 2.0, kSize / 2.0)];
}

}  // namespace

NSCursor* CMTrimCursor(CMTrimCursorKind kind) {
    static NSCursor* cursors[3] = {nil, nil, nil};
    const NSInteger index = kind;
    if (index < 0 || index > 2) return NSCursor.resizeLeftRightCursor;
    if (cursors[index] == nil) cursors[index] = MakeCursor(kind);
    return cursors[index];
}
