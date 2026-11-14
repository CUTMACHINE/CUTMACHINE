// PRD19 -- ROADMAP.md. What a build says it is (BuildInfo.h): the project's
// one version number, and the commit of the checkout it was built in.
//
// Portable: no AppKit, no Metal, no FFmpeg. The values are compiled in, so
// this checks their shape and that the version is CMakeLists.txt's; whether
// the commit is the right one is checked where it is read, by
// cutmachine_build_info_script_tests on a repository it makes.

#include "BuildInfo.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

bool IsDigit(unsigned char character) { return std::isdigit(character) != 0; }

bool IsLowercaseHex(unsigned char character) {
    return IsDigit(character) || (character >= 'a' && character <= 'f');
}

bool AllOf(const std::string& text, bool (*accepts)(unsigned char)) {
    return !text.empty() && std::all_of(text.begin(), text.end(), accepts);
}

// "N.N.N": three runs of digits, no sign, no suffix.
bool IsThreePartVersion(const std::string& version) {
    std::vector<std::string> parts(1);
    for (const char character : version) {
        if (character == '.') {
            parts.emplace_back();
        } else {
            parts.back().push_back(character);
        }
    }
    return parts.size() == 3 &&
           std::all_of(parts.begin(), parts.end(), [](const std::string& part) {
               return AllOf(part, IsDigit);
           });
}

}  // namespace

int main() {
    const BuildInfo& build = CurrentBuild();

    Check(IsThreePartVersion(build.version),
          "the version has the form N.N.N: '" + build.version + "'");
    Check(build.version == CUTMACHINE_PROJECT_VERSION,
          "the binary's version is the project's, declared once in "
          "CMakeLists.txt: '" +
              build.version + "' against '" CUTMACHINE_PROJECT_VERSION "'");

    // git's own bounds for an abbreviated hash: four characters at least,
    // the whole SHA-1 or SHA-256 at most.
    const bool known = build.commit != "unknown";
    Check(!known || (AllOf(build.commit, IsLowercaseHex) &&
                     build.commit.size() >= 4 && build.commit.size() <= 64),
          "the commit is 'unknown' or a hexadecimal hash: '" + build.commit +
              "'");
    Check(known || !build.dirty,
          "a build with no commit to compare with is not called dirty");

    Check(&CurrentBuild() == &build && CurrentBuild().commit == build.commit,
          "the build is described once, and the same way every time");

    Check(IsThreePartVersion("0.1.0") && IsThreePartVersion("12.0.345") &&
              !IsThreePartVersion("0.1") && !IsThreePartVersion("0.1.0.2") &&
              !IsThreePartVersion("0.1.x") && !IsThreePartVersion("0..1") &&
              !IsThreePartVersion("v0.1.0") && !IsThreePartVersion(""),
          "the form checked is three runs of digits and nothing else");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "All build info tests passed: " << build.version << ' '
              << build.commit << (build.dirty ? " (dirty)" : "") << '\n';
    return 0;
}
