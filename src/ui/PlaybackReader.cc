#include "PlaybackReader.h"

#include "Document.h"
#include "PlaybackReads.h"
#include "Timeline.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <utility>

namespace playback_reader {

std::vector<PlannedRead> PlanReads(
    const Document& document, const Timeline& timeline, int64_t fromFrame,
    int64_t frames, const monitor_occlusion::PictureLookup& pictures,
    bool zoomed) {
    const MediaRate rate = document.sequence.frame_rate;
    if (rate.num <= 0 || rate.den <= 0 || frames <= 0) return {};
    std::vector<const DocumentTrack*> tracks;
    for (const DocumentTrack& track : document.sequence.tracks)
        if (track.kind == "video" && track.visible) tracks.push_back(&track);
    std::stable_sort(tracks.begin(), tracks.end(),
                     [](const DocumentTrack* a, const DocumentTrack* b) {
                         return a->index < b->index;
                     });
    struct Running {
        PlannedRead read;
        size_t track = 0;
        int64_t seen = 0;
    };
    // One run in progress per layer: a track and the clip it shows.
    std::map<std::string, Running> running;
    std::vector<std::pair<size_t, PlannedRead>> done;
    const auto close = [&](std::map<std::string, Running>::iterator it) {
        done.emplace_back(it->second.track, it->second.read);
        return running.erase(it);
    };
    for (int64_t frame = fromFrame; frame < fromFrame + frames; ++frame) {
        const RationalTime at{frame * rate.den, rate.num};
        if (at >= timeline.Duration()) break;
        const std::set<Ulid> hidden = monitor_occlusion::HiddenTracks(
            document, timeline, at, pictures, zoomed);
        for (size_t order = 0; order < tracks.size(); ++order) {
            const DocumentTrack& track = *tracks[order];
            if (hidden.count(track.id) != 0) continue;
            for (const ResolvedLayer& layer :
                 timeline.ResolveTrackLayers(track.id, at)) {
                const DocumentClip* clip =
                    document.FindClip(layer.frame.clip_id);
                if (clip == nullptr || IsMotionClip(*clip)) continue;
                const std::string key = track.id + "/" + clip->id;
                const int64_t sourceFrame = layer.frame.source_frame;
                auto it = running.find(key);
                if (it != running.end()) {
                    PlannedRead& read = it->second.read;
                    if (it->second.seen == frame - 1 &&
                        read.source == layer.frame.source_id &&
                        sourceFrame >= read.last &&
                        sourceFrame <= read.last + 4 &&
                        frame - read.at < kMaxPlannedRunFrames) {
                        read.last = sourceFrame;
                        it->second.seen = frame;
                        continue;
                    }
                    close(it);
                }
                running[key] = {
                    {layer.frame.source_id, sourceFrame, sourceFrame, frame},
                    order,
                    frame};
            }
        }
        for (auto it = running.begin(); it != running.end();) {
            if (it->second.seen < frame)
                it = close(it);
            else
                ++it;
        }
    }
    for (auto it = running.begin(); it != running.end();) it = close(it);
    std::stable_sort(
        done.begin(), done.end(), [](const auto& a, const auto& b) {
            return a.second.at != b.second.at ? a.second.at < b.second.at
                                              : a.first < b.first;
        });
    std::vector<PlannedRead> plan;
    plan.reserve(done.size());
    for (auto& [order, read] : done) plan.push_back(read);
    return plan;
}

std::vector<ByteRun> RunsFor(
    const std::vector<read_ahead::SampleRange>& samples, int64_t first,
    int64_t last, int64_t maxRun) {
    std::vector<ByteRun> runs;
    if (samples.empty()) return runs;
    const int64_t count = static_cast<int64_t>(samples.size());
    first = std::max<int64_t>(0, first);
    last = std::min<int64_t>(count - 1, last);
    for (int64_t frame = first; frame <= last; ++frame) {
        const read_ahead::SampleRange& sample =
            samples[static_cast<size_t>(frame)];
        if (!runs.empty() &&
            runs.back().offset + runs.back().size == sample.offset &&
            runs.back().size + sample.size <= maxRun) {
            runs.back().size += sample.size;
            continue;
        }
        runs.push_back({sample.offset, sample.size});
    }
    return runs;
}

struct Reader::Read {
    int descriptor = -1;
    ByteRun run;
};

Reader::Reader() = default;

Reader::~Reader() {
    Stop();
    for (auto& [source, file] : sources_)
        if (file.descriptor >= 0) close(file.descriptor);
}

void Reader::SetSource(
    const Ulid& source, std::string path,
    std::shared_ptr<const std::vector<read_ahead::SampleRange>> samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    SourceFile& file = sources_[source];
    if (file.path != path && file.descriptor >= 0) {
        close(file.descriptor);
        file.descriptor = -1;
    }
    file.path = std::move(path);
    file.samples = std::move(samples);
    if (file.samples && !file.samples->empty()) unplaced_.erase(source);
    plannedFrom_ = -1;
}

void Reader::Start(std::unique_ptr<Document> document, bool zoomed,
                   RationalTime position, int64_t horizonSeconds,
                   bool threaded) {
    Stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!document) return;
    document_ = std::move(document);
    timeline_ = std::make_unique<Timeline>(*document_);
    zoomed_ = zoomed;
    const MediaRate rate = document_->sequence.frame_rate;
    horizonFrames_ =
        rate.den > 0 ? horizonSeconds * rate.num / rate.den : int64_t{0};
    positionFrame_ = rate.num > 0 ? position.to_frames(rate.num, rate.den) : 0;
    plannedFrom_ = -1;
    plan_.clear();
    read_.clear();
    buffer_.resize(static_cast<size_t>(kReaderMaxRunBytes));
    running_ = true;
    stopping_ = false;
    if (threaded) thread_ = std::thread(&Reader::Run, this);
}

void Reader::SetPosition(RationalTime position) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!document_) return;
        const MediaRate rate = document_->sequence.frame_rate;
        if (rate.num <= 0) return;
        positionFrame_ = position.to_frames(rate.num, rate.den);
    }
    wake_.notify_all();
}

void Reader::Stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        running_ = false;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    timeline_.reset();
    document_.reset();
    plan_.clear();
    read_.clear();
}

bool Reader::Running() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

bool Reader::Covers(const Ulid& source) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto file = sources_.find(source);
    return file != sources_.end() && !file->second.path.empty() &&
           file->second.samples && !file->second.samples->empty();
}

Reader::Stats Reader::GetStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats stats = stats_;
    stats.unplaced_sources = static_cast<int64_t>(unplaced_.size());
    return stats;
}

int Reader::DescriptorFor(const Ulid& source) {
    SourceFile& file = sources_[source];
    if (file.descriptor < 0 && !file.path.empty()) {
        file.descriptor = open(file.path.c_str(), O_RDONLY | O_CLOEXEC);
#if defined(F_RDAHEAD)
        // What is read is what the plan names, no more.
        if (file.descriptor >= 0) fcntl(file.descriptor, F_RDAHEAD, 0);
#endif
    }
    return file.descriptor;
}

bool Reader::NextRead(Read& read) {
    if (!timeline_ || !document_) return false;
    // Planned again from each new frame on screen: a plan starts at the
    // playhead, so what it names is still to be shown, and the frames passed
    // while a read was under way drop out of it.
    if (plannedFrom_ != positionFrame_) {
        plan_ =
            PlanReads(*document_, *timeline_, positionFrame_, horizonFrames_,
                      playback_reads::PicturesFromLibrary(*document_), zoomed_);
        plannedFrom_ = positionFrame_;
        ++stats_.plans;
        // What was read of a source and is behind the plan is forgotten.
        std::map<Ulid, int64_t> lowest;
        for (const PlannedRead& planned : plan_) {
            auto [it, inserted] = lowest.emplace(planned.source, planned.first);
            if (!inserted) it->second = std::min(it->second, planned.first);
            const auto file = sources_.find(planned.source);
            if (file == sources_.end() || !file->second.samples ||
                file->second.samples->empty())
                unplaced_.insert(planned.source);
        }
        for (auto it = read_.begin(); it != read_.end();) {
            const auto low = lowest.find(it->first);
            if (low == lowest.end()) {
                it = read_.erase(it);
                continue;
            }
            it->second.erase(it->second.begin(),
                             it->second.lower_bound(low->second));
            ++it;
        }
    }
    for (const PlannedRead& planned : plan_) {
        const auto file = sources_.find(planned.source);
        if (file == sources_.end() || !file->second.samples ||
            file->second.samples->empty())
            continue;
        const auto& samples = *file->second.samples;
        const int64_t count = static_cast<int64_t>(samples.size());
        std::set<int64_t>& done = read_[planned.source];
        for (int64_t frame = std::max<int64_t>(0, planned.first);
             frame <= planned.last && frame < count; ++frame) {
            if (done.count(frame) != 0) continue;
            // This frame and the ones after it the plan still needs, as
            // long as their samples follow on in the file.
            const read_ahead::SampleRange& firstSample =
                samples[static_cast<size_t>(frame)];
            ByteRun run{firstSample.offset, firstSample.size};
            done.insert(frame);
            for (int64_t next = frame + 1; next <= planned.last && next < count;
                 ++next) {
                const read_ahead::SampleRange& sample =
                    samples[static_cast<size_t>(next)];
                if (done.count(next) != 0 ||
                    run.offset + run.size != sample.offset ||
                    run.size + sample.size > kReaderMaxRunBytes)
                    break;
                run.size += sample.size;
                done.insert(next);
            }
            read.descriptor = DescriptorFor(planned.source);
            read.run = run;
            return read.descriptor >= 0;
        }
    }
    return false;
}

int64_t Reader::Step() {
    Read read;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_ || !NextRead(read)) return 0;
    }
    if (buffer_.size() < static_cast<size_t>(read.run.size))
        buffer_.resize(static_cast<size_t>(read.run.size));
    int64_t got = 0;
    while (got < read.run.size) {
        const ssize_t n = pread(read.descriptor, buffer_.data() + got,
                                static_cast<size_t>(read.run.size - got),
                                static_cast<off_t>(read.run.offset + got));
        if (n <= 0) break;
        got += n;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    stats_.bytes += got;
    ++stats_.reads;
    return got > 0 ? got : 1;
}

void Reader::Run() {
    while (true) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
        }
        if (Step() > 0) continue;
        std::unique_lock<std::mutex> lock(mutex_);
        wake_.wait_for(lock, std::chrono::milliseconds(20),
                       [&] { return stopping_; });
    }
}

}  // namespace playback_reader
