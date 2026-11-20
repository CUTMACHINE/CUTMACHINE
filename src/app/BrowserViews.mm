// DC3 -- docs/roadmap/DECOUPAGE-2026-10.md. The views of the media browser
// and of the panels' lists: bin outline, list rows and cells, icon grid,
// inline rename, icon tiles. Moved out of main.mm unchanged.

#include "AppDelegatePrivate.h"

@implementation ContextOutlineView
// PRD37 -- ROADMAP.md. A bin's cell ends at the list's visible edge. The
// outline is wider than the list it scrolls in (the rows' insets, the
// indentation: 144 points of outline in a list of 112, measured on 3
// October 2026), and a name left to that width ran past the edge and was
// cut there, « 1_RUSH », instead of ending in an ellipsis.
- (NSRect)frameOfCellAtColumn:(NSInteger)column row:(NSInteger)row {
    NSRect frame = [super frameOfCellAtColumn:column row:row];
    NSClipView* clip = self.enclosingScrollView.contentView;
    if (clip == nil) return frame;
    const CGFloat edge = NSMaxX([self convertRect:clip.bounds fromView:clip]);
    if (NSMaxX(frame) > edge)
        frame.size.width = std::max<CGFloat>(0.0, edge - NSMinX(frame));
    return frame;
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0)
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    else {
        for (NSInteger candidate = 0; candidate < self.numberOfRows;
             ++candidate) {
            if ([[self itemAtRow:candidate] isEqual:@"__root__"]) {
                [self selectRowIndexes:[NSIndexSet indexSetWithIndex:candidate]
                    byExtendingSelection:NO];
                break;
            }
        }
    }
    return [super menuForEvent:event];
}
@end

@implementation ContextTableView
- (NSMenu*)menuForEvent:(NSEvent*)event {
    const NSInteger row = [self
        rowAtPoint:[self convertPoint:event.locationInWindow fromView:nil]];
    if (row >= 0 && ![self.selectedRowIndexes containsIndex:row])
        [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
            byExtendingSelection:NO];
    return [super menuForEvent:event];
}
- (void)mouseDown:(NSEvent*)event {
    NSView* content = self.window.contentView;
    NSView* hit = [content hitTest:[content convertPoint:event.locationInWindow
                                                fromView:nil]];
    for (NSView* candidate = hit; candidate && candidate != self;
         candidate = candidate.superview) {
        if (![candidate isKindOfClass:NSTextField.class]) continue;
        NSTextField* field = (NSTextField*)candidate;
        if (![field.identifier hasPrefix:@"browser:"] || !field.editable) break;
        const NSInteger row = [self rowForView:field];
        if (row >= 0)
            [self selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
        [field selectText:field];
        return;
    }
    [super mouseDown:event];
}
@end

@implementation CMListCellView
- (void)cmShowRowSelected:(BOOL)selected {
    self.textField.textColor = CMThemeColor(ui::theme::ListRowLabel(selected));
}
@end

@implementation CMIndustrialTableRowView {
    NSTrackingArea* _trackingArea;
    BOOL _hovered;
    CALayer* _hoverEdge;
}

- (instancetype)initWithFrame:(NSRect)frameRect {
    if ((self = [super initWithFrame:frameRect])) {
        self.wantsLayer = YES;
        _hoverEdge = [CALayer layer];
        _hoverEdge.backgroundColor = CMThemeColor(ui::theme::kAccent).CGColor;
        [self.layer addSublayer:_hoverEdge];
    }
    return self;
}

- (BOOL)wantsUpdateLayer {
    return YES;
}

- (void)updateTrackingAreas {
    [super updateTrackingAreas];
    if (_trackingArea) [self removeTrackingArea:_trackingArea];
    _trackingArea = [[NSTrackingArea alloc]
        initWithRect:NSZeroRect
             options:NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited |
                     NSTrackingActiveInActiveApp
               owner:self
            userInfo:nil];
    [self addTrackingArea:_trackingArea];
}

- (void)mouseEntered:(NSEvent*)event {
    (void)event;
    _hovered = YES;
    self.needsDisplay = YES;
}

- (void)mouseExited:(NSEvent*)event {
    (void)event;
    _hovered = NO;
    self.needsDisplay = YES;
}

- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    for (NSView* view in self.subviews)
        if ([view isKindOfClass:CMListCellView.class])
            [(CMListCellView*)view cmShowRowSelected:selected];
    self.needsDisplay = YES;
}

// PRD37 -- a cell placed in a row already selected takes its state too.
- (void)didAddSubview:(NSView*)subview {
    [super didAddSubview:subview];
    if ([subview isKindOfClass:CMListCellView.class])
        [(CMListCellView*)subview cmShowRowSelected:self.selected];
}

- (void)drawSelectionInRect:(NSRect)dirtyRect {
    (void)dirtyRect;
}

- (void)updateLayer {
    const ui::theme::Color background =
        self.selected ? ui::theme::kListRowSelected
        : _hovered    ? ui::theme::Mix(ui::theme::kSurfacePanel,
                                       ui::theme::kTextPrimary, 0.05f)
                      : ui::theme::kSurfacePanel;
    self.layer.backgroundColor = CMThemeColor(background).CGColor;
    _hoverEdge.frame = CGRectMake(0.0, 0.0, 2.0, self.bounds.size.height);
    _hoverEdge.hidden = !_hovered || self.selected;
}

@end

@implementation ContextCollectionView
- (NSIndexPath*)iconIndexPathAtPoint:(NSPoint)point {
    // The flow layout and its flipped document view can disagree about the Y
    // axis. Resolve from actual visible item views: these are the rectangles
    // AppKit displayed and the user can physically click.
    for (NSCollectionViewItem* item in self.visibleItems) {
        NSIndexPath* path = [self indexPathForItem:item];
        if (!path) continue;
        const NSRect frame = [item.view convertRect:item.view.bounds
                                             toView:self];
        if (NSPointInRect(point, frame)) return path;
    }
    return nil;
}
- (BOOL)point:(NSPoint)point isInLabelAtIndexPath:(NSIndexPath*)indexPath {
    NSCollectionViewItem* item = [self itemAtIndexPath:indexPath];
    if (!item.textField) return NO;
    const NSRect labelRect = [item.textField convertRect:item.textField.bounds
                                                  toView:self];
    return NSPointInRect(point, labelRect);
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSIndexPath* indexPath =
        [self iconIndexPathAtPoint:[self convertPoint:event.locationInWindow
                                             fromView:nil]];
    if (indexPath && ![self.selectionIndexPaths containsObject:indexPath])
        self.selectionIndexPaths = [NSSet setWithObject:indexPath];
    return [super menuForEvent:event];
}
- (void)mouseDown:(NSEvent*)event {
    if (AppTestHooks* hooks = TestHooks()) hooks->MediaIconPressed();
    const NSPoint point = [self convertPoint:event.locationInWindow
                                    fromView:nil];
    // Always prefer current on-screen geometry. forwardedMouseDownPath is
    // only a fallback because NSCollectionView can reuse an item's view
    // across reloadData, leaving a previously forwarded path stale.
    NSIndexPath* path =
        [self iconIndexPathAtPoint:point] ?: self.forwardedMouseDownPath;
    self.forwardedMouseDownPath = nil;
    if (!path) {
        self.selectionIndexPaths = [NSSet set];
        return;
    }
    const BOOL renameClick =
        event.clickCount == 1 && [self point:point isInLabelAtIndexPath:path];
    NSSet<NSIndexPath*>* previous = self.selectionIndexPaths;
    const BOOL commandPressed =
        (event.modifierFlags & NSEventModifierFlagCommand) != 0;
    const BOOL shiftPressed =
        (event.modifierFlags & NSEventModifierFlagShift) != 0;
    const BOOL deferSingleSelection = !commandPressed && !shiftPressed &&
                                      previous.count > 1 &&
                                      [previous containsObject:path];
    NSSet<NSIndexPath*>* selection = [NSSet setWithObject:path];
    if (deferSingleSelection) {
        // Keep the group intact until AppKit distinguishes a click from a
        // drag. A plain mouse-up still collapses to the clicked item.
        selection = previous;
    } else if (shiftPressed && self.allowsMultipleSelection &&
               (self.rangeSelectionAnchor || previous.count > 0)) {
        NSIndexPath* anchor = self.rangeSelectionAnchor ?: previous.anyObject;
        const NSUInteger first = std::min(anchor.item, path.item);
        const NSUInteger last = std::max(anchor.item, path.item);
        NSMutableSet<NSIndexPath*>* range =
            commandPressed ? [previous mutableCopy] : [NSMutableSet set];
        for (NSUInteger item = first; item <= last; ++item)
            [range addObject:[NSIndexPath indexPathForItem:item inSection:0]];
        selection = range;
    } else if (commandPressed && self.allowsMultipleSelection) {
        NSMutableSet<NSIndexPath*>* toggled = [previous mutableCopy];
        if ([toggled containsObject:path])
            [toggled removeObject:path];
        else
            [toggled addObject:path];
        selection = toggled;
    }
    self.selectionIndexPaths = selection;
    if (!shiftPressed) self.rangeSelectionAnchor = path;
    NSSet<NSIndexPath*>* added = [selection
        objectsPassingTest:^BOOL(NSIndexPath* candidate, BOOL* stop) {
          (void)stop;
          return ![previous containsObject:candidate];
        }];
    if (!renameClick && added.count > 0 &&
        [self.delegate respondsToSelector:@selector(collectionView:
                                              didSelectItemsAtIndexPaths:)])
        [self.delegate collectionView:self didSelectItemsAtIndexPaths:added];

    const NSEventMask trackingMask =
        NSEventMaskLeftMouseDragged | NSEventMaskLeftMouseUp;
    while (true) {
        NSEvent* next = [self.window nextEventMatchingMask:trackingMask];
        if (!next || next.type == NSEventTypeLeftMouseUp) {
            if (deferSingleSelection) {
                self.selectionIndexPaths = [NSSet setWithObject:path];
                self.rangeSelectionAnchor = path;
            }
            if (renameClick) {
                NSCollectionViewItem* item = [self itemAtIndexPath:path];
                if (item.textField.editable) [item.textField selectText:self];
            } else if (event.clickCount == 2 && self.doubleClickAction) {
                [NSApp sendAction:self.doubleClickAction
                               to:self.doubleClickTarget
                             from:self];
            }
            return;
        }
        const NSPoint current = [self convertPoint:next.locationInWindow
                                          fromView:nil];
        if (std::hypot(current.x - point.x, current.y - point.y) < 4.0)
            continue;
        if ([self.delegate
                respondsToSelector:@selector(collectionView:
                                       canDragItemsAtIndexPaths:withEvent:)] &&
            ![self.delegate collectionView:self
                  canDragItemsAtIndexPaths:selection
                                 withEvent:event])
            continue;
        if (![self.delegate
                respondsToSelector:@selector(collectionView:
                                       pasteboardWriterForItemAtIndexPath:)])
            continue;
        id<NSPasteboardWriting> writer = [self.delegate collectionView:self
                                    pasteboardWriterForItemAtIndexPath:path];
        if (!writer) continue;
        if ([writer isKindOfClass:NSPasteboardItem.class] &&
            selection.count > 1) {
            NSArray<NSIndexPath*>* ordered = [selection.allObjects
                sortedArrayUsingComparator:^NSComparisonResult(
                    NSIndexPath* left, NSIndexPath* right) {
                  if (left.item < right.item) return NSOrderedAscending;
                  if (left.item > right.item) return NSOrderedDescending;
                  return NSOrderedSame;
                }];
            NSMutableArray<NSString*>* mediaIds = [NSMutableArray array];
            for (NSIndexPath* selectedPath in ordered) {
                id<NSPasteboardWriting> selectedWriter =
                    [self.delegate collectionView:self
                        pasteboardWriterForItemAtIndexPath:selectedPath];
                if (![selectedWriter isKindOfClass:NSPasteboardItem.class])
                    continue;
                NSString* identifier = [(NSPasteboardItem*)selectedWriter
                    stringForType:kCutmachineMediaPasteboardType];
                if (identifier) [mediaIds addObject:identifier];
            }
            if (mediaIds.count > 1)
                [(NSPasteboardItem*)writer
                    setPropertyList:mediaIds
                            forType:kCutmachineMediaSelectionPasteboardType];
            if (AppTestHooks* hooks = TestHooks())
                hooks->MediaDragWritten(mediaIds.count);
        }
        NSCollectionViewItem* collectionItem = [self itemAtIndexPath:path];
        if (!collectionItem) continue;
        NSDraggingItem* draggingItem =
            [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
        NSPoint imageOffset = NSZeroPoint;
        NSImage* image = [self draggingImageForItemsAtIndexPaths:selection
                                                       withEvent:event
                                                          offset:&imageOffset];
        [draggingItem setDraggingFrame:collectionItem.view.frame
                              contents:image];
        if (AppTestHooks* hooks = TestHooks()) hooks->MediaDragSessionBegins();
        [self beginDraggingSessionWithItems:@[ draggingItem ]
                                      event:next
                                     source:self];
        return;
    }
}

- (NSDragOperation)draggingSession:(NSDraggingSession*)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context {
    (void)session;
    return context == NSDraggingContextWithinApplication
               ? kMediaLocalDragOperations
               : NSDragOperationCopy;
}

@end

@implementation BrowserRenameTextField
- (ContextCollectionView*)owningCollectionView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return (ContextCollectionView*)ancestor;
}

- (NSTableView*)owningTableView {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:NSTableView.class])
        ancestor = ancestor.superview;
    return (NSTableView*)ancestor;
}

- (void)selectBrowserObjectForEvent:(NSEvent*)event {
    if (ContextCollectionView* collection = [self owningCollectionView]) {
        NSIndexPath* path = [collection
            iconIndexPathAtPoint:[collection convertPoint:event.locationInWindow
                                                 fromView:nil]];
        if (path) collection.selectionIndexPaths = [NSSet setWithObject:path];
        return;
    }
    if (NSTableView* table = [self owningTableView]) {
        const NSInteger row = [table rowForView:self];
        if (row >= 0)
            [table selectRowIndexes:[NSIndexSet indexSetWithIndex:row]
                byExtendingSelection:NO];
    }
}

- (void)mouseDown:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    // A name is an explicit editing affordance. Enter the field editor on the
    // first click instead of waiting for NSTableView's delayed second click.
    if (event.type == NSEventTypeLeftMouseDown && event.clickCount == 1 &&
        self.editable) {
        [self selectText:self];
        return;
    }
    [super mouseDown:event];
}

- (NSMenu*)menuForEvent:(NSEvent*)event {
    [self selectBrowserObjectForEvent:event];
    if (ContextCollectionView* collection = [self owningCollectionView])
        return collection.menu;
    if (NSTableView* table = [self owningTableView]) return table.menu;
    return [super menuForEvent:event];
}
@end

@implementation MediaIconView
- (NSView*)hitTest:(NSPoint)point {
    if (!NSPointInRect(point, self.bounds)) return nil;
    // One owner for the whole tile avoids a race between NSTextField's field
    // editor and NSCollectionView's selection/drag tracking. The collection
    // resolves the label rectangle after mouse-up and starts editing there.
    return self;
}
- (void)mouseDown:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    if (ancestor) {
        ((ContextCollectionView*)ancestor).forwardedMouseDownPath =
            self.indexPath;
        [(ContextCollectionView*)ancestor mouseDown:event];
    } else
        [super mouseDown:event];
}
- (NSMenu*)menuForEvent:(NSEvent*)event {
    NSView* ancestor = self.superview;
    while (ancestor && ![ancestor isKindOfClass:ContextCollectionView.class])
        ancestor = ancestor.superview;
    return ancestor ? [(ContextCollectionView*)ancestor menuForEvent:event]
                    : [super menuForEvent:event];
}
@end

@implementation MediaIconItem
- (void)loadView {
    self.view =
        [[MediaIconView alloc] initWithFrame:NSMakeRect(0, 0, 132, 112)];
    NSImageView* image =
        [[NSImageView alloc] initWithFrame:NSMakeRect(14, 27, 104, 76)];
    image.imageScaling = NSImageScaleProportionallyUpOrDown;
    image.wantsLayer = YES;
    image.layer.backgroundColor =
        CMThemeColor(ui::theme::kSurfaceControl).CGColor;
    image.layer.cornerRadius = 0.0;
    NSTextField* label = [[BrowserRenameTextField alloc]
        initWithFrame:NSMakeRect(4, 4, 124, 19)];
    label.editable = NO;
    label.selectable = NO;
    label.bezeled = NO;
    label.drawsBackground = NO;
    label.alignment = NSTextAlignmentCenter;
    label.font = [NSFont systemFontOfSize:11.0];
    label.lineBreakMode = NSLineBreakByTruncatingMiddle;
    self.imageView = image;
    self.textField = label;
    [self.view addSubview:image];
    [self.view addSubview:label];
}
- (void)setSelected:(BOOL)selected {
    [super setSelected:selected];
    self.view.layer.backgroundColor =
        selected ? [NSColor selectedContentBackgroundColor].CGColor
                 : NSColor.clearColor.CGColor;
}
@end
