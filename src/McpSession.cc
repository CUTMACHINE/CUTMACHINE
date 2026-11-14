#include "McpSession.h"

#include "CanonicalHash.h"
#include "Export.h"
#include "IdResolver.h"
#include "MotionDocument.h"
#include "Project.h"
#include "TimelineTranscription.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <set>
#include <utility>

using mcp_json::Value;

// IA5 -- the default names the state the backend can see: the whole project
// when it has one (the same hash project edits return as `project_hash`),
// else its document.
bool McpBackend::Revision(std::string& revision, std::string& message) {
    Project project;
    std::string ignored;
    if (SnapshotProject(project, ignored)) {
        revision = CanonicalHash(project.SaveToString());
        return true;
    }
    Document document;
    if (!SnapshotDocument(document, message)) {
        return false;
    }
    revision = CanonicalHash(document.SaveToString());
    return true;
}

namespace {

// What a task read, named so that "the same inputs" is a string comparison.
struct TaskInputs {
    std::string tool;
    Ulid timeline_id;
    std::vector<Ulid> media_ids;
    // PRD16 -- what an export reads: the timelines it encodes, or the
    // motion composition render_motion_video draws.
    std::vector<Ulid> timeline_ids;
    Ulid composition_id;
};

// PRD16 -- ROADMAP.md. The tools whose task writes a file the caller named:
// they work on the project pinned when they were asked for (StartTask).
bool IsExportTask(const std::string& tool) {
    return tool == "export_timeline" || tool == "export_timelines" ||
           tool == "render_motion_video";
}

// PRD17 -- ROADMAP.md. What the export of one timeline reads of its
// project, and nothing else: the timeline itself, the colour settings, the
// rushes its clips name, the compositions its motion clips show and all
// they reach. PRD16 hashed the timeline's whole Document, rushes and bins
// included: in a series, an agent ingesting a rush or filing one in a bin
// for its own film marked every other film's export stale.
std::string TimelineReadIdentity(const Project& project, const Ulid& id) {
    if (project.FindTimeline(id) == nullptr) {
        return "absent";
    }
    const Document whole = project.MakeDocument(id);
    Document read;
    read.sequence = whole.sequence;
    read.color_management = whole.color_management;
    std::set<Ulid> media;
    std::set<Ulid> compositions;
    for (const DocumentTrack& track : whole.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.source_id.empty()) media.insert(clip.source_id);
            if (!clip.composition_id.empty())
                compositions.insert(clip.composition_id);
        }
    }
    for (const LibraryMedia& item : whole.library)
        if (media.count(item.id) != 0) read.library.push_back(item);
    for (const DocumentSource& source : whole.sources)
        if (media.count(source.id) != 0) read.sources.push_back(source);
    std::string reach;
    for (const Ulid& composition : compositions) {
        if (const DocumentComposition* shown =
                whole.FindComposition(composition)) {
            read.compositions.push_back(*shown);
            reach += MotionReachKey(whole, composition);
        }
    }
    return CanonicalHash(read.SaveToString() + '\n' + reach);
}

// PRD16 -- what an export reads, named. For a timeline, what its plan is
// built from (TimelineReadIdentity). For a composition rendered alone, its
// definition and everything it reaches (MotionReachKey). A change elsewhere
// in the project -- another timeline, another composition, another rush --
// leaves an export current.
std::string ExportReadIdentity(const Project& project,
                               const TaskInputs& inputs) {
    std::string identity;
    for (const Ulid& id : inputs.timeline_ids) {
        identity += id + ':' + TimelineReadIdentity(project, id) + ';';
    }
    if (!inputs.composition_id.empty()) {
        const Document document = project.MakeActiveDocument();
        const DocumentComposition* composition =
            document.FindComposition(inputs.composition_id);
        identity +=
            inputs.composition_id + ':' +
            (composition == nullptr
                 ? std::string("absent")
                 : CanonicalHash(
                       composition->definition.Dump() + '\n' +
                       MotionReachKey(document, inputs.composition_id))) +
            ';';
    }
    return identity;
}

struct TaskIdentity {
    std::string revision;
    std::string inputs;
};

std::string TimeText(const RationalTime& time) {
    return std::to_string(time.value) + '/' + std::to_string(time.rate);
}

// The audible arrangement for a timeline transcription (the key its cache is
// stored under), each rush's file and timebase otherwise. A task's result is
// only as good as these; anything else in the project may move freely.
TaskIdentity Identify(McpBackend& backend, const TaskInputs& inputs) {
    TaskIdentity identity;
    std::string message;
    if (!backend.Revision(identity.revision, message)) {
        identity.revision = "unavailable: " + message;
    }
    Project project;
    const bool hasProject = backend.SnapshotProject(project, message);
    if (IsExportTask(inputs.tool)) {
        identity.inputs = hasProject ? ExportReadIdentity(project, inputs)
                                     : "unavailable: " + message;
        return identity;
    }
    if (inputs.tool == "transcribe_timeline") {
        Document document;
        if (hasProject) {
            if (project.FindTimeline(inputs.timeline_id) == nullptr) {
                identity.inputs = "timeline absent";
                return identity;
            }
            document = project.MakeDocument(inputs.timeline_id);
        } else if (!backend.SnapshotDocument(document, message) ||
                   document.sequence.id != inputs.timeline_id) {
            identity.inputs = "timeline absent";
            return identity;
        }
        TimelineAudioPlan plan;
        identity.inputs =
            BuildTimelineAudioPlan(document, "project.cutmachine.json", plan,
                                   message)
                ? plan.cache_identity
                : "no audible audio: " + message;
        return identity;
    }
    Document document;
    if (hasProject) {
        document = project.MakeActiveDocument();
    } else if (!backend.SnapshotDocument(document, message)) {
        identity.inputs = "unavailable: " + message;
        return identity;
    }
    for (const Ulid& id : inputs.media_ids) {
        identity.inputs += id + ':';
        const LibraryMedia* media = document.FindLibraryMedia(id);
        const DocumentSource* source = document.FindSource(id);
        if (media == nullptr || source == nullptr) {
            identity.inputs += "absent;";
            continue;
        }
        identity.inputs += media->path + '|' + source->path + '|' +
                           std::to_string(source->rate.num) + '/' +
                           std::to_string(source->rate.den) + '|' +
                           TimeText(source->duration) + '|' +
                           (media->has_audio ? "audio" : "mute") + '|' +
                           (media->has_video ? "video" : "sound") + ';';
    }
    return identity;
}

std::string StaleReason(const TaskInputs& inputs) {
    if (inputs.tool == "render_motion_video") {
        return "composition " + inputs.composition_id +
               ", or a composition or a media it shows, changed while it was "
               "rendered: the files are of the composition as it stood when "
               "the render was asked for";
    }
    if (IsExportTask(inputs.tool)) {
        std::string timelines;
        for (const Ulid& id : inputs.timeline_ids) {
            timelines += (timelines.empty() ? "" : ", ") + id;
        }
        return "timeline " + timelines +
               ", or a rush, a setting or a composition it reads, changed "
               "while the export ran: the file is of the montage as it stood "
               "when the export was asked for";
    }
    if (inputs.tool == "transcribe_timeline") {
        return "the audible arrangement of timeline " + inputs.timeline_id +
               " changed while the task ran: this transcript is of the "
               "arrangement it started from, not of the current one";
    }
    return "a rush this task analysed was relinked, re-probed or removed "
           "while it ran: its cached analysis may not match the rush the "
           "project now names";
}

// Key order does not make two calls different.
std::string CanonicalJson(const Value& value) {
    if (value.IsObject()) {
        std::vector<std::pair<std::string, const Value*>> fields;
        for (const auto& entry : value.AsObject()) {
            fields.emplace_back(entry.first, &entry.second);
        }
        std::sort(fields.begin(), fields.end(),
                  [](const auto& left, const auto& right) {
                      return left.first < right.first;
                  });
        std::string text = "{";
        for (size_t index = 0; index < fields.size(); ++index) {
            if (index != 0) {
                text += ',';
            }
            text += '"' + mcp_json::EscapeJsonString(fields[index].first) +
                    "\":" + CanonicalJson(*fields[index].second);
        }
        return text + '}';
    }
    if (value.IsArray()) {
        std::string text = "[";
        for (size_t index = 0; index < value.AsArray().size(); ++index) {
            if (index != 0) {
                text += ',';
            }
            text += CanonicalJson(value.AsArray()[index]);
        }
        return text + ']';
    }
    return value.Dump();
}

// OR2 -- a tool can already carry session metadata (a saved run receipt's
// revision, a task's cancellation flag). Replace it instead of making a
// duplicate key, which strict JSON readers reject. Keep the usual large
// read's bytes untouched when no replacement is needed.
void PrependMember(std::string& objectJson, const std::string& key,
                   const Value& value) {
    const size_t open = objectJson.find_first_not_of(" \t\r\n");
    if (open == std::string::npos || objectJson[open] != '{') {
        return;
    }
    const std::string encodedKey = '"' + mcp_json::EscapeJsonString(key) + '"';
    if (objectJson.find(encodedKey) != std::string::npos) {
        Value existing;
        std::string ignored;
        if (!Value::Parse(objectJson, existing, ignored) ||
            !existing.IsObject()) {
            return;
        }
        Value replaced = Value::MakeObject();
        replaced.Set(key, value);
        for (const auto& [name, member] : existing.AsObject()) {
            if (name != key) {
                replaced.Set(name, member);
            }
        }
        objectJson = replaced.Dump();
        return;
    }
    const size_t next = objectJson.find_first_not_of(" \t\r\n", open + 1);
    const bool empty = next != std::string::npos && objectJson[next] == '}';
    objectJson.insert(open + 1,
                      encodedKey + ':' + value.Dump() + (empty ? "" : ","));
}

McpToolCallOutcome Refusal(const std::string& errorName,
                           const std::string& message,
                           const Value* result = nullptr) {
    McpToolCallOutcome outcome;
    outcome.ok = false;
    outcome.error_name = errorName;
    outcome.message = message;
    Value envelope = Value::MakeObject();
    envelope.Set("ok", Value::MakeBool(false));
    envelope.Set("error", Value::MakeString(errorName));
    envelope.Set("detail", Value::MakeString(message));
    if (result != nullptr) {
        envelope.Set("result", *result);
    }
    outcome.result_json = envelope.Dump();
    return outcome;
}

McpToolCallOutcome Success(const std::string& resultJson) {
    McpToolCallOutcome outcome;
    outcome.ok = true;
    outcome.result_json = resultJson;
    return outcome;
}

const char* StateName(MediaTaskState state) {
    switch (state) {
        case MediaTaskState::Queued:
            return "queued";
        case MediaTaskState::Running:
            return "running";
        case MediaTaskState::Succeeded:
            return "succeeded";
        case MediaTaskState::Failed:
            return "failed";
        case MediaTaskState::Cancelled:
            return "cancelled";
    }
    return "failed";
}

MediaTaskKind KindOf(const std::string& tool) {
    if (tool == "analyze_speech_onset") {
        return MediaTaskKind::SpeechOnset;
    }
    if (tool == "analyze_shot_quality") {
        return MediaTaskKind::ShotQuality;
    }
    if (tool == "analyze_audio_beats") {
        return MediaTaskKind::Beat;
    }
    if (tool == "analyze_isolated_frames") {
        return MediaTaskKind::IsolatedFrames;
    }
    if (tool == "align_words") {
        return MediaTaskKind::WordAlignment;
    }
    if (tool == "analyze_speakers") {
        return MediaTaskKind::SpeakerDiarization;
    }
    if (tool == "measure_audio_offset") {
        return MediaTaskKind::AudioOffset;
    }
    if (tool == "propose_ducking") {
        return MediaTaskKind::Ducking;
    }
    if (IsExportTask(tool)) {
        return MediaTaskKind::Export;
    }
    return MediaTaskKind::Transcription;
}

// Where the task's context lives while its job runs. The runner is called
// on the worker thread, inside that job, so a plain pointer is enough.
struct RunnerSlot {
    MediaTaskContext* context = nullptr;
};

// Unknown arguments are refused before a task is queued, not an hour later.
bool CheckSchemaKeys(const McpTool& tool, const Value& arguments,
                     std::string& message) {
    Value schema;
    std::string error;
    if (!Value::Parse(tool.input_schema_json, schema, error)) {
        return true;
    }
    const Value* properties = schema.Find("properties");
    for (const auto& entry : arguments.AsObject()) {
        if (properties == nullptr || properties->Find(entry.first) == nullptr) {
            message = "'" + tool.name + "' has unknown argument '" +
                      entry.first + "'";
            return false;
        }
    }
    return true;
}

// PRD16 -- a preset an export task names is checked before the task is
// queued, the way the tool reads it (Exporter::ReadPreset), rather than
// failing once the tasks ahead of it are done.
bool CheckTaskPreset(const Value& holder, const Value& arguments,
                     const std::string& field, std::string& errorName,
                     std::string& message) {
    const Value* preset = holder.Find("preset");
    if (preset == nullptr || !preset->IsString()) {
        return true;
    }
    const Value* codec = arguments.Find("codec");
    ExportPresetId id = ExportPresetId::H264;
    if (!Exporter::ReadPreset(
            preset->AsString(),
            codec != nullptr && codec->IsString() ? codec->AsString() : "",
            field, id, message)) {
        errorName = "ValidationFailed";
        return false;
    }
    return true;
}

// PRD16 -- ROADMAP.md. What an export task reads, resolved on the project
// pinned for it, and refused by name before anything is queued: a timeline
// it does not hold, named with every timeline there is, as the tool names
// them; a composition no id or prefix names; a preset that is no preset, or
// contradicts the codec.
bool ResolveExportInputs(const Project& project, const McpTool& tool,
                         const Value& arguments, TaskInputs& inputs,
                         Value& workerArguments, std::string& errorName,
                         std::string& message) {
    const auto timeline = [&](const Value* value, const std::string& field) {
        if (value == nullptr || !value->IsString() ||
            value->AsString().empty()) {
            errorName = "ValidationFailed";
            message = "'" + field +
                      "' is required: name the timeline to export -- " +
                      Exporter::DescribeTimelines(project);
            return false;
        }
        if (project.FindTimeline(value->AsString()) == nullptr) {
            errorName = "UnknownSequence";
            message = "unknown timeline_id '" + value->AsString() +
                      "'; the project's timelines: " +
                      Exporter::DescribeTimelines(project);
            return false;
        }
        inputs.timeline_ids.push_back(value->AsString());
        return true;
    };
    if (tool.name == "export_timeline") {
        return timeline(arguments.Find("timeline_id"),
                        "export_timeline.timeline_id") &&
               CheckTaskPreset(arguments, arguments, "export_timeline.preset",
                               errorName, message);
    }
    if (tool.name == "export_timelines") {
        const Value* outputs = arguments.Find("outputs");
        if (outputs == nullptr || !outputs->IsArray() ||
            outputs->AsArray().empty()) {
            errorName = "ValidationFailed";
            message =
                "'export_timelines.outputs' must be a nonempty array of "
                "{timeline_id, output_path}";
            return false;
        }
        for (size_t index = 0; index < outputs->AsArray().size(); ++index) {
            const Value& output = outputs->AsArray()[index];
            const std::string field =
                "export_timelines.outputs[" + std::to_string(index) + "]";
            if (!output.IsObject()) {
                errorName = "ValidationFailed";
                message = "'" + field + "' must be an object";
                return false;
            }
            if (!timeline(output.Find("timeline_id"), field + ".timeline_id") ||
                !CheckTaskPreset(output, arguments, field + ".preset",
                                 errorName, message)) {
                return false;
            }
        }
        return true;
    }
    // render_motion_video: the composition by its id or an unambiguous
    // prefix, as the tool resolves it, held by its full id from here on.
    const Value* requested = arguments.Find("composition_id");
    if (requested == nullptr || !requested->IsString() ||
        requested->AsString().empty()) {
        errorName = "ValidationFailed";
        message = "'render_motion_video.composition_id' is required";
        return false;
    }
    const std::string& prefix = requested->AsString();
    std::vector<Ulid> matches;
    for (const DocumentComposition& composition : project.compositions) {
        if (composition.id == prefix) {
            matches = {composition.id};
            break;
        }
        if (composition.id.compare(0, prefix.size(), prefix) == 0) {
            matches.push_back(composition.id);
        }
    }
    if (matches.size() != 1) {
        errorName = matches.empty() ? "UnknownComposition" : "ValidationFailed";
        message = matches.empty() ? "unknown composition_id '" + prefix + "'"
                                  : "composition_id '" + prefix +
                                        "' matches several compositions";
        return false;
    }
    inputs.composition_id = matches.front();
    workerArguments.Set("composition_id",
                        Value::MakeString(inputs.composition_id));
    return true;
}

// Resolves what the task will read against the caller's snapshot, and
// rewrites the arguments with full identifiers: the task works on these
// rushes, or on this timeline, even if the active one changes meanwhile.
// PRD16 -- an export reads `pinned`, the project as it was asked on.
bool ResolveTaskInputs(McpBackend& backend, const McpTool& tool,
                       const Value& arguments, const Project* pinned,
                       TaskInputs& inputs, Value& workerArguments,
                       std::string& errorName, std::string& message) {
    inputs.tool = tool.name;
    workerArguments = arguments;
    if (pinned != nullptr) {
        return ResolveExportInputs(*pinned, tool, arguments, inputs,
                                   workerArguments, errorName, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    if (tool.name == "transcribe_timeline") {
        const Value* requested = arguments.Find("timeline_id");
        if (requested != nullptr && !requested->IsString()) {
            errorName = "ValidationFailed";
            message = "'transcribe_timeline.timeline_id' must be a string";
            return false;
        }
        Project project;
        std::string ignored;
        const bool hasProject = backend.SnapshotProject(project, ignored);
        inputs.timeline_id =
            requested != nullptr && !requested->AsString().empty()
                ? requested->AsString()
            : hasProject ? project.active_timeline_id
                         : document.sequence.id;
        const bool known =
            hasProject ? project.FindTimeline(inputs.timeline_id) != nullptr
                       : document.sequence.id == inputs.timeline_id;
        if (!known) {
            errorName = "UnknownSequence";
            message = "unknown timeline_id '" + inputs.timeline_id + "'";
            return false;
        }
        workerArguments.Set("timeline_id",
                            Value::MakeString(inputs.timeline_id));
        return true;
    }
    const IdResolver resolver(document);
    const auto resolve = [&](const Value& value, const std::string& field,
                             Ulid& id) {
        if (!value.IsString()) {
            errorName = "ValidationFailed";
            message = "'" + field + "' must be a string";
            return false;
        }
        if (!resolver.Resolve(field, value.AsString(), id, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        return true;
    };
    if (const Value* single = arguments.Find("media_id")) {
        Ulid id;
        if (!resolve(*single, tool.name + ".media_id", id)) {
            return false;
        }
        inputs.media_ids.push_back(id);
        workerArguments.Set("media_id", Value::MakeString(id));
    }
    if (const Value* list = arguments.Find("media_ids")) {
        if (!list->IsArray()) {
            errorName = "ValidationFailed";
            message = "'" + tool.name + ".media_ids' must be an array";
            return false;
        }
        Value full = Value::MakeArray();
        for (size_t index = 0; index < list->AsArray().size(); ++index) {
            Ulid id;
            if (!resolve(
                    list->AsArray()[index],
                    tool.name + ".media_ids[" + std::to_string(index) + "]",
                    id)) {
                return false;
            }
            inputs.media_ids.push_back(id);
            full.Push(Value::MakeString(id));
        }
        workerArguments.Set("media_ids", full);
    }
    return true;
}

}  // namespace

struct McpSession::Task {
    Ulid id;
    std::string tool;
    std::string arguments_json;
    std::string requested_revision;
    TaskInputs inputs;
    // Written by the worker, read under the session mutex.
    bool started = false;
    std::string snapshot_revision;
    bool finished = false;
    MediaTaskSnapshot final_state;
    McpToolCallOutcome outcome;
    bool stale = false;
    std::string stale_reason;
};

McpSession::McpSession() = default;

McpSession::~McpSession() {
    if (shared_manager_ != nullptr) {
        // DEC10 -- the shared manager outlives this session, and a task's
        // work and completion reach into it: its tasks are cancelled and
        // waited for, as the own manager's destructor joins its worker.
        for (const Ulid& id : ActiveTaskIds()) shared_manager_->Cancel(id);
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait(lock, [&] {
            for (const auto& entry : tasks_)
                if (!entry.second->finished) return false;
            return true;
        });
    }
    manager_.CancelAll();
}

void McpSession::SetMediaWorkDecorator(WorkDecorator decorator) {
    std::lock_guard<std::mutex> lock(mutex_);
    decorator_ = std::move(decorator);
}

void McpSession::SetTaskManager(MediaTaskManager* shared,
                                std::string labelPrefix) {
    std::lock_guard<std::mutex> lock(mutex_);
    shared_manager_ = shared;
    task_label_prefix_ = std::move(labelPrefix);
}

MediaTaskManager& McpSession::TaskManager() {
    return shared_manager_ != nullptr ? *shared_manager_ : manager_;
}

std::shared_ptr<McpSession::Task> McpSession::FindTask(const Ulid& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = tasks_.find(id);
    return found == tasks_.end() ? nullptr : found->second;
}

std::vector<Ulid> McpSession::ActiveTaskIds() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Ulid> active;
    for (const Ulid& id : task_order_) {
        const auto found = tasks_.find(id);
        if (found != tasks_.end() && !found->second->finished) {
            active.push_back(id);
        }
    }
    return active;
}

void McpSession::PruneFinishedLocked() {
    size_t finished = 0;
    for (const Ulid& id : task_order_) {
        if (tasks_.at(id)->finished) {
            ++finished;
        }
    }
    for (auto id = task_order_.begin();
         id != task_order_.end() && finished > kRetainedFinishedTasks;) {
        if (tasks_.at(*id)->finished) {
            tasks_.erase(*id);
            id = task_order_.erase(id);
            --finished;
        } else {
            ++id;
        }
    }
}

std::string McpSession::TaskView(const Task& task, bool withResult) const {
    MediaTaskSnapshot state;
    bool started = false;
    bool finished = false;
    std::string snapshotRevision;
    McpToolCallOutcome outcome;
    bool stale = false;
    std::string staleReason;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished = task.finished;
        started = task.started;
        state = task.final_state;
        snapshotRevision = task.snapshot_revision;
        outcome = task.outcome;
        stale = task.stale;
        staleReason = task.stale_reason;
    }
    if (!finished) {
        state.state =
            started ? MediaTaskState::Running : MediaTaskState::Queued;
        for (const MediaTaskSnapshot& live :
             const_cast<McpSession*>(this)->TaskManager().Snapshot()) {
            if (live.id == task.id) {
                state = live;
                break;
            }
        }
    }
    Value view = Value::MakeObject();
    view.Set("task_id", Value::MakeString(task.id));
    view.Set("tool", Value::MakeString(task.tool));
    view.Set("state", Value::MakeString(StateName(state.state)));
    char progress[16];
    std::snprintf(
        progress, sizeof(progress), "%.3f",
        state.state == MediaTaskState::Succeeded ? 1.0 : state.progress);
    view.Set("progress", Value::MakeRawNumber(progress));
    if (!state.detail.empty()) {
        view.Set("detail", Value::MakeString(state.detail));
    }
    Value arguments;
    std::string parseError;
    if (Value::Parse(task.arguments_json, arguments, parseError)) {
        view.Set("arguments", arguments);
    }
    view.Set("requested_revision", Value::MakeString(task.requested_revision));
    if (!snapshotRevision.empty()) {
        view.Set("snapshot_revision", Value::MakeString(snapshotRevision));
    }
    if (withResult && finished) {
        Value payload;
        if (Value::Parse(outcome.result_json, payload, parseError)) {
            if (state.state != MediaTaskState::Cancelled) {
                view.Set(outcome.ok ? "result" : "failure", payload);
            } else if (payload.IsObject() &&
                       payload.Find("result") != nullptr) {
                // PRD16 -- a batch stopped by cancel_task lists the files it
                // completed before, which stay on disk.
                view.Set("failure", payload);
            }
        }
    }
    view.Set("stale", Value::MakeBool(stale));
    if (stale) {
        view.Set("stale_reason", Value::MakeString(staleReason));
    }
    return view.Dump();
}

bool McpSession::StartTask(const McpToolRegistry& registry, McpBackend& backend,
                           const McpTool& tool, const Value& arguments,
                           McpToolCallOutcome& outcome, Ulid& taskId) {
    std::string errorName;
    std::string message;
    if (!CheckSchemaKeys(tool, arguments, message)) {
        outcome = Refusal("ValidationFailed", message);
        return false;
    }
    auto task = std::make_shared<Task>();
    task->tool = tool.name;
    task->arguments_json = arguments.Dump();
    // PRD16 -- ROADMAP.md. An export works on the project as it is now, on
    // this thread -- the window's own, through the app's relay -- while the
    // montage goes on: the task's fork holds this copy (PinTaskProject).
    const bool exporting = IsExportTask(tool.name);
    std::shared_ptr<Project> pinned;
    if (exporting) {
        pinned = std::make_shared<Project>();
        if (!backend.SnapshotProject(*pinned, message)) {
            outcome = Refusal("IoError", message);
            return false;
        }
    }
    Value workerArguments;
    if (!ResolveTaskInputs(backend, tool, arguments, pinned.get(), task->inputs,
                           workerArguments, errorName, message)) {
        outcome = Refusal(errorName, message);
        return false;
    }
    backend.Revision(task->requested_revision, message);

    WorkDecorator decorator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        decorator = decorator_;
    }
    auto slot = std::make_shared<RunnerSlot>();
    // The command's media work runs inline, on the task's own context: the
    // progress it reports is the task's, and cancel_task reaches it.
    MediaTaskRunner runner = [slot, decorator](MediaTaskKind kind,
                                               const std::string& label,
                                               MediaTaskManager::Work work) {
        MediaTaskSnapshot result;
        result.kind = kind;
        result.label = label;
        MediaTaskContext* context = slot->context;
        if (context == nullptr) {
            result.state = MediaTaskState::Failed;
            result.error = "media work ran outside its task";
            return result;
        }
        if (decorator) {
            work = decorator(kind, label, std::move(work));
        }
        std::string error;
        bool succeeded = false;
        try {
            succeeded = work(*context, error);
        } catch (const std::exception& exception) {
            error = exception.what();
        }
        if (context->Cancelled()) {
            result.state = MediaTaskState::Cancelled;
        } else if (succeeded) {
            result.state = MediaTaskState::Succeeded;
            result.progress = 1.0;
        } else {
            result.state = MediaTaskState::Failed;
            result.error = error.empty() ? "task failed" : error;
        }
        return result;
    };
    std::unique_ptr<McpBackend> forked = backend.ForkForTask(runner, message);
    if (forked == nullptr) {
        outcome = Refusal("UnsupportedOperation", message);
        return false;
    }
    if (exporting && !forked->PinTaskProject(pinned.get(), message)) {
        outcome = Refusal("UnsupportedOperation", message);
        return false;
    }
    // Released as soon as the task ends: the manager keeps finished tasks,
    // and with them their work, long after.
    auto fork =
        std::make_shared<std::shared_ptr<McpBackend>>(std::move(forked));
    const std::string toolName = tool.name;
    MediaTaskManager::Work work = [this, &registry, fork, slot, task, toolName,
                                   workerArguments](MediaTaskContext& context,
                                                    std::string& error) {
        McpBackend& worker = **fork;
        slot->context = &context;
        // PRD16 -- for an export, the pinned project: what it encodes.
        const TaskIdentity before = Identify(worker, task->inputs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task->started = true;
            task->snapshot_revision = before.revision;
        }
        const McpToolCallOutcome result =
            registry.Call(worker, toolName, workerArguments);
        if (IsExportTask(toolName)) {
            // The package as it is now, against what the file was made of.
            std::string ignored;
            worker.PinTaskProject(nullptr, ignored);
        }
        const TaskIdentity after = Identify(worker, task->inputs);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            task->outcome = result;
            if (result.ok && !context.Cancelled() &&
                before.inputs != after.inputs) {
                task->stale = true;
                task->stale_reason = StaleReason(task->inputs);
            }
        }
        slot->context = nullptr;
        error = result.ok ? std::string() : result.message;
        return result.ok;
    };
    // DEC10 -- the fork goes first, and the waiters are told under the
    // lock: once a waiter sees the task finished, the completion touches
    // nothing of this session again, which a session on a shared manager
    // waits for before it goes (~McpSession).
    MediaTaskManager::Completion completion =
        [this, task, fork](const MediaTaskSnapshot& finished) {
            fork->reset();
            std::lock_guard<std::mutex> lock(mutex_);
            task->final_state = finished;
            task->finished = true;
            PruneFinishedLocked();
            finished_.notify_all();
        };
    {
        // Held across Enqueue so the completion, which prunes, never sees
        // the task missing from task_order_.
        std::lock_guard<std::mutex> lock(mutex_);
        task->id = TaskManager().Enqueue(
            KindOf(tool.name), task_label_prefix_ + tool.name, std::move(work),
            std::move(completion));
        tasks_[task->id] = task;
        task_order_.push_back(task->id);
    }
    taskId = task->id;
    outcome = Success(TaskView(*task, false));
    return true;
}

McpToolCallOutcome McpSession::CallTaskTool(const std::string& toolName,
                                            const Value& arguments) {
    static const Value kEmptyObject = Value::MakeObject();
    const Value& args = arguments.IsNull() ? kEmptyObject : arguments;
    if (!args.IsObject()) {
        return Refusal("ValidationFailed",
                       "'" + toolName + "' arguments must be a JSON object");
    }
    const std::vector<std::string> allowed =
        toolName == "get_task"
            ? std::vector<std::string>{"task_id", "wait_seconds"}
        : toolName == "cancel_task" ? std::vector<std::string>{"task_id"}
                                    : std::vector<std::string>{};
    for (const auto& entry : args.AsObject()) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            return Refusal("ValidationFailed", "'" + toolName +
                                                   "' has unknown argument '" +
                                                   entry.first + "'");
        }
    }
    if (toolName == "list_tasks") {
        std::vector<Ulid> order;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            order = task_order_;
        }
        std::string list = "[";
        for (auto id = order.rbegin(); id != order.rend(); ++id) {
            const std::shared_ptr<Task> task = FindTask(*id);
            if (task == nullptr) {
                continue;
            }
            if (list.size() > 1) {
                list += ',';
            }
            list += TaskView(*task, false);
        }
        return Success("{\"tasks\":" + list + "]}");
    }
    const Value* idField = args.Find("task_id");
    if (idField == nullptr || !idField->IsString() ||
        idField->AsString().empty()) {
        return Refusal("ValidationFailed",
                       "'" + toolName + ".task_id' is required");
    }
    const std::shared_ptr<Task> task = FindTask(idField->AsString());
    if (task == nullptr) {
        return Refusal("UnknownTask",
                       "no task '" + idField->AsString() +
                           "' in this server: it was never started here, or "
                           "the server has restarted since");
    }
    int64_t waitSeconds = 0;
    if (const Value* wait = args.Find("wait_seconds")) {
        if (!wait->AsInt64(waitSeconds) || waitSeconds < 0 ||
            waitSeconds > kMaximumWaitSeconds) {
            return Refusal("ValidationFailed",
                           "'get_task.wait_seconds' must be an integer from 0 "
                           "to 30");
        }
    }
    int64_t waitMilliseconds = waitSeconds * 1000;
    bool cancelRequested = false;
    if (toolName == "cancel_task") {
        cancelRequested = TaskManager().Cancel(task->id);
        waitMilliseconds = kCancelWaitMilliseconds;
    }
    {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_.wait_for(lock, std::chrono::milliseconds(waitMilliseconds),
                           [&] { return task->finished; });
    }
    std::string view = TaskView(*task, true);
    bool finished = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        finished = task->finished;
    }
    if (cancelRequested && !finished) {
        PrependMember(view, "cancel_requested", Value::MakeBool(true));
    }
    return Success(view);
}

McpToolCallOutcome McpSession::Call(const McpToolRegistry& registry,
                                    McpBackend& backend,
                                    const std::string& toolName,
                                    const Value& rawArguments) {
    static const Value kEmptyObject = Value::MakeObject();
    const Value& arguments =
        rawArguments.IsNull() ? kEmptyObject : rawArguments;
    const auto tool = std::find_if(
        registry.Tools().begin(), registry.Tools().end(),
        [&](const McpTool& item) { return item.name == toolName; });
    if (tool == registry.Tools().end() || !arguments.IsObject()) {
        return registry.Call(backend, toolName, arguments);
    }
    if (toolName == "get_task" || toolName == "cancel_task" ||
        toolName == "list_tasks") {
        return CallTaskTool(toolName, arguments);
    }

    // Split what this session consumes from what the tool reads.
    bool async = false;
    std::string key;
    std::string expectedRevision;
    Value toolArguments = Value::MakeObject();
    for (const auto& entry : arguments.AsObject()) {
        const std::string& name = entry.first;
        const bool sessionKey = name == kMcpAsyncKey ||
                                name == kMcpIdempotencyKey ||
                                name == kMcpExpectedRevisionKey;
        if (!sessionKey) {
            toolArguments.Set(name, entry.second);
            continue;
        }
        const bool accepted =
            name == kMcpAsyncKey ? tool->background
            : name == kMcpIdempotencyKey
                ? tool->idempotent
                : tool->idempotent && toolName != "create_project";
        if (!accepted) {
            std::string refusal = "'" + toolName;
            refusal += "' does not take '" + name + "'";
            return Refusal("ValidationFailed", refusal);
        }
        if (name == kMcpAsyncKey) {
            if (!entry.second.IsBool()) {
                return Refusal("ValidationFailed",
                               "'" + toolName + ".async' must be a boolean");
            }
            async = entry.second.AsBool();
        } else if (!entry.second.IsString() ||
                   entry.second.AsString().empty() ||
                   entry.second.AsString().size() > 256) {
            std::string refusal = "'" + toolName;
            refusal += "." + name +
                       "' must be a non-empty string of at most 256 "
                       "characters";
            return Refusal("ValidationFailed", refusal);
        } else if (name == kMcpIdempotencyKey) {
            key = entry.second.AsString();
        } else {
            expectedRevision = entry.second.AsString();
        }
    }

    // PRD14 -- ROADMAP.md. The catalog's own reading of the arguments
    // (McpArguments.h), before this session reads them itself: a background
    // task checks its keys and resolves its rushes on the names the schema
    // declares, and one call written two ways has one fingerprint.
    {
        Value normalized;
        std::string refusal;
        if (!registry.NormalizeArguments(toolName, toolArguments, normalized,
                                         refusal)) {
            return Refusal("ValidationFailed", refusal);
        }
        toolArguments = std::move(normalized);
    }

    // PRD16 -- ROADMAP.md. A relative path names a file in the folder of
    // the project's package, never in this process's working directory: a
    // task gets the absolute path, and so does the answer.
    if (std::any_of(McpCallerPathArguments().begin(),
                    McpCallerPathArguments().end(),
                    [&](const McpCallerPathArgument& entry) {
                        return toolName == entry.tool;
                    }) &&
        backend.HasOpenProject()) {
        ResolveMcpCallerPaths(toolName, backend.ProjectFolder(), toolArguments);
    }

    // A replay comes first: the call it repeats may have been made against
    // a revision that its own success has since moved.
    const std::string fingerprint =
        toolName + (async ? " async\n" : "\n") + CanonicalJson(toolArguments);
    if (!key.empty()) {
        Remembered remembered;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            const auto entry = remembered_.find(key);
            if (entry != remembered_.end()) {
                remembered = entry->second;
                found = true;
            }
        }
        if (found) {
            if (remembered.fingerprint != fingerprint) {
                return Refusal(
                    "IdempotencyKeyReused",
                    "idempotency_key '" + key +
                        "' was already used for a different call; a key "
                        "names one call, choose a new one for this");
            }
            McpToolCallOutcome replay = remembered.outcome;
            if (!remembered.task_id.empty()) {
                if (const std::shared_ptr<Task> task =
                        FindTask(remembered.task_id)) {
                    replay.result_json = TaskView(*task, false);
                }
            }
            PrependMember(replay.result_json, "replayed",
                          Value::MakeBool(true));
            return replay;
        }
    }

    if (!expectedRevision.empty() && backend.HasOpenProject()) {
        std::string current;
        std::string message;
        if (!backend.Revision(current, message)) {
            return Refusal("IoError", message);
        }
        if (current != expectedRevision) {
            Value result = Value::MakeObject();
            result.Set("expected_revision",
                       Value::MakeString(expectedRevision));
            result.Set("current_revision", Value::MakeString(current));
            // PRD17 -- ROADMAP.md. Which grain moved: the caller's timeline,
            // what the timelines share, or only other timelines -- in a
            // series, the common case, where sending the call again at the
            // current revision is all it takes.
            RememberRevision(backend, current);
            RevisionMark seen;
            RevisionMark now;
            bool known = false;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                for (const RevisionMark& mark : revision_marks_) {
                    if (mark.revision == expectedRevision) {
                        seen = mark;
                        known = true;
                    }
                    if (mark.revision == current) {
                        now = mark;
                    }
                }
            }
            const std::string head = "the project is at revision " + current +
                                     ", not " + expectedRevision;
            if (!known || now.revision != current) {
                return Refusal("StaleRevision",
                               head +
                                   ", a revision this server did not answer "
                                   "recently: read the project again before "
                                   "this call",
                               &result);
            }
            Ulid target;
            if (const Value* named = toolArguments.Find("timeline_id");
                named != nullptr && named->IsString()) {
                target = named->AsString();
            } else if (McpToolTargetsOneTimeline(toolName)) {
                target = now.active_timeline_id;
            }
            std::vector<Ulid> changed;
            for (const auto& [id, bytes] : now.timelines) {
                const auto before = seen.timelines.find(id);
                if (before == seen.timelines.end() || before->second != bytes)
                    changed.push_back(id);
            }
            for (const auto& [id, bytes] : seen.timelines) {
                if (now.timelines.count(id) == 0) changed.push_back(id);
            }
            const bool shared = seen.shared != now.shared;
            const bool own =
                !target.empty() && std::find(changed.begin(), changed.end(),
                                             target) != changed.end();
            Value ids = Value::MakeArray();
            std::string others;
            for (const Ulid& id : changed) {
                ids.Push(Value::MakeString(id));
                if (id != target) others += (others.empty() ? "" : ", ") + id;
            }
            result.Set("changed_timeline_ids", ids);
            result.Set("shared_changed", Value::MakeBool(shared));
            std::string detail;
            if (own) {
                detail = head + ": timeline " + target +
                         " itself changed since you read it" +
                         (shared ? ", and what the timelines share too" : "") +
                         "; read it again before this call";
            } else if (shared) {
                detail = head +
                         ": what the timelines share changed since (rushes, "
                         "bins, compositions, colour settings, the timelines "
                         "and which is active); read the project again "
                         "before this call";
            } else {
                detail = head + ", but only other timelines changed since (" +
                         others + ")" +
                         (target.empty()
                              ? std::string()
                              : ": timeline " + target + " is as you read it") +
                         " -- send this call again with expected_revision " +
                         current;
            }
            return Refusal("StaleRevision", detail, &result);
        }
    }

    std::vector<Ulid> active = ActiveTaskIds();
    const bool switching =
        toolName == "create_project" || toolName == "open_project";
    if (!switching) {
        // PRD16 -- an export rewrites no cache: a synchronous analysis is
        // only refused behind a task that may.
        active.erase(std::remove_if(
                         active.begin(), active.end(),
                         [&](const Ulid& id) {
                             const std::shared_ptr<Task> running = FindTask(id);
                             const auto listed = std::find_if(
                                 registry.Tools().begin(),
                                 registry.Tools().end(),
                                 [&](const McpTool& item) {
                                     return running != nullptr &&
                                            item.name == running->tool;
                                 });
                             return listed == registry.Tools().end() ||
                                    !listed->writes_analysis_cache;
                         }),
                     active.end());
    }
    if (!active.empty() &&
        (switching || (tool->writes_analysis_cache && !async))) {
        Value result = Value::MakeObject();
        Value ids = Value::MakeArray();
        for (const Ulid& id : active) {
            ids.Push(Value::MakeString(id));
        }
        result.Set("tasks", ids);
        // PRD17 -- ROADMAP.md. The agent that meets this in a series did not
        // start those tasks: they are named, with what to do.
        Value running = Value::MakeArray();
        const std::string named = RunningTasks(active, running);
        result.Set("running", running);
        // PRD40 -- ROADMAP.md. async: true is offered only to a tool the
        // catalog lets run in the background: analyze_hesitations, told to
        // pass it, was then refused for passing it.
        const std::string busyAdvice =
            tool->background
                ? "run this one with async: true to queue it behind, or wait "
                  "for those with get_task"
                : "wait for those with get_task (wait_seconds), then send "
                  "this call again: '" +
                      toolName + "' does not run in the background";
        return Refusal(
            switching ? "TasksRunning" : "AnalysisBusy",
            switching
                ? "background tasks still run on the open project (" + named +
                      "): wait for them with get_task, or stop them with "
                      "cancel_task, before switching projects"
                : "a background analysis may be rewriting the caches this "
                  "one installs (" +
                      named + "): " + busyAdvice,
            &result);
    }

    McpToolCallOutcome outcome;
    Ulid taskId;
    if (async) {
        if (!backend.HasOpenProject()) {
            return registry.Call(backend, toolName, toolArguments);
        }
        // PRD17 -- strict timeline mode holds for a task, before it is
        // queued: a task resolves the active timeline when none is named.
        if (backend.RequiresExplicitTimeline() &&
            McpCallNeedsTimeline(toolName, toolArguments)) {
            return Refusal("TimelineRequired",
                           McpTimelineRequiredMessage(backend, toolName));
        }
        StartTask(registry, backend, *tool, toolArguments, outcome, taskId);
    } else {
        outcome = registry.Call(backend, toolName, toolArguments);
    }
    if (!outcome.ok) {
        return outcome;
    }

    if (toolName == "create_project" || toolName == "open_project") {
        // Keys name calls made on the project just left, and so do the
        // revisions remembered.
        std::lock_guard<std::mutex> lock(mutex_);
        remembered_.clear();
        remembered_order_.clear();
        revision_marks_.clear();
    }
    std::string revision;
    std::string message;
    if (backend.HasOpenProject() && backend.Revision(revision, message)) {
        PrependMember(outcome.result_json, "revision",
                      Value::MakeString(revision));
        RememberRevision(backend, revision);
    }
    if (!key.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        remembered_[key] = {fingerprint, outcome, taskId};
        remembered_order_.push_back(key);
        while (remembered_order_.size() > kRetainedIdempotencyKeys) {
            remembered_.erase(remembered_order_.front());
            remembered_order_.pop_front();
        }
    }
    return outcome;
}

bool McpSession::MarkRevision(McpBackend& backend, const std::string& revision,
                              RevisionMark& mark) {
    Project project;
    std::string message;
    if (!backend.SnapshotProject(project, message)) {
        return false;
    }
    mark.revision = revision;
    mark.active_timeline_id = project.active_timeline_id;
    for (const DocumentSequence& timeline : project.timelines) {
        Document alone;
        alone.sequence = timeline;
        mark.timelines[timeline.id] = CanonicalHash(alone.SaveToString());
    }
    project.timelines.clear();
    mark.shared = CanonicalHash(project.SaveToString());
    return true;
}

// Only when the revision moved: a read leaves it where it was, and costs
// nothing here.
void McpSession::RememberRevision(McpBackend& backend,
                                  const std::string& revision) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!revision_marks_.empty() &&
            revision_marks_.back().revision == revision) {
            return;
        }
    }
    RevisionMark mark;
    if (!MarkRevision(backend, revision, mark)) {
        return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    revision_marks_.push_back(std::move(mark));
    while (revision_marks_.size() > kRetainedRevisionMarks) {
        revision_marks_.pop_front();
    }
}

std::string McpSession::RunningTasks(const std::vector<Ulid>& ids,
                                     Value& running) const {
    std::string named;
    for (const Ulid& id : ids) {
        const std::shared_ptr<Task> task = FindTask(id);
        const std::string tool = task != nullptr ? task->tool : "";
        Value item = Value::MakeObject();
        item.Set("task_id", Value::MakeString(id));
        item.Set("tool", Value::MakeString(tool));
        running.Push(std::move(item));
        named += (named.empty() ? "task " : ", task ") + id + " (" + tool + ")";
    }
    return named;
}

const std::vector<McpCallerPathArgument>& McpCallerPathArguments() {
    static const std::vector<McpCallerPathArgument> kArguments = {
        {"export_timeline", "output_path", nullptr},
        {"export_timelines", "output_path", "outputs"},
        {"render_motion_video", "output", nullptr},
        {"verify_export", "path", nullptr},
        // OR4 -- docs/roadmap/DONNEES-2026-10.md. Like verify_export, the
        // file an export wrote, named as export_timeline was given it.
        {"check_timeline", "path", nullptr},
        // DEC19 -- docs/roadmap/PRODUIT-2026-10b.md. The same exported file
        // again, whose cartons it measures: three tools that read what an
        // export wrote, one reading of the path.
        {"check_caption_legibility", "path", nullptr},
        // PRD39 -- ROADMAP.md. The other file a tool writes where a relative
        // path is accepted. The others that write one require it absolute
        // and keep requiring it (export_resolve_xml, export_motion_after,
        // the after_* tools, create_project); the other tools that read a
        // file (import_srt, apply_cdl, set_sequence_lut...) are not added.
        {"export_cdl", "path", nullptr},
        // SN7 -- docs/roadmap/SON-2026-10.md. The stem render_sound writes.
        {"render_sound", "out", nullptr},
    };
    return kArguments;
}

void ResolveMcpCallerPaths(const std::string& tool, const std::string& folder,
                           Value& arguments) {
    if (folder.empty() || !arguments.IsObject()) {
        return;
    }
    const auto resolve = [&](Value& holder, const std::string& name) {
        const Value* value = holder.Find(name);
        if (value == nullptr || !value->IsString() ||
            value->AsString().empty()) {
            return;
        }
        const std::filesystem::path path(value->AsString());
        if (path.is_relative()) {
            holder.Set(name,
                       Value::MakeString((std::filesystem::path(folder) / path)
                                             .lexically_normal()
                                             .string()));
        }
    };
    for (const McpCallerPathArgument& entry : McpCallerPathArguments()) {
        if (tool != entry.tool) {
            continue;
        }
        if (entry.list == nullptr) {
            resolve(arguments, entry.argument);
            continue;
        }
        const Value* list = arguments.Find(entry.list);
        if (list == nullptr || !list->IsArray()) {
            continue;
        }
        Value items = Value::MakeArray();
        for (const Value& original : list->AsArray()) {
            Value item = original;
            if (item.IsObject()) {
                resolve(item, entry.argument);
            }
            items.Push(std::move(item));
        }
        arguments.Set(entry.list, std::move(items));
    }
}
