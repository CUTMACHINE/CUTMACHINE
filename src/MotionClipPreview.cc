#include "MotionClipPreview.h"

#include "MotionClip.h"
#include "MotionDocument.h"

void MotionPreviewFromPremultiplied(const uint8_t* rgba, int32_t width,
                                    int32_t height, MotionPreviewImage& image) {
    image.width = width;
    image.height = height;
    const size_t plane = image.PlaneSize();
    image.planes.assign(plane * 4, 0);
    uint8_t* green = image.planes.data();
    uint8_t* blue = green + plane;
    uint8_t* red = blue + plane;
    uint8_t* alpha = red + plane;
    for (size_t index = 0; index < plane; ++index) {
        const uint8_t* pixel = rgba + (index * 4);
        const uint32_t coverage = pixel[3];
        alpha[index] = static_cast<uint8_t>(coverage);
        if (coverage == 0) {
            continue;
        }
        if (coverage == 255) {
            red[index] = pixel[0];
            green[index] = pixel[1];
            blue[index] = pixel[2];
            continue;
        }
        const auto straight = [coverage](uint32_t premultiplied) {
            const uint32_t value =
                ((premultiplied * 255U) + (coverage / 2U)) / coverage;
            return static_cast<uint8_t>(value > 255U ? 255U : value);
        };
        red[index] = straight(pixel[0]);
        green[index] = straight(pixel[1]);
        blue[index] = straight(pixel[2]);
    }
}

bool MotionPreviewFor(const Document& document, const ResolvedFrame& frame,
                      MotionPreviewRequest& request) {
    const DocumentClip* clip = document.FindClip(frame.clip_id);
    if (clip == nullptr || !IsMotionClip(*clip)) {
        return false;
    }
    const DocumentComposition* composition =
        document.FindComposition(clip->composition_id);
    if (composition == nullptr) {
        return false;
    }
    int64_t width = 0;
    int64_t height = 0;
    if (const mcp_json::Value* value = composition->definition.Find("width")) {
        value->AsInt64(width);
    }
    if (const mcp_json::Value* value = composition->definition.Find("height")) {
        value->AsInt64(height);
    }
    request.key = MotionClipPictureKey(
        *composition, clip->motion_props, static_cast<int32_t>(width),
        static_cast<int32_t>(height),
        MotionReachKey(document, composition->id));
    request.composition = composition;
    request.props = clip->motion_props;
    request.frame = frame.source_frame;
    return true;
}

const MotionPreviewImage* MotionPreviewCache::Find(const std::string& key,
                                                   int64_t frame) {
    const auto found = index_.find({key, frame});
    if (found == index_.end()) {
        return nullptr;
    }
    entries_.splice(entries_.begin(), entries_, found->second);
    return &found->second->image;
}

const MotionPreviewImage& MotionPreviewCache::Insert(const std::string& key,
                                                     int64_t frame,
                                                     MotionPreviewImage image) {
    const Key entryKey{key, frame};
    if (const auto found = index_.find(entryKey); found != index_.end()) {
        bytes_ -= found->second->image.planes.size();
        entries_.erase(found->second);
        index_.erase(found);
    }
    bytes_ += image.planes.size();
    entries_.push_front({entryKey, std::move(image)});
    index_[entryKey] = entries_.begin();
    while (bytes_ > capacity_ && entries_.size() > 1) {
        const Entry& oldest = entries_.back();
        bytes_ -= oldest.image.planes.size();
        index_.erase(oldest.key);
        entries_.pop_back();
    }
    return entries_.front().image;
}
