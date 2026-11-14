#pragma once

// PRD27 -- docs/roadmap/PRODUIT-2026-10b.md. What VoiceOver reads of the
// three views AppKit does not draw. The timeline and the two monitors are
// Metal surfaces (AGENTS.md, "les quatre faits"): to VoiceOver they were
// three silent rectangles. Each now says what it is, and the timeline what it
// holds -- where the playhead stands, as its timecode reads on screen, and
// which shot is selected. The window asks for that value each time VoiceOver
// reads it, so it is never stale.
//
// Plain C++: no AppKit. main.mm hands in the playhead, the rate its timecode
// is shown at, and the names the timeline draws on the selected clips; it
// only passes the result on. tests/ui_accessibility_tests.cc.

#include "Document.h"
#include "RationalTime.h"
#include "Timecode.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace ui::accessibility {

// What each Metal view is called, in the words of the window itself.
inline constexpr const char* kTimelineLabel = "Timeline";
inline constexpr const char* kSourceMonitorLabel = "Moniteur Source";
inline constexpr const char* kRecordMonitorLabel = "Moniteur Record";

// How many selected shots the value names before it counts the others.
inline constexpr size_t kNamedSelection = 3;

// A name as it is spoken: a carton's line breaks read as spaces, and a shot
// without a name is still a shot.
inline std::string SpokenClipName(const std::string& name) {
    std::string spoken = name;
    std::replace(spoken.begin(), spoken.end(), '\n', ' ');
    std::replace(spoken.begin(), spoken.end(), '\r', ' ');
    return spoken.empty() ? "plan sans nom" : spoken;
}

// A selected clip, as the timeline shows it.
struct SelectedClip {
    // The name the timeline draws on it.
    std::string name;
    // Its link group (DocumentClip::link_group_id); empty without one.
    std::string link_group;
};

// The shots a selection holds, by name, in its order. A picture and its
// sound, linked, are selected together and drawn as one shot: they are one
// here, named by the first of them, so that selecting an imported rush does
// not read « 2 plans sélectionnés : A et A ».
inline std::vector<std::string> SelectedShots(
    const std::vector<SelectedClip>& clips) {
    std::vector<std::string> shots;
    std::vector<std::string> groups;
    for (const SelectedClip& clip : clips) {
        if (!clip.link_group.empty()) {
            if (std::find(groups.begin(), groups.end(), clip.link_group) !=
                groups.end()) {
                continue;
            }
            groups.push_back(clip.link_group);
        }
        shots.push_back(clip.name);
    }
    return shots;
}

// The timeline's value: "Tête de lecture 00:00:01:12, aucun plan
// sélectionné", "…, plan sélectionné : Interview A", "…, 3 plans
// sélectionnés : A, B et C", "…, 5 plans sélectionnés : A, B, C et 2
// autres". `selected` is SelectedShots'. The timecode is FormatTimecode's,
// the label's under the timeline, at the same rate.
inline std::string TimelineValue(const RationalTime& playhead, MediaRate rate,
                                 const std::vector<std::string>& selected) {
    std::string value =
        "Tête de lecture " + FormatTimecode(playhead, rate) + ", ";
    if (selected.empty()) {
        return value + "aucun plan sélectionné";
    }
    if (selected.size() == 1) {
        return value + "plan sélectionné : " + SpokenClipName(selected.front());
    }
    value += std::to_string(selected.size()) + " plans sélectionnés : ";
    const size_t named = std::min(selected.size(), kNamedSelection);
    const size_t others = selected.size() - named;
    for (size_t index = 0; index < named; ++index) {
        if (index > 0) {
            value += others == 0 && index + 1 == named ? " et " : ", ";
        }
        value += SpokenClipName(selected[index]);
    }
    if (others > 0) {
        value += " et " + std::to_string(others) +
                 (others == 1 ? " autre" : " autres");
    }
    return value;
}

}  // namespace ui::accessibility
