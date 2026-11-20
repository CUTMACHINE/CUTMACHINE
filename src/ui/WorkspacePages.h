#pragma once

// ON1, ON2 -- docs/roadmap/ONGLETS-2026-10.md. The window's pages, as
// Resolve's: one mission each, a bar of tabs at the bottom, Shift and a
// digit to reach one (DaVinci Resolve 21.1 manual, p. 21: "Shift - 2 through
// 8", Media first, then Cut, Edit, Fusion, Color, Fairlight, Deliver). The
// author, 31 October 2026: "chaque onglet a une mission ; ici il y a pas mal de
// trucs dont j'ai pas besoin dans je monte et qui s'affichent quand même",
// and the Inspector and the media "trop petits pour être utiles quand je
// veux travailler vraiment dessus".
//
// So a page opens only the side panels its mission needs, and a panel it
// opens is wide: a share of the window, never the 300 and 320 points the
// panels had when every one of them was always shown. Within a page the
// panels can still be called and dismissed; what each page shows is a
// local preference (UiPreferences.h), never in the document, and Edit is
// where a fresh window starts. Plain C++, tested without AppKit: main.mm
// only wires it.

#include <algorithm>
#include <array>
#include <optional>
#include <string>

namespace ui::workspace {

enum class Page { Media, Edit, Color };

struct PageDescriptor {
    Page page;
    const char* identifier;  // stable, ASCII; preference keys only
    const char* title;       // the tab's label
    const char* command;     // its shortcut's id (KeyboardShortcuts.h)
};

// In Resolve's order, which is also the order of their digits.
inline const std::array<PageDescriptor, 3>& Pages() {
    static const std::array<PageDescriptor, 3> kPages{{
        {Page::Media, "media", "Médias", "page.media"},
        {Page::Edit, "edit", "Montage", "page.edit"},
        {Page::Color, "color", "Couleur", "page.color"},
    }};
    return kPages;
}

inline constexpr Page kDefaultPage = Page::Edit;

inline const PageDescriptor& Describe(Page page) {
    for (const PageDescriptor& descriptor : Pages())
        if (descriptor.page == page) return descriptor;
    return Pages()[1];
}

inline std::optional<Page> PageNamed(const std::string& identifier) {
    for (const PageDescriptor& descriptor : Pages())
        if (identifier == descriptor.identifier) return descriptor.page;
    return std::nullopt;
}

// What a page shows beside the timeline and Record: the media panel on the
// left, the Inspector's dock on the right, the Source viewer beside Record.
struct PanelSet {
    bool media = false;
    bool inspector = false;
    bool source = false;

    bool operator==(const PanelSet& other) const {
        return media == other.media && inspector == other.inspector &&
               source == other.source;
    }
};

// What a page opens the first time: Media is the media panel and Source,
// Edit the timeline and Record alone, Color the grade in the Inspector.
inline PanelSet DefaultPanels(Page page) {
    switch (page) {
        case Page::Media:
            return {true, false, true};
        case Page::Edit:
            return {false, false, false};
        case Page::Color:
            return {false, true, false};
    }
    return {};
}

enum class Side { Media, Inspector };

// The share of the window a panel takes on a page, and the least it ever
// gets: enough for the bins' names and two columns of thumbnails, enough
// for the Inspector's sliders to be longer than its labels. The centre --
// the viewers and the timeline -- keeps kCentreMinimum before either panel
// keeps its floor: on a small window the panels give way first.
struct PanelShare {
    double fraction;
    double floor;
};

inline PanelShare ShareOf(Page page, Side side) {
    if (side == Side::Media)
        return page == Page::Media ? PanelShare{0.56, 420.0}
                                   : PanelShare{0.30, 380.0};
    return page == Page::Color ? PanelShare{0.52, 440.0}
                               : PanelShare{0.30, 380.0};
}

inline constexpr double kCentreMinimum = 480.0;

// ON4, ON5, ON7 -- each mission has its own working surfaces. The source
// browser needs the whole height; editing keeps the timeline, Record and
// a readable trace beside it. The smoke run's reference layout can still
// ask for the original surfaces explicitly.
inline bool ShowsTimeline(Page page) { return page != Page::Media; }
inline bool ShowsRecord(Page page) { return page != Page::Media; }
inline bool ShowsActivity(Page page) { return page == Page::Edit; }
inline constexpr double kActivityWidth = 280.0;

inline double BinSidebarWidth(Page page, double panel_width) {
    if (page != Page::Media) {
        return 112.0;
    }
    return std::clamp(panel_width * 0.27, 160.0, 240.0);
}

struct PanelWidths {
    double media = 0.0;
    double inspector = 0.0;
};

// The widths of the panels `panels` opens on `page`, in a window `width`
// points wide with `divider` points between panes. A width the hand set on
// this page (`chosen`, zero when none) replaces the share, within the same
// bounds. Zero for a panel the page does not show.
inline PanelWidths SidePanelWidths(Page page, const PanelSet& panels,
                                   double width, double divider,
                                   const PanelWidths& chosen = {}) {
    const auto wanted = [&](Side side, double hand) {
        const PanelShare share = ShareOf(page, side);
        return hand > 0.0 ? std::max(hand, share.floor * 0.5)
                          : std::max(width * share.fraction, share.floor);
    };
    PanelWidths widths;
    if (panels.media) widths.media = wanted(Side::Media, chosen.media);
    if (panels.inspector)
        widths.inspector = wanted(Side::Inspector, chosen.inspector);
    const int dividers = (panels.media ? 1 : 0) + (panels.inspector ? 1 : 0);
    const double room = width - kCentreMinimum - dividers * divider;
    const double asked = widths.media + widths.inspector;
    if (asked > room && asked > 0.0) {
        const double scale = std::max(room, 0.0) / asked;
        widths.media *= scale;
        widths.inspector *= scale;
    }
    return widths;
}

}  // namespace ui::workspace
