#pragma once

// MOTION-AFTER-2026-09 -- a motion composition handed to After Effects as a
// project a motion designer can retouch: precompositions, layers and
// keyframes, never baked frames. It revisits the decision of MOTION-2026-09
// ("the bridge stops at versions and rendering") at the author's request of 28
// September 2026, and keeps its principle: After is an output, not another
// owner. The project it builds is a fork; nothing comes back.
//
// The engine describes, a fixed JSX script builds (tools/after-bridge/
// scripts/motion_import.jsx). The description is data a test can read
// without After: which layer each node became, where it sits, and every
// keyframe with the ease After needs to draw the same curve.
//
// Mapping (docs/MOTION_FORMAT.md, « Vers After Effects ») --
//   sequence                        -> precomposition, shifted to `from`
//   group with opacity, blend mode,
//   filter or overflow hidden       -> precomposition (After's group)
//   other group                     -> null, parent of its children
//   box with a background           -> shape layer, rectangle and fill
//   text                            -> text layer, its font's PostScript
//                                      name, set on the engine's baseline
//   path                            -> shape layer, trim paths animated
//   image                           -> footage layer
//   video, audio (MA11)             -> footage layer, its sound included
//   clipPath (MA11)                 -> layer mask, keyed when animated
//   text highlights (MB3)           -> rectangles below the editable text,
//                                      together in a precomposition
// Keyframes keep their frames; cubic-bezier easing becomes After's speed and
// influence, which draw the same curve on one dimension; step-end becomes a
// hold keyframe. What After cannot say the same way is reported, never
// silently dropped: slice() becomes an approximating Wave Warp.
//
// MA11 -- what follows from the layout rather than from a keyframe (a box
// whose width animates, an image that covers it, a word its neighbour
// pushes) is read from the engine at every frame it moves: a curve is
// written only when it gives back every one of those frames, a key per
// frame otherwise, and the report says so. MA12 -- the motion blur goes
// with the composition: After's shutter opens on the frame, as the
// engine's does.

#include "Json.h"
#include "MotionComposition.h"
#include "MotionEvaluate.h"

#include <atomic>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

class Document;

inline constexpr char kMotionAfterSchema[] = "after-motion.v1";

// A sound of the timeline the composition plays in: an audio layer of the
// main composition, on the same stretch of its file, its gain and its fades
// as audio level keyframes. Seconds, since After places audio on no frame.
struct MotionAfterAudio {
    std::string path;
    std::string name;
    double source_in = 0.0;
    double timeline_in = 0.0;
    double duration = 0.0;
    double gain_db = 0.0;
    double fade_in = 0.0;
    double fade_out = 0.0;
};

struct MotionAfterExportOptions {
    // Name of the main composition in After.
    std::string name = "Motion";
    // Relative image sources resolve against this directory.
    std::string media_directory;
    // MA11 -- the file of each project media a video or an audio node
    // names by ULID (MotionReach::media_paths).
    std::map<std::string, std::string> media_paths;
    // Where the JSX saves the project; empty: it builds and leaves it open.
    std::string aep_path;
    // MOTION-AFTER-2026-09 -- the timeline's sounds, under the pictures.
    std::vector<MotionAfterAudio> audio;
    // MA4 -- ids of the nodes that boil: a translateX or translateY held at
    // a constant step (a texture that jumps every n frames) becomes an
    // expression a motion designer can retune, posterizeTime and random
    // within the track's range, instead of one hold keyframe per jump. The
    // jumps are then After's, not the engine's: every track converted is
    // named in the report. The nodes are named, never guessed from their
    // keyframes; one without such a track is refused.
    std::vector<std::string> boil;
};

struct MotionAfterExportReport {
    int64_t comps = 0;
    int64_t layers = 0;
    int64_t keyframes = 0;
    // "path: what After gets instead" -- approximated or left out.
    std::vector<std::string> notes;
};

bool ExportMotionToAfter(const MotionComposition& composition,
                         const mcp_json::Value& props, MotionMeasurer& measurer,
                         const MotionEvaluationContext& context,
                         const MotionAfterExportOptions& options,
                         mcp_json::Value& description,
                         MotionAfterExportReport& report, MotionError& error);

// MA1 -- the sounds of one timeline (`document` is that timeline's) as its
// audio tracks play them, mute and solo included; a relative source resolves
// against `directory`. A clip played at another speed is refused (Unsupported):
// an audio layer has no equivalent here.
bool MotionAfterTimelineAudio(const Document& document,
                              const std::string& directory,
                              std::vector<MotionAfterAudio>& audio,
                              MotionError& error);

// MA1 -- what a surface of the model hands the describer: the composition,
// its props, the fonts to register, the project's reach (media and the
// compositions an instance may show) and the export's options.
struct MotionAfterRequest {
    const MotionComposition* composition = nullptr;
    mcp_json::Value props;
    std::vector<std::string> font_directories;
    MotionReach reach;
    MotionAfterExportOptions options;
};

// Measuring text needs Core Text, which the model does not link: the
// executable installs the describer beside the motion renderers
// (MotionFrameRenderer.h says why); null in a process that links the model
// alone, where export_motion_after is then refused.
using MotionAfterDescriber = bool (*)(const MotionAfterRequest& request,
                                      mcp_json::Value& description,
                                      MotionAfterExportReport& report,
                                      MotionError& error);

namespace motion_after_describer {
inline std::atomic<MotionAfterDescriber>& Slot() {
    static std::atomic<MotionAfterDescriber> describer{nullptr};
    return describer;
}
}  // namespace motion_after_describer

inline void SetMotionAfterDescriber(MotionAfterDescriber describer) {
    motion_after_describer::Slot().store(describer);
}

inline MotionAfterDescriber CurrentMotionAfterDescriber() {
    return motion_after_describer::Slot().load();
}

// The report as the CLI and export_motion_after give it: comps, layers,
// keyframes and notes.
void MotionAfterReportFields(const MotionAfterExportReport& report,
                             mcp_json::Value& result);

// After's temporal ease for one cubic-bezier segment of a dimension going
// from v0 to v1 in `seconds`: the out ease of its first keyframe and the in
// ease of its second, each (speed in units per second, influence in
// percent). Exposed for tests.
struct MotionAfterEase {
    double speed = 0.0;
    double influence = 0.0;
};
void MotionAfterBezierEase(double x1, double y1, double x2, double y2,
                           double v0, double v1, double seconds,
                           MotionAfterEase& out, MotionAfterEase& in);
