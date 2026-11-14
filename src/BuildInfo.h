#pragma once

#include <string>

// PRD19 -- ROADMAP.md. What a binary is: the bundle said 0.1, the MCP server
// said 0.1.0, and nothing said which commit either came from -- while the
// bench keeps copies of binaries precisely to know which one ran. One
// number, declared once (CMakeLists.txt's project(VERSION)), and the commit
// the build tree was at.
//
// The commit is read when the binary is built, not when the tree is
// configured (cmake/BuildInfo.cmake): a build tree outlives many commits.
// No build date: it would make two builds of one commit differ.
struct BuildInfo {
    // "MAJOR.MINOR.PATCH".
    std::string version;
    // The short hash of HEAD, or "unknown" when the sources are not a git
    // checkout (an archive) or git could not be run.
    std::string commit;
    // Tracked files differed from that commit when the binary was built, so
    // the hash alone does not name what was compiled. False when the commit
    // is unknown.
    bool dirty = false;
};

// The build this code belongs to. Portable: no file is read and nothing is
// run, the values are compiled in.
const BuildInfo& CurrentBuild();
