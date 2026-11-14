// PRD28 -- docs/roadmap/PRODUIT-2026-10b.md. How the window test reports a
// check it could not play (SmokeReport.h): what each need asks of the
// screen, the run's last line, and the exit status CTest reads -- 1 for a
// real failure, 77 (Skipped) when checks could not be played, 0 when
// everything was played and passed.
//
// Plain C++: no AppKit, no window.

#include "SmokeReport.h"

#include <iostream>
#include <string>

namespace {

using ui_smoke::ExitStatus;
using ui_smoke::MissingNeed;
using ui_smoke::Needs;
using ui_smoke::NotRun;
using ui_smoke::Screen;
using ui_smoke::SummaryLine;

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

template <typename Function>
void Test(const std::string& name, Function function) {
    const int before = failures;
    function();
    if (before == failures) {
        std::cout << "PASS: " << name << '\n';
    }
}

constexpr const char* kNotKey = "the editor window could not become key";
constexpr const char* kHidden =
    "the editor window is not visible (covered, minimised or on another "
    "Space)";

}  // namespace

int main() {
    Test("a screen with its window key and visible plays every check", [] {
        const Screen screen;
        Check(MissingNeed(Needs::kNothing, screen).empty(), "nothing");
        Check(MissingNeed(Needs::kKeyWindow, screen).empty(), "key window");
        Check(MissingNeed(Needs::kVisibleWindow, screen).empty(),
              "visible window");
    });

    Test("a window that is not key holds back only what needs it", [] {
        Screen screen;
        screen.key = false;
        screen.not_key = kNotKey;
        Check(MissingNeed(Needs::kNothing, screen).empty(),
              "a check that needs nothing is still played");
        Check(MissingNeed(Needs::kKeyWindow, screen) == kNotKey,
              "the key window: " + MissingNeed(Needs::kKeyWindow, screen));
        Check(MissingNeed(Needs::kVisibleWindow, screen).empty(),
              "a window beside the one in front is still visible");
    });

    Test("a hidden window holds back what is captured, with its reason", [] {
        Screen screen;
        screen.key = false;
        screen.not_key = kNotKey;
        screen.visible = false;
        screen.not_visible = kHidden;
        Check(MissingNeed(Needs::kVisibleWindow, screen) == kHidden,
              "the visible window: " +
                  MissingNeed(Needs::kVisibleWindow, screen));
        Check(MissingNeed(Needs::kKeyWindow, screen) == kNotKey,
              "and the key window");
        Check(MissingNeed(Needs::kNothing, screen).empty(), "nothing");
    });

    Test("a need held back without a reason still says which", [] {
        Screen screen;
        screen.key = false;
        screen.visible = false;
        Check(!MissingNeed(Needs::kKeyWindow, screen).empty(), "key");
        Check(!MissingNeed(Needs::kVisibleWindow, screen).empty(), "visible");
    });

    Test("every check played and passed: the line reads as it did, exit 0", [] {
        const NotRun notRun;
        Check(SummaryLine(0, notRun) == "UI smoke: 0 failure(s)",
              SummaryLine(0, notRun));
        Check(ExitStatus(0, notRun) == 0, "exit status");
    });

    Test("a real failure exits 1, played or not", [] {
        NotRun notRun;
        Check(ExitStatus(2, notRun) == 1, "failures alone");
        Check(SummaryLine(2, notRun) == "UI smoke: 2 failure(s)",
              SummaryLine(2, notRun));
        notRun.Add(kNotKey);
        Check(ExitStatus(1, notRun) == 1,
              "a failure is not hidden by checks not played");
        Check(SummaryLine(1, notRun) ==
                  std::string("UI smoke: 1 failure(s), 1 not run: ") + kNotKey,
              SummaryLine(1, notRun));
    });

    Test("nothing failed but checks were not played: 77, counted, with why",
         [] {
             NotRun notRun;
             for (int index = 0; index < 9; ++index) {
                 notRun.Add(kNotKey);
             }
             Check(notRun.count() == 9, "count");
             Check(ExitStatus(0, notRun) == ui_smoke::kNotRunExitStatus &&
                       ui_smoke::kNotRunExitStatus == 77,
                   "exit status " + std::to_string(ExitStatus(0, notRun)));
             Check(SummaryLine(0, notRun) ==
                       "UI smoke: 0 failure(s), 9 not run: the editor window "
                       "could not become key",
                   SummaryLine(0, notRun));
         });

    Test("several reasons each keep their count, in the order they came", [] {
        NotRun notRun;
        notRun.Add(kNotKey);
        notRun.Add(kHidden);
        notRun.Add(kHidden);
        notRun.Add(kNotKey);
        notRun.Add(kHidden);
        Check(notRun.reasons().size() == 2 &&
                  notRun.reasons()[0].first == kNotKey &&
                  notRun.reasons()[0].second == 2 &&
                  notRun.reasons()[1].second == 3,
              "tally");
        Check(SummaryLine(0, notRun) ==
                  std::string("UI smoke: 0 failure(s), 5 not run: ") + kNotKey +
                      " (2); " + kHidden + " (3)",
              SummaryLine(0, notRun));
    });

    if (failures > 0) {
        std::cerr << failures << " smoke_report check(s) failed\n";
        return 1;
    }
    return 0;
}
