#pragma once

#include "Ulid.h"

#include <cstddef>
#include <cstdint>
#include <list>
#include <map>
#include <mutex>
#include <set>

struct AVFrame;

class FrameCache {
public:
    using SourceId = Ulid;

    struct PrefetchWindow {
        size_t ahead = 0;
        size_t behind = 0;
        size_t totalIncludingCurrent = 1;
    };

    explicit FrameCache(size_t byteBudget);
    ~FrameCache();

    FrameCache(const FrameCache&) = delete;
    FrameCache& operator=(const FrameCache&) = delete;

    bool Put(const SourceId& sourceId, int64_t frameIndex,
             const AVFrame* frame);
    AVFrame* GetExact(const SourceId& sourceId, int64_t frameIndex);
    AVFrame* GetNearest(const SourceId& sourceId, int64_t frameIndex,
                        int64_t& outFrameIndex);
    // PERF13 -- the picture GetNearest would hand back, described rather
    // than copied, and without touching the LRU order: its size as stored,
    // and whether it carries the alpha plane Renderer.mm multiplies by (a
    // pixel format of four components flagged alpha). False when no picture
    // of the source is held, which is when the monitor has nothing to draw
    // for it.
    bool DescribeNearest(const SourceId& sourceId, int64_t frameIndex,
                         int& width, int& height, bool& alpha) const;
    bool Contains(const SourceId& sourceId, int64_t frameIndex) const;
    bool TouchFrame(const SourceId& sourceId, int64_t frameIndex);
    // PERF14 -- the pictures of `sourceId` held from `frameIndex` on, in
    // `direction` (positive forward), up to the first gap or `maxFrames`,
    // marked as just used. A shot asked for ahead of its cut, or of its
    // return from under a cutaway, has its first pictures decoded seconds
    // before they are shown, and nothing touched them meanwhile: while the
    // cutaway's pictures streamed through the budget, least recently used
    // went first, and they were evicted before pictures already shown --
    // measured, 3 late ticks in 2,401 from the internal SSD became 34 once
    // the return was asked two seconds ahead instead of one. A request
    // repeated for the same frame refreshes them (DecodeWorker).
    void TouchRun(const SourceId& sourceId, int64_t frameIndex, int direction,
                  size_t maxFrames);
    void RegisterSource(const SourceId& sourceId);
    void UnregisterSource(const SourceId& sourceId);
    void ClearSource(const SourceId& sourceId);
    // Evicts the least recently used pictures until at most `bytes` remain:
    // what the editor keeps once playback stops (PERF10).
    void TrimTo(size_t bytes);
    // PERF14 -- the sources playback reads now: on screen, and the next
    // shot or the end of a cutaway it asks for ahead (main.mm). The window
    // below is shared among them rather than among every decoder open; a
    // source outside them shares it as one more. Empty, as at rest: shared
    // among the decoders open, as PERF10 left it.
    void SetReadSources(std::set<SourceId> sources);
    // How many pictures a decoder keeps around the one asked for: the
    // budget shared as read_ahead::DecodedWindow says (ReadAhead.h).
    PrefetchWindow WindowForSource(const SourceId& sourceId) const;

    size_t ByteBudget() const;
    size_t TotalBytes() const;
    size_t EntryCount() const;

private:
    struct Key {
        SourceId sourceId;
        int64_t frameIndex;

        bool operator<(const Key& other) const {
            return sourceId < other.sourceId || (sourceId == other.sourceId &&
                                                 frameIndex < other.frameIndex);
        }
    };

    struct Entry {
        AVFrame* frame = nullptr;
        size_t bytes = 0;
        std::list<Key>::iterator lruPosition;
    };

    using EntryMap = std::map<Key, Entry>;

    static size_t FrameBytes(const AVFrame* frame);
    // The picture of `sourceId` nearest `frameIndex`, the earlier one on a
    // tie; end() when none is held. The caller holds the mutex.
    EntryMap::const_iterator Nearest(const SourceId& sourceId,
                                     int64_t frameIndex) const;
    void Touch(EntryMap::iterator entry);
    void EvictToBudget();
    void EvictDownTo(size_t bytes);

    const size_t byteBudget_;
    size_t totalBytes_ = 0;
    mutable std::mutex mutex_;
    EntryMap entries_;
    std::list<Key> lru_;
    std::set<SourceId> activeSources_;
    std::set<SourceId> readSources_;
    std::map<SourceId, size_t> sourceFrameBytes_;
};
