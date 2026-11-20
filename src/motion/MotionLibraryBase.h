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
// CH3 -- docs/roadmap/CHARTE-2026-11.md (DEC60). For kits only, the inbox is
// no longer one-way: the engine writes charters too, and keeps
// kits/<name>.json equal to the latest version in both directions (rule R
// and the write order, at the end of this file). Pieces keep MB10.
//
// Portable: plain C++ and SQLite (tests/motion_templates_tests.cc).

#include "MotionKitEdit.h"
#include "MotionTemplates.h"

#include <cstddef>
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
//
// CH3 -- the kit files of the inbox are imported by rule R (below), each
// re-read under the base's lock, rather than by "a content the base never
// held": a kit file edited by hand back to the bytes of an older version
// is a new version, not ignored. A kit file R finds diverged lands in
// `issues` with its reason, and the base's latest version of that kit is
// still listed. FindMotionLibraryKit reads through here, and so behaves
// the same. Piece files keep MB10.
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

// CH3 -- docs/roadmap/CHARTE-2026-11.md. Charters written by the engine:
// by the panel (CH6), by the agent (CH4), and edited by hand in
// kits/<name>.json all the same (DEC60). A charter is not state of the
// project document (DEC6: a project keeps the values, not the kit), so
// its writes are library writes, like SaveMotionPieceVersion, outside the
// EditLog: undoing one restores an older version as a new one (DEC61).
//
// Rule R -- kits only, and the one place where kits/<name>.json is read
// into the base. It runs inside one BEGIN IMMEDIATE transaction and reads
// the file again under that lock (never bytes read before it began), then
// compares it with the latest version of that name only:
//   1. no file, or none of that exact name -- kits/ has no entry spelled
//      <name>.json byte for byte, though a disk that ignores case (macOS's
//      by default) would open kits/marque.json for kits/Marque.json: Absent,
//      so that one kit never reads or imports another's file;
//   2. not a regular file (a directory, a symbolic link: renaming over a
//      link would replace the link, not its target), larger than a kit
//      can be (16 MiB), or unreadable: Diverged;
//   3. the file's bytes equal the latest version's content, byte for byte
//      and before any parsing: Matches;
//   4. ParseMotionKit refuses the bytes: Diverged, with its reason;
//   5. otherwise the bytes, as they are, become version latest + 1, with
//      the file's path as source: Matches.
// R replaces, for kits, the import by "a hash the base never held" (Import
// in MotionLibraryBase.cc), so ReadMotionLibraryBase, FindMotionLibraryKit
// and every function below see the same thing; pieces keep MB10.
//
// The write order -- SaveMotionKitVersion, EditMotionKitVersion and
// RestoreMotionKitVersion, after checking what can be checked without the
// base (a name IsMotionPieceName refuses, and for a save ParseMotionKit of
// DumpMotionKit(file): InvalidKit):
//   1. BEGIN IMMEDIATE; a name that differs only by case from another kit
//      name of the base, or from an entry of kits/, is refused (InvalidKit,
//      ROLLBACK): on a disk that ignores case both would be one file, and
//      each write would replace the other kit's. The rule holds on every
//      disk, so that a library behaves the same wherever it is copied.
//      Then rule R on the kit's file;
//   2. Diverged: ROLLBACK, KitFileDiverged with the file's reason in the
//      message and its path as `path`: a hand edit in progress is never
//      overwritten, and nothing is written;
//   3. `expected_version` is not the latest version (0 when there is none):
//      COMMIT, which keeps a version R's step 5 just imported, and
//      StaleKit, `saved.version` receiving the latest version: the writer
//      read something older, and reads again;
//   4. the new content is DumpMotionKit of the new tree. Equal to
//      DumpMotionKit of the latest version's tree (a reformatted file is
//      not a change; a latest version that is not JSON, or no version at
//      all, is never equal): no version is added (`created` false,
//      `version` the latest), and the file is written only when R found
//      it Absent; COMMIT;
//   5. otherwise the new content is inserted as latest + 1, written to
//      `.<name>.json.tmp-<pid>-<sequence>` beside the file (a leading dot,
//      so no inbox reader takes it for a kit), fsync'd, renamed over
//      kits/<name>.json (kits/ created when missing), then COMMIT. A
//      failure before the rename removes the temporary file, ROLLBACK,
//      IoError. A COMMIT that fails after the rename leaves the file one
//      version ahead of the base: the next R imports it (step 5), so the
//      two agree again without anyone choosing. The rewritten file keeps
//      the permissions of the one it replaces; a hard link to the old file
//      is not kept (the rename puts another file under the name, and the
//      link keeps the old bytes). Temporary files of this kit that a write
//      cut short left behind are removed once an hour old, never another
//      kit's.
// A person who saves the file between step 1 and the rename loses that
// save to the rename; the window is one write long, and accepted. Without
// an inbox (`location.inbox` empty) R finds every file Absent and only the
// base is written; `saved.path` is then empty.

// What R found of kits/<name>.json.
enum class MotionKitFileState { Absent, Matches, Diverged };

// One version of a kit, as the base holds it.
struct MotionKitVersion {
    // 0 when the kit has no version yet (a file R found diverged before any
    // version existed).
    int64_t version = 0;
    // The bytes stored: those of the file R imported, or DumpMotionKit of
    // what the engine wrote.
    std::string content;
    // The file's path for an import; "save_motion_kit", "edit_motion_kit",
    // "panel" or "restore v<k>" for a write (the caller's `source`).
    std::string source;
    // As the base writes it: "2026-11-16T10:00:00.000Z".
    std::string created_at;
    // ParseMotionKit's refusal of `content` under this binary, empty when
    // it parses: an older version is listed, and restorable, even when a
    // later binary reads it differently.
    std::string issue;
};

// A kit as the panel and read_motion_kit show it.
struct MotionKitState {
    MotionKitVersion latest;
    // `latest.content` parsed, `version` set; only `name` when
    // `latest.issue` is not empty.
    MotionKitInfo kit;
    // kits/<name>.json, whether it exists or not; empty without an inbox.
    std::string path;
    MotionKitFileState file = MotionKitFileState::Absent;
    // Diverged: why, said as a person reads it (ParseMotionKit's reason,
    // "not a regular file"...); empty otherwise.
    std::string file_issue;
    // What R imported while reading: "kit <name> v<n>", with the file.
    std::vector<MotionLibraryIssue> imported;
};

// What a write did.
struct MotionKitSaved {
    // The version written, or the latest one when none was (`created`
    // false); on StaleKit, the latest version.
    int64_t version = 0;
    bool created = false;
    // The latest version's content, which is the file's bytes after the
    // write.
    std::string content;
    // kits/<name>.json.
    std::string path;
    // What R imported before writing.
    std::vector<MotionLibraryIssue> imported;
};

// Every kit name the library knows, sorted, once each: the names of the
// base, those whose latest version no longer parses included (the panel
// lists them, to show their JSON and restore an older version), and those
// of the kit files of the inbox, a file R finds diverged included. R runs
// first on every kit file, as ReadMotionLibraryBase runs it. A file whose
// name is not a kit name (IsMotionPieceName) is not listed.
bool ListMotionKitNames(const MotionLibraryLocation& location,
                        std::vector<std::string>& names, MotionKitError& error);

// The kit `name` now: R on kits/<name>.json alone, then the latest
// version. UnknownKit when the base holds no version of it and there is
// no file; a diverged file without any version is a state too
// (`latest.version` 0), so that the panel can say what is wrong with it.
bool ReadMotionKitState(const MotionLibraryLocation& location,
                        const std::string& name, MotionKitState& state,
                        MotionKitError& error);

// The versions of `name`, newest first, at most `limit` (0: all), after R
// on its file. UnknownKit when there is none.
bool ListMotionKitVersions(const MotionLibraryLocation& location,
                           const std::string& name, size_t limit,
                           std::vector<MotionKitVersion>& versions,
                           MotionKitError& error);

// `file` as the next version of `name`, by the write order above.
// `expected_version` is the version the writer read; 0 creates the kit,
// and is stale when the kit exists (an inbox file R imports counts).
// `source` is kept with the version.
bool SaveMotionKitVersion(const MotionLibraryLocation& location,
                          const std::string& name, const mcp_json::Value& file,
                          int64_t expected_version, const std::string& source,
                          MotionKitSaved& saved, MotionKitError& error);

// The latest version of `name` with `edits` made (EditMotionKit), saved by
// the write order above, all in the one transaction: nothing can come
// between the version read and the version written. InvalidKit when the
// latest version is not JSON or the edited charter is refused;
// InvalidEdit for an edit outside the grammar.
bool EditMotionKitVersion(const MotionLibraryLocation& location,
                          const std::string& name,
                          const std::vector<MotionKitEdit>& edits,
                          int64_t expected_version, const std::string& source,
                          MotionKitSaved& saved, MotionKitError& error);

// The content of version `version` of `name`, as DumpMotionKit spells it,
// saved as a new version (source "restore v<version>") by the write order
// above: versions never change, so going back is going forward. Works
// whatever the latest version is, one that no longer parses included: it
// is how a broken charter comes back. UnknownKit for a version the base
// does not hold; InvalidKit when that version no longer parses.
bool RestoreMotionKitVersion(const MotionLibraryLocation& location,
                             const std::string& name, int64_t version,
                             int64_t expected_version, MotionKitSaved& saved,
                             MotionKitError& error);
