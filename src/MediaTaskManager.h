#pragma once

#include "Ulid.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

enum class MediaTaskKind {
    Probe,
    Thumbnail,
    Waveform,
    Proxy,
    Relink,
    Beat,
    Transcription,
    ShotQuality,
    SpeechOnset,
    // R8 -- docs/roadmap/REEL-2026-09.md. Its own kind rather than
    // ShotQuality's: the two decode the same rushes for different
    // questions, and the task bar names what it is waiting for.
    IsolatedFrames,
    // AX2 -- docs/roadmap/AX-2026-09.md. MFA aligning words to the rushes.
    WordAlignment,
    // PRD16 -- ROADMAP.md. FFmpeg encoding one file of an export: a timeline,
    // an entry of a batch, or a motion composition rendered alone.
    Export,
    // SYNCHRO-2026-10 SY1 -- two rushes' sound decoded and lined up.
    AudioOffset,
    // PA3 -- docs/roadmap/PARITE-2026-11.md. A rush's sound read by the
    // speaker embedding model: who speaks when.
    SpeakerDiarization,
    // FC6 -- FILMCRAFT-2026-10. A voice's rush decoded, its speech ranges
    // and the ducking proposed under it.
    Ducking,
    // GN6a -- GENERATIF-2026-10. The local model saying a voice-over
    // (VoiceOver.h): a minute or more of computing per take.
    VoiceOver
};
enum class MediaTaskState { Queued, Running, Succeeded, Failed, Cancelled };

struct MediaTaskSnapshot {
    Ulid id;
    MediaTaskKind kind = MediaTaskKind::Probe;
    MediaTaskState state = MediaTaskState::Queued;
    std::string label;
    std::string detail;
    double progress = 0.0;
    std::string error;
};

const char* MediaTaskKindName(MediaTaskKind kind);
const char* MediaTaskStateName(MediaTaskState state);

class MediaTaskContext {
public:
    bool Cancelled() const;
    void SetProgress(double progress, std::string detail = {});
    // PRD16 -- ROADMAP.md. The flag Cancelled() reads, for work that hands
    // it on to code polling an atomic itself: Exporter::Run kills FFmpeg and
    // removes its partial file the moment it turns true. Lives as long as
    // the task.
    const std::atomic_bool* CancelFlag() const { return cancelled_.get(); }

private:
    friend class MediaTaskManager;
    MediaTaskContext(std::shared_ptr<std::atomic_bool> cancelled,
                     std::function<void(double, std::string)> progress);
    std::shared_ptr<std::atomic_bool> cancelled_;
    std::function<void(double, std::string)> progress_;
};

class MediaTaskManager {
public:
    using Work = std::function<bool(MediaTaskContext&, std::string&)>;
    using Completion = std::function<void(const MediaTaskSnapshot&)>;

    explicit MediaTaskManager(size_t concurrency = 1);
    ~MediaTaskManager();
    MediaTaskManager(const MediaTaskManager&) = delete;
    MediaTaskManager& operator=(const MediaTaskManager&) = delete;

    Ulid Enqueue(MediaTaskKind kind, std::string label, Work work,
                 Completion completion = {});
    bool Cancel(const Ulid& id);
    void CancelAll();
    std::vector<MediaTaskSnapshot> Snapshot() const;
    bool WaitForIdle(int timeoutMilliseconds);

    // PERF-2026-09. Finished tasks used to be kept forever, and Snapshot()
    // copies every one of them -- four std::strings apiece -- under the same
    // mutex the workers take to report progress. The editor polls it on
    // every display tick, so importing a few hundred rushes turned an idle
    // window into a permanent 60 Hz copy of the whole session's history,
    // slowing the workers down alongside. Only the most recent finished
    // tasks are kept now; the oldest are dropped as new ones finish.
    //
    // The bound is generous on purpose: the editor reconciles finished tasks
    // against its own pending lists once per display tick, so a task would
    // have to be pushed out by this many newer *finished* tasks inside a
    // single tick to be missed -- which two worker threads doing FFmpeg work
    // cannot do.
    static constexpr size_t kRetainedFinishedTasks = 256;

private:
    struct Task;
    void WorkerLoop();
    bool IdleLocked() const;
    void PruneFinishedLocked();

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::condition_variable idleCondition_;
    std::map<Ulid, std::shared_ptr<Task>> tasks_;
    std::vector<Ulid> queue_;
    std::vector<std::thread> workers_;
    bool stopping_ = false;
};

// CHAT-PARITE -- ROADMAP.md F2.4 and P7. The CLI and the MCP server run a
// transcription on a manager of their own and wait for it; the chat panel
// has to wait too, since a tool call answers with its result, but on the
// editor's manager: the transcription it starts then shows in the task bar
// and is cancelled there, like the one the Media menu starts, and the main
// thread never waits on Whisper.
constexpr int kMediaTaskWaitLimitMilliseconds = 24 * 60 * 60 * 1000;

// Runs `work` as one task of `manager` and blocks the calling thread until
// that task -- and only it; the manager's other tasks keep running -- has
// finished. Returns its final snapshot. Past `timeoutMilliseconds` the task
// is cancelled and still waited for, because a task's work usually refers
// to the caller's stack; the snapshot then reads Cancelled, with the error
// "timed out". Must not be called from one of `manager`'s own workers.
//
// `enqueued`, when given, receives the task's id as soon as it is queued,
// before the wait: the caller can then cancel that task, and only it, from
// another thread (visual QA of 19 September 2026: a transcription the chat
// started could not be stopped from the chat).
MediaTaskSnapshot RunMediaTaskAndWait(
    MediaTaskManager& manager, MediaTaskKind kind, std::string label,
    MediaTaskManager::Work work,
    int timeoutMilliseconds = kMediaTaskWaitLimitMilliseconds,
    const std::function<void(const Ulid&)>& enqueued = {});

// Where a command runs its long media work. Empty means a private manager
// the command waits on; the app passes one that waits on its own.
using MediaTaskRunner = std::function<MediaTaskSnapshot(
    MediaTaskKind, std::string, MediaTaskManager::Work)>;
