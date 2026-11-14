#pragma once

// MB10 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The machine's motion
// library kept in the user's personal SQLite base, beside the project
// archive (ProjectArchive.h) but in a file of its own: the archive is
// append-only and may be turned off, the library is where a user ranges
// what they reuse. One base per user, on their machine: a client's pieces
// and charter never travel to anyone else. The pieces the binary ships stay
// in the binary (MotionTemplates.h), the same on every machine.
//
// A piece or a kit is a row: its file as JSON (motion-piece.v1,
// motion-kit.v1), its name, a version, where it came from, when. Saving
// under a name that exists adds the next version and keeps the others;
// rows never change once written. The latest version of each name is what
// the shelf lists.
//
// JSON stays the format of exchange. The library folder of MB1 is now an
// inbox: every piece file in it, and every kit file in its kits/ folder,
// whose content the base does not hold yet for that name, enters as the
// next version when the library is read. A generator writes a file there;
// nothing in the folder is ever changed or removed by the engine. The
// folder of a machine that had a library before MB10 is imported the same
// way, on the first read.
//
// Portable: plain C++ and SQLite (tests/motion_templates_tests.cc).

#include "MotionTemplates.h"

#include <cstdint>
#include <string>
#include <vector>

// Where this user's library lives.
struct MotionLibraryLocation {
    // The SQLite base; created on first use.
    std::string base;
    // The folder whose piece files (and kits/ files) are imported.
    std::string inbox;
};

// $CUTMACHINE_MOTION_LIBRARY (LocalEnv.h) names a folder: the base is
// library.sqlite in it, and the folder is the inbox. Unset: the base is
// ~/Library/Application Support/CUTMACHINE/library.sqlite on macOS and
// ~/.local/share/cutmachine/library.sqlite elsewhere (UserDirectories.h),
// beside the project archive, and the inbox
// ~/.local/share/cutmachine/motion-library, where MB1 kept the library. Both
// empty without a home.
MotionLibraryLocation CurrentMotionLibraryLocation();

struct MotionLibraryContents {
    // The latest version of each piece, by name; `version` set.
    std::vector<MotionTemplateInfo> pieces;
    // The latest version of each kit, by name; `version` set.
    std::vector<MotionKitInfo> kits;
    // Files of the inbox that are neither pieces nor kits, names the binary
    // ships, a base that cannot be opened: said, never in silence, never a
    // reason to hide the rest.
    std::vector<MotionLibraryIssue> issues;
    // What this read imported from the inbox: "piece <name> v<n>" and
    // "kit <name> v<n>", each with the file it came from.
    std::vector<MotionLibraryIssue> imported;
};

// Imports the inbox's new files, then reads the latest version of every
// piece and kit. A base that cannot be opened falls back to the inbox
// alone (unversioned) and says why in `issues`.
void ReadMotionLibraryBase(const MotionLibraryLocation& location,
                           const std::vector<MotionTemplateInfo>& shipped,
                           MotionLibraryContents& contents);

// DEC6 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md. The latest version
// of kit `name` in this user's library (the inbox read first), for a tool
// that takes a brand's charter by name: add_caption_style, check_timeline.
// False with `error` naming the kits the library holds, and why a file of
// that name is not one when there is such a file.
bool FindMotionLibraryKit(const MotionLibraryLocation& location,
                          const std::string& name, MotionKitInfo& kit,
                          std::string& error);

// `piece` as the next version of its name. When the name is in the library
// already and `overwrite` is false, nothing is written and `existed` is
// true; with `overwrite`, the new version is added and the older ones kept.
// `version` receives the version written.
bool SaveMotionPieceVersion(const MotionLibraryLocation& location,
                            const MotionTemplateInfo& piece, bool overwrite,
                            int64_t& version, bool& existed,
                            std::string& error);
