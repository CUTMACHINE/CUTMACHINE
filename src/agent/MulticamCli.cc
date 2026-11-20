#include "MulticamCli.h"

#include "Cli.h"
#include "IdResolver.h"
#include "Json.h"
#include "Multicam.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectStorage.h"

#include <cstdint>
#include <cstdlib>
#include <utility>

namespace {

using mcp_json::Value;

constexpr char kAdd[] = "--add-multicam-group";
constexpr char kSet[] = "--set-multicam-angle";
constexpr char kRemove[] = "--remove-multicam-group";
constexpr char kList[] = "--list-multicam-groups";
// F1.5 -- decided on 8 October 2026: a cut through a group.
constexpr char kSplit[] = "--split-multicam-group";

struct Arguments {
    std::vector<std::string> positional;
    std::string timeline;
    std::string name;
    std::string active;
    std::string group;
    std::vector<std::string> angle_names;
};

int Usage(const std::string& detail, std::string& output) {
    return FailCliCommand("ValidationFailed", detail, output, 2);
}

// Only the options a command documents are accepted, so a misspelt flag is an
// error rather than a positional id.
bool Parse(const std::string& command, const std::vector<std::string>& input,
           Arguments& parsed, std::string& error) {
    const bool add = command == kAdd;
    const bool set = command == kSet;
    const bool split = command == kSplit;
    for (size_t index = 0; index < input.size(); ++index) {
        const std::string& token = input[index];
        const bool hasValue = index + 1 < input.size();
        std::string* single = nullptr;
        if (token == "--timeline") {
            single = &parsed.timeline;
        } else if (add && token == "--name") {
            single = &parsed.name;
        } else if ((add || split) && token == "--active") {
            single = &parsed.active;
        } else if (set && token == "--group") {
            single = &parsed.group;
        } else if (add && token == "--angle-name") {
            if (!hasValue) {
                error = "'--angle-name' needs a value";
                return false;
            }
            parsed.angle_names.push_back(input[++index]);
            continue;
        } else if (token.rfind("--", 0) == 0) {
            error = "unknown ";
            error.append(command).append(" option '").append(token) += '\'';
            return false;
        } else {
            parsed.positional.push_back(token);
            continue;
        }
        if (!hasValue || !single->empty()) {
            error = "'" + token + "' needs exactly one value";
            return false;
        }
        *single = input[++index];
    }
    return true;
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= text.size()) {
        const size_t comma = text.find(',', start);
        items.push_back(text.substr(start, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - start));
        if (comma == std::string::npos) {
            break;
        }
        start = comma + 1;
    }
    return items;
}

// A full timeline id, or an unambiguous prefix of one, like every other id.
bool SelectTimeline(const Project& project, const std::string& requested,
                    Ulid& selected, std::string& message) {
    if (requested.empty()) {
        selected = project.active_timeline_id;
        return true;
    }
    if (project.FindTimeline(requested) != nullptr) {
        selected = requested;
        return true;
    }
    std::vector<Ulid> matches;
    for (const DocumentSequence& timeline : project.timelines) {
        if (timeline.id.rfind(requested, 0) == 0) {
            matches.push_back(timeline.id);
        }
    }
    if (matches.size() == 1) {
        selected = matches.front();
        return true;
    }
    message = (matches.empty() ? "unknown timeline_id '"
                               : "ambiguous timeline_id prefix '") +
              requested + "'";
    return false;
}

// What every write command does once it holds a resolved operation: apply it
// on the --apply-op path, then add what the edit did.
int ApplyAndReport(const std::string& projectPath, const Document& before,
                   const Ulid& timelineId, const Operation& operation,
                   std::string& output) {
    Value report;
    const bool described = DescribeMulticamEdit(before, operation, report);
    const int status = ApplyOperationCommand(
        projectPath, SerializeOperation(operation), output, timelineId);
    if (status != 0 || !described) {
        return status;
    }
    Value result;
    std::string parseError;
    if (!Value::Parse(output, result, parseError) || !result.IsObject()) {
        return status;
    }
    result.Set("timeline_id", Value::MakeString(timelineId));
    for (const auto& [key, value] : report.AsObject()) {
        result.Set(key, value);
    }
    output = result.Dump() + '\n';
    return status;
}

}  // namespace

bool IsMulticamCommand(const std::string& command) {
    return command == kAdd || command == kSet || command == kRemove ||
           command == kList || command == kSplit;
}

int MulticamCommand(const std::string& command,
                    const std::vector<std::string>& arguments,
                    std::string& output) {
    if (!IsMulticamCommand(command)) {
        return Usage("unknown multicam command '" + command + "'", output);
    }
    Arguments parsed;
    std::string message;
    if (!Parse(command, arguments, parsed, message)) {
        return Usage(message, output);
    }
    const size_t expected = command == kList ? 1 : command == kSplit ? 3 : 2;
    if (parsed.positional.size() != expected) {
        return Usage(command + " takes " + std::to_string(expected) +
                         " positional argument(s), got " +
                         std::to_string(parsed.positional.size()),
                     output);
    }
    const std::string& projectPath = parsed.positional[0];

    ProjectSessionLock lock;
    if (command != kList) {
        std::string owner;
        if (!lock.Acquire(projectPath, owner, message)) {
            return FailCliCommand(
                "IoError", command + " cannot lock the project: " + message,
                output);
        }
    }
    Project project;
    if (!LoadStoredProject(projectPath, project, message)) {
        return FailCliCommand("ParseError", message, output);
    }
    Ulid timelineId;
    if (!SelectTimeline(project, parsed.timeline, timelineId, message)) {
        return FailCliCommand("UnknownSequence", message, output);
    }
    const Document document = project.MakeDocument(timelineId);

    if (command == kList) {
        Value result = Value::MakeObject();
        result.Set("ok", Value::MakeBool(true));
        const Value groups = DescribeMulticamGroups(document.sequence);
        for (const auto& [key, value] : groups.AsObject()) {
            result.Set(key, value);
        }
        output = result.Dump() + '\n';
        return 0;
    }

    const IdResolver resolver(document);
    const auto resolve = [&](const std::string& field, const std::string& text,
                             Ulid& id) {
        return resolver.Resolve(field, text, id, message);
    };
    EditError error = EditError::None;
    Operation operation = RemoveMulticamGroupOperation{};

    if (command == kAdd) {
        const std::vector<std::string> clips = SplitList(parsed.positional[1]);
        if (!parsed.angle_names.empty() &&
            parsed.angle_names.size() != clips.size()) {
            return Usage("give one --angle-name per clip (" +
                             std::to_string(clips.size()) + " clips, " +
                             std::to_string(parsed.angle_names.size()) +
                             " names)",
                         output);
        }
        std::vector<MulticamAngleRequest> angles;
        for (size_t index = 0; index < clips.size(); ++index) {
            MulticamAngleRequest angle;
            if (!resolve("clip_ids[" + std::to_string(index) + "]",
                         clips[index], angle.clip_id)) {
                return FailCliCommand("ValidationFailed", message, output);
            }
            if (!parsed.angle_names.empty()) {
                angle.name = parsed.angle_names[index];
            }
            angles.push_back(std::move(angle));
        }
        Ulid activeClip;
        if (!parsed.active.empty() &&
            !resolve("active_clip_id", parsed.active, activeClip)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        AddMulticamGroupOperation add;
        if (!BuildAddMulticamGroup(document, parsed.name, angles, activeClip,
                                   add, error, message)) {
            return FailCliCommand(EditErrorName(error), message, output);
        }
        operation = std::move(add);
    } else if (command == kSet) {
        Ulid groupId;
        if (!parsed.group.empty() &&
            !resolve("group_id", parsed.group, groupId)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        Ulid angleOrClip;
        if (parsed.positional[1] != "none" &&
            !resolve("angle_id", parsed.positional[1], angleOrClip)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        SetMulticamActiveAngleOperation set;
        if (!BuildSetMulticamActiveAngle(document, groupId, angleOrClip, set,
                                         error, message)) {
            return FailCliCommand(EditErrorName(error), message, output);
        }
        operation = set;
    } else if (command == kSplit) {
        Ulid groupId;
        if (!resolve("group_id", parsed.positional[1], groupId)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        // The position in frames of the timeline, or as value/rate.
        const std::string& text = parsed.positional[2];
        const size_t slash = text.find('/');
        char* end = nullptr;
        const long long value = std::strtoll(text.c_str(), &end, 10);
        long long rate = 0;
        RationalTime position;
        if (slash == std::string::npos && end != nullptr && *end == '\0' &&
            !text.empty()) {
            position = {value * document.sequence.frame_rate.den,
                        document.sequence.frame_rate.num};
        } else if (slash != std::string::npos && end == text.c_str() + slash) {
            char* rateEnd = nullptr;
            rate = std::strtoll(text.c_str() + slash + 1, &rateEnd, 10);
            if (rateEnd == nullptr || *rateEnd != '\0' || rate <= 0 ||
                rate > INT32_MAX) {
                return Usage(
                    "position '" + text + "' is neither frames nor value/rate",
                    output);
            }
            position = {value, static_cast<int32_t>(rate)};
        } else {
            return Usage(
                "position '" + text + "' is neither frames nor value/rate",
                output);
        }
        Ulid activeAngle;
        if (!parsed.active.empty() &&
            !resolve("active_angle_id", parsed.active, activeAngle)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        BatchOperation batch;
        MulticamCut cut;
        if (!BuildSplitMulticamGroup(document, groupId, position, activeAngle,
                                     batch, cut, error, message)) {
            return FailCliCommand(EditErrorName(error), message, output);
        }
        const int status = ApplyOperationCommand(
            projectPath, SerializeOperation(batch), output, timelineId);
        Value result;
        std::string parseError;
        if (status == 0 && Value::Parse(output, result, parseError) &&
            result.IsObject()) {
            result.Set("timeline_id", Value::MakeString(timelineId));
            result.Set("group_id", Value::MakeString(cut.group_id));
            Value angles = Value::MakeArray();
            for (const Ulid& angle : cut.angle_ids) {
                angles.Push(Value::MakeString(angle));
            }
            result.Set("angle_ids", std::move(angles));
            result.Set("active_angle_id",
                       Value::MakeString(cut.active_angle_id));
            output = result.Dump() + '\n';
        }
        return status;
    } else {
        Ulid groupId;
        if (!resolve("group_id", parsed.positional[1], groupId)) {
            return FailCliCommand("ValidationFailed", message, output);
        }
        if (document.FindMulticamGroup(groupId) == nullptr) {
            return FailCliCommand(
                EditErrorName(EditError::UnknownMulticamGroup),
                "'" + groupId + "' is not a multicam group", output);
        }
        operation = RemoveMulticamGroupOperation{groupId};
    }
    return ApplyAndReport(projectPath, document, timelineId, operation, output);
}
