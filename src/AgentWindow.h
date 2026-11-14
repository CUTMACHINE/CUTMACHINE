#pragma once

// ON3 -- docs/roadmap/ONGLETS-2026-10.md. The Agent in a window of its own.
// The author, 31 October 2026: « l'agent jsp trop, pas envie de le shove mais
// c'est le truc de l'app. Il devrait être togglable (une fenêtre
// flottante ?) ». It was the second tab of the right dock, where it took the
// Inspector's room on every page, and went behind it on the Couleur page and
// for a carton's text. Now it floats over the editor, shown and hidden from
// any page by one button or one key, and opens where it was left.
//
// Where it opens, as policy: plain C++, tested without AppKit. Rectangles
// are in AppKit's screen coordinates (points, origin at the bottom left of
// the main screen, y upward); a window's rectangle includes its title bar.

#include <algorithm>
#include <optional>
#include <vector>

namespace ui::agent_window {

struct Rect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;

    double Top() const { return y + height; }
    double Right() const { return x + width; }
    bool operator==(const Rect& other) const {
        return x == other.x && y == other.y && width == other.width &&
               height == other.height;
    }
};

// Long enough for a request and its answer side by side with the editor,
// never so narrow that the transcript's lines are a word wide.
inline constexpr double kDefaultWidth = 400.0;
inline constexpr double kDefaultHeight = 560.0;
inline constexpr double kMinimumWidth = 320.0;
inline constexpr double kMinimumHeight = 360.0;
// From the editor's edges, and under its title bar.
inline constexpr double kInset = 16.0;
inline constexpr double kEditorTitleBar = 28.0;
// A remembered place is kept while this much of its title bar is on a
// screen: enough to grab it and bring it back.
inline constexpr double kGrabWidth = 80.0;
inline constexpr double kGrabHeight = 20.0;

// The first place: the editor's top right corner, under its title bar. On
// the Montage page that is Record's right margin, which a film narrower
// than the window leaves empty. Smaller on a small editor, never under the
// minimum.
inline Rect DefaultPlace(const Rect& editor) {
    Rect place;
    place.width = std::max(
        kMinimumWidth, std::min(kDefaultWidth, editor.width - 2.0 * kInset));
    place.height =
        std::max(kMinimumHeight,
                 std::min(kDefaultHeight,
                          editor.height - kEditorTitleBar - 2.0 * kInset));
    place.x = editor.Right() - kInset - place.width;
    place.y = editor.Top() - kEditorTitleBar - kInset - place.height;
    return place;
}

// Whether `place`'s title bar can be grabbed on `screen` (a visible frame).
inline bool Grabbable(const Rect& place, const Rect& screen) {
    const double left = std::max(place.x, screen.x);
    const double right = std::min(place.Right(), screen.Right());
    const double top = std::min(place.Top(), screen.Top());
    const double bottom = std::max(place.Top() - kGrabHeight, screen.y);
    return right - left >= kGrabWidth && top - bottom >= kGrabHeight;
}

// Where the window opens: where it was left, while its title bar can still
// be grabbed on one of `screens` (their visible frames) -- a screen
// unplugged since, or a resolution changed, would leave it out of reach --
// at no less than the minimum size and no more than that screen; otherwise
// the first place, beside `editor`.
inline Rect PlaceAgentWindow(const std::optional<Rect>& remembered,
                             const Rect& editor,
                             const std::vector<Rect>& screens) {
    if (remembered) {
        for (const Rect& screen : screens) {
            if (!Grabbable(*remembered, screen)) continue;
            Rect place = *remembered;
            place.width = std::clamp(place.width, kMinimumWidth,
                                     std::max(kMinimumWidth, screen.width));
            place.height = std::clamp(place.height, kMinimumHeight,
                                      std::max(kMinimumHeight, screen.height));
            // A height raised to the minimum grows downward: the title bar
            // stays where the hand put it.
            place.y = remembered->Top() - place.height;
            return place;
        }
    }
    return DefaultPlace(editor);
}

}  // namespace ui::agent_window
