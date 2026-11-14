// HEADLESS-2026-10 H4 -- docs/roadmap/HEADLESS-2026-10.md. Where the engine
// keeps this user's data and log (UserDirectories.h), on both systems from
// one host: macOS keeps what it had, Linux no longer grows a ~/Library.
//
// Plain C++: the home is given, except for Home() itself.

#include "UserDirectories.h"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void CheckEqual(const std::string& actual, const std::string& expected,
                const std::string& message) {
    Check(actual == expected,
          message + ": '" + actual + "', expected '" + expected + "'");
}

}  // namespace

int main() {
    using user_directories::System;

    // macOS: the paths the archive, the motion library and the log had
    // before this ticket, byte for byte.
    CheckEqual(user_directories::ApplicationData("/Users/someone", System::kMacOS),
               "/Users/someone/Library/Application Support/CUTMACHINE",
               "macOS data stay in Application Support");
    CheckEqual(user_directories::Logs("/Users/someone", System::kMacOS),
               "/Users/someone/Library/Logs/CUTMACHINE",
               "macOS logs stay in ~/Library/Logs");

    // Elsewhere: beside the motion inbox, and the log where a Linux user
    // looks for one.
    CheckEqual(user_directories::ApplicationData("/home/someone", System::kOther),
               "/home/someone/.local/share/cutmachine",
               "Linux data go under ~/.local/share");
    CheckEqual(user_directories::Logs("/home/someone", System::kOther),
               "/home/someone/.local/state/cutmachine",
               "Linux logs go under ~/.local/state");

    // No home, no place: the callers then write nothing rather than guess.
    Check(user_directories::ApplicationData("", System::kMacOS).empty() &&
              user_directories::ApplicationData("", System::kOther).empty() &&
              user_directories::Logs("", System::kMacOS).empty() &&
              user_directories::Logs("", System::kOther).empty(),
          "an empty home gives no directory");

    // $HOME first, and the process's own system.
    ::setenv("HOME", "/tmp/cutmachine-user-directories-home", 1);
    CheckEqual(user_directories::Home(),
               "/tmp/cutmachine-user-directories-home", "Home() reads $HOME");
    CheckEqual(user_directories::Logs(),
               user_directories::Logs("/tmp/cutmachine-user-directories-home",
                                      user_directories::kThisSystem),
               "Logs() is this system's, under $HOME");
    // An empty $HOME is no home: the user database answers instead.
    ::setenv("HOME", "", 1);
    Check(!user_directories::Home().empty(),
          "an empty $HOME falls back to the user database");

    if (failures == 0) {
        std::cout << "user_directories_tests: ok\n";
    }
    return failures == 0 ? 0 : 1;
}
