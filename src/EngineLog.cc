#include "EngineLog.h"

#include "BuildInfo.h"
#include "ProcessLauncher.h"
#include "UserDirectories.h"

#include <fcntl.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <memory>
#include <utility>

namespace engine_log {
namespace {

// ---------------------------------------------------------------------
// Numbers and time written by hand into a caller's buffer: the crash line
// is built with this same code inside a signal handler, where neither
// gmtime nor snprintf may be called and nothing may be allocated.
// ---------------------------------------------------------------------

// `value` as exactly `width` digits, zero-padded.
size_t PutDigits(char* out, uint64_t value, size_t width) {
    for (size_t index = width; index > 0; --index) {
        out[index - 1] = static_cast<char>('0' + value % 10);
        value /= 10;
    }
    return width;
}

// `value` with as many digits as it needs.
size_t PutNumber(char* out, uint64_t value) {
    char reversed[20];
    size_t count = 0;
    do {
        reversed[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value != 0);
    for (size_t index = 0; index < count; ++index) {
        out[index] = reversed[count - 1 - index];
    }
    return count;
}

size_t PutText(char* out, const char* text, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        out[index] = text[index];
    }
    return length;
}

// Days since 1970-01-01 as a civil date of the proleptic Gregorian
// calendar (the public-domain days-to-civil algorithm).
void CivilFromDays(int64_t days, int64_t& year, unsigned& month,
                   unsigned& day) {
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const auto dayOfEra = static_cast<unsigned>(days - era * 146097);
    const unsigned yearOfEra =
        (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) /
        365;
    const unsigned dayOfYear =
        dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
    const unsigned monthIndex = (5 * dayOfYear + 2) / 153;
    day = dayOfYear - (153 * monthIndex + 2) / 5 + 1;
    month = monthIndex < 10 ? monthIndex + 3 : monthIndex - 9;
    year = static_cast<int64_t>(yearOfEra) + era * 400 + (month <= 2 ? 1 : 0);
}

// "2026-10-03T13:45:12.345Z" into `out`, 24 characters.
constexpr size_t kTimeLength = 24;
size_t PutUtcTime(char* out, int64_t unixMilliseconds) {
    int64_t seconds = unixMilliseconds / 1000;
    int64_t milliseconds = unixMilliseconds % 1000;
    if (milliseconds < 0) {
        milliseconds += 1000;
        --seconds;
    }
    int64_t days = seconds / 86400;
    int64_t secondOfDay = seconds % 86400;
    if (secondOfDay < 0) {
        secondOfDay += 86400;
        --days;
    }
    int64_t year = 0;
    unsigned month = 1;
    unsigned day = 1;
    CivilFromDays(days, year, month, day);
    year = year < 0 ? 0 : (year > 9999 ? 9999 : year);
    size_t length = PutDigits(out, static_cast<uint64_t>(year), 4);
    out[length++] = '-';
    length += PutDigits(out + length, month, 2);
    out[length++] = '-';
    length += PutDigits(out + length, day, 2);
    out[length++] = 'T';
    length +=
        PutDigits(out + length, static_cast<uint64_t>(secondOfDay / 3600), 2);
    out[length++] = ':';
    length += PutDigits(out + length,
                        static_cast<uint64_t>(secondOfDay / 60 % 60), 2);
    out[length++] = ':';
    length +=
        PutDigits(out + length, static_cast<uint64_t>(secondOfDay % 60), 2);
    out[length++] = '.';
    length += PutDigits(out + length, static_cast<uint64_t>(milliseconds), 3);
    out[length++] = 'Z';
    return length;
}

int64_t NowUnixMilliseconds() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

int OpenForAppend(const std::string& path) {
    // 0600: the log says what this user did and when; nobody else needs it.
    return ::open(path.c_str(), O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC,
                  0600);
}

bool IsTokenCharacter(char character) {
    return (character >= 'a' && character <= 'z') ||
           (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_' ||
           character == '-' || character == '.';
}

// ---------------------------------------------------------------------
// The process's log. `g_active` is read without the lock by every tool
// call; the rest is guarded by the state's mutex. The state is never
// destroyed: a relay or HTTP thread may still finish a call while static
// destructors run at exit, and must not find a destroyed mutex.
// ---------------------------------------------------------------------

std::atomic<bool> g_active{false};

struct ProcessState {
    std::mutex mutex;
    std::unique_ptr<LogFile> file;
    std::string surface;
    bool stopped = false;
};

ProcessState& State() {
    static auto* state = new ProcessState();
    return *state;
}

// ---------------------------------------------------------------------
// Signal handlers. Everything they read is written before they are
// installed, once per process, and never changes after.
// ---------------------------------------------------------------------

// What kills a process with a crash report: a "crash" line.
constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE};
// What stops it without one -- SIGTERM is how an MCP client ends the stdio
// server it spawned when its session closes: a "stop" line, or a server
// would only ever be seen to start. The HTTP server catches SIGINT and
// SIGTERM itself, after this, to stop cleanly and write its stop line.
constexpr int kStopSignals[] = {SIGTERM, SIGINT, SIGHUP};
constexpr size_t kFatalSignalCount =
    sizeof(kFatalSignals) / sizeof(kFatalSignals[0]);
constexpr size_t kStopSignalCount =
    sizeof(kStopSignals) / sizeof(kStopSignals[0]);
constexpr size_t kSignalCount = kFatalSignalCount + kStopSignalCount;
struct sigaction g_previous_actions[kSignalCount];
std::terminate_handler g_previous_terminate = nullptr;
std::atomic<bool> g_crash_handlers_installed{false};
std::atomic<bool> g_crash_written{false};
std::atomic<int> g_crash_descriptor{-1};
// " surface=<surface> ", prepared when the handlers are installed.
char g_crash_surface[96];
size_t g_crash_surface_length = 0;
// A stack overflow raises SIGSEGV with no stack left to run a handler on:
// the installing thread gets this one, unless it already has its own.
alignas(16) char g_alternate_stack[64 * 1024];

int HandledSignal(size_t index) {
    return index < kFatalSignalCount ? kFatalSignals[index]
                                     : kStopSignals[index - kFatalSignalCount];
}

// The line a signal or std::terminate leaves, built on the stack and
// written with async-signal-safe calls only -- clock_gettime, getpid and
// write are all on POSIX's list. Once per process: the first event writes
// it, and the SIGABRT that the terminate handler's abort() raises next
// adds nothing.
void WriteCrashLine(const char* what, int signalNumber) {
    if (g_crash_written.exchange(true)) {
        return;
    }
    const int descriptor = g_crash_descriptor.load();
    if (descriptor < 0) {
        return;
    }
    char line[kTimeLength + 32 + sizeof(g_crash_surface) + 32];
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    size_t length = PutUtcTime(
        line, static_cast<int64_t>(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
    static const char kPid[] = " pid=";
    length += PutText(line + length, kPid, sizeof(kPid) - 1);
    length += PutNumber(line + length, static_cast<uint64_t>(::getpid()));
    length += PutText(line + length, g_crash_surface, g_crash_surface_length);
    length += PutText(line + length, what, std::strlen(what));
    if (signalNumber >= 0) {
        length += PutNumber(line + length, static_cast<uint64_t>(signalNumber));
    }
    line[length++] = '\n';
    const ssize_t written = ::write(descriptor, line, length);
    (void)written;
}

void SignalHandler(int signalNumber) {
    const int savedErrno = errno;
    bool fatal = false;
    for (const int fatalSignal : kFatalSignals) {
        fatal = fatal || fatalSignal == signalNumber;
    }
    WriteCrashLine(fatal ? "crash signal=" : "stop signal=", signalNumber);
    // FI9 -- docs/roadmap/FIABILITE-2026-10.md. A stopped engine stops what
    // it started: its children lead their own process groups, which the
    // terminal's Ctrl-C does not reach.
    if (!fatal) {
        process::SignalRunningGroups(signalNumber);
    }
    // The signal goes back to what was installed before -- the default
    // action, for which the system writes its crash report as it always did
    // -- and is raised again: blocked while this handler runs, it is
    // delivered as soon as the handler returns. A fault is delivered on the
    // thread that took it, which still holds the exception the report names.
    for (size_t index = 0; index < kSignalCount; ++index) {
        if (HandledSignal(index) == signalNumber) {
            ::sigaction(signalNumber, &g_previous_actions[index], nullptr);
            break;
        }
    }
    ::raise(signalNumber);
    errno = savedErrno;
}

[[noreturn]] void CrashTerminateHandler() {
    WriteCrashLine("crash terminate", -1);
    if (g_previous_terminate != nullptr) {
        g_previous_terminate();
    }
    std::abort();
}

void InstallCrashHandlers(const std::string& surface, int descriptor) {
    if (g_crash_handlers_installed.exchange(true)) {
        return;
    }
    std::string prepared = " surface=" + LogToken(surface) + " ";
    if (prepared.size() > sizeof(g_crash_surface)) {
        prepared = " surface=? ";
    }
    g_crash_surface_length =
        PutText(g_crash_surface, prepared.data(), prepared.size());
    g_crash_descriptor.store(descriptor);

    stack_t current{};
    if (::sigaltstack(nullptr, &current) == 0 &&
        (current.ss_flags & SS_DISABLE) != 0) {
        stack_t alternate{};
        alternate.ss_sp = g_alternate_stack;
        alternate.ss_size = sizeof(g_alternate_stack);
        alternate.ss_flags = 0;
        ::sigaltstack(&alternate, nullptr);
    }
    struct sigaction action {};
    action.sa_handler = SignalHandler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_ONSTACK;
    for (size_t index = 0; index < kSignalCount; ++index) {
        ::sigaction(HandledSignal(index), &action, &g_previous_actions[index]);
    }
    g_previous_terminate = std::set_terminate(CrashTerminateHandler);
}

}  // namespace

std::string LogDirectoryFromEnvironment() {
    const char* enabled = std::getenv("CUTMACHINE_LOG");
    if (enabled != nullptr && std::strcmp(enabled, "0") == 0) {
        return {};
    }
    const char* directory = std::getenv("CUTMACHINE_LOG_DIR");
    if (directory != nullptr && *directory != '\0') {
        return directory;
    }
    // HEADLESS-2026-10 H4 -- ~/Library/Logs on macOS (UserDirectories.h).
    return user_directories::Logs();
}

std::string FormatUtcTime(int64_t unixMilliseconds) {
    char buffer[kTimeLength];
    const size_t length = PutUtcTime(buffer, unixMilliseconds);
    return std::string(buffer, length);
}

std::string LogToken(const std::string& text) {
    if (text.empty() || text.size() > 64) {
        return "?";
    }
    for (const char character : text) {
        if (!IsTokenCharacter(character)) {
            return "?";
        }
    }
    return text;
}

std::string ToolCallEvent(const std::string& tool, const std::string& errorName,
                          int64_t milliseconds, const std::string& timelineId) {
    std::string event = "call tool=" + LogToken(tool);
    event += errorName.empty() ? std::string(" ok")
                               : " error=" + LogToken(errorName);
    event += " ms=" + std::to_string(milliseconds < 0 ? 0 : milliseconds);
    if (!timelineId.empty()) {
        event += " timeline=" + LogToken(timelineId);
    }
    return event;
}

std::string LogLine(int64_t unixMilliseconds, long pid,
                    const std::string& surface, const std::string& event) {
    return FormatUtcTime(unixMilliseconds) + " pid=" + std::to_string(pid) +
           " surface=" + LogToken(surface) + " " + event + "\n";
}

LogFile::LogFile(std::string directory, uint64_t rotateBytes)
    : directory_(std::move(directory)), rotate_bytes_(rotateBytes) {}

LogFile::~LogFile() {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
    }
}

std::string LogFile::Path() const {
    return (std::filesystem::path(directory_) / kLogFileName).string();
}

bool LogFile::Open(std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (descriptor_ >= 0) {
        return true;
    }
    std::error_code error;
    std::filesystem::create_directories(directory_, error);
    if (error) {
        message = "cannot create '" + directory_ + "': " + error.message();
        return false;
    }
    descriptor_ = OpenForAppend(Path());
    if (descriptor_ < 0) {
        message = "cannot open '" + Path() + "': " + std::strerror(errno);
        return false;
    }
    return true;
}

bool LogFile::Append(const std::string& line) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (descriptor_ < 0) {
        return false;
    }
    struct stat status {};
    if (::fstat(descriptor_, &status) == 0) {
        if (status.st_nlink == 0) {
            // Deleted under us: lines written to it would be lost.
            ReopenLocked();
        } else if (static_cast<uint64_t>(status.st_size) >= rotate_bytes_) {
            RotateLocked();
        }
    }
    ssize_t written = 0;
    do {
        written = ::write(descriptor_, line.data(), line.size());
    } while (written < 0 && errno == EINTR);
    return written == static_cast<ssize_t>(line.size());
}

// Every process that writes the file sees it full at the same time. The
// rename is decided under a lock on the file being replaced, and only if
// the name still stands for it: the second process to get the lock finds
// the name taken by a new file, or by none yet, and only reopens -- it
// never renames the fresh file over the one just set aside.
void LogFile::RotateLocked() {
    struct stat ours {};
    if (::fstat(descriptor_, &ours) != 0) {
        return;
    }
    const std::string path = Path();
    ::flock(descriptor_, LOCK_EX);
    struct stat named {};
    if (::stat(path.c_str(), &named) == 0 && named.st_dev == ours.st_dev &&
        named.st_ino == ours.st_ino) {
        ::rename(path.c_str(), (path + ".1").c_str());
    }
    ::flock(descriptor_, LOCK_UN);
    ReopenLocked();
}

bool LogFile::ReopenLocked() {
    std::error_code ignored;
    std::filesystem::create_directories(directory_, ignored);
    const int fresh = OpenForAppend(Path());
    if (fresh < 0) {
        return false;
    }
    // dup2 keeps the descriptor's number, the one a crash handler holds,
    // and clears close-on-exec, set again below.
    const bool moved = ::dup2(fresh, descriptor_) >= 0;
    ::close(fresh);
    if (moved) {
        ::fcntl(descriptor_, F_SETFD, FD_CLOEXEC);
    }
    return moved;
}

bool StartProcessLog(const std::string& surface, bool crashHandlers) {
    ProcessState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.file) {
        return true;
    }
    const std::string directory = LogDirectoryFromEnvironment();
    if (directory.empty()) {
        return false;
    }
    auto file = std::make_unique<LogFile>(directory);
    std::string message;
    if (!file->Open(message)) {
        std::fprintf(stderr, "cutmachine: no engine log: %s\n",
                     message.c_str());
        return false;
    }
    state.surface = LogToken(surface);
    state.stopped = false;
    const BuildInfo& build = CurrentBuild();
    file->Append(LogLine(
        NowUnixMilliseconds(), static_cast<long>(::getpid()), state.surface,
        "start version=" + LogToken(build.version) + " commit=" +
            LogToken(build.commit) + (build.dirty ? "-dirty" : "")));
    if (crashHandlers) {
        InstallCrashHandlers(state.surface, file->Descriptor());
    }
    state.file = std::move(file);
    g_active.store(true);
    return true;
}

void StopProcessLog() {
    ProcessState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.file || state.stopped) {
        return;
    }
    state.file->Append(LogLine(NowUnixMilliseconds(),
                               static_cast<long>(::getpid()), state.surface,
                               "stop"));
    state.stopped = true;
    // A crash while the process winds down still finds its descriptor
    // open, and a call that ends meanwhile still gets its line.
    if (g_crash_handlers_installed.load()) {
        return;
    }
    g_active.store(false);
    state.file.reset();
}

bool Active() { return g_active.load(); }

void RecordToolCall(const std::string& tool, const std::string& errorName,
                    Clock::time_point started, const std::string& timelineId) {
    if (!g_active.load()) {
        return;
    }
    const int64_t milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() -
                                                              started)
            .count();
    const std::string event =
        ToolCallEvent(tool, errorName, milliseconds, timelineId);
    ProcessState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (!state.file) {
        return;
    }
    state.file->Append(LogLine(NowUnixMilliseconds(),
                               static_cast<long>(::getpid()), state.surface,
                               event));
}

std::string ProcessLogPath() {
    ProcessState& state = State();
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.file ? state.file->Path() : std::string();
}

}  // namespace engine_log
