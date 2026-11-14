#include "AudioPlayback.h"

#include "AudioDecode.h"
#include "AudioMixFilters.h"
#include "ClipEnabled.h"
#include "MotionAudio.h"
#include "MotionClip.h"
#include "MotionDocument.h"
#include "PlaybackPresentation.h"

#import <AVFAudio/AVFAudio.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr int32_t kMixRate = 48000;
constexpr int32_t kScrubSamples = 2880;     // 60 ms
constexpr int32_t kScrubFadeSamples = 240;  // 5 ms

// A decoded rush, whole, held for the mix as 16-bit samples (AudioDecode.h).
// The mix scales back at the realtime boundary (kSampleScale).
using PCMSource = audio_decode::StereoPcm16;

constexpr float kSampleScale = 1.0f / 32768.0f;

struct MixClip {
    std::shared_ptr<const PCMSource> source;
    int64_t timelineStart = 0;
    int64_t sourceStart = 0;
    int64_t length = 0;
    // The conversion to an amplitude is deliberately deferred to this
    // realtime boundary. The document retains exact dB and exact durations;
    // libm merely turns them into samples for AVAudioEngine.
    float gain = 1.0f;
    int64_t fadeInSamples = 0;
    int64_t fadeOutSamples = 0;
    // EF2 -- a cross fade's side ramps on the equal power curve.
    bool equalPowerIn = false;
    bool equalPowerOut = false;
};

struct MixPlan {
    std::vector<MixClip> clips;
};

// PERF-2026-09. A MixClip minus the one thing that has to wait: the decoded
// samples. RebuildTimeline resolves everything a clip contributes to the mix
// from the Document alone and keeps it here, so the plan the realtime thread
// reads can be rebuilt from the sources decoded so far -- as many times as
// decoding finishes a new one -- without the Document being consulted again,
// let alone retained.
struct PlannedClip {
    Ulid source_id;
    int64_t timelineStart = 0;
    int64_t sourceStart = 0;
    int64_t length = 0;
    float gain = 1.0f;
    int64_t fadeInSamples = 0;
    int64_t fadeOutSamples = 0;
    bool equalPowerIn = false;
    bool equalPowerOut = false;
};

size_t PcmBytes(const PCMSource& source) {
    return (source.left.capacity() + source.right.capacity()) * sizeof(int16_t);
}

// Retention on top of what is audible, not a cap on it: a source the active
// timeline plays is never dropped, exactly as before. This budget only says
// how much *already decoded* audio is worth keeping around once it stops
// being audible, so that switching back to the timeline it belongs to is
// silent-free and costs no disk. Roughly 90 minutes at 48 kHz stereo 16-bit.
constexpr size_t kRetainedAudioBudgetBytes = 1024u * 1024u * 1024u;

}  // namespace

struct AudioPlayback::Impl {
    AVAudioEngine* engine = nil;
    AVAudioSourceNode* sourceNode = nil;
    std::filesystem::path baseDirectory;
    std::shared_ptr<const MixPlan> plan = std::make_shared<MixPlan>();
    std::atomic<int64_t> cursor{0};
    std::atomic<int> direction{0};
    std::atomic<int32_t> scrubRemaining{0};
    std::atomic<uint64_t> generation{0};
    std::atomic<int64_t> lastScrubSample{std::numeric_limits<int64_t>::min()};
    std::atomic<uint64_t> scrubTriggerCount{0};
    // PRD37 -- ROADMAP.md. The editor thread's alone, as the engine is: when
    // the output last had something to play, and how many times it was
    // opened (PlaybackPresentation.h, kAudioOutputIdleGrace).
    std::chrono::steady_clock::time_point lastSound;
    uint64_t outputStarts = 0;

    // PERF-2026-09. Everything below belongs to the decode thread and the
    // editor thread jointly; the realtime render block touches none of it and
    // reads `plan` alone, so it never waits on this mutex.
    mutable std::mutex mutex;
    std::condition_variable wakeup;
    std::condition_variable idle;
    std::thread decoder;
    // Read by the decode thread inside DecodeSource, so it cannot live under
    // `mutex`: the point is to reach a decode that is already running.
    std::atomic_bool cancel{false};
    bool stopping = false;
    bool decoding = false;
    std::map<Ulid, std::shared_ptr<const PCMSource>> sources;
    std::vector<PlannedClip> planned;
    std::set<Ulid> audible;
    // A source that cannot be decoded is remembered as such: retrying it on
    // every rebuild would re-open an offline rush over and over.
    std::set<Ulid> failed;
    std::deque<std::pair<Ulid, std::filesystem::path>> queue;
    // REMOTION-PARITY-2026-09 REM2 -- a motion clip's sound is not decoded
    // from a file: the engine mixes it (MotionAudio.h). Its queue entry
    // names a job here instead of a path.
    std::map<Ulid, std::function<bool(PCMSource&, std::string&)>> jobs;
    std::map<Ulid, uint64_t> lastUsed;
    uint64_t useTick = 0;

    // Both callers already hold `mutex`.
    void PublishPlanLocked() {
        auto next = std::make_shared<MixPlan>();
        next->clips.reserve(planned.size());
        for (const PlannedClip& clip : planned) {
            const auto source = sources.find(clip.source_id);
            if (source == sources.end()) continue;
            MixClip mixed;
            mixed.source = source->second;
            mixed.timelineStart = clip.timelineStart;
            mixed.sourceStart = clip.sourceStart;
            mixed.length = clip.length;
            mixed.gain = clip.gain;
            mixed.fadeInSamples = clip.fadeInSamples;
            mixed.fadeOutSamples = clip.fadeOutSamples;
            mixed.equalPowerIn = clip.equalPowerIn;
            mixed.equalPowerOut = clip.equalPowerOut;
            next->clips.push_back(std::move(mixed));
        }
        std::atomic_store(&plan, std::static_pointer_cast<const MixPlan>(next));
    }

    // Evicting a source the plan still names is harmless -- every MixClip
    // holds its own shared_ptr, so the realtime thread keeps reading valid
    // samples until the plan is replaced -- but it would be re-decoded on the
    // next rebuild, so only sources the active timeline does not play are
    // considered.
    void EvictLocked() {
        size_t total = 0;
        for (const auto& source : sources) total += PcmBytes(*source.second);
        while (total > kRetainedAudioBudgetBytes) {
            auto oldest = sources.end();
            uint64_t oldestTick = std::numeric_limits<uint64_t>::max();
            for (auto item = sources.begin(); item != sources.end(); ++item) {
                if (audible.count(item->first) != 0) continue;
                const auto used = lastUsed.find(item->first);
                const uint64_t tick = used == lastUsed.end() ? 0 : used->second;
                if (tick < oldestTick) {
                    oldestTick = tick;
                    oldest = item;
                }
            }
            if (oldest == sources.end()) break;
            total -= PcmBytes(*oldest->second);
            lastUsed.erase(oldest->first);
            sources.erase(oldest);
        }
    }
};

AudioPlayback::AudioPlayback() : impl_(new Impl()) {}

AudioPlayback::~AudioPlayback() {
    Stop();
    // PRD37 -- Stop() leaves the output open; this object closes it.
    if (impl_->engine && impl_->engine.isRunning) [impl_->engine stop];
    impl_->cancel.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->wakeup.notify_all();
    if (impl_->decoder.joinable()) impl_->decoder.join();
    if (impl_->engine && impl_->sourceNode)
        [impl_->engine detachNode:impl_->sourceNode];
    delete impl_;
}

bool AudioPlayback::Open(const Document& document,
                         const std::string& baseDirectory, std::string& error) {
    // PRD37 -- the output a previous Open left open goes with its engine.
    Stop();
    if (impl_->engine && impl_->engine.isRunning) [impl_->engine stop];
    // Open re-initialises. Anything the previous document left decoding has
    // to be abandoned before the cache is cleared, or its result would land
    // in the new one after the fact.
    impl_->cancel.store(true);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->queue.clear();
    }
    WaitForDecodes(2000);
    impl_->cancel.store(false);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->sources.clear();
        impl_->planned.clear();
        impl_->audible.clear();
        impl_->failed.clear();
        impl_->queue.clear();
        impl_->lastUsed.clear();
    }
    impl_->baseDirectory = baseDirectory;
    EnsureDecoderStarted();
    RebuildTimeline(document);

    impl_->engine = [[AVAudioEngine alloc] init];
    AVAudioFormat* format =
        [[AVAudioFormat alloc] initWithCommonFormat:AVAudioPCMFormatFloat32
                                         sampleRate:kMixRate
                                           channels:2
                                        interleaved:NO];
    Impl* state = impl_;
    impl_->sourceNode = [[AVAudioSourceNode alloc]
        initWithFormat:format
           renderBlock:^OSStatus(
               BOOL* isSilence, const AudioTimeStamp* timestamp,
               AVAudioFrameCount frameCount, AudioBufferList* outputData) {
             (void)timestamp;
             const auto plan = std::atomic_load(&state->plan);
             const uint64_t generation = state->generation.load();
             const int direction = state->direction.load();
             const int64_t cursor = state->cursor.load();
             const int32_t scrubRemaining = state->scrubRemaining.load();
             const bool scrubbing = scrubRemaining > 0;
             float* left = static_cast<float*>(outputData->mBuffers[0].mData);
             float* right = static_cast<float*>(outputData->mBuffers[1].mData);
             std::fill(left, left + frameCount, 0.0f);
             std::fill(right, right + frameCount, 0.0f);
             bool audible = false;
             if (direction != 0) {
                 for (AVAudioFrameCount index = 0; index < frameCount;
                      ++index) {
                     if (scrubbing && index >= static_cast<AVAudioFrameCount>(
                                                   scrubRemaining))
                         break;
                     const int64_t timelineSample =
                         cursor + static_cast<int64_t>(index) * direction;
                     for (const MixClip& clip : plan->clips) {
                         const int64_t offset =
                             timelineSample - clip.timelineStart;
                         if (offset < 0 || offset >= clip.length) continue;
                         const int64_t sourceSample = clip.sourceStart + offset;
                         if (sourceSample < 0 ||
                             sourceSample >=
                                 static_cast<int64_t>(clip.source->left.size()))
                             continue;
                         // EF2 -- the export's afade, by the one rule
                         // (AudioMixFilters.h).
                         const float envelope =
                             clip.gain *
                             audio_mix::ClipAudioEnvelopeAt(
                                 offset, clip.length, clip.fadeInSamples,
                                 clip.fadeOutSamples, clip.equalPowerIn,
                                 clip.equalPowerOut);
                         const float scaled = envelope * kSampleScale;
                         left[index] += static_cast<float>(
                                            clip.source->left[sourceSample]) *
                                        scaled;
                         right[index] += static_cast<float>(
                                             clip.source->right[sourceSample]) *
                                         scaled;
                         audible = true;
                     }
                     left[index] = std::clamp(left[index], -1.0f, 1.0f);
                     right[index] = std::clamp(right[index], -1.0f, 1.0f);
                     if (scrubbing) {
                         const int32_t elapsed =
                             kScrubSamples - scrubRemaining + index;
                         const int32_t remaining = scrubRemaining - index;
                         const float attack = std::clamp(
                             static_cast<float>(elapsed) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float release = std::clamp(
                             static_cast<float>(remaining) / kScrubFadeSamples,
                             0.0f, 1.0f);
                         const float gain = std::min(attack, release);
                         left[index] *= gain;
                         right[index] *= gain;
                     }
                 }
                 if (state->generation.load() == generation) {
                     const int32_t consumed =
                         scrubbing
                             ? std::min<int32_t>(scrubRemaining, frameCount)
                             : static_cast<int32_t>(frameCount);
                     state->cursor.store(
                         cursor + static_cast<int64_t>(consumed) * direction);
                     if (scrubbing) {
                         const int32_t next = scrubRemaining - consumed;
                         state->scrubRemaining.store(next);
                         if (next == 0) state->direction.store(0);
                     }
                 }
             }
             *isSilence = !audible;
             return noErr;
           }];
    [impl_->engine attachNode:impl_->sourceNode];
    [impl_->engine connect:impl_->sourceNode
                        to:impl_->engine.mainMixerNode
                    format:format];
    error.clear();
    return true;
}

// PERF-2026-09. This used to decode, on the calling thread, the whole audio
// of every source the timeline plays -- 48 kHz stereo float, so a 30-minute
// interview rush is about 690 MB and several seconds of work. It runs after
// every edit, every timeline switch and once per project open, all on the
// thread AppKit draws from, which is precisely where the editor was seen to
// freeze when a project or a timeline was opened.
//
// The plan the realtime thread reads has always skipped a clip whose source
// is not decoded, so a partially decoded mix was already a state this design
// tolerated -- it just never occurred, because decoding blocked until it
// could not. Making the decode a background job turns that tolerated state
// into the normal one: the mix comes up as sources land, a moment after the
// picture, instead of the whole editor waiting for all of them.
void AudioPlayback::RebuildTimeline(const Document& document) {
    std::set<Ulid> audible;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio") continue;
        for (const DocumentClip& clip : track.clips)
            audible.insert(clip.source_id);
    }

    const bool hasSolo = std::any_of(
        document.sequence.tracks.begin(), document.sequence.tracks.end(),
        [](const DocumentTrack& track) {
            return track.kind == "audio" && track.solo;
        });
    std::vector<PlannedClip> planned;
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "audio" || track.muted || (hasSolo && !track.solo))
            continue;
        for (const DocumentClip& clip : track.clips) {
            // A disabled clip plays nothing, as it exports nothing
            // (ClipEnabled.h).
            if (!ClipIsHeard(clip)) continue;
            // AGENT-2026-09 A46 -- the 3 ms edge fade the export applies,
            // so the monitor plays what the file will. EF2 -- and a cross
            // fade's handles and ramps, by the export's own rule.
            audio_mix::ClipAudioEdges edges;
            std::string edgeError;
            if (!audio_mix::ResolveClipAudioEdges(document, clip, track.id,
                                                  edges, edgeError))
                edges = audio_mix::ClipOwnAudioEdges(clip);
            PlannedClip mixed;
            mixed.source_id = clip.source_id;
            mixed.timelineStart =
                clip.timeline_in.sub(edges.lead).to_frames(kMixRate);
            mixed.sourceStart =
                clip.source_in.sub(edges.lead).to_frames(kMixRate);
            mixed.length = edges.length.to_frames(kMixRate);
            const float gainDb = static_cast<float>(clip.audio_gain_db.num) /
                                 static_cast<float>(clip.audio_gain_db.den);
            mixed.gain = std::pow(10.0f, gainDb / 20.0f);
            mixed.fadeInSamples = edges.fade_in.to_frames(kMixRate);
            mixed.fadeOutSamples = edges.fade_out.to_frames(kMixRate);
            mixed.equalPowerIn = edges.equal_power_in;
            mixed.equalPowerOut = edges.equal_power_out;
            if (mixed.length > 0) planned.push_back(std::move(mixed));
        }
    }

    // REM2 -- the sound of each motion clip whose composition plays one:
    // the engine's mix of the part the clip shows, keyed by what makes it
    // (composition, props, what it reaches, the window), rendered on the
    // decode thread like a rush. Hidden video tracks play nothing, as they
    // export nothing.
    std::map<Ulid, std::function<bool(PCMSource&, std::string&)>> jobs;
    const std::map<std::string, std::string> mediaPaths =
        MotionMediaPaths(document);
    for (const DocumentTrack& track : document.sequence.tracks) {
        if (track.kind != "video" || !track.visible) continue;
        for (const DocumentClip& clip : track.clips) {
            if (!IsMotionClip(clip) || !ClipIsHeard(clip)) continue;
            const DocumentComposition* stored =
                document.FindComposition(clip.composition_id);
            auto composition = std::make_shared<MotionComposition>();
            MotionReach reach;
            MotionError motionError;
            if (stored == nullptr ||
                !ParseDocumentComposition(*stored, *composition, motionError) ||
                !MotionReachFor(document, stored->id, reach, motionError))
                continue;
            MotionMediaMeasurer probe(impl_->baseDirectory.string(),
                                      mediaPaths);
            std::string voices;
            size_t count = 0;
            if (!DescribeMotionAudio(*composition, clip.motion_props,
                                     reach.Context(), probe, voices, count,
                                     motionError) ||
                count == 0)
                continue;
            const int64_t first = clip.source_in.to_frames(kMixRate);
            const int64_t length = clip.duration.to_frames(kMixRate);
            const Ulid key =
                "motion:" +
                MotionClipPictureKey(*stored, clip.motion_props,
                                     composition->width, composition->height,
                                     MotionReachKey(document, stored->id)) +
                ":" + std::to_string(first) + ":" + std::to_string(length);
            PlannedClip mixed;
            mixed.source_id = key;
            mixed.timelineStart = clip.timeline_in.to_frames(kMixRate);
            mixed.sourceStart = 0;
            mixed.length = length;
            if (audio_mix::WantsEdgeFade(clip)) {
                const int64_t edge =
                    audio_mix::kAudioEdgeFade.to_frames(kMixRate);
                mixed.fadeInSamples = edge;
                mixed.fadeOutSamples = edge;
            }
            if (mixed.length <= 0) continue;
            planned.push_back(mixed);
            audible.insert(key);
            const std::string directory = impl_->baseDirectory.string();
            const mcp_json::Value props = clip.motion_props;
            jobs[key] = [composition, reach, props, directory, mediaPaths,
                         first, length](PCMSource& output, std::string& error) {
                MotionMediaMeasurer measurer(directory, mediaPaths);
                MotionPcm mix;
                MotionError failure;
                if (!MixMotionAudio(*composition, props, reach.Context(),
                                    measurer, first, length, mix, failure)) {
                    error = failure.code + ": " + failure.message;
                    return false;
                }
                const auto quantise = [](float sample) {
                    return static_cast<int16_t>(std::lround(
                        std::clamp(sample, -1.0f, 32767.0f / 32768.0f) *
                        32768.0f));
                };
                output.left.resize(mix.left.size());
                output.right.resize(mix.right.size());
                std::transform(mix.left.begin(), mix.left.end(),
                               output.left.begin(), quantise);
                std::transform(mix.right.begin(), mix.right.end(),
                               output.right.begin(), quantise);
                return true;
            };
        }
    }

    // Resolved before taking the lock: a relative source path is answered
    // from the project directory, which the decode thread has no business
    // knowing about.
    std::map<Ulid, std::filesystem::path> paths;
    for (const DocumentSource& source : document.sources) {
        if (audible.count(source.id) == 0) continue;
        std::filesystem::path path(source.path);
        if (path.is_relative()) path = impl_->baseDirectory / path;
        paths.emplace(source.id, path.lexically_normal());
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->planned = std::move(planned);
        impl_->audible = audible;
        for (const Ulid& id : impl_->audible)
            impl_->lastUsed[id] = ++impl_->useTick;
        for (const auto& item : paths) {
            if (impl_->sources.count(item.first) != 0 ||
                impl_->failed.count(item.first) != 0)
                continue;
            const bool queued = std::any_of(
                impl_->queue.begin(), impl_->queue.end(),
                [&](const auto& entry) { return entry.first == item.first; });
            if (!queued) impl_->queue.push_back(item);
        }
        // REM2 -- a motion sound is queued by its key, its job beside it.
        for (auto& job : jobs) {
            if (impl_->sources.count(job.first) != 0 ||
                impl_->failed.count(job.first) != 0)
                continue;
            const bool queued = std::any_of(
                impl_->queue.begin(), impl_->queue.end(),
                [&](const auto& entry) { return entry.first == job.first; });
            impl_->jobs[job.first] = std::move(job.second);
            if (!queued) impl_->queue.push_back({job.first, {}});
        }
        impl_->EvictLocked();
        impl_->PublishPlanLocked();
    }
    EnsureDecoderStarted();
    impl_->wakeup.notify_one();
}

// Both entry points are called from the editor thread, which is the only
// thread allowed to touch `decoder` itself.
void AudioPlayback::EnsureDecoderStarted() {
    if (!impl_->decoder.joinable())
        impl_->decoder = std::thread(&AudioPlayback::DecodeLoop, this);
}

void AudioPlayback::DecodeLoop() {
    while (true) {
        Ulid sourceId;
        std::filesystem::path path;
        std::function<bool(PCMSource&, std::string&)> job;
        {
            std::unique_lock<std::mutex> lock(impl_->mutex);
            impl_->wakeup.wait(lock, [this] {
                return impl_->stopping || !impl_->queue.empty();
            });
            if (impl_->stopping) return;
            sourceId = impl_->queue.front().first;
            path = impl_->queue.front().second;
            impl_->queue.pop_front();
            const auto found = impl_->jobs.find(sourceId);
            if (found != impl_->jobs.end()) {
                job = std::move(found->second);
                impl_->jobs.erase(found);
                path = sourceId;
            }
            impl_->decoding = true;
        }

        auto decoded = std::make_shared<PCMSource>();
        std::string decodeError;
        // PERF13 -- the sound alone, never the pictures around it.
        const bool ok = job ? job(*decoded, decodeError)
                            : audio_decode::DecodeStereoPcm16(
                                  path.string(), kMixRate, *decoded,
                                  decodeError, &impl_->cancel);
        const bool cancelled = impl_->cancel.load();
        if (!ok && !cancelled && decodeError != "no audio stream")
            std::fprintf(stderr, "Audio disabled for %s: %s\n",
                         path.string().c_str(), decodeError.c_str());

        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->decoding = false;
            if (ok) {
                impl_->sources[sourceId] = std::move(decoded);
                impl_->lastUsed[sourceId] = ++impl_->useTick;
                impl_->EvictLocked();
                impl_->PublishPlanLocked();
            } else if (!cancelled) {
                // A rush abandoned mid-decode is not a rush that cannot be
                // decoded: remembering it as failed would keep it silent for
                // the rest of the session.
                impl_->failed.insert(sourceId);
            }
            if (impl_->queue.empty()) impl_->idle.notify_all();
            if (impl_->stopping) return;
        }
    }
}

bool AudioPlayback::WaitForDecodes(int timeoutMilliseconds) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    return impl_->idle.wait_for(
        lock, std::chrono::milliseconds(std::max(0, timeoutMilliseconds)),
        [this] { return impl_->queue.empty() && !impl_->decoding; });
}

// PRD37 -- opens the output if it is closed. It is the only place that
// does: an open output costs the caller nothing more.
bool AudioPlayback::EnsureOutputStarted(const char* failure,
                                        std::string& error) {
    impl_->lastSound = std::chrono::steady_clock::now();
    if (impl_->engine.isRunning) return true;
    NSError* startError = nil;
    if (![impl_->engine startAndReturnError:&startError]) {
        error = startError.localizedDescription.UTF8String ?: failure;
        return false;
    }
    ++impl_->outputStarts;
    return true;
}

bool AudioPlayback::PlayFrom(RationalTime position, int direction,
                             std::string& error) {
    if (!impl_->engine || !impl_->sourceNode || direction == 0) {
        error = "audio engine is not initialized";
        return false;
    }
    impl_->generation.fetch_add(1);
    impl_->cursor.store(position.to_frames(kMixRate));
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    impl_->direction.store(std::clamp(direction, -4, 4));
    if (!EnsureOutputStarted("audio start failed", error)) {
        impl_->direction.store(0);
        return false;
    }
    error.clear();
    return true;
}

bool AudioPlayback::ScrubAt(RationalTime position, std::string& error) {
    if (!impl_->engine || !impl_->sourceNode) {
        error = "audio engine is not initialized";
        return false;
    }
    const int64_t sample = position.to_frames(kMixRate);
    if (impl_->lastScrubSample.exchange(sample) == sample) {
        error.clear();
        return true;
    }
    impl_->scrubTriggerCount.fetch_add(1);
    impl_->generation.fetch_add(1);
    impl_->cursor.store(sample);
    impl_->scrubRemaining.store(kScrubSamples);
    impl_->direction.store(1);
    if (!EnsureOutputStarted("audio scrub failed", error)) {
        impl_->direction.store(0);
        impl_->scrubRemaining.store(0);
        impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
        return false;
    }
    error.clear();
    return true;
}

// PRD37 -- silence, not a closed output. Closing the output here and
// opening it again on the next play cost 130 to 200 ms on the editor
// thread whenever the two followed each other within about 150 ms (measured
// on 3 October 2026: CoreAudio finishes closing the device before it opens
// it again) -- and setPlaybackDirection: did exactly that at every start, the
// playback clock already running: three to five frames skipped, and the
// sound that late behind the picture for the whole play. The output closes
// once it has been silent long enough (ReleaseIdleOutput).
void AudioPlayback::Stop() {
    if (!impl_) return;
    impl_->generation.fetch_add(1);
    impl_->direction.store(0);
    impl_->scrubRemaining.store(0);
    impl_->lastScrubSample.store(std::numeric_limits<int64_t>::min());
    impl_->lastSound = std::chrono::steady_clock::now();
}

void AudioPlayback::ReleaseIdleOutput(
    std::chrono::steady_clock::time_point now) {
    if (!impl_->engine) return;
    const bool open = impl_->engine.isRunning;
    // A scrub ends on the realtime thread, which sets the direction back to
    // zero: until then, it sounds.
    const bool sounding = impl_->direction.load() != 0;
    if (playback::ShouldReleaseAudioOutput(open, sounding,
                                           now - impl_->lastSound))
        [impl_->engine pause];
}

bool AudioPlayback::OutputOpen() const {
    return impl_->engine != nil && impl_->engine.isRunning;
}

uint64_t AudioPlayback::OutputStartCount() const { return impl_->outputStarts; }

size_t AudioPlayback::DecodedSourceCount() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->sources.size();
}

size_t AudioPlayback::DecodedBytes() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    size_t bytes = 0;
    for (const auto& source : impl_->sources)
        if (source.second) bytes += PcmBytes(*source.second);
    return bytes;
}

size_t AudioPlayback::PlannedClipCount() const {
    return std::atomic_load(&impl_->plan)->clips.size();
}

uint64_t AudioPlayback::ScrubTriggerCount() const {
    return impl_->scrubTriggerCount.load();
}

int AudioPlayback::ShuttleSpeed() const { return impl_->direction.load(); }
