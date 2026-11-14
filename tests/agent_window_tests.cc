// ON3 -- docs/roadmap/ONGLETS-2026-10.md. Where the Agent's window opens:
// the policy main.mm wires, held here without AppKit.

#include "AgentWindow.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

using ui::agent_window::Rect;

bool Inside(const Rect& inner, const Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.Right() <= outer.Right() && inner.Top() <= outer.Top();
}

}  // namespace

int main() {
    using namespace ui::agent_window;
    // A 1 600 x 960 editor on a laptop screen whose menu bar takes 24
    // points, and a second screen to its right.
    const Rect laptop{0.0, 0.0, 1728.0, 1093.0};
    const Rect second{1728.0, 0.0, 1920.0, 1080.0};
    const Rect editor{64.0, 60.0, 1600.0, 960.0};
    const std::vector<Rect> both{laptop, second};

    // The first time: the editor's top right corner, under its title bar,
    // at the default size.
    {
        const Rect place = PlaceAgentWindow(std::nullopt, editor, both);
        Check(place.width == kDefaultWidth && place.height == kDefaultHeight,
              "the first place has the default size");
        Check(place.Right() == editor.Right() - kInset &&
                  place.Top() == editor.Top() - kEditorTitleBar - kInset,
              "the first place is the editor's top right corner, under its "
              "title bar");
        Check(Inside(place, editor), "the first place is over the editor");
    }

    // A small editor: the window shrinks with it, down to the minimum.
    {
        const Rect small{100.0, 100.0, 600.0, 440.0};
        const Rect place = DefaultPlace(small);
        Check(place.width == kDefaultWidth &&
                  place.height == small.height - kEditorTitleBar - 2 * kInset,
              "on a short editor the window is as tall as the room under its "
              "title bar");
        const Rect tiny{100.0, 100.0, 300.0, 200.0};
        const Rect least = DefaultPlace(tiny);
        Check(least.width == kMinimumWidth && least.height == kMinimumHeight,
              "never under the minimum, even on a tiny editor");
    }

    // Where the hand left it, on either screen, is where it opens.
    {
        const Rect left{80.0, 300.0, 420.0, 600.0};
        Check(PlaceAgentWindow(left, editor, both) == left,
              "a place on the editor's screen is kept as it was");
        const Rect away{2400.0, 200.0, 500.0, 700.0};
        Check(PlaceAgentWindow(away, editor, both) == away,
              "a place on the second screen is kept as it was");
        const Rect edge{1600.0, 400.0, 400.0, 500.0};
        Check(PlaceAgentWindow(edge, editor, {laptop}) == edge,
              "half off the screen, its title bar still within reach, it is "
              "kept");
    }

    // The second screen unplugged: the window comes back beside the editor.
    {
        const Rect away{2400.0, 200.0, 500.0, 700.0};
        Check(PlaceAgentWindow(away, editor, {laptop}) == DefaultPlace(editor),
              "a place on a screen that is gone gives the first place");
        const Rect sliver{1700.0, 400.0, 400.0, 500.0};
        Check(
            PlaceAgentWindow(sliver, editor, {laptop}) == DefaultPlace(editor),
            "a title bar 28 points on the screen cannot be grabbed: the "
            "first place");
        const Rect above{200.0, 1080.0, 400.0, 500.0};
        Check(PlaceAgentWindow(above, editor, {laptop}) == DefaultPlace(editor),
              "a title bar above the top of the screen: the first place");
    }

    // A remembered size under the minimum is raised, the title bar where it
    // was; one larger than its screen is brought down to it.
    {
        const Rect small{200.0, 500.0, 120.0, 90.0};
        const Rect place = PlaceAgentWindow(small, editor, both);
        Check(place.width == kMinimumWidth && place.height == kMinimumHeight &&
                  place.x == small.x && place.Top() == small.Top(),
              "a size under the minimum is raised, the title bar kept");
        const Rect huge{0.0, 0.0, 3000.0, 1093.0};
        const Rect fitted = PlaceAgentWindow(huge, editor, {laptop});
        Check(fitted.width == laptop.width && fitted.height == laptop.height,
              "a size larger than its screen is brought down to the screen");
    }

    if (failures == 0) std::cout << "agent window tests passed\n";
    return failures == 0 ? 0 : 1;
}
