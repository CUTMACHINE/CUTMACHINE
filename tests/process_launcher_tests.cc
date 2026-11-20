// PRD1 -- docs/roadmap/PRODUIT-2026-10.md. The engine's one launcher of
// other programs, and its one resolution of the FFmpeg binary.
//
// The launcher is tried on /bin/sh: what it does with the two standard
// streams, how it bounds the error tail and a read, how it reports a program
// that could not start, and how it cancels -- including a child that closed
// its pipes and one that ignores SIGTERM. The resolution is tried on shell
// scripts named `ffmpeg`: the order (CUTMACHINE_FFMPEG, beside the
// executable, PATH), the refusal that names where it looked, the memo, and
// that a binary a caller named itself is never resolved.
//
// Plain C++ and POSIX: no AppKit, no FFmpeg, no network.

#include "FfmpegBinary.h"
#include "FfmpegPipe.h"
#include "MfaAlignment.h"
#include "ProcessLauncher.h"

#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "TestSupport.h"

namespace {

process::Request Shell(const std::string& script) {
    process::Request request;
    request.arguments = {"/bin/sh", "-c", script};
    return request;
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                         start)
        .count();
}

// An executable shell script.
void WriteScript(const std::filesystem::path& path, const std::string& body) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path) << "#!/bin/sh\n" << body << '\n';
    chmod(path.c_str(), 0755);
}

void TestLauncher() {
    Test("standard output arrives in order, the status is read", [] {
        std::string output;
        int started = 0;
        process::Request request = Shell("printf 'un '; printf 'deux'");
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        request.on_started = [&] { ++started; };
        request.on_output = [&](const char* bytes, size_t count) {
            output.append(bytes, count);
        };
        const process::Result result = process::Run(request);
        Check(result.Started() && result.Succeeded(), "the script succeeds");
        Check(output == "un deux", "its output is whole: " + output);
        Check(result.error_tail.empty(), "it wrote no error");
        Check(started == 1, "on_started is called once");
        Check(!result.cancelled && !result.Signalled(),
              "nothing was cancelled or signalled");
    });

    Test("a failing program gives its status and its error tail", [] {
        process::Request request = Shell("echo 'la raison' >&2; exit 3");
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        const process::Result result = process::Run(request);
        Check(result.Started() && !result.Succeeded(), "it ran and failed");
        Check(result.ExitStatus() == 3, "its exit status is 3");
        Check(result.error_tail == "la raison\n",
              "its error output is kept: " + result.error_tail);
    });

    Test("the error tail keeps the end, a read is bounded", [] {
        // 20 000 bytes on each stream, in numbered lines of fifty.
        const std::string flood =
            "i=0; while [ $i -lt 400 ]; do "
            "printf '%049d\\n' $i; printf '%049d\\n' $i >&2; i=$((i+1)); "
            "done";
        size_t total = 0;
        size_t largest = 0;
        process::Request request = Shell(flood);
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        request.read_bytes = 64;
        request.error_tail_bytes = 100;
        request.on_output = [&](const char*, size_t count) {
            total += count;
            largest = std::max(largest, count);
        };
        const process::Result result = process::Run(request);
        Check(result.Succeeded(), "the flood ends well");
        Check(total == 20000,
              "all of standard output is read: " + std::to_string(total));
        Check(largest > 0 && largest <= 64,
              "no chunk is larger than a read: " + std::to_string(largest));
        Check(
            result.error_tail.size() == 100,
            "the tail is bounded: " + std::to_string(result.error_tail.size()));
        Check(result.error_tail.rfind("399\n") == 96,
              "and it is the end of the error output");
    });

    Test("a discarded stream is not read, an inherited one is not kept", [] {
        std::string output;
        process::Request request = Shell("echo dehors; echo bruit >&2");
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kDiscard;
        request.on_output = [&](const char* bytes, size_t count) {
            output.append(bytes, count);
        };
        process::Result result = process::Run(request);
        Check(result.Succeeded() && output == "dehors\n",
              "standard output is still read: " + output);
        Check(result.error_tail.empty(), "nothing is kept of the error output");

        process::Request quiet = Shell("exit 0");
        result = process::Run(quiet);
        Check(result.Succeeded(),
              "a program with no pipe at all is waited for");
        process::Request failing = Shell("exit 7");
        result = process::Run(failing);
        Check(!result.Succeeded() && result.ExitStatus() == 7,
              "and its status is read");
    });

    Test("an argument is one argument, whatever is in it", [] {
        const std::string awkward = "a b 'c' \"d\" $HOME; echo non && *";
        std::string output;
        process::Request request;
        request.arguments = {"/bin/sh", "-c", "printf '%s' \"$1\"", "sh",
                             awkward};
        request.standard_output = process::Stream::kPipe;
        request.on_output = [&](const char* bytes, size_t count) {
            output.append(bytes, count);
        };
        Check(process::Run(request).Succeeded() && output == awkward,
              "no shell reads the argument: " + output);
    });

    Test("a program that cannot start is a spawn failure", [] {
        int started = 0;
        process::Request request;
        request.arguments = {"/nonexistent/cutmachine-prd1/program"};
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        request.on_started = [&] { ++started; };
        process::Result result = process::Run(request);
        Check(!result.Started() && result.stage == process::Stage::kSpawn,
              "it did not start");
        Check(result.system_error == ENOENT,
              "the reason is the system's: " + result.SystemErrorText());
        Check(!result.Succeeded() && started == 0, "on_started is not called");

        process::Request empty;
        result = process::Run(empty);
        Check(result.stage == process::Stage::kSpawn,
              "no program at all is a spawn failure too");
    });

    // DEC39 -- docs/roadmap/PHASES-2026-08.md. The editor reopening on
    // another project is started this way, as the old one quits.
    Test("a detached program runs on without being waited for", [] {
        const std::filesystem::path folder =
            std::filesystem::temp_directory_path() /
            ("cutmachine-detached-" + std::to_string(getpid()));
        std::filesystem::create_directories(folder);
        const std::filesystem::path written = folder / "written";
        const auto start = std::chrono::steady_clock::now();
        const process::Result started = process::StartDetached(
            {"/bin/sh", "-c", "sleep 0.3; echo $$ > \"$1\"", "sh",
             written.string()});
        Check(started.Started() && SecondsSince(start) < 0.25,
              "it returns once started, before the program ends");
        while (!std::filesystem::exists(written) && SecondsSince(start) < 10.0)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        std::string pid;
        std::ifstream(written) >> pid;
        Check(!pid.empty() && pid != std::to_string(getpid()),
              "the program ran, as a process of its own: " + pid);
        const process::Result missing =
            process::StartDetached({"/nonexistent/cutmachine-dec39/editor"});
        Check(!missing.Started() && missing.system_error == ENOENT,
              "a program that cannot start says why: " +
                  missing.SystemErrorText());
        Check(process::StartDetached({}).stage == process::Stage::kSpawn,
              "no program at all is a spawn failure");
        std::error_code ignored;
        std::filesystem::remove_all(folder, ignored);
    });

    Test("cancellation ends the program with SIGTERM", [] {
        std::atomic_bool cancel{false};
        process::Request request = Shell("echo pret; exec sleep 30");
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        request.on_output = [&](const char*, size_t) { cancel = true; };
        request.cancelled = [&] { return cancel.load(); };
        const auto start = std::chrono::steady_clock::now();
        const process::Result result = process::Run(request);
        Check(result.cancelled, "the launcher reports the cancellation");
        Check(result.Signalled() && result.TerminationSignal() == SIGTERM,
              "the program died of SIGTERM");
        Check(!result.Succeeded(), "a cancelled run did not succeed");
        Check(SecondsSince(start) < 2.0, "without waiting for the sleep");
    });

    Test("a program that ignores SIGTERM is killed after the grace", [] {
        // Cancelled only once the trap is in place, which the line says.
        std::atomic_bool cancel{false};
        process::Request request =
            Shell("trap '' TERM; echo pret; exec sleep 30");
        request.standard_output = process::Stream::kPipe;
        request.on_output = [&](const char*, size_t) { cancel = true; };
        request.cancelled = [&] { return cancel.load(); };
        const auto start = std::chrono::steady_clock::now();
        const process::Result result = process::Run(request);
        const double seconds = SecondsSince(start);
        Check(result.cancelled && result.Signalled() &&
                  result.TerminationSignal() == SIGKILL,
              "SIGKILL ends it");
        Check(seconds > 1.9 && seconds < 10.0,
              "after the two seconds it was given: " + std::to_string(seconds));
    });

    Test("a program that closed its pipes can still be cancelled", [] {
        // Both pipes reach their end at once while the program goes on:
        // the wait that follows must keep listening for the cancellation.
        std::atomic_bool cancel{false};
        process::Request request = Shell("exec >&- 2>&-; exec sleep 30");
        request.standard_output = process::Stream::kPipe;
        request.standard_error = process::Stream::kPipe;
        int polls = 0;
        request.cancelled = [&] { return ++polls > 3; };
        const auto start = std::chrono::steady_clock::now();
        const process::Result result = process::Run(request);
        Check(result.cancelled && result.Signalled(),
              "it is cancelled although nothing can be read from it");
        Check(SecondsSince(start) < 5.0, "without waiting for the sleep");
    });

    // FI9 -- docs/roadmap/FIABILITE-2026-10.md. These children keep the
    // pipes open after their shell exits; cancellation must reach them.
    for (bool ignoresTerm : {false, true}) {
        Test(ignoresTerm ? "a resistant descendant is killed after the grace"
                         : "cancellation stops a wrapper's descendant",
             [&] {
                 std::atomic_bool cancel{false};
                 process::Request request = Shell(
                     std::string("(") + (ignoresTerm ? "trap '' TERM; " : "") +
                     "echo ready; exec sleep 6) & wait");
                 request.standard_output = process::Stream::kPipe;
                 request.standard_error = process::Stream::kPipe;
                 request.on_output = [&](const char*, size_t) {
                     cancel = true;
                 };
                 request.cancelled = [&] { return cancel.load(); };
                 const auto start = std::chrono::steady_clock::now();
                 const auto result = process::Run(request);
                 const double seconds = SecondsSince(start);
                 // bash outlives a SIGTERM that lands before its `wait`
                 // and exits 0 (seen under load): the wrapper's status
                 // proves nothing, the descendant's end below does.
                 Check(result.cancelled && result.Started(),
                       "the wrapper's run was cancelled");
                 Check(
                     seconds < 4.5,
                     "the descendant did not keep the pipes for six seconds: " +
                         std::to_string(seconds));
                 if (ignoresTerm) {
                     Check(seconds > 1.9, "the descendant was given its grace");
                 }
             });
    }

    // FI9 -- a child leading its own group is out of reach of the Ctrl-C a
    // terminal sends: the engine passes its stop signal on, descendant
    // included.
    Test("a stop signal passed on reaches a running group", [] {
        std::atomic_bool ready{false};
        process::Request request = Shell("(echo ready; exec sleep 6) & wait");
        request.standard_output = process::Stream::kPipe;
        request.on_output = [&](const char*, size_t) { ready = true; };
        const auto start = std::chrono::steady_clock::now();
        process::Result result;
        std::thread running([&] { result = process::Run(request); });
        while (!ready.load() && SecondsSince(start) < 5.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        process::SignalRunningGroups(SIGTERM);
        running.join();
        // As above, bash's own status proves nothing: the descendant
        // holding the pipe is what has to be stopped.
        Check(ready.load() && result.Started(),
              "the program started and was reached");
        Check(SecondsSince(start) < 4.5,
              "its descendant did not keep the pipe for six seconds: " +
                  std::to_string(SecondsSince(start)));
    });
}

// FI10 -- docs/roadmap/FIABILITE-2026-10.md. A shell stands in for Python:
// the real MFA entry point, task cancellation and stderr handling still run.
void TestMfa(const std::filesystem::path& root) {
    Test("MFA remains cancellable after stderr closes", [&] {
        const auto ready = root / "mfa-ready";
        const auto script = root / "mfa-close-stderr.sh";
        WriteScript(script, "exec 2>&-\necho ready > \"$1\"\nexec sleep 6");
        MfaAlignmentSettings settings;
        settings.python = "/bin/sh";
        settings.script = script.string();
        MediaTaskManager manager(1);
        const auto id = manager.Enqueue(
            MediaTaskKind::WordAlignment, "MFA fixture",
            [&](MediaTaskContext& context, std::string& error) {
                return RunMfaAlignment(settings, ready.string(), {}, "",
                                       root.string(), "", context, error);
            });
        const auto start = std::chrono::steady_clock::now();
        while (!std::filesystem::exists(ready) && SecondsSince(start) < 3.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        Check(std::filesystem::exists(ready), "the script closed stderr");
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        Check(manager.Cancel(id), "cancellation was requested");
        Check(manager.WaitForIdle(4000), "the cancelled task finishes");
        const auto tasks = manager.Snapshot();
        Check(tasks.size() == 1 && tasks[0].state == MediaTaskState::Cancelled,
              "MFA reports cancellation, not success");
    });

    Test("MFA keeps the script's failure and stderr", [&] {
        const auto script = root / "mfa-failure.sh";
        WriteScript(script, "echo 'alignment fixture failure' >&2; exit 7");
        MfaAlignmentSettings settings;
        settings.python = "/bin/sh";
        settings.script = script.string();
        MediaTaskManager manager(1);
        const auto result = RunMediaTaskAndWait(
            manager, MediaTaskKind::WordAlignment, "MFA fixture",
            [&](MediaTaskContext& context, std::string& error) {
                return RunMfaAlignment(settings, "unused", {}, "",
                                       root.string(), "", context, error);
            });
        Check(result.state == MediaTaskState::Failed &&
                  result.error.find("alignment fixture failure") !=
                      std::string::npos,
              "the traceback tail reaches the caller: " + result.error);
    });
}

void TestLocate(const std::filesystem::path& root) {
    const std::filesystem::path first = root / "first";
    const std::filesystem::path second = root / "second";
    const std::filesystem::path bundle = root / "bundle";
    const std::filesystem::path custom = root / "custom" / "mon-ffmpeg";
    const std::filesystem::path empty = root / "empty";
    WriteScript(first / "ffmpeg", "echo first");
    WriteScript(second / "ffmpeg", "echo second");
    WriteScript(bundle / "ffmpeg", "echo bundle");
    WriteScript(custom, "echo custom");
    std::filesystem::create_directories(empty);
    const std::string settingsFile = (root / "settings.env").string();

    ffmpeg::Search everything;
    everything.configured = custom.string();
    everything.executable_directory = bundle.string();
    everything.search_path = first.string() + ":" + second.string();
    everything.settings_file = settingsFile;

    Test("CUTMACHINE_FFMPEG wins over the bundle and PATH", [&] {
        const ffmpeg::Location location = ffmpeg::Locate(everything);
        Check(location.Found() && location.path == custom.string(),
              "the configured binary is the answer: " + location.path);
        Check(location.origin == ffmpeg::Origin::kConfigured &&
                  std::string(ffmpeg::OriginName(location.origin)) ==
                      "CUTMACHINE_FFMPEG",
              "and it says where it came from");
    });

    Test("a configured binary that is not one is refused, not skipped", [&] {
        ffmpeg::Search search = everything;
        search.configured = (root / "custom" / "absent").string();
        ffmpeg::Location location = ffmpeg::Locate(search);
        Check(!location.Found(), "nothing is found although PATH has one");
        Check(
            location.refusal.find("FFmpeg not found: ") == 0 &&
                location.refusal.find("CUTMACHINE_FFMPEG") !=
                    std::string::npos &&
                location.refusal.find(search.configured) != std::string::npos &&
                location.refusal.find("not an executable file") !=
                    std::string::npos &&
                location.refusal.find(settingsFile) != std::string::npos,
            "the refusal names the variable, its value and the file: " +
                location.refusal);

        // A file that exists without the execute bit, and a directory.
        const std::filesystem::path plain = root / "custom" / "plain";
        std::ofstream(plain) << "not a program\n";
        search.configured = plain.string();
        Check(!ffmpeg::Locate(search).Found(), "a plain file is not a binary");
        search.configured = (root / "custom").string();
        Check(!ffmpeg::Locate(search).Found(), "a directory is not a binary");
    });

    Test("a configured bare name is looked up on PATH", [&] {
        WriteScript(second / "ffmpeg7", "echo seven");
        ffmpeg::Search search = everything;
        search.configured = "ffmpeg7";
        ffmpeg::Location location = ffmpeg::Locate(search);
        Check(location.path == (second / "ffmpeg7").string() &&
                  location.origin == ffmpeg::Origin::kConfigured,
              "the name is found in a directory of PATH: " + location.path);
        search.configured = "ffmpeg8";
        location = ffmpeg::Locate(search);
        Check(
            !location.Found() &&
                location.refusal.find("'ffmpeg8'") != std::string::npos &&
                location.refusal.find(search.search_path) != std::string::npos,
            "an unknown name is refused with the PATH searched: " +
                location.refusal);
    });

    Test("the bundled binary wins over PATH", [&] {
        ffmpeg::Search search = everything;
        search.configured.clear();
        const ffmpeg::Location location = ffmpeg::Locate(search);
        Check(location.path == (bundle / "ffmpeg").string() &&
                  location.origin == ffmpeg::Origin::kBundled,
              "beside the executable comes before PATH: " + location.path);
    });

    Test("PATH is searched in order, and only for executables", [&] {
        ffmpeg::Search search = everything;
        search.configured.clear();
        search.executable_directory = empty.string();
        ffmpeg::Location location = ffmpeg::Locate(search);
        Check(location.path == (first / "ffmpeg").string() &&
                  location.origin == ffmpeg::Origin::kSearchPath,
              "the first directory wins: " + location.path);

        // A file without the execute bit and a directory, both named
        // ffmpeg, in the directories that come first.
        const std::filesystem::path plain = root / "plain";
        const std::filesystem::path folder = root / "folder";
        std::filesystem::create_directories(plain);
        std::ofstream(plain / "ffmpeg") << "not a program\n";
        std::filesystem::create_directories(folder / "ffmpeg");
        search.search_path = plain.string() + ":" + folder.string() + ":" +
                             (root / "absent").string() + ":" + second.string();
        location = ffmpeg::Locate(search);
        Check(location.path == (second / "ffmpeg").string(),
              "what cannot be run is passed over: " + location.path);
    });

    Test("the answer is absolute", [&] {
        const std::filesystem::path before = std::filesystem::current_path();
        std::filesystem::current_path(root);
        ffmpeg::Search search;
        search.search_path = "empty:second";
        ffmpeg::Location location = ffmpeg::Locate(search);
        // The working directory may be reached through a symbolic link
        // (/tmp on macOS): compare what the two paths name.
        Check(std::filesystem::path(location.path).is_absolute() &&
                  std::filesystem::equivalent(location.path, second / "ffmpeg"),
              "a relative PATH entry gives an absolute path: " + location.path);
        search.search_path.clear();
        search.configured = "custom/mon-ffmpeg";
        location = ffmpeg::Locate(search);
        Check(std::filesystem::path(location.path).is_absolute() &&
                  std::filesystem::equivalent(location.path, custom),
              "so does a relative CUTMACHINE_FFMPEG: " + location.path);
        std::filesystem::current_path(before);
    });

    Test("nothing anywhere is a named refusal that says where it looked", [&] {
        ffmpeg::Search search;
        search.executable_directory = empty.string();
        search.search_path = empty.string() + ":" + (root / "absent").string();
        search.settings_file = settingsFile;
        const ffmpeg::Location location = ffmpeg::Locate(search);
        Check(!location.Found() && location.path.empty(), "nothing is found");
        const std::string& refusal = location.refusal;
        Check(refusal.find("FFmpeg not found: ") == 0,
              "it starts with its name: " + refusal);
        Check(
            refusal.find("CUTMACHINE_FFMPEG is not set") != std::string::npos &&
                refusal.find(empty.string()) != std::string::npos &&
                refusal.find(search.search_path) != std::string::npos,
            "it says the three places it looked: " + refusal);
        Check(refusal.find("Set CUTMACHINE_FFMPEG") != std::string::npos &&
                  refusal.find(settingsFile) != std::string::npos,
              "and which variable to set, in which file: " + refusal);
        Check(ffmpeg::RefusalName(refusal) == "FfmpegNotFound" &&
                  ffmpeg::RefusalName("unable to fit the captions: " +
                                      refusal) == "FfmpegNotFound",
              "a surface can name it, wrapped or not");
        Check(ffmpeg::RefusalName("unable to start FFmpeg: no").empty() &&
                  ffmpeg::RefusalName("").empty(),
              "and names nothing else");
    });
}

// The process's own resolution: its environment, the settings file, and the
// memo. One function, in order, because the settings file is read once per
// process and the memo is the process's.
void TestResolved(const std::filesystem::path& root) {
    const std::filesystem::path fromFile = root / "from-file" / "ffmpeg";
    const std::filesystem::path fromEnvironment = root / "from-env" / "ffmpeg";
    const std::filesystem::path onPath = root / "on-path";
    const std::filesystem::path later = root / "later";
    const std::filesystem::path marker = root / "launched";
    WriteScript(fromFile, "echo from-file \"$@\"");
    WriteScript(fromEnvironment, "echo from-env \"$@\"");
    WriteScript(onPath / "ffmpeg",
                "echo on-path \"$@\"; : > '" + marker.string() + "'");
    std::filesystem::create_directories(later);

    Test("the settings file names the binary when the environment does not",
         [&] {
             std::string path;
             std::string error;
             Check(ffmpeg::Executable("", path, error) &&
                       path == fromFile.string(),
                   "CUTMACHINE_FFMPEG is read from the file: " + path + error);
             Check(ffmpeg::Resolved().origin == ffmpeg::Origin::kConfigured,
                   "as a configured binary");
         });

    Test("a binary the caller names is used as given", [&] {
        std::string path;
        std::string error;
        Check(ffmpeg::Executable("/nonexistent/ffmpeg", path, error) &&
                  path == "/nonexistent/ffmpeg" && error.empty(),
              "an explicit path is neither checked nor replaced");
        process::Request request;
        request.arguments = {"/nonexistent/ffmpeg", "-version"};
        const process::Result result = ffmpeg::Run(request);
        Check(result.stage == process::Stage::kSpawn &&
                  ffmpeg::StartError(result, "test pipes") ==
                      "unable to start FFmpeg: No such file or directory",
              "and failing to start it is said in the launcher's words: " +
                  ffmpeg::StartError(result, "test pipes"));
    });

    Test("the environment wins over the file", [&] {
        setenv("CUTMACHINE_FFMPEG", fromEnvironment.c_str(), 1);
        std::string path;
        std::string error;
        Check(ffmpeg::Executable("", path, error) &&
                  path == fromEnvironment.string(),
              "the variable set in the environment is the answer: " + path);
    });

    Test("an empty first argument runs the resolved binary", [&] {
        std::string output;
        process::Request request;
        request.arguments = {"", "-un", "deux mots"};
        request.standard_output = process::Stream::kPipe;
        request.on_output = [&](const char* bytes, size_t count) {
            output.append(bytes, count);
        };
        Check(ffmpeg::Run(request).Succeeded() &&
                  output == "from-env -un deux mots\n",
              "the launch went to it, arguments intact: " + output);

        // ReadFfmpegRawFrames is the same launch, cut into frames.
        WriteScript(fromEnvironment, "printf 'abcdefgh'; echo bilan >&2");
        std::vector<std::string> frames;
        std::string error;
        std::string diagnostics;
        Check(ReadFfmpegRawFrames(
                  {"", "-f", "rawvideo"}, 4,
                  [&](const uint8_t* frame) {
                      frames.emplace_back(reinterpret_cast<const char*>(frame),
                                          4);
                  },
                  error, &diagnostics) &&
                  frames == std::vector<std::string>{"abcd", "efgh"} &&
                  diagnostics == "bilan\n",
              "frames are whole and in order, diagnostics kept: " + error);
        WriteScript(fromEnvironment, "echo 'pas de flux' >&2; exit 1");
        Check(!ReadFfmpegRawFrames(
                  {"", "-f", "rawvideo"}, 4, [](const uint8_t*) {}, error) &&
                  error == "pas de flux\n",
              "a failure is FFmpeg's own last words: " + error);
    });

    Test("without the variable, PATH decides, and the answer is kept", [&] {
        unsetenv("CUTMACHINE_FFMPEG");
        setenv("PATH", (later.string() + ":" + onPath.string()).c_str(), 1);
        std::string path;
        std::string error;
        Check(ffmpeg::Executable("", path, error) &&
                  path == (onPath / "ffmpeg").string(),
              "the binary on PATH is found: " + path + error);
        Check(ffmpeg::Resolved().origin == ffmpeg::Origin::kSearchPath,
              "as one found on PATH");

        // An earlier directory gains an ffmpeg while PATH keeps its value:
        // the process keeps the binary it started with.
        WriteScript(later / "ffmpeg", "echo later");
        Check(ffmpeg::Executable("", path, error) &&
                  path == (onPath / "ffmpeg").string(),
              "one search per environment: the first answer stands: " + path);

        // PATH itself changes, as the suites that put FFmpeg's directory
        // on it mid-run do: the answer follows.
        setenv("PATH", later.c_str(), 1);
        Check(ffmpeg::Executable("", path, error) &&
                  path == (later / "ffmpeg").string(),
              "a changed PATH is searched again: " + path);
    });

    Test("no binary: the launch is refused by name and nothing starts", [&] {
        std::filesystem::remove(marker);
        const std::filesystem::path nowhere = root / "nowhere";
        std::filesystem::create_directories(nowhere);
        setenv("PATH", nowhere.c_str(), 1);
        int started = 0;
        process::Request request;
        request.arguments = {"", "-version"};
        request.standard_output = process::Stream::kPipe;
        request.on_started = [&] { ++started; };
        const process::Result result = ffmpeg::Run(request);
        Check(result.stage == process::Stage::kRefused && !result.Started() &&
                  !result.Succeeded(),
              "the launch is refused");
        const std::string sentence = ffmpeg::StartError(result, "test pipes");
        Check(sentence == result.refusal &&
                  ffmpeg::RefusalName(sentence) == "FfmpegNotFound" &&
                  sentence.find(nowhere.string()) != std::string::npos &&
                  sentence.find("Set CUTMACHINE_FFMPEG") != std::string::npos &&
                  sentence.find((root / "settings.env").string()) !=
                      std::string::npos,
              "in the resolver's sentence: " + sentence);
        Check(started == 0 && !std::filesystem::exists(marker),
              "and no program was started");

        std::string error;
        Check(!ReadFfmpegRawFrames(
                  {"", "-f", "rawvideo"}, 4, [](const uint8_t*) {}, error) &&
                  ffmpeg::RefusalName(error) == "FfmpegNotFound",
              "the frame reader relays the refusal: " + error);

        // A failure is not remembered: installing FFmpeg is enough.
        WriteScript(nowhere / "ffmpeg", "echo enfin");
        std::string path;
        Check(ffmpeg::Executable("", path, error) &&
                  path == (nowhere / "ffmpeg").string(),
              "a binary that appears is found without a restart: " + path);
    });
}

}  // namespace

int main() {
    std::error_code ignored;
    const std::filesystem::path root =
        std::filesystem::canonical(std::filesystem::temp_directory_path()) /
        ("cutmachine-process-launcher-" + std::to_string(getpid()));
    std::filesystem::remove_all(root, ignored);
    std::filesystem::create_directories(root);

    // The machine's own settings must not decide this suite's answers: the
    // settings file is this suite's, and the variable starts unset.
    const std::filesystem::path settings = root / "settings.env";
    std::ofstream(settings)
        << "# PRD1 test settings\n"
        << "CUTMACHINE_FFMPEG=" << (root / "from-file" / "ffmpeg").string()
        << '\n';
    setenv("CUTMACHINE_ENV_FILE", settings.c_str(), 1);
    unsetenv("CUTMACHINE_FFMPEG");

    TestLauncher();
    TestMfa(root);
    TestLocate(root);
    TestResolved(root);

    std::filesystem::remove_all(root, ignored);
    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All process launcher tests passed\n";
    return 0;
}
