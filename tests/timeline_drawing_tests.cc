// The timeline's display list against the geometry a press finds.
//
// -timelineRenderData drew the clips in main.mm while TimelineView.cc decided
// where a click lands: two codes for one rectangle, and a divergence that
// showed at the mouse and failed no test (AGENTS.md, "Interface"). The list
// is built in portable C++ now (TimelineDrawing.h), so the two are compared
// here, under Linux, without a window: every clip a press can find is drawn
// across exactly the width that finds it, and nothing is drawn past the
// drawable. Vertically the two disagree by one point, measured and pinned
// below until the interface decides which one moves.

#include "TimelineDrawing.h"

#include "EditLog.h"
#include "TimelineView.h"
#include "UiTheme.h"

#include <cmath>
#include <iostream>
#include <string>
#include <variant>
#include <vector>

#include "TestSupport.h"

namespace {

const Ulid kSource = "01KTD000000000000000000001";
const Ulid kVideoTrack = "01KTD000000000000000000002";
const Ulid kAudioTrack = "01KTD000000000000000000003";

Document Fixture() {
    Document document;
    document.sources = {{kSource, "source.mov", {25, 1}, {2500, 25}}};
    document.sequence.tracks = {
        {kVideoTrack,
         "video",
         0,
         {{"01KTD000000000000000000011", kSource, {0, 25}, {50, 25}, {0, 25}},
          {"01KTD000000000000000000012",
           kSource,
           {75, 25},
           {25, 25},
           {100, 25}},
          // Far right: past the drawable, so neither drawn nor found.
          {"01KTD000000000000000000013",
           kSource,
           {0, 25},
           {25, 25},
           {5000, 25}}}},
        {kAudioTrack,
         "audio",
         0,
         {{"01KTD000000000000000000021", kSource, {0, 25}, {50, 25}, {0, 25}}}},
    };
    return document;
}

struct Drawn {
    Document document = Fixture();
    EditLog log;
    TimelineViewport viewport;
    std::vector<ResolvedSlot> requested;
    std::set<Ulid> offline;
    std::map<Ulid, AudioWaveform> waveforms;
    trim::TrimSession trim;
    AgentJournal journal;
    std::vector<Ulid> retime;
    TimelineDrawingInput input;

    Drawn() {
        viewport.view_start = {0, 25};
        viewport.pixels_per_second = 100.0;
        viewport.track_height = 44.0;
        viewport.header_width = 96.0;
        input.document = &document;
        input.shown_document = &document;
        input.viewport = &viewport;
        input.requested = &requested;
        input.offline_source_ids = &offline;
        input.waveforms = &waveforms;
        input.trim_session = &trim;
        input.agent_journal = &journal;
        input.retime_clip_ids = &retime;
        input.width = 900.0;
        input.timeline_height = 300.0;
        input.video_height = 200.0;
        input.ruler_label_width = 74.0;
        input.marker_note_width = [](const std::string& note) {
            return 6.0 * static_cast<double>(note.size());
        };
        input.duration = {125, 25};
    }
};

bool Near(double left, double right) { return std::abs(left - right) < 1e-9; }

}  // namespace

int main() {
    Test("every clip a press finds is drawn over the rectangle that finds it",
         [] {
             Drawn drawn;
             Project project;
             TimelineInteraction interaction(drawn.document, drawn.log,
                                             drawn.viewport);
             drawn.input.project = &project;
             drawn.input.interaction = &interaction;
             const TimelineRenderData data =
                 BuildTimelineRenderData(drawn.input);
             const std::vector<TimelineClipRect> found = VisibleTimelineClips(
                 drawn.document, drawn.viewport, drawn.input.width,
                 interaction.SelectedClipIds(), std::nullopt);
             Check(!found.empty(), "the fixture has clips in view");
             const double top = drawn.input.video_height;
             for (const TimelineClipRect& clip : found) {
                 const double left =
                     std::max(std::min(clip.x, clip.x + clip.width),
                              drawn.viewport.header_width);
                 const double right = std::min(
                     std::max(clip.x, clip.x + clip.width), drawn.input.width);
                 if (right <= left) continue;
                 // The clip's body: the tallest rectangle drawn across its
                 // whole visible width, starting inside its lane.
                 const MetalRect* body = nullptr;
                 for (const MetalDrawCommand& command : data.overlays) {
                     const auto* rect = std::get_if<MetalRect>(&command);
                     if (rect == nullptr || !Near(rect->x, left) ||
                         !Near(rect->width, right - left) ||
                         rect->height < 10.0)
                         continue;
                     if (rect->y >= top + clip.y &&
                         rect->y < top + clip.y + clip.height &&
                         (body == nullptr || rect->height > body->height))
                         body = rect;
                 }
                 Check(body != nullptr,
                       "clip " + clip.clip_id + " is drawn where it is found");
                 if (body == nullptr) continue;
                 // Measured when the list left main.mm: the body is drawn
                 // from 2 points below the rectangle's top, which already
                 // carries kTimelineClipRowInset, over kTimelineClipBodyHeight
                 // -- so its last point is drawn below what a press finds.
                 // Pinned, not endorsed: closing it moves either pixels (the
                 // window test's capture) or the hit zone, a decision for
                 // the interface. Change kDrawnBelowFound with that fix.
                 constexpr double kDrawnBelowFound = 1.0;
                 const double drawnBottom = body->y + body->height;
                 const double foundBottom = top + clip.y + clip.height;
                 Check(body->y >= top + clip.y &&
                           Near(drawnBottom - foundBottom, kDrawnBelowFound),
                       "clip " + clip.clip_id +
                           "'s body overhangs what a press finds by the "
                           "measured point, no more, no less (drawn " +
                           std::to_string(body->y - top) + "+" +
                           std::to_string(body->height) + ", found " +
                           std::to_string(clip.y) + "+" +
                           std::to_string(clip.height) + ")");
             }
         });

    Test("a clip outside the drawable is neither drawn nor found", [] {
        Drawn drawn;
        Project project;
        TimelineInteraction interaction(drawn.document, drawn.log,
                                        drawn.viewport);
        drawn.input.project = &project;
        drawn.input.interaction = &interaction;
        const TimelineRenderData data = BuildTimelineRenderData(drawn.input);
        const double farX = drawn.viewport.TimeToX({5000, 25});
        Check(farX > drawn.input.width, "the far clip starts past the edge");
        for (const MetalDrawCommand& command : data.overlays) {
            const auto* rect = std::get_if<MetalRect>(&command);
            Check(rect == nullptr || rect->x < drawn.input.width,
                  "nothing is drawn past the drawable's width");
        }
    });

    Test("the timeline is drawn below the monitor, over its whole width", [] {
        Drawn drawn;
        Project project;
        TimelineInteraction interaction(drawn.document, drawn.log,
                                        drawn.viewport);
        drawn.input.project = &project;
        drawn.input.interaction = &interaction;
        const TimelineRenderData data = BuildTimelineRenderData(drawn.input);
        Check(data.video_height == drawn.input.video_height,
              "the monitor keeps the height it was given");
        bool background = false;
        for (const MetalDrawCommand& command : data.overlays) {
            const auto* rect = std::get_if<MetalRect>(&command);
            if (rect != nullptr && Near(rect->x, 0.0) &&
                Near(rect->y, drawn.input.video_height) &&
                Near(rect->width, drawn.input.width) &&
                Near(rect->height, drawn.input.timeline_height))
                background = true;
        }
        Check(background, "the timeline's background spans the drawable");
    });

    return failures == 0 ? 0 : 1;
}
