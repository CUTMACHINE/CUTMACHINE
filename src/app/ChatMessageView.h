#pragma once

// ACP8 -- docs/roadmap/ACP-2026-09.md. The panel's readable message row,
// shared with its AppKit checks and capture preview. Layout uses frames;
// the original transcript remains the session's and the log's, unchanged.

#import <AppKit/AppKit.h>

typedef NS_ENUM(NSInteger, CMChatRowStyle) {
    CMChatRowStyleUser,
    CMChatRowStyleAssistant,
    CMChatRowStyleToolCall,
    CMChatRowStyleToolResultOk,
    CMChatRowStyleToolResultError,
    CMChatRowStyleError,
    CMChatRowStylePlan,
    CMChatRowStyleAgentJournal,
    CMChatRowStyleAgentJournalRefused,
};

@interface CMChatRowView : NSView
- (instancetype)initWithText:(NSString*)text style:(CMChatRowStyle)style;
- (void)cmSetText:(NSString*)text;
- (void)cmSetImage:(NSImage*)image;
- (void)setResizeHandler:(void (^)(void))handler;
- (CGFloat)layoutForWidth:(CGFloat)width;
@end
