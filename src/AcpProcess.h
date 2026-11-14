#pragma once

// ACP2 -- docs/roadmap/ACP-2026-09.md. The agent as a child process: its
// stdin and stdout carry the protocol (AcpClient.h), its stderr goes to a
// log file, and it runs in its own process group so that stopping it also
// stops what it started (npx starts node, which starts the agent's CLI).
//
// An app opened from the Finder inherits launchd's PATH, which has neither
// Homebrew nor nvm, so `npx` is not found. The agent is started with the
// environment of the person's login shell instead, read once per start;
// if that fails, with the app's own.
//
// POSIX only (posix_spawn, pipes, process groups), no AppKit.

#include "AcpClient.h"

#include <sys/types.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace acp {

using Environment = std::vector<std::pair<std::string, std::string>>;

// The variables between the two `marker`s of `output`, as `env -0` prints
// them (NAME=value, each followed by a NUL). False if a marker is missing.
bool ParseShellEnvironment(const std::string& output, const std::string& marker,
                           Environment& environment);

// This process's environment.
Environment CurrentEnvironment();

// Removes the variables by which Claude Code recognises that it runs inside
// another Claude Code session: the app may have been launched from one, and
// the agent it starts is not nested in it.
void StripNestedAgentVariables(Environment& environment);

// What an agent is started with: the login shell's environment, else this
// process's, less the nested-session variables. `note`, if given, says why
// the shell's environment was not used.
Environment AgentEnvironment(std::string* note = nullptr);

struct AcpProcessOptions {
    // A shell command line, run by /bin/sh: "npx -y @agentclientprotocol/
    // claude-agent-acp", "codex-acp", a path...
    std::string command;
    std::string cwd;
    Environment environment;
    // Where the agent's stderr goes; empty discards it.
    std::string stderr_path;
};

class AcpProcessTransport final : public AcpTransport {
public:
    static std::unique_ptr<AcpProcessTransport> Start(
        const AcpProcessOptions& options, std::string& error);
    // Takes the started process; Start is the way to get one.
    AcpProcessTransport(pid_t pid, int stdinFd, int stdoutFd,
                        std::string stderrPath);
    ~AcpProcessTransport() override;
    AcpProcessTransport(const AcpProcessTransport&) = delete;
    AcpProcessTransport& operator=(const AcpProcessTransport&) = delete;

    // Accepts a line for ordered delivery. A later pipe failure interrupts
    // ReadLine, just like the agent closing its output.
    bool WriteLine(const std::string& line, std::string& error) override;
    bool WriteCancellation(const std::string& line,
                           std::string& error) override;
    // Once closed, returns the complete lines already received -- buffered,
    // or waiting in the pipe at that moment -- then false (FI7).
    bool ReadLine(std::string& line) override;
    void Close() override;
    bool Closed() const override { return closed_; }
    std::string DiagnosticHint() const override { return stderr_path_; }

private:
    // Close's work, callable from the destructor without virtual dispatch.
    void CloseInputAndSignal();
    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. Protocol writes are queued
    // in order: neither sending a large prompt nor its cancellation may
    // block the panel on an agent which stopped reading.
    void WritePendingLines();
    bool QueueLine(const std::string& line, std::string& error,
                   bool cancellation);
    // The first complete line of read_buffer_ whose newline lies at or
    // after `searchFrom`, without it.
    bool TakeBufferedLine(std::string& line, size_t searchFrom);

    // FI7 -- docs/roadmap/FIABILITE-2026-10.md. What ReadLine still takes
    // from the pipe after closing, at most: far more than a final answer,
    // and a bound on an agent that keeps writing after it was told to stop.
    static constexpr size_t kDrainAfterCloseBytes = size_t{8} * 1024 * 1024;

    struct PendingLine {
        std::string bytes;
        bool cancellation = false;
    };

    pid_t pid_ = -1;
    int stdin_fd_ = -1;
    int stdout_fd_ = -1;
    std::string stderr_path_;
    std::string read_buffer_;
    // ReadLine's own (the turn's thread): the pipe is emptied once after
    // closing, never waited on.
    bool drained_after_close_ = false;
    std::mutex write_mutex_;
    std::atomic_bool closed_{false};
    std::condition_variable pending_write_;
    std::deque<PendingLine> writes_;
    // Protected by write_mutex_. Only the delivery of session/cancel is
    // bounded: an ordinary large prompt can take as long as it needs.
    std::chrono::steady_clock::time_point cancellation_deadline_{};
    std::thread writer_;
};

}  // namespace acp
