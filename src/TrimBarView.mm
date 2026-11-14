#import "TrimBarView.h"

#include <algorithm>
#include <string>
#include <vector>

#import "UiComponents.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

constexpr CGFloat kButtonHeight = 20.0;
constexpr CGFloat kButtonMinimum = 26.0;
constexpr CGFloat kButtonPadding = 14.0;
constexpr CGFloat kGap = 2.0;
constexpr CGFloat kGroupGap = 10.0;

// Where a group of buttons ends, for the wider gap after it: the sides,
// the nudges, then Extend and the dynamic trim, then the way out.
bool EndsGroup(const std::string& action) {
    return action == "side.b" || action == "nudge.+5" || action == "dynamic" ||
           action == "play.around";
}

// What may go when the row is short: the label first, then Extend and the
// dynamic trim, whose keys and menu entries stay.
int Dispensable(const std::string& action) {
    return action == "extend" || action == "dynamic" || action == "play.around"
               ? 1
               : 0;
}

}  // namespace

@implementation CMTrimBarView {
    NSTextField* _label;
    NSMutableArray<NSButton*>* _buttons;
    std::vector<std::string> _actions;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _label = [NSTextField labelWithString:@""];
        _label.font = CMFont(ui::theme::kFontSizeCaption, NSFontWeightSemibold);
        _label.textColor = CMAccentColor();
        _label.accessibilityLabel = @"Mode trim";
        [self addSubview:_label];
        _buttons = [NSMutableArray array];
        self.hidden = YES;
    }
    return self;
}

// A click beside the buttons is the tool row's.
- (NSView*)hitTest:(NSPoint)point {
    NSView* hit = [super hitTest:point];
    return hit == self || hit == _label ? nil : hit;
}

- (void)showModel:(const trim::TrimBarModel&)model {
    self.hidden = !model.shown;
    if (!model.shown) return;
    _label.stringValue = [NSString stringWithUTF8String:model.label.c_str()];
    std::vector<std::string> actions;
    for (const trim::TrimBarButton& button : model.buttons)
        actions.push_back(button.action);
    if (actions != _actions) {
        for (NSButton* button in _buttons) [button removeFromSuperview];
        [_buttons removeAllObjects];
        for (const trim::TrimBarButton& item : model.buttons) {
            NSString* title =
                [NSString stringWithUTF8String:item.title.c_str()];
            const bool toggles =
                item.action.rfind("side.", 0) == 0 || item.action == "dynamic";
            NSButton* button =
                toggles
                    ? CMMakeStyledToggle(title, self, @selector(cmPressed:))
                    : CMMakeStyledButton(title, self, @selector(cmPressed:));
            button.controlSize = NSControlSizeSmall;
            button.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
            button.refusesFirstResponder = YES;
            button.identifier =
                [NSString stringWithUTF8String:item.action.c_str()];
            [self addSubview:button];
            [_buttons addObject:button];
        }
        _actions = actions;
    }
    for (size_t index = 0; index < model.buttons.size(); ++index) {
        const trim::TrimBarButton& item = model.buttons[index];
        NSButton* button = _buttons[index];
        NSString* help = [NSString stringWithUTF8String:item.help.c_str()];
        button.toolTip = help;
        button.accessibilityLabel = help;
        button.enabled = item.enabled;
        button.state = item.on ? NSControlStateValueOn : NSControlStateValueOff;
    }
    [self cmLayOut];
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayOut];
}

// Left to right, each button as wide as its title; on a short row the
// label goes first, then Extend and the dynamic trim.
- (void)cmLayOut {
    const NSSize size = self.bounds.size;
    const CGFloat y = std::floor((size.height - kButtonHeight) / 2.0);
    std::vector<CGFloat> widths;
    for (NSButton* button in _buttons) {
        const NSSize title = [button.attributedTitle size];
        widths.push_back(
            std::max(kButtonMinimum, std::ceil(title.width) + kButtonPadding));
    }
    const auto needed = [&](bool label, int dropped) {
        CGFloat total =
            label ? std::ceil([_label.attributedStringValue size].width) +
                        kGroupGap
                  : 0.0;
        for (size_t index = 0; index < widths.size(); ++index) {
            if (Dispensable(_actions[index]) && dropped > 0) continue;
            total +=
                widths[index] + (EndsGroup(_actions[index]) ? kGroupGap : kGap);
        }
        return total;
    };
    bool showLabel = needed(true, 0) <= size.width;
    const int dropped = needed(showLabel, 0) <= size.width ? 0 : 1;
    CGFloat x = 0.0;
    _label.hidden = !showLabel;
    if (showLabel) {
        const CGFloat width =
            std::ceil([_label.attributedStringValue size].width) + 2.0;
        _label.frame = NSMakeRect(x, y + 2.0, width, kButtonHeight - 4.0);
        x += width + kGroupGap - 2.0;
    }
    for (size_t index = 0; index < widths.size(); ++index) {
        NSButton* button = _buttons[index];
        const bool hide = Dispensable(_actions[index]) && dropped > 0;
        button.hidden = hide || x + widths[index] > size.width;
        if (button.hidden) continue;
        button.frame = NSMakeRect(x, y, widths[index], kButtonHeight);
        x += widths[index] + (EndsGroup(_actions[index]) ? kGroupGap : kGap);
    }
}

- (nullable NSButton*)buttonForAction:(NSString*)action {
    for (NSButton* button in _buttons)
        if ([button.identifier isEqualToString:action]) return button;
    return nil;
}

- (void)cmPressed:(NSButton*)sender {
    if (sender.identifier.length > 0)
        [self.delegate trimBar:self didPress:sender.identifier];
}

@end
