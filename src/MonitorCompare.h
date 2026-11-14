#pragma once
// G6 -- docs/roadmap/GRADE-2026-09.md. Comparing on the Record monitor, as a
// colourist does before trusting a grade: the picture without its grade, at
// a keystroke, and a reference picture beside the live one.
//
// Both are local monitoring preferences (UiPreferences, NSUserDefaults),
// like the scopes: they change what the monitor shows, never the document,
// the export or what a tool measures.
//
//  * Without the grade: Record draws its clips without their color.* stacks
//    and without the sequence's look. The input and display transforms
//    stay -- they are how a picture is read and shown, not a grade.
//  * The reference: a still of Record exactly as it was shown -- grade and
//    look included -- drawn as it is over the left part of the frame by
//    the output pass, the live picture showing on the right. It does not
//    go back through the working space: a display-referred picture comes
//    back from the ACES SDR view's inverse up to 0.08 off on saturated
//    colours (measured, tests/monitor_compare_render_tests.mm), which is
//    the size of what a split is for. The scopes keep measuring the live
//    picture alone.
//
// Portable: no AppKit, no Metal (tests/monitor_compare_tests.cc); main.mm
// wires it.

#include <cstdint>
#include <vector>

struct MonitorCompare {
    bool bypass_grade = false;
    bool split = false;
    // The split, as a fraction of the frame width from its left edge: the
    // reference shows left of it.
    double split_position = 0.5;
};

// A stored split position as the monitor uses it: 0 to 1, and one half for
// anything that is not a number in that range.
double ClampSplitPosition(double position);

// Where the reference is drawn: the left `position` of the sequence frame
// in whole pixels, and the fraction of the still it samples -- the same
// width over the still's width, so the reference is neither stretched nor
// moved against the live picture.
struct ReferenceSplit {
    int32_t width = 0;
    int32_t height = 0;
    double source_width = 0.0;
};
ReferenceSplit ReferenceSplitFor(int32_t sequenceWidth, int32_t sequenceHeight,
                                 double position);

// A still grabbed from Record: what the monitor handed the display, 8 bits
// per channel, red, green, blue and alpha per pixel, top row first -- what
// an image file stores and what the reference texture takes.
struct ReferenceStill {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> rgba;

    size_t ByteSize() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    }
};

// The still from what Renderer::RenderFramesToPixels reads back (0..1 per
// channel), alpha made opaque: the monitor's black is the picture's.
ReferenceStill ReferenceStillFromRgba(const std::vector<float>& rgba,
                                      int32_t width, int32_t height);

// A still is shown only at the size it was grabbed: a sequence resized since
// would stretch it, and a stretched reference compares nothing.
bool ReferenceFits(const ReferenceStill& still, int32_t sequenceWidth,
                   int32_t sequenceHeight);
