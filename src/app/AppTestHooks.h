#pragma once

// STRUCTURE-2026-11 ST1 -- docs/roadmap/STRUCTURE-2026-11.md. The window
// test (cutmachine_ui_smoke_tests) and its play probe link the very objects
// of the editor; they used to compile main.mm a second time under a macro,
// so that what they ran was not what a person runs. What they observe of
// the editor, and what they answer in a person's place, goes through this
// seam instead, which only their executable installs (tests/ui_smoke/).
// The product installs nothing: every call site then does exactly what it
// did in the product before the seam.

#import <AppKit/AppKit.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "ReadAhead.h"
#include "Ulid.h"

@class AppDelegate;

namespace cutmachine_app {

// How playback reads ahead (PERF13 to PERF17). The defaults are the
// product's; the play probe turns one off, or changes a horizon, to measure
// what it is worth on the same binary.
struct PlaybackTuning {
    // PERF13 -- a track an opaque shot hides is neither composited nor
    // decoded in playback.
    bool hide_occluded = true;
    // PERF14 -- off, playback reads as before PERF14: one second ahead of a
    // cut or of a hidden track shown again, and no bytes asked ahead.
    bool read_ahead = true;
    int64_t byte_seconds = read_ahead::kByteHorizonSeconds;
    int64_t reappear_seconds = read_ahead::kReappearanceLookaheadSeconds;
    int64_t cut_seconds = read_ahead::kCutLookaheadSeconds;
    // PERF15 -- the one reader ahead of forward playback.
    bool reader = true;
    // PERF17 -- the next piece of the same rush, decoded on into.
    bool next_piece = true;
};

class AppTestHooks {
public:
    virtual ~AppTestHooks() = default;

    // What the editor tells the run.
    // The media browser: an icon pressed, a drag of `mediaCount` media
    // written to the pasteboard, then its dragging session begun.
    virtual void MediaIconPressed() = 0;
    virtual void MediaDragWritten(NSUInteger mediaCount) = 0;
    virtual void MediaDragSessionBegins() = 0;
    // The editor's window is up, with `decoders` decoders open.
    virtual void EditorWindowShown(size_t decoders) = 0;
    // The window is laid out and its display timer runs: the run starts.
    virtual void EditorReady(AppDelegate* editor) = 0;
    // A decoder starts opening on a source; one is installed, `seconds`
    // after its open started (`playing`: during playback); the decoders
    // are reloaded.
    virtual void DecoderOpening(const Ulid& sourceId) = 0;
    virtual void DecoderInstalled(const std::string& file, double seconds,
                                  bool playing) = 0;
    virtual void DecodeWorkersReloaded() = 0;
    // A tick at the display's deadline; `topmostLate` when the topmost
    // active layer had no frame for it.
    virtual void DisplayDeadline(bool topmostLate) = 0;
    // An event of the agent reached the window's journal.
    virtual void AgentEventRecorded() = 0;

    // What the run answers in a person's place.
    // The window as it was before pages, every panel open, and nothing
    // written into the editor's preferences.
    virtual bool LaysOutEveryPanel() = 0;
    // Whether the run answers the save sheets; then the destination it set,
    // taken once, or nil.
    virtual bool AnswersSaveSheets() = 0;
    virtual NSURL* TakeSaveSheetAnswer() = 0;
    // Where Record's reference still is kept; nil for the person's folder.
    virtual NSURL* ReferenceStillURL() = 0;
    // The display list without the clip badges and their veil (PRD24).
    virtual bool HidesClipSignage() = 0;
    // The local time a journal line says; empty for the clock's.
    virtual std::string AgentJournalTime() = 0;
    virtual PlaybackTuning Playback() = 0;
};

// The hooks the executable installed, or null in the product.
AppTestHooks* TestHooks();
// Installed once, before the editor starts, by the test's executable.
void InstallTestHooks(AppTestHooks* hooks);

// The executable's body (main.mm): the command line, or the editor.
// `editorProject`, when given, skips the command line and opens the editor
// on that project file, as the window test and the play probe do.
int RunCutmachine(int argc, char* argv[], const char* editorProject);

}  // namespace cutmachine_app
