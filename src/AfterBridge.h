#pragma once

// AFTER-COMP -- After Effects is an output adapter, never another owner of
// project state. Requests are validated here, then a thin host runner executes
// one JSX job and returns a versioned JSON response. No Document is read or
// mutated on this path.

#include "Json.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

inline constexpr char kAfterRequestSchema[] = "after-request.v2";
inline constexpr char kAfterResponseSchema[] = "after-response.v2";

enum class AfterBridgeMethod {
    CreateProject,
    OpenProjectCopy,
    DescribeProject,
    ListComps,
    ListFootage,
    DescribeComp,
    RenderFrame,
    SetText,
    SetProperty,
    SetLayerState,
    ReplaceFootage,
    ImportFootage,
    AddLayer,
    DuplicateLayer,
    SaveProject,
    // MA1 -- docs/roadmap/MOTION-AFTER-2026-09.md. A new project built from
    // an after-motion.v1 description (MotionAfterExport.h) and saved.
    BuildMotionProject,
};

struct AfterBridgeRequest {
    AfterBridgeMethod method = AfterBridgeMethod::DescribeProject;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
};

struct AfterBridgeResponse {
    bool ok = false;
    std::string project_name;
    std::string path;
    std::optional<int64_t> item_count;
    std::string after_version;
    mcp_json::Value result;
    std::string error;
    std::string detail;
};

const char* AfterBridgeMethodName(AfterBridgeMethod method);

// Strict protocol helpers: unknown fields and method-specific path mistakes
// are rejected. Successful parsing never consults the filesystem or launches
// After Effects, which keeps protocol tests deterministic.
bool ParseAfterBridgeRequest(const std::string& json,
                             AfterBridgeRequest& request, std::string& error);
std::string SerializeAfterBridgeRequest(const AfterBridgeRequest& request);
bool ParseAfterBridgeResponse(const std::string& json,
                              AfterBridgeResponse& response,
                              std::string& error);
std::string SerializeAfterBridgeResponse(const AfterBridgeResponse& response);

struct AfterBridgeInvocation {
    AfterBridgeRequest request;
    std::string script_name;
    std::string job_script;
    std::string response_path;
};

// Injected by tests. Production passes an empty runner and uses the native
// macOS transport. A runner returns transport success separately from the
// response's own `ok`, so malformed output and an After refusal stay distinct.
using AfterBridgeRunner = std::function<bool(
    const AfterBridgeInvocation& invocation, std::string& responseJson,
    std::string& errorName, std::string& message)>;

// Parses one after-request.v2 value, validates its local path preconditions,
// assembles the matching JSX job, executes it, validates after-response.v2 and
// returns a canonical JSON response. On a response with ok:false, errorName
// and message carry the After error for MCP's ordinary isError envelope.
bool RunAfterBridgeRequest(const std::string& requestJson,
                           std::string& responseJson, std::string& errorName,
                           std::string& message,
                           const AfterBridgeRunner& runner = {});
