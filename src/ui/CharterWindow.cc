// CH5 -- docs/roadmap/CHARTE-2026-11.md (DEC63). Where the Charte panel
// opens (CharterWindow.h): the Agent window's rule for a remembered place,
// and a first place in the middle of the editor, where three previews side
// by side have the room.

#include "CharterWindow.h"

#include <algorithm>
#include <optional>
#include <vector>

namespace ui::charter {

namespace {

using ui::agent_window::Grabbable;
using ui::agent_window::kEditorTitleBar;
using ui::agent_window::kInset;

// The screen the editor stands on: the one holding its centre, else the
// one it overlaps most; null when it is on none.
const Rect* ScreenOf(const Rect& editor, const std::vector<Rect>& screens) {
    const double centreX = editor.x + editor.width / 2.0;
    const double centreY = editor.y + editor.height / 2.0;
    const Rect* best = nullptr;
    double bestArea = 0.0;
    for (const Rect& screen : screens) {
        if (centreX >= screen.x && centreX <= screen.Right() &&
            centreY >= screen.y && centreY <= screen.Top()) {
            return &screen;
        }
        const double width = std::min(editor.Right(), screen.Right()) -
                             std::max(editor.x, screen.x);
        const double height =
            std::min(editor.Top(), screen.Top()) - std::max(editor.y, screen.y);
        if (width > 0.0 && height > 0.0 && width * height > bestArea) {
            bestArea = width * height;
            best = &screen;
        }
    }
    return best;
}

}  // namespace

Rect PlaceCharterWindow(const std::optional<Rect>& remembered,
                        const Rect& editor, const std::vector<Rect>& screens) {
    if (remembered) {
        for (const Rect& screen : screens) {
            if (!Grabbable(*remembered, screen)) {
                continue;
            }
            Rect place = *remembered;
            place.width =
                std::clamp(place.width, kWindowMinimumWidth,
                           std::max(kWindowMinimumWidth, screen.width));
            place.height =
                std::clamp(place.height, kWindowMinimumHeight,
                           std::max(kWindowMinimumHeight, screen.height));
            // A height raised to the minimum grows downward: the title bar
            // stays where the hand put it.
            place.y = remembered->Top() - place.height;
            return place;
        }
    }
    // The room under the editor's title bar, less an inset on each side.
    const double roomWidth = editor.width - 2.0 * kInset;
    const double roomHeight = editor.height - kEditorTitleBar - 2.0 * kInset;
    Rect place;
    place.width =
        std::max(kWindowMinimumWidth, std::min(kWindowDefaultWidth, roomWidth));
    place.height = std::max(kWindowMinimumHeight,
                            std::min(kWindowDefaultHeight, roomHeight));
    const Rect* screen = ScreenOf(editor, screens);
    if (screen != nullptr) {
        place.width =
            std::max(kWindowMinimumWidth, std::min(place.width, screen->width));
        place.height = std::max(kWindowMinimumHeight,
                                std::min(place.height, screen->height));
    }
    place.x = editor.x + (editor.width - place.width) / 2.0;
    place.y = editor.y + (editor.height - kEditorTitleBar - place.height) / 2.0;
    if (screen != nullptr) {
        // Inside the visible frame: the title bar first, then the left edge.
        place.x = std::max(screen->x,
                           std::min(place.x, screen->Right() - place.width));
        place.y = std::min(std::max(place.y, screen->y),
                           screen->Top() - place.height);
    }
    return place;
}

}  // namespace ui::charter
