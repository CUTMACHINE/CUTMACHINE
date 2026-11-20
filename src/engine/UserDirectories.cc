#include "UserDirectories.h"

#include <pwd.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>

namespace user_directories {

std::string Home() {
    if (const char* value = std::getenv("HOME");
        value != nullptr && *value != '\0') {
        return value;
    }
    if (const passwd* entry = ::getpwuid(::getuid());
        entry != nullptr && entry->pw_dir != nullptr) {
        return entry->pw_dir;
    }
    return {};
}

std::string ApplicationData(const std::string& home, System system) {
    if (home.empty()) {
        return {};
    }
    const std::filesystem::path root(home);
    return (system == System::kMacOS
                ? root / "Library" / "Application Support" / "CUTMACHINE"
                : root / ".local" / "share" / "cutmachine")
        .string();
}

std::string ApplicationData() { return ApplicationData(Home(), kThisSystem); }

std::string Logs(const std::string& home, System system) {
    if (home.empty()) {
        return {};
    }
    const std::filesystem::path root(home);
    return (system == System::kMacOS ? root / "Library" / "Logs" / "CUTMACHINE"
                                     : root / ".local" / "state" / "cutmachine")
        .string();
}

std::string Logs() { return Logs(Home(), kThisSystem); }

}  // namespace user_directories
