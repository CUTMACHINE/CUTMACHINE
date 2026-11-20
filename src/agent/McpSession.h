#pragma once

// IA5 -- ROADMAP.md. What a running MCP server keeps between two calls, and
// only a running server: `--call-tool` and the chat panel run every tool to
// its end within the call, so none of this means anything there and the
// catalog refuses its arguments by name (McpTools.h).
//
// Three things were missing for an agent driving a long montage over HTTP:
//
//   - Background tasks. A transcription holds the single HTTP connection for
//     minutes to an hour; a client that gives up in between loses the answer
//     and, retrying, runs Whisper again. `async: true` on an analysis starts
//     it on this session's worker and answers at once with a task_id;
//     get_task, cancel_task and list_tasks follow it. The task outlives the
//     connection that started it.
//   - Idempotency keys. A call whose answer was lost is sent again with the
//     same `idempotency_key` and gets the first answer back (`replayed:
//     true`) instead of a second timeline. The keys live in this object, in
//     the server's memory, never in the project: they are a promise about one
//     conversation with one server, and a key kept on disk would outlive the
//     session and replay an answer days later. A server restart forgets them;
//     `expected_revision` covers that case.
//   - Expected revision. Every answer carries the project's `revision`
//     (McpBackend::Revision); a mutation given `expected_revision` is refused
//     with StaleRevision when the project has moved since.
//
// A replay applies nothing, so it adds no entry to the timeline's journal:
// inside a PL1 intervention (begin_intervention/end_intervention) a replayed
// edit never becomes a second step, and a replayed end_intervention returns
// the fold it already made instead of failing on the history it rewrote.
//
// Writes stay serialized. The request thread is the only one that edits the
// project; a task's backend (McpBackend::ForkForTask) refuses edits and only
// installs analysis caches; tasks run one at a time; and while one is queued
// or running, a synchronous analysis that installs a cache is refused
// (AnalysisBusy) rather than raced, as are create_project and open_project
// (TasksRunning), which would move the lock the task works under.
//
// A task notes the revision and the identity of its inputs when it starts --
// the audible arrangement of the timeline for transcribe_timeline, each named
// rush's path, rate and duration otherwise -- and compares them once its
// result is installed. A change is reported (`stale`, `stale_reason`), never
// passed over.
//
// PRD16 -- ROADMAP.md. Exports take the same path: export_timeline,
// export_timelines and render_motion_video run as tasks too, since the
// product's use is a series -- seven films exported at once -- which held
// one call for many minutes, without progress or a way to stop it. Three
// things differ from an analysis. The task writes the file its caller named
// and no cache, so it never makes a synchronous analysis wait. It works on
// the project as it was when it was asked for: the session snapshots it on
// the calling thread and pins it on the fork (McpBackend::PinTaskProject),
// so the edits that go on meanwhile reach neither the file nor the plan,
// and `stale` says when the exported timeline -- or what it reads -- moved
// before the end. And its media work is FFmpeg, run through the task's
// runner one file at a time (Exporter::RunPlan): get_task reads the frames
// encoded and, for a batch, which file and how many are complete;
// cancel_task kills FFmpeg and the partial file goes with it.

#include "Json.h"
#include "McpBackend.h"
#include "McpTools.h"
#include "MediaTaskManager.h"
#include "Ulid.h"

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

// PRD16 -- ROADMAP.md. The arguments that name, at the caller's path, a file
// a tool writes or reads. In a running server -- `--mcp-serve`, the app's
// relay -- a relative one is taken from the folder that holds the project's
// package (McpBackend::ProjectFolder): an agent cannot know the server's
// working directory, and a file written there is lost to it. The tool's
// answer then names the absolute path it wrote. `--call-tool` and the CLI,
// typed in a shell, have no session and keep the shell's directory. `list`,
// when set, names an array argument whose objects each carry `argument`.
// A tool that writes a file at its caller's path joins by a line here.
struct McpCallerPathArgument {
    const char* tool;
    const char* argument;
    const char* list;
};
const std::vector<McpCallerPathArgument>& McpCallerPathArguments();

// Rewrites `arguments` so that every argument of the table `tool` has names
// an absolute path, a relative one being taken from `folder`. Nothing changes
// when `folder` is empty, or for a tool the table does not name.
void ResolveMcpCallerPaths(const std::string& tool, const std::string& folder,
                           mcp_json::Value& arguments);

class McpSession {
public:
    McpSession();
    ~McpSession();
    McpSession(const McpSession&) = delete;
    McpSession& operator=(const McpSession&) = delete;

    // The server's entry point for `tools/call`: handles the session's own
    // tools and arguments, then hands the rest to `registry`. `registry` must
    // outlive this session, since a task runs it on the worker.
    McpToolCallOutcome Call(const McpToolRegistry& registry,
                            McpBackend& backend, const std::string& toolName,
                            const mcp_json::Value& arguments);

    // Tests only: replaces the media work a task's command would run (Whisper,
    // a decode) with a stand-in, so a slow, cancellable task can be driven
    // without a model. Must be set before the first task starts.
    using WorkDecorator = std::function<MediaTaskManager::Work(
        MediaTaskKind, const std::string&, MediaTaskManager::Work)>;
    void SetMediaWorkDecorator(WorkDecorator decorator);

    // DEC10 -- docs/roadmap/AX-2026-09.md (AX7). The tasks this session
    // starts (`async: true`: exports, analyses) run on `shared` instead of
    // the session's own manager, each named `labelPrefix` + the tool. The
    // app's endpoint gives it the editor's manager: an agent relayed to the
    // window then has its tasks in the window's task bar, which cancels them,
    // as ingest_media has had since 19 October -- they ran on a manager of
    // the endpoint's own that nothing on screen showed. get_task and
    // cancel_task read and cancel them there. `shared` must outlive the
    // session, which cancels its own tasks there and waits for them before
    // it goes. Set before the first task.
    void SetTaskManager(MediaTaskManager* shared, std::string labelPrefix);

    static constexpr size_t kRetainedIdempotencyKeys = 1024;
    static constexpr size_t kRetainedFinishedTasks =
        MediaTaskManager::kRetainedFinishedTasks;
    static constexpr int kCancelWaitMilliseconds = 10000;
    static constexpr int64_t kMaximumWaitSeconds = 30;

    // PRD17 -- how many project revisions the session remembers the
    // timelines of, to tell a StaleRevision refusal which grain moved.
    static constexpr size_t kRetainedRevisionMarks = 256;

private:
    struct Task;
    struct Remembered {
        std::string fingerprint;
        McpToolCallOutcome outcome;
        Ulid task_id;
    };
    // PRD17 -- ROADMAP.md. A revision this session answered with, and what
    // it was made of: each timeline's own bytes, hashed, and the rest of the
    // project -- what the timelines share. expected_revision stays at the
    // project's grain (ROADMAP.md, "Trois grains de révision"); this only
    // lets its refusal say whether the caller's timeline moved, or another.
    struct RevisionMark {
        std::string revision;
        Ulid active_timeline_id;
        std::string shared;
        std::map<Ulid, std::string> timelines;
    };
    static bool MarkRevision(McpBackend& backend, const std::string& revision,
                             RevisionMark& mark);
    void RememberRevision(McpBackend& backend, const std::string& revision);
    std::string RunningTasks(const std::vector<Ulid>& ids,
                             mcp_json::Value& running) const;

    McpToolCallOutcome CallTaskTool(const std::string& toolName,
                                    const mcp_json::Value& arguments);
    bool StartTask(const McpToolRegistry& registry, McpBackend& backend,
                   const McpTool& tool, const mcp_json::Value& arguments,
                   McpToolCallOutcome& outcome, Ulid& taskId);
    std::string TaskView(const Task& task, bool withResult) const;
    std::shared_ptr<Task> FindTask(const Ulid& id) const;
    std::vector<Ulid> ActiveTaskIds() const;
    void PruneFinishedLocked();
    // DEC10 -- the manager the tasks run on: the shared one when set.
    MediaTaskManager& TaskManager();

    mutable std::mutex mutex_;
    std::condition_variable finished_;
    std::map<Ulid, std::shared_ptr<Task>> tasks_;
    std::vector<Ulid> task_order_;
    std::map<std::string, Remembered> remembered_;
    std::deque<std::string> remembered_order_;
    std::deque<RevisionMark> revision_marks_;
    WorkDecorator decorator_;
    // DEC10 -- SetTaskManager.
    MediaTaskManager* shared_manager_ = nullptr;
    std::string task_label_prefix_ = "MCP ";
    // Declared last, so destroyed first: its destructor cancels and joins the
    // worker while everything a task touches still exists.
    MediaTaskManager manager_{1};
};
