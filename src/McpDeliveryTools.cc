// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// delivery and exchange: checks and stats, exports and their verification,
// Resolve XML and EDL. Moved out of McpTools.cc as they were; what they share
// with the other files of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"
#include "ResolveSend.h"

using namespace mcp_tools;

namespace {

// OR4 -- docs/roadmap/DONNEES-2026-10.md. The montage against a policy,
// read-only: the document always, the delivered file when `path` names it
// (TimelineCheck.h). A policy that cannot be read, or a window that is not
// whole frames, is refused; a check the engine cannot run is reported as
// not run, never as passed.
bool DispatchCheckTimeline(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "check_timeline";
    if (!CheckKnownKeys(args, {"policy", "path"}, tool, message))
        return Fail(errorName, message, message);
    std::string path;
    if (!ReadString(args, "path", tool, false, "", path, message))
        return Fail(errorName, message, message);
    TimelinePolicy policy;
    if (const Value* field = args.Find("policy")) {
        if (!ParseTimelinePolicy(*field, policy, message))
            return Fail(errorName, message, message);
    }
    // DEC6 -- the charter the policy names, from this user's library.
    if (!policy.kit_name.empty()) {
        MotionKitInfo kit;
        if (!FindMotionLibraryKit(CurrentMotionLibraryLocation(),
                                  policy.kit_name, kit, message)) {
            errorName = "UnknownKit";
            return false;
        }
        policy.kit = std::move(kit);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    TimelineFileMeasures measures;
    if (!path.empty()) {
        int64_t window = 0;
        if (!FinalWindowFrames(document.sequence, policy, window, message))
            return Fail(errorName, message, message);
        // PRD1 -- no path of its own: the binary FfmpegBinary.h resolves.
        if (!MeasureDeliveredFile(document, policy, path, std::string(),
                                  measures, message)) {
            errorName = "IoError";
            return false;
        }
    }
    TimelineCheckReport report;
    if (!CheckTimeline(document, policy, path.empty() ? nullptr : &measures,
                       report, message))
        return Fail(errorName, message, message);
    resultJson = SerializeTimelineCheck(report);
    return true;
}

// R10 -- docs/roadmap/REEL-2026-09.md.
bool DispatchImportResolveTimeline(McpBackend& backend, const IdResolver&,
                                   const Value& args, std::string& resultJson,
                                   std::string& errorName,
                                   std::string& message) {
    const std::string tool = "import_resolve_timeline";
    std::string path;
    std::string database;
    // PRD17 -- `timeline_id`, read by McpToolRegistry::Call, which selects
    // the timeline the import writes into: the active one when it is left
    // out, as before.
    // DEC15 (GR15) -- `resolve_database`, the grades read beside the cut.
    if (!CheckKnownKeys(args,
                        {"manifest_path", "timeline_id", "resolve_database"},
                        tool, message) ||
        !ReadString(args, "manifest_path", tool, true, "", path, message) ||
        !ReadString(args, "resolve_database", tool, false, "", database,
                    message))
        return Fail(errorName, message, message);
    return backend.ImportResolveTimeline(path, database, resultJson, errorName,
                                         message);
}

// PA16 -- docs/roadmap/PARITE-2026-11.md. The montage to Resolve, whole, in
// one call: the descriptor of the selected timeline, the bridge, its report
// (ResolveSend.h). Changes nothing in the project.
bool DispatchSendToResolve(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    const std::string tool = "send_to_resolve";
    std::string grade;
    int64_t node = 1;
    ResolveSendRequest request;
    if (!CheckKnownKeys(args, {"name", "grade", "resolve_node", "dry_run"},
                        tool, message) ||
        !ReadString(args, "name", tool, false, "", request.name, message) ||
        !ReadString(args, "grade", tool, false, "none", grade, message) ||
        !ReadInt64(args, "resolve_node", tool, false, 1, node, message) ||
        !ReadBool(args, "dry_run", tool, false, request.dry_run, message))
        return Fail(errorName, message, message);
    if (grade != "none" && grade != "cdl")
        return Fail(errorName, message,
                    "'" + tool + ".grade' must be 'none' or 'cdl'");
    if (node < 1 || node > 1000)
        return Fail(errorName, message,
                    "'" + tool + ".resolve_node' is a node number, from 1");
    if (request.name.find(':') != std::string::npos)
        return Fail(errorName, message,
                    "'" + tool +
                        ".name' holds a ':', which Resolve refuses in a "
                        "timeline name (measured 12 October): write 16x9, "
                        "not 16:9");
    request.options.grade_cdl = grade == "cdl";
    request.options.cdl_node = static_cast<int>(node);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return SendTimelineToResolve(
        std::move(document), backend.PackageDirectory(), request,
        LocateResolveBridge(), resultJson, errorName, message);
}

// R9 -- docs/roadmap/REEL-2026-09.md.
bool DispatchExportResolveXml(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    const std::string tool = "export_resolve_xml";
    std::string path;
    std::string scaling;
    if (!CheckKnownKeys(args, {"path", "resolve_scaling"}, tool, message) ||
        !ReadString(args, "path", tool, true, "", path, message) ||
        !ReadString(args, "resolve_scaling", tool, false, "", scaling, message))
        return Fail(errorName, message, message);
    return backend.ExportResolveXml(path, scaling, resultJson, errorName,
                                    message);
}

// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. The CMX 3600 lists (Edl.h),
// written from the selected timeline's snapshot: no backend capability of
// their own, the way export_cdl writes its file, so the window's chat and a
// server write the same bytes. Like export_resolve_xml, the destination must
// be absolute and new (OutputExists).
bool ReadEdlCommon(const Value& args, const std::string& tool,
                   std::string& path, std::string& startTimecode,
                   std::string& errorName, std::string& message) {
    if (!ReadString(args, "path", tool, true, "", path, message) ||
        !ReadString(args, "start_timecode", tool, false,
                    kEdlDefaultStartTimecode, startTimecode, message))
        return Fail(errorName, message, message);
    if (!std::filesystem::path(path).is_absolute())
        return Fail(errorName, message,
                    "'" + tool + ".path' must be an absolute path");
    return true;
}

bool DispatchExportEdl(McpBackend& backend, const IdResolver&,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    const std::string tool = "export_edl";
    std::string path;
    EdlExportOptions options;
    if (!CheckKnownKeys(args, {"path", "start_timecode"}, tool, message))
        return Fail(errorName, message, message);
    if (!ReadEdlCommon(args, tool, path, options.start_timecode, errorName,
                       message))
        return false;
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return ExportEdlFiles(document, path, options, resultJson, errorName,
                          message);
}

// PA1 -- docs/roadmap/PARITE-2026-11.md. The two files the other editors
// read (Interchange.h), written from the selected timeline's snapshot the
// way export_edl writes its lists; the package directory resolves the
// library's relative media paths, which another editor cannot.
bool DispatchExportInterchange(McpBackend& backend, const Value& args,
                               const std::string& tool, bool otio,
                               std::string& resultJson, std::string& errorName,
                               std::string& message) {
    std::string path;
    if (!CheckKnownKeys(args, {"path"}, tool, message) ||
        !ReadString(args, "path", tool, true, "", path, message))
        return Fail(errorName, message, message);
    if (!std::filesystem::path(path).is_absolute())
        return Fail(errorName, message,
                    "'" + tool + ".path' must be an absolute path");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    const std::string directory = backend.PackageDirectory();
    return otio ? ExportOtioFile(std::move(document), directory, path,
                                 resultJson, errorName, message)
                : ExportFcpxmlFile(std::move(document), directory, path,
                                   resultJson, errorName, message);
}

bool DispatchExportOtio(McpBackend& backend, const IdResolver&,
                        const Value& args, std::string& resultJson,
                        std::string& errorName, std::string& message) {
    return DispatchExportInterchange(backend, args, "export_otio", true,
                                     resultJson, errorName, message);
}

bool DispatchExportFcpxml(McpBackend& backend, const IdResolver&,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    return DispatchExportInterchange(backend, args, "export_fcpxml", false,
                                     resultJson, errorName, message);
}

bool DispatchExportEdlMarkers(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    const std::string tool = "export_edl_markers";
    std::string path;
    std::string startTimecode;
    std::string styleName;
    if (!CheckKnownKeys(args, {"path", "style", "start_timecode"}, tool,
                        message) ||
        !ReadString(args, "style", tool, false, "loc", styleName, message))
        return Fail(errorName, message, message);
    if (!ReadEdlCommon(args, tool, path, startTimecode, errorName, message))
        return false;
    EdlMarkerStyle style = EdlMarkerStyle::Loc;
    if (!ParseEdlMarkerStyle(styleName, style))
        return Fail(errorName, message,
                    "'" + tool + ".style' must be 'loc' or 'resolve'");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    return ExportEdlMarkersFile(document, path, style, startTimecode,
                                resultJson, errorName, message);
}

bool DispatchImportEdlMarkers(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    const std::string tool = "import_edl_markers";
    std::string path;
    std::string startTimecode;
    // `timeline_id` is read by McpToolRegistry::Call, like every edit's.
    if (!CheckKnownKeys(args, {"path", "start_timecode", "timeline_id"}, tool,
                        message))
        return Fail(errorName, message, message);
    if (!ReadString(args, "path", tool, true, "", path, message) ||
        !ReadString(args, "start_timecode", tool, false,
                    kEdlDefaultStartTimecode, startTimecode, message))
        return Fail(errorName, message, message);
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        errorName = "IoError";
        message = "cannot open '" + path + "'";
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    EdlMarkerImport imported;
    if (!BuildEdlMarkerImport(document, contents.str(), startTimecode, imported,
                              message))
        return Fail(errorName, message, message);
    Value report;
    std::string parseError;
    Value::Parse(EdlMarkerImportReportJson(imported, path), report, parseError);
    if (imported.operations.empty()) {
        Value reply = Value::MakeObject();
        reply.Set("applied", Value::MakeBool(false));
        reply.Set("edl_markers", std::move(report));
        resultJson = reply.Dump();
        return true;
    }
    std::string applied;
    if (!backend.ApplyOperation(EdlMarkerImportOperation(imported), applied,
                                errorName, message))
        return false;
    Value reply;
    if (!Value::Parse(applied, reply, parseError) || !reply.IsObject()) {
        resultJson = applied;
        return true;
    }
    reply.Set("edl_markers", std::move(report));
    resultJson = reply.Dump();
    return true;
}

// QC-2026-09 (A8) -- count the composited result, not the clips underneath
// it. The pure calculation is shared with `--timeline-stats`, so an agent and
// a human checking the same project receive identical arithmetic.
bool DispatchTimelineStats(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {}, "timeline_stats", message))
        return Fail(errorName, message, message);
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    TimelineStats stats;
    if (!CalculateTimelineStats(document, stats, message))
        return Fail(errorName, message, message);
    resultJson = SerializeTimelineStats(stats);
    return true;
}

// PRD16 -- ROADMAP.md. The settings export_timeline and each entry of
// export_timelines encode `timeline` with. A preset is resolved at the
// timeline's format; no codec and no preset is the engine's default for it
// (Exporter::DefaultSettings). A codec named alone keeps, byte for byte, what
// these tools have always built for it: H.264 at H264Preset's bit rate, HEVC
// at 45 Mbit/s whatever the size -- not what `--codec hevc` builds, which
// scales its bit rate; `preset: hevc_high_quality` is that one. `zeroFormat`
// leaves the size and cadence of a codec named alone to the batch, which
// gives each entry its timeline's exact format (Exporter::ExportBatch).
ExportSettings ChooseExportSettings(const Document& timeline,
                                    const std::string& codec,
                                    const std::string& presetKey,
                                    ExportPresetId preset,
                                    const std::string& outputPath,
                                    bool zeroFormat) {
    if (!presetKey.empty()) {
        return Exporter::SettingsForPreset(preset, timeline, outputPath);
    }
    if (codec.empty()) {
        return Exporter::DefaultSettings(timeline, outputPath);
    }
    ExportSettings settings =
        codec == "h264" ? Exporter::H264Preset(outputPath) : ExportSettings{};
    settings.output_path = outputPath;
    if (zeroFormat) {
        settings.width = 0;
        settings.height = 0;
        settings.frame_rate = {0, 1};
    } else {
        settings.width = timeline.sequence.width;
        settings.height = timeline.sequence.height;
        settings.frame_rate = timeline.sequence.frame_rate;
    }
    return settings;
}

// PA2 -- docs/roadmap/PARITE-2026-11.md. `loudness_target`, in whole LUFS:
// the loudness the delivery is brought to (ExportLoudnessTarget). Left out,
// the mix is delivered as it stands.
bool ReadLoudnessTarget(const Value& args, const std::string& tool,
                        ExportLoudnessTarget& loudness, std::string& message) {
    if (args.Find("loudness_target") == nullptr) return true;
    int64_t lufs = 0;
    if (!ReadInt64(args, "loudness_target", tool, true, 0, lufs, message))
        return false;
    if (lufs * 1000 < kMinimumLoudnessTargetMillilufs ||
        lufs * 1000 > kMaximumLoudnessTargetMillilufs) {
        message = "'" + tool +
                  ".loudness_target' is a loudness in LUFS, from -40 to -5 "
                  "(-14 for the web, -23 for broadcast)";
        return false;
    }
    loudness.requested = true;
    loudness.integrated_millilufs = static_cast<int32_t>(lufs * 1000);
    return true;
}

bool DispatchExportTimelines(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    const std::string tool = "export_timelines";
    if (!CheckKnownKeys(args,
                        {"outputs", "codec", "software", "overwrite", "preview",
                         "sdr_tag", "loudness_target"},
                        tool, message)) {
        return Fail(errorName, message, message);
    }
    std::string codec;
    bool software = false;
    bool overwrite = false;
    bool preview = true;
    std::string sdrTag;
    if (!ReadString(args, "sdr_tag", tool, false, "rec709", sdrTag, message) ||
        !ReadString(args, "codec", tool, false, "", codec, message) ||
        !ReadBool(args, "software", tool, false, software, message) ||
        !ReadBool(args, "overwrite", tool, false, overwrite, message) ||
        !ReadBool(args, "preview", tool, true, preview, message)) {
        return Fail(errorName, message, message);
    }
    ExportLoudnessTarget loudness;
    if (!ReadLoudnessTarget(args, tool, loudness, message))
        return Fail(errorName, message, message);
    const auto* outputs = args.Find("outputs");
    if ((outputs == nullptr) || !outputs->IsArray() ||
        outputs->AsArray().empty() ||
        (!codec.empty() && codec != "h264" && codec != "hevc") ||
        (sdrTag != "rec709" && sdrTag != "rec709a")) {
        return Fail(errorName, message,
                    "nonempty outputs, codec h264 or hevc and sdr_tag rec709 "
                    "or rec709a are required");
    }
    // PRD16 -- the format, and the default, of each entry are its own
    // timeline's: read once, for every entry.
    Project project;
    std::string snapshotError;
    const bool hasProject = backend.SnapshotProject(project, snapshotError);
    std::vector<std::pair<Ulid, ExportSettings>> items;
    for (size_t index = 0; index < outputs->AsArray().size(); ++index) {
        const Value& output = outputs->AsArray()[index];
        std::string id;
        std::string path;
        std::string presetKey;
        if (!output.IsObject() ||
            !CheckKnownKeys(output, {"timeline_id", "output_path", "preset"},
                            tool, message) ||
            !ReadString(output, "timeline_id", tool, true, "", id, message) ||
            !ReadString(output, "output_path", tool, true, "", path, message) ||
            !ReadString(output, "preset", tool, false, "", presetKey,
                        message)) {
            return Fail(errorName, message, "invalid batch output: " + message);
        }
        ExportPresetId preset = ExportPresetId::H264;
        if (!presetKey.empty() &&
            !Exporter::ReadPreset(
                presetKey, codec,
                tool + ".outputs[" + std::to_string(index) + "].preset", preset,
                message)) {
            return Fail(errorName, message, message);
        }
        // A timeline this snapshot does not hold leaves its format to the
        // batch, which refuses it by name before anything is rendered.
        const bool known = hasProject && project.FindTimeline(id) != nullptr;
        ExportSettings settings =
            ChooseExportSettings(known ? project.MakeDocument(id) : Document{},
                                 codec, presetKey, preset, path, true);
        if (!known) {
            settings.width = 0;
            settings.height = 0;
            settings.frame_rate = {0, 1};
        }
        settings.overwrite = overwrite;
        settings.loudness = loudness;
        if (sdrTag == "rec709a") settings.sdr_tag = ExportSdrTag::Rec709A;
        if (software) {
            Exporter::UseSoftwareEncoder(settings);
        }
        items.emplace_back(id, std::move(settings));
    }
    return backend.ExportProjectTimelines(items, preview, resultJson, errorName,
                                          message);
}

bool DispatchExportTimeline(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(
            args,
            {"timeline_id", "output_path", "codec", "preset", "software",
             "overwrite", "sdr_tag", "verify", "loudness_target"},
            "export_timeline", message)) {
        return Fail(errorName, message, message);
    }
    ExportLoudnessTarget loudness;
    if (!ReadLoudnessTarget(args, "export_timeline", loudness, message))
        return Fail(errorName, message, message);
    std::string timelineId;
    std::string outputPath;
    std::string codec;
    std::string presetKey;
    bool software = false;
    bool overwrite = false;
    bool verify = false;
    std::string sdrTag;
    if (!ReadBool(args, "verify", "export_timeline", false, verify, message) ||
        !ReadString(args, "sdr_tag", "export_timeline", false, "rec709", sdrTag,
                    message) ||
        !ReadString(args, "timeline_id", "export_timeline", false, "",
                    timelineId, message) ||
        !ReadString(args, "output_path", "export_timeline", true, "",
                    outputPath, message) ||
        !ReadString(args, "codec", "export_timeline", false, "", codec,
                    message) ||
        !ReadString(args, "preset", "export_timeline", false, "", presetKey,
                    message) ||
        !ReadBool(args, "software", "export_timeline", false, software,
                  message) ||
        !ReadBool(args, "overwrite", "export_timeline", false, overwrite,
                  message)) {
        return Fail(errorName, message, message);
    }
    if (timelineId.empty()) {
        // PRD16 -- kept required: an export names its film. The refusal
        // names every timeline there is, so the agent answers in one turn.
        Project project;
        std::string snapshotError;
        return Fail(errorName, message,
                    "'export_timeline.timeline_id' is required: name the "
                    "timeline to export" +
                        (backend.SnapshotProject(project, snapshotError)
                             ? " -- " + Exporter::DescribeTimelines(project)
                             : std::string()));
    }
    if ((!codec.empty() && codec != "h264" && codec != "hevc") ||
        (sdrTag != "rec709" && sdrTag != "rec709a")) {
        return Fail(errorName, message,
                    "export requires a timeline ID, codec h264 or hevc and "
                    "sdr_tag rec709 or rec709a");
    }
    ExportPresetId preset = ExportPresetId::H264;
    if (!presetKey.empty() &&
        !Exporter::ReadPreset(presetKey, codec, "export_timeline.preset",
                              preset, message)) {
        return Fail(errorName, message, message);
    }
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    ExportSettings settings = ChooseExportSettings(document, codec, presetKey,
                                                   preset, outputPath, false);
    if (software) {
        Exporter::UseSoftwareEncoder(settings);
    }
    settings.overwrite = overwrite;
    settings.loudness = loudness;
    if (sdrTag == "rec709a") settings.sdr_tag = ExportSdrTag::Rec709A;
    if (!backend.ExportTimeline(settings, resultJson, errorName, message))
        return false;
    if (!verify) return true;
    // AGENT-2026-09 A7 -- ROADMAP.md. The check every delivery makes next,
    // in the same call: the file is compared to the document as
    // verify_export does, and its report rides along as `verification`.
    // The export stands whatever the check says; a check that could not
    // run says so in `verification_error` rather than failing the export.
    Value exported;
    std::string parseError;
    if (!Value::Parse(resultJson, exported, parseError) || !exported.IsObject())
        return true;
    std::string verificationJson;
    std::string verifyErrorName;
    std::string verifyMessage;
    if (backend.VerifyExportedFile(outputPath, verificationJson,
                                   verifyErrorName, verifyMessage)) {
        Value verification;
        if (Value::Parse(verificationJson, verification, parseError))
            exported.Set("verification", std::move(verification));
        else
            exported.Set("verification_error",
                         Value::MakeString("unreadable report: " + parseError));
    } else {
        exported.Set("verification_error",
                     Value::MakeString(verifyErrorName + ": " + verifyMessage));
    }
    resultJson = exported.Dump();
    return true;
}

bool DispatchVerifyExport(McpBackend& backend, const IdResolver&,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    if (!CheckKnownKeys(args, {"timeline_id", "path"}, "verify_export",
                        message)) {
        return Fail(errorName, message, message);
    }
    std::string timelineId;
    std::string path;
    if (!ReadString(args, "timeline_id", "verify_export", true, "", timelineId,
                    message) ||
        !ReadString(args, "path", "verify_export", true, "", path, message)) {
        return Fail(errorName, message, message);
    }
    if (timelineId.empty() || path.empty()) {
        return Fail(errorName, message,
                    "verify_export requires a timeline ID and a file path");
    }
    return backend.VerifyExportedFile(path, resultJson, errorName, message);
}

}  // namespace

void RegisterDeliveryTools(const McpToolAdder& add) {
    add("check_timeline",
        "Check a timeline against a delivery policy, without editing it. Seven "
        "checks, each passed, failed or not_run with its reason: links "
        "(picture and sound of one source playing together are linked), sync "
        "(every linked pair reads its source at one offset), duration (inside "
        "the policy's bounds; not run without any), final_black (the last "
        "second; with path, the file's frames too), caption_contrast (on the "
        "file only, as check_caption_legibility), charter (with policy.kit: "
        "every caption or motion font and colour the charter does not allow, "
        "named where it is set) and jump_cuts (a cut inside one rush shown as "
        "it is, judged only with policy.jump_cuts \"forbidden\"). Run it "
        "before exporting, and again with path on the final file. Beside the "
        "checks, judged by none, `generated` lists every clip whose media a "
        "generation service made (clip, track, media, range, service, model): "
        "name each one in your report to the person; and `jump_cuts` lists "
        "every jump cut (frame, track, the two clips, the rush), whatever the "
        "policy. Two more checks are listed only when the policy asks for "
        "them: loudness (policy.loudness_target, on the file only: its "
        "integrated loudness within one LU of the target and its true peak "
        "under policy.true_peak_max) and format (policy.aspect_ratio against "
        "the sequence's size).",
        SchemaBuilder()
            .Field("policy",
                   std::string("{\"type\":\"object\",\"description\":\"") +
                       Esc("What the timeline is judged against; every key "
                           "optional. Without name, a policy that changes "
                           "anything is called \"custom\". final_window is "
                           "whole frames, one second by default; "
                           "minimum_contrast is 3/1 by default.") +
                       "\",\"properties\":{\"name\":" +
                       StringSchema("Said in the report, 1 to 64 bytes.") +
                       ",\"duration_min\":" + kTimeSchemaText +
                       ",\"duration_max\":" + kTimeSchemaText +
                       ",\"final_black\":" +
                       EnumSchema({"forbidden", "allowed"},
                                  "Black at the end of the film; forbidden "
                                  "by default.") +
                       ",\"jump_cuts\":" +
                       EnumSchema({"forbidden", "allowed"},
                                  "A cut inside one rush shown as it is; "
                                  "allowed (listed only) by default, "
                                  "forbidden for an interview.") +
                       ",\"final_window\":" + kTimeSchemaText +
                       ",\"minimum_contrast\":" + kFractionSchemaText +
                       ",\"kit\":" +
                       StringSchema("A brand's charter of the library "
                                    "(list_motion_templates kits): adds "
                                    "the charter check.") +
                       ",\"loudness_target\":" +
                       IntSchema("The delivered file's loudness, in whole "
                                 "LUFS from -40 to -5 (the one given to "
                                 "export_timeline): adds the loudness "
                                 "check, judged with path only.") +
                       ",\"true_peak_max\":" +
                       IntSchema("The file's highest true peak, in whole "
                                 "dBTP from -20 to 0; -1 by default. With "
                                 "loudness_target only.") +
                       ",\"aspect_ratio\":" +
                       StringSchema("The frame's shape, as \"9:16\": adds "
                                    "the format check.") +
                       "},\"additionalProperties\":false}",
                   false)
            .Field("path",
                   StringSchema("The file exported from this timeline, .mp4 "
                                "or .mov: adds its last frames and its "
                                "cartons to the checks."),
                   false)
            .Build("check_timeline arguments"),
        DispatchCheckTimeline);

    add("timeline_stats",
        "Calculate exact editorial statistics for the composited timeline: "
        "duration, visible shots and cuts, shots per minute, cutaway share, "
        "and the first visible cutaway. Clips hidden underneath a higher "
        "visible video track do not count as visible changes.",
        SchemaBuilder().Build("timeline_stats arguments"),
        DispatchTimelineStats);

    // PRD16 -- the preset keys, in the engine's order: what the schema
    // offers is what --export --preset accepts.
    std::vector<std::string> presetKeys;

    for (const ExportPresetDescriptor& preset : Exporter::Presets())
        presetKeys.emplace_back(preset.key);

    const std::string presetDescription =
        "Export preset, as the app and --export --preset name it: h264 (High "
        "8-bit), hevc_high_quality (Main10, hardware), hevc_master (Main10, "
        "x265 CRF 18), hevc_web_1080 (1920x1080, 20 Mbit/s), "
        "hevc_uhd_delivery (3840x2160, 80 Mbit/s); the others keep the "
        "timeline's size. A codec that contradicts it is refused.";

    add("export_timeline",
        "Export one entire timeline atomically, at its stored dimensions (or "
        "its preset's) and exact frame rate; every clip is placed on an exact "
        "frame. Without codec or preset, the engine's default for the "
        "timeline: H.264 High 8-bit, or HEVC Main10 when the sequence delivers "
        "HLG -- the file --export writes. Grades, fades and opacity render as "
        "on the monitor. Visible caption tracks are rendered with their style. "
        "The export does not edit the project.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full timeline ID to export. Required: guards "
                                "against exporting the wrong film."),
                   true)
            .Field("output_path",
                   StringSchema("Destination .mp4 or .mov; a server takes a "
                                "relative path from the project's folder."),
                   true)
            .Field("codec",
                   EnumSchema({"h264", "hevc"},
                              "Without it, the preset's codec or the "
                              "engine's default."),
                   false)
            .Field("preset", EnumSchema(presetKeys, presetDescription), false)
            .Field("software",
                   BoolSchema("Use software encoding; default false."), false)
            .Field("overwrite",
                   BoolSchema("Replace an existing export; default false."),
                   false)
            .Field("sdr_tag",
                   EnumSchema({"rec709", "rec709a"},
                              "How an SDR file tells players to show it: "
                              "rec709 (default, 1-1-1, shown lighter by "
                              "Apple's players) or rec709a (Resolve's "
                              "Rec.709-A, gamma 2.4 as on the monitor; .mov "
                              "only)."),
                   false)
            .Field("loudness_target",
                   IntSchema("Loudness the delivery is brought to, in whole "
                             "LUFS from -40 to -5 (-14 for the web, -23 for "
                             "broadcast): the rendered mix is measured as a "
                             "broadcast meter does, one gain brings it there "
                             "under the mix's limiter, and `loudness` in the "
                             "answer says what was measured before and "
                             "after. Left out, the mix is delivered as it "
                             "stands."),
                   false)
            .Field("verify",
                   BoolSchema("true: compare the file with the timeline "
                              "right after, as verify_export does, and "
                              "return its report as `verification`; "
                              "default false."),
                   false)
            .Build("export_timeline arguments"),
        DispatchExportTimeline);

    add("verify_export",
        "Compare an exported file with its timeline, frame by frame, and name "
        "the frames that diverge. Compared on normalised luma thumbnails: a "
        "grade or a colour transform is not a difference. Reports runs of "
        "frames: 'offset' (another frame of the expected source is shown; "
        "offset is shown minus expected source frame), 'wrong_picture', "
        "'picture_in_gap' and 'black_frame', plus the exported and expected "
        "frame counts. A motion clip alone is checked against its render; over "
        "a picture, like dissolves and translucent clips, it is unverifiable. "
        "Frames of a still shot whose neighbours look identical are counted as "
        "ambiguous and never reported as shifted. `exact` is true only if the "
        "file has exactly the expected frames, every one of them was compared "
        "and none diverges: an unverifiable frame keeps it false, with "
        "verified_fraction (the share compared) and not_exact_because beside "
        "it, and divergent_frames says whether anything compared is wrong. "
        "Slow: run it once, on the final file. Read-only.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Full timeline ID the file was exported from."),
                   true)
            .Field("path",
                   StringSchema("Exported .mp4 or .mov file; a server takes a "
                                "relative path from the project's folder."),
                   true)
            .Build("verify_export arguments"),
        DispatchVerifyExport);

    add("export_timelines",
        "Validate all entire timelines and distinct output paths before "
        "exporting a batch at each timeline's exact format, unless an entry's "
        "preset sets its size. Without codec or preset, each entry takes the "
        "engine's default for its timeline, as in export_timeline. Encoding is "
        "sequential; completed files are retained and listed if a later item "
        "fails.",
        SchemaBuilder()
            .Field(
                "outputs",
                ArraySchema(
                    SchemaBuilder()
                        .Field("timeline_id", StringSchema("Full timeline ID."),
                               true)
                        .Field("output_path",
                               StringSchema("Destination .mp4 or .mov; "
                                            "relative: the project's folder, "
                                            "in a server."),
                               true)
                        .Field("preset",
                               EnumSchema(presetKeys,
                                          "This entry's preset, as in "
                                          "export_timeline."),
                               false)
                        .Build("Output"),
                    "Explicit timelines and destinations."),
                true)
            .Field("codec",
                   EnumSchema({"h264", "hevc"},
                              "Every entry's codec; without it, each "
                              "entry's preset or default."),
                   false)
            .Field("software", BoolSchema("Software encoder; default false."),
                   false)
            .Field("overwrite",
                   BoolSchema("Replace existing files; default false."), false)
            .Field("preview", BoolSchema("Preview only; default true."), false)
            .Field("sdr_tag",
                   EnumSchema({"rec709", "rec709a"},
                              "rec709 (default, 1-1-1) or rec709a (Resolve's "
                              "Rec.709-A, gamma 2.4, .mov only), as in "
                              "export_timeline."),
                   false)
            .Field("loudness_target",
                   IntSchema("Loudness the delivery is brought to, in whole "
                             "LUFS from -40 to -5 (-14 for the web, -23 for "
                             "broadcast): the rendered mix is measured as a "
                             "broadcast meter does, one gain brings it there "
                             "under the mix's limiter, and `loudness` in the "
                             "answer says what was measured before and "
                             "after. Every entry's; left out, each mix is "
                             "delivered as it stands."),
                   false)
            .Build("export_timelines arguments"),
        DispatchExportTimelines);

    // R10 -- docs/roadmap/REEL-2026-09.md. The timeline beside the Media
    // Pool: `--import-resolve` brings the rushes in, this reads the cut.
    add("import_resolve_timeline",
        "Read a DaVinci Resolve timeline into a timeline of the open project "
        "(timeline_id; default the active one), from the manifest "
        "`resolve_bridge.py --read-timeline` writes: clips, source ranges, "
        "video and audio tracks, and links to media already in the library, "
        "clip speeds and cross dissolves. Import the Media Pool first "
        "(`--import-resolve`): this never adds a media, so it cannot duplicate "
        "one. Refused, by name and without writing anything, when the timeline "
        "carries what the document cannot express -- a framing, a Resolve "
        "effect, a clip with no file, a media the library does not hold, a "
        "speed that could not be measured, a transition that is not a cross "
        "dissolve between two adjacent clips (a fade from black or another "
        "kind of transition). The one thing the Resolve scripting API does not "
        "publish is the keyframes of a framing: what it could not see is "
        "listed in `unpublished`. With resolve_database (the project's "
        "Project.db), the answer also says what was graded in Resolve, in "
        "`grades`: for each graded clip, by the clip_id it gets here, its "
        "nodes and the primaries they set (lift, gamma, gain, offset, "
        "contrast, pivot, saturation, hue, temperature, tint, log wheels...), "
        "as Resolve stores them. A grade of primaries only is written as "
        "color.resolve_primaries (`written_stages`); the rest is reported in "
        "`unrendered`, `unread` and `unmatched`. The database is copied and "
        "the copy read, never the file itself; one it cannot read, or without "
        "this timeline, refuses the call.",
        SchemaBuilder()
            .Field("timeline_id",
                   StringSchema("Timeline to read it into; default: the "
                                "active one."),
                   false)
            .Field("manifest_path",
                   StringSchema("Absolute path of the manifest written by "
                                "`resolve_bridge.py --read-timeline`."),
                   true)
            .Field("resolve_database",
                   StringSchema("Absolute path of the Resolve project's "
                                "Project.db, to read its grades; left out, "
                                "none are read."),
                   false)
            .Build("import_resolve_timeline arguments"),
        DispatchImportResolveTimeline);

    // PA16 -- docs/roadmap/PARITE-2026-11.md.
    add("send_to_resolve",
        "Send the selected timeline to the project open in DaVinci Resolve "
        "Studio 21.1+, whole, as a new timeline, then read it back. Video "
        "clips go picture only, audio clips sound only on their own tracks "
        "(a voice track of rushes' own sound arrives mono, music stereo), "
        "linked as in the document; constant speeds and held frames; cross "
        "dissolves and audio cross fades; dips to black as opacity fades; "
        "framing for the project's fit or fill; opacity; disabled clips; "
        "audio gain and fades; hidden and muted tracks; markers; the first "
        "caption track as subtitles. grade 'cdl' adds each clip's grade as "
        "export_cdl folds it, on node resolve_node -- valid only when that "
        "node grades the values the engine grades. What the API cannot carry "
        "is named in `not_sent`, clip by clip, not refused: keyframes (first "
        "frame's framing kept), crops, blurs, masks, speed ramps (mean "
        "speed), motion clips (place left empty), a second caption track, "
        "caption styles, the look, grades unless asked. The answer gives "
        "`timeline`, `project`, what was set, `verified` (positions, lengths, "
        "speeds, transitions, markers, subtitles, length read back; "
        "differences in `mismatches`), `timings_ms` and `elapsed_ms` (1.7 s "
        "for 220 clips, measured). Refused before Resolve is touched: a rush "
        "missing from its Media Pool, another frame rate, a framed montage in "
        "a project that neither fits nor fills. Without `name`, the "
        "sequence's name or the next free one; a taken `name` is refused. "
        "A Resolve with a dialog open stops answering: the send gives up "
        "after 10 minutes (ResolveTimeout). dry_run writes the descriptor "
        "and stops. Changes nothing in the project.",
        SchemaBuilder()
            .Field("name",
                   StringSchema("The timeline's name in Resolve; default the "
                                "sequence's, or the next free one. No ':'."),
                   false)
            .Field("grade",
                   EnumSchema({"none", "cdl"},
                              "'none' (default): grades are named in "
                              "not_sent; 'cdl': sent as CDLs."),
                   false)
            .Field("resolve_node", IntSchema("Node the CDLs go on; default 1."),
                   false)
            .Field("dry_run",
                   BoolSchema("Write the descriptor and stop; default false."),
                   false)
            .Build("send_to_resolve arguments"),
        DispatchSendToResolve);

    // R9 -- docs/roadmap/REEL-2026-09.md. The return trip that carries a
    // speed and a dissolve, beside import_resolve_timeline's direction.
    add("export_resolve_xml",
        "Write the selected timeline as a DaVinci Resolve FCP7 XML (xmeml v5) "
        "file: clips, video layers, source ranges, clip speeds (the Time Remap "
        "filter), existing cross dissolves, and each clip's framing -- scale "
        "and position, keyed when animated -- as a Basic Motion filter. A "
        "clip's colour grade is not carried and is counted in "
        "`grades_not_sent`, its blur in `blurs_not_sent`. Import it with "
        "`resolve_bridge.py --send-xml`, which stamps the timecodes and checks "
        "every clip. send_to_resolve is the route for a whole montage (sound, "
        "subtitles, markers, speeds, transitions); this one remains for "
        "animated framing, whose keys it carries and the API cannot set. "
        "Refused, by "
        "name, when a position, a length or a source in-point is not a whole "
        "sequence frame once the speed is applied. This route carries picture "
        "only: the audio tracks left behind are counted in the answer.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the .xml file to write."),
                   true)
            .Field("resolve_scaling",
                   EnumSchema({"fit", "fill"},
                              "How the Resolve project places a picture whose "
                              "size is not the timeline's (Project Settings, "
                              "Image Scaling, Mismatched resolution files, or "
                              "the timeline's own setting): 'fit' for \"Scale "
                              "entire image to fit\", 'fill' for \"Scale full "
                              "frame with crop\"."),
                   false)
            .Build("export_resolve_xml arguments"),
        DispatchExportResolveXml);

    // FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. CMX 3600 lists (Edl.h).
    add("export_edl",
        "Write the selected timeline as CMX 3600 edit decision lists, one per "
        "visible video track, from the bottom up: `<stem>_V1.edl`, "
        "`<stem>_V2.edl`... beside `path`. Cuts, cross dissolves (D), fades "
        "from and to black (BL), and a constant speed as an M2 line only when "
        "speed * timecode base is a whole number of tenths of a frame per "
        "second (1/2 at 25 i/s is 012.5; 2/3 is refused by name). 29.97 and "
        "59.94 are written drop-frame. Every rush is reel AX, named by a `* "
        "FROM CLIP NAME:` comment; source timecodes are the media's own time "
        "from 00:00:00:00 (the document holds no camera timecode), the record "
        "side starts at `start_timecode`. A source at another cadence than the "
        "sequence, a sub-frame position, a held frame, a dissolve touching a "
        "speed ramp or leaving a retimed clip, a dip to colour between two "
        "clips are refused by name. What a list cannot carry is counted and "
        "named in `losses` (grades, framing, opacity, blurs, ramps written at "
        "their mean speed, motion clips, hidden multicam angles, audio, "
        "caption and hidden tracks, the sequence look, markers -- "
        "export_edl_markers writes those). Cuts, dissolves and an M2 speed "
        "were read back in DaVinci Resolve; drop-frame and camera timecodes "
        "were not tried.",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of a .edl file; each track's "
                                "list is written beside it, suffixed _V<n>."),
                   true)
            .Field("start_timecode",
                   StringSchema("Record timecode of the timeline's first "
                                "frame, HH:MM:SS:FF; 01:00:00:00 by default."),
                   false)
            .Build("export_edl arguments"),
        DispatchExportEdl);

    // PA1 -- docs/roadmap/PARITE-2026-11.md. The generic outputs
    // (Interchange.h): every track in one file, for an editor other than
    // Resolve.
    add("export_otio",
        "Write the selected timeline as an OpenTimelineIO file (.otio): every "
        "video and audio track in one file, the gaps between clips, constant "
        "speeds (a LinearTimeWarp effect; a held frame as FreezeFrame), cross "
        "dissolves and audio cross fades between two touching clips, the "
        "timeline's markers, and a disabled clip as a disabled clip. No "
        "position is refused: a time on a whole frame is written in frames, "
        "one inside a frame in its own rate and counted in "
        "`off_frame_times`. Hidden multicam angles and the clips of a hidden "
        "or muted track travel disabled. What the file does not carry is "
        "counted and named in `losses`: grades, framing, opacity, blurs, "
        "audio gains and fades, ramps (written at their mean speed), motion "
        "clips (their place left empty), caption tracks, the sequence look, "
        "and any transition a track cannot place (a dip to colour, an edge "
        "fade). For DaVinci Resolve, export_resolve_xml remains the measured "
        "route. Not verified in any editor yet (`verified_in_editor`).",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the .otio file to write."),
                   true)
            .Build("export_otio arguments"),
        DispatchExportOtio);

    add("export_fcpxml",
        "Write the selected timeline as a Final Cut Pro XML file (.fcpxml, "
        "version 1.10), which Final Cut and other editors import: one gap "
        "the length of the timeline with every clip connected to it, video "
        "tracks on lanes 1, 2... from the bottom up and audio tracks on "
        "lanes -1, -2..., so nothing ripples on import. Carries cuts on "
        "every track, constant speeds and held frames (a timeMap), disabled "
        "clips, and the timeline's markers. No position is refused: a time "
        "is an exact fraction of a second. No transition travels in this "
        "layout (`transitions_not_sent`); the rest of what stays behind is "
        "counted and named in `losses` as export_otio does. Not verified in "
        "any editor yet (`verified_in_editor`).",
        SchemaBuilder()
            .Field("path",
                   StringSchema("Absolute path of the .fcpxml file to write."),
                   true)
            .Build("export_fcpxml arguments"),
        DispatchExportFcpxml);

    add("export_edl_markers",
        "Write the selected timeline's markers as a CMX 3600 list, one event "
        "per marker: style 'loc' (default) adds a `* LOC: <tc> <COLOUR> "
        "<name>` comment with Avid's eight colour words, a span's length lost "
        "(counted in `durations_not_sent`); style 'resolve' adds the line "
        "Resolve's own marker list writes, ` |C:ResolveColorBlue |M:<name> "
        "|D:<frames>`, which keeps a span's length. A colour with no word of "
        "its own is written as the nearest and counted in "
        "`colors_approximated`. Not yet verified in DaVinci Resolve.",
        SchemaBuilder()
            .Field("path", StringSchema("Absolute path of the .edl to write."),
                   true)
            .Field("style",
                   EnumSchema({"loc", "resolve"},
                              "'loc' (default) or 'resolve': see above."),
                   false)
            .Field("start_timecode",
                   StringSchema("Record timecode of the timeline's first "
                                "frame; 01:00:00:00 by default."),
                   false)
            .Build("export_edl_markers arguments"),
        DispatchExportEdlMarkers);

    add("import_edl_markers",
        "Add the markers of a CMX 3600 list to the selected timeline: `* LOC: "
        "<tc> <COLOUR> <name>` comments and Resolve's ` "
        "|C:ResolveColor<Colour> |M:<name> |D:<frames>` lines, which carry a "
        "span (D:1 is a point). Colour words are Avid's eight and Resolve's "
        "sixteen, case aside; an unknown one is refused by its line, never "
        "guessed. Markers enter in category 'note'. The FCM line is honoured "
        "(drop-frame on 29.97 and 59.94 only). Not yet verified on a list "
        "Resolve wrote.",
        SchemaBuilder()
            .Field("path", StringSchema("Path of the .edl to read."), true)
            .Field("start_timecode",
                   StringSchema("Record timecode of the timeline's first "
                                "frame; 01:00:00:00 by default."),
                   false)
            .Build("import_edl_markers arguments"),
        DispatchImportEdlMarkers);
}
