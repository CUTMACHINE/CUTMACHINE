// Hors lot, 10 October 2026. The window's keyboard shortcuts
// (KeyboardShortcuts.h): every default reads, none takes another's key or a
// key the window keeps, each command with a DaVinci Resolve equivalent has
// Resolve's key, and no default sits on a key that does something else in
// Resolve. Then how a typed combination reads and how a key is shown.
//
// Plain C++: no AppKit, no window.

#include "KeyboardShortcuts.h"

#include <iostream>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "TestSupport.h"

namespace {

namespace keys = ui::shortcuts;

keys::Chord Read(const std::string& spec) {
    keys::Chord chord;
    std::string error;
    Check(keys::ParseSpec(spec, chord, error),
          "\"" + spec + "\" reads (" + error + ")");
    return chord;
}

std::string Refusal(const std::string& spec) {
    keys::Chord chord;
    std::string error;
    Check(!keys::ParseSpec(spec, chord, error), "\"" + spec + "\" is refused");
    return error;
}

// Bindings of Resolve 21.1.0.17's default keyboard preset, "DaVinci
// Resolve", exported by its scripting API on 10 October 2026, written as
// this window writes keys: the preset's "Ctrl" is Command on a Mac,
// "Backspace" is Delete, "Del" Forward Delete. Only the actions and keys the
// defaults meet: what each command answers to, and what Resolve does with
// the keys a default could take. Ctrl+Z (Control) and Alt+Shift+M are bound
// to nothing in that preset.
const std::vector<std::pair<std::string, std::string>>& Resolve21() {
    static const std::vector<std::pair<std::string, std::string>> bindings = {
        {"editPointer", "A"},
        {"editBlade", "B"},
        {"controlGoToMousePointer", "C"},
        {"editMatchFrame", "F"},
        {"viewHandMode", "H"},
        {"markIn", "I"},
        {"controlPlayReverse", "J"},
        {"controlStop", "K"},
        {"controlPlayForward", "L"},
        {"markMarkerAdd", "M"},
        {"editSnapping", "N"},
        {"markOut", "O"},
        {"workspaceViewerModeCinemaViewer", "P"},
        {"workspaceViewerModeCinemaViewer", "Cmd+F"},
        {"editTrim", "T"},
        {"editSelectEditPoint", "V"},
        // FC4 -- the trim keys of the same export (Resolve 21.1 manual, p.
        // 1069-1070, says the same), and what Resolve does with the keys
        // around them.
        {"editToggleEditPointType", "U"},
        {"editNudgeTrimStepTrimMultiFrameLeft", "Shift+,"},
        {"editNudgeTrimStepTrimMultiFrameRight", "Shift+."},
        {"editNudgeTrimStepExtendEdit", "E"},
        {"editToggleDynamicTrimMode", "W"},
        {"editDynamicTrimStop", "Cmd+K"},
        {"trimToggleSlideMode", "S"},
        {"editSelectItemAtPlayhead", "Shift+V"},
        {"trimSelectVideoEditPoint", "Alt+E"},
        {"trimSelectAudioEditPoint", "Shift+E"},
        {"editToggleVideoAudioSelection", "Alt+U"},
        {"editSlipAudioOneFrameReverse", "Alt+,"},
        {"editSlipAudioOneFrameForward", "Alt+."},
        {"editNudgeSwapEditReverse", "Cmd+Shift+,"},
        {"editNudgeSwapEditForward", "Cmd+Shift+."},
        {"editRangeSelection", "R"},
        {"clipRetimeControls", "Cmd+R"},
        {"clipFreezeFrame", "Shift+R"},
        {"clipResetRetime", "Cmd+Alt+R"},
        {"editSelectClipsSelectTrackAfter", "Y"},
        // Not from the measured export: the action names are read in Resolve
        // 21.1's binary, the keys are those the author uses there (29 October
        // 2026).
        {"editSelectClipsSelectAllAfter", "Alt+Y"},
        {"clipEnableDisableClip", "D"},
        {"viewZoomViewerToFit", "Z"},
        {"controlPlayToggle", "Space"},
        {"editNudgeTrimStepNudgeReverse", ","},
        {"editNudgeTrimStepNudgeForward", "."},
        {"editBackspace", "Delete"},
        {"editDelete", "Shift+Delete"},
        {"markResetInOut", "Alt+X"},
        {"markMarkerModify", "Shift+M"},
        {"markMarkerClear", "Alt+M"},
        {"viewZoomSubZoomToFit", "Shift+Z"},
        {"viewTogglePrimariesLogMode", "Alt+Z"},
        {"viewZoomActualSize", "Alt+Shift+Z"},
        {"editRedo", "Cmd+Shift+Z"},
        {"editCopy", "Cmd+C"},
        {"editPaste", "Cmd+V"},
        {"editLinkedSelection", "Cmd+Shift+L"},
        {"editAddTransition", "Cmd+T"},
        {"editInsertOverwriteActionInsert", "F9"},
        {"editInsertOverwriteActionOverwrite", "F10"},
        {"viewBypassColorGrades", "Shift+D"},
        {"viewStillsStillGrab", "Cmd+Alt+G"},
        {"viewSplitScreenModeOnOffToggle", "Cmd+Alt+W"},
    };
    return bindings;
}

}  // namespace

int main() {
    Test("every default reads, and no two share a key", [] {
        std::map<std::pair<std::string, std::uint32_t>, std::string> owners;
        for (const keys::ReservedChord& reserved : keys::ReservedChords()) {
            const keys::Chord chord = Read(reserved.spec);
            owners[{chord.key, chord.modifiers}] = reserved.title;
        }
        for (const keys::Definition& definition : keys::Definitions()) {
            const keys::Chord chord = Read(definition.default_spec);
            if (chord.key.empty()) {
                continue;
            }
            const auto taken = owners.find({chord.key, chord.modifiers});
            Check(taken == owners.end(),
                  std::string(definition.id) + " takes " +
                      definition.default_spec + ", already " +
                      (taken == owners.end() ? "" : taken->second));
            owners[{chord.key, chord.modifiers}] = definition.title;
        }
    });

    Test("a command with a Resolve equivalent has Resolve's key", [] {
        for (const keys::Definition& definition : keys::Definitions()) {
            const std::string action = definition.resolve_action;
            if (action.empty()) {
                continue;
            }
            bool found = false;
            bool same = false;
            for (const auto& [name, spec] : Resolve21()) {
                if (name != action) {
                    continue;
                }
                found = true;
                same = same || Read(spec) == Read(definition.default_spec);
            }
            Check(found, std::string(definition.id) + ": " + action +
                             " is in the measured preset");
            Check(same, std::string(definition.id) + " has " +
                            definition.default_spec + ", Resolve's " + action +
                            " another key");
        }
    });

    Test("no default sits on a key that does something else in Resolve", [] {
        for (const keys::Definition& definition : keys::Definitions()) {
            const keys::Chord chord = Read(definition.default_spec);
            if (chord.key.empty()) {
                continue;
            }
            for (const auto& [name, spec] : Resolve21()) {
                Check(Read(spec) != chord || name == definition.resolve_action,
                      std::string(definition.id) + " on " +
                          definition.default_spec + ", which is " + name +
                          " in Resolve");
            }
        }
    });

    Test("Forward Delete ripples, as Resolve's Del does", [] {
        const keys::Chord forward = Read(keys::kForwardDeleteMeans);
        bool ripple = false;
        for (const keys::Definition& definition : keys::Definitions()) {
            if (std::string(definition.id) == "edit.ripple") {
                ripple = Read(definition.default_spec) == forward &&
                         std::string(definition.resolve_action) == "editDelete";
            }
        }
        Check(ripple, "Forward Delete means the ripple delete's key");
    });

    Test("FC4: the trim commands answer to Resolve's trim actions", [] {
        const std::map<std::string, std::string> expected = {
            {"tool.trim", "editTrim"},
            {"trim.select", "editSelectEditPoint"},
            {"trim.side", "editToggleEditPointType"},
            {"trim.nudge.back", "editNudgeTrimStepNudgeReverse"},
            {"trim.nudge.forward", "editNudgeTrimStepNudgeForward"},
            {"trim.nudge.back.many", "editNudgeTrimStepTrimMultiFrameLeft"},
            {"trim.nudge.forward.many", "editNudgeTrimStepTrimMultiFrameRight"},
            {"trim.extend", "editNudgeTrimStepExtendEdit"},
            {"trim.dynamic", "editToggleDynamicTrimMode"},
            {"clip.retime", "clipRetimeControls"},
        };
        for (const auto& [id, action] : expected) {
            bool found = false;
            for (const keys::Definition& definition : keys::Definitions())
                if (id == definition.id) {
                    found = true;
                    Check(action == definition.resolve_action,
                          id + " answers to " + action);
                }
            Check(found, id + " is a command");
        }
        for (const keys::Definition& definition : keys::Definitions())
            Check(std::string(definition.id) != "tool.slip",
                  "the Slip tool is gone into the Trim tool");
    });

    // DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. A key stored for the
    // Slip tool belongs to the Trim tool that replaced it.
    Test("DEC40: a key stored under tool.slip opens the Trim tool", [] {
        const auto read = [](const std::map<std::string, std::string>& stored,
                             const std::string& id, std::string& spec) {
            return keys::StoredSpecFor(
                id,
                [&](const std::string& name, std::string& value) {
                    const auto found = stored.find(name);
                    if (found == stored.end()) return false;
                    value = found->second;
                    return true;
                },
                spec);
        };
        std::string spec;
        Check(read({{"tool.slip", "Y"}}, "tool.trim", spec) && spec == "Y",
              "the Slip tool's key is read for the Trim tool");
        Check(read({{"tool.slip", "Y"}, {"tool.trim", "Shift+T"}}, "tool.trim",
                   spec) &&
                  spec == "Shift+T",
              "a key stored under the new name wins");
        Check(read({{"tool.slip", ""}}, "tool.trim", spec) && spec.empty(),
              "no key, chosen for the Slip tool, stays a choice");
        Check(!read({{"tool.select", "A"}}, "tool.trim", spec),
              "nothing stored: the default applies");
        Check(!read({{"tool.slip", "Y"}}, "tool.select", spec),
              "the old name lends its key to its successor only");
        for (const keys::RenamedCommand& renamed : keys::RenamedCommands()) {
            bool current = false;
            for (const keys::Definition& definition : keys::Definitions()) {
                Check(
                    std::string(definition.id) != renamed.former_id,
                    std::string(renamed.former_id) + " is no command any more");
                current = current || std::string(definition.id) == renamed.id;
            }
            Check(current, std::string(renamed.id) + " is a command");
        }
    });

    Test("FC4: a Shift the character needed is not a modifier", [] {
        const keys::Chord period = Read(".");
        const keys::Chord fastForward = Read("Shift+.");
        const keys::Chord comma = Read(",");
        const keys::Chord fastBack = Read("Shift+,");
        // French layout: "." is Shift and the ";" key, "?" Shift and ",".
        Check(keys::TypedKeyMatches(period, ".", ";", keys::kShift),
              "French: Shift+; types the period, and is the period");
        Check(!keys::TypedKeyMatches(fastForward, ".", ";", keys::kShift),
              "French: Shift+; is not Shift+.");
        Check(keys::TypedKeyMatches(comma, ",", ",", 0),
              "French: the comma alone");
        Check(keys::TypedKeyMatches(fastBack, "?", ",", keys::kShift),
              "French: Shift+, types ? and is Shift+,");
        Check(!keys::TypedKeyMatches(comma, "?", ",", keys::kShift),
              "French: Shift+, is not the comma");
        // US layout: "." alone, Shift+. types ">".
        Check(keys::TypedKeyMatches(period, ".", ".", 0), "US: the period");
        Check(keys::TypedKeyMatches(fastForward, ">", ".", keys::kShift),
              "US: Shift+. types > and is Shift+.");
        Check(!keys::TypedKeyMatches(period, ">", ".", keys::kShift),
              "US: Shift+. is not the period");
        // Letters keep Shift as a modifier: the character is the same.
        Check(keys::TypedKeyMatches(Read("Shift+M"), "m", "m", keys::kShift) &&
                  !keys::TypedKeyMatches(Read("M"), "m", "m", keys::kShift),
              "Shift+M stays Shift+M");
        Check(keys::TypedKeyMatches(Read("Shift+M"), "m", ",", keys::kShift) &&
                  !keys::TypedKeyMatches(Read("M"), "m", ",", keys::kShift),
              "a letter keeps its Shift, whatever its key code reads as");
        Check(!keys::TypedKeyMatches(Read(""), "", "", 0), "no key, no match");
        Check(keys::TypedKeyMatches(Read("Cmd+R"), "r", "r", keys::kCommand),
              "Cmd+R");
    });

    Test("a combination reads its modifiers and its one key", [] {
        const keys::Chord linked = Read("Cmd+Shift+L");
        Check(linked.key == "l" &&
                  linked.modifiers == (keys::kCommand | keys::kShift),
              "Cmd+Shift+L");
        const keys::Chord spaced = Read("  alt + X ");
        Check(spaced.key == "x" && spaced.modifiers == keys::kOption,
              "spaces around the parts");
        Check(Read("⌃+⌥+⇧+⌘+K").modifiers == (keys::kControl | keys::kOption |
                                              keys::kShift | keys::kCommand),
              "the menus' symbols");
        Check(Read("Control+Option+Command+K") == Read("Ctrl+Alt+Cmd+K"),
              "modifiers by their long names");
        Check(Read("espace").key == "space" && Read("Suppr").key == "delete" &&
                  Read("retour").key == "delete" &&
                  Read("virgule").key == "," && Read("Comma").key == "," &&
                  Read("point").key == ".",
              "the French names");
        Check(Read("é").key == "é", "one accented letter is one key");
        const keys::Chord none = Read("   ");
        Check(none.key.empty() && none.modifiers == 0, "empty is no key");
    });

    Test("function keys F1 to F19", [] {
        Check(Read("F9").key == "f9" && Read("Shift+f12").key == "f12" &&
                  Read("F19").key == "f19",
              "F9, Shift+F12, F19");
        Check(keys::FunctionKeyNumber("f1") == 1 &&
                  keys::FunctionKeyNumber("f19") == 19 &&
                  keys::FunctionKeyNumber("f20") == 0 &&
                  keys::FunctionKeyNumber("f0") == 0 &&
                  keys::FunctionKeyNumber("f09") == 0 &&
                  keys::FunctionKeyNumber("f") == 0 &&
                  keys::FunctionKeyNumber("fx") == 0,
              "only F1 to F19");
        Refusal("F20");
    });

    Test("a combination that does not read says why", [] {
        Check(Refusal("Shift") == "Ajoutez une touche après les modificateurs.",
              "modifiers alone");
        Check(Refusal("A+B") ==
                  "Une combinaison ne peut contenir qu’une "
                  "touche.",
              "two keys");
        Check(Refusal("Cmd+") ==
                  "Utilisez une lettre, un chiffre, une "
                  "touche F1 à F19, Space ou Delete.",
              "a separator without its key");
        Refusal("Cmd+tab");
    });

    Test("a key is shown as the Mac menus write it", [] {
        // ui_smoke's D2 reads "(⌥M)" off the status line.
        Check(keys::DisplaySpec("Alt+M") == "⌥M", "Alt+M");
        Check(keys::DisplaySpec("Alt+Shift+M") == "⌥⇧M", "Alt+Shift+M");
        Check(keys::DisplaySpec("Ctrl+Z") == "⌃Z", "Ctrl+Z");
        Check(keys::DisplaySpec("Cmd+Shift+L") == "⇧⌘L", "Cmd+Shift+L");
        Check(keys::DisplaySpec("F10") == "F10", "F10");
        Check(keys::DisplaySpec("Space") == "Espace", "Space");
        Check(keys::DisplaySpec("Shift+Delete") == "⇧⌫", "Shift+Delete");
        Check(keys::DisplaySpec(",") == ",", "a comma");
        Check(keys::DisplaySpec("").empty() && keys::DisplaySpec("A+B").empty(),
              "nothing for no key or a spec that does not read");
    });

    return failures == 0 ? 0 : 1;
}
