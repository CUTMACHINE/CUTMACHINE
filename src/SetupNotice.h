#pragma once

// PRD21 (window) -- docs/roadmap/PRODUIT-2026-10b.md. What the window says of
// the setup report (SetupCheck.h): one line on the welcome screen when
// something required is missing, and the whole diagnosis, in French, behind
// « CUTMACHINE ▸ Diagnostic de l'installation… ».
//
// An app opened from the Finder gets launchd's PATH, where FFmpeg is not, and
// nothing said so at launch: thumbnails, waveforms, proxies and every export
// failed one task at a time, each in the engine's words. The report knows
// what is missing; this decides what the window shows of it.
//
// The French is composed from each item's identifier, its state and the
// structured parts of what was found (paths, origins, the capabilities an
// FFmpeg lacks, the files of MFA it could not find), never by translating
// the report's English sentences. Those stay the engine's: they go along,
// untouched, in the JSON that « Copier » appends (ClipboardText).
//
// One setting the report does not read belongs to the window: a key its
// agent panel saved (in the Keychain). The window reads it and hands it over
// as WindowFacts; nothing here reads preferences, the Keychain, the
// environment or the file system. Either the engine's setting or the
// window's key makes that item ready, and the text says which one serves
// what. The Whisper model was such a setting too, a preference of the
// window's menus, until DEC23 (WhisperModelSetting.h): they read the
// engine's now, and « CUTMACHINE ▸ Modèle Whisper… » writes it.
//
// Plain C++: no AppKit. tests/setup_notice_tests.cc.

#include "SetupCheck.h"

#include <string>
#include <vector>

namespace setup_notice {

// What the window knows besides the report.
struct WindowFacts {
    // The settings file a repair names, as the text shows it (the window
    // writes the home directory as ~). Empty: repairs name the environment
    // only.
    std::string settings_file;
    // A key is saved in the agent panel's settings, in the Keychain. Whether
    // one is, only: the key itself never comes here.
    bool agent_key_saved = false;
    // What the binary is, "0.1.0, commit 40a62d4", the text's first line.
    // Empty: no such line.
    std::string build;
};

// « prêt », « absent », « incomplet », « facultatif, absent ».
const char* StateLabel(setup::State state);

// The name the window gives an item: « FFmpeg », « Modèle Whisper »,
// « MFA », « Clé de l'agent »,
// « Copie de l'archive », « Voix off ». An identifier this version does not
// know is shown as it is.
std::string ItemTitle(const std::string& id);

// One item of the diagnosis, with the window's facts taken into account.
struct Entry {
    std::string id;
    std::string title;
    // The report's state, except for the agent key, ready when the window's
    // own key is.
    setup::State state = setup::State::kMissing;
    bool required = false;
    // What was found, or what is wrong with it: one sentence a line.
    std::vector<std::string> found;
    // What cannot be done without it, and how to repair it. Empty when the
    // entry is ready.
    std::string prevents;
    std::string repair;
};

// The report's items, in its order.
std::vector<Entry> Entries(const setup::Report& report,
                           const WindowFacts& facts);

// The welcome screen's line: empty unless a required item is missing.
// Without FFmpeg: « FFmpeg introuvable : les vignettes, les formes d'onde et
// l'export vidéo ne fonctionneront pas. »
std::string WelcomeLine(const setup::Report& report);

// One sentence for the whole report: the welcome line when something
// required is missing, else that nothing required is.
std::string Summary(const setup::Report& report);

// A stretch of the diagnosis. The window sets headings apart (one per
// entry: « FFmpeg : absent »); the text is the runs end to end.
struct Run {
    std::string text;
    bool heading = false;
};

// The build line, the summary, then each entry: its heading, what was
// found, « Sans … » and « À faire : … » when it is not ready. Ends with a
// word on the settings file when a repair may send someone there.
std::vector<Run> DiagnosticRuns(const setup::Report& report,
                                const WindowFacts& facts);

// The runs' text, end to end: what the dialog shows.
std::string DiagnosticText(const setup::Report& report,
                           const WindowFacts& facts);

// What « Copier » puts on the pasteboard, to send to someone who helps: the
// text, then the engine's report as `cutmachine --check-setup` prints it
// (setup::ReportJson), its English sentences included. Never a key.
std::string ClipboardText(const setup::Report& report,
                          const WindowFacts& facts);

}  // namespace setup_notice
