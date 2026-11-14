#pragma once

// Hors lot, 10 October 2026 -- the window's keyboard shortcuts as policy:
// the commands that can be reassigned and their default keys, how a typed
// combination reads, and how a key is shown. Plain C++, no AppKit: main.mm
// turns a chord into NSEventModifierFlags and menu key equivalents
// (tests/keyboard_shortcuts_tests.cc).
//
// The defaults follow DaVinci Resolve. Resolve is where the author's timelines
// end up and where his hands are trained, and his rule for this window is to
// take Resolve's convention when there are several. Measured against the
// default keyboard preset of Resolve 21.1, exported by its scripting API
// (ExportKeyboardPreset), eight of thirty defaults differed, six of them on a
// key that does something else in Resolve: V selects an edit point there, C
// moves the playhead to the pointer, Y selects the clips after it, F is match
// frame, `,` and `.` nudge by a frame, Alt+M clears a marker. Each command
// with a Resolve equivalent names it below, and the test holds its default to
// that action's key. A command Resolve has no key for -- the zoom tool, the
// playhead's frame/sample switch -- takes a combination Resolve leaves free.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ui::shortcuts {

// Modifier bits; main.mm maps them to NSEventModifierFlags.
enum Modifier : std::uint8_t {
    kCommand = 1u << 0,
    kOption = 1u << 1,
    kControl = 1u << 2,
    kShift = 1u << 3,
};

struct Definition {
    const char* id;
    const char* title;
    const char* default_spec;
    // The action of Resolve 21.1's default keyboard preset this command
    // answers to, or "" when Resolve has none.
    const char* resolve_action;
};

inline const std::vector<Definition>& Definitions() {
    static const std::vector<Definition> table = {
        // Its blade is B alone; C would move the playhead there.
        {"tool.select", "Outil · Sélection", "A", "editPointer"},
        {"tool.hand", "Outil · Main", "H", "viewHandMode"},
        // No zoom tool in Resolve, and every plain letter has a use there
        // (Z fits the viewer): Control-Z is free.
        {"tool.zoom", "Outil · Zoom", "Ctrl+Z", ""},
        {"tool.cut", "Outil · Lame", "B", "editBlade"},
        {"tool.cut.alternate", "Outil · Lame (secondaire)", "", ""},
        // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. T is Resolve's Trim Edit
        // mode, which "ripples edits and slips or slides clips" (Resolve
        // 21.1 manual, p. 1061 and p. 1069): the Slip tool it replaces
        // slipped on any part of a clip, edges included; the Trim tool
        // ripples on an edge and slips on the body (p. 1063 to p. 1066).
        {"tool.trim", "Outil · Trim (ripple, slip, slide)", "T", "editTrim"},
        {"play.toggle", "Lecture · Lecture/Pause", "Space",
         "controlPlayToggle"},
        {"play.reverse", "Lecture · Arrière", "J", "controlPlayReverse"},
        {"play.stop", "Lecture · Arrêt", "K", "controlStop"},
        {"play.forward", "Lecture · Avant", "L", "controlPlayForward"},
        // ON9 -- docs/roadmap/ONGLETS-2026-10.md. Resolve plays from before
        // to after the selected edit point, clip or playhead on the slash,
        // and loops it on Command-slash (manual 21.1, p. 1070). The names
        // its exported preset gives them were not read.
        {"play.around", "Lecture · Autour de la sélection", "/", ""},
        {"play.loop", "Lecture · En boucle", "Cmd+/", ""},
        {"mark.in", "Clip · Point d’entrée", "I", "markIn"},
        {"mark.out", "Clip · Point de sortie", "O", "markOut"},
        {"mark.clear", "Clip · Effacer In/Out", "Alt+X", "markResetInOut"},
        // D2 -- docs/roadmap/DIALOGUE-2026-10.md. M is the marker key in
        // every editor the people using this one come from.
        {"marker.add", "Marqueur · Poser", "M", "markMarkerAdd"},
        {"marker.note", "Marqueur · Annoter", "Shift+M", "markMarkerModify"},
        // Alt+M clears a marker in Resolve, which has no key for this.
        {"playhead.resolution", "Lecture · Tête à l’image ou à l’échantillon",
         "Alt+Shift+M", ""},
        // Delete lifts, leaving a gap; Shift+Delete ripples (Resolve 21.1
        // manual, p. 909-910). Forward Delete ripples too: see
        // kForwardDeleteMeans.
        {"edit.delete", "Édition · Supprimer", "Delete", "editBackspace"},
        {"edit.ripple", "Édition · Suppression ripple", "Shift+Delete",
         "editDelete"},
        {"edit.copy", "Édition · Copier les clips", "Cmd+C", "editCopy"},
        {"edit.paste", "Édition · Coller les clips", "Cmd+V", "editPaste"},
        {"timeline.fit", "Timeline · Tout cadrer", "Shift+Z",
         "viewZoomSubZoomToFit"},
        {"view.fullscreen", "Affichage · Vidéo plein écran", "P",
         "workspaceViewerModeCinemaViewer"},
        // ON1 -- docs/roadmap/ONGLETS-2026-10.md. Resolve's pages are Shift
        // and their digit, Media 2, Edit 4, Color 6 (manual, p. 21). The
        // names its exported preset gives them were not read: no action
        // is claimed.
        {"page.media", "Onglet · Médias", "Shift+2", ""},
        {"page.edit", "Onglet · Montage", "Shift+4", ""},
        {"page.color", "Onglet · Couleur", "Shift+6", ""},
        // ON2 -- the panels a page can call and dismiss.
        {"panel.media", "Affichage · Médiathèque", "", ""},
        {"panel.inspector", "Affichage · Inspecteur", "", ""},
        // ON3 -- the Agent's window, from any page and from its own text
        // field: hence Command. Resolve has no agent, and its manual (21.1)
        // names no Option-Command-A.
        {"panel.agent", "Affichage · Agent", "Cmd+Alt+A", ""},
        {"timeline.snapping", "Timeline · Magnétisme", "N", "editSnapping"},
        {"timeline.linked", "Timeline · Sélection liée", "Cmd+Shift+L",
         "editLinkedSelection"},
        // Out of lot -- the author, 29 October 2026. Resolve's Select All Clips
        // Forward: every clip after the playhead, every track.
        {"select.after",
         "Timeline · Tout sélectionner après la tête de lecture", "Alt+Y",
         "editSelectClipsSelectAllAfter"},
        {"transition.add", "Timeline · Ajouter un fondu enchaîné", "Cmd+T",
         "editAddTransition"},
        {"source.insert", "Source → Record · Insérer", "F9",
         "editInsertOverwriteActionInsert"},
        {"source.overwrite", "Source → Record · Écraser", "F10",
         "editInsertOverwriteActionOverwrite"},
        // FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. The trim mode at the
        // keyboard, on Resolve 21.1's keys. Each works in Selection (A) and
        // in Trim (T); a nudge, an extension and a dynamic trim resize in
        // the first and ripple in the second (manual, p. 1061-1062 and p.
        // 1069-1070), a roll is the same in both.
        // V: the edit point nearest the playhead, centred (a roll), and the
        // playhead goes to it (manual, p. 1069).
        {"trim.select", "Trim · Point de montage le plus proche", "V",
         "editSelectEditPoint"},
        // U: outgoing, centred, incoming, in turn (manual, p. 1070).
        {"trim.side", "Trim · Côté du point (sortie, roll, entrée)", "U",
         "editToggleEditPointType"},
        // Comma and period: one frame; with Shift, the "fast nudge", five
        // frames by default (manual, p. 1069-1070).
        {"trim.nudge.back", "Trim · Une image en arrière", ",",
         "editNudgeTrimStepNudgeReverse"},
        {"trim.nudge.forward", "Trim · Une image en avant", ".",
         "editNudgeTrimStepNudgeForward"},
        {"trim.nudge.back.many", "Trim · Cinq images en arrière", "Shift+,",
         "editNudgeTrimStepTrimMultiFrameLeft"},
        {"trim.nudge.forward.many", "Trim · Cinq images en avant", "Shift+.",
         "editNudgeTrimStepTrimMultiFrameRight"},
        // E: Extend Edit, the selected points to the playhead (manual, p.
        // 1070 and p. 1088).
        {"trim.extend", "Trim · Étendre à la tête de lecture", "E",
         "editNudgeTrimStepExtendEdit"},
        // W: Dynamic Trim mode, in which J, K and L trim the selection
        // instead of playing (manual, p. 1069 and p. 1085-1086).
        {"trim.dynamic", "Trim · Trim dynamique (J K L)", "W",
         "editToggleDynamicTrimMode"},
        // FC3 -- Command-R shows a clip's Retime controls, whose ends are
        // dragged to stretch it (manual, p. 1218-1219).
        {"clip.retime", "Clip · Contrôles de vitesse", "Cmd+R",
         "clipRetimeControls"},
        // Out of lot -- the author, 29 October 2026: "D on a selected shot
        // disables it: greyed, no longer seen or heard". Resolve's Clip >
        // Enable/Disable Clip, on the whole selection (ClipEnabled.h).
        {"clip.enable", "Plan · Activer / désactiver", "D",
         "clipEnableDisableClip"},
        // G6 -- docs/roadmap/GRADE-2026-09.md.
        {"color.bypass", "Couleur · Ignorer l’étalonnage", "Shift+D",
         "viewBypassColorGrades"},
        {"color.reference.grab", "Couleur · Prendre l’image de référence",
         "Cmd+Alt+G", "viewStillsStillGrab"},
        {"color.reference.split", "Couleur · Comparer à l’image de référence",
         "Cmd+Alt+W", "viewSplitScreenModeOnOffToggle"},
    };
    return table;
}

// DEC40 -- docs/roadmap/FILMCRAFT-2026-10.md. Commands whose identifier
// changed after keys were stored under the old one. tool.slip became
// tool.trim on 19 October 2026 (FC4): the Trim tool replaced the Slip tool
// on its key, and a key the person had given the Slip tool was no longer
// read, T applying instead. The author decided on 23 October that the
// identifier stays tool.trim; the key stored under the old name now opens what
// replaced it.
struct RenamedCommand {
    const char* id;
    const char* former_id;
};

inline const std::vector<RenamedCommand>& RenamedCommands() {
    static const std::vector<RenamedCommand> table = {
        {"tool.trim", "tool.slip"},
    };
    return table;
}

// The key stored for `id`, read through `stored` (the window's preferences;
// it answers whether a string is stored under a name, and which): the one
// stored under `id` wins, then the one under the name `id` replaced. False
// when neither is stored: the default applies. A key stored as "" (no key)
// is a choice, and is returned as one.
template <typename Lookup>
bool StoredSpecFor(const std::string& id, Lookup stored, std::string& spec) {
    if (stored(id, spec)) return true;
    for (const RenamedCommand& renamed : RenamedCommands())
        if (id == renamed.id && stored(renamed.former_id, spec)) return true;
    return false;
}

// A key and its modifiers. The key is lowercase: a letter, a digit or
// another single character, "space", "delete", or "f1" to "f19". Empty when
// the command has no key.
struct Chord {
    std::string key;
    std::uint32_t modifiers = 0;

    bool operator==(const Chord& other) const {
        return key == other.key && modifiers == other.modifiers;
    }
    bool operator!=(const Chord& other) const { return !(*this == other); }
};

// The combinations the window keeps for itself, which no command can take.
struct ReservedChord {
    const char* spec;
    const char* title;
};

inline const std::vector<ReservedChord>& ReservedChords() {
    static const std::vector<ReservedChord> reserved = {
        {"Cmd+Q", "Quitter"},
        {"Cmd+Comma", "Raccourcis clavier"},
        {"Cmd+Z", "Annuler"},
        {"Cmd+Shift+Z", "Rétablir"},
        {"Cmd+Shift+T", "Ajouter une piste vidéo"},
        {"Cmd+Alt+Shift+T", "Ajouter une piste audio"},
    };
    return reserved;
}

// Forward Delete is Shift+Delete, as in Resolve, whose preset gives ripple
// delete both "Del" and "Shift+Backspace" for keyboards without the key.
inline const char kForwardDeleteMeans[] = "Shift+Delete";

// 1 to 19 for "f1" to "f19", 0 for any other key.
inline int FunctionKeyNumber(const std::string& key) {
    if (key.size() < 2 || key.size() > 3 || key[0] != 'f' || key[1] == '0') {
        return 0;
    }
    int number = 0;
    for (std::size_t index = 1; index < key.size(); ++index) {
        if (key[index] < '0' || key[index] > '9') {
            return 0;
        }
        number = number * 10 + (key[index] - '0');
    }
    return number >= 1 && number <= 19 ? number : 0;
}

namespace detail {

inline std::string Trimmed(const std::string& text) {
    const auto blank = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    std::size_t first = 0;
    std::size_t last = text.size();
    while (first < last && blank(text[first])) {
        ++first;
    }
    while (last > first && blank(text[last - 1])) {
        --last;
    }
    return text.substr(first, last - first);
}

// ASCII only: main.mm lowercases the rest before calling, as NSString does.
inline std::string AsciiLower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

inline std::size_t CodePoints(const std::string& text) {
    std::size_t count = 0;
    for (unsigned char c : text) {
        if ((c & 0xC0) != 0x80) {
            ++count;
        }
    }
    return count;
}

inline std::uint32_t ModifierOf(const std::string& token) {
    if (token == "cmd" || token == "command" || token == "⌘") {
        return kCommand;
    }
    if (token == "alt" || token == "option" || token == "⌥") {
        return kOption;
    }
    if (token == "ctrl" || token == "control" || token == "⌃") {
        return kControl;
    }
    if (token == "shift" || token == "⇧") {
        return kShift;
    }
    return 0;
}

inline std::string KeyName(const std::string& key) {
    if (key == "espace") {
        return "space";
    }
    if (key == "suppr" || key == "retour") {
        return "delete";
    }
    if (key == "virgule" || key == "comma") {
        return ",";
    }
    if (key == "point") {
        return ".";
    }
    return key;
}

}  // namespace detail

// Reads "Cmd+Shift+L", "Alt+X", "Space", "F9", "⌥+⇧+M"... An empty spec is
// a command without a key. False, with the reason in French, otherwise.
inline bool ParseSpec(const std::string& spec, Chord& chord,
                      std::string& error) {
    chord = Chord{};
    error.clear();
    const std::string trimmed = detail::Trimmed(spec);
    if (trimmed.empty()) {
        return true;
    }
    std::uint32_t modifiers = 0;
    std::string key;
    bool hasKey = false;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = trimmed.find('+', start);
        const std::string token = detail::AsciiLower(detail::Trimmed(
            trimmed.substr(start, end == std::string::npos ? std::string::npos
                                                           : end - start)));
        const std::uint32_t modifier = detail::ModifierOf(token);
        if (modifier != 0) {
            modifiers |= modifier;
        } else if (!hasKey) {
            key = token;
            hasKey = true;
        } else {
            error = "Une combinaison ne peut contenir qu’une touche.";
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (!hasKey) {
        error = "Ajoutez une touche après les modificateurs.";
        return false;
    }
    key = detail::KeyName(key);
    if (detail::CodePoints(key) != 1 && key != "space" && key != "delete" &&
        FunctionKeyNumber(key) == 0) {
        error =
            "Utilisez une lettre, un chiffre, une touche F1 à F19, "
            "Space ou Delete.";
        return false;
    }
    chord.key = key;
    chord.modifiers = modifiers;
    return true;
}

// How the window shows a chord, in the order the Mac menus write
// modifiers: "⌥⇧M", "⌃Z", "F9", "Espace", "⌫". Empty for no key.
inline std::string Display(const Chord& chord) {
    if (chord.key.empty()) {
        return "";
    }
    std::string text;
    const auto has = [&](Modifier modifier) {
        return (chord.modifiers & modifier) != 0;
    };
    if (has(kControl)) {
        text += "⌃";
    }
    if (has(kOption)) {
        text += "⌥";
    }
    if (has(kShift)) {
        text += "⇧";
    }
    if (has(kCommand)) {
        text += "⌘";
    }
    if (chord.key == "space") {
        text += "Espace";
    } else if (chord.key == "delete") {
        text += "⌫";
    } else if (FunctionKeyNumber(chord.key) != 0) {
        text += "F" + chord.key.substr(1);
    } else if (chord.key.size() == 1 && chord.key[0] >= 'a' &&
               chord.key[0] <= 'z') {
        text += static_cast<char>(chord.key[0] - 'a' + 'A');
    } else {
        text += chord.key;
    }
    return text;
}

// FC4 -- docs/roadmap/FILMCRAFT-2026-10.md. Whether a key typed with
// `modifiers` is `expected`. `typed` is the character the key gave, Shift
// applied (AppKit's charactersIgnoringModifiers), `unshifted` the one it
// gives without Shift; both lowercase. On the French layout the period is
// Shift and the semicolon key: compared with Shift held, a default of "."
// never matched. A Shift the character needed is not a modifier of that
// character; it is one of the key's unshifted character, which is how
// "Shift+," reads on a US keyboard, where it types "<", and on the French
// one, where it types "?". "Shift+." cannot be typed on the French layout.
// A letter keeps its Shift whatever `unshifted` says: Shift only changes
// its case, and a key event built by hand may carry a key code another
// layout reads as something else.
inline bool TypedKeyMatches(const Chord& expected, const std::string& typed,
                            const std::string& unshifted,
                            std::uint32_t modifiers) {
    if (expected.key.empty()) {
        return false;
    }
    const bool letter = typed.size() == 1 && typed[0] >= 'a' && typed[0] <= 'z';
    const bool shiftMadeTyped = (modifiers & kShift) != 0 && !letter &&
                                !unshifted.empty() && unshifted != typed;
    if (!shiftMadeTyped) {
        return typed == expected.key && modifiers == expected.modifiers;
    }
    if (typed == expected.key &&
        (modifiers & ~static_cast<std::uint32_t>(kShift)) ==
            expected.modifiers) {
        return true;
    }
    return unshifted == expected.key && modifiers == expected.modifiers;
}

// Display of a spec as typed; empty when it has no key or does not read.
inline std::string DisplaySpec(const std::string& spec) {
    Chord chord;
    std::string error;
    return ParseSpec(spec, chord, error) ? Display(chord) : "";
}

}  // namespace ui::shortcuts
