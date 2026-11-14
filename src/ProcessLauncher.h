#pragma once

// PRD1 -- docs/roadmap/PRODUIT-2026-10.md. The one place the engine starts
// another program.
//
// Twelve files each carried their own copy of the same posix_spawnp launch:
// two pipes, a poll loop, a SIGTERM that turns into a SIGKILL after two
// seconds, a bounded tail of the error output. The copies had drifted in
// small ways (the size of the tail, the size of a read, whether the error
// output was kept at all) and shared their defects: a pipe an unrelated
// child inherited, a loop that spun on a descriptor it had just closed.
// One launcher means one place to fix either, and one place PRD2 can refuse
// a launch from.
//
// What a caller still owns is what differed between the copies and matters
// to it: what it does with each chunk of standard output, when it considers
// itself cancelled, how much of the error output it keeps, and the words of
// its own error messages. The launcher reports what happened and leaves the
// sentence to the caller.
//
// The program is always an argument vector. Nothing here goes through a
// shell, so a path with a space or a quote in it is one argument.
//
// Plain C++ and POSIX: no AppKit, usable from the model library.

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace process {

// Where one of the child's standard streams goes.
enum class Stream {
    // The launching process's own stream: the child writes to the terminal.
    kInherit,
    // Read by the launcher: standard output is handed to `on_output` chunk
    // by chunk, standard error is kept as a bounded tail.
    kPipe,
    // /dev/null.
    kDiscard,
};

struct Request {
    // The program first, then its arguments. A program without a slash is
    // looked up on PATH, as posix_spawnp does.
    std::vector<std::string> arguments;
    Stream standard_output = Stream::kInherit;
    Stream standard_error = Stream::kInherit;
    // FI10 -- docs/roadmap/FIABILITE-2026-10.md. Batch scripts such as
    // MFA must not read from the application's terminal.
    bool discard_input = false;
    // The most one read takes from either pipe, so the most `on_output`
    // receives at once. It sets how often a caller reporting progress per
    // chunk reports it.
    size_t read_bytes = 4096;
    // How much of the end of the error output is kept. The end is where a
    // program says why it failed.
    size_t error_tail_bytes = 8192;
    // Called once the program has started, before anything is read from
    // it. A launch that fails never calls it.
    std::function<void()> on_started;
    // Each chunk read from standard output, in order, on the calling
    // thread. Only called when `standard_output` is kPipe.
    std::function<void(const char* bytes, size_t count)> on_output;
    // Polled at least every 100 ms while the child runs. The first true
    // sends SIGTERM to the whole process group; after two seconds any
    // remaining members get SIGKILL. Descendants are stopped too.
    // Unset: the child is never interrupted.
    std::function<bool()> cancelled;
};

// How far a launch got.
enum class Stage {
    // Declined before anything was started; `refusal` says why. Run() itself
    // never returns this: it is for a caller that checks something first
    // (FfmpegBinary.h) and answers with the same type.
    kRefused,
    // pipe(2) failed; `system_error` is its errno.
    kPipes,
    // The program could not be started; `system_error` is posix_spawnp's.
    kSpawn,
    // The program ran and was waited for; `status` is how it ended.
    kRan,
};

struct Result {
    Stage stage = Stage::kRan;
    std::string refusal;
    int system_error = 0;
    // As waitpid(2) returned it. Read it through the accessors below.
    int status = 0;
    // The launcher saw `cancelled` return true and signalled the child.
    bool cancelled = false;
    // The last `error_tail_bytes` of standard error, when it was piped.
    std::string error_tail;

    bool Started() const { return stage == Stage::kRan; }
    // Ran to its end and exited with status 0.
    bool Succeeded() const;
    bool Signalled() const;
    // Meaningful when Signalled().
    int TerminationSignal() const;
    // Meaningful when the program ran and was not signalled.
    int ExitStatus() const;
    // strerror(system_error).
    std::string SystemErrorText() const;
};

// Starts the program, feeds `on_output`, and returns once it has been
// waited for. Blocks the calling thread for the whole run.
Result Run(const Request& request);

// DEC39 -- docs/roadmap/PHASES-2026-08.md. Starts the program and returns
// at once, never waiting for it: for a caller about to exit that leaves a
// program running after it, the editor reopening on another project. The
// child leads its own process group, so neither SignalRunningGroups nor a
// signal sent to this process's group reaches it, and it reads /dev/null;
// its output goes where this process's goes. No descriptor of this process
// reaches it but the three standard ones: on macOS by
// POSIX_SPAWN_CLOEXEC_DEFAULT, which Linux does not have (there, a
// descriptor opened without FD_CLOEXEC is inherited). `stage` is kRan once
// started, `status` stays 0; nobody waits for the child, so a caller that
// lives on would leave it a zombie when it ends.
Result StartDetached(const std::vector<std::string>& arguments);

// FI9 -- docs/roadmap/FIABILITE-2026-10.md. Each child leads its own process
// group, so the Ctrl-C a terminal sends to the engine's group no longer
// reaches it: a stopped command left its encoder running. This passes
// `signalNumber` to every group a Run() has started and not yet waited for.
// Async-signal-safe: the stop handler calls it (EngineLog.cc).
void SignalRunningGroups(int signalNumber);

}  // namespace process
