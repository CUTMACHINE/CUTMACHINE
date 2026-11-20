#pragma once

// The files of a project package, and how they are written durably: the
// journals' paths beside a project, the reading and the synchronized writing
// of a whole file, and the transaction that publishes several artifacts at
// once (SAVING_ROADMAP.md, S2 and S4).
//
// These used to live in Cli.cc, so that the storage of a project
// (ProjectStorage.cc, RunStepStorage.cc) and the importers included the
// command line's header to reach them: the model depended on
// one of its surfaces (PHILOSOPHY.md, principle 3). They depend on nothing
// above the edit log.

#include "EditLog.h"

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

std::string TimelineEditLogPathForProject(const std::string& projectPath,
                                          const std::string& timelineId);
std::string ProjectEditLogPathForProject(const std::string& projectPath);
// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md. The project's journal as stored
// in its package, for list_motifs to read; a project never edited at the
// project level has none, and `log` is left empty.
bool LoadProjectEditLogIfAny(const std::string& projectPath,
                             ProjectEditLog& log, EditError& error,
                             std::string& message);

// A whole file read into `contents`.
bool ReadTextFile(const std::string& path, std::string& contents,
                  std::string& message);
// A whole file written and synchronized to storage (F_FULLFSYNC where the
// filesystem has it), not renamed into place: the caller decides that.
bool WriteDurableFile(const std::filesystem::path& path,
                      const std::string& contents, std::string& message);

// Transactional primitive for storage layouts that add package-local
// artifacts. All destinations must be regular files on the same filesystem.
bool CommitTextArtifacts(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    std::string& message);

// S2 -- SAVING_ROADMAP.md. Recovers an interrupted transaction rooted in
// directory and removes leftovers whose durable commit marker is already gone.
bool RecoverTextArtifactTransaction(const std::string& directory,
                                    std::string& message);

// S4 -- SAVING_ROADMAP.md. Project packages use this variant so removal of
// obsolete canonical files belongs to the generation being published.
bool CommitTextArtifactsAndRemove(
    const std::vector<std::pair<std::string, std::string>>& artifacts,
    const std::vector<std::string>& removals, std::string& message);
