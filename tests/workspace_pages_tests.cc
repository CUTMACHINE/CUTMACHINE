// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. What each page of the window
// opens, and how wide: the policy main.mm wires, held here without AppKit.

#include "KeyboardShortcuts.h"
#include "WorkspaceActivity.h"
#include "WorkspacePages.h"

#include <cmath>
#include <iostream>
#include <set>
#include <string>

#include "TestSupport.h"

int main() {
    using ui::workspace::Page;
    using ui::workspace::PanelSet;

    // Resolve's pages in Resolve's order, each reached by its digit.
    {
        std::set<std::string> identifiers;
        std::string order;
        for (const auto& page : ui::workspace::Pages()) {
            identifiers.insert(page.identifier);
            order += std::string(page.title) + ' ';
            Check(ui::workspace::PageNamed(page.identifier) == page.page,
                  std::string(page.identifier) + " reads back as its page");
        }
        Check(identifiers.size() == ui::workspace::Pages().size(),
              "each page has its own identifier");
        Check(order == "Médias Montage Couleur ",
              "the tabs in Resolve's order: " + order);
        Check(!ui::workspace::PageNamed("cut") &&
                  ui::workspace::kDefaultPage == Page::Edit,
              "no Cut page; a fresh window starts on Montage");
        const std::pair<const char*, const char*> keys[] = {
            {"page.media", "Shift+2"},
            {"page.edit", "Shift+4"},
            {"page.color", "Shift+6"}};
        for (const auto& [command, spec] : keys) {
            bool found = false;
            for (const auto& definition : ui::shortcuts::Definitions())
                if (std::string(definition.id) == command) {
                    found = true;
                    Check(std::string(definition.default_spec) == spec,
                          std::string(command) + " is " + spec +
                              " as in Resolve, not " + definition.default_spec);
                }
            Check(found, std::string(command) + " is a command");
        }
        for (const auto& page : ui::workspace::Pages()) {
            bool found = false;
            for (const auto& definition : ui::shortcuts::Definitions())
                found = found || std::string(definition.id) == page.command;
            Check(found, std::string(page.title) + "'s command exists");
        }
    }

    // What each page opens.
    Check(ui::workspace::DefaultPanels(Page::Media) ==
              PanelSet{true, false, true},
          "Médias opens the media panel and Source");
    Check(ui::workspace::DefaultPanels(Page::Edit) == PanelSet{},
          "Montage opens neither panel, nor Source: the timeline and Record");
    Check(ui::workspace::DefaultPanels(Page::Color) ==
              PanelSet{false, true, false},
          "Couleur opens the Inspector");

    // A panel a page opens is wide.
    {
        const auto widths = ui::workspace::SidePanelWidths(
            Page::Edit, {true, true, false}, 1600.0, 1.0);
        Check(std::abs(widths.media - 480.0) < 1e-9 &&
                  std::abs(widths.inspector - 480.0) < 1e-9,
              "on a 1600-point window, Montage's panels take 480 points "
              "each, not 320 and 300");
        const auto color = ui::workspace::SidePanelWidths(
            Page::Color, {false, true, false}, 1600.0, 1.0);
        Check(color.media == 0.0 && std::abs(color.inspector - 832.0) < 1e-9,
              "Couleur's Inspector takes 832 points for two grade columns");
        const auto media = ui::workspace::SidePanelWidths(
            Page::Media, {true, false, true}, 1600.0, 1.0);
        Check(std::abs(media.media - 896.0) < 1e-9 && media.inspector == 0.0,
              "Médias' panel takes 896 points for readable bins and a grid");
        const auto small = ui::workspace::SidePanelWidths(
            Page::Edit, {true, false, false}, 1000.0, 1.0);
        Check(std::abs(small.media - 380.0) < 1e-9,
              "on a small window a panel keeps its floor of 380 points");
    }

    // The centre is kept before the panels' floors.
    {
        const auto widths = ui::workspace::SidePanelWidths(
            Page::Edit, {true, true, false}, 1100.0, 1.0);
        Check(std::abs(widths.media + widths.inspector + 2.0 +
                       ui::workspace::kCentreMinimum - 1100.0) < 1e-9 &&
                  std::abs(widths.media - widths.inspector) < 1e-9,
              "on 1100 points, both panels shrink evenly so the centre keeps "
              "480");
        const auto tiny = ui::workspace::SidePanelWidths(
            Page::Edit, {true, true, false}, 400.0, 1.0);
        Check(tiny.media == 0.0 && tiny.inspector == 0.0,
              "a window smaller than the centre gives the panels nothing");
    }

    // A width the hand set on a page replaces the share.
    {
        const auto widths = ui::workspace::SidePanelWidths(
            Page::Edit, {false, true, false}, 1600.0, 1.0, {0.0, 620.0});
        Check(std::abs(widths.inspector - 620.0) < 1e-9,
              "the Inspector dragged to 620 points stays at 620");
        const auto narrow = ui::workspace::SidePanelWidths(
            Page::Edit, {false, true, false}, 1600.0, 1.0, {0.0, 40.0});
        Check(std::abs(narrow.inspector - 190.0) < 1e-9,
              "never under half its floor");
    }

    // ON4/ON5/ON7 -- the working surfaces belong to the mission, and the
    // trace says only what was recorded, including an absent reason.
    Check(!ui::workspace::ShowsTimeline(Page::Media) &&
              !ui::workspace::ShowsRecord(Page::Media) &&
              ui::workspace::ShowsActivity(Page::Edit) &&
              !ui::workspace::ShowsActivity(Page::Color),
          "Media gives Source the height; Edit keeps the trace beside Record");
    Check(ui::workspace::BinSidebarWidth(Page::Media, 896) == 240 &&
              ui::workspace::BinSidebarWidth(Page::Media, 420) == 160 &&
              ui::workspace::BinSidebarWidth(Page::Edit, 896) == 112,
          "Media widens the bins without moving the compact browser");
    HistoryEntry change;
    change.type = "SetClipEffects";
    Check(ui::workspace::ChangeText(change, {}) ==
              "Étalonnage\nMotif non précisé",
          "an operation alone invents neither an intention nor an author");
    change.label = "Reprendre le raccord";
    change.operations = 2;
    EditMotif motif;
    motif.reason = MotifReason::Marker;
    motif.author = MotifAuthor::Person;
    motif.note = "Laisser finir le mot";
    Check(ui::workspace::ChangeText(change, {{0, 2, motif}}) ==
              "Reprendre le raccord · 2 opérations\nPersonne · Marqueur — "
              "Laisser finir le mot",
          "the trace preserves the intervention label, author and note");

    {
        Document document;
        EditLog log;
        EditError error = EditError::None;
        std::string message;
        for (int index = 0; index < 210; ++index) {
            DocumentMarker marker;
            marker.name = "Trace";
            marker.time = {0, 25};
            EditMotif recorded;
            recorded.note = "geste " + std::to_string(index);
            Check(log.Apply(document, AddMarkerOperation{marker}, error,
                            message, {{0, 1, recorded}}),
                  "record a change for the trace: " + message);
        }
        Check(ui::workspace::RecentChanges(log).size() == 8 &&
                  ui::workspace::RecentChanges(log, 0).empty(),
              "the trace bounds its applied tail");
        for (int index = 0; index < 209; ++index) {
            Check(log.Undo(document, error, message), "undo: " + message);
        }
        const auto remaining = ui::workspace::RecentChanges(log);
        Check(remaining.size() == 1 &&
                  remaining.front().find("geste 0") != std::string::npos,
              "more than 200 undone entries do not hide an applied change");
        Check(log.Redo(document, error, message), "redo: " + message);
        const auto redone = ui::workspace::RecentChanges(log);
        Check(redone.size() == 2 &&
                  redone.front().find("geste 1") != std::string::npos &&
                  redone.back().find("geste 0") != std::string::npos,
              "redo restores the newest applied change and its motif");
    }

    if (failures == 0) {
        std::cout << "PASS: workspace pages\n";
        return 0;
    }
    std::cerr << failures << " failure(s)\n";
    return 1;
}
