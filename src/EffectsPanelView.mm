#import "EffectsPanelView.h"

#include "EffectsLibrary.h"
#include "UiComponents.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

#include <vector>

NSPasteboardType const kCutmachineEffectPasteboardType =
    @"com.cutmachine.effect";

namespace {

// One line of the list: a category's title, or one of its effects.
struct EffectRow {
    const ui::effects::Effect* effect = nullptr;
    ui::effects::Category category = ui::effects::Category::VideoTransition;
};

std::vector<EffectRow> BuildRows() {
    std::vector<EffectRow> rows;
    bool first = true;
    ui::effects::Category current = ui::effects::Category::VideoTransition;
    for (const ui::effects::Effect& effect : ui::effects::Catalog()) {
        if (first || effect.category != current) {
            rows.push_back({nullptr, effect.category});
            current = effect.category;
            first = false;
        }
        rows.push_back({&effect, effect.category});
    }
    return rows;
}

}  // namespace

@implementation CMEffectsPanelView {
    std::vector<EffectRow> _rows;
    NSTableView* _table;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if (!(self = [super initWithFrame:frame])) return nil;
    _rows = BuildRows();
    const double width = frame.size.width;
    const double height = frame.size.height;

    NSTextField* title = CMMakeSectionHeader(@"EFFETS");
    title.frame = NSMakeRect(14.0, height - 34.0, width - 28.0, 18.0);
    title.autoresizingMask = NSViewMinYMargin | NSViewWidthSizable;
    [self addSubview:title];

    _table = [[NSTableView alloc]
        initWithFrame:NSMakeRect(0.0, 0.0, width - 24.0, 300.0)];
    NSTableColumn* column = [[NSTableColumn alloc] initWithIdentifier:@"name"];
    column.width = width - 30.0;
    column.resizingMask = NSTableColumnAutoresizingMask;
    [_table addTableColumn:column];
    _table.headerView = nil;
    _table.dataSource = self;
    _table.delegate = self;
    _table.rowHeight = 26.0;
    _table.backgroundColor = CMSurfacePanelColor();
    _table.usesAlternatingRowBackgroundColors = NO;
    _table.columnAutoresizingStyle =
        NSTableViewFirstColumnOnlyAutoresizingStyle;
    _table.target = self;
    _table.doubleAction = @selector(cmApplySelected:);
    [_table setDraggingSourceOperationMask:NSDragOperationCopy forLocal:NO];
    _table.accessibilityLabel = @"Effets";

    NSScrollView* scroll = [[NSScrollView alloc]
        initWithFrame:NSMakeRect(12.0, 56.0, width - 24.0, height - 100.0)];
    scroll.autoresizingMask = NSViewHeightSizable | NSViewWidthSizable;
    scroll.documentView = _table;
    scroll.hasVerticalScroller = YES;
    scroll.borderType = NSNoBorder;
    scroll.drawsBackground = YES;
    scroll.backgroundColor = CMSurfacePanelColor();
    [self addSubview:scroll];

    NSTextField* hint = [NSTextField
        wrappingLabelWithString:@"Glissez un effet sur un raccord de la "
                                @"timeline. Double-clic : au raccord le plus "
                                @"proche de la tête de lecture."];
    hint.frame = NSMakeRect(14.0, 8.0, width - 28.0, 40.0);
    hint.autoresizingMask = NSViewMaxYMargin | NSViewWidthSizable;
    hint.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    hint.textColor = NSColor.secondaryLabelColor;
    [self addSubview:hint];
    return self;
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return static_cast<NSInteger>(_rows.size());
}

- (BOOL)tableView:(NSTableView*)tableView isGroupRow:(NSInteger)row {
    (void)tableView;
    return row >= 0 && row < static_cast<NSInteger>(_rows.size()) &&
           _rows[static_cast<size_t>(row)].effect == nullptr;
}

- (BOOL)tableView:(NSTableView*)tableView shouldSelectRow:(NSInteger)row {
    return ![self tableView:tableView isGroupRow:row];
}

- (NSView*)tableView:(NSTableView*)tableView
    viewForTableColumn:(NSTableColumn*)tableColumn
                   row:(NSInteger)row {
    (void)tableView;
    (void)tableColumn;
    if (row < 0 || row >= static_cast<NSInteger>(_rows.size())) return nil;
    const EffectRow& line = _rows[static_cast<size_t>(row)];
    NSTextField* label = [NSTextField
        labelWithString:
            [NSString stringWithUTF8String:line.effect != nullptr
                                               ? line.effect->title
                                               : ui::effects::CategoryTitle(
                                                     line.category)]];
    if (line.effect != nullptr) {
        label.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightRegular);
        label.toolTip = [NSString stringWithUTF8String:line.effect->help];
    } else {
        label.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightSemibold);
        label.textColor = NSColor.secondaryLabelColor;
    }
    label.lineBreakMode = NSLineBreakByTruncatingTail;
    return label;
}

// The drag carries the effect's id; the timeline decides where it lands.
- (id<NSPasteboardWriting>)tableView:(NSTableView*)tableView
              pasteboardWriterForRow:(NSInteger)row {
    (void)tableView;
    if (row < 0 || row >= static_cast<NSInteger>(_rows.size())) return nil;
    const ui::effects::Effect* effect = _rows[static_cast<size_t>(row)].effect;
    if (effect == nullptr) return nil;
    NSPasteboardItem* item = [[NSPasteboardItem alloc] init];
    [item setString:[NSString stringWithUTF8String:effect->id]
            forType:kCutmachineEffectPasteboardType];
    return item;
}

- (void)cmApplySelected:(id)sender {
    (void)sender;
    const NSInteger row =
        _table.clickedRow >= 0 ? _table.clickedRow : _table.selectedRow;
    if (row < 0 || row >= static_cast<NSInteger>(_rows.size())) return;
    const ui::effects::Effect* effect = _rows[static_cast<size_t>(row)].effect;
    if (effect == nullptr) return;
    [self.delegate effectsPanel:self
              applyEffectWithId:[NSString stringWithUTF8String:effect->id]];
}

@end
