#pragma once

// SN7 -- docs/roadmap/SON-2026-10.md. A ditty rendered against a timeline of
// the open project, by the engine, without Node.
//
// The render is tools/cutmachine-sound's own JavaScript (job.mjs), played by
// cutmachine-sound-host (SoundHost.mm) in a JavaScriptCore context: a process
// of its own, found beside the engine's executable, the only one signed with
// com.apple.security.cs.allow-jit, and stopped when it runs past its time (a
// JSContext has no public time limit; a process can be killed). It answers the
// report `cm-sound render` prints, and the same bytes (SN7, measured against
// the Node CLI).
//
// What the engine adds is the timeline as it is now: the document handed to the
// host is the one in memory, so a render follows the window's last edit without
// a save in between. The cuts are read from it by the renderer's own rules
// (project.mjs, SN2), never here.
//
// The host is built on macOS only. Without one -- the engine without a window,
// under Linux -- the render is refused by name; `node
// tools/cutmachine-sound/cli.mjs` renders the same JavaScript there.

#include "Document.h"
#include "Json.h"

#include <string>

struct SoundRenderRequest {
    // The ditty, or with a partition the instrument set; files read when the
    // job is built.
    std::string ditty_path;
    std::string partition_path;
    int64_t seed = 1;
    // Values for the ditty's input.* sliders: numbers and texts.
    mcp_json::Value settings = mcp_json::Value::MakeObject();
    int32_t sample_rate = 48000;
    // Seconds rendered past the edit's end (a reverb tail), a number; null for
    // none.
    mcp_json::Value tail;
    // The video track that makes the cuts, when several hold clips; empty for
    // the renderer's rule.
    std::string track_id;
    // The timecode of the timeline's first frame; empty for 01:00:00:00.
    std::string start_timecode;
    // Absolute: where the stem goes, and the folder of one WAV per loop (empty:
    // none).
    std::string out_path;
    std::string stems_dir;
    // Also report each loop's trigger samples and their notes.
    bool triggers = false;
    int timeout_seconds = 300;
};

// The job the host reads (job.mjs's), for `document`'s timeline. False with the
// reason when a file cannot be read or a setting is neither a number nor a
// text.
bool BuildSoundJob(const Document& document, const SoundRenderRequest& request,
                   mcp_json::Value& job, std::string& error);

// CUTMACHINE_SOUND_HOST when set, else cutmachine-sound-host beside the running
// executable; empty when there is none.
std::string SoundHostPath();

// Renders, and answers the host's report as JSON. Refusals, by `errorName`:
// SoundHostMissing (no host), SoundTimeout (stopped after timeout_seconds),
// SoundRenderFailed (the renderer refused: a ditty that throws, a partition
// naming a cut the edit no longer has...), IoError.
bool RenderSound(const Document& document, const SoundRenderRequest& request,
                 std::string& reportJson, std::string& errorName,
                 std::string& message);
