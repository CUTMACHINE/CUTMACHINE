#pragma once

// ON4 -- docs/roadmap/ONGLETS-2026-10.md. Colour puts its primary tools
// first and, when both remain usable, beside each other. The same row
// tables feed this portable policy and AppKit's frames, including rows
// unfolded by the person. Nothing enters the document.

#include "UiTheme.h"

#include <algorithm>
#include <array>
#include <vector>

namespace ui::inspector {

struct GradeWorkspaceRect {
    double x = 0.0;
    double y = 0.0;
    double width = 0.0;
    double height = 0.0;
};

struct GradeWorkspaceLayout {
    // Preamble, wheels, curves, then secondary settings/properties.
    std::array<std::vector<GradeWorkspaceRect>, 4> groups;
    bool columns = false;
    double height = 0.0;
};

inline GradeWorkspaceLayout LayoutGradeWorkspace(
    double width, const std::array<std::vector<double>, 4>& heights) {
    using namespace ui::theme;
    GradeWorkspaceLayout layout;
    width = std::max(0.0, width);
    layout.columns = width >= 2.0 * kGradeWorkspaceColumnMinWidth + kSpaceM;
    const double columnWidth = layout.columns ? (width - kSpaceM) / 2.0 : width;
    const auto place = [&](size_t group, double x, double y, double rowWidth) {
        for (const double height : heights[group]) {
            const double bounded = std::max(0.0, height);
            layout.groups[group].push_back({x, y, rowWidth, bounded});
            y += bounded + kSpaceXxs;
        }
        return y;
    };
    const double primaryTop = place(0, 0.0, kSpaceM, width);
    const double wheelsEnd = place(1, 0.0, primaryTop, columnWidth);
    const double curvesEnd =
        place(2, layout.columns ? columnWidth + kSpaceM : 0.0,
              layout.columns ? primaryTop : wheelsEnd, columnWidth);
    layout.height =
        place(3, 0.0, std::max(wheelsEnd, curvesEnd) + kSpaceM, width) +
        kSpaceM;
    return layout;
}

}  // namespace ui::inspector
