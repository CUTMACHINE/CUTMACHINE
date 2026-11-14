#pragma once

// MOTION-2026-09 M4 -- ROADMAP.md. What the monitor shows of a motion clip:
// the policy, portable and tested; main.mm only wires it to the renderer
// and to Renderer.mm.
//
// A motion frame enters the monitor the way a decoded video frame does
// (ROADMAP.md, MOTION-2026-09: "comme une image vidéo, pas par la display
// list"): as a planar picture with its own alpha, which Renderer.mm already
// uploads plane by plane and composites by that alpha. Which frame at which
// tick is Timeline's answer (MotionClip.h); this decides what the picture is
// called, where it is kept and how it is laid out.

#include "Document.h"
#include "Timeline.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <string>
#include <utility>
#include <vector>

// One frame of a motion clip as the monitor uploads it: FFmpeg's GBRAP
// layout, planes green, blue, red and alpha one after the other, 8 bits,
// straight alpha, display-referred sRGB.
struct MotionPreviewImage {
    int32_t width = 0;
    int32_t height = 0;
    std::vector<uint8_t> planes;

    size_t PlaneSize() const {
        return static_cast<size_t>(width) * static_cast<size_t>(height);
    }
};

// Premultiplied RGBA rows (MotionRender.h's raster) to that layout. A
// transparent pixel is black and transparent; a partly covered one gets its
// colour back by dividing by its alpha, rounded to nearest.
void MotionPreviewFromPremultiplied(const uint8_t* rgba, int32_t width,
                                    int32_t height, MotionPreviewImage& image);

// What a monitor slot needs from the motion renderer: the picture's name in
// the cache -- the clip's composition, props and size, MotionClipPictureKey
// -- and the composition frame. False when the slot does not show a motion
// clip.
struct MotionPreviewRequest {
    std::string key;
    const DocumentComposition* composition = nullptr;
    mcp_json::Value props;
    int64_t frame = 0;
};

bool MotionPreviewFor(const Document& document, const ResolvedFrame& frame,
                      MotionPreviewRequest& request);

// Rendered pictures, least recently used first out once they hold more
// than `capacity` bytes. The picture being inserted always stays: a cache
// smaller than one frame still shows the current one.
class MotionPreviewCache {
public:
    explicit MotionPreviewCache(size_t capacity) : capacity_(capacity) {}

    // Null when absent; a hit becomes the most recently used.
    const MotionPreviewImage* Find(const std::string& key, int64_t frame);
    const MotionPreviewImage& Insert(const std::string& key, int64_t frame,
                                     MotionPreviewImage image);
    size_t Bytes() const { return bytes_; }
    size_t Count() const { return entries_.size(); }

private:
    using Key = std::pair<std::string, int64_t>;
    struct Entry {
        Key key;
        MotionPreviewImage image;
    };
    size_t capacity_ = 0;
    size_t bytes_ = 0;
    std::list<Entry> entries_;  // most recently used first
    std::map<Key, std::list<Entry>::iterator> index_;
};

// About 30 frames at 1080x1920: a second of playback of one title, or the
// frames a scrub goes back and forth over.
inline constexpr size_t kMotionPreviewCacheBytes = size_t{256} << 20;
