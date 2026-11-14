#pragma once

// PRD35 -- ROADMAP.md (docs/roadmap/PRODUIT-2026-10b.md). The engine log:
// one line of text per event in ~/Library/Logs/CUTMACHINE/cutmachine.log.
// When a server or the app misbehaved on someone's machine, nothing said
// which binary ran, which tools were called, which one failed, nor that the
// process went down -- the system's crash report named none of it.
//
// Only a server (`--mcp-serve`, stdio or HTTP) and the app keep a log
// (main.mm), each writing its surface, version, commit and pid when it
// starts, a line when it stops, and one line per tool call from the one
// place every surface's calls pass, McpToolRegistry::Call. One-shot CLI
// commands and the test binaries never start it, so they write nothing.
//
// A line carries what identifies an event and never what a call was about:
// no argument, no result. Those hold paths, clients' names and the words of
// the people filmed, and a log is a file that gets attached to a message.
//
// CUTMACHINE_LOG_DIR replaces the folder (CMakeLists.txt points every test
// at the build tree); CUTMACHINE_LOG=0 turns everything off. The file is
// rotated at 5 MB, one older file kept (cutmachine.log.1). Several processes
// write the same file -- each Claude session spawns its own server -- so a
// line is appended by a single write(2) on an O_APPEND descriptor, and a
// rotation is decided under a lock on the file it replaces.
//
// Portable: POSIX only, no AppKit.

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>

namespace engine_log {

using Clock = std::chrono::steady_clock;

// 5 MB, the size at which cutmachine.log becomes cutmachine.log.1.
inline constexpr uint64_t kRotateBytes = uint64_t{5} * 1024 * 1024;
inline constexpr char kLogFileName[] = "cutmachine.log";

// The folder the log goes to, read from the environment: empty when
// CUTMACHINE_LOG is "0", CUTMACHINE_LOG_DIR when it is set and not empty,
// otherwise ~/Library/Logs/CUTMACHINE on macOS and ~/.local/state/cutmachine
// elsewhere ($HOME, else the user database; UserDirectories.h), or empty
// when no home is known.
std::string LogDirectoryFromEnvironment();

// "2026-10-03T13:45:12.345Z": ISO 8601, UTC, to the millisecond. UTC
// because the crash line is timed inside a signal handler, where the time
// zone cannot be read; every line is timed the same way.
std::string FormatUtcTime(int64_t unixMilliseconds);

// `text` as a log value: kept when it is a short identifier (a tool name,
// an error code, a timeline ID), otherwise "?". A caller can name an unknown
// tool, or put anything in timeline_id: that text must neither start a new
// line nor carry a path into the log.
std::string LogToken(const std::string& text);

// The event of a tool call, after the line's time, pid and surface:
// "call tool=<name> ok ms=<n>" or "call tool=<name> error=<code> ms=<n>",
// then " timeline=<id>" when the call named a timeline.
std::string ToolCallEvent(const std::string& tool, const std::string& errorName,
                          int64_t milliseconds, const std::string& timelineId);

// A whole line, newline included: "<time> pid=<pid> surface=<surface>
// <event>\n".
std::string LogLine(int64_t unixMilliseconds, long pid,
                    const std::string& surface, const std::string& event);

// One log file, appended a line at a time. Thread-safe.
class LogFile {
public:
    explicit LogFile(std::string directory,
                     uint64_t rotateBytes = kRotateBytes);
    ~LogFile();
    LogFile(const LogFile&) = delete;
    LogFile& operator=(const LogFile&) = delete;

    // Creates the folder if needed and opens the file for appending.
    bool Open(std::string& message);
    // Appends `line` with one write(2), first rotating the file when it has
    // reached the limit, or reopening it when another process rotated it or
    // someone deleted it. False when nothing could be written.
    bool Append(const std::string& line);
    // The open descriptor, or -1. Its number never changes while the file
    // is open: a rotation moves the new file onto it (dup2), so a crash
    // handler holding the number always writes to a live log.
    int Descriptor() const { return descriptor_; }
    std::string Path() const;

private:
    void RotateLocked();
    bool ReopenLocked();

    std::string directory_;
    uint64_t rotate_bytes_;
    int descriptor_ = -1;
    mutable std::mutex mutex_;
};

// --- The process's log, as a server and the app keep it. ---

// Opens the log of this process and writes its start line: surface,
// version, commit and pid. The surface ("mcp-stdio", "mcp-http", "app") is
// a state of the process, set here once. With `crashHandlers`, also installs
// the handlers that leave a line when the process dies abnormally:
// std::terminate and SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE. Each writes a
// short line ("crash signal=11", "crash terminate") on the open descriptor,
// with async-signal-safe calls only, and then hands the signal back to what
// was there before -- the default action, for which the system writes its
// crash report as it always did. SIGTERM, SIGINT and SIGHUP leave "stop
// signal=15" the same way: SIGTERM is how an MCP client ends the stdio
// server it spawned. main.mm passes true; a test never does in its own
// process, only in a child it forks to crash.
//
// False, and nothing written, when the log is turned off or cannot be
// opened (a line on stderr says why): the process runs on without it.
bool StartProcessLog(const std::string& surface, bool crashHandlers);
// The stop line, once. With crash handlers the log stays open -- a crash
// while the process winds down still leaves its line; without them it is
// closed and the process keeps none again.
void StopProcessLog();
// Whether this process keeps a log: one atomic read, all a process that
// does not pays per tool call.
bool Active();
// One tool call: its name, its error code (empty when it succeeded), when
// it started, and the timeline its arguments named (empty when none).
// Nothing when the process keeps no log.
void RecordToolCall(const std::string& tool, const std::string& errorName,
                    Clock::time_point started, const std::string& timelineId);
// The file this process writes, or empty.
std::string ProcessLogPath();

}  // namespace engine_log
