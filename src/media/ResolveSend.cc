#include "ResolveSend.h"

#include "CommandEnvelope.h"
#include "FfmpegBinary.h"
#include "Json.h"
#include "LocalEnv.h"
#include "ProcessLauncher.h"
#include "ProjectStorage.h"
#include "ToolFiles.h"
#include "UserDirectories.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <system_error>

#ifndef CUTMACHINE_RESOLVE_BRIDGE_DEFAULT
#define CUTMACHINE_RESOLVE_BRIDGE_DEFAULT ""
#endif

namespace {

using mcp_json::Value;

constexpr const char* kNotFoundPrefix = "Resolve bridge not found: ";

const tool_files::Role kBridge = {
    "sidecar/resolve_bridge.py", kResolveBridgeVariable,
    "../sidecar/resolve_bridge.py", "sidecar/resolve_bridge.py"};

bool IsRegularFile(const std::string& path) {
    std::error_code error;
    return std::filesystem::is_regular_file(path, error);
}

// A name for the descriptor's file: the timeline's, kept to what a file
// name holds, and the time it was sent, so every send stays readable.
std::string DescriptorFileName(const std::string& timeline) {
    std::string stem;
    for (const char c : timeline) {
        const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           (c >= '0' && c <= '9') || c == '-' || c == '_';
        stem += plain ? c : '_';
    }
    if (stem.empty()) stem = "timeline";
    if (stem.size() > 60) stem.resize(60);
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &local);
    return stem + "-" + stamp + ".json";
}

Value LossesValue(const ResolveTimelineExport& exported) {
    Value losses = Value::MakeArray();
    for (const ResolveSendLoss& loss : exported.losses) {
        Value entry = Value::MakeObject();
        entry.Set("what", Value::MakeString(loss.what));
        entry.Set("detail", Value::MakeString(loss.detail));
        entry.Set("count",
                  Value::MakeInt(static_cast<int64_t>(loss.ids.size())));
        Value ids = Value::MakeArray();
        for (const Ulid& id : loss.ids) ids.Push(Value::MakeString(id));
        entry.Set("ids", std::move(ids));
        losses.Push(std::move(entry));
    }
    return losses;
}

}  // namespace

ResolveBridgeSettings LocateResolveBridge() {
    ResolveBridgeSettings settings;
    tool_files::Context context;
    context.executable_directory = ffmpeg::ExecutableDirectory();
    std::error_code pathError;
    const std::filesystem::path working =
        std::filesystem::current_path(pathError);
    if (!pathError) context.working_directory = working.string();
    std::string problem;
    const tool_files::File bridge =
        tool_files::Locate(kBridge, local_env::Value(kResolveBridgeVariable),
                           CUTMACHINE_RESOLVE_BRIDGE_DEFAULT, context, problem);
    if (!bridge.Found()) {
        settings.refusal =
            tool_files::Refusal(kNotFoundPrefix, {problem}, {&kBridge},
                                local_env::LocalEnvFilePath());
        return settings;
    }
    settings.bridge = bridge.path;
    const std::string python = local_env::Value(kResolvePythonVariable);
    if (!python.empty()) {
        settings.python = python;
    } else if (IsRegularFile("/usr/bin/python3")) {
        settings.python = "/usr/bin/python3";
    } else {
        settings.python = "python3";
    }
    return settings;
}

bool SendTimelineToResolve(Document document,
                           const std::string& packageDirectory,
                           const ResolveSendRequest& request,
                           const ResolveBridgeSettings& settings,
                           std::string& resultJson, std::string& errorName,
                           std::string& error) {
    AbsoluteLibraryPaths(document, packageDirectory);
    ResolveTimelineExport exported;
    if (!BuildResolveTimelineExport(document, exported, error,
                                    request.options)) {
        errorName = "InvalidExport";
        return false;
    }
    std::filesystem::path directory = request.directory;
    if (directory.empty())
        directory = local_env::Value(kResolveSendDirectoryVariable);
    if (directory.empty()) {
        const std::string data = user_directories::ApplicationData();
        if (data.empty()) {
            errorName = "IoError";
            error =
                "no folder to write the Resolve descriptor in: HOME is "
                "not set";
            return false;
        }
        directory = std::filesystem::path(data) / "resolve-send";
    }
    std::error_code fileError;
    std::filesystem::create_directories(directory, fileError);
    const std::filesystem::path descriptor =
        directory /
        DescriptorFileName(request.name.empty() ? exported.name : request.name);
    {
        std::ofstream output(descriptor, std::ios::binary | std::ios::trunc);
        output << SerializeResolveTimelineExport(exported);
        if (!output) {
            errorName = "IoError";
            error =
                "cannot write the Resolve descriptor at " + descriptor.string();
            return false;
        }
    }

    Value answer = Value::MakeObject();
    answer.Set("ok", Value::MakeBool(true));
    answer.Set("descriptor", Value::MakeString(descriptor.string()));
    if (request.dry_run) {
        answer.Set("dry_run", Value::MakeBool(true));
        answer.Set("timeline",
                   Value::MakeString(request.name.empty() ? exported.name
                                                          : request.name));
        int64_t video = 0;
        for (const ResolveTimelineClip& clip : exported.clips)
            if (!clip.audio) ++video;
        answer.Set("video_clips", Value::MakeInt(video));
        answer.Set("audio_clips",
                   Value::MakeInt(static_cast<int64_t>(exported.clips.size()) -
                                  video));
        answer.Set(
            "transitions",
            Value::MakeInt(static_cast<int64_t>(exported.transitions.size())));
        answer.Set(
            "markers",
            Value::MakeInt(static_cast<int64_t>(exported.markers.size())));
        answer.Set(
            "subtitles",
            Value::MakeInt(static_cast<int64_t>(exported.subtitle_cues)));
        answer.Set("multicam_angles_not_sent",
                   Value::MakeInt(exported.multicam_angles_not_sent));
        answer.Set("not_sent", LossesValue(exported));
        resultJson = answer.Dump();
        errorName.clear();
        error.clear();
        return true;
    }

    if (!settings.refusal.empty()) {
        errorName = "ResolveBridgeMissing";
        error = settings.refusal;
        return false;
    }
    process::Request run;
    run.arguments = {settings.python, settings.bridge, "--send",
                     descriptor.string()};
    if (!request.name.empty()) {
        run.arguments.push_back("--name");
        run.arguments.push_back(request.name);
    }
    run.standard_output = process::Stream::kPipe;
    run.standard_error = process::Stream::kPipe;
    run.discard_input = true;
    std::string output;
    run.on_output = [&output](const char* bytes, size_t count) {
        output.append(bytes, count);
    };
    const auto started = std::chrono::steady_clock::now();
    const auto deadline =
        started + std::chrono::seconds(request.timeout_seconds);
    bool timedOut = false;
    run.cancelled = [&]() {
        if (std::chrono::steady_clock::now() < deadline) return false;
        timedOut = true;
        return true;
    };
    const process::Result result = process::Run(run);
    if (!result.Started()) {
        errorName = "ResolveBridgeMissing";
        error = "cannot start " + settings.python +
                " for the Resolve bridge: " + result.SystemErrorText();
        return false;
    }
    if (timedOut) {
        errorName = "ResolveTimeout";
        error = "Resolve stopped answering after " +
                std::to_string(request.timeout_seconds) +
                " s: a dialog is probably open in it, and someone has to "
                "close it. The descriptor is at " +
                descriptor.string();
        return false;
    }
    if (!result.Succeeded()) {
        errorName = "ResolveSendFailed";
        std::string tail = result.error_tail;
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' '))
            tail.pop_back();
        error =
            tail.empty() ? "the Resolve bridge failed without a word" : tail;
        return false;
    }
    Value report;
    std::string parseError;
    // The report is the last line of the bridge's standard output.
    std::string line = output;
    while (!line.empty() && line.back() == '\n') line.pop_back();
    const size_t newline = line.rfind('\n');
    if (newline != std::string::npos) line = line.substr(newline + 1);
    if (!Value::Parse(line, report, parseError) || !report.IsObject()) {
        errorName = "ResolveSendFailed";
        error = "the Resolve bridge answered no report (" + parseError +
                "): " + output.substr(0, 400);
        return false;
    }
    for (const auto& [key, value] : report.AsObject()) {
        if (key != "ok") answer.Set(key, value);
    }
    answer.Set("not_sent", LossesValue(exported));
    answer.Set("multicam_angles_not_sent",
               Value::MakeInt(exported.multicam_angles_not_sent));
    // The bridge's whole run, Python's start and Resolve's connection
    // included: beside its timings_ms, what the steps leave unaccounted.
    answer.Set(
        "elapsed_ms",
        Value::MakeInt(std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - started)
                           .count()));
    std::string summary = result.error_tail;
    while (!summary.empty() && summary.back() == '\n') summary.pop_back();
    const size_t lastLine = summary.rfind('\n');
    if (lastLine != std::string::npos) summary = summary.substr(lastLine + 1);
    if (!summary.empty()) answer.Set("summary", Value::MakeString(summary));
    resultJson = answer.Dump();
    errorName.clear();
    error.clear();
    return true;
}

namespace {

// The French name of what did not travel, by the loss's kind.
std::string LossLabel(const std::string& what) {
    static const std::pair<const char*, const char*> kLabels[] = {
        {"animations", "images clés (le plan garde son cadrage de départ)"},
        {"crops", "recadrages"},
        {"blurs", "flous"},
        {"masks", "masques"},
        {"speed_ramps", "rampes de vitesse (envoyées à leur vitesse moyenne)"},
        {"motion_clips", "plans de motion (leur place reste vide)"},
        {"caption_tracks", "pistes de légendes au-delà de la première"},
        {"caption_style", "style des cartons"},
        {"look_lut", "look de la séquence"},
        {"grades", "étalonnage"},
        {"grades_without_cdl", "étalonnages sans forme CDL"},
        {"dip_colors", "fondus par une autre couleur que le noir"},
        {"fades_rounded", "fondus du son arrondis à l'image"},
        {"marker_colors", "couleurs de marqueur approchées"},
        {"markers_merged", "marqueurs fusionnés (un par image)"},
        {"transitions", "transitions vers un plan non envoyé"},
    };
    for (const auto& [key, label] : kLabels)
        if (what == key) return label;
    return what;
}

int64_t IntField(const Value& object, const char* key) {
    const Value* field = object.Find(key);
    int64_t value = 0;
    if (field != nullptr && field->IsNumber()) field->AsInt64(value);
    return value;
}

std::string Plural(int64_t count, const std::string& word) {
    return std::to_string(count) + " " + word + (count > 1 ? "s" : "");
}

}  // namespace

ResolveSendNotice ResolveSendNoticeFor(bool sent, const std::string& resultJson,
                                       const std::string& errorName,
                                       const std::string& error) {
    ResolveSendNotice notice;
    if (!sent) {
        notice.failed = true;
        notice.status = "Envoi dans Resolve impossible";
        notice.title = "Envoi dans Resolve impossible";
        if (errorName == "ResolveTimeout") {
            notice.text =
                "Resolve ne répond plus : une fenêtre y est sans doute "
                "ouverte. Ferme-la dans Resolve, puis renvoie.\n\n" +
                error;
        } else if (errorName == "ResolveBridgeMissing") {
            notice.text =
                "Le pont vers Resolve est introuvable "
                "(sidecar/resolve_bridge.py, ou CUTMACHINE_RESOLVE_BRIDGE)."
                "\n\n" +
                error;
        } else if (errorName == "InvalidExport") {
            notice.text =
                "Le montage ne peut pas être décrit pour Resolve ; rien n'a "
                "été créé.\n\n" +
                error;
        } else {
            // The bridge's own sentence, already in French.
            notice.text = error;
        }
        return notice;
    }
    Value answer;
    std::string parseError;
    if (!Value::Parse(resultJson, answer, parseError) || !answer.IsObject()) {
        notice.status = "Envoyé dans Resolve";
        return notice;
    }
    const Value* timeline = answer.Find("timeline");
    const std::string name =
        timeline != nullptr && timeline->IsString() ? timeline->AsString() : "";
    const Value* verified = answer.Find("verified");
    const bool clean =
        verified != nullptr && verified->IsBool() && verified->AsBool();
    const double seconds =
        static_cast<double>(IntField(answer, "elapsed_ms")) / 1000.0;
    char duration[32];
    std::snprintf(duration, sizeof duration, "%.1f", seconds);
    std::string secondsText(duration);
    std::replace(secondsText.begin(), secondsText.end(), '.', ',');
    notice.status = "Envoyé dans Resolve : « " + name + " », " +
                    Plural(IntField(answer, "video_clips"), "plan") +
                    " image, " +
                    Plural(IntField(answer, "audio_clips"), "plan") +
                    " son, en " + secondsText + " s" +
                    (clean ? ", relu sans écart" : ", avec des écarts");

    std::string text;
    if (const Value* mismatches = answer.Find("mismatches");
        mismatches != nullptr && mismatches->IsArray() &&
        !mismatches->AsArray().empty()) {
        text += "Relu dans Resolve, différent du montage :\n";
        for (const Value& line : mismatches->AsArray())
            if (line.IsString()) text += "• " + line.AsString() + "\n";
        text += "\n";
    }
    if (const Value* losses = answer.Find("not_sent");
        losses != nullptr && losses->IsArray() && !losses->AsArray().empty()) {
        text += "Pas envoyé (l'API de Resolve ne le porte pas) :\n";
        for (const Value& loss : losses->AsArray()) {
            const Value* what = loss.Find("what");
            if (what == nullptr || !what->IsString()) continue;
            text += "• " + LossLabel(what->AsString()) + " : " +
                    std::to_string(IntField(loss, "count")) + "\n";
        }
    }
    if (!text.empty()) {
        notice.title = clean ? "Envoyé dans Resolve, avec des manques"
                             : "Envoyé dans Resolve, avec des écarts";
        while (!text.empty() && text.back() == '\n') text.pop_back();
        notice.text = text;
    }
    return notice;
}

int SendResolveCommand(const std::string& projectPath,
                       const std::string& timelineId,
                       const ResolveSendRequest& request, std::string& output) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        return FailCliCommand("InvalidDocument", error, output);
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected)) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + selected + "'", output);
    }
    std::string errorName;
    std::string result;
    if (!SendTimelineToResolve(
            project.MakeDocument(selected),
            std::filesystem::absolute(projectPath).parent_path().string(),
            request, LocateResolveBridge(), result, errorName, error)) {
        return FailCliCommand(errorName, error, output);
    }
    output = result + "\n";
    return 0;
}
