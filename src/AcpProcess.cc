#include "AcpProcess.h"

#include <fcntl.h>
#include <poll.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <utility>

extern char** environ;

namespace acp {

namespace {

// Claude Code marks the sessions it runs with these, and a Claude Code
// started with them takes itself for a session nested in another.
constexpr const char* kNestedAgentVariables[] = {"CLAUDECODE",
                                                 "CLAUDE_CODE_ENTRYPOINT"};

// A login shell that reads its startup files (nvm, Homebrew's shellenv) can
// be slow; one that waits on something is not waited for.
constexpr int kShellTimeoutMs = 10000;

std::string ShellQuote(const std::string& text) {
    std::string quoted = "'";
    for (const char character : text) {
        if (character == '\'') {
            quoted += "'\\''";
        } else {
            quoted += character;
        }
    }
    return quoted + "'";
}

struct Spawned {
    pid_t pid = -1;
    int stdin_fd = -1;  // -1 when stdin is /dev/null
    int stdout_fd = -1;
};

// Starts `argv` in its own process group with stdout on a pipe, stdin on a
// pipe or /dev/null, stderr to `stderrPath` or /dev/null, an empty signal
// mask (a dispatch worker's blocked signals would otherwise survive exec and
// make the agent deaf to SIGTERM), and none of this process's descriptors
// but those three where the platform allows it -- the project lock among
// them must not outlive the app in an orphaned agent.
bool SpawnPiped(const std::vector<std::string>& argv,
                const Environment& environment, bool pipeStdin,
                const std::string& stderrPath, Spawned& spawned,
                std::string& error) {
    int inPipe[2] = {-1, -1};
    int outPipe[2] = {-1, -1};
    const auto closeAll = [&]() {
        for (int fd : {inPipe[0], inPipe[1], outPipe[0], outPipe[1]}) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
    };
    if ((pipeStdin && ::pipe(inPipe) != 0) || ::pipe(outPipe) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        closeAll();
        return false;
    }
    if (pipeStdin) {
        ::fcntl(inPipe[1], F_SETFD, FD_CLOEXEC);
    }
    ::fcntl(outPipe[0], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (pipeStdin) {
        posix_spawn_file_actions_adddup2(&actions, inPipe[0], STDIN_FILENO);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0);
    }
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(
        &actions, STDERR_FILENO,
        stderrPath.empty() ? "/dev/null" : stderrPath.c_str(),
        O_WRONLY | O_CREAT | O_APPEND, 0600);
#ifndef POSIX_SPAWN_CLOEXEC_DEFAULT
    if (pipeStdin) {
        posix_spawn_file_actions_addclose(&actions, inPipe[0]);
    }
    posix_spawn_file_actions_addclose(&actions, outPipe[1]);
#endif

    posix_spawnattr_t attributes;
    posix_spawnattr_init(&attributes);
    short flags =
        POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
    flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
    posix_spawnattr_setflags(&attributes, flags);
    posix_spawnattr_setpgroup(&attributes, 0);
    sigset_t noSignals;
    sigemptyset(&noSignals);
    posix_spawnattr_setsigmask(&attributes, &noSignals);
    sigset_t defaults;
    sigemptyset(&defaults);
    sigaddset(&defaults, SIGPIPE);
    sigaddset(&defaults, SIGTERM);
    sigaddset(&defaults, SIGINT);
    posix_spawnattr_setsigdefault(&attributes, &defaults);

    std::vector<char*> arguments;
    for (const std::string& argument : argv) {
        arguments.push_back(const_cast<char*>(argument.c_str()));
    }
    arguments.push_back(nullptr);
    std::vector<std::string> variables;
    variables.reserve(environment.size());
    for (const auto& [name, value] : environment) {
        variables.push_back(name + "=" + value);
    }
    std::vector<char*> envp;
    for (std::string& variable : variables) {
        envp.push_back(variable.data());
    }
    envp.push_back(nullptr);

    pid_t pid = -1;
    const int status = ::posix_spawn(&pid, arguments[0], &actions, &attributes,
                                     arguments.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attributes);
    if (status != 0) {
        error = "cannot start " + argv[0] + ": " + std::strerror(status);
        closeAll();
        return false;
    }
    if (pipeStdin) {
        ::close(inPipe[0]);
    }
    ::close(outPipe[1]);
    spawned.pid = pid;
    spawned.stdin_fd = pipeStdin ? inPipe[1] : -1;
    spawned.stdout_fd = outPipe[0];
    return true;
}

// Waits up to `graceMs` for the group leader, then kills whatever is left
// of its group and reaps the leader.
void EndProcessGroup(pid_t pid, int graceMs) {
    if (pid <= 0) {
        return;
    }
    int status = 0;
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(graceMs);
    bool reaped = false;
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t done = ::waitpid(pid, &status, WNOHANG);
        if (done == pid || (done < 0 && errno != EINTR)) {
            reaped = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ::kill(-pid, SIGKILL);
    if (!reaped) {
        while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
}

bool CaptureLoginShell(Environment& environment, std::string& why) {
    std::string shell;
    if (const char* fromEnvironment = std::getenv("SHELL")) {
        shell = fromEnvironment;
    }
    if (shell.empty()) {
        if (const passwd* entry = ::getpwuid(::getuid());
            entry != nullptr && entry->pw_shell != nullptr) {
            shell = entry->pw_shell;
        }
    }
    if (shell.empty()) {
        shell = "/bin/zsh";
    }
    const std::string marker =
        "__CUTMACHINE_ENV_" + std::to_string(::getpid()) + "__";
    // Interactive as well as login: nvm is set up in ~/.zshrc, which a
    // login shell alone does not read. Whatever the startup files print
    // lands before the first marker and is ignored.
    const std::vector<std::string> argv = {
        shell, "-l", "-i", "-c",
        "printf '%s' " + marker + "; /usr/bin/env -0; printf '%s' " + marker};
    Spawned spawned;
    if (!SpawnPiped(argv, CurrentEnvironment(), false, {}, spawned, why)) {
        return false;
    }
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(kShellTimeoutMs);
    bool complete = false;
    while (!complete) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                              deadline - std::chrono::steady_clock::now())
                              .count();
        if (left <= 0) {
            break;
        }
        pollfd readable{spawned.stdout_fd, POLLIN, 0};
        const int ready = ::poll(&readable, 1, static_cast<int>(left));
        if (ready < 0 && errno == EINTR) {
            continue;
        }
        if (ready <= 0) {
            break;
        }
        char chunk[16384];
        const ssize_t count = ::read(spawned.stdout_fd, chunk, sizeof(chunk));
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        output.append(chunk, static_cast<size_t>(count));
        const size_t first = output.find(marker);
        complete =
            first != std::string::npos &&
            output.find(marker, first + marker.size()) != std::string::npos;
    }
    ::close(spawned.stdout_fd);
    ::kill(-spawned.pid, SIGTERM);
    EndProcessGroup(spawned.pid, 500);
    if (!complete) {
        why = shell + " did not print its environment within " +
              std::to_string(kShellTimeoutMs / 1000) + " s";
        return false;
    }
    if (!ParseShellEnvironment(output, marker, environment)) {
        why = shell + " printed an environment that could not be read";
        return false;
    }
    return true;
}

}  // namespace

bool ParseShellEnvironment(const std::string& output, const std::string& marker,
                           Environment& environment) {
    const size_t first = output.find(marker);
    if (marker.empty() || first == std::string::npos) {
        return false;
    }
    const size_t begin = first + marker.size();
    const size_t end = output.find(marker, begin);
    if (end == std::string::npos) {
        return false;
    }
    environment.clear();
    size_t position = begin;
    while (position < end) {
        size_t stop = output.find('\0', position);
        if (stop == std::string::npos || stop > end) {
            stop = end;
        }
        const std::string entry = output.substr(position, stop - position);
        const size_t equals = entry.find('=');
        if (equals != std::string::npos && equals > 0) {
            environment.emplace_back(entry.substr(0, equals),
                                     entry.substr(equals + 1));
        }
        position = stop + 1;
    }
    return true;
}

Environment CurrentEnvironment() {
    Environment environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr;
         ++entry) {
        const std::string text(*entry);
        const size_t equals = text.find('=');
        if (equals != std::string::npos && equals > 0) {
            environment.emplace_back(text.substr(0, equals),
                                     text.substr(equals + 1));
        }
    }
    return environment;
}

void StripNestedAgentVariables(Environment& environment) {
    environment.erase(
        std::remove_if(environment.begin(), environment.end(),
                       [](const auto& variable) {
                           for (const char* name : kNestedAgentVariables) {
                               if (variable.first == name) {
                                   return true;
                               }
                           }
                           return false;
                       }),
        environment.end());
}

Environment AgentEnvironment(std::string* note) {
    Environment environment;
    std::string why;
    if (!CaptureLoginShell(environment, why)) {
        environment = CurrentEnvironment();
        if (note != nullptr) {
            *note = why;
        }
    }
    StripNestedAgentVariables(environment);
    return environment;
}

std::unique_ptr<AcpProcessTransport> AcpProcessTransport::Start(
    const AcpProcessOptions& options, std::string& error) {
    if (options.command.empty()) {
        error = "aucune commande d'agent";
        return nullptr;
    }
    // The command on a line of its own, so that a list ("FOO=1 npx ...",
    // "a; b") runs whole; the shell stays in the agent's process group.
    std::string script;
    if (!options.cwd.empty()) {
        script = "cd " + ShellQuote(options.cwd) + " || exit 127\n";
    }
    script += options.command;
    Spawned spawned;
    if (!SpawnPiped({"/bin/sh", "-c", script}, options.environment, true,
                    options.stderr_path, spawned, error)) {
        return nullptr;
    }
    for (int fd : {spawned.stdin_fd, spawned.stdout_fd}) {
        const int flags = ::fcntl(fd, F_GETFL);
        if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            error = std::string("agent pipe: ") + std::strerror(errno);
            ::close(spawned.stdin_fd);
            ::close(spawned.stdout_fd);
            ::kill(-spawned.pid, SIGTERM);
            EndProcessGroup(spawned.pid, 2000);
            return nullptr;
        }
    }
#ifdef F_SETNOSIGPIPE
    // An agent that died between two messages must fail the next write,
    // not kill the app with SIGPIPE.
    ::fcntl(spawned.stdin_fd, F_SETNOSIGPIPE, 1);
#else
    ::signal(SIGPIPE, SIG_IGN);
#endif
    return std::make_unique<AcpProcessTransport>(
        spawned.pid, spawned.stdin_fd, spawned.stdout_fd, options.stderr_path);
}

AcpProcessTransport::AcpProcessTransport(pid_t pid, int stdinFd, int stdoutFd,
                                         std::string stderrPath)
    : pid_(pid),
      stdin_fd_(stdinFd),
      stdout_fd_(stdoutFd),
      stderr_path_(std::move(stderrPath)),
      writer_([this] { WritePendingLines(); }) {}

AcpProcessTransport::~AcpProcessTransport() {
    CloseInputAndSignal();
    writer_.join();
    ::close(stdin_fd_);
    // Close interrupts both I/O loops without waiting for the agent. The
    // worker destroying the transport gives its group a moment to finish.
    EndProcessGroup(pid_, 2000);
    if (stdout_fd_ >= 0) {
        ::close(stdout_fd_);
    }
}

bool AcpProcessTransport::WriteLine(const std::string& line,
                                    std::string& error) {
    return QueueLine(line, error, false);
}

bool AcpProcessTransport::WriteCancellation(const std::string& line,
                                            std::string& error) {
    return QueueLine(line, error, true);
}

bool AcpProcessTransport::QueueLine(const std::string& line, std::string& error,
                                    bool cancellation) {
    const std::lock_guard<std::mutex> lock(write_mutex_);
    if (closed_) {
        error = "l'agent est fermé";
        return false;
    }
    writes_.push_back({line + '\n', cancellation});
    if (cancellation &&
        cancellation_deadline_ == std::chrono::steady_clock::time_point{}) {
        cancellation_deadline_ =
            std::chrono::steady_clock::now() + std::chrono::seconds(2);
    }
    pending_write_.notify_one();
    return true;
}

void AcpProcessTransport::WritePendingLines() {
    while (!closed_) {
        PendingLine pending;
        {
            std::unique_lock<std::mutex> lock(write_mutex_);
            pending_write_.wait(lock,
                                [this] { return closed_ || !writes_.empty(); });
            if (closed_) {
                return;
            }
            pending = std::move(writes_.front());
            writes_.pop_front();
        }
        const std::string& data = pending.bytes;
        size_t written = 0;
        while (!closed_ && written < data.size()) {
            bool expired = false;
            {
                const std::lock_guard<std::mutex> lock(write_mutex_);
                expired =
                    cancellation_deadline_ !=
                        std::chrono::steady_clock::time_point{} &&
                    std::chrono::steady_clock::now() >= cancellation_deadline_;
            }
            if (expired) {
                CloseInputAndSignal();
                return;
            }
            pollfd writable{stdin_fd_, POLLOUT, 0};
            const int ready = ::poll(&writable, 1, 100);
            if (closed_) {
                return;
            }
            if (ready == 0 || (ready < 0 && errno == EINTR)) {
                continue;
            }
            if (ready < 0 ||
                (writable.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                CloseInputAndSignal();
                return;
            }
            const ssize_t count = ::write(stdin_fd_, data.data() + written,
                                          data.size() - written);
            if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            if (count <= 0) {
                CloseInputAndSignal();
                return;
            }
            written += static_cast<size_t>(count);
        }
        if (pending.cancellation) {
            const std::lock_guard<std::mutex> lock(write_mutex_);
            if (std::none_of(writes_.begin(), writes_.end(),
                             [](const PendingLine& line) {
                                 return line.cancellation;
                             })) {
                cancellation_deadline_ = {};
            }
        }
    }
}

bool AcpProcessTransport::TakeBufferedLine(std::string& line,
                                           size_t searchFrom) {
    const size_t newline = read_buffer_.find('\n', searchFrom);
    if (newline == std::string::npos) {
        return false;
    }
    line.assign(read_buffer_, 0, newline);
    read_buffer_.erase(0, newline + 1);
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    return true;
}

bool AcpProcessTransport::ReadLine(std::string& line) {
    size_t searchFrom = 0;
    while (!closed_) {
        if (TakeBufferedLine(line, searchFrom)) {
            return true;
        }
        searchFrom = read_buffer_.size();
        if (stdout_fd_ < 0) {
            return false;
        }
        pollfd readable{stdout_fd_, POLLIN, 0};
        const int ready = ::poll(&readable, 1, 100);
        if (closed_) {
            break;
        }
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0) {
            return false;
        }
        char chunk[65536];
        const ssize_t count = ::read(stdout_fd_, chunk, sizeof(chunk));
        if (count > 0) {
            read_buffer_.append(chunk, static_cast<size_t>(count));
            continue;
        }
        if (count < 0 && (errno == EINTR || errno == EAGAIN)) {
            continue;
        }
        // End of the agent's output: a last line without its newline still
        // counts once.
        if (!read_buffer_.empty()) {
            line = std::move(read_buffer_);
            read_buffer_.clear();
            return true;
        }
        return false;
    }
    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. Closed (Stop while starting,
    // an undeliverable session/cancel, a broken pipe): nothing more is waited
    // for, but what the agent already wrote is still the turn's. Its final
    // answer may be in the buffer or still in the pipe. The pipe is emptied
    // once, without blocking and within a bound, so an agent deaf to SIGTERM
    // that keeps writing cannot hold the reader; then only complete lines
    // are returned. A tail without its newline may have been cut by the
    // signal Close sent, and is dropped.
    if (!drained_after_close_ && stdout_fd_ >= 0) {
        drained_after_close_ = true;
        size_t budget = kDrainAfterCloseBytes;
        char chunk[65536];
        while (budget > 0) {
            const ssize_t count =
                ::read(stdout_fd_, chunk, std::min(sizeof(chunk), budget));
            if (count > 0) {
                read_buffer_.append(chunk, static_cast<size_t>(count));
                budget -= static_cast<size_t>(count);
                continue;
            }
            if (count < 0 && errno == EINTR) {
                continue;
            }
            break;  // empty for now (EAGAIN), ended, or failed
        }
    }
    return TakeBufferedLine(line, 0);
}

void AcpProcessTransport::Close() { CloseInputAndSignal(); }

void AcpProcessTransport::CloseInputAndSignal() {
    {
        const std::lock_guard<std::mutex> lock(write_mutex_);
        if (closed_.exchange(true)) {
            return;
        }
    }
    pending_write_.notify_one();
    if (pid_ > 0) {
        ::kill(-pid_, SIGTERM);
    }
}

}  // namespace acp
