#import "WorkspaceBar.h"

#include <algorithm>

#import "UiComponents.h"
#include "UiTheme.h"
#include "UiThemeAppKit.h"

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

constexpr CGFloat kTabWidth = 96.0;
constexpr CGFloat kTabHeight = 26.0;
constexpr CGFloat kToggleWidth = 104.0;
constexpr CGFloat kToggleHeight = 22.0;

}  // namespace

@implementation CMWorkspaceBarView {
    CMTabStripView* _pages;
    NSButton* _mediaToggle;
    NSButton* _inspectorToggle;
    NSButton* _agentToggle;
    BOOL _showing;
}

- (instancetype)initWithFrame:(NSRect)frame {
    if ((self = [super initWithFrame:frame])) {
        NSMutableArray<NSString*>* titles = [NSMutableArray array];
        for (const ui::workspace::PageDescriptor& page : ui::workspace::Pages())
            [titles addObject:@(page.title)];
        _pages = [[CMTabStripView alloc]
            initWithFrame:NSMakeRect(0.0, 0.0, kTabWidth * titles.count,
                                     kTabHeight)
                   titles:titles];
        _pages.target = self;
        _pages.action = @selector(cmPageChosen:);
        _pages.accessibilityLabel = @"Onglets";
        [self addSubview:_pages];

        _mediaToggle = CMMakeStyledToggle(@"Médiathèque", self,
                                          @selector(cmPanelToggled:));
        _mediaToggle.tag = static_cast<NSInteger>(ui::workspace::Side::Media);
        _mediaToggle.toolTip = @"Montrer ou cacher la médiathèque";
        _inspectorToggle =
            CMMakeStyledToggle(@"Inspecteur", self, @selector(cmPanelToggled:));
        _inspectorToggle.tag =
            static_cast<NSInteger>(ui::workspace::Side::Inspector);
        _inspectorToggle.toolTip = @"Montrer ou cacher l'Inspecteur";
        _agentToggle =
            CMMakeStyledToggle(@"Agent", self, @selector(cmAgentToggled:));
        [self showAgentShown:NO unread:NO key:@""];
        for (NSButton* toggle in
             @[ _mediaToggle, _inspectorToggle, _agentToggle ]) {
            toggle.controlSize = NSControlSizeSmall;
            toggle.font = CMFont(ui::theme::kFontSizeSmall, NSFontWeightMedium);
            [self addSubview:toggle];
        }
        [self cmLayoutForSize:frame.size];
    }
    return self;
}

- (void)setStatusLabel:(NSTextField*)statusLabel {
    [_statusLabel removeFromSuperview];
    _statusLabel = statusLabel;
    if (statusLabel) {
        statusLabel.autoresizingMask = NSViewNotSizable;
        [self addSubview:statusLabel];
    }
    [self cmLayoutForSize:self.bounds.size];
}

// The toggles at the right edge, the Agent's apart; the pages centred, or
// moved left of the toggles on a narrow window (the editor's 900 points
// leave less than the 992 a centre would need); and the status line in
// what is left on the left. No autoresizing: each resize lays it out again.
- (void)cmLayoutForSize:(NSSize)size {
    const CGFloat inset = CGF(ui::theme::kSpaceL);
    const CGFloat gap = CGF(ui::theme::kSpaceS);
    const NSSize pages = _pages.frame.size;
    const CGFloat togglesLeft =
        size.width - 2.0 * inset - 3.0 * kToggleWidth - gap;
    const CGFloat pagesX =
        std::min(std::floor((size.width - pages.width) / 2.0),
                 togglesLeft - inset - pages.width);
    _pages.frame =
        NSMakeRect(pagesX, std::floor((size.height - pages.height) / 2.0),
                   pages.width, pages.height);
    CGFloat right = size.width - inset - kToggleWidth;
    _agentToggle.frame =
        NSMakeRect(right, std::floor((size.height - kToggleHeight) / 2.0),
                   kToggleWidth, kToggleHeight);
    right -= inset;
    for (NSButton* toggle : @[ _inspectorToggle, _mediaToggle ]) {
        right -= kToggleWidth;
        toggle.frame =
            NSMakeRect(right, std::floor((size.height - kToggleHeight) / 2.0),
                       kToggleWidth, kToggleHeight);
        right -= gap;
    }
    if (_statusLabel) {
        const CGFloat labelHeight = 18.0;
        _statusLabel.frame = NSMakeRect(
            inset, std::floor((size.height - labelHeight) / 2.0),
            std::max<CGFloat>(0.0, pagesX - inset - gap), labelHeight);
    }
}

- (void)setFrameSize:(NSSize)newSize {
    [super setFrameSize:newSize];
    [self cmLayoutForSize:newSize];
}

- (void)showPage:(ui::workspace::Page)page
          panels:(const ui::workspace::PanelSet&)panels {
    _showing = YES;
    const auto& all = ui::workspace::Pages();
    for (size_t index = 0; index < all.size(); ++index)
        if (all[index].page == page)
            [_pages selectIndex:static_cast<NSInteger>(index)];
    _showing = NO;
    _mediaToggle.state =
        panels.media ? NSControlStateValueOn : NSControlStateValueOff;
    _inspectorToggle.state =
        panels.inspector ? NSControlStateValueOn : NSControlStateValueOff;
}

- (NSButton*)agentToggle {
    return _agentToggle;
}

- (void)showAgentShown:(BOOL)shown unread:(BOOL)unread key:(NSString*)key {
    _agentToggle.state = shown ? NSControlStateValueOn : NSControlStateValueOff;
    const BOOL marked = unread && !shown;
    _agentToggle.title = marked ? @"Agent •" : @"Agent";
    NSString* help = marked
                         ? @"L'agent a écrit depuis que sa fenêtre est cachée"
                         : @"Montrer ou cacher la fenêtre de l'agent";
    _agentToggle.toolTip =
        key.length > 0 ? [NSString stringWithFormat:@"%@ · %@", help, key]
                       : help;
    _agentToggle.accessibilityLabel = _agentToggle.toolTip;
}

- (void)cmPageChosen:(CMTabStripView*)strip {
    if (_showing) return;
    const auto& all = ui::workspace::Pages();
    const NSInteger index = strip.selectedIndex;
    if (index < 0 || index >= static_cast<NSInteger>(all.size())) return;
    [self.delegate workspaceBar:self
                  didChoosePage:all[static_cast<size_t>(index)].page];
}

- (void)cmAgentToggled:(NSButton*)sender {
    (void)sender;
    [self.delegate workspaceBarDidToggleAgent:self];
}

- (void)cmPanelToggled:(NSButton*)sender {
    [self.delegate workspaceBar:self
                 didTogglePanel:static_cast<ui::workspace::Side>(sender.tag)];
}

@end
