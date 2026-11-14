// PERF13 -- MonitorOcclusion.h's promise, on the GPU: a layer the rule
// hides contributes nothing to Record. For each case the monitor's own
// passes (Renderer::RenderFramesToPixels, as main.mm grabs Record) render
// the composite with the hidden layer and without it, and the two must be
// equal value for value; the rule, asked about a document drawn the same
// way, must agree. Two controls -- a translucent shot, a letterboxed one --
// show that the comparison does see the layer underneath when it shows.
// Skipped (77) without a Metal device.

#include "ClipFraming.h"
#include "Document.h"
#include "MonitorOcclusion.h"
#include "Renderer.h"
#include "Timeline.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

const Ulid kLower = "01KQ0000000000000000000001";
const Ulid kUpper = "01KQ0000000000000000000002";
const Ulid kV1 = "01KQ0000000000000000000010";
const Ulid kV2 = "01KQ0000000000000000000011";
const Ulid kLowerClip = "01KQ0000000000000000000020";
const Ulid kUpperClip = "01KQ0000000000000000000021";

// A flat picture without alpha, planar RGB as the renderer reads it.
AVFrame* Flat(int width, int height, int red, int green, int blue) {
    AVFrame* frame = av_frame_alloc();
    frame->format = AV_PIX_FMT_GBRP;
    frame->width = width;
    frame->height = height;
    frame->color_range = AVCOL_RANGE_JPEG;
    av_frame_get_buffer(frame, 0);
    const int values[3] = {green, blue, red};
    for (int plane = 0; plane < 3; ++plane)
        for (int row = 0; row < height; ++row)
            std::fill(frame->data[plane] + row * frame->linesize[plane],
                      frame->data[plane] + row * frame->linesize[plane] + width,
                      static_cast<uint8_t>(values[plane]));
    return frame;
}

struct Layer {
    int width = 0;
    int height = 0;
    int red = 0;
    int green = 0;
    int blue = 0;
    // As a fraction, the way a clip holds it.
    EffectParamValue opacity{1, 1};
    bool fill = false;
};

struct Case {
    std::string label;
    int32_t sequenceWidth = 320;
    int32_t sequenceHeight = 180;
    uint32_t targetWidth = 320;
    uint32_t targetHeight = 180;
    Layer lower;
    Layer upper;
    // Whether the rule hides the lower layer, and so whether the two
    // renders must be equal.
    bool hidden = true;
};

// The document the case draws: the lower layer on V1, the upper on V2, both
// across the whole second, described as an ingest would describe them.
Document DocumentOf(const Case& item) {
    Document document;
    document.sequence.width = item.sequenceWidth;
    document.sequence.height = item.sequenceHeight;
    document.sequence.frame_rate = {25, 1};
    const auto add = [&](const Ulid& media, const Ulid& track,
                         const Ulid& clipId, int32_t index,
                         const Layer& layer) {
        LibraryMedia described;
        described.id = media;
        described.path = media + ".mov";
        described.metadata_complete = true;
        described.width = layer.width;
        described.height = layer.height;
        described.rate = {25, 1};
        described.duration = {25, 25};
        document.library.push_back(described);
        document.sources.push_back(
            {media, described.path, described.rate, described.duration});
        DocumentClip clip;
        clip.id = clipId;
        clip.source_id = media;
        clip.timeline_in = {0, 25};
        clip.duration = {25, 25};
        clip.source_in = {0, 25};
        clip.opacity = layer.opacity;
        if (layer.fill) clip.framing.mode = FramingMode::Fill;
        DocumentTrack lane;
        lane.id = track;
        lane.kind = "video";
        lane.index = index;
        lane.clips = {clip};
        document.sequence.tracks.push_back(lane);
    };
    add(kLower, kV1, kLowerClip, 0, item.lower);
    add(kUpper, kV2, kUpperClip, 1, item.upper);
    return document;
}

// Record's display list for these layers, built as
// -presentNearestFrameAtDeadline: builds it.
TimelineRenderData DataOf(const Case& item, const Document& document,
                          const std::vector<const Layer*>& layers,
                          const ColorManagementSettings& color) {
    TimelineRenderData data;
    data.color_management = color;
    data.display_sdr_preview = true;
    data.sequence_width = item.sequenceWidth;
    data.sequence_height = item.sequenceHeight;
    data.video_height = item.targetHeight;
    for (const Layer* layer : layers) {
        data.video_rotation_degrees.push_back(0);
        data.video_opacities.push_back(static_cast<float>(layer->opacity.num) /
                                       static_cast<float>(layer->opacity.den));
        data.video_color_grades.emplace_back();
        data.video_color_inputs.emplace_back();
        data.video_veils.emplace_back();
        data.video_display_referred.push_back(0);
        MonitorFraming framing;
        if (layer->fill) {
            ClipFraming filled;
            filled.mode = FramingMode::Fill;
            ClipFramingGeometry geometry;
            std::string error;
            if (ResolveClipFraming(filled, layer->width, layer->height,
                                   document.sequence.width,
                                   document.sequence.height, geometry, error))
                framing =
                    MonitorFramingOf(geometry, layer->width, layer->height);
        }
        data.video_framings.push_back(framing);
    }
    return data;
}

std::vector<float> Render(Renderer& renderer, const Case& item,
                          const Document& document,
                          const std::vector<const Layer*>& layers,
                          const ColorManagementSettings& color) {
    std::vector<AVFrame*> frames;
    for (const Layer* layer : layers)
        frames.push_back(Flat(layer->width, layer->height, layer->red,
                              layer->green, layer->blue));
    std::vector<float> pixels;
    const bool rendered = renderer.RenderFramesToPixels(
        frames, DataOf(item, document, layers, color), item.targetWidth,
        item.targetHeight, pixels);
    for (AVFrame*& frame : frames) av_frame_free(&frame);
    Check(rendered, item.label + ": Record renders");
    return pixels;
}

void Compare(Renderer& renderer, const Case& item,
             const ColorManagementSettings& color, const std::string& space) {
    const Document document = DocumentOf(item);
    const Timeline timeline(document);
    const monitor_occlusion::PictureLookup pictures =
        [&](const Ulid& source,
            int64_t) -> std::optional<monitor_occlusion::Picture> {
        const Layer& layer = source == kLower ? item.lower : item.upper;
        return monitor_occlusion::Picture{layer.width, layer.height, false};
    };
    const bool ruled = monitor_occlusion::HiddenTracks(
                           document, timeline, {12, 25}, pictures, false)
                           .count(kV1) != 0;
    Check(ruled == item.hidden, item.label + ": the rule " +
                                    (item.hidden ? "hides" : "shows") +
                                    " the lower layer");
    const std::vector<float> both =
        Render(renderer, item, document, {&item.lower, &item.upper}, color);
    const std::vector<float> upperAlone =
        Render(renderer, item, document, {&item.upper}, color);
    const bool same = !both.empty() && both == upperAlone;
    Check(same == item.hidden,
          item.label + " (" + space + "): " +
              (item.hidden ? "without the hidden layer Record is the same, "
                             "value for value"
                           : "the layer underneath shows, and the "
                             "comparison sees it"));
}

}  // namespace

int main() {
    @autoreleasepool {
        if (MTLCreateSystemDefaultDevice() == nil) {
            std::cout << "SKIP: no Metal device\n";
            return 77;
        }
        Renderer renderer;
        Check(renderer.Initialize(nil), "the renderer starts without a view");
        if (failures != 0) return 1;

        const Layer red{64, 36, 200, 40, 30};
        const Layer blue{128, 72, 30, 60, 210};
        std::vector<Case> cases;
        cases.push_back({"a cutaway of another size, frame-filling viewer", 320,
                         180, 320, 180, red, blue, true});
        cases.push_back({"the same in a letterboxed viewer", 320, 180, 320, 240,
                         red, blue, true});
        // Fit scales that do not land on a whole number: 320/48.
        cases.push_back({"sizes whose fit is inexact", 320, 180, 320, 180,
                         Layer{48, 27, 200, 40, 30},
                         Layer{160, 90, 30, 60, 210}, true});
        // A vertical frame pillarboxed in a landscape viewer: its edges fall
        // between pixels.
        cases.push_back({"a vertical frame, pillarboxed", 180, 320, 320, 180,
                         Layer{36, 64, 200, 40, 30},
                         Layer{72, 128, 30, 60, 210}, true});
        Layer filled{96, 72, 30, 60, 210};
        filled.fill = true;
        cases.push_back({"a 4:3 cutaway filled into the frame", 320, 180, 320,
                         240, red, filled, true});
        Layer translucent = blue;
        translucent.opacity = {1, 2};
        cases.push_back({"control: a translucent cutaway", 320, 180, 320, 180,
                         red, translucent, false});
        cases.push_back({"control: a 4:3 cutaway fitted, with bars", 320, 180,
                         320, 180, red, Layer{96, 72, 30, 60, 210}, false});

        ColorManagementSettings managed;
        managed.enabled = true;
        managed.input_gamut = "sony_sgamut3_cine";
        managed.input_transfer = "sony_slog3";
        for (const Case& item : cases) {
            Compare(renderer, item, ColorManagementSettings{}, "unmanaged");
            Compare(renderer, item, managed, "S-Log3 to Rec.709");
        }
    }
    if (failures == 0)
        std::cout << "monitor_occlusion_render: all checks passed\n";
    return failures == 0 ? 0 : 1;
}
