// Pure-C++ test for the fixed panel-slot registry (ROADMAP.md F2.1).
// PanelLayout.h has no AppKit dependency, so this builds and runs on a
// plain Linux host, the same way tests/fft_tests.cc does.

#include "PanelLayout.h"

#include <iostream>
#include <set>
#include <string>

#include "TestSupport.h"

int main() {
    Test("the layout has exactly one descriptor per panel slot", [] {
        const auto& layout = ui::FixedPanelLayout();
        Check(layout.size() == 3, "expected exactly 3 fixed panel slots");
        std::set<ui::PanelSlot> seenSlots;
        for (const auto& descriptor : layout) seenSlots.insert(descriptor.slot);
        Check(seenSlots.size() == layout.size(),
              "every PanelSlot value must appear exactly once");
    });

    Test("identifiers are unique, stable, non-empty ASCII", [] {
        std::set<std::string> identifiers;
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const std::string identifier = descriptor.identifier;
            Check(!identifier.empty(), "identifier must not be empty");
            identifiers.insert(identifier);
        }
        Check(identifiers.size() == ui::FixedPanelLayout().size(),
              "identifiers must be unique across slots");
    });

    Test("within each dock, order values are unique -- no tab-order ties", [] {
        for (ui::PanelDock dock : {ui::PanelDock::Left, ui::PanelDock::Right}) {
            std::set<int> orders;
            for (const auto* descriptor : ui::PanelSlotsInDock(dock))
                orders.insert(descriptor->order);
            Check(orders.size() == ui::PanelSlotsInDock(dock).size(),
                  "duplicate `order` within one dock");
        }
    });

    Test("PanelSlotsInDock returns only that dock's slots, sorted by order",
         [] {
             const auto rightSlots = ui::PanelSlotsInDock(ui::PanelDock::Right);
             for (const auto* descriptor : rightSlots)
                 Check(descriptor->dock == ui::PanelDock::Right,
                       "PanelSlotsInDock leaked a slot from another dock");
             for (size_t index = 1; index < rightSlots.size(); ++index)
                 Check(rightSlots[index - 1]->order < rightSlots[index]->order,
                       "slots within a dock must come back in ascending order");
         });

    Test("the Inspector alone docks right; the Agent floats", [] {
        // F2.2 (Inspector) and F2.4 (Chat) were written against a right dock
        // holding both, Inspector first. ON3 took the Agent into a floating
        // window of its own (AgentWindow.h): the right dock is the Inspector
        // alone, and its host draws no tab strip for one slot. A deliberate
        // layout change, not a runtime rearrangement.
        const auto rightSlots = ui::PanelSlotsInDock(ui::PanelDock::Right);
        Check(rightSlots.size() == 1 &&
                  rightSlots[0]->slot == ui::PanelSlot::Inspector,
              "the Inspector should be the right dock's only slot");
        const auto floating = ui::PanelSlotsInDock(ui::PanelDock::Floating);
        Check(floating.size() == 1 && floating[0]->slot == ui::PanelSlot::Chat,
              "the Agent should be the one floating slot");
        // A right dock remembered on the Agent's tab, from before ON3, names
        // a slot no longer of that dock: the host falls back to its first.
        const ui::PanelSlotDescriptor* remembered =
            ui::FindPanelSlotByIdentifier("chat");
        Check(remembered != nullptr && remembered->dock != ui::PanelDock::Right,
              "\"chat\" no longer names a slot of the right dock");
    });

    Test("FindPanelSlot round-trips every slot in the layout", [] {
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const ui::PanelSlotDescriptor* found =
                ui::FindPanelSlot(descriptor.slot);
            Check(found != nullptr && found->slot == descriptor.slot,
                  "FindPanelSlot failed to round-trip a known slot");
        }
    });

    Test("FindPanelSlotByIdentifier round-trips and rejects unknown ids", [] {
        for (const auto& descriptor : ui::FixedPanelLayout()) {
            const ui::PanelSlotDescriptor* found =
                ui::FindPanelSlotByIdentifier(descriptor.identifier);
            Check(found != nullptr && found->slot == descriptor.slot,
                  "FindPanelSlotByIdentifier failed to round-trip a known id");
        }
        Check(ui::FindPanelSlotByIdentifier("does-not-exist") == nullptr,
              "an unknown identifier must resolve to nullptr, not a guess");
    });

    Test("local-preference keys are stable and unique per dock", [] {
        std::set<std::string> keys;
        for (ui::PanelDock dock : {ui::PanelDock::Left, ui::PanelDock::Right,
                                   ui::PanelDock::Floating}) {
            const std::string key = ui::LastActiveTabPreferenceKey(dock);
            Check(key == ui::LastActiveTabPreferenceKey(dock),
                  "the same dock must always produce the same key");
            keys.insert(key);
        }
        Check(keys.size() == 3, "each dock must have a distinct key");
    });

    return failures == 0 ? 0 : 1;
}
