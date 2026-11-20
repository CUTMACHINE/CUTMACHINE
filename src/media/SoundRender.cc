#include "SoundRender.h"

#include "FfmpegBinary.h"
#include "ProcessLauncher.h"
#include "Ulid.h"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {

using mcp_json::Value;

bool ReadText(const std::string& path, const char* what, std::string& text,
              std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = std::string("cannot read the ") + what + " '" + path + "'";
        return false;
    }
    std::ostringstream content;
    content << file.rdbuf();
    text = content.str();
    return true;
}

// The last line of the host's error output, {"ok":false,"error":"..."}: its
// message.
std::string HostRefusal(const std::string& tail) {
    std::string line = tail;
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
        line.pop_back();
    }
    const size_t start = line.rfind('\n');
    if (start != std::string::npos) {
        line = line.substr(start + 1);
    }
    Value refusal;
    std::string parseError;
    if (Value::Parse(line, refusal, parseError) && refusal.IsObject()) {
        const Value* error = refusal.Find("error");
        if (error != nullptr && error->IsString()) {
            return error->AsString();
        }
    }
    return line;
}

}  // namespace

bool BuildSoundJob(const Document& document, const SoundRenderRequest& request,
                   Value& job, std::string& error) {
    job = Value::MakeObject();
    std::string code;
    if (!ReadText(request.ditty_path, "ditty", code, error)) {
        return false;
    }
    job.Set("code", Value::MakeString(code));
    job.Set("filename",
            Value::MakeString(
                std::filesystem::path(request.ditty_path).filename().string()));
    if (!request.partition_path.empty()) {
        std::string text;
        Value partition;
        std::string parseError;
        if (!ReadText(request.partition_path, "partition", text, error)) {
            return false;
        }
        if (!Value::Parse(text, partition, parseError)) {
            error = "the partition '" + request.partition_path +
                    "' is not JSON: " + parseError;
            return false;
        }
        job.Set("partition", std::move(partition));
        job.Set("partitionFile",
                Value::MakeString(std::filesystem::path(request.partition_path)
                                      .filename()
                                      .string()));
    }
    job.Set("seed", Value::MakeInt(request.seed));
    if (!request.settings.IsObject()) {
        error = "settings must be an object of input names";
        return false;
    }
    for (const auto& [name, value] : request.settings.AsObject()) {
        if (!value.IsNumber() && !value.IsString()) {
            error = "setting '" + name + "' must be a number or a text";
            return false;
        }
    }
    job.Set("inputs", request.settings);
    job.Set("sampleRate", Value::MakeInt(request.sample_rate));
    if (!request.tail.IsNull()) {
        job.Set("tail", request.tail);
    }
    job.Set("stems", Value::MakeBool(!request.stems_dir.empty()));
    job.Set("triggers", Value::MakeBool(request.triggers));

    // The timeline as the document holds it now, read by the renderer's own
    // rules (project.mjs).
    Value timeline;
    std::string parseError;
    if (!Value::Parse(document.SaveToString(), timeline, parseError)) {
        error = "the document does not read back as JSON: " + parseError;
        return false;
    }
    Value project = Value::MakeObject();
    project.Set("json", std::move(timeline));
    if (!request.track_id.empty()) {
        project.Set("track", Value::MakeString(request.track_id));
    }
    if (!request.start_timecode.empty()) {
        project.Set("start", Value::MakeString(request.start_timecode));
    }
    job.Set("project", std::move(project));
    error.clear();
    return true;
}

std::string SoundHostPath() {
    if (const char* configured = std::getenv("CUTMACHINE_SOUND_HOST");
        configured != nullptr && *configured != '\0') {
        return configured;
    }
    const std::string directory = ffmpeg::ExecutableDirectory();
    if (directory.empty()) {
        return {};
    }
    const std::filesystem::path host =
        std::filesystem::path(directory) / "cutmachine-sound-host";
    std::error_code missing;
    return std::filesystem::is_regular_file(host, missing) ? host.string()
                                                           : std::string();
}

bool RenderSound(const Document& document, const SoundRenderRequest& request,
                 std::string& reportJson, std::string& errorName,
                 std::string& message) {
    reportJson.clear();
    const std::string host = SoundHostPath();
    std::error_code absent;
    if (host.empty() || !std::filesystem::is_regular_file(host, absent)) {
        errorName = "SoundHostMissing";
        message =
            (host.empty()
                 ? std::string("cutmachine-sound-host is not beside the engine")
                 : "no cutmachine-sound-host at '" + host + "'") +
            ": sound renders through JavaScriptCore on macOS only. Without it, "
            "render with `node tools/cutmachine-sound/cli.mjs render <ditty> "
            "--project <project>`";
        return false;
    }
    Value job;
    if (!BuildSoundJob(document, request, job, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    Value hostRequest = Value::MakeObject();
    hostRequest.Set("job", std::move(job));
    hostRequest.Set("out", Value::MakeString(request.out_path));
    hostRequest.Set("stems_dir", request.stems_dir.empty()
                                     ? Value::MakeNull()
                                     : Value::MakeString(request.stems_dir));

    std::error_code ignored;
    const std::filesystem::path requestPath =
        std::filesystem::temp_directory_path(ignored) /
        ("cutmachine-sound-" + GenerateUlid() + ".json");
    {
        std::ofstream file(requestPath, std::ios::binary);
        file << hostRequest.Dump();
        if (!file) {
            errorName = "IoError";
            message = "cannot write the render's request '" +
                      requestPath.string() + "'";
            return false;
        }
    }

    process::Request launch;
    launch.arguments = {host, requestPath.string()};
    launch.standard_output = process::Stream::kPipe;
    launch.standard_error = process::Stream::kPipe;
    launch.discard_input = true;
    std::string output;
    launch.on_output = [&](const char* bytes, size_t count) {
        output.append(bytes, count);
    };
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(request.timeout_seconds);
    launch.cancelled = [deadline] {
        return std::chrono::steady_clock::now() > deadline;
    };
    const process::Result result = process::Run(launch);
    std::filesystem::remove(requestPath, ignored);

    if (result.cancelled) {
        errorName = "SoundTimeout";
        message = "the render was stopped after " +
                  std::to_string(request.timeout_seconds) + " s (timeout)";
        return false;
    }
    if (!result.Started()) {
        errorName = "SoundHostMissing";
        message = "cannot start '" + host + "': " + result.SystemErrorText();
        return false;
    }
    if (!result.Succeeded()) {
        errorName = "SoundRenderFailed";
        message = HostRefusal(result.error_tail);
        if (message.empty()) {
            message = result.Signalled()
                          ? "the host stopped on signal " +
                                std::to_string(result.TerminationSignal())
                          : "the host exited with status " +
                                std::to_string(result.ExitStatus());
        }
        return false;
    }
    Value report;
    std::string parseError;
    if (!Value::Parse(output, report, parseError) || !report.IsObject()) {
        errorName = "IoError";
        message = "the host answered no report: " + parseError;
        return false;
    }
    reportJson = report.Dump();
    errorName.clear();
    message.clear();
    return true;
}
