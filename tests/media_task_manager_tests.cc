#include "MediaTaskManager.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

int main() {
    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) {
            ++failures;
            std::cerr << "FAIL: " << message << '\n';
        }
    };

    MediaTaskManager manager(1);
    std::atomic_int completions{0};
    const Ulid first = manager.Enqueue(
        MediaTaskKind::Probe, "probe.mov",
        [](MediaTaskContext& context, std::string&) {
            context.SetProgress(0.5, "Headers");
            return true;
        },
        [&](const MediaTaskSnapshot& task) {
            if (task.state == MediaTaskState::Succeeded) ++completions;
        });
    check(manager.WaitForIdle(1000), "successful task reaches idle");
    auto snapshot = manager.Snapshot();
    check(snapshot.size() == 1 && snapshot[0].id == first &&
              snapshot[0].state == MediaTaskState::Succeeded &&
              snapshot[0].progress == 1.0 && completions.load() == 1,
          "success and completion are observable");

    std::atomic_bool entered{false};
    const Ulid running = manager.Enqueue(
        MediaTaskKind::Proxy, "proxy.mov",
        [&](MediaTaskContext& context, std::string&) {
            entered.store(true);
            while (!context.Cancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return false;
        });
    while (!entered.load()) std::this_thread::yield();
    std::atomic_bool queuedRan{false};
    std::atomic_bool queuedCompletion{false};
    const Ulid queued = manager.Enqueue(
        MediaTaskKind::Thumbnail, "queued.mov",
        [&](MediaTaskContext&, std::string&) {
            queuedRan.store(true);
            return true;
        },
        [&](const MediaTaskSnapshot& task) {
            queuedCompletion.store(task.state == MediaTaskState::Cancelled);
        });
    check(manager.Cancel(queued), "queued task accepts cancellation");
    check(queuedCompletion.load() && !queuedRan.load(),
          "queued cancellation completes without running work");
    check(manager.Cancel(running), "running task accepts cancellation");
    check(manager.WaitForIdle(1000), "cancelled task reaches idle");
    snapshot = manager.Snapshot();
    const auto cancelled = std::find_if(
        snapshot.begin(), snapshot.end(),
        [&](const MediaTaskSnapshot& task) { return task.id == running; });
    check(cancelled != snapshot.end() &&
              cancelled->state == MediaTaskState::Cancelled,
          "running task reports cancelled state");

    const Ulid failed =
        manager.Enqueue(MediaTaskKind::Waveform, "broken.wav",
                        [](MediaTaskContext&, std::string& error) {
                            error = "decode failed";
                            return false;
                        });
    check(manager.WaitForIdle(1000), "failed task reaches idle");
    snapshot = manager.Snapshot();
    const auto failure = std::find_if(
        snapshot.begin(), snapshot.end(),
        [&](const MediaTaskSnapshot& task) { return task.id == failed; });
    check(failure != snapshot.end() &&
              failure->state == MediaTaskState::Failed &&
              failure->error == "decode failed",
          "failure reason is retained");

    // PERF-2026-09. Finished tasks used to accumulate for the life of the
    // session, and the editor copies the whole set on every display tick, so
    // importing a few hundred rushes left an idle window copying that
    // history sixty times a second under the workers' own mutex. The history
    // is bounded now, newest kept.
    const size_t overflow = MediaTaskManager::kRetainedFinishedTasks + 40;
    std::vector<Ulid> manyIds;
    manyIds.reserve(overflow);
    for (size_t index = 0; index < overflow; ++index)
        manyIds.push_back(manager.Enqueue(
            MediaTaskKind::Thumbnail, "bulk",
            [](MediaTaskContext&, std::string&) { return true; }));
    check(manager.WaitForIdle(5000), "the bulk of tasks reaches idle");
    snapshot = manager.Snapshot();
    check(snapshot.size() <= MediaTaskManager::kRetainedFinishedTasks,
          "finished task history stays bounded");
    const auto newest = std::find_if(snapshot.begin(), snapshot.end(),
                                     [&](const MediaTaskSnapshot& task) {
                                         return task.id == manyIds.back();
                                     });
    const auto oldest = std::find_if(snapshot.begin(), snapshot.end(),
                                     [&](const MediaTaskSnapshot& task) {
                                         return task.id == manyIds.front();
                                     });
    check(newest != snapshot.end() && oldest == snapshot.end(),
          "the newest finished tasks are the ones kept");

    // CHAT-PARITE -- ROADMAP.md F2.4. The chat worker waits on the editor's
    // own manager for the one transcription it asked for, while that manager
    // keeps serving thumbnails and proxies; cancelling from the task bar must
    // release the wait with the reason, and a wait that expires must not
    // return while the work still refers to the waiter's stack.
    {
        MediaTaskManager shared(2);
        std::atomic_bool otherStop{false};
        const Ulid other = shared.Enqueue(
            MediaTaskKind::Proxy, "long proxy",
            [&](MediaTaskContext& context, std::string&) {
                while (!otherStop.load() && !context.Cancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return true;
            });
        const MediaTaskSnapshot done = RunMediaTaskAndWait(
            shared, MediaTaskKind::Transcription, "Whisper 1 media",
            [](MediaTaskContext& context, std::string&) {
                context.SetProgress(0.5, "Transcription");
                return true;
            });
        check(done.state == MediaTaskState::Succeeded &&
                  done.kind == MediaTaskKind::Transcription &&
                  done.label == "Whisper 1 media",
              "the waiter gets its own task back, finished");
        const auto others = shared.Snapshot();
        check(std::any_of(others.begin(), others.end(),
                          [&](const MediaTaskSnapshot& task) {
                              return task.id == other &&
                                     task.state == MediaTaskState::Running;
                          }),
              "and does not wait for the manager's other tasks");

        const MediaTaskSnapshot failedTask = RunMediaTaskAndWait(
            shared, MediaTaskKind::Transcription, "broken",
            [](MediaTaskContext&, std::string& error) {
                error = "Whisper model is not a regular file";
                return false;
            });
        check(failedTask.state == MediaTaskState::Failed &&
                  failedTask.error == "Whisper model is not a regular file",
              "a failure comes back with the task's own reason");

        std::atomic_bool started{false};
        MediaTaskSnapshot cancelledTask;
        std::thread waiter([&] {
            cancelledTask = RunMediaTaskAndWait(
                shared, MediaTaskKind::Transcription, "cancel me",
                [&](MediaTaskContext& context, std::string&) {
                    started.store(true);
                    while (!context.Cancelled()) {
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(1));
                    }
                    return false;
                });
        });
        while (!started.load()) {
            std::this_thread::yield();
        }
        for (const MediaTaskSnapshot& task : shared.Snapshot()) {
            if (task.label == "cancel me") {
                shared.Cancel(task.id);
            }
        }
        waiter.join();
        check(cancelledTask.state == MediaTaskState::Cancelled &&
                  cancelledTask.error.empty(),
              "cancelling from the task bar releases the waiter");

        std::atomic_bool stillRunning{false};
        const MediaTaskSnapshot expired = RunMediaTaskAndWait(
            shared, MediaTaskKind::Transcription, "too slow",
            [&](MediaTaskContext& context, std::string&) {
                stillRunning.store(true);
                while (!context.Cancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
                stillRunning.store(false);
                return false;
            },
            10);
        check(expired.state == MediaTaskState::Cancelled &&
                  expired.error == "timed out" && !stillRunning.load(),
              "an expired wait cancels the task and returns once it stopped");
        otherStop.store(true);
        check(shared.WaitForIdle(1000), "the shared manager drains");
    }

    // A task still queued when its manager is destroyed never runs; its
    // waiter must still be released.
    {
        auto doomed = std::make_unique<MediaTaskManager>(1);
        std::atomic_bool blockerStarted{false};
        doomed->Enqueue(
            MediaTaskKind::Proxy, "blocker",
            [&](MediaTaskContext& context, std::string&) {
                blockerStarted.store(true);
                while (!context.Cancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                return false;
            });
        while (!blockerStarted.load()) {
            std::this_thread::yield();
        }
        std::atomic_bool released{false};
        MediaTaskSnapshot orphan;
        std::thread waiter([&] {
            orphan = RunMediaTaskAndWait(
                *doomed, MediaTaskKind::Transcription, "queued",
                [](MediaTaskContext&, std::string&) { return true; });
            released.store(true);
        });
        for (int attempt = 0; attempt < 1000; ++attempt) {
            const auto queued = doomed->Snapshot();
            if (std::any_of(queued.begin(), queued.end(),
                            [](const MediaTaskSnapshot& task) {
                                return task.label == "queued";
                            })) {
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        doomed.reset();
        for (int attempt = 0; attempt < 5000 && !released.load(); ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        check(released.load() && orphan.state == MediaTaskState::Cancelled,
              "a task dropped by the manager's destructor releases its waiter");
        if (!released.load()) {
            waiter.detach();
            std::cerr << "FAIL: waiter still blocked; aborting\n";
            return 1;
        }
        waiter.join();
    }

    // Visual QA of 19 September 2026 -- the chat's Stop button cancels the
    // task a turn waits on: the waiter learns its id while it runs, and
    // cancelling that id ends the wait with the task cancelled.
    {
        MediaTaskManager chatTasks(1);
        Ulid seen;
        std::thread canceller;
        const MediaTaskSnapshot stopped = RunMediaTaskAndWait(
            chatTasks, MediaTaskKind::Probe, "long work",
            [](MediaTaskContext& context, std::string& error) {
                while (!context.Cancelled()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                error = "stopped";
                return false;
            },
            kMediaTaskWaitLimitMilliseconds,
            [&](const Ulid& id) {
                seen = id;
                canceller = std::thread([&chatTasks, id] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(20));
                    chatTasks.Cancel(id);
                });
            });
        canceller.join();
        check(!seen.empty() && stopped.id == seen &&
                  stopped.state == MediaTaskState::Cancelled,
              "the waiter gets the task's id at once, and cancelling it "
              "stops the wait");
    }

    if (failures) return 1;
    std::cout << "All media task manager tests passed\n";
    return 0;
}
