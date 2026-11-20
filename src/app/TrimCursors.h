#pragma once

// ON9 -- docs/roadmap/ONGLETS-2026-10.md. The cursors of an edge, drawn as
// Resolve draws them: a bracket on the side that moves -- "]" for the tail
// of the outgoing clip, "[" for the head of the incoming one, "][" for a
// roll -- across a double arrow. AppKit has no such cursor; its one double
// arrow said nothing of which side a press would take (the author, 31 October
// 2026: « des fois il est en mode trim normal, des fois il bouge ce qu'il y
// a dans le plan »).

#import <AppKit/AppKit.h>

typedef NS_ENUM(NSInteger, CMTrimCursorKind) {
    CMTrimCursorOut,
    CMTrimCursorIn,
    CMTrimCursorRoll,
};

NSCursor* _Nonnull CMTrimCursor(CMTrimCursorKind kind);
