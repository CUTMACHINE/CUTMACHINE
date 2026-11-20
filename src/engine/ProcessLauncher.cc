#include "ProcessLauncher.h"

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>

extern char** environ;

namespace process {
namespace {

constexpr int kPollMilliseconds = 100;
constexpr std::chrono::seconds kTerminationGrace(2);

// FI9 -- the groups SignalRunningGroups reaches: a fixed table of lock-free
// slots, readable from a signal handler. More runs at once than slots go
// unlisted, which only loses that forwarding for them.
constexpr size_t kGroupSlots = 256;
std::atomic<pid_t> g_running_groups[kGroupSlots];
static_assert(std::atomic<pid_t>::is_always_lock_free);

// Lists a started group until the run that started it has waited for it.
class RunningGroup {
public:
    explicit RunningGroup(pid_t group) {
        for (std::atomic<pid_t>& slot : g_running_groups) {
            pid_t empty = 0;
            if (slot.compare_exchange_strong(empty, group)) {
                slot_ = &slot;
                return;
            }
        }
    }
    ~RunningGroup() {
        if (slot_ != nullptr) {
            slot_->store(0);
        }
    }
    RunningGroup(const RunningGroup&) = delete;
    RunningGroup& operator=(const RunningGroup&) = delete;

private:
    std::atomic<pid_t>* slot_ = nullptr;
};

void AppendTail(std::string& tail, const char* bytes, size_t count,
                size_t maximum) {
    tail.append(bytes, count);
    if (tail.size() > maximum) {
        tail.erase(0, tail.size() - maximum);
    }
}

void CloseIfOpen(int& descriptor) {
    if (descriptor >= 0) {
        close(descriptor);
        descriptor = -1;
    }
}

// A pipe whose two ends close on exec in every child but the one they are
// handed to. Without it, a second program started from another thread while
// this one runs inherits the write end, and this launcher waits for an end
// of file that only comes when that unrelated program exits too.
bool OpenPipe(int descriptors[2]) {
    if (pipe(descriptors) != 0) {
        return false;
    }
    for (int index = 0; index < 2; ++index) {
        const int flags = fcntl(descriptors[index], F_GETFD);
        if (flags >= 0) {
            fcntl(descriptors[index], F_SETFD, flags | FD_CLOEXEC);
        }
    }
    return true;
}

}  // namespace

bool Result::Succeeded() const {
    return Started() && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

bool Result::Signalled() const { return Started() && WIFSIGNALED(status); }

int Result::TerminationSignal() const { return WTERMSIG(status); }

int Result::ExitStatus() const { return WEXITSTATUS(status); }

std::string Result::SystemErrorText() const {
    return std::strerror(system_error);
}

Result Run(const Request& request) {
    Result result;
    if (request.arguments.empty() || request.arguments.front().empty()) {
        result.stage = Stage::kSpawn;
        result.system_error = ENOENT;
        return result;
    }
    std::vector<std::string> storage = request.arguments;
    std::vector<char*> argv;
    argv.reserve(storage.size() + 1);
    for (std::string& value : storage) {
        argv.push_back(value.data());
    }
    argv.push_back(nullptr);

    const bool pipeOutput = request.standard_output == Stream::kPipe;
    const bool pipeError = request.standard_error == Stream::kPipe;
    int outputPipe[2] = {-1, -1};
    int errorPipe[2] = {-1, -1};
    if ((pipeOutput && !OpenPipe(outputPipe)) ||
        (pipeError && !OpenPipe(errorPipe))) {
        result.stage = Stage::kPipes;
        result.system_error = errno;
        CloseIfOpen(outputPipe[0]);
        CloseIfOpen(outputPipe[1]);
        CloseIfOpen(errorPipe[0]);
        CloseIfOpen(errorPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (request.discard_input) {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0);
    }
    if (pipeOutput) {
        posix_spawn_file_actions_adddup2(&actions, outputPipe[1],
                                         STDOUT_FILENO);
    } else if (request.standard_output == Stream::kDiscard) {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null",
                                         O_WRONLY, 0);
    }
    if (pipeError) {
        posix_spawn_file_actions_adddup2(&actions, errorPipe[1], STDERR_FILENO);
    } else if (request.standard_error == Stream::kDiscard) {
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null",
                                         O_WRONLY, 0);
    }
    for (const int descriptor :
         {outputPipe[0], errorPipe[0], outputPipe[1], errorPipe[1]}) {
        if (descriptor >= 0) {
            posix_spawn_file_actions_addclose(&actions, descriptor);
        }
    }
    // FI9 -- docs/roadmap/FIABILITE-2026-10.md. A wrapper may start the real
    // encoder as its child. Killing only the wrapper leaves that encoder
    // running, holding the output pipes open. A dispatch worker's signal
    // mask must not make either process deaf to the cancellation.
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP |
                                              POSIX_SPAWN_SETSIGMASK |
                                              POSIX_SPAWN_SETSIGDEF);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    pid_t child = 0;
    const int spawnResult =
        posix_spawnp(&child, storage.front().c_str(), &actions, &attributes,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    CloseIfOpen(outputPipe[1]);
    CloseIfOpen(errorPipe[1]);
    if (spawnResult != 0) {
        CloseIfOpen(outputPipe[0]);
        CloseIfOpen(errorPipe[0]);
        result.stage = Stage::kSpawn;
        result.system_error = spawnResult;
        return result;
    }
    const RunningGroup listed(child);
    if (request.on_started) {
        request.on_started();
    }

    bool forced = false;
    std::chrono::steady_clock::time_point cancellationStarted;
    const auto pollCancellation = [&]() {
        if (!request.cancelled) {
            return;
        }
        if (!result.cancelled && request.cancelled()) {
            result.cancelled = true;
            cancellationStarted = std::chrono::steady_clock::now();
            kill(-child, SIGTERM);
        }
        if (result.cancelled && !forced &&
            std::chrono::steady_clock::now() - cancellationStarted >
                kTerminationGrace) {
            forced = true;
            kill(-child, SIGKILL);
        }
    };

    std::vector<char> bytes(std::max<size_t>(request.read_bytes, 1));
    while (outputPipe[0] >= 0 || errorPipe[0] >= 0) {
        pollCancellation();
        // A negative descriptor is one poll(2) skips: a pipe that reached
        // its end must not be polled again, or the loop spins on it.
        pollfd descriptors[2] = {
            {outputPipe[0], POLLIN, 0},
            {errorPipe[0], POLLIN, 0},
        };
        if (poll(descriptors, 2, kPollMilliseconds) < 0 && errno != EINTR) {
            break;
        }
        if (outputPipe[0] >= 0 &&
            (descriptors[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t count =
                read(outputPipe[0], bytes.data(), bytes.size());
            if (count <= 0) {
                CloseIfOpen(outputPipe[0]);
            } else if (request.on_output) {
                request.on_output(bytes.data(), static_cast<size_t>(count));
            }
        }
        if (errorPipe[0] >= 0 &&
            (descriptors[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t count =
                read(errorPipe[0], bytes.data(), bytes.size());
            if (count <= 0) {
                CloseIfOpen(errorPipe[0]);
            } else {
                AppendTail(result.error_tail, bytes.data(),
                           static_cast<size_t>(count),
                           request.error_tail_bytes);
            }
        }
    }
    // Only reached with a pipe still open when poll(2) itself failed. Closed
    // before waiting, so a child blocked on a full pipe ends instead of
    // keeping this thread forever.
    CloseIfOpen(outputPipe[0]);
    CloseIfOpen(errorPipe[0]);

    if (!request.cancelled) {
        while (waitpid(child, &result.status, 0) < 0 && errno == EINTR) {
        }
        return result;
    }
    // A cancellable run stays cancellable until the child is reaped: one
    // that closed its pipes, or was given none, is otherwise out of reach.
    // The wait starts short because the usual child exits as its pipes
    // close.
    int delayMilliseconds = 1;
    for (;;) {
        const pid_t reaped = waitpid(child, &result.status, WNOHANG);
        if (reaped == child || (reaped < 0 && errno != EINTR)) {
            break;
        }
        pollCancellation();
        poll(nullptr, 0, delayMilliseconds);
        delayMilliseconds = std::min(delayMilliseconds * 2, kPollMilliseconds);
    }
    // The leader can exit before descendants which closed their streams.
    // Once it is reaped, finish the cancelled group before returning.
    if (result.cancelled) {
        kill(-child, SIGKILL);
    }
    return result;
}

Result StartDetached(const std::vector<std::string>& arguments) {
    Result result;
    if (arguments.empty() || arguments.front().empty()) {
        result.stage = Stage::kSpawn;
        result.system_error = EINVAL;
        return result;
    }
    std::vector<char*> argv;
    std::vector<std::string> storage = arguments;
    for (std::string& argument : storage) argv.push_back(argument.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                     O_RDONLY, 0);
#if defined(__APPLE__)
    // Every other descriptor closes in the child, the standard ones being
    // named by a file action or inherited explicitly.
    posix_spawn_file_actions_addinherit_np(&actions, STDOUT_FILENO);
    posix_spawn_file_actions_addinherit_np(&actions, STDERR_FILENO);
#endif
    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags =
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#if defined(__APPLE__)
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    sigaddset(&defaults, SIGPIPE);
    posix_spawnattr_setsigdefault(&attributes, &defaults);
    pid_t child = 0;
    const int spawnResult =
        posix_spawnp(&child, storage.front().c_str(), &actions, &attributes,
                     argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (spawnResult != 0) {
        result.stage = Stage::kSpawn;
        result.system_error = spawnResult;
    }
    return result;
}

void SignalRunningGroups(int signalNumber) {
    for (const std::atomic<pid_t>& slot : g_running_groups) {
        const pid_t group = slot.load();
        if (group > 0) {
            kill(-group, signalNumber);
        }
    }
}

}  // namespace process
