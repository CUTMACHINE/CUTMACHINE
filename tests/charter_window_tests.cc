// CH5 -- docs/roadmap/CHARTE-2026-11.md (DEC63). Where the Charte panel
// opens: the policy the panel wires, on the Agent window's rules
// (agent_window_tests.cc), held here without AppKit.

#include "CharterWindow.h"
#include "TestSupport.h"

#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

using ui::charter::Rect;

bool Inside(const Rect& inner, const Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.Right() <= outer.Right() && inner.Top() <= outer.Top();
}

}  // namespace

int main() {
    using namespace ui::charter;
    using ui::agent_window::kEditorTitleBar;
    // A 1 600 x 960 editor on a laptop screen whose menu bar takes 24
    // points, and a second screen to its right.
    const Rect laptop{0.0, 0.0, 1728.0, 1093.0};
    const Rect second{1728.0, 0.0, 1920.0, 1080.0};
    const Rect editor{64.0, 60.0, 1600.0, 960.0};
    const std::vector<Rect> both{laptop, second};

    // The first time: centred on the editor, under its title bar, at the
    // default size.
    {
        const Rect place = PlaceCharterWindow(std::nullopt, editor, both);
        Check(place.width == kWindowDefaultWidth &&
                  place.height == kWindowDefaultHeight,
              "the first place has the default size");
        Check(place.x + place.width / 2.0 == editor.x + editor.width / 2.0 &&
                  place.y + place.height / 2.0 ==
                      editor.y + (editor.height - kEditorTitleBar) / 2.0,
              "the first place is centred on the editor, under its title "
              "bar");
        Check(Inside(place, editor), "the first place is over the editor");
        Check(kWindowDefaultWidth > ui::agent_window::kDefaultWidth &&
                  kWindowMinimumWidth > ui::agent_window::kMinimumWidth,
              "the panel is larger than the Agent window: three previews "
              "and the fields");
    }

    // A small editor: the panel shrinks with it, down to the minimum.
    {
        const Rect small{100.0, 100.0, 1000.0, 640.0};
        const Rect place = PlaceCharterWindow(std::nullopt, small, both);
        Check(place.width == small.width - 2 * ui::agent_window::kInset &&
                  place.height == small.height - kEditorTitleBar -
                                      2 * ui::agent_window::kInset,
              "on a small editor the panel takes the room under its title "
              "bar");
        const Rect tiny{100.0, 100.0, 400.0, 300.0};
        const Rect least = PlaceCharterWindow(std::nullopt, tiny, both);
        Check(least.width == kWindowMinimumWidth &&
                  least.height == kWindowMinimumHeight,
              "never under the minimum, even on a tiny editor");
        Check(Inside(least, laptop),
              "a minimum wider than the editor still opens on its screen");
    }

    // An editor half off its screen: the first place is brought back
    // inside the visible frame.
    {
        const Rect offside{-700.0, 200.0, 1200.0, 800.0};
        const Rect place = PlaceCharterWindow(std::nullopt, offside, both);
        Check(Inside(place, laptop),
              "an editor half off the screen: the panel opens inside it");
        const Rect low{300.0, -500.0, 1200.0, 800.0};
        Check(Inside(PlaceCharterWindow(std::nullopt, low, both), laptop),
              "an editor below the screen: the panel opens inside it");
        const Rect onSecond{2000.0, 100.0, 1600.0, 900.0};
        Check(Inside(PlaceCharterWindow(std::nullopt, onSecond, both), second),
              "an editor on the second screen: the panel opens there");
    }

    // A screen smaller than the default: the panel fits it, never under
    // the minimum, its title bar on the screen.
    {
        const Rect small{0.0, 0.0, 1024.0, 640.0};
        const Rect wide{0.0, 0.0, 1024.0, 640.0};
        const Rect place = PlaceCharterWindow(std::nullopt, wide, {small});
        Check(place.width <= small.width && place.height <= small.height &&
                  place.width >= kWindowMinimumWidth &&
                  place.height >= kWindowMinimumHeight && Inside(place, small),
              "a small screen: the panel fits it");
        const Rect shallow{0.0, 0.0, 1280.0, 400.0};
        const Rect squat = PlaceCharterWindow(std::nullopt, shallow, {shallow});
        Check(squat.height == kWindowMinimumHeight &&
                  squat.Top() == shallow.Top(),
              "a screen shorter than the minimum: the minimum, the title bar "
              "at the top of the screen");
    }

    // Where the hand left it, on either screen, is where it opens.
    {
        const Rect left{80.0, 200.0, 1000.0, 700.0};
        Check(PlaceCharterWindow(left, editor, both) == left,
              "a place on the editor's screen is kept as it was");
        const Rect away{2100.0, 100.0, 1200.0, 800.0};
        Check(PlaceCharterWindow(away, editor, both) == away,
              "a place on the second screen is kept as it was");
        const Rect edge{1400.0, 300.0, 1000.0, 600.0};
        Check(PlaceCharterWindow(edge, editor, {laptop}) == edge,
              "half off the screen, its title bar still within reach, it is "
              "kept");
    }

    // The second screen unplugged: the panel comes back over the editor.
    {
        const Rect first = PlaceCharterWindow(std::nullopt, editor, {laptop});
        const Rect away{2100.0, 100.0, 1200.0, 800.0};
        Check(PlaceCharterWindow(away, editor, {laptop}) == first,
              "a place on a screen that is gone gives the first place");
        const Rect sliver{1700.0, 300.0, 1000.0, 600.0};
        Check(PlaceCharterWindow(sliver, editor, {laptop}) == first,
              "a title bar 28 points on the screen cannot be grabbed: the "
              "first place");
        const Rect above{200.0, 1080.0, 1000.0, 600.0};
        Check(PlaceCharterWindow(above, editor, {laptop}) == first,
              "a title bar above the top of the screen: the first place");
    }

    // A remembered size under the minimum is raised, the title bar where it
    // was; one larger than its screen is brought down to it.
    {
        const Rect small{200.0, 300.0, 400.0, 300.0};
        const Rect place = PlaceCharterWindow(small, editor, both);
        Check(place.width == kWindowMinimumWidth &&
                  place.height == kWindowMinimumHeight && place.x == small.x &&
                  place.Top() == small.Top(),
              "a size under the minimum is raised, the title bar kept");
        const Rect huge{0.0, 0.0, 3000.0, 1093.0};
        const Rect fitted = PlaceCharterWindow(huge, editor, {laptop});
        Check(fitted.width == laptop.width && fitted.height == laptop.height,
              "a size larger than its screen is brought down to the screen");
    }

    if (failures == 0) std::cout << "charter window tests passed\n";
    return failures == 0 ? 0 : 1;
}
