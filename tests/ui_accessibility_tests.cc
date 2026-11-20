// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. What VoiceOver reads of the
// Metal views (UiAccessibility.h): their names, and the timeline's value --
// the playhead's timecode as the label under the timeline shows it, and the
// selected shots by the names the timeline draws on them.
//
// Plain C++: no AppKit.

#include "UiAccessibility.h"

#include "Timecode.h"

#include <iostream>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace {

using ui::accessibility::TimelineValue;

void Expect(const std::string& actual, const std::string& expected) {
    Check(actual == expected,
          "expected \"" + expected + "\", got \"" + actual + "\"");
}

constexpr MediaRate kPal{25, 1};

}  // namespace

int main() {
    Test("the three Metal views are named in the window's own words", [] {
        Expect(ui::accessibility::kTimelineLabel, "Timeline");
        Expect(ui::accessibility::kSourceMonitorLabel, "Moniteur Source");
        Expect(ui::accessibility::kRecordMonitorLabel, "Moniteur Record");
    });

    Test("nothing selected: the playhead alone", [] {
        Expect(TimelineValue({37, 25}, kPal, {}),
               "Tête de lecture 00:00:01:12, aucun plan sélectionné");
    });

    Test("one shot selected: its name", [] {
        Expect(TimelineValue({37, 25}, kPal, {"Interview A"}),
               "Tête de lecture 00:00:01:12, plan sélectionné : Interview A");
    });

    Test("two and three shots: counted, then named", [] {
        Expect(TimelineValue({0, 25}, kPal, {"A", "B"}),
               "Tête de lecture 00:00:00:00, 2 plans sélectionnés : A et B");
        Expect(TimelineValue({0, 25}, kPal, {"A", "B", "C"}),
               "Tête de lecture 00:00:00:00, 3 plans sélectionnés : A, B et "
               "C");
    });

    Test("more than three shots: three named, the others counted", [] {
        Expect(TimelineValue({0, 25}, kPal, {"A", "B", "C", "D"}),
               "Tête de lecture 00:00:00:00, 4 plans sélectionnés : A, B, C "
               "et 1 autre");
        Expect(TimelineValue({0, 25}, kPal, {"A", "B", "C", "D", "E"}),
               "Tête de lecture 00:00:00:00, 5 plans sélectionnés : A, B, C "
               "et 2 autres");
    });

    Test("a carton reads on one line, a shot without a name is still one", [] {
        Expect(
            TimelineValue({50, 25}, kPal, {"Carton corrigé\nsur deux lignes"}),
            "Tête de lecture 00:00:02:00, plan sélectionné : Carton "
            "corrigé sur deux lignes");
        Expect(TimelineValue({50, 25}, kPal, {""}),
               "Tête de lecture 00:00:02:00, plan sélectionné : plan "
               "sans nom");
    });

    Test("a picture and its linked sound are one shot, named once", [] {
        using ui::accessibility::SelectedClip;
        using ui::accessibility::SelectedShots;
        const std::vector<std::string> linked =
            SelectedShots({{"Interview A", "g1"}, {"Interview A", "g1"}});
        Check(linked == std::vector<std::string>{"Interview A"}, "linked pair");
        Expect(TimelineValue({37, 25}, kPal, linked),
               "Tête de lecture 00:00:01:12, plan sélectionné : Interview A");
        // Two linked pairs and a carton without a group: three shots, in
        // the selection's order, each named by its first clip.
        const std::vector<std::string> mixed =
            SelectedShots({{"A", "g1"},
                           {"B", "g2"},
                           {"A (son)", "g1"},
                           {"Carton", ""},
                           {"B (son)", "g2"}});
        Check(mixed == std::vector<std::string>{"A", "B", "Carton"},
              "mixed selection");
        // Clips without a group are never merged, even under one name.
        Check(SelectedShots({{"Plan", ""}, {"Plan", ""}}).size() == 2,
              "ungrouped clips stay apart");
        Check(SelectedShots({}).empty(), "nothing selected");
    });

    Test("the timecode is the one under the timeline, at its rate", [] {
        // Three minutes into a 29.97 sequence: FormatTimecode's nominal
        // 30-frame clock, as the label shows it.
        const MediaRate ntsc{30000, 1001};
        const RationalTime threeMinutes{5394, 30};
        Expect(TimelineValue(threeMinutes, ntsc, {}),
               "Tête de lecture " + FormatTimecode(threeMinutes, ntsc) +
                   ", aucun plan sélectionné");
        Expect(TimelineValue({3600 * 25 + 1, 25}, kPal, {}),
               "Tête de lecture 01:00:00:01, aucun plan sélectionné");
    });

    if (failures > 0) {
        std::cerr << failures << " ui_accessibility check(s) failed\n";
        return 1;
    }
    return 0;
}
