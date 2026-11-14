#import "MotionInspectorView.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

#include "UiTheme.h"
#include "UiThemeAppKit.h"

#import "UiComponents.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSString* Text(const std::string& text) {
    return [NSString stringWithUTF8String:text.c_str()] ?: @"";
}

constexpr CGFloat kSectionHeaderHeight = 20.0;
constexpr CGFloat kLayerHeight = 18.0;
constexpr CGFloat kLayerIndent = 12.0;
constexpr CGFloat kKeyWidth = 22.0;
constexpr CGFloat kEasingWidth = 92.0;
constexpr CGFloat kSpringWidth = 64.0;
constexpr CGFloat kGap = 4.0;

// What the curve popup offers, in the vocabulary of interpolate.
NSArray<NSString*>* Easings() {
    return @[ @"linear", @"ease", @"ease-in", @"ease-out", @"ease-in-out" ];
}

// Tags: a field's index, times ten, plus the part of its row.
constexpr NSInteger kPartValue = 0;
constexpr NSInteger kPartKey = 1;
constexpr NSInteger kPartEasing = 2;
constexpr NSInteger kPartSpringOn = 3;
constexpr NSInteger kPartSpringOff = 4;
constexpr NSInteger kPartSpringFrom = 5;
constexpr NSInteger kPartSpringTo = 6;
constexpr NSInteger kPartSpringDelay = 7;
constexpr NSInteger kPartSpringDamping = 8;

NSInteger Tag(size_t field, NSInteger part) {
    return static_cast<NSInteger>(field) * 10 + part;
}

}  // namespace

@interface CMMotionInspectorBody : NSView
@end

@implementation CMMotionInspectorBody
- (BOOL)isFlipped {
    return YES;
}
@end

@implementation CMMotionInspectorView {
    std::vector<MotionField> _fields;
    NSTextField* _title;
    NSButton* _close;
    NSScrollView* _scrollView;
    CMMotionInspectorBody* _body;
    // Top-to-bottom: a section header or a row, and its height.
    std::vector<std::pair<NSView*, CGFloat>> _items;
    // Per row: the controls in its container, laid out left to right, the
    // first taking what the others leave.
    std::vector<std::vector<std::pair<NSView*, CGFloat>>> _rowControls;
    std::vector<CMControlRowView*> _rows;
    // MA6 -- the uid of each layer button, by its tag.
    std::vector<std::string> _layerUids;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _title = [NSTextField labelWithString:@""];
        _title.font = CMFont(ui::theme::kFontSizeBody, NSFontWeightSemibold);
        _title.textColor = CMTextPrimaryColor();
        _title.lineBreakMode = NSLineBreakByTruncatingTail;
        [self addSubview:_title];
        _close = CMMakeStyledButton(@"Clip", self, @selector(cmClose:));
        _close.toolTip = @"Revenir à l'Inspector du clip";
        [self addSubview:_close];
        _body = [[CMMotionInspectorBody alloc] initWithFrame:NSZeroRect];
        _scrollView = [[NSScrollView alloc] initWithFrame:NSZeroRect];
        _scrollView.hasVerticalScroller = YES;
        _scrollView.hasHorizontalScroller = NO;
        _scrollView.autohidesScrollers = YES;
        _scrollView.borderType = NSNoBorder;
        _scrollView.drawsBackground = NO;
        _scrollView.documentView = _body;
        [self addSubview:_scrollView];
    }
    return self;
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayout];
}

- (NSTextField*)cmField:(NSString*)text tag:(NSInteger)tag {
    NSTextField* field = [NSTextField textFieldWithString:text];
    field.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    field.textColor = CMTextPrimaryColor();
    field.bezeled = YES;
    field.bezelStyle = NSTextFieldSquareBezel;
    field.target = self;
    field.action = @selector(cmCommit:);
    field.tag = tag;
    // Return commits, and so does leaving the field.
    field.cell.sendsActionOnEndEditing = YES;
    return field;
}

- (NSPopUpButton*)cmPopUp:(NSArray<NSString*>*)items
                 selected:(NSString*)selected
                      tag:(NSInteger)tag
                   action:(SEL)action {
    NSPopUpButton* popup = [[NSPopUpButton alloc] initWithFrame:NSZeroRect
                                                      pullsDown:NO];
    [popup addItemsWithTitles:items];
    // A value the list does not offer (a cubic-bezier curve) is shown as it
    // is, not as the first item.
    if (selected.length > 0 && ![items containsObject:selected]) {
        [popup addItemWithTitle:selected];
    }
    if (selected.length > 0) {
        [popup selectItemWithTitle:selected];
    }
    popup.controlSize = NSControlSizeSmall;
    popup.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightRegular);
    popup.target = self;
    popup.action = action;
    popup.tag = tag;
    return popup;
}

- (NSInteger)layerCount {
    return static_cast<NSInteger>(_layerUids.size());
}

// One layer of the list: indented by its depth, a scene in the section's
// weight, the selected one in the accent colour.
- (NSButton*)cmLayer:(const MotionOutlineRow&)row
            selected:(BOOL)selected
                 tag:(NSInteger)tag {
    NSButton* button = [NSButton buttonWithTitle:@""
                                          target:self
                                          action:@selector(cmSelectLayer:)];
    button.bordered = NO;
    button.tag = tag;
    button.toolTip = Text(row.kind);
    NSMutableParagraphStyle* paragraph = [[NSMutableParagraphStyle alloc] init];
    paragraph.firstLineHeadIndent = kLayerIndent * CGF(row.depth);
    paragraph.headIndent = paragraph.firstLineHeadIndent;
    paragraph.lineBreakMode = NSLineBreakByTruncatingTail;
    NSDictionary* attributes = @{
        NSFontAttributeName : CMFont(
            ui::theme::kFontSizeSmall,
            row.scene || selected ? NSFontWeightSemibold : NSFontWeightRegular),
        NSForegroundColorAttributeName : selected ? CMAccentColor()
                                                  : CMTextPrimaryColor(),
        NSParagraphStyleAttributeName : paragraph
    };
    button.attributedTitle =
        [[NSAttributedString alloc] initWithString:Text(row.name)
                                        attributes:attributes];
    return button;
}

- (void)showTitle:(NSString*)title
          outline:(const std::vector<MotionOutlineRow>&)outline
            shown:(const std::vector<size_t>&)shown
         selected:(const std::string&)selected
           fields:(const std::vector<MotionField>&)fields {
    _fields = fields;
    _title.stringValue = title;
    for (NSView* view in [_body.subviews copy]) {
        [view removeFromSuperview];
    }
    _items.clear();
    _rowControls.clear();
    _rows.clear();
    _layerUids.clear();
    if (!shown.empty()) {
        NSTextField* header = CMMakeSectionHeader(@"Calques");
        [_body addSubview:header];
        _items.emplace_back(header, kSectionHeaderHeight);
        for (const size_t index : shown) {
            if (index >= outline.size()) {
                continue;
            }
            const MotionOutlineRow& row = outline[index];
            NSButton* layer =
                [self cmLayer:row
                     selected:!selected.empty() && row.uid == selected
                          tag:static_cast<NSInteger>(_layerUids.size())];
            _layerUids.push_back(row.uid);
            [_body addSubview:layer];
            _items.emplace_back(layer, kLayerHeight);
        }
    }
    std::string group;
    for (size_t index = 0; index < _fields.size(); ++index) {
        const MotionField& field = _fields[index];
        if (field.group != group) {
            group = field.group;
            NSTextField* header = CMMakeSectionHeader(Text(group));
            [_body addSubview:header];
            _items.emplace_back(header, kSectionHeaderHeight);
        }
        CMControlRowView* row =
            [[CMControlRowView alloc] initWithFrame:NSZeroRect
                                          labelText:Text(field.label)];
        row.valueLabel.hidden = YES;
        std::vector<std::pair<NSView*, CGFloat>> controls;
        if (field.spring) {
            // A spring: its four settings, and the way out of it.
            const std::pair<NSInteger, std::string> parts[] = {
                {kPartSpringFrom, MotionNumberValue(field.spring_from).Dump()},
                {kPartSpringTo, MotionNumberValue(field.spring_to).Dump()},
                {kPartSpringDelay, std::to_string(field.spring_delay)},
                {kPartSpringDamping,
                 MotionNumberValue(field.spring_damping).Dump()}};
            NSArray<NSString*>* tips = @[
                @"Départ", @"Arrivée", @"Retard (images)", @"Amortissement"
            ];
            for (size_t part = 0; part < 4; ++part) {
                NSTextField* setting =
                    [self cmField:Text(parts[part].second)
                              tag:Tag(index, parts[part].first)];
                setting.toolTip = tips[part];
                controls.emplace_back(setting, 0.0);
            }
            NSButton* off = CMMakeStyledButton(@"×", self, @selector(cmPart:));
            off.tag = Tag(index, kPartSpringOff);
            off.toolTip = @"Retirer le ressort";
            // PRD27 -- what VoiceOver reads instead of the glyph.
            off.accessibilityLabel = off.toolTip;
            controls.emplace_back(off, kKeyWidth);
        } else if (field.kind == MotionFieldKind::Choice) {
            NSMutableArray<NSString*>* items = [NSMutableArray array];
            for (const std::string& choice : field.choices) {
                [items addObject:Text(choice)];
            }
            controls.emplace_back([self cmPopUp:items
                                       selected:Text(field.text)
                                            tag:Tag(index, kPartValue)
                                         action:@selector(cmCommitChoice:)],
                                  0.0);
        } else {
            NSTextField* value = [self cmField:Text(field.text)
                                           tag:Tag(index, kPartValue)];
            value.editable = field.editable;
            if (!field.editable) {
                value.toolTip = @"Lié à une prop : réglez la prop";
            }
            controls.emplace_back(value, 0.0);
        }
        if (field.keyable && !field.spring) {
            NSButton* key = CMMakeStyledToggle(field.key_here ? @"◆" : @"◇",
                                               self, @selector(cmPart:));
            key.tag = Tag(index, kPartKey);
            key.state =
                field.key_here ? NSControlStateValueOn : NSControlStateValueOff;
            key.toolTip = field.key_here ? @"Retirer la clé à cette image"
                                         : @"Poser une clé à cette image";
            key.accessibilityLabel = key.toolTip;
            controls.emplace_back(key, kKeyWidth);
            if (field.animated) {
                NSPopUpButton* easing =
                    [self cmPopUp:Easings()
                         selected:Text(field.easing)
                              tag:Tag(index, kPartEasing)
                           action:@selector(cmCommitEasing:)];
                easing.toolTip = @"Courbe entre les clés";
                controls.emplace_back(easing, kEasingWidth);
            } else if (field.kind == MotionFieldKind::Number) {
                NSButton* spring =
                    CMMakeStyledButton(@"Ressort", self, @selector(cmPart:));
                spring.tag = Tag(index, kPartSpringOn);
                spring.toolTip = @"Animer par un ressort";
                controls.emplace_back(spring, kSpringWidth);
            }
        }
        for (const auto& control : controls) {
            [row.controlContainer addSubview:control.first];
        }
        [_body addSubview:row];
        _items.emplace_back(row, CGF(ui::theme::kControlRowHeight));
        _rows.push_back(row);
        _rowControls.push_back(std::move(controls));
    }
    [self cmLayout];
}

- (void)cmLayout {
    const NSSize size = self.bounds.size;
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    const CGFloat headerHeight = 28.0;
    // Rows from the bottom in this view: the header sits at the top.
    _title.frame =
        NSMakeRect(inset, size.height - headerHeight + 6.0,
                   std::max<CGFloat>(0.0, size.width - 3 * inset - 56), 18.0);
    _close.frame = NSMakeRect(size.width - inset - 52.0,
                              size.height - headerHeight + 4.0, 52.0, 20.0);
    _scrollView.frame =
        NSMakeRect(0.0, 0.0, size.width,
                   std::max<CGFloat>(0.0, size.height - headerHeight));
    const NSSize visible = _scrollView.contentSize;
    CGFloat contentHeight = 2 * inset;
    for (const auto& item : _items) {
        contentHeight += item.second + CGF(ui::theme::kSpaceXxs);
    }
    _body.frame = NSMakeRect(0.0, 0.0, visible.width,
                             std::max(contentHeight, visible.height));
    CGFloat y = inset;
    for (const auto& item : _items) {
        if ([item.first isKindOfClass:[CMControlRowView class]]) {
            CMControlRowView* row = (CMControlRowView*)item.first;
            row.frame = NSMakeRect(0.0, y, visible.width, item.second);
            // The row reserves a slot for a read-only value these rows hide:
            // the controls take it, or a keyable number keeps a few points
            // for its value once the key and spring buttons are placed.
            const NSRect slot = row.valueLabel.frame;
            const NSRect container = row.controlContainer.frame;
            row.controlContainer.frame = NSMakeRect(
                NSMinX(slot), NSMinY(container),
                NSMaxX(container) - NSMinX(slot), NSHeight(container));
        } else {
            item.first.frame = NSMakeRect(
                inset, y, std::max<CGFloat>(0.0, visible.width - 2 * inset),
                item.second);
        }
        y += item.second + CGF(ui::theme::kSpaceXxs);
    }
    // Inside each row: fixed widths as given, the rest shared by the
    // controls that asked for none.
    for (size_t row = 0; row < _rows.size(); ++row) {
        const NSRect container = _rows[row].controlContainer.bounds;
        const auto& controls = _rowControls[row];
        CGFloat fixed = 0.0;
        size_t flexible = 0;
        for (const auto& control : controls) {
            fixed += control.second;
            flexible += control.second == 0.0 ? 1 : 0;
        }
        const CGFloat gaps =
            kGap * CGF(controls.size() > 0 ? controls.size() - 1 : 0);
        const CGFloat share =
            flexible > 0
                ? std::max<CGFloat>(0.0, (container.size.width - fixed - gaps) /
                                             CGF(flexible))
                : 0.0;
        CGFloat x = 0.0;
        for (const auto& control : controls) {
            const CGFloat width =
                control.second == 0.0 ? share : control.second;
            control.first.frame =
                NSMakeRect(x, 1.0, width, container.size.height - 2.0);
            x += width + kGap;
        }
    }
}

- (const MotionField*)cmFieldForTag:(NSInteger)tag {
    const size_t index = static_cast<size_t>(tag / 10);
    return index < _fields.size() ? &_fields[index] : nullptr;
}

- (void)cmCommit:(NSTextField*)sender {
    const MotionField* field = [self cmFieldForTag:sender.tag];
    if (field == nullptr || !field->editable) {
        return;
    }
    const NSInteger part = sender.tag % 10;
    if (part >= kPartSpringFrom && part <= kPartSpringDamping) {
        [self cmCommitSpringOfTag:sender.tag];
        return;
    }
    // Unchanged text is not an edit: leaving a field does not commit one.
    if ([sender.stringValue isEqualToString:Text(field->text)]) {
        return;
    }
    [self.delegate motionInspector:self
                       commitField:Text(field->property)
                              text:sender.stringValue];
}

- (void)cmCommitChoice:(NSPopUpButton*)sender {
    const MotionField* field = [self cmFieldForTag:sender.tag];
    if (field == nullptr || sender.titleOfSelectedItem == nil) {
        return;
    }
    [self.delegate motionInspector:self
                       commitField:Text(field->property)
                              text:sender.titleOfSelectedItem];
}

- (void)cmCommitEasing:(NSPopUpButton*)sender {
    const MotionField* field = [self cmFieldForTag:sender.tag];
    if (field == nullptr || sender.titleOfSelectedItem == nil) {
        return;
    }
    [self.delegate motionInspector:self
                            easing:sender.titleOfSelectedItem
                       forProperty:Text(field->property)];
}

// The four settings of a spring row, read together.
- (void)cmCommitSpringOfTag:(NSInteger)tag {
    const size_t index = static_cast<size_t>(tag / 10);
    if (index >= _rowControls.size()) {
        return;
    }
    double values[4] = {0.0, 0.0, 0.0, 10.0};
    for (size_t part = 0; part < 4 && part < _rowControls[index].size();
         ++part) {
        NSTextField* setting = (NSTextField*)_rowControls[index][part].first;
        values[part] = setting.doubleValue;
    }
    [self.delegate motionInspector:self
                            spring:Text(_fields[index].property)
                                on:YES
                              from:values[0]
                                to:values[1]
                             delay:static_cast<NSInteger>(values[2])
                           damping:values[3]];
}

- (void)cmPart:(NSButton*)sender {
    const MotionField* field = [self cmFieldForTag:sender.tag];
    if (field == nullptr) {
        return;
    }
    NSString* property = Text(field->property);
    switch (sender.tag % 10) {
        case kPartKey:
            [self.delegate motionInspector:self toggleKey:property];
            break;
        case kPartSpringOn: {
            const double value = std::strtod(field->text.c_str(), nullptr);
            [self.delegate motionInspector:self
                                    spring:property
                                        on:YES
                                      from:value
                                        to:value
                                     delay:0
                                   damping:10.0];
            break;
        }
        case kPartSpringOff:
            [self.delegate motionInspector:self
                                    spring:property
                                        on:NO
                                      from:field->spring_to
                                        to:field->spring_to
                                     delay:0
                                   damping:field->spring_damping];
            break;
        default:
            break;
    }
}

- (void)cmSelectLayer:(NSButton*)sender {
    const auto index = static_cast<size_t>(sender.tag);
    if (index >= _layerUids.size() || _layerUids[index].empty()) {
        return;
    }
    [self.delegate motionInspector:self selectNode:Text(_layerUids[index])];
}

- (void)cmClose:(id)sender {
    (void)sender;
    [self.delegate motionInspectorDidClose:self];
}

@end
