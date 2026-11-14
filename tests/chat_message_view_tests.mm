#import "ChatMessageView.h"
#import "UiThemeAppKit.h"

#include "UiTheme.h"

#include <cmath>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {
int failures = 0;
void Check(bool condition, const char* message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

NSView* Find(NSView* root, NSString* identifier) {
    if ([root.identifier isEqualToString:identifier]) {
        return root;
    }
    for (NSView* child in root.subviews) {
        NSView* found = Find(child, identifier);
        if (found != nil) {
            return found;
        }
    }
    return nil;
}
}  // namespace

int main(int argc, char** argv) {
    @autoreleasepool {
        [NSApplication sharedApplication];
        NSString* text =
            @"Agent : **Film livré**, 59 secondes.\n\n"
            @"Ouvrir `/tmp/Un dossier/export.mp4`.\n\n"
            @"À revoir : un mot à 8,5 s, ainsi que le logo visible "
            @"sur la cape dans le plan d’ouverture.\n\n"
            @"```json\n{\"compte_rendu\":1,\"propos\":\"Film\"}\n```";
        CMChatRowView* row =
            [[CMChatRowView alloc] initWithText:text
                                          style:CMChatRowStyleAssistant];
        const CGFloat collapsed = [row layoutForWidth:280];
        NSTextView* body = (NSTextView*)Find(row, @"chat.message.body");
        NSTextView* report = (NSTextView*)Find(row, @"chat.message.report");
        NSButton* button = (NSButton*)Find(row, @"chat.message.report_toggle");
        Check(body.selectable && !body.editable,
              "the readable reply can be selected and copied");
        Check(![body.string containsString:@"**"] &&
                  [body.string containsString:@"Film livré"] &&
                  [body.string containsString:@"À revoir"],
              "Markdown emphasis renders without losing the warning");
        Check([body.string containsString:@"export.mp4"] &&
                  ![body.string containsString:@"/tmp/Un dossier"],
              "the long delivery path is shown as a filename");
        const NSRange link = [body.string rangeOfString:@"export.mp4"];
        NSURL* url = link.location != NSNotFound
                         ? [body.textStorage attribute:NSLinkAttributeName
                                               atIndex:link.location
                                        effectiveRange:nil]
                         : nil;
        Check([url.path isEqualToString:@"/tmp/Un dossier/export.mp4"],
              "the filename opens the exact original path, spaces included");
        Check(report.hidden && !button.hidden &&
                  ![body.string containsString:@"compte_rendu"],
              "the machine report starts folded with an explicit control");
        __block int resized = 0;
        [row setResizeHandler:^{
          ++resized;
        }];
        [button performClick:nil];
        const CGFloat expanded = [row layoutForWidth:280];
        Check(!report.hidden && expanded > collapsed && resized == 1 &&
                  [report.string containsString:@"\"compte_rendu\":1"],
              "the report is reachable in full and expansion requests layout");
        [button performClick:nil];
        Check(report.hidden &&
                  std::abs([row layoutForWidth:280] - collapsed) < 0.1,
              "folding the report restores the original row height");
        const CGFloat narrow = [row layoutForWidth:220];
        const CGFloat wide = [row layoutForWidth:500];
        if (!(narrow > wide)) {
            std::cerr << "Row heights: narrow=" << narrow << " wide=" << wide
                      << '\n';
        }
        Check(narrow > wide && std::isfinite(narrow),
              "wrapped text remeasures when the dock is resized");

        // A deterministic preview uses the actual renderer, without an
        // agent, a network call, a project or a key window. Its input may be
        // a private pasted message; no such text belongs in the repository.
        if (argc == 3) {
            NSError* error = nil;
            NSString* input =
                [NSString stringWithContentsOfFile:@(argv[1])
                                          encoding:NSUTF8StringEncoding
                                             error:&error];
            Check(input != nil, "preview message can be read");
            [row cmSetText:input ?: text];
            const CGFloat height = [row layoutForWidth:480];
            NSWindow* window = [[NSWindow alloc]
                initWithContentRect:NSMakeRect(0, 0, 480, height)
                          styleMask:NSWindowStyleMaskBorderless
                            backing:NSBackingStoreBuffered
                              defer:NO];
            window.contentView = row;
            window.backgroundColor = CMSurfacePanelColor();
            row.layer.backgroundColor = CMSurfacePanelColor().CGColor;
            [row display];
            NSBitmapImageRep* bitmap =
                [row bitmapImageRepForCachingDisplayInRect:row.bounds];
            [row cacheDisplayInRect:row.bounds toBitmapImageRep:bitmap];
            NSData* png =
                [bitmap representationUsingType:NSBitmapImageFileTypePNG
                                     properties:@{}];
            Check([png writeToFile:@(argv[2]) atomically:YES],
                  "preview image is written");
        }
    }
    return failures == 0 ? 0 : 1;
}
