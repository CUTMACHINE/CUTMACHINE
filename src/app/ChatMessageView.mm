#import "ChatMessageView.h"

#include "ChatPresentation.h"
#include "UiTheme.h"

#import "UiComponents.h"
#import "UiThemeAppKit.h"

#include <algorithm>
#include <cmath>

namespace {

CGFloat CGF(double value) { return static_cast<CGFloat>(value); }

NSTextView* MakeTextView(NSString* identifier) {
    NSTextView* view = [[NSTextView alloc] initWithFrame:NSZeroRect];
    view.identifier = identifier;
    view.editable = NO;
    view.selectable = YES;
    view.richText = YES;
    view.drawsBackground = NO;
    view.textContainerInset = NSZeroSize;
    view.horizontallyResizable = NO;
    view.verticallyResizable = NO;
    view.minSize = NSZeroSize;
    view.maxSize = NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX);
    view.textContainer.lineFragmentPadding = 0;
    view.textContainer.widthTracksTextView = NO;
    view.textContainer.heightTracksTextView = NO;
    view.linkTextAttributes = @{
        NSForegroundColorAttributeName : CMThemeColor(ui::theme::kAccent),
        NSUnderlineStyleAttributeName : @(NSUnderlineStyleSingle)
    };
    return view;
}

NSAttributedString* MessageText(NSString* text, NSColor* color, BOOL markdown) {
    NSMutableAttributedString* result = nil;
    if (markdown) {
        if (@available(macOS 12.0, *)) {
            NSAttributedStringMarkdownParsingOptions* options =
                [NSAttributedStringMarkdownParsingOptions new];
            // Keep the author's paragraphs, list markers and line breaks;
            // Foundation's inline parser interprets emphasis, code and links.
            options.interpretedSyntax =
                NSAttributedStringMarkdownInterpretedSyntaxInlineOnlyPreservingWhitespace;
            result = [[[NSAttributedString alloc]
                initWithMarkdownString:text
                               options:options
                               baseURL:[NSURL fileURLWithPath:@"/"]
                                 error:nil] mutableCopy];
        }
    }
    if (result == nil) {
        result = [[NSMutableAttributedString alloc] initWithString:text];
    }
    const NSRange whole = NSMakeRange(0, result.length);
    NSMutableParagraphStyle* paragraph = [NSMutableParagraphStyle new];
    paragraph.lineSpacing = CGF(ui::theme::kSpaceXxs);
    paragraph.paragraphSpacing = CGF(ui::theme::kSpaceS);
    paragraph.lineBreakMode = NSLineBreakByWordWrapping;
    [result addAttributes:@{
        NSFontAttributeName :
            CMFont(ui::theme::kChatBodyFontSize, NSFontWeightRegular),
        NSForegroundColorAttributeName : color,
        NSParagraphStyleAttributeName : paragraph
    }
                    range:whole];
    if (@available(macOS 12.0, *)) {
        NSMutableArray<NSDictionary*>* paths = [NSMutableArray array];
        [result
            enumerateAttribute:NSInlinePresentationIntentAttributeName
                       inRange:whole
                       options:0
                    usingBlock:^(NSNumber* intent, NSRange range, BOOL* stop) {
                      (void)stop;
                      const NSUInteger flags = intent.unsignedIntegerValue;
                      NSFont* font = CMFont(
                          ui::theme::kChatBodyFontSize,
                          (flags &
                           NSInlinePresentationIntentStronglyEmphasized) != 0
                              ? NSFontWeightSemibold
                              : NSFontWeightRegular);
                      if ((flags & NSInlinePresentationIntentCode) != 0) {
                          font = [NSFont
                              monospacedSystemFontOfSize:
                                  CGF(ui::theme::kFontSizeBody)
                                                  weight:NSFontWeightRegular];
                          NSString* code =
                              [result.string substringWithRange:range];
                          if ([code hasPrefix:@"/"] || [code hasPrefix:@"~/"]) {
                              [paths addObject:@{
                                  @"range" : [NSValue valueWithRange:range],
                                  @"path" : code.stringByExpandingTildeInPath
                              }];
                          }
                      } else if ((flags &
                                  NSInlinePresentationIntentEmphasized) != 0) {
                          font = [NSFontManager.sharedFontManager
                              convertFont:font
                              toHaveTrait:NSItalicFontMask];
                      }
                      [result addAttribute:NSFontAttributeName
                                     value:font
                                     range:range];
                    }];
        // A file's name is what the person opens. Its full path stays in
        // the link, tooltip and original log, rather than wrapping for six
        // lines in the narrow dock. Replace in reverse to keep ranges valid.
        for (NSDictionary* path in paths.reverseObjectEnumerator) {
            const NSRange range = [path[@"range"] rangeValue];
            NSString* full = path[@"path"];
            NSMutableDictionary* attributes =
                [[result attributesAtIndex:range.location
                            effectiveRange:nil] mutableCopy];
            attributes[NSLinkAttributeName] = [NSURL fileURLWithPath:full];
            attributes[NSToolTipAttributeName] = full;
            NSAttributedString* label = [[NSAttributedString alloc]
                initWithString:full.lastPathComponent.length > 0
                                   ? full.lastPathComponent
                                   : full
                    attributes:attributes];
            [result replaceCharactersInRange:range withAttributedString:label];
        }
    }
    return result;
}

CGFloat TextHeight(NSTextView* view, CGFloat width) {
    [view setFrameSize:NSMakeSize(width, std::max<CGFloat>(
                                             1, view.frame.size.height))];
    view.textContainer.containerSize = NSMakeSize(width, CGFLOAT_MAX);
    [view.layoutManager ensureLayoutForTextContainer:view.textContainer];
    return std::ceil(
               [view.layoutManager usedRectForTextContainer:view.textContainer]
                   .size.height) +
           CGF(ui::theme::kSpaceXxs);
}

}  // namespace

@implementation CMChatRowView {
    NSView* _accentStrip;
    NSTextView* _bodyView;
    NSTextView* _reportView;
    NSButton* _reportButton;
    NSImageView* _imageView;
    CMChatRowStyle _style;
    BOOL _reportExpanded;
    void (^_resizeHandler)(void);
}

- (BOOL)isFlipped {
    return YES;
}

- (void)setResizeHandler:(void (^)(void))handler {
    _resizeHandler = [handler copy];
}

- (void)cmSetImage:(NSImage*)image {
    if (_imageView == nil) {
        _imageView = [[NSImageView alloc] initWithFrame:NSZeroRect];
        _imageView.imageScaling = NSImageScaleProportionallyUpOrDown;
        _imageView.imageAlignment = NSImageAlignLeft;
        [self addSubview:_imageView];
    }
    _imageView.image = image;
}

- (void)cmSetText:(NSString*)text {
    const bool assistant = _style == CMChatRowStyleAssistant;
    const auto shown =
        assistant ? chat::PresentChatMessage(text.UTF8String ?: "")
                  : chat::ChatMessagePresentation{text.UTF8String ?: "", {}};
    const BOOL failed = _style == CMChatRowStyleError ||
                        _style == CMChatRowStyleToolResultError ||
                        _style == CMChatRowStyleAgentJournalRefused;
    NSString* body = @(shown.markdown.c_str());
    if (body.length == 0 && !shown.technical_report.empty()) {
        body = @"Rapport technique disponible ci-dessous.";
    }
    [_bodyView.textStorage
        setAttributedString:MessageText(body,
                                        failed ? CMThemeColor(ui::theme::kError)
                                               : CMTextPrimaryColor(),
                                        assistant)];
    _reportButton.hidden = shown.technical_report.empty();
    _reportExpanded = NO;
    _reportView.hidden = YES;
    _reportButton.title = @"Afficher le rapport technique";
    [_reportView.textStorage
        setAttributedString:
            [[NSAttributedString alloc]
                initWithString:@(shown.technical_report.c_str())
                    attributes:@{
                        NSFontAttributeName : [NSFont
                            monospacedSystemFontOfSize:
                                CGF(ui::theme::kFontSizeSmall)
                                                weight:NSFontWeightRegular],
                        NSForegroundColorAttributeName : CMTextSecondaryColor()
                    }]];
}

- (instancetype)initWithText:(NSString*)text style:(CMChatRowStyle)style {
    self = [super initWithFrame:NSZeroRect];
    if (self != nil) {
        _style = style;
        self.wantsLayer = YES;
        _accentStrip = [[NSView alloc] initWithFrame:NSZeroRect];
        _accentStrip.wantsLayer = YES;
        _accentStrip.layer.backgroundColor = [self accentColor].CGColor;
        [self addSubview:_accentStrip];
        _bodyView = MakeTextView(@"chat.message.body");
        [self addSubview:_bodyView];
        _reportView = MakeTextView(@"chat.message.report");
        [self addSubview:_reportView];
        _reportButton = CMMakeStyledButton(@"Afficher le rapport technique",
                                           self, @selector(toggleReport:));
        _reportButton.identifier = @"chat.message.report_toggle";
        [self addSubview:_reportButton];
        [self cmSetText:text ?: @""];
    }
    return self;
}

- (void)toggleReport:(id)sender {
    (void)sender;
    _reportExpanded = !_reportExpanded;
    _reportView.hidden = !_reportExpanded;
    _reportButton.title = _reportExpanded ? @"Masquer le rapport technique"
                                          : @"Afficher le rapport technique";
    if (_resizeHandler != nil) {
        _resizeHandler();
    }
}

- (NSColor*)accentColor {
    switch (_style) {
        case CMChatRowStyleUser:
            return CMThemeColor(ui::theme::kAccent);
        case CMChatRowStyleAssistant:
            return CMTextSecondaryColor();
        case CMChatRowStyleToolCall:
            return CMThemeColor(ui::theme::kAccentHi);
        case CMChatRowStyleToolResultOk:
            return CMThemeColor(ui::theme::kRenderCached);
        case CMChatRowStyleToolResultError:
        case CMChatRowStyleError:
            return CMThemeColor(ui::theme::kError);
        case CMChatRowStylePlan:
            return CMThemeColor(ui::theme::kMarkIn);
        case CMChatRowStyleAgentJournal:
        case CMChatRowStyleAgentJournalRefused:
            return CMThemeColor(ui::theme::kAgentMark);
    }
    return CMTextSecondaryColor();
}

- (CGFloat)layoutForWidth:(CGFloat)width {
    const CGFloat inset = CGF(ui::theme::kSpaceM);
    const CGFloat strip = CGF(ui::theme::kChatAccentWidth);
    const CGFloat textWidth = std::max<CGFloat>(1, width - strip - 2 * inset);
    CGFloat y = inset;
    const CGFloat bodyHeight = TextHeight(_bodyView, textWidth);
    _bodyView.frame = NSMakeRect(strip + inset, y, textWidth, bodyHeight);
    y += bodyHeight;
    if (!_reportButton.hidden) {
        y += inset;
        _reportButton.frame = NSMakeRect(strip + inset, y, textWidth,
                                         CGF(ui::theme::kControlRowHeight));
        y += _reportButton.frame.size.height;
        if (_reportExpanded) {
            y += inset;
            const CGFloat height = TextHeight(_reportView, textWidth);
            _reportView.frame = NSMakeRect(strip + inset, y, textWidth, height);
            y += height;
        }
    }
    const NSSize size = _imageView.image.size;
    if (_imageView.image != nil && size.width > 0 && size.height > 0) {
        y += inset;
        const CGFloat height =
            std::min<CGFloat>(CGF(ui::theme::kChatImageMaxHeight),
                              textWidth * size.height / size.width);
        _imageView.frame = NSMakeRect(strip + inset, y, textWidth, height);
        y += height;
    }
    y += inset;
    _accentStrip.frame = NSMakeRect(0, 0, strip, y);
    self.frame = NSMakeRect(0, 0, width, y);
    return y;
}

@end
