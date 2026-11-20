#pragma once

// CH5 -- docs/roadmap/CHARTE-2026-11.md (DEC63). The Charte panel floats
// over the editor, opened on demand (Affichage > Charte... and its
// shortcut), never a page nor a button always shown: a charter is set once
// for a brand and then left alone, and it should not take the editor's room
// the rest of the time.
//
// Where it opens, as policy, on the Agent window's rules (AgentWindow.h):
// where it was left while its title bar can still be grabbed on a screen,
// otherwise a first place over the editor. Larger than the Agent window:
// a list of charters, three previews side by side and the fields.
// Rectangles in AppKit's screen coordinates (points, origin at the bottom
// left of the main screen, y upward), title bar included.
//
// Portable: no AppKit (tests/charter_window_tests.cc).

#include "AgentWindow.h"

#include <optional>
#include <vector>

namespace ui::charter {

using Rect = ui::agent_window::Rect;

inline constexpr double kWindowDefaultWidth = 1120.0;
inline constexpr double kWindowDefaultHeight = 720.0;
inline constexpr double kWindowMinimumWidth = 860.0;
inline constexpr double kWindowMinimumHeight = 520.0;

// Where the panel opens: `remembered` while ui::agent_window::Grabbable on
// one of `screens` (their visible frames), at no less than the minimum size
// and no more than that screen, its title bar kept where the hand put it;
// otherwise centred on `editor` under its title bar, at the default size,
// smaller on a small editor or screen but never under the minimum, and
// moved inside the visible frame of the editor's screen (the one holding
// its centre, else the one it overlaps most) -- its title bar first, when
// the minimum is taller than that screen.
Rect PlaceCharterWindow(const std::optional<Rect>& remembered,
                        const Rect& editor, const std::vector<Rect>& screens);

}  // namespace ui::charter
