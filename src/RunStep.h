#pragma once

// DB3, OR2 -- docs/roadmap/DONNEES-2026-10.md. A graph checkpoint cannot
// prove that an edit landed. The package commits this receipt together with
// the montage, its journals and its archive outbox. Retrying compares the
// request and the saved state instead of applying a second edit.

#include "EditPlan.h"
#include "Project.h"

#include <string>

struct RunStepRequest {
    Ulid run_id;
    Ulid step_id;
    std::string stage;
    Ulid timeline_id;
    std::string base_revision;
    std::string base_history_hash;
    std::string base_project_history_hash;
    std::string plan_json;
    std::string observations_json = "{}";
};

struct RunStepReceipt {
    std::string format = "cutmachine.run-step.v1";
    Ulid run_id;
    Ulid step_id;
    std::string stage;
    Ulid project_id;
    Ulid timeline_id;
    Ulid snapshot_id;
    std::string base_revision;
    std::string result_revision;
    std::string base_history_hash;
    std::string timeline_history_json;
    std::string project_history_json;
    std::string request_json;
};

inline constexpr size_t kMaxRunStepBytes = size_t{16} * 1024 * 1024;

bool ValidateRunStepRequest(const RunStepRequest& request,
                            std::string& message);

bool ParseRunStepRequest(const std::string& json, RunStepRequest& request,
                         std::string& message);
std::string SerializeRunStepRequest(const RunStepRequest& request);
bool ParseRunStepReceipt(const std::string& json, RunStepReceipt& receipt,
                         std::string& message);
std::string SerializeRunStepReceipt(const RunStepReceipt& receipt);

// Pure application through the existing plan and EditLog. An empty plan
// records an observation boundary without changing the montage.
bool ApplyRunStep(Project& project, EditLog& log, const RunStepRequest& request,
                  std::string& errorName, std::string& message);
bool RunStepMatchesState(const RunStepReceipt& receipt, const Project& project,
                         const EditLog& log, const ProjectEditLog& projectLog);
