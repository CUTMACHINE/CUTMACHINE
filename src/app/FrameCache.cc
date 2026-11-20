#include "FrameCache.h"

#include "ReadAhead.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace {

AVFrame* RetainFrame(const AVFrame* source) {
    AVFrame* retained = av_frame_alloc();
    if (!retained || av_frame_ref(retained, source) < 0) {
        av_frame_free(&retained);
        return nullptr;
    }
    return retained;
}

// PERF10 -- docs/roadmap/PERF-2026-09.md. A cached picture owns its bytes.
// A reference would pin a buffer of the decoder's pool, and FFmpeg's pools
// never free a buffer while the decoder is open: evicting the picture only
// handed the buffer back to that pool for reuse. Every decoder so kept as
// many pictures as it had ever had in here at once -- up to 1.4 GB for a 4K
// rush holding the budget to itself -- and the budget bounded nothing:
// playing a 4K reel reached 8.6 GB with six decoders open (25 September
// 2026). A copy of a 4K 10-bit 4:2:2 picture costs 0.7 ms; decoding it, 5.5.
// A hardware frame, whose pixels are not in these planes, is referenced.
AVFrame* OwnFrame(const AVFrame* source) {
    if (source->hw_frames_ctx) return RetainFrame(source);
    AVFrame* owned = av_frame_alloc();
    if (!owned) return nullptr;
    owned->format = source->format;
    owned->width = source->width;
    owned->height = source->height;
    if (av_frame_get_buffer(owned, 0) < 0 || av_frame_copy(owned, source) < 0 ||
        av_frame_copy_props(owned, source) < 0) {
        av_frame_free(&owned);
        return nullptr;
    }
    return owned;
}

}  // namespace

FrameCache::FrameCache(size_t byteBudget) : byteBudget_(byteBudget) {}

FrameCache::~FrameCache() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& item : entries_) {
        av_frame_free(&item.second.frame);
    }
}

size_t FrameCache::FrameBytes(const AVFrame* frame) {
    size_t bytes = 0;
    for (const AVBufferRef* buffer : frame->buf) {
        if (buffer) {
            bytes += buffer->size;
        }
    }
    for (int index = 0; index < frame->nb_extended_buf; ++index) {
        if (frame->extended_buf[index]) {
            bytes += frame->extended_buf[index]->size;
        }
    }
    return bytes;
}

void FrameCache::Touch(EntryMap::iterator entry) {
    lru_.splice(lru_.begin(), lru_, entry->second.lruPosition);
    entry->second.lruPosition = lru_.begin();
}

void FrameCache::EvictToBudget() { EvictDownTo(byteBudget_); }

void FrameCache::EvictDownTo(size_t bytes) {
    while (totalBytes_ > bytes && !lru_.empty()) {
        const Key key = lru_.back();
        auto entry = entries_.find(key);
        if (entry != entries_.end()) {
            totalBytes_ -= entry->second.bytes;
            av_frame_free(&entry->second.frame);
            entries_.erase(entry);
        }
        lru_.pop_back();
    }
}

bool FrameCache::Put(const SourceId& sourceId, int64_t frameIndex,
                     const AVFrame* frame) {
    if (!frame) {
        return false;
    }
    {
        // Checked before copying: a worker filling around a seek puts
        // pictures the cache already holds.
        std::lock_guard<std::mutex> lock(mutex_);
        auto existing = entries_.find(Key{sourceId, frameIndex});
        if (existing != entries_.end()) {
            Touch(existing);
            return true;
        }
    }
    AVFrame* retained = OwnFrame(frame);
    if (!retained) {
        return false;
    }
    const size_t bytes = FrameBytes(retained);

    std::lock_guard<std::mutex> lock(mutex_);
    auto& knownFrameBytes = sourceFrameBytes_[sourceId];
    knownFrameBytes = std::max(knownFrameBytes, bytes);
    const Key key = {sourceId, frameIndex};
    auto existing = entries_.find(key);
    if (existing != entries_.end()) {
        Touch(existing);
        av_frame_free(&retained);
        return true;
    }

    lru_.push_front(key);
    entries_.emplace(key, Entry{retained, bytes, lru_.begin()});
    totalBytes_ += bytes;
    EvictToBudget();
    return entries_.find(key) != entries_.end();
}

AVFrame* FrameCache::GetExact(const SourceId& sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return nullptr;
    }
    Touch(entry);
    return RetainFrame(entry->second.frame);
}

FrameCache::EntryMap::const_iterator FrameCache::Nearest(
    const SourceId& sourceId, int64_t frameIndex) const {
    auto next = entries_.lower_bound(Key{sourceId, frameIndex});
    auto best = entries_.end();

    if (next != entries_.end() && next->first.sourceId == sourceId) {
        best = next;
    }
    if (next != entries_.begin()) {
        auto previous = std::prev(next);
        if (previous->first.sourceId == sourceId) {
            if (best == entries_.end() ||
                std::llabs(previous->first.frameIndex - frameIndex) <=
                    std::llabs(best->first.frameIndex - frameIndex)) {
                best = previous;
            }
        }
    }
    return best;
}

AVFrame* FrameCache::GetNearest(const SourceId& sourceId, int64_t frameIndex,
                                int64_t& outFrameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto nearest = Nearest(sourceId, frameIndex);
    if (nearest == entries_.end()) {
        return nullptr;
    }
    // A map's erase of an empty range turns the const iterator back into a
    // mutable one, at no cost.
    const auto best = entries_.erase(nearest, nearest);
    outFrameIndex = best->first.frameIndex;
    Touch(best);
    return RetainFrame(best->second.frame);
}

bool FrameCache::DescribeNearest(const SourceId& sourceId, int64_t frameIndex,
                                 int& width, int& height, bool& alpha) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto nearest = Nearest(sourceId, frameIndex);
    if (nearest == entries_.end()) {
        return false;
    }
    const AVFrame* frame = nearest->second.frame;
    width = frame->width;
    height = frame->height;
    const AVPixFmtDescriptor* pixel =
        av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
    alpha = pixel != nullptr && pixel->nb_components >= 4 &&
            (pixel->flags & AV_PIX_FMT_FLAG_ALPHA) != 0;
    return true;
}

bool FrameCache::Contains(const SourceId& sourceId, int64_t frameIndex) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(Key{sourceId, frameIndex}) != entries_.end();
}

bool FrameCache::TouchFrame(const SourceId& sourceId, int64_t frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto entry = entries_.find(Key{sourceId, frameIndex});
    if (entry == entries_.end()) {
        return false;
    }
    Touch(entry);
    return true;
}

void FrameCache::TouchRun(const SourceId& sourceId, int64_t frameIndex,
                          int direction, size_t maxFrames) {
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t step = direction < 0 ? -1 : 1;
    // Touched farthest first, so that the frame asked for ends up the most
    // recent and goes last.
    std::vector<EntryMap::iterator> run;
    for (size_t count = 0; count < maxFrames; ++count) {
        auto entry = entries_.find(
            Key{sourceId, frameIndex + step * static_cast<int64_t>(count)});
        if (entry == entries_.end()) break;
        run.push_back(entry);
    }
    for (auto entry = run.rbegin(); entry != run.rend(); ++entry) Touch(*entry);
}

void FrameCache::RegisterSource(const SourceId& sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.insert(sourceId);
}

void FrameCache::UnregisterSource(const SourceId& sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    activeSources_.erase(sourceId);
}

void FrameCache::TrimTo(size_t bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    EvictDownTo(bytes);
}

void FrameCache::ClearSource(const SourceId& sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto entry = entries_.begin(); entry != entries_.end();) {
        if (entry->first.sourceId != sourceId) {
            ++entry;
            continue;
        }
        totalBytes_ -= entry->second.bytes;
        lru_.erase(entry->second.lruPosition);
        av_frame_free(&entry->second.frame);
        entry = entries_.erase(entry);
    }
    sourceFrameBytes_.erase(sourceId);
}

void FrameCache::SetReadSources(std::set<SourceId> sources) {
    std::lock_guard<std::mutex> lock(mutex_);
    readSources_ = std::move(sources);
}

FrameCache::PrefetchWindow FrameCache::WindowForSource(
    const SourceId& sourceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto bytes = sourceFrameBytes_.find(sourceId);
    if (bytes == sourceFrameBytes_.end() || bytes->second == 0) {
        return {};
    }
    const size_t sharing =
        readSources_.empty()
            ? activeSources_.size()
            : readSources_.size() + (readSources_.count(sourceId) ? 0 : 1);
    const read_ahead::Window window =
        read_ahead::DecodedWindow(byteBudget_, bytes->second, sharing);
    return PrefetchWindow{window.ahead, window.behind, window.total};
}

size_t FrameCache::ByteBudget() const { return byteBudget_; }

size_t FrameCache::TotalBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return totalBytes_;
}

size_t FrameCache::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}
