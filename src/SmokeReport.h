#pragma once

// PRD28 -- docs/roadmap/PRODUIT-2026-10b.md. How the window test
// (cutmachine_ui_smoke_tests, its harness in main.mm) tells an editor that is
// broken from a screen it could not use.
//
// The test drives the real editor window. Some of its checks need that
// window in front: a click AppKit hands to the key window only (a click on a
// non-key window activates it instead of reaching the view under it), or a
// capture of what WindowServer composites (a covered window is not
// composited again, and its capture would be a stale picture). Run while
// someone uses the Mac -- another application in front, the window covered
// or on another Space -- the test failed eight to fourteen checks, always
// among the same ones, whatever the code: a red run said something about the
// screen first, and could not say which.
//
// Each check now declares what it needs of the screen (Needs), where the
// check is made -- never a list of names kept elsewhere. A check the screen
// cannot give is not counted: it is reported SKIP with the reason. A run
// ends in one of three ways:
//   - a real failure: exit status 1, as before;
//   - nothing failed, but checks were not played: kNotRunExitStatus (77),
//     which CMakeLists.txt declares as the test's SKIP_RETURN_CODE, so that
//     CTest shows it Skipped rather than Passed;
//   - everything played and passed: 0.
//
// Plain C++: no AppKit. What the screen is -- the window key, the window
// visible -- is observed by the harness and handed in as plain facts.
// tests/smoke_report_tests.cc.

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ui_smoke {

// What a check needs of the screen to be played.
enum class Needs : std::uint8_t {
    // The document, AppKit's or Metal's state, read in process.
    kNothing,
    // A gesture AppKit hands to the key window only -- a click through the
    // window on a view that does not take the first click -- or what such a
    // gesture left behind.
    kKeyWindow,
    // What WindowServer composites: a capture of the window, and whatever is
    // measured from one.
    kVisibleWindow,
};

// What the run knows of its window. Every field is the harness's
// observation, or its simulation (CUTMACHINE_UI_SMOKE_SIMULATE).
struct Screen {
    bool key = true;
    bool visible = true;
    // Why not, when `key` or `visible` is false: the reason a check that
    // needs it is reported with.
    std::string not_key;
    std::string not_visible;
};

// Why a check that needs `needs` cannot be played on `screen`; empty when it
// can be.
inline std::string MissingNeed(Needs needs, const Screen& screen) {
    switch (needs) {
        case Needs::kKeyWindow:
            if (screen.key) {
                return std::string();
            }
            return screen.not_key.empty() ? "the window is not key"
                                          : screen.not_key;
        case Needs::kVisibleWindow:
            if (screen.visible) {
                return std::string();
            }
            return screen.not_visible.empty() ? "the window is not visible"
                                              : screen.not_visible;
        case Needs::kNothing:
            break;
    }
    return std::string();
}

// The checks a run did not play, counted by reason in the order the reasons
// first came.
class NotRun {
public:
    void Add(const std::string& reason) {
        ++count_;
        for (std::pair<std::string, int>& entry : reasons_) {
            if (entry.first == reason) {
                ++entry.second;
                return;
            }
        }
        reasons_.emplace_back(reason, 1);
    }
    int count() const { return count_; }
    const std::vector<std::pair<std::string, int>>& reasons() const {
        return reasons_;
    }

private:
    int count_ = 0;
    std::vector<std::pair<std::string, int>> reasons_;
};

// CTest's SKIP_RETURN_CODE for cutmachine_ui_smoke_tests (CMakeLists.txt).
inline constexpr int kNotRunExitStatus = 77;

// The run's last line. "UI smoke: 0 failure(s)" when every check was played,
// as it always read; otherwise it counts the checks not played and says why:
// "UI smoke: 0 failure(s), 9 not run: the editor window could not become
// key", each reason followed by its own count when there are several.
inline std::string SummaryLine(int failures, const NotRun& notRun) {
    std::string line = "UI smoke: " + std::to_string(failures) + " failure(s)";
    if (notRun.count() == 0) {
        return line;
    }
    line += ", " + std::to_string(notRun.count()) + " not run: ";
    const bool counted = notRun.reasons().size() > 1;
    for (size_t index = 0; index < notRun.reasons().size(); ++index) {
        const std::pair<std::string, int>& entry = notRun.reasons()[index];
        if (index > 0) {
            line += "; ";
        }
        line += entry.first;
        if (counted) {
            line += " (" + std::to_string(entry.second) + ")";
        }
    }
    return line;
}

// 1 on any failure, whatever was not played; kNotRunExitStatus when nothing
// failed but something was not played; 0 when all of it was played and
// passed.
inline int ExitStatus(int failures, const NotRun& notRun) {
    if (failures > 0) {
        return 1;
    }
    return notRun.count() > 0 ? kNotRunExitStatus : 0;
}

}  // namespace ui_smoke
