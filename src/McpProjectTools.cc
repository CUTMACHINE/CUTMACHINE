// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// the project and its timelines: the archive, procedures, creation, opening,
// ingest, describe, timeline management and the editor window.
// Moved out of McpTools.cc as they were; what they share with the other files
// of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

const char kMediaRateSchemaText[] =
    "{\"type\":\"object\",\"description\":\"Exact positive integer frame "
    "rate.\","
    "\"properties\":{\"num\":{\"type\":\"integer\",\"minimum\":1},"
    "\"den\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"num\","
    "\"den\"],\"additionalProperties\":false}";

bool ReadMediaRate(const Value& args, const std::string& key,
                   const std::string& path, MediaRate& rate,
                   std::string& message) {
    const Value* value = args.Find(key);
    if (!value || !value->IsObject()) {
        message = "'" + path + "." + key + "' must be an object";
        return false;
    }
    if (!CheckKnownKeys(*value, {"num", "den"}, path + "." + key, message))
        return false;
    int64_t num = 0;
    int64_t den = 0;
    if (!ReadInt64(*value, "num", path + "." + key, true, 0, num, message) ||
        !ReadInt64(*value, "den", path + "." + key, true, 0, den, message))
        return false;
    if (num <= 0 || den <= 0 || num > std::numeric_limits<int32_t>::max() ||
        den > std::numeric_limits<int32_t>::max()) {
        message =
            "'" + path + "." + key + "' must contain positive int32 values";
        return false;
    }
    rate = {static_cast<int32_t>(num), static_cast<int32_t>(den)};
    return true;
}

// IA7 -- ROADMAP.md. The three calls that let a run start from a folder of
// rushes: the project and its library exist in the engine and the CLI
// (`--create-project`, `--ingest`), and a server started without a project
// could not reach either.
// PRD2 -- docs/roadmap/PRODUIT-2026-10.md. The answer that starts a session
// on a project also says what this machine's FFmpeg cannot do, when there
// is something to say: no binary, or one that lacks a filter or an encoder
// the engine uses. Carried by these two answers rather than by a tool of
// its own (A2), and absent when nothing is missing, so the usual answer
// keeps its bytes. It is the diagnosis of the process that answers -- the
// app's when the app holds the project, which is the one whose PATH is not
// the shell's.
void AppendFfmpegProblem(std::string& resultJson) {
    const ffmpeg::Diagnosis diagnosis = ffmpeg::Diagnose();
    if (diagnosis.Complete()) {
        return;
    }
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject()) {
        return;
    }
    result.Set(ffmpeg::kDiagnosisKey, ffmpeg::DiagnosisProblemJson(diagnosis));
    resultJson = result.Dump();
}

// PRD17 -- ROADMAP.md. `require_timeline` switches strict timeline mode for
// this server's session once the project is open, a refused open changing
// nothing; left out, the mode stays what it was. Every answer says which
// mode the session is in, so a series' first agent knows what it set.
bool ReadRequireTimeline(const Value& args, const std::string& tool,
                         bool& given, bool& required, std::string& message) {
    given = args.Find("require_timeline") != nullptr;
    return ReadBool(args, "require_timeline", tool, false, required, message);
}

void SetRequireTimeline(McpBackend& backend, bool given, bool required,
                        std::string& resultJson) {
    if (given) {
        backend.SetRequireExplicitTimeline(required);
    }
    Value result;
    std::string parseError;
    if (!Value::Parse(resultJson, result, parseError) || !result.IsObject()) {
        return;
    }
    result.Set("require_timeline",
               Value::MakeBool(backend.RequiresExplicitTimeline()));
    resultJson = result.Dump();
}

bool DispatchCreateProject(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "create_project";
    if (!CheckKnownKeys(args, {"path", "name", "format", "require_timeline"},
                        tool, message))
        return Fail(errorName, message, message);
    std::string path;
    std::string name;
    std::string format;
    bool requireGiven = false;
    bool required = false;
    if (!ReadString(args, "path", tool, true, "", path, message) ||
        !ReadString(args, "name", tool, true, "", name, message) ||
        !ReadString(args, "format", tool, false, "", format, message) ||
        !ReadRequireTimeline(args, tool, requireGiven, required, message))
        return Fail(errorName, message, message);
    if (!backend.CreateProject(path, name, format, resultJson, errorName,
                               message)) {
        return false;
    }
    SetRequireTimeline(backend, requireGiven, required, resultJson);
    AppendFfmpegProblem(resultJson);
    return true;
}

bool DispatchOpenProject(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    std::string path;
    bool requireGiven = false;
    bool required = false;
    if (!CheckKnownKeys(args, {"path", "require_timeline"}, "open_project",
                        message) ||
        !ReadString(args, "path", "open_project", true, "", path, message) ||
        !ReadRequireTimeline(args, "open_project", requireGiven, required,
                             message))
        return Fail(errorName, message, message);
    if (!backend.OpenProject(path, resultJson, errorName, message)) {
        return false;
    }
    SetRequireTimeline(backend, requireGiven, required, resultJson);
    AppendFfmpegProblem(resultJson);
    return true;
}

// PRD18 -- ROADMAP.md (ACP4). The engine's procedures (Procedures.h): read
// from the binary, never from a project, so the call needs none and touches
// no backend -- answered from the chat panel and before any project is open
// alike.
bool DispatchGetProcedure(McpBackend&, const IdResolver&, const Value& args,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) {
    const std::string tool = "get_procedure";
    std::string name;
    std::string file;
    if (!CheckKnownKeys(args, {"name", "file"}, tool, message) ||
        !ReadString(args, "name", tool, false, "", name, message) ||
        !ReadString(args, "file", tool, false, "", file, message))
        return Fail(errorName, message, message);
    if (name.empty()) {
        if (!file.empty()) {
            return Fail(errorName, message,
                        "'get_procedure.file' names a file of a procedure: "
                        "give 'name' too");
        }
        resultJson = ProcedureListingJson().Dump();
        return true;
    }
    Value answer;
    if (!ProcedureFileJson(name, file.empty() ? kProcedureMainFile : file,
                           answer, errorName, message)) {
        return false;
    }
    resultJson = answer.Dump();
    return true;
}

// DB1 -- whole-project capture, deliberately outside timeline targeting.
// Database paths are absolute, also when no project is open for a query.
bool ReadArchiveDatabase(const Value& args, const std::string& tool,
                         std::string& database, std::string& errorName,
                         std::string& message) {
    if (!ReadString(args, "database", tool, false,
                    project_archive::DefaultDatabasePath(), database,
                    message)) {
        return Fail(errorName, message, message);
    }
    if (!std::filesystem::path(database).is_absolute()) {
        return Fail(errorName, message,
                    "'" + tool +
                        ".database' must be an absolute local path; "
                        "supply it explicitly when automatic archiving is off");
    }
    return true;
}

bool DispatchProjectArchive(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message,
                            bool query) {
    const std::string tool =
        query ? "query_project_archive" : "archive_project";
    const std::vector<std::string> allowed =
        query ? std::vector<std::string>{"database", "sql", "max_rows"}
              : std::vector<std::string>{"database", "snapshot_id", "run_id",
                                         "step"};
    std::string database;
    if (!CheckKnownKeys(args, allowed, tool, message) ||
        !ReadArchiveDatabase(args, tool, database, errorName, message)) {
        return Fail(errorName, message, message);
    }
    Value result;
    if (query) {
        std::string sql;
        int64_t rows = 100;
        if (!ReadString(args, "sql", tool, true, "", sql, message)) {
            return Fail(errorName, message, message);
        }
        if (const Value* limit = args.Find("max_rows")) {
            if (!limit->AsInt64(rows) || rows < 1 || rows > 1000) {
                return Fail(errorName, message,
                            "'query_project_archive.max_rows' must be an "
                            "integer between 1 and 1000");
            }
        }
        if (!project_archive::Query(database, sql, static_cast<size_t>(rows),
                                    result, errorName, message)) {
            return false;
        }
    } else {
        project_archive::CaptureContext context;
        if (!ReadString(args, "snapshot_id", tool, false, "",
                        context.snapshot_id, message) ||
            !ReadString(args, "run_id", tool, false, "", context.run_id,
                        message) ||
            !ReadString(args, "step", tool, false, "", context.step, message)) {
            return Fail(errorName, message, message);
        }
        Project project;
        if (!backend.SnapshotProject(project, message)) {
            errorName = "IoError";
            return false;
        }
        if (!project_archive::Capture(database, project, context, result,
                                      errorName, message)) {
            return false;
        }
    }
    resultJson = result.Dump();
    return true;
}

// DB2 -- pending delivery is file maintenance, never a montage operation.
bool DispatchArchiveMaintenance(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message,
                                bool retry) {
    const std::string tool =
        retry ? "sync_project_archive" : "project_archive_status";
    if (!CheckKnownKeys(args, {}, tool, message)) {
        return Fail(errorName, message, message);
    }
    return backend.ProjectArchiveStatus(retry, resultJson, errorName, message);
}

bool DispatchImportArchive(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "import_project_archive";
    std::string database;
    if (!CheckKnownKeys(args, {"database", "projects"}, tool, message) ||
        !ReadArchiveDatabase(args, tool, database, errorName, message)) {
        return Fail(errorName, message, message);
    }
    const Value* projects = args.Find("projects");
    if (projects == nullptr || !projects->IsArray()) {
        return Fail(errorName, message, "projects must be an array of paths");
    }
    std::vector<std::string> paths;
    for (const auto& path : projects->AsArray()) {
        if (!path.IsString()) {
            return Fail(errorName, message, "project paths must be strings");
        }
        paths.push_back(path.AsString());
    }
    return backend.ImportProjectArchive(database, paths, resultJson, errorName,
                                        message);
}

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. The day's copy of the local
// archive, written now into the configured folder. No project: the copy is
// of the archive, and the folder is a machine setting, never an argument.
bool DispatchBackupArchive(McpBackend&, const IdResolver&, const Value& args,
                           std::string& resultJson, std::string& errorName,
                           std::string& message) {
    if (!CheckKnownKeys(args, {}, "backup_project_archive", message)) {
        return Fail(errorName, message, message);
    }
    project_archive::BackupReport report;
    if (!project_archive::Backup(project_archive::DefaultDatabasePath(),
                                 project_archive::BackupDirectory(),
                                 project_archive::BackupMode::kNow,
                                 std::time(nullptr), report)) {
        errorName = report.error_name;
        message = report.message;
        return false;
    }
    resultJson = project_archive::BackupReportJson(report).Dump();
    return true;
}

bool DispatchIngestMedia(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    const std::string tool = "ingest_media";
    std::string path;
    bool recursive = false;
    // DEC32 (PRD7) -- absent, known rushes stay in their bins; present,
    // "" included (the root), everything ingested is filed there.
    std::optional<Ulid> targetBin;
    Ulid bin;
    if (!CheckKnownKeys(args, {"path", "recursive", "bin_id"}, tool, message) ||
        !ReadString(args, "path", tool, true, "", path, message) ||
        !ReadBool(args, "recursive", tool, false, recursive, message) ||
        !ReadOptionalIdOrEmpty(args, "bin_id", tool, resolver, bin, message))
        return Fail(errorName, message, message);
    if (args.Find("bin_id") != nullptr) targetBin = bin;
    return backend.IngestMedia(path, recursive, targetBin, resultJson,
                               errorName, message);
}

// AGENT-2026-09 A25 -- ROADMAP.md. A describe cut to what is asked: a
// finished montage's snapshot weighs 74 KB, three quarters of it the
// tracks, and an agent that wanted the captions or the library read the
// project's files by hand rather than pay it again.
bool DispatchDescribe(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    const std::string tool = "describe";
    static const std::vector<std::string> kAllowed = {"only", "kinds"};
    if (!CheckKnownKeys(args, kAllowed, tool, message))
        return Fail(errorName, message, message);
    std::vector<std::string> only;
    std::vector<std::string> kinds;
    if (!ReadStringArray(args, "only", tool, only, message) ||
        !ReadStringArray(args, "kinds", tool, kinds, message))
        return Fail(errorName, message, message);
    for (const std::string& kind : kinds)
        if (kind != "video" && kind != "audio" && kind != "caption")
            return Fail(errorName, message,
                        "'" + tool +
                            ".kinds' takes 'video', 'audio' and "
                            "'caption'");
    // PRD35 -- ROADMAP.md. `history` is no section of the snapshot: the
    // timeline's journal (SummarizeHistory, EditLog.h), read only when it
    // is named -- describe is heavy enough without it.
    Value history;
    if (std::find(only.begin(), only.end(), "history") != only.end()) {
        only.erase(std::remove(only.begin(), only.end(), "history"),
                   only.end());
        TimelineHistory read;
        if (!backend.ReadTimelineHistory(read, errorName, message)) {
            return false;
        }
        std::string parseError;
        Value::Parse(TimelineHistoryJson(read), history, parseError);
        if (only.empty()) {
            Value answer = Value::MakeObject();
            answer.Set("history", std::move(history));
            resultJson = answer.Dump();
            return true;
        }
    }
    std::string detail;
    if (!backend.Describe(resultJson, detail)) {
        errorName = "IoError";
        message = detail;
        return false;
    }
    if (only.empty() && kinds.empty()) return true;
    Value snapshot;
    std::string parseError;
    if (!Value::Parse(resultJson, snapshot, parseError) ||
        !snapshot.IsObject()) {
        errorName = "IoError";
        message = "malformed snapshot: " + parseError;
        return false;
    }
    for (const std::string& section : only) {
        if (snapshot.Find(section) != nullptr) continue;
        std::string known;
        for (const auto& [key, value] : snapshot.AsObject())
            known += (known.empty() ? "" : ", ") + key;
        known += ", history";  // PRD35
        return Fail(errorName, message,
                    "'" + tool + ".only' names no section '" + section +
                        "'; the sections are: " + known);
    }
    Value cut = Value::MakeObject();
    for (const auto& [key, value] : snapshot.AsObject()) {
        if (!only.empty() &&
            std::find(only.begin(), only.end(), key) == only.end())
            continue;
        if (key == "timeline" && !kinds.empty() && value.IsObject()) {
            Value timeline = Value::MakeObject();
            for (const auto& [field, inner] : value.AsObject()) {
                if (field != "tracks" || !inner.IsArray()) {
                    timeline.Set(field, inner);
                    continue;
                }
                Value tracks = Value::MakeArray();
                for (const Value& track : inner.AsArray()) {
                    const Value* kind = track.Find("kind");
                    if (kind != nullptr && kind->IsString() &&
                        std::find(kinds.begin(), kinds.end(),
                                  kind->AsString()) != kinds.end())
                        tracks.Push(track);
                }
                timeline.Set("tracks", std::move(tracks));
            }
            cut.Set(key, std::move(timeline));
            continue;
        }
        cut.Set(key, value);
    }
    if (!history.IsNull()) {
        cut.Set("history", std::move(history));
    }
    resultJson = cut.Dump();
    return true;
}

// AX5 -- docs/roadmap/AX-2026-09.md. "Ce plan-là", "ici": what the editor
// points at in the app, resolved against the timeline on screen, so an agent
// asked about "this clip" needs no arithmetic to find it (principle 7). The
// frames are those of the sequence and, under the playhead, of each source.
bool DispatchGetEditorView(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "get_editor_view", message))
        return Fail(errorName, message, message);
    EditorView view;
    if (!backend.ReadEditorView(view, message)) {
        errorName = "UnsupportedOperation";
        return false;
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const MediaRate rate = document.sequence.frame_rate;
    const auto time = [](const RationalTime& value) {
        Value item = Value::MakeObject();
        item.Set("value", Value::MakeInt(value.value));
        item.Set("rate", Value::MakeInt(value.rate));
        return item;
    };
    Value root = Value::MakeObject();
    root.Set("timeline_id", Value::MakeString(view.timeline_id));
    root.Set("timeline_name", Value::MakeString(document.sequence.name));
    root.Set("playhead", time(view.playhead));
    root.Set("playhead_frame",
             Value::MakeInt(view.playhead.to_frames(rate.num, rate.den)));
    Value under = Value::MakeArray();
    Value selected = Value::MakeArray();
    for (const DocumentTrack& track : document.sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            const RationalTime end = clip.timeline_in.add(clip.duration);
            const bool playing = clip.timeline_in.compare(view.playhead) <= 0 &&
                                 view.playhead.compare(end) < 0;
            const bool chosen =
                std::find(view.selected_clip_ids.begin(),
                          view.selected_clip_ids.end(),
                          clip.id) != view.selected_clip_ids.end();
            if (!playing && !chosen) continue;
            Value item = Value::MakeObject();
            item.Set("clip_id", Value::MakeString(clip.id));
            item.Set("track_id", Value::MakeString(track.id));
            item.Set("track_kind", Value::MakeString(track.kind));
            item.Set("track_index", Value::MakeInt(track.index));
            item.Set("source_id", Value::MakeString(clip.source_id));
            item.Set(
                "timeline_in_frame",
                Value::MakeInt(clip.timeline_in.to_frames(rate.num, rate.den)));
            item.Set("timeline_end_frame",
                     Value::MakeInt(end.to_frames(rate.num, rate.den)));
            if (!clip.caption_text.empty())
                item.Set("caption_text", Value::MakeString(clip.caption_text));
            if (playing) {
                Value current = item;
                if (const DocumentSource* source =
                        document.FindSource(clip.source_id)) {
                    const RationalTime inSource =
                        clip.source_in.add(view.playhead.sub(clip.timeline_in));
                    current.Set("source_frame",
                                Value::MakeInt(inSource.to_frames(
                                    source->rate.num, source->rate.den)));
                }
                under.Push(std::move(current));
            }
            if (chosen) selected.Push(std::move(item));
        }
    }
    root.Set("under_playhead", std::move(under));
    root.Set("selected", std::move(selected));
    // D5, D6 -- docs/roadmap/DIALOGUE-2026-10.md. The markers the playhead
    // stands on or inside of: "ce passage" is the span around the playhead,
    // named with where it starts and how long it is, in sequence frames.
    const int64_t playheadFrame = view.playhead.to_frames(rate.num, rate.den);
    Value marked = Value::MakeArray();
    for (const DocumentMarker& marker : document.sequence.markers) {
        const int64_t start = marker.time.to_frames(rate.num, rate.den);
        const int64_t end =
            marker.time.add(marker.duration).to_frames(rate.num, rate.den);
        if (playheadFrame < start || playheadFrame > end) continue;
        Value item = Value::MakeObject();
        item.Set("marker_id", Value::MakeString(marker.id));
        item.Set("name", Value::MakeString(marker.name));
        item.Set("time_frame", Value::MakeInt(start));
        if (marker.duration.value > 0)
            item.Set("duration_frames", Value::MakeInt(end - start));
        item.Set("category", Value::MakeString(marker.category));
        marked.Push(std::move(item));
    }
    root.Set("markers_under_playhead", std::move(marked));
    // D5 -- docs/roadmap/DIALOGUE-2026-10.md. "Entre l'entrée et la sortie":
    // the range the editor marked, as delete_timeline_range reads it (Out
    // excluded), each mark exact and as a sequence frame. `valid` says the
    // two are set and in order, which is when the window acts on them.
    if (view.mark_in || view.mark_out) {
        Value range = Value::MakeObject();
        if (view.mark_in) {
            range.Set("in", time(*view.mark_in));
            range.Set("in_frame", Value::MakeInt(view.mark_in->to_frames(
                                      rate.num, rate.den)));
        }
        if (view.mark_out) {
            range.Set("out", time(*view.mark_out));
            range.Set("out_frame", Value::MakeInt(view.mark_out->to_frames(
                                       rate.num, rate.den)));
        }
        range.Set("valid", Value::MakeBool(view.mark_in && view.mark_out &&
                                           *view.mark_out > *view.mark_in));
        root.Set("marked_range", std::move(range));
    }
    resultJson = root.Dump();
    return true;
}

// CF1 -- the host reads its screens; the shared tool serializes the answer.
bool WriteVideoOutput(McpBackend& backend,
                      const std::optional<std::string>& screen,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    VideoOutputState state;
    if (!backend.AccessVideoOutput(screen, state, errorName, message)) {
        return false;
    }
    Value answer = Value::MakeObject();
    answer.Set("selected_screen_id",
               Value::MakeString(state.selected_screen_id));
    answer.Set("active_screen_id", Value::MakeString(state.active_screen_id));
    answer.Set("enabled", Value::MakeBool(!state.active_screen_id.empty()));
    answer.Set("presentation", Value::MakeString("rec709_sdr"));
    Value screens = Value::MakeArray();
    for (const VideoOutputScreen& display : state.screens) {
        Value item = Value::MakeObject();
        item.Set("id", Value::MakeString(display.id));
        item.Set("name", Value::MakeString(display.name));
        item.Set("width", Value::MakeInt(display.width));
        item.Set("height", Value::MakeInt(display.height));
        item.Set("editor_screen", Value::MakeBool(display.editor_screen));
        screens.Push(std::move(item));
    }
    answer.Set("screens", std::move(screens));
    resultJson = answer.Dump();
    return true;
}

// CF1 -- a local viewing preference; no document mutation or edit operation.
bool DispatchGetVideoOutput(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "get_video_output", message)) {
        return Fail(errorName, message, message);
    }
    return WriteVideoOutput(backend, std::nullopt, resultJson, errorName,
                            message);
}

bool DispatchSetVideoOutput(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    std::string screen;
    if (!CheckKnownKeys(args, {"screen_id"}, "set_video_output", message) ||
        !ReadString(args, "screen_id", "set_video_output", true, "", screen,
                    message)) {
        return Fail(errorName, message, message);
    }
    return WriteVideoOutput(backend, screen, resultJson, errorName, message);
}

// VQA5 -- docs/roadmap/QA-AGENTIQUE-2026-09.md. What get_editor_view names,
// as the editor sees it. An agent session has no screen-recording
// permission, so the app draws its own window off screen, Metal surfaces
// included (WindowCapture.h): covered, behind another app or on another
// Space, it is drawn as it stands.
bool DispatchCaptureEditorWindow(McpBackend& backend, const IdResolver&,
                                 const Value& args, std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    static const char kTool[] = "capture_editor_window";
    if (!CheckKnownKeys(args, {"region", "scale"}, kTool, message))
        return Fail(errorName, message, message);
    EditorCaptureRequest request;
    if (!ReadString(args, "region", kTool, false, "window", request.region,
                    message) ||
        !ReadInt32(args, "scale", kTool, false, 1, request.scale, message))
        return Fail(errorName, message, message);
    if (request.region != "window" && request.region != "timeline" &&
        request.region != "record" && request.region != "source")
        return Fail(errorName, message,
                    "'capture_editor_window.region' must be window, "
                    "timeline, record or source");
    if (request.scale != 1 && request.scale != 2)
        return Fail(errorName, message,
                    "'capture_editor_window.scale' must be 1 or 2");
    EditorCapture capture;
    if (!backend.CaptureEditorWindow(request, capture, errorName, message)) {
        if (errorName.empty()) errorName = "IoError";
        return false;
    }
    Value regions = Value::MakeArray();
    for (const EditorCaptureRegion& region : capture.regions) {
        Value item = Value::MakeObject();
        item.Set("name", Value::MakeString(region.name));
        item.Set("x", Value::MakeInt(region.x));
        item.Set("y", Value::MakeInt(region.y));
        item.Set("width", Value::MakeInt(region.width));
        item.Set("height", Value::MakeInt(region.height));
        regions.Push(std::move(item));
    }
    Value text = Value::MakeObject();
    text.Set("region", Value::MakeString(request.region));
    text.Set("scale", Value::MakeInt(request.scale));
    text.Set("width", Value::MakeInt(capture.width));
    text.Set("height", Value::MakeInt(capture.height));
    text.Set("regions", std::move(regions));
    Value envelope = Value::MakeObject();
    envelope.Set(kMcpImageDataKey,
                 Value::MakeString(EncodeBase64(capture.jpeg)));
    envelope.Set(kMcpImageMimeKey, Value::MakeString("image/jpeg"));
    envelope.Set(kMcpImageTextKey, Value::MakeString(text.Dump()));
    resultJson = envelope.Dump();
    return true;
}

bool DispatchSetActiveTimeline(McpBackend& backend, const IdResolver& resolver,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"timeline_id"};
    if (!CheckKnownKeys(args, kAllowed, "set_active_timeline", message))
        return Fail(errorName, message, message);
    SetActiveProjectTimelineOperation operation;
    std::string requested;
    if (!ReadString(args, "timeline_id", "set_active_timeline", true, "",
                    requested, message))
        return Fail(errorName, message, message);
    // Deliberately not run through IdResolver: it resolves ids present on the
    // *active* timeline, and the whole point of this tool is to name one that
    // is not. The engine validates the id against the project instead.
    operation.timeline_id = requested;
    (void)resolver;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool ReadProjectTimelineId(McpBackend& backend, const std::string& requested,
                           Ulid& timelineId, std::string& errorName,
                           std::string& message) {
    if (requested.empty()) {
        errorName = "ValidationFailed";
        message = "'timeline_id' must not be empty";
        return false;
    }
    std::string json;
    if (!backend.Describe(json, message)) {
        errorName = "IoError";
        return false;
    }
    Value description;
    std::string parseError;
    if (!Value::Parse(json, description, parseError)) {
        errorName = "IoError";
        message = "describe returned invalid JSON: " + parseError;
        return false;
    }
    const Value* timelines = description.Find("timelines");
    if (!timelines || !timelines->IsArray()) {
        errorName = "UnsupportedOperation";
        message = "this backend does not expose project timelines";
        return false;
    }
    std::vector<Ulid> matches;
    for (const Value& item : timelines->AsArray()) {
        const Value* id = item.Find("id");
        if (id && id->IsString() &&
            id->AsString().compare(0, requested.size(), requested) == 0)
            matches.push_back(id->AsString());
    }
    if (matches.empty()) {
        errorName = "UnknownSequence";
        message = "unknown timeline_id '" + requested + "'";
        return false;
    }
    if (matches.size() != 1) {
        errorName = "ValidationFailed";
        message = "ambiguous timeline_id prefix '" + requested + "'";
        return false;
    }
    timelineId = matches.front();
    return true;
}

bool DispatchListTimelines(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "list_timelines", message))
        return Fail(errorName, message, message);
    std::string description;
    if (!backend.Describe(description, message)) {
        errorName = "IoError";
        return false;
    }
    Value root;
    std::string parseError;
    if (!Value::Parse(description, root, parseError))
        return Fail(errorName, message,
                    "describe returned invalid JSON: " + parseError);
    const Value* timelines = root.Find("timelines");
    if (!timelines || !timelines->IsArray())
        return Fail(errorName, message,
                    "this backend does not expose project timelines");
    resultJson = "{\"timelines\":" + timelines->Dump() + "}";
    return true;
}

bool DispatchAddTimeline(McpBackend& backend, const IdResolver&,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args,
                        {"name", "width", "height", "frame_rate", "activate"},
                        "add_timeline", message))
        return Fail(errorName, message, message);
    AddProjectTimelineOperation operation;
    bool activate = false;
    if (!ReadString(args, "name", "add_timeline", true, "", operation.name,
                    message) ||
        !ReadInt32(args, "width", "add_timeline", true, operation.width,
                   operation.width, message) ||
        !ReadInt32(args, "height", "add_timeline", true, operation.height,
                   operation.height, message) ||
        !ReadMediaRate(args, "frame_rate", "add_timeline", operation.frame_rate,
                       message) ||
        !ReadBool(args, "activate", "add_timeline", false, activate, message))
        return Fail(errorName, message, message);
    // PRD17 -- ROADMAP.md. Named before it is applied, so that activate can
    // name it next: the operation keeps an identity given to it.
    const Ulid timelineId = GenerateUlid();
    operation.timeline_id = timelineId;
    if (!backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                  message))
        return false;
    if (!activate) return true;
    // The creation stands either way: an activation refused is said in the
    // answer, beside the timeline it would have made active. Two entries of
    // the project's journal, the activation undone first.
    Value created;
    std::string parseError;
    if (!Value::Parse(resultJson, created, parseError) || !created.IsObject())
        return true;
    std::string activated;
    std::string activationError;
    std::string activationMessage;
    if (backend.ApplyProjectEdit(SetActiveProjectTimelineOperation{timelineId},
                                 activated, activationError,
                                 activationMessage)) {
        Value answer;
        if (Value::Parse(activated, answer, parseError) && answer.IsObject()) {
            if (const Value* hash = answer.Find("project_hash"))
                created.Set("project_hash", *hash);
        }
        created.Set("active_timeline_id", Value::MakeString(timelineId));
    } else {
        created.Set(
            "activation_error",
            Value::MakeString(activationError + ": " + activationMessage));
    }
    resultJson = created.Dump();
    return true;
}

bool DispatchRemoveTimeline(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"timeline_id"}, "remove_timeline", message))
        return Fail(errorName, message, message);
    std::string requested;
    if (!ReadString(args, "timeline_id", "remove_timeline", true, "", requested,
                    message))
        return Fail(errorName, message, message);
    RemoveProjectTimelineOperation operation;
    if (!ReadProjectTimelineId(backend, requested, operation.timeline_id,
                               errorName, message))
        return false;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool DispatchCloneTimeline(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(
            args, {"source_timeline_id", "name", "make_active", "activate"},
            "clone_timeline", message)) {
        return Fail(errorName, message, message);
    }
    std::string requested;
    CloneProjectTimelineOperation operation;
    bool activate = false;
    if (!ReadString(args, "source_timeline_id", "clone_timeline", true, "",
                    requested, message) ||
        !ReadString(args, "name", "clone_timeline", true, "", operation.name,
                    message) ||
        !ReadBool(args, "make_active", "clone_timeline", false,
                  operation.make_active, message) ||
        !ReadBool(args, "activate", "clone_timeline", false, activate,
                  message)) {
        return Fail(errorName, message, message);
    }
    // PRD17 -- `activate`, the name every creating tool takes; `make_active`
    // stays accepted, and the two must agree when both are given.
    if (args.Find("activate") != nullptr) {
        if (args.Find("make_active") != nullptr &&
            activate != operation.make_active) {
            return Fail(errorName, message,
                        "'clone_timeline.activate' and "
                        "'clone_timeline.make_active' disagree: give one");
        }
        operation.make_active = activate;
    }
    if (!ReadProjectTimelineId(backend, requested, operation.source_timeline_id,
                               errorName, message)) {
        return false;
    }
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md, DEC41. The person's choice
// between timelines, written into the project's journal with the timeline
// kept, the ones it was chosen among and, on the entry, why. The motif and
// its author are required before this runs (McpToolRegistry::Call): a
// choice nobody can say the reason or the maker of is no preference.
bool DispatchChooseTimelineVariant(McpBackend& backend, const IdResolver&,
                                   const Value& args, std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    const std::string tool = "choose_timeline_variant";
    if (!CheckKnownKeys(args,
                        {"timeline_id", "alternative_timeline_ids", "activate"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    std::string kept;
    std::vector<std::string> alternatives;
    ChooseProjectTimelineOperation operation;
    if (!ReadString(args, "timeline_id", tool, true, "", kept, message) ||
        !ReadStringArray(args, "alternative_timeline_ids", tool, alternatives,
                         message) ||
        !ReadBool(args, "activate", tool, false, operation.make_active,
                  message)) {
        return Fail(errorName, message, message);
    }
    if (alternatives.empty()) {
        return Fail(errorName, message,
                    "'" + tool +
                        ".alternative_timeline_ids' is required: a choice "
                        "names the timelines it was made among");
    }
    if (!ReadProjectTimelineId(backend, kept, operation.timeline_id, errorName,
                               message)) {
        return false;
    }
    for (const std::string& alternative : alternatives) {
        Ulid id;
        if (!ReadProjectTimelineId(backend, alternative, id, errorName,
                                   message)) {
            return false;
        }
        operation.alternative_timeline_ids.push_back(id);
    }
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

bool DispatchCreateTimelineVariants(McpBackend& backend, const IdResolver&,
                                    const Value& args, std::string& resultJson,
                                    std::string& errorName,
                                    std::string& message) {
    const std::string tool = "create_timeline_variants";
    if (!CheckKnownKeys(
            args,
            {"templates", "variants", "find", "expected_count", "preview"},
            tool, message)) {
        return Fail(errorName, message, message);
    }
    std::string find;
    int64_t count = 1;
    bool preview = true;
    if (!ReadString(args, "find", tool, true, "", find, message) ||
        !ReadInt64(args, "expected_count", tool, false, 1, count, message) ||
        !ReadBool(args, "preview", tool, true, preview, message)) {
        return Fail(errorName, message, message);
    }
    const auto* templates = args.Find("templates");
    const auto* values = args.Find("variants");
    if ((templates == nullptr) || !templates->IsArray() ||
        templates->AsArray().empty() || (values == nullptr) ||
        !values->IsArray() || values->AsArray().empty() || find.empty() ||
        count <= 0) {
        return Fail(errorName, message,
                    "nonempty templates, variants and find, and a positive "
                    "expected_count are required");
    }
    Project project;
    if (!backend.SnapshotProject(project, message)) {
        errorName = "IoError";
        return false;
    }
    CreateProjectTimelineVariantsOperation operation;
    for (const auto& value : values->AsArray()) {
        std::string name;
        std::string text;
        if (!value.IsObject() ||
            !CheckKnownKeys(value, {"name", "text"}, tool + ".variants",
                            message) ||
            !ReadString(value, "name", tool, true, "", name, message) ||
            !ReadString(value, "text", tool, true, "", text, message) ||
            name.empty()) {
            return Fail(errorName, message, "invalid variant: " + message);
        }
        for (const auto& source : templates->AsArray()) {
            ProjectTimelineVariant variant;
            std::string suffix;
            CaptionTextReplacement replacement;
            replacement.from = find;
            replacement.to = text;
            replacement.expected_count = count;
            if (!source.IsObject() ||
                !CheckKnownKeys(source, {"timeline_id", "suffix", "clip_id"},
                                tool + ".templates", message) ||
                !ReadString(source, "timeline_id", tool, true, "",
                            variant.clone.source_timeline_id, message) ||
                !ReadString(source, "suffix", tool, true, "", suffix,
                            message) ||
                !ReadString(source, "clip_id", tool, false, "",
                            replacement.clip_id, message)) {
                return Fail(errorName, message,
                            "invalid variant template: " + message);
            }
            variant.clone.name = name + suffix;
            variant.replacements.push_back(std::move(replacement));
            operation.variants.push_back(std::move(variant));
        }
    }
    const Project before = project;
    ProjectEditLog previewLog;
    EditError error = EditError::None;
    if (!previewLog.Apply(project, operation, error, message)) {
        errorName = EditErrorName(error);
        return false;
    }
    operation = std::get<CreateProjectTimelineVariantsOperation>(
        previewLog.AppliedEntries().back().op);
    // Reapply the guarded intention, not an unconditional replay delta, so a
    // preview used later still checks the template's current text and name.
    operation.exact_project_result.reset();
    Value outputs = Value::MakeArray();
    for (const auto& variant : operation.variants) {
        Value output = Value::MakeObject();
        output.Set("source_timeline_id",
                   Value::MakeString(variant.clone.source_timeline_id));
        output.Set("timeline_id", Value::MakeString(variant.clone.timeline_id));
        output.Set("name", Value::MakeString(variant.clone.name));
        Value changes = Value::MakeArray();
        const auto* original =
            before.FindTimeline(variant.clone.source_timeline_id);
        const Document edited = project.MakeDocument(variant.clone.timeline_id);
        for (const auto& track : original->tracks) {
            if (track.kind != "caption") {
                continue;
            }
            for (const auto& clip : track.clips) {
                const auto* updated =
                    edited.FindClip(variant.clone.id_remap.at(clip.id));
                if ((updated != nullptr) &&
                    updated->caption_text != clip.caption_text) {
                    Value change = Value::MakeObject();
                    change.Set("source_clip_id", Value::MakeString(clip.id));
                    change.Set("clip_id", Value::MakeString(updated->id));
                    change.Set("before", Value::MakeString(clip.caption_text));
                    change.Set("after",
                               Value::MakeString(updated->caption_text));
                    changes.Push(std::move(change));
                }
            }
        }
        output.Set("changes", std::move(changes));
        outputs.Push(std::move(output));
    }
    if (!preview) {
        std::string applied;
        if (!backend.ApplyProjectEdit(operation, applied, errorName, message)) {
            return false;
        }
    }
    Value response = Value::MakeObject();
    response.Set("ok", Value::MakeBool(true));
    response.Set("preview", Value::MakeBool(preview));
    response.Set("total", Value::MakeInt(operation.variants.size()));
    response.Set("timelines", std::move(outputs));
    Value canonical;
    std::string parseError;
    Value::Parse(SerializeProjectOperation(operation), canonical, parseError);
    response.Set("operation", std::move(canonical));
    resultJson = response.Dump();
    return true;
}

bool DispatchRenameTimeline(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"timeline_id", "name"}, "rename_timeline",
                        message))
        return Fail(errorName, message, message);
    std::string requested;
    RenameProjectItemOperation operation;
    if (!ReadString(args, "timeline_id", "rename_timeline", true, "", requested,
                    message) ||
        !ReadString(args, "name", "rename_timeline", true, "", operation.name,
                    message))
        return Fail(errorName, message, message);
    if (!ReadProjectTimelineId(backend, requested, operation.item_id, errorName,
                               message))
        return false;
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

// BINS_SPEC.md -- filing a timeline in a bin was a drag in the app's media
// pool and nothing else, so an agent tidying a project could file every rush
// and none of its timelines (4 November 2026). The operation
// already existed; this is its tool. Bins are shared by every timeline, so
// the active timeline's resolver names them.
bool DispatchSetTimelineBin(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"timeline_id", "bin_id"}, "set_timeline_bin",
                        message)) {
        return Fail(errorName, message, message);
    }
    std::string requested;
    SetProjectTimelineBinOperation operation;
    if (!ReadString(args, "timeline_id", "set_timeline_bin", true, "",
                    requested, message) ||
        !ReadOptionalIdOrEmpty(args, "bin_id", "set_timeline_bin", resolver,
                               operation.bin_id, message)) {
        return Fail(errorName, message, message);
    }
    if (!ReadProjectTimelineId(backend, requested, operation.timeline_id,
                               errorName, message)) {
        return false;
    }
    return backend.ApplyProjectEdit(std::move(operation), resultJson, errorName,
                                    message);
}

}  // namespace

void RegisterProjectTools(const McpToolAdder& add) {
    // PRD18 -- ROADMAP.md (ACP4). The know-how travels with the engine: the
    // procedures it names are the ones this binary embeds.
    std::string procedureNames;

    for (const Procedure& procedure : ListProcedures()) {
        procedureNames += (procedureNames.empty() ? "" : ", ") + procedure.name;
    }

    add("archive_project",
        "Archive the complete current project as canonical JSON in a local "
        "SQLite database, all timelines included, without editing it. "
        "Adds an immutable project_snapshots row. Supply snapshot_id (ULID) "
        "to retry durably: identical data returns the first capture; "
        "different data refuses SnapshotIdReused. run_id and step label "
        "the capture. Stored project saves also archive automatically.",
        SchemaBuilder()
            .Field("database",
                   StringSchema("Absolute local SQLite path; "
                                "defaults to the local archive."),
                   false)
            .Field("snapshot_id", StringSchema("ULID; omit to generate one."),
                   false)
            .Field("run_id",
                   StringSchema("Run identifier, at most 1024 bytes."), false)
            .Field("step", StringSchema("Editing stage, at most 1024 bytes."),
                   false)
            .Build("archive_project arguments"),
        [](McpBackend& backend, const IdResolver& ids, const Value& args,
           std::string& result, std::string& error, std::string& message) {
            return DispatchProjectArchive(backend, ids, args, result, error,
                                          message, false);
        });

    add("query_project_archive",
        "Query an existing local CUTMACHINE SQLite archive with one read-only "
        "SELECT (WITH allowed). Works without an open project. "
        "project_snapshots has snapshot_id, project_id, revision_hash, "
        "created_at (UTC), run_id, step and document_json (canonical TEXT). "
        "Use json_extract/json_each to inspect projects and timelines. Returns "
        "columns, row arrays and truncated. Default 100 rows, maximum 1000, 4 "
        "MiB output and bounded execution.",
        SchemaBuilder()
            .Field("database",
                   StringSchema("Absolute local SQLite path; "
                                "defaults to the local archive."),
                   false)
            .Field("sql", StringSchema("One read-only SELECT, at most 64 KiB."),
                   true)
            .Field("max_rows",
                   "{\"type\":\"integer\",\"minimum\":1,\"maximum\":1000}",
                   false)
            .Build("query_project_archive arguments"),
        [](McpBackend& backend, const IdResolver& ids, const Value& args,
           std::string& result, std::string& error, std::string& message) {
            return DispatchProjectArchive(backend, ids, args, result, error,
                                          message, true);
        });

    add("project_archive_status",
        "Inspect this project's pending automatic SQL captures without "
        "writing. Reports database, enabled, pending and complete. "
        "The JSON save remains valid when archive delivery is pending.",
        SchemaBuilder().Build("project_archive_status arguments"),
        [](McpBackend& backend, const IdResolver& ids, const Value& args,
           std::string& result, std::string& error, std::string& message) {
            return DispatchArchiveMaintenance(backend, ids, args, result, error,
                                              message, false);
        });

    add("sync_project_archive",
        "Retry this project's durable pending SQL captures in the configured "
        "local archive. Does not edit the project or replay operations. "
        "Reports archived, pending, complete and any delivery refusal. "
        "A capture already delivered before a crash is reused by its ULID.",
        SchemaBuilder().Build("sync_project_archive arguments"),
        [](McpBackend& backend, const IdResolver& ids, const Value& args,
           std::string& result, std::string& error, std::string& message) {
            return DispatchArchiveMaintenance(backend, ids, args, result, error,
                                              message, true);
        });

    add("import_project_archive",
        "Import 1 to 100 existing CUTMACHINE packages into a local SQLite "
        "archive without editing them. No open project required. Each path "
        "is absolute and names a package or its project.cutmachine.json. "
        "Locks and validates each project; returns successes and refusals "
        "individually. Reimporting identical canonical JSON reuses its "
        "import capture. A changed project adds a row.",
        SchemaBuilder()
            .Field("database",
                   StringSchema("Absolute local SQLite path; "
                                "defaults to the local archive."),
                   false)
            .Field("projects",
                   "{\"type\":\"array\",\"minItems\":1,"
                   "\"maxItems\":100,\"items\":{\"type\":\"string\"}}",
                   true)
            .Build("import_project_archive arguments"),
        DispatchImportArchive);

    add("backup_project_archive",
        "Copy the local project archive now into the folder set by "
        "CUTMACHINE_ARCHIVE_BACKUP_DIR, as CUTMACHINE_projects_<YYYYMMDD>"
        ".sqlite (local date), replacing today's copy, then keep the 7 "
        "newest dated copies. No open project required. The copy is one "
        "consistent state even while agents write. Saves already make one "
        "copy a day; refuses NotConfigured without a folder.",
        SchemaBuilder().Build("backup_project_archive arguments"),
        DispatchBackupArchive);

    add("get_procedure",
        "Read the editing procedures this engine carries (" + procedureNames +
            "): how to edit an interview, make motion design or cut a reel or "
            "an ad with these tools -- the skills Claude Code loads inside the "
            "CUTMACHINE repository, served here to any agent. Without "
            "arguments: each procedure's name, description and files (path, "
            "bytes). With `name`: its SKILL.md, its files, and `links`, each "
            "relative link of the text with the `name` and `file` to ask for, "
            "or `served: false` for a file of the repository the binary does "
            "not carry. With `name` and `file`: that file. The text is the "
            "skill's own, in French; a script it says to run (outils/*.py) is "
            "one of its files: write it to disk to run it. Reads only the "
            "binary: answered with no project open.",
        SchemaBuilder()
            .Field("name",
                   StringSchema("A procedure, as the list names it, e.g. "
                                "monter-une-itw. Omitted: the list."),
                   false)
            .Field("file",
                   StringSchema("One of its files, as `files` or `links` give "
                                "it, e.g. mots-et-bornes.md or "
                                "references/motifs.md. Default: SKILL.md."),
                   false)
            .Build("get_procedure arguments"),
        DispatchGetProcedure);

    // PRD17 -- strict timeline mode, for a series of agents sharing this
    // server, is one argument of the two calls that open a project.
    const std::string requireTimelineDescription =
        "true: every timeline call must name timeline_id (several agents on "
        "one project); false: the active timeline again; omitted: unchanged.";

    add("create_project",
        "Create a new project package at an absolute path that does not exist "
        "yet, then open it in this server in place of the project it held, "
        "which is released once the new one is open. Its first timeline has "
        "the requested format and one video and one audio track, named in the "
        "result. The answer carries `ffmpeg` as open_project's does.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the package directory to "
                                "create, e.g. "
                                "/Users/me/Films/ITW.cutmachine-project."),
                   true)
            .Field("name", StringSchema("Project name."), true)
            .Field("format",
                   StringSchema("First timeline's format as WIDTHxHEIGHT@RATE, "
                                "e.g. 1080x1920@25 or 3840x2160@24000/1001. "
                                "Defaults to 1920x1080@25."),
                   false)
            .Field("require_timeline", BoolSchema(requireTimelineDescription),
                   false)
            .Build("create_project arguments"),
        DispatchCreateProject);

    add("open_project",
        "Open an existing project in this server, by the absolute path of its "
        "package directory or of its project.cutmachine.json. "
        // PRD18 -- ROADMAP.md. The note on the app, which the server's
        // instructions no longer have room for.
        "A server started with --stdio instead hands the call to the "
        "CUTMACHINE window that has the project open, and the answer carries "
        "`app: true`: the editor watches and can undo what you do, "
        "get_editor_view says what they are looking at, and the markers they "
        "place are in describe (only: markers). "
        "When this machine's FFmpeg is missing, or lacks a filter or an "
        "encoder the engine uses, the answer carries `ffmpeg`: the error, "
        "and each lack with what it prevents.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the package directory or of "
                                "its project.cutmachine.json."),
                   true)
            .Field("require_timeline", BoolSchema(requireTimelineDescription),
                   false)
            .Build("open_project arguments"),
        DispatchOpenProject);

    add("ingest_media",
        "Add one media file, or every media file of a directory, to the open "
        "project's library and mount each as a source -- what `--ingest` does, "
        "under the lock this server holds. Media already in the library are "
        "skipped; files that are not media are listed with the reason. The "
        "audio of each new file is decoded to measure its level, so a large "
        "folder takes a while. The new media and the moves into `bin_id` are "
        "one step of the timeline's journal: `undo` takes the import back.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of a media file or a "
                                "directory."),
                   true)
            .Field("recursive",
                   BoolSchema("Also scan subdirectories; default false."),
                   false)
            .Field("bin_id",
                   StringSchema("Bin ULID or alias to file the media in; "
                                "\"\" is the root. Media already in the "
                                "library move there too. Absent: new media "
                                "go to the root and known ones stay."),
                   false)
            .Build("ingest_media arguments"),
        DispatchIngestMedia);

    // PRD14 -- ROADMAP.md. The aliases are said here, once: the snapshot
    // showed them and no description told an agent that a tool takes them,
    // nor what one is worth after the edit that follows. The spelling and
    // the rule are IdResolver.h's.
    add("describe",
        "Return the same JSON snapshot of the project as `--describe`: "
        "sequence, tracks, clips, sources, library, bins, markers, "
        "transitions, caption styles, multicam groups and color management. "
        "Positions are frames and decimal seconds; a setting an agent can "
        "change (clip effects, opacity and audio, transitions, caption styles, "
        "color management) is spelled exactly as its tool takes it, so it can "
        "be sent back unchanged. A clip without `effects`, `opacity` or "
        "`audio` has none, full opacity, unity gain and no fade. An `alias` "
        "stands for its ID in any argument: T1... a track, A1, A2... the clips "
        "of T1 (B1... of T2), M1... a media, K1... a marker. Aliases are ranks "
        "in this snapshot and renumber after edits: after an edit, name by ID. "
        "To read less: `only` keeps the sections named (sequence, timeline, "
        "library, bins, markers, transitions, caption_styles, multicam_groups, "
        "color_management, timelines; `history`, the timeline's journal, only "
        "when named) and `kinds` keeps only the tracks of those kinds -- the "
        "captions alone are `only:[\"timeline\"], kinds:[\"caption\"]`. "
        // PRD18 -- ROADMAP.md. What the server's instructions, held under
        // the 2,048 characters a client keeps, no longer have room for.
        "In a delta, an absent `speed` is 1/1 and each `shifted` rule covers "
        "its whole stretch: apply it literally. Read again only after a "
        "failure or a change made outside this session.",
        SchemaBuilder()
            // PRD35 -- ROADMAP.md.
            .Field("only",
                   ArraySchema(
                       StringSchema("A section of the snapshot."),
                       "Sections to keep; all when omitted. `history`, never "
                       "in the full snapshot, lists the timeline's journal, "
                       "oldest first: each entry's `rank`, `type` (an "
                       "operation, or `Batch` for a group), `label`, "
                       "`operations` applied in that one step, and `undone: "
                       "true` past the undo cursor; the last 200, with the "
                       "`total` and the `applied` count. A `DeleteGap` entry "
                       "is any edit that writes the result of tracks -- a "
                       "gap closed, a rush inserted or overwritten, a range "
                       "deleted: `clips_added` and `clips_removed` tell them "
                       "apart. The journal keeps no date and no author. "
                       // PRD40 -- ROADMAP.md. Where the creation went.
                       "Creating a timeline (add_timeline, clone_timeline, "
                       "create_interview_short...) is a step of the "
                       "project's journal: its history starts with the first "
                       "edit made to it."),
                   false)
            .Field("kinds",
                   ArraySchema(EnumSchema({"video", "audio", "caption"}, ""),
                               "Track kinds to keep in the timeline; all "
                               "when omitted."),
                   false)
            .Build("describe arguments"),
        DispatchDescribe);

    add("get_editor_view",
        "What the editor is looking at in the CUTMACHINE window this session "
        "is relayed to (open_project answered app: true): the timeline on "
        "screen, the playhead (exact, and as a sequence frame), the clips "
        "under the playhead on every track with the source frame each plays "
        "there, the selected clips, and the markers the playhead is on or "
        "inside of (a marker with `duration_frames` is a passage the editor "
        "marked), and the In/Out range they set (marked_range: in and out as "
        "delete_timeline_range takes them, Out excluded; absent when neither "
        "is set). Read it when the editor says \"this clip\", \"here\", \"this "
        "passage\", \"between In and Out\": it names what they point at. "
        "Interface state, read only.",
        SchemaBuilder().Build("get_editor_view arguments"),
        DispatchGetEditorView);

    add("get_video_output",
        "List the connected screens and the editor's video clean feed: "
        "selected_screen_id is the locally remembered display UUID, "
        "active_screen_id the display currently showing Record's picture. "
        "An unplugged display suspends output, retaining the choice. "
        "The editor's own screen cannot be used. Rec.709 SDR preview, with "
        "captions, without viewer controls, scopes, zoom or reference split. "
        "Needs an open editor window (open_project answered app: true).",
        SchemaBuilder().Build("get_video_output arguments"),
        DispatchGetVideoOutput);
    add("set_video_output",
        "Show the video clean feed on a screen from get_video_output. This "
        "saves a machine-local preference, never changes the project or its "
        "edit history.",
        SchemaBuilder()
            .Field("screen_id", StringSchema("Display UUID; empty disables."),
                   true)
            .Build("set_video_output arguments"),
        DispatchSetVideoOutput);

    add("capture_editor_window",
        "The CUTMACHINE window this session is relayed to (open_project "
        "answered app: true), as a picture: what the editor sees, title bar, "
        "panels, Source, Record and the timeline included. It works with the "
        "window covered. Look at it when the editor reports something on "
        "screen, or to check how a change looks. The text lists where the "
        "timeline, Record and Source sit in the image. macOS's glass and its "
        "dimming of secondary text are missing. Interface state, read only.",
        SchemaBuilder()
            .Field("region",
                   EnumSchema({"window", "timeline", "record", "source"},
                              "The whole window (default), or one of its "
                              "pictures."),
                   false)
            .Field("scale",
                   IntSchema("Pixels per point: 1 (default), or 2 for the "
                             "Retina detail of small text."),
                   false)
            .Build("capture_editor_window arguments"),
        DispatchCaptureEditorWindow);

    add("set_active_timeline",
        "Make a timeline the active one: the default of every tool's "
        "timeline_id, and the one the window shows. In the project's journal: "
        "`undo` does not reach it.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full ID of the timeline to activate."), true)
            .Build("set_active_timeline arguments"),
        DispatchSetActiveTimeline);

    add("list_timelines",
        "List every project timeline with its stable ID, dimensions, exact "
        "frame rate, duration and active state.",
        SchemaBuilder().Build("list_timelines takes no arguments"),
        DispatchListTimelines);

    add("add_timeline",
        "Create a project timeline, in the project's journal: `undo` does not "
        "reach it.",
        SchemaBuilder()
            .Field("name", StringSchema("Timeline name."), true)
            .Field("width", PositiveIntSchema("Stored pixel width."), true)
            .Field("height", PositiveIntSchema("Stored pixel height."), true)
            .Field("frame_rate", kMediaRateSchemaText, true)
            .Field("activate",
                   BoolSchema("Make it the active timeline; default false."),
                   false)
            .Build("add_timeline arguments"),
        DispatchAddTimeline);

    add("remove_timeline",
        "Remove one project timeline, in the project's journal: `undo` does "
        "not reach it.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full ID or unambiguous prefix."), true)
            .Build("remove_timeline arguments"),
        DispatchRemoveTimeline);

    add("clone_timeline",
        "Duplicate a complete timeline with fresh object IDs, preserving exact "
        "timing, tracks, effects, captions, transitions, multicam and bin "
        "metadata. Sources are reused. In the project's journal: `undo` does "
        "not reach it.",
        SchemaBuilder()
            .Field(
                "source_timeline_id",
                StringSchema("Full source timeline ID or unambiguous prefix."),
                true)
            .Field("name", StringSchema("New timeline name."), true)
            .Field("activate",
                   BoolSchema("Make the clone the active timeline; default "
                              "false."),
                   false)
            .Build("clone_timeline arguments"),
        DispatchCloneTimeline);

    // MOTIF6 -- docs/roadmap/MOTIF-2026-10.md, DEC41.
    add("choose_timeline_variant",
        "Record a choice between timelines (variants, clones, declensions) in "
        "the project's journal: the one kept, the ones it was chosen among, "
        "and why. `motif` author: person when the person chose, agent when the "
        "choice was left to you. Changes nothing in the project but, with "
        "activate, the active timeline. list_motifs with journal: project "
        "reads the choices back.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("The timeline kept: full ID or unambiguous "
                                "prefix."),
                   true)
            .Field("alternative_timeline_ids",
                   ArraySchema("{\"type\":\"string\"}",
                               "The timelines it was chosen among, at least "
                               "one: full IDs or unambiguous prefixes."),
                   true)
            .Field("activate",
                   BoolSchema("Make the kept timeline the active one; default "
                              "false."),
                   false)
            .Field("motif",
                   "{\"type\":\"object\",\"description\":\"Why it was "
                   "chosen: {reason, note, author}, author required (person or "
                   "agent); see list_motifs.\"}",
                   true)
            .Build("choose_timeline_variant arguments"),
        DispatchChooseTimelineVariant);

    add("create_timeline_variants",
        "Create dates/text variants across all addressed templates as one "
        "atomic project operation, in the project's journal: `undo` does not "
        "reach it. Only caption-track literal text changes. A preview returns "
        "before/after text and a guarded operation.",
        SchemaBuilder()
            .Field(
                "templates",
                ArraySchema(
                    SchemaBuilder()
                        .Field("timeline_id", StringSchema("Full template ID."),
                               true)
                        .Field("suffix",
                               StringSchema("Format suffix, e.g. _4x5."), true)
                        .Field("clip_id",
                               StringSchema(
                                   "Optional full source caption clip ID."),
                               false)
                        .Build("Template"),
                    "Templates for every variant."),
                true)
            .Field(
                "variants",
                ArraySchema(
                    SchemaBuilder()
                        .Field("name", StringSchema("Variant name prefix."),
                               true)
                        .Field("text",
                               StringSchema("Literal replacement text."), true)
                        .Build("Variant"),
                    "Dates or text values."),
                true)
            .Field("find", StringSchema("Literal old date/text substring."),
                   true)
            .Field("expected_count",
                   PositiveIntSchema("Occurrences per template; default 1."),
                   false)
            .Field("preview", BoolSchema("Preview only; default true."), false)
            .Build("create_timeline_variants arguments"),
        DispatchCreateTimelineVariants);

    add("rename_timeline",
        "Rename one project timeline, in the project's journal: `undo` does "
        "not reach it.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full ID or unambiguous prefix."), true)
            .Field("name", StringSchema("New timeline name."), true)
            .Build("rename_timeline arguments"),
        DispatchRenameTimeline);

    add("set_timeline_bin",
        "File one project timeline into a bin (or back to the project root) "
        "through the reversible project edit log, as set_media_bin files a "
        "rush. list_timelines and describe give each filed timeline its "
        "bin_id.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full ID or unambiguous prefix."), true)
            .Field("bin_id",
                   StringSchema("Destination bin; omit or empty for the "
                                "project root. ID, unique prefix or alias."),
                   false)
            .Build("set_timeline_bin arguments"),
        DispatchSetTimelineBin);
}
