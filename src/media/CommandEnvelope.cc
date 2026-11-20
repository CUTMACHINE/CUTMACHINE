#include "CommandEnvelope.h"

#include "FfmpegBinary.h"
#include "Json.h"
#include "MfaAlignment.h"
#include "ProjectStorage.h"
#include "SpeakerDiarization.h"

int FailCliCommand(const std::string& errorCode, const std::string& detail,
                   std::string& output, int exitStatus) {
    return FailCliCommandWithResult(errorCode, detail, {}, output, exitStatus);
}

int FailCliCommandWithResult(const std::string& errorCode,
                             const std::string& detail,
                             const std::string& resultJson, std::string& output,
                             int exitStatus) {
    mcp_json::Value result;
    std::string parseError;
    const bool hasResult =
        !resultJson.empty() &&
        mcp_json::Value::Parse(resultJson, result, parseError) &&
        result.IsObject();
    // PRD1 -- a command that needed FFmpeg and had none says so by name,
    // whichever code its caller gave the sentence it relayed.
    // PRD20 -- and so does one whose project path leads nowhere: IoError,
    // as for any file that is not there, where each command used to give
    // the name of its own load failure (ParseError, InvalidDocument).
    std::string named = ffmpeg::RefusalName(detail);
    // PRD21 -- and so does one that needed MFA and found it nowhere.
    if (named.empty()) {
        named = MfaRefusalName(detail);
    }
    // PA3 -- and so does one that needed the diarization model.
    if (named.empty()) {
        named = DiarizationRefusalName(detail);
    }
    if (named.empty() && IsMissingProjectMessage(detail)) {
        named = "IoError";
    }
    output = "{\"ok\":false,\"error\":\"" +
             mcp_json::EscapeJsonString(named.empty() ? errorCode : named) +
             "\",\"detail\":\"" + mcp_json::EscapeJsonString(detail) + "\"" +
             (hasResult ? ",\"result\":" + result.Dump() : "") + "}\n";
    return exitStatus;
}

bool ReadCliCommandResult(const std::string& output, bool commandSucceeded,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) {
    mcp_json::Value value;
    std::string parseError;
    if (!mcp_json::Value::Parse(output, value, parseError)) {
        errorName = "IoError";
        message = "malformed command output: " + parseError;
        return false;
    }
    if (!commandSucceeded) {
        const mcp_json::Value* errorField = value.Find("error");
        const mcp_json::Value* detailField = value.Find("detail");
        errorName = (errorField != nullptr && errorField->IsString())
                        ? errorField->AsString()
                        : "InvalidOperation";
        message = (detailField != nullptr && detailField->IsString())
                      ? detailField->AsString()
                      : "operation failed";
        // PRD20 -- what a refusal carries beside its sentence
        // (FailCliCommandWithResult) reaches the caller too: a tool's
        // outcome attaches it as `result`, so the files an ingest could not
        // read are named over MCP as they are on the command line.
        const mcp_json::Value* resultField = value.Find("result");
        if (resultField != nullptr && resultField->IsObject()) {
            resultJson = resultField->Dump();
        }
        return false;
    }
    resultJson = output;
    return true;
}

std::string WithCliUsage(const std::string& output, int exitStatus,
                         const std::string& usage) {
    mcp_json::Value value;
    std::string parseError;
    if (exitStatus != 2 || usage.empty() ||
        !mcp_json::Value::Parse(output, value, parseError) ||
        !value.IsObject()) {
        return output;
    }
    const mcp_json::Value* error = value.Find("error");
    const mcp_json::Value* detail = value.Find("detail");
    if (error == nullptr || !error->IsString() ||
        error->AsString() != "ValidationFailed" || detail == nullptr ||
        !detail->IsString() ||
        detail->AsString().find("usage: ") != std::string::npos) {
        return output;
    }
    const mcp_json::Value* result = value.Find("result");
    std::string decorated;
    FailCliCommandWithResult(
        error->AsString(), detail->AsString() + "; " + usage,
        result != nullptr && result->IsObject() ? result->Dump()
                                                : std::string(),
        decorated, exitStatus);
    return decorated;
}

std::string WithCliOk(const std::string& output) {
    constexpr char kSpace[] = " \t\r\n";
    const size_t open = output.find_first_not_of(kSpace);
    if (open == std::string::npos || output[open] != '{') {
        return output;
    }
    const size_t first = output.find_first_not_of(kSpace, open + 1);
    if (first == std::string::npos || output.compare(first, 4, "\"ok\"") == 0) {
        return output;
    }
    return output.substr(0, open + 1) + "\"ok\":true" +
           (output[first] == '}' ? "" : ",") + output.substr(open + 1);
}
