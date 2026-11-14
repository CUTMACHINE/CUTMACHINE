#pragma once

// DB3, OR2 -- ROADMAP.md. Keep run evidence in the package's existing
// multi-file transaction, so an unavailable SQL archive cannot break replay.

#include "RunStep.h"

bool CommitStoredRunStep(const std::string& projectPath,
                         const RunStepRequest& request, Project& committed,
                         EditLog& timelineLog, std::string& resultJson,
                         std::string& errorName, std::string& message,
                         const Project* liveProject = nullptr,
                         const EditLog* liveLog = nullptr,
                         const ProjectEditLog* liveProjectLog = nullptr);
bool ReadStoredRunStep(const std::string& projectPath, const Ulid& stepId,
                       std::string& resultJson, std::string& errorName,
                       std::string& message);
