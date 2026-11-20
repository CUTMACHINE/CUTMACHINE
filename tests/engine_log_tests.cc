// PRD35 -- ROADMAP.md. The engine log (EngineLog.h): the shape of a line,
// where the file goes, its rotation, writers in several processes, and the
// line an abnormal end leaves.
//
// Portable: POSIX only. The crash handlers are never installed in this
// process: each crash is played by a child it forks, which installs a
// stand-in for the default action first (an _exit with a known status) --
// the engine's handler must hand the signal back to it, and no child leaves
// a crash report behind on the machine that runs the suite.

#include "BuildInfo.h"
#include "EngineLog.h"
#include "UserDirectories.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <string>
#include <vector>

#include "TestSupport.h"

namespace fs = std::filesystem;

namespace {

std::vector<std::string> ReadLines(const fs::path& path) {
    std::vector<std::string> lines;
    std::ifstream input(path, std::ios::binary);
    std::string line;
    while (std::getline(input, line)) {
        lines.push_back(line);
    }
    return lines;
}

// An environment variable set for one scope, and put back as it was.
class ScopedEnvironment {
public:
    ScopedEnvironment(const char* name, const char* value) : name_(name) {
        if (const char* previous = std::getenv(name)) {
            had_ = true;
            previous_ = previous;
        }
        if (value != nullptr) {
            ::setenv(name, value, 1);
        } else {
            ::unsetenv(name);
        }
    }
    ~ScopedEnvironment() {
        if (had_) {
            ::setenv(name_.c_str(), previous_.c_str(), 1);
        } else {
            ::unsetenv(name_.c_str());
        }
    }
    ScopedEnvironment(const ScopedEnvironment&) = delete;
    ScopedEnvironment& operator=(const ScopedEnvironment&) = delete;

private:
    std::string name_;
    bool had_ = false;
    std::string previous_;
};

const std::regex kTimePattern(
    R"(^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}Z$)");

// What the default action of a fatal signal is replaced by in a crashing
// child: an exit whose status names the signal, and no crash report.
void ExitNamingSignal(int signalNumber) { ::_exit(100 + signalNumber); }

// "NNN" followed by 46 'x' and a newline: a 50-byte line numbered `index`.
std::string NumberedLine(int index) {
    char head[8];
    std::snprintf(head, sizeof(head), "%03d", index);
    return std::string(head) + std::string(46, 'x') + "\n";
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.rfind(prefix, 0) == 0;
}

void CheckFormatting() {
    Check(engine_log::FormatUtcTime(0) == "1970-01-01T00:00:00.000Z",
          "the epoch is written in UTC to the millisecond");
    Check(engine_log::FormatUtcTime(951782400000) == "2000-02-29T00:00:00.000Z",
          "a leap day of a century divisible by 400");
    Check(
        engine_log::FormatUtcTime(4102444800000) == "2100-01-01T00:00:00.000Z",
        "2100 is no leap year");
    Check(
        engine_log::FormatUtcTime(1759498512345) == "2025-10-03T13:35:12.345Z",
        "milliseconds are kept");
    Check(
        engine_log::FormatUtcTime(1791017999999) == "2026-10-03T08:59:59.999Z",
        "the last millisecond of a second");
    Check(engine_log::FormatUtcTime(-1) == "1969-12-31T23:59:59.999Z",
          "a time before the epoch rounds down, not toward zero");

    Check(engine_log::LogToken("describe") == "describe",
          "a tool name is written as it is");
    Check(engine_log::LogToken("01K30000000000000000000002") ==
              "01K30000000000000000000002",
          "a timeline ID is written as it is");
    Check(engine_log::LogToken("0.1.0") == "0.1.0", "a version is kept");
    for (const std::string& unsafe :
         {std::string(), std::string("two words"),
          std::string("/Users/someone/Client X/project"),
          std::string("describe\n2026-10-03T00:00:00.000Z pid=1 forged"),
          std::string("caf\xc3\xa9"), std::string(65, 'a')}) {
        Check(engine_log::LogToken(unsafe) == "?",
              "text that is no short identifier is not written: '" + unsafe +
                  "'");
    }

    Check(engine_log::ToolCallEvent("describe", "", 12, "") ==
              "call tool=describe ok ms=12",
          "a call that succeeded");
    Check(engine_log::ToolCallEvent("remove_clip", "UnknownClip", 3,
                                    "01K30000000000000000000002") ==
              "call tool=remove_clip error=UnknownClip ms=3 "
              "timeline=01K30000000000000000000002",
          "a refused call names its error and its timeline");
    Check(engine_log::ToolCallEvent("move_clip", "", -4, "../../etc/passwd") ==
              "call tool=move_clip ok ms=0 timeline=?",
          "a timeline_id that is no ID is not written, a negative time is 0");

    Check(engine_log::LogLine(0, 4242, "mcp-stdio", "stop") ==
              "1970-01-01T00:00:00.000Z pid=4242 surface=mcp-stdio stop\n",
          "a line: time, pid, surface, event, newline");
}

void CheckDirectory(const fs::path& root) {
    {
        const ScopedEnvironment off("CUTMACHINE_LOG", "0");
        const ScopedEnvironment directory("CUTMACHINE_LOG_DIR",
                                          root.string().c_str());
        Check(engine_log::LogDirectoryFromEnvironment().empty(),
              "CUTMACHINE_LOG=0 turns the log off, whatever the folder");
    }
    {
        const ScopedEnvironment on("CUTMACHINE_LOG", "1");
        const ScopedEnvironment directory("CUTMACHINE_LOG_DIR",
                                          root.string().c_str());
        Check(engine_log::LogDirectoryFromEnvironment() == root.string(),
              "CUTMACHINE_LOG_DIR replaces the folder");
    }
    for (const char* unset : {static_cast<const char*>(nullptr), ""}) {
        const ScopedEnvironment on("CUTMACHINE_LOG", nullptr);
        const ScopedEnvironment directory("CUTMACHINE_LOG_DIR", unset);
        const ScopedEnvironment home("HOME", (root / "home").string().c_str());
        // HEADLESS-2026-10 H4 -- ~/.local/state/cutmachine off macOS.
        const fs::path expected =
            user_directories::kThisSystem == user_directories::System::kMacOS
                ? root / "home" / "Library" / "Logs" / "CUTMACHINE"
                : root / "home" / ".local" / "state" / "cutmachine";
        Check(engine_log::LogDirectoryFromEnvironment() == expected.string(),
              "without CUTMACHINE_LOG_DIR (or with it empty), the log goes to "
              "this system's log folder under $HOME: " +
                  engine_log::LogDirectoryFromEnvironment());
    }
}

void CheckRotation(const fs::path& root) {
    const fs::path directory = root / "rotation";
    engine_log::LogFile file(directory.string(), 1000);
    std::string message;
    Check(file.Open(message), "the log opens, its folder created: " + message);
    for (int index = 1; index <= 40; ++index) {
        Check(file.Append(NumberedLine(index)), "a line is appended");
    }
    const fs::path current = directory / "cutmachine.log";
    const fs::path older = directory / "cutmachine.log.1";
    std::vector<std::string> currentLines = ReadLines(current);
    std::vector<std::string> olderLines = ReadLines(older);
    Check(olderLines.size() == 20 && StartsWith(olderLines.front(), "001") &&
              StartsWith(olderLines.back(), "020"),
          "the file is set aside once it holds the limit: lines 1 to 20");
    Check(currentLines.size() == 20 &&
              StartsWith(currentLines.front(), "021") &&
              StartsWith(currentLines.back(), "040"),
          "the next lines start a new file: 21 to 40");
    Check(file.Append(NumberedLine(41)), "line 41");
    olderLines = ReadLines(older);
    currentLines = ReadLines(current);
    Check(olderLines.size() == 20 && StartsWith(olderLines.front(), "021"),
          "one older file only: the second rotation replaces the first");
    Check(currentLines.size() == 1 && StartsWith(currentLines.front(), "041"),
          "and line 41 opens the new file");
    Check(!fs::exists(directory / "cutmachine.log.2"),
          "no second older file is kept");

    // Two writers with their own descriptors -- two processes -- see the
    // file full together: one sets it aside, the other must not set the
    // fresh file aside over it.
    const fs::path shared = root / "shared";
    engine_log::LogFile first(shared.string(), 1000);
    engine_log::LogFile second(shared.string(), 1000);
    Check(first.Open(message) && second.Open(message),
          "two writers open one file");
    for (int index = 1; index <= 20; ++index) {
        engine_log::LogFile& writer = index % 2 == 1 ? first : second;
        writer.Append(NumberedLine(index));
    }
    first.Append(NumberedLine(21));
    second.Append(NumberedLine(22));
    const std::vector<std::string> kept =
        ReadLines(shared / "cutmachine.log.1");
    const std::vector<std::string> fresh = ReadLines(shared / "cutmachine.log");
    Check(kept.size() == 20 && StartsWith(kept.front(), "001"),
          "the full file is set aside once, whole");
    Check(fresh.size() == 2 && StartsWith(fresh.front(), "021") &&
              StartsWith(fresh.back(), "022"),
          "the second writer joins the new file instead of rotating again");

    // Deleted under a writer: the next line recreates it.
    fs::remove(shared / "cutmachine.log");
    first.Append(NumberedLine(23));
    const std::vector<std::string> recreated =
        ReadLines(shared / "cutmachine.log");
    Check(recreated.size() == 1 && StartsWith(recreated.front(), "023"),
          "a log deleted while open is recreated, not written into the void");
}

// Children appending at once: every line arrives whole, none is lost or
// torn -- with rotation too, where a line may only go with a whole file.
void CheckProcesses(const fs::path& root, uint64_t rotateBytes,
                    const std::string& name) {
    const fs::path directory = root / name;
    constexpr int kChildren = 4;
    constexpr int kLines = 300;
    std::vector<pid_t> children;
    for (int child = 0; child < kChildren; ++child) {
        const pid_t pid = ::fork();
        if (pid == 0) {
            engine_log::LogFile file(directory.string(), rotateBytes);
            std::string message;
            if (!file.Open(message)) {
                ::_exit(2);
            }
            for (int index = 0; index < kLines; ++index) {
                const std::string text = "child=" + std::to_string(child) +
                                         " n=" + std::to_string(index) + " " +
                                         std::string(80, 'a' + child) + "\n";
                if (!file.Append(text)) {
                    ::_exit(3);
                }
            }
            ::_exit(0);
        }
        children.push_back(pid);
    }
    bool clean = true;
    for (const pid_t pid : children) {
        int status = 0;
        ::waitpid(pid, &status, 0);
        clean = clean && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    }
    Check(clean, name + ": every writer ran to its end");

    const std::regex shape(R"(^child=(\d) n=(\d+) ([a-d])\3{79}$)");
    std::set<std::string> seen;
    size_t malformed = 0;
    size_t duplicated = 0;
    for (const fs::path& path :
         {directory / "cutmachine.log.1", directory / "cutmachine.log"}) {
        for (const std::string& line : ReadLines(path)) {
            std::smatch match;
            if (!std::regex_match(line, match, shape) ||
                match[3].str()[0] != 'a' + std::stoi(match[1].str())) {
                ++malformed;
                continue;
            }
            if (!seen.insert(match[1].str() + ":" + match[2].str()).second) {
                ++duplicated;
            }
        }
    }
    const size_t written = static_cast<size_t>(kChildren) * kLines;
    Check(malformed == 0, name + ": no line is torn or interleaved (" +
                              std::to_string(malformed) + " malformed)");
    Check(duplicated == 0, name + ": no line is written twice");
    if (rotateBytes >= engine_log::kRotateBytes) {
        Check(seen.size() == written, name + ": no line is lost (" +
                                          std::to_string(seen.size()) + " of " +
                                          std::to_string(written) + ")");
    } else {
        Check(fs::exists(directory / "cutmachine.log.1") &&
                  fs::file_size(directory / "cutmachine.log.1") >= rotateBytes,
              name + ": the file was set aside full");
        Check(!fs::exists(directory / "cutmachine.log.2"),
              name + ": one older file only");
    }
}

// The child: the stand-in for the default action of every signal the log
// handles (unless `standIns` is false), the log with its handlers, then
// `crash`. Returns how the child ended and the log's lines.
template <typename Crash>
std::vector<std::string> RunCrashingChild(const fs::path& directory,
                                          Crash crash, int& status, pid_t& pid,
                                          bool standIns = true) {
    pid = ::fork();
    if (pid == 0) {
        for (const int signalNumber : {SIGSEGV, SIGBUS, SIGABRT, SIGILL, SIGFPE,
                                       SIGTERM, SIGINT, SIGHUP}) {
            if (standIns) {
                ::signal(signalNumber, ExitNamingSignal);
            }
        }
        // The runtime's terminate handler says why it aborts; not here.
        if (std::freopen("/dev/null", "w", stderr) == nullptr) {
            ::_exit(4);
        }
        ::setenv("CUTMACHINE_LOG_DIR", directory.string().c_str(), 1);
        ::unsetenv("CUTMACHINE_LOG");
        if (!engine_log::StartProcessLog("crash-test", true)) {
            ::_exit(2);
        }
        crash();
        ::_exit(3);
    }
    status = 0;
    ::waitpid(pid, &status, 0);
    return ReadLines(directory / "cutmachine.log");
}

size_t CountCrashLines(const std::vector<std::string>& lines) {
    size_t count = 0;
    for (const std::string& line : lines) {
        if (line.find(" crash ") != std::string::npos) {
            ++count;
        }
    }
    return count;
}

void CheckCrashLine(const std::vector<std::string>& lines, pid_t pid,
                    const std::string& event, const std::string& label) {
    Check(lines.size() == 2, label + ": a start line and a crash line, got " +
                                 std::to_string(lines.size()));
    if (lines.size() != 2) {
        return;
    }
    const BuildInfo& build = CurrentBuild();
    const std::string identity =
        " pid=" + std::to_string(pid) + " surface=crash-test ";
    Check(lines[0].size() > 24 &&
              std::regex_match(lines[0].substr(0, 24), kTimePattern) &&
              lines[0].substr(24) ==
                  identity + "start version=" + build.version +
                      " commit=" + build.commit + (build.dirty ? "-dirty" : ""),
          label + ": the start line names the build and the pid: " + lines[0]);
    Check(lines[1].size() > 24 &&
              std::regex_match(lines[1].substr(0, 24), kTimePattern) &&
              lines[1].substr(24) == identity + event,
          label + ": the line it leaves: " + lines[1]);
}

void CheckCrashes(const fs::path& root) {
    // Every fatal signal, sent by the process to itself.
    for (const int signalNumber : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) {
        const fs::path directory =
            root / ("signal-" + std::to_string(signalNumber));
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines = RunCrashingChild(
            directory, [signalNumber] { ::raise(signalNumber); }, status, pid);
        const std::string label = "signal " + std::to_string(signalNumber);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 100 + signalNumber,
              label + ": handed back to the action installed before");
        CheckCrashLine(lines, pid,
                       "crash signal=" + std::to_string(signalNumber), label);
    }

    // The signals that stop a process without a crash report: a stop line.
    for (const int signalNumber : {SIGTERM, SIGINT, SIGHUP}) {
        const fs::path directory =
            root / ("stop-" + std::to_string(signalNumber));
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines = RunCrashingChild(
            directory, [signalNumber] { ::raise(signalNumber); }, status, pid);
        const std::string label = "signal " + std::to_string(signalNumber);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 100 + signalNumber,
              label + ": handed back to the action installed before");
        CheckCrashLine(lines, pid,
                       "stop signal=" + std::to_string(signalNumber), label);
    }
    // And with the default action itself, which SIGTERM can be left to: it
    // writes no crash report. The client that sent it sees the death it
    // always saw.
    {
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines = RunCrashingChild(
            root / "sigterm-default", [] { ::raise(SIGTERM); }, status, pid,
            false);
        Check(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM,
              "SIGTERM still ends the process as it did");
        CheckCrashLine(lines, pid, "stop signal=" + std::to_string(SIGTERM),
                       "SIGTERM, default action");
    }

    // A real fault: an access to page zero, at an address the compiler
    // cannot see.
    {
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines = RunCrashingChild(
            root / "fault",
            [] {
                // The fault is the point: an address made from an integer.
                const uintptr_t page = std::strtoul("16", nullptr, 10);
                // NOLINTNEXTLINE(performance-no-int-to-ptr)
                auto* address = reinterpret_cast<volatile int*>(page);
                *address = 1;
            },
            status, pid);
        // A bad access is SIGSEGV on macOS, SIGSEGV or SIGBUS elsewhere.
        Check(WIFEXITED(status) && (WEXITSTATUS(status) == 100 + SIGSEGV ||
                                    WEXITSTATUS(status) == 100 + SIGBUS),
              "a fault is handed back to the action installed before");
        Check(lines.size() == 2 &&
                  lines[1].find(" crash signal=") != std::string::npos,
              "a fault leaves its crash line");
    }

    // abort(): the line, then the default action's stand-in.
    {
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines =
            RunCrashingChild(root / "abort", [] { std::abort(); }, status, pid);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 100 + SIGABRT,
              "abort() is handed back");
        CheckCrashLine(lines, pid, "crash signal=" + std::to_string(SIGABRT),
                       "abort");
    }

    // std::terminate: the line, then the runtime's own handler, whose
    // abort() adds no second line.
    {
        int status = 0;
        pid_t pid = 0;
        const std::vector<std::string> lines = RunCrashingChild(
            root / "terminate", [] { std::terminate(); }, status, pid);
        Check(WIFEXITED(status) && WEXITSTATUS(status) == 100 + SIGABRT,
              "std::terminate goes on to the runtime's handler and abort()");
        CheckCrashLine(lines, pid, "crash terminate", "terminate");
        Check(CountCrashLines(lines) == 1, "one crash, one line");
    }
}

// In this process: the log without crash handlers, as a test may start it.
void CheckProcessLog(const fs::path& root) {
    struct sigaction before {};
    ::sigaction(SIGSEGV, nullptr, &before);
    struct sigaction beforeTerm {};
    ::sigaction(SIGTERM, nullptr, &beforeTerm);

    const fs::path off = root / "off";
    {
        const ScopedEnvironment disabled("CUTMACHINE_LOG", "0");
        const ScopedEnvironment directory("CUTMACHINE_LOG_DIR",
                                          off.string().c_str());
        Check(!engine_log::StartProcessLog("mcp-stdio", false),
              "CUTMACHINE_LOG=0: no log");
        Check(!engine_log::Active() && !fs::exists(off),
              "and not even its folder");
        engine_log::RecordToolCall("describe", "", engine_log::Clock::now(),
                                   "");
        Check(!fs::exists(off), "a call writes nothing without a log");
    }

    const fs::path on = root / "process";
    const ScopedEnvironment enabled("CUTMACHINE_LOG", nullptr);
    const ScopedEnvironment directory("CUTMACHINE_LOG_DIR",
                                      on.string().c_str());
    Check(engine_log::StartProcessLog("mcp-http", false), "the log starts");
    Check(engine_log::Active(), "the process keeps a log");
    Check(engine_log::ProcessLogPath() == (on / "cutmachine.log").string(),
          "in the folder named");
    engine_log::RecordToolCall("describe", "", engine_log::Clock::now(),
                               "01K30000000000000000000002");
    engine_log::RecordToolCall("remove_clip", "UnknownClip",
                               engine_log::Clock::now(), "");
    engine_log::StopProcessLog();
    engine_log::StopProcessLog();
    Check(!engine_log::Active() && engine_log::ProcessLogPath().empty(),
          "stopped without crash handlers, the process keeps no log");
    engine_log::RecordToolCall("describe", "", engine_log::Clock::now(), "");

    const std::vector<std::string> lines = ReadLines(on / "cutmachine.log");
    const std::string pid = std::to_string(::getpid());
    const std::regex call(
        R"(^.{24} pid=\d+ surface=mcp-http call tool=\S+ .*$)");
    Check(lines.size() == 4, "start, two calls, one stop: " +
                                 std::to_string(lines.size()) + " lines");
    if (lines.size() == 4) {
        Check(lines[0].find(" pid=" + pid + " surface=mcp-http start ") !=
                  std::string::npos,
              "the start line: " + lines[0]);
        Check(lines[1].find(" surface=mcp-http call tool=describe ok ms=") !=
                      std::string::npos &&
                  lines[1].size() > 36 &&
                  lines[1].substr(lines[1].size() - 36) ==
                      " timeline=01K30000000000000000000002",
              "a call that succeeded, with the timeline it named: " + lines[1]);
        Check(lines[2].find(" call tool=remove_clip error=UnknownClip ms=") !=
                  std::string::npos,
              "a refused call: " + lines[2]);
        Check(
            lines[3].size() > 24 &&
                lines[3].substr(24) == " pid=" + pid + " surface=mcp-http stop",
            "the stop line: " + lines[3]);
        for (const std::string& line : lines) {
            Check(std::regex_match(line.substr(0, 24), kTimePattern),
                  "every line starts with its time: " + line);
        }
        Check(std::regex_match(lines[1], call) &&
                  std::regex_match(lines[2], call),
              "call lines have one shape");
    }

    struct sigaction after {};
    ::sigaction(SIGSEGV, nullptr, &after);
    struct sigaction afterTerm {};
    ::sigaction(SIGTERM, nullptr, &afterTerm);
    Check(before.sa_handler == after.sa_handler &&
              beforeTerm.sa_handler == afterTerm.sa_handler,
          "a log started without crash handlers leaves the signals alone");
}

int Run() {
    const fs::path root =
        fs::temp_directory_path() /
        ("cutmachine_engine_log_tests_" + std::to_string(::getpid()));
    fs::remove_all(root);
    fs::create_directories(root);

    CheckFormatting();
    CheckDirectory(root);
    CheckRotation(root);
    CheckProcesses(root, engine_log::kRotateBytes, "processes");
    CheckProcesses(root, uint64_t{8} * 1024, "processes-rotating");
    // Children first: one forked while this process kept a log would
    // inherit it already started, without its own crash handlers.
    CheckCrashes(root);
    CheckProcessLog(root);

    fs::remove_all(root);
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "engine log tests passed\n";
    return 0;
}

}  // namespace

int main() {
    try {
        return Run();
    } catch (const std::exception& exception) {
        std::cerr << "FAIL: " << exception.what() << '\n';
        return 1;
    }
}
