#pragma once

// HEADLESS-2026-10 H4 -- docs/roadmap/HEADLESS-2026-10.md. Where the engine
// keeps what belongs to this user rather than to a project: the archive of
// projects (ProjectArchive.h), the motion library's base
// (MotionLibraryBase.h), the engine log (EngineLog.h).
//
// The three were written for macOS, under ~/Library, and the build without
// a window (headless/) put a ~/Library folder in a Linux home. Elsewhere
// they go where the engine already kept the rest on every system -- the
// motion inbox under ~/.local/share/cutmachine, the settings file under
// ~/.config/cutmachine -- and the log under
// ~/.local/state/cutmachine, the place for one. XDG_DATA_HOME and
// XDG_STATE_HOME are not read, as those paths do not read them either: one
// variable honoured by half the engine would split a user's data in two.
// Each place keeps its own override (CUTMACHINE_PROJECT_ARCHIVE,
// CUTMACHINE_MOTION_LIBRARY, CUTMACHINE_LOG_DIR).
//
// Plain C++ and POSIX.

#include <string>

namespace user_directories {

enum class System { kMacOS, kOther };

// The system this process runs on.
#if defined(__APPLE__)
inline constexpr System kThisSystem = System::kMacOS;
#else
inline constexpr System kThisSystem = System::kOther;
#endif

// $HOME, else the user database; empty when neither answers.
std::string Home();

// The engine's own data: ~/Library/Application Support/CUTMACHINE on macOS,
// ~/.local/share/cutmachine elsewhere. Empty when `home` is.
std::string ApplicationData(const std::string& home, System system);
std::string ApplicationData();

// The engine log's folder: ~/Library/Logs/CUTMACHINE on macOS,
// ~/.local/state/cutmachine elsewhere. Empty when `home` is.
std::string Logs(const std::string& home, System system);
std::string Logs();

}  // namespace user_directories
