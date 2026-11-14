#pragma once

// M5 -- docs/roadmap/MOTION-2026-09.md. "No text outside the safe area, in
// 9:16 as in 16:9", and "a text that does not fit" refused rather than
// broken:
// read from the evaluated frame, where the engine has already measured every
// line with the real fonts (MotionEvaluate.h), never estimated by the agent.
//
// The safe area is the frame less a margin on each side, a fraction of its
// width and of its height: 5 % by default, EBU R 95's graphics-safe area
// (90 % of the frame).
//
// PRD38 -- docs/roadmap/PRODUIT-2026-10b.md. A uniform margin does not know
// a vertical app's interface: on 29 September 2026 the author judged "not in
// the safe zone" a 9:16 text this check passed, because Reels and TikTok cover
// the bottom with the caption and the call to action and the right edge with
// their buttons. So a zone has a margin per side, and the vertical social
// one is named (SocialVerticalMotionSafeZone) rather than left to every
// caller to rebuild from a skill.
//
// What is checked: every text, image and background a viewer sees --
// accumulated opacity at least one half, which leaves out the first frames of
// a fade-in and the last of a fade-out, where a sliding title may still
// overhang -- with its transforms applied as the renderer applies them
// (translate, rotate and scale about the box's centre, nested down the tree).
// A background that reaches the frame's edge on a side bleeds off it on
// purpose -- a card's colour, a band across the whole width -- and is not held
// to the margin on that side; one that stops between the safe area and the
// frame's edge is. Not checked: paths (M1b shapes), clip paths, and the
// per-unit transforms of animated words or characters, whose line is checked
// instead.
//
// M5b -- docs/roadmap/MOTION-2026-09.md. A node marked `decor` in its
// composition is drawn and not judged, nor are its descendants: the echoes
// of a marquee that the frame cuts on purpose, a wall of words. The author saw
// a marquee whose readable phrase was wider than the frame (6 November 2026);
// the check could not say so while the echoes failed it on every frame.
// What was left out is listed in the report (`decor`), never silently.
// Found on the same marquee: an element wholly off the frame -- the second
// band waiting beyond the right edge before it enters -- was reported at
// rest outside the zone, and so was the text its band's overflow: hidden
// still hid. Nothing of either is on screen: an element is judged on what
// its clipping ancestors and the frame leave of it, and one they leave
// nothing of is neither an issue nor a passage.
//
// Portable: no AppKit (tests/motion_templates_tests.cc).

#include "MotionEvaluate.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

inline constexpr int64_t kMotionSafeMarginPercent = 5;

// PRD38 -- one side's margin, an exact fraction of the frame: of its width
// for the left and the right side, of its height for the top and the
// bottom. A fraction rather than pixels so that one zone holds in 1080x1920
// as in 2160x3840; a fraction rather than a whole percentage because
// TikTok's rail is 140 px of a 1080 px frame, which no whole percentage
// names.
struct MotionSafeMargin {
    int64_t num = kMotionSafeMarginPercent;
    int64_t den = 100;
};

// PRD38 -- the four margins a check applies, and the name its report gives
// them: "default", "social_vertical", or "custom" for margins a caller set
// side by side.
struct MotionSafeZone {
    std::string name = "default";
    MotionSafeMargin top;
    MotionSafeMargin bottom;
    MotionSafeMargin left;
    MotionSafeMargin right;
};

// The default zone: `percent` of the frame on every side (5: EBU R 95).
MotionSafeZone DefaultMotionSafeZone(
    int64_t percent = kMotionSafeMarginPercent);

// PRD38 -- a vertical video for the social apps. Meta's Reels zone -- 14 %
// of the height at the top, 35 % at the bottom, 6 % of the width at each
// side -- with TikTok's button rail on the right: the right edge stops at
// 940 of 1080 px, 140/1080 of the width, which is tighter than the Reels
// 6 %. On 1080x1920 that is x 64.8 to 940 and y 268.8 to 1248, the
// rectangle the monter-un-reel skill and the session of 29 September 2026
// give as x 65 to 940 and y 270 to 1250 (their pixels are these
// percentages, rounded). Not read again at Meta or TikTok: when a platform
// moves its interface, this is the one place to change.
MotionSafeZone SocialVerticalMotionSafeZone();

// The zone a tool's `zone` names, "default" or "social_vertical"; false for
// any other name.
bool NamedMotionSafeZone(const std::string& name, MotionSafeZone& zone);

// DEC34 -- docs/roadmap/MOTION-2026-09.md. Whether a timeline `width` by
// `height` is 9:16 exactly (1080x1920, 2160x3840, 720x1280...), by cross
// multiplication: no aspect is rounded into vertical.
bool IsNineBySixteen(int64_t width, int64_t height);

// DEC34 -- the zone a check of a clip on a timeline applies when its caller
// names none (no zone, no margin_percent, no margins): social_vertical on a
// 9:16 timeline, decided by the author on 23 October 2026 -- a vertical film is
// watched under the apps' interface far more often than on a screen of its
// own -- and the default zone on any other. A composition checked alone has
// no timeline and keeps the default zone (McpMotionClipTools.cc).
MotionSafeZone TimelineMotionSafeZone(int64_t width, int64_t height);

// "5 % on each side" or "top 14 %, bottom 35 %, left 6 %, right 12.96 %":
// the zone as a refusal names it.
std::string DescribeMotionSafeZone(const MotionSafeZone& zone);

struct MotionSafeAreaIssue {
    // "outside_safe_area": the element's bounds leave the safe area;
    // "text_overflows_box": a line is wider than the box it was laid out in;
    // "word_split": a word too long for its box, which Core Text broke
    // between two letters -- nothing overflows, and the word reads as two.
    std::string kind;
    std::string node_id;
    std::string uid;
    // The text, or the image's source; empty for a background. For a
    // word_split, the two pieces of the word, joined by '|'.
    std::string text;
    // MM3 -- the node's place in the tree, which tells two nodes apart when
    // neither has an id nor a ULID (a composition read from a file).
    std::string path;
    // MM3 -- outside_safe_area: whether the element stood still on a frame
    // it was outside -- its bounds those of the frame checked before. A
    // title sliding through the edge is in motion; a title held there is
    // at rest.
    bool at_rest = false;
    // Which edges of the safe area are crossed: any of "left", "right",
    // "top", "bottom". Empty for an overflow.
    std::vector<std::string> edges;
    // The composition frames where it happens, first and last, and the
    // element's bounds at the first, in composition pixels.
    int64_t first_frame = 0;
    int64_t last_frame = 0;
    MotionBox bounds;
};

struct MotionSafeAreaReport {
    // PRD38 -- what used to be a single margin_percent: the default zone is
    // that margin on every side, and the report still writes margin_percent
    // whenever the four sides are one whole percentage.
    MotionSafeZone zone;
    // The safe rectangle, in composition pixels.
    MotionBox safe;
    int64_t checked_frames = 0;
    std::vector<MotionSafeAreaIssue> issues;
    // MM3 -- count only what is outside at rest: an element that crosses
    // the edge while it moves (a page sliding in, a title leaving) is a
    // passage, counted in `in_motion` instead. Overflows and split words
    // count either way.
    bool at_rest_only = false;
    int64_t in_motion = 0;
    // M5b -- the decor nodes left out, by id (by path without one), in
    // first-seen order: what the verdict does not cover.
    std::vector<std::string> decor;
    // What each element's bounds were on the frame checked last, to tell
    // still from moving (CheckMotionFrameSafeArea keeps it).
    std::map<std::string, MotionBox> previous_bounds;
};

// Adds what `frame` shows outside the safe area to `report`, merging an
// issue already seen on an earlier frame (same kind, same node) into it.
// Call it on each frame to check, in order, after setting the zone.
void CheckMotionFrameSafeArea(const MotionFrame& frame,
                              MotionSafeAreaReport& report);

// The report as the agent reads it: the zone's name and its margins in
// percent, before the rectangle and the issues.
std::string SerializeMotionSafeAreaReport(const MotionSafeAreaReport& report);
