// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// the frame the clips sit in: tracks, the sequence's format and colour
// management, bins, markers and transitions. Moved out of McpTools.cc as they
// were; what they share with the other files of the catalog is in
// McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

// PRD15 -- ROADMAP.md. The two types Document::Validate lets an edit write
// (TransitionRender.h). A document written before R5 may hold another, and
// is read as it was; no edit writes one, so the argument is a closed set,
// declared so that it takes any case (McpArguments.h).
std::string TransitionTypeSchema(const std::string& description) {
    return EnumSchema(
        {kTransitionCrossDissolve, kTransitionDipToColor, kTransitionCrossFade},
        description);
}

bool ReadAlignment(const Value& args, const std::string& key,
                   const std::string& path, TransitionAlignment& out,
                   std::string& message) {
    std::string text;
    if (!ReadString(args, key, path, false, "Center", text, message))
        return false;
    if (text == "Center") {
        out = TransitionAlignment::Center;
        return true;
    }
    if (text == "StartAtCut") {
        out = TransitionAlignment::StartAtCut;
        return true;
    }
    if (text == "EndAtCut") {
        out = TransitionAlignment::EndAtCut;
        return true;
    }
    message = "'" + path + "." + key +
              "' must be one of 'Center', 'StartAtCut', 'EndAtCut'";
    return false;
}

bool DispatchAddTrack(McpBackend& backend, const IdResolver&, const Value& args,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "kind", "index", "locked", "sync_lock", "visible", "muted", "solo"};
    if (!CheckKnownKeys(args, kAllowed, "add_track", message))
        return Fail(errorName, message, message);
    AddTrackOperation op;
    int64_t index = -1;
    if (!ReadString(args, "kind", "add_track", true, "", op.kind, message) ||
        !ReadInt64(args, "index", "add_track", false, -1, index, message) ||
        !ReadBool(args, "locked", "add_track", false, op.locked, message) ||
        !ReadBool(args, "sync_lock", "add_track", true, op.sync_lock,
                  message) ||
        !ReadBool(args, "visible", "add_track", true, op.visible, message) ||
        !ReadBool(args, "muted", "add_track", false, op.muted, message) ||
        !ReadBool(args, "solo", "add_track", false, op.solo, message))
        return Fail(errorName, message, message);
    if (index < std::numeric_limits<int32_t>::min() ||
        index > std::numeric_limits<int32_t>::max())
        return Fail(errorName, message, "'add_track.index' is out of range");
    op.index = static_cast<int32_t>(index);
    // Omitted: one above every track of the timeline, whatever their kind.
    if (args.Find("index") == nullptr) {
        Document document;
        if (!backend.SnapshotDocument(document, message)) {
            errorName = "IoError";
            return false;
        }
        op.index = 0;
        for (const DocumentTrack& track : document.sequence.tracks)
            op.index = std::max(op.index, track.index + 1);
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveTrack(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_track", message))
        return Fail(errorName, message, message);
    RemoveTrackOperation op;
    if (!ReadId(args, "track_id", "remove_track", resolver, true, op.track_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackLock(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "locked"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_lock", message))
        return Fail(errorName, message, message);
    SetTrackLockOperation op;
    if (!ReadId(args, "track_id", "set_track_lock", resolver, true, op.track_id,
                message) ||
        !ReadBool(args, "locked", "set_track_lock", false, op.locked, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackSyncLock(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "sync_lock"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_sync_lock", message))
        return Fail(errorName, message, message);
    SetTrackSyncLockOperation op;
    if (!ReadId(args, "track_id", "set_track_sync_lock", resolver, true,
                op.track_id, message) ||
        !ReadBool(args, "sync_lock", "set_track_sync_lock", true, op.sync_lock,
                  message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetTrackOutput(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"track_id", "visible",
                                                      "muted", "solo"};
    if (!CheckKnownKeys(args, kAllowed, "set_track_output", message))
        return Fail(errorName, message, message);
    SetTrackOutputOperation op;
    if (!ReadId(args, "track_id", "set_track_output", resolver, true,
                op.track_id, message) ||
        !ReadBool(args, "visible", "set_track_output", true, op.visible,
                  message) ||
        !ReadBool(args, "muted", "set_track_output", false, op.muted,
                  message) ||
        !ReadBool(args, "solo", "set_track_output", false, op.solo, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateSequence(McpBackend& backend, const IdResolver& resolver,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"name", "width", "height",
                                                      "frame_rate"};
    if (!CheckKnownKeys(args, kAllowed, "update_sequence", message))
        return Fail(errorName, message, message);
    // PRD40 -- ROADMAP.md. The fields given change; the others keep the
    // sequence's own value. All four were required, and an editor of the
    // 3 October series who only resized the frame was refused twice before
    // restating the name and the rate. A call that names none changes
    // nothing and is refused rather than journaled as a step.
    if (std::none_of(
            kAllowed.begin(), kAllowed.end(),
            [&](const std::string& key) { return args.Find(key) != nullptr; }))
        return Fail(errorName, message,
                    "'update_sequence' changes nothing: give one or more of "
                    "name, width, height and frame_rate");
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    UpdateSequenceOperation op;
    op.sequence_id = resolver.SequenceId();
    op.name = document.sequence.name;
    op.width = document.sequence.width;
    op.height = document.sequence.height;
    op.frame_rate = document.sequence.frame_rate;
    if (!ReadString(args, "name", "update_sequence", false, op.name, op.name,
                    message) ||
        !ReadInt32(args, "width", "update_sequence", false, op.width, op.width,
                   message) ||
        !ReadInt32(args, "height", "update_sequence", false, op.height,
                   op.height, message))
        return Fail(errorName, message, message);
    if (const Value* frameRate = args.Find("frame_rate")) {
        if (!ReadFractionValue(*frameRate, "update_sequence.frame_rate",
                               op.frame_rate.num, op.frame_rate.den, message))
            return Fail(errorName, message, message);
        if (op.frame_rate.den <= 0 || op.frame_rate.num <= 0)
            return Fail(errorName, message,
                        "'update_sequence.frame_rate' must be positive");
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetColorManagement(McpBackend& backend, const IdResolver& resolver,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    (void)resolver;
    static const std::vector<std::string> kAllowed = {
        "enabled",     "input_gamut",   "input_transfer", "input_ycbcr_matrix",
        "input_range", "working_gamut", "output_gamut",   "output_transfer"};
    if (!CheckKnownKeys(args, kAllowed, "set_color_management", message))
        return Fail(errorName, message, message);
    // The fields given change the project's setting; the others keep their
    // value. Every field used to be required, and `enabled` defaulted to
    // false: switching management on meant restating seven values, and
    // restating them without `enabled` switched it off.
    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    SetColorManagementOperation op;
    const ColorManagementSettings current = document.color_management;
    const std::string tool = "set_color_management";
    if (!ReadBool(args, "enabled", tool, current.enabled, op.settings.enabled,
                  message) ||
        !ReadString(args, "input_gamut", tool, false, current.input_gamut,
                    op.settings.input_gamut, message) ||
        !ReadString(args, "input_transfer", tool, false, current.input_transfer,
                    op.settings.input_transfer, message) ||
        !ReadString(args, "input_ycbcr_matrix", tool, false,
                    current.input_ycbcr_matrix, op.settings.input_ycbcr_matrix,
                    message) ||
        !ReadString(args, "input_range", tool, false, current.input_range,
                    op.settings.input_range, message) ||
        !ReadString(args, "working_gamut", tool, false, current.working_gamut,
                    op.settings.working_gamut, message) ||
        !ReadString(args, "output_gamut", tool, false, current.output_gamut,
                    op.settings.output_gamut, message) ||
        !ReadString(args, "output_transfer", tool, false,
                    current.output_transfer, op.settings.output_transfer,
                    message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddBin(McpBackend& backend, const IdResolver& resolver,
                    const Value& args, std::string& resultJson,
                    std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"name", "parent_id"};
    if (!CheckKnownKeys(args, kAllowed, "add_bin", message))
        return Fail(errorName, message, message);
    AddBinOperation op;
    if (!ReadString(args, "name", "add_bin", true, "", op.name, message) ||
        !ReadOptionalIdOrEmpty(args, "parent_id", "add_bin", resolver,
                               op.parent_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveBin(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_bin", message))
        return Fail(errorName, message, message);
    RemoveBinOperation op;
    if (!ReadId(args, "bin_id", "remove_bin", resolver, true, op.bin_id,
                message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRenameBin(McpBackend& backend, const IdResolver& resolver,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id", "name"};
    if (!CheckKnownKeys(args, kAllowed, "rename_bin", message))
        return Fail(errorName, message, message);
    RenameBinOperation op;
    if (!ReadId(args, "bin_id", "rename_bin", resolver, true, op.bin_id,
                message) ||
        !ReadString(args, "name", "rename_bin", true, "", op.name, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchMoveBin(McpBackend& backend, const IdResolver& resolver,
                     const Value& args, std::string& resultJson,
                     std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"bin_id", "parent_id"};
    if (!CheckKnownKeys(args, kAllowed, "move_bin", message))
        return Fail(errorName, message, message);
    MoveBinOperation op;
    if (!ReadId(args, "bin_id", "move_bin", resolver, true, op.bin_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "parent_id", "move_bin", resolver,
                               op.parent_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetMediaBin(McpBackend& backend, const IdResolver& resolver,
                         const Value& args, std::string& resultJson,
                         std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id", "bin_id"};
    if (!CheckKnownKeys(args, kAllowed, "set_media_bin", message))
        return Fail(errorName, message, message);
    SetMediaBinOperation op;
    if (!ReadId(args, "media_id", "set_media_bin", resolver, true, op.media_id,
                message) ||
        !ReadOptionalIdOrEmpty(args, "bin_id", "set_media_bin", resolver,
                               op.bin_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchSetMediaProxy(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"media_id", "proxy_path"};
    if (!CheckKnownKeys(args, kAllowed, "set_media_proxy", message))
        return Fail(errorName, message, message);
    SetMediaProxyOperation op;
    if (!ReadId(args, "media_id", "set_media_proxy", resolver, true,
                op.media_id, message) ||
        !ReadString(args, "proxy_path", "set_media_proxy", false, "",
                    op.proxy_path, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddMarker(McpBackend& backend, const IdResolver&,
                       const Value& args, std::string& resultJson,
                       std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "name", "time", "duration", "color", "category", "insertion_index"};
    if (!CheckKnownKeys(args, kAllowed, "add_marker", message))
        return Fail(errorName, message, message);
    AddMarkerOperation op;
    int64_t insertionIndex = -1;
    if (!ReadString(args, "name", "add_marker", true, "", op.marker.name,
                    message) ||
        !ReadTime(args, "time", "add_marker", true, op.marker.time, message) ||
        !ReadTime(args, "duration", "add_marker", false, op.marker.duration,
                  message) ||
        !ReadString(args, "color", "add_marker", false, op.marker.color,
                    op.marker.color, message) ||
        !ReadString(args, "category", "add_marker", false, op.marker.category,
                    op.marker.category, message) ||
        !ReadInt64(args, "insertion_index", "add_marker", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    // D4 -- the agent's question keeps its colour, and keeps it to itself.
    const Value* askedColor = args.Find("color");
    if (!ResolveMarkerColor(op.marker.category,
                            askedColor != nullptr ? op.marker.color : "",
                            op.marker.color, message)) {
        return Fail(errorName, message, message);
    }
    op.insertion_index = insertionIndex;
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveMarker(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"marker_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_marker", message))
        return Fail(errorName, message, message);
    RemoveMarkerOperation op;
    if (!ReadId(args, "marker_id", "remove_marker", resolver, true,
                op.marker_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateMarker(McpBackend& backend, const IdResolver& resolver,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "marker_id", "name", "time", "duration", "color", "category"};
    if (!CheckKnownKeys(args, kAllowed, "update_marker", message))
        return Fail(errorName, message, message);
    UpdateMarkerOperation op;
    if (!ReadId(args, "marker_id", "update_marker", resolver, true,
                op.marker_id, message) ||
        !ReadString(args, "name", "update_marker", true, "", op.name,
                    message) ||
        !ReadTime(args, "time", "update_marker", true, op.time, message) ||
        !ReadTime(args, "duration", "update_marker", false, op.duration,
                  message) ||
        !ReadString(args, "color", "update_marker", true, "", op.color,
                    message) ||
        !ReadString(args, "category", "update_marker", true, "", op.category,
                    message))
        return Fail(errorName, message, message);
    // D4 -- as add_marker.
    if (!ResolveMarkerColor(op.category, op.color, op.color, message)) {
        return Fail(errorName, message, message);
    }
    // D6 -- docs/roadmap/DIALOGUE-2026-10.md. The four fields this tool
    // always took are replaced; the span came later and is kept when left
    // out, so that renaming a passage the editor marked does not quietly
    // turn it back into a point.
    if (!args.Find("duration")) {
        Document document;
        std::string snapshotError;
        if (backend.SnapshotDocument(document, snapshotError)) {
            if (const DocumentMarker* marker =
                    document.FindMarker(op.marker_id))
                op.duration = marker->duration;
        }
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchAddTransition(McpBackend& backend, const IdResolver& resolver,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "track_id", "left_clip_id", "right_clip_id",   "type",
        "duration", "alignment",    "insertion_index", "color"};
    if (!CheckKnownKeys(args, kAllowed, "add_transition", message))
        return Fail(errorName, message, message);
    AddTransitionOperation op;
    int64_t insertionIndex = -1;
    // R5 -- docs/roadmap/REEL-2026-09.md. One side may be left out: a
    // "dip_to_color" needs only the clip it covers, which is how a track
    // opens on black. Document::Validate refuses a cross dissolve, or a
    // dip whose alignment does not match, so an omission by mistake is a
    // named refusal rather than an edge fade nobody asked for.
    if (!ReadId(args, "track_id", "add_transition", resolver, true,
                op.transition.track_id, message) ||
        !ReadId(args, "left_clip_id", "add_transition", resolver, false,
                op.transition.left_clip_id, message) ||
        !ReadId(args, "right_clip_id", "add_transition", resolver, false,
                op.transition.right_clip_id, message) ||
        !ReadString(args, "type", "add_transition", false, op.transition.type,
                    op.transition.type, message) ||
        !ReadString(args, "color", "add_transition", false, op.transition.color,
                    op.transition.color, message) ||
        !ReadTime(args, "duration", "add_transition", true,
                  op.transition.duration, message) ||
        !ReadAlignment(args, "alignment", "add_transition",
                       op.transition.alignment, message) ||
        !ReadInt64(args, "insertion_index", "add_transition", false, -1,
                   insertionIndex, message))
        return Fail(errorName, message, message);
    op.insertion_index = insertionIndex;
    // EF2 -- docs/roadmap/EFFETS-2026-10.md. Left out, the type is the
    // track's: a cross fade on a sound track, a dissolve on a picture one.
    if (!args.Find("type")) {
        Document document;
        std::string snapshotError;
        if (backend.SnapshotDocument(document, snapshotError)) {
            if (const DocumentTrack* track =
                    document.FindTrack(op.transition.track_id);
                track != nullptr && track->kind == "audio")
                op.transition.type = kTransitionCrossFade;
        }
    }
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchRemoveTransition(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"transition_id"};
    if (!CheckKnownKeys(args, kAllowed, "remove_transition", message))
        return Fail(errorName, message, message);
    RemoveTransitionOperation op;
    if (!ReadId(args, "transition_id", "remove_transition", resolver, true,
                op.transition_id, message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchUpdateTransition(McpBackend& backend, const IdResolver& resolver,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {
        "transition_id", "type", "duration", "alignment", "color"};
    if (!CheckKnownKeys(args, kAllowed, "update_transition", message))
        return Fail(errorName, message, message);
    UpdateTransitionOperation op;
    if (!ReadId(args, "transition_id", "update_transition", resolver, true,
                op.transition_id, message) ||
        !ReadString(args, "type", "update_transition", false, op.type, op.type,
                    message) ||
        !ReadString(args, "color", "update_transition", false, op.color,
                    op.color, message) ||
        !ReadTime(args, "duration", "update_transition", true, op.duration,
                  message) ||
        !ReadAlignment(args, "alignment", "update_transition", op.alignment,
                       message))
        return Fail(errorName, message, message);
    return backend.ApplyOperation(op, resultJson, errorName, message);
}

bool DispatchConformSequence(McpBackend& backend, const IdResolver&,
                             const Value& args, std::string& resultJson,
                             std::string& errorName, std::string& message) {
    static const std::vector<std::string> kAllowed = {"preview"};
    if (!CheckKnownKeys(args, kAllowed, "conform_sequence", message))
        return Fail(errorName, message, message);
    bool preview = false;
    if (!ReadBool(args, "preview", "conform_sequence", false, preview, message))
        return Fail(errorName, message, message);

    Document document;
    if (!backend.SnapshotDocument(document, message)) {
        errorName = "IoError";
        return false;
    }
    SequenceFormatProposal proposal;
    if (!ResolveSequenceFormat(document.library, proposal, message))
        return Fail(errorName, message, message);

    const DocumentSequence& current = document.sequence;
    const bool alreadyConformed =
        current.width == proposal.chosen.width &&
        current.height == proposal.chosen.height &&
        current.frame_rate.num == proposal.chosen.frame_rate.num &&
        current.frame_rate.den == proposal.chosen.frame_rate.den;

    // Applying a no-op would still push an entry onto the undo stack, so a
    // sequence that already matches is reported rather than rewritten.
    if (preview || alreadyConformed) {
        resultJson = SequenceFormatProposalJson(
            proposal, std::string(",\"applied\":false,\"already_conformed\":") +
                          (alreadyConformed ? "true" : "false"));
        return true;
    }

    UpdateSequenceOperation operation;
    operation.sequence_id = current.id;
    operation.name = current.name;
    operation.width = proposal.chosen.width;
    operation.height = proposal.chosen.height;
    operation.frame_rate = proposal.chosen.frame_rate;
    // MOTIF3 -- the format the rushes imply, chosen by the rule.
    Value ruleParams = Value::MakeObject();
    ruleParams.Set("width", Value::MakeInt(proposal.chosen.width));
    ruleParams.Set("height", Value::MakeInt(proposal.chosen.height));
    ruleParams.Set(
        "frame_rate",
        Value::MakeString(std::to_string(proposal.chosen.frame_rate.num) + "/" +
                          std::to_string(proposal.chosen.frame_rate.den)));
    RuleMotifScope ruleMotif(backend);
    if (!ruleMotif.Set(MotifReason::Format, "conform_sequence",
                       std::move(ruleParams), errorName, message))
        return false;
    std::string applied;
    if (!backend.ApplyOperation(operation, applied, errorName, message))
        return false;
    // The proposal, not the bare {"ok":true} the apply path returns: the
    // caller has to see which format was chosen and what it passed over.
    resultJson = SequenceFormatProposalJson(
        proposal, ",\"applied\":true,\"already_conformed\":false");
    return true;
}

}  // namespace

void RegisterSequenceTools(const McpToolAdder& add) {
    add("add_track",
        "Append a new, empty video, audio or caption track at an unused "
        "index.",
        SchemaBuilder()
            .Field("kind",
                   EnumSchema({"video", "audio", "caption"}, "Track kind."),
                   true)
            .Field("index",
                   IntSchema("Unique, non-negative track index; video, "
                             "audio and caption tracks share one numbering. "
                             "Omitted: one above every track."),
                   false)
            .Field("locked", BoolSchema("Start the track locked."), false)
            .Field("sync_lock",
                   BoolSchema("Whether ripple edits on other tracks affect "
                              "this one (default true)."),
                   false)
            .Field("visible", BoolSchema("Initial video visibility."), false)
            .Field("muted", BoolSchema("Initial audio mute state."), false)
            .Field("solo", BoolSchema("Initial audio solo state."), false)
            .Build("add_track arguments"),
        DispatchAddTrack);

    add("remove_track", "Remove a track and every clip on it.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to remove."), true)
            .Build("remove_track arguments"),
        DispatchRemoveTrack);

    add("set_track_lock", "Lock or unlock a track.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to lock/unlock."), true)
            .Field("locked", BoolSchema("Desired lock state."), false)
            .Build("set_track_lock arguments"),
        DispatchSetTrackLock);

    add("set_track_sync_lock",
        "Enable or disable whether ripple edits started on other tracks "
        "shift this one.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to change."), true)
            .Field("sync_lock", BoolSchema("Desired sync-lock state."), false)
            .Build("set_track_sync_lock arguments"),
        DispatchSetTrackSyncLock);

    add("set_track_output",
        "Set video visibility or audio mute/solo output state.",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track to change."), true)
            .Field("visible", BoolSchema("Video track visibility."), true)
            .Field("muted", BoolSchema("Audio track mute state."), true)
            .Field("solo", BoolSchema("Audio track solo state."), true)
            .Build("set_track_output arguments"),
        DispatchSetTrackOutput);

    // PRD40 -- every field optional: the ones left out keep their value.
    add("update_sequence",
        "Rename the sequence and/or change its frame size and rate. Only "
        "the fields given change: the others keep their current value.",
        SchemaBuilder()
            .Field("name", StringSchema("New sequence name."), false)
            .Field("width", IntSchema("Frame width in pixels."), false)
            .Field("height", IntSchema("Frame height in pixels."), false)
            .Field("frame_rate", kFractionSchemaText, false)
            .Build("update_sequence arguments"),
        DispatchUpdateSequence);

    add("set_color_management",
        "Configure the pinned OpenColorIO input, working, and output pipeline. "
        "It belongs to the project: every timeline shares one setting, "
        "whichever timeline_id is given, and changing it here changes it for "
        "all of them. Only the fields given change ({enabled: true} alone "
        "switches it on).",
        SchemaBuilder()
            .Field("enabled",
                   BoolSchema("Enable or disable project color management "
                              "(default: unchanged)."),
                   false)
            .Field("input_gamut",
                   EnumSchema({"rec709", "sony_sgamut3_cine", "sony_sgamut3",
                               "rec2020"},
                              "Source RGB primaries."),
                   false)
            .Field("input_transfer",
                   EnumSchema({"rec709", "sony_slog3", "linear", "hlg"},
                              "Source transfer function. hlg is BT.2100 HLG "
                              "(arib-std-b67)."),
                   false)
            .Field("input_ycbcr_matrix",
                   EnumSchema({"auto", "bt709", "bt2020_ncl"},
                              "YUV decoding matrix."),
                   false)
            .Field(
                "input_range",
                EnumSchema({"auto", "full", "limited"}, "Input signal range."),
                false)
            .Field("working_gamut",
                   EnumSchema({"acescct", "rec2020", "rec709"},
                              "Creative grading working space."),
                   false)
            .Field("output_gamut",
                   EnumSchema({"rec709", "rec2020"}, "Delivery RGB primaries."),
                   false)
            .Field("output_transfer",
                   EnumSchema({"rec709", "hlg"}, "Delivery transfer."), false)
            .Build("set_color_management arguments"),
        DispatchSetColorManagement);

    add("add_bin",
        "Create a new media bin, optionally nested under an existing one.",
        SchemaBuilder()
            .Field("name", StringSchema("Bin name."), true)
            .Field("parent_id",
                   IdSchema("Parent bin; omit or empty for the project root."),
                   false)
            .Build("add_bin arguments"),
        DispatchAddBin);

    add("remove_bin", "Remove an empty bin.",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to remove."), true)
            .Build("remove_bin arguments"),
        DispatchRemoveBin);

    add("rename_bin", "Rename an existing bin.",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to rename."), true)
            .Field("name", StringSchema("New bin name."), true)
            .Build("rename_bin arguments"),
        DispatchRenameBin);

    add("move_bin",
        "Move a bin under a different parent (or to the project root).",
        SchemaBuilder()
            .Field("bin_id", IdSchema("Bin to move."), true)
            .Field("parent_id",
                   IdSchema("New parent bin; omit or empty for the project "
                            "root."),
                   false)
            .Build("move_bin arguments"),
        DispatchMoveBin);

    add("set_media_bin",
        "File a library media item into a bin (or back to the project "
        "root).",
        SchemaBuilder()
            .Field("media_id", IdSchema("Library media item."), true)
            .Field("bin_id",
                   IdSchema("Destination bin; omit or empty for the project "
                            "root."),
                   false)
            .Build("set_media_bin arguments"),
        DispatchSetMediaBin);

    add("set_media_proxy",
        "Attach a proxy file to a library media item, or detach it with an "
        "empty or omitted proxy_path. The monitor decodes the proxy in place "
        "of the original when proxies are on; the export always reads the "
        "original. Journaled: undo detaches or restores the previous one.",
        SchemaBuilder()
            .Field("media_id", IdSchema("Library media item."), true)
            .Field("proxy_path",
                   StringSchema("Proxy file, relative to the project when it "
                                "can be; omit or empty to detach."),
                   false)
            .Build("set_media_proxy arguments"),
        DispatchSetMediaProxy);

    add("add_marker",
        "Add a sequence marker at an exact time. With `duration` it marks a "
        "span from `time`. category \"question\" asks the editor to look here: "
        "they answer in its name and remove it once resolved -- read markers "
        "back to know.",
        SchemaBuilder()
            .Field("name", StringSchema("Marker label."), true)
            .Field("time", kTimeSchemaText, true)
            .Field("duration", kTimeSchemaText, false)
            .Field("color", StringSchema("Marker color, e.g. '#f5c542'."),
                   false)
            .Field("category", StringSchema("Marker category."), false)
            .Field("insertion_index",
                   IntSchema("Position in the marker list; omit to append."),
                   false)
            .Build("add_marker arguments"),
        DispatchAddMarker);

    add("remove_marker", "Remove a sequence marker.",
        SchemaBuilder()
            .Field("marker_id", IdSchema("Marker to remove."), true)
            .Build("remove_marker arguments"),
        DispatchRemoveMarker);

    add("update_marker",
        "Replace a marker's name, time, color and category. Its `duration` "
        "(a span, zero for a point) is replaced when given and kept when "
        "left out.",
        SchemaBuilder()
            .Field("marker_id", IdSchema("Marker to update."), true)
            .Field("name", StringSchema("New marker label."), true)
            .Field("time", kTimeSchemaText, true)
            .Field("duration", kTimeSchemaText, false)
            .Field("color", StringSchema("New marker color."), true)
            .Field("category", StringSchema("New marker category."), true)
            .Build("update_marker arguments"),
        DispatchUpdateMarker);

    add("add_transition",
        "Add a transition on a track: a 'cross_dissolve' spanning the cut "
        "between two adjacent clips, or a 'dip_to_color' veiling a junction -- "
        "which may be the edge of the track, with one clip id omitted, for an "
        "opening or closing fade to a colour. On an audio track, a "
        "'cross_fade': both sounds read past the cut through their handles, "
        "one falling while the other rises on an equal power curve (Resolve's "
        "Cross Fade +3 dB).",
        SchemaBuilder()
            .Field("track_id", IdSchema("Track the transition belongs to."),
                   true)
            .Field("left_clip_id",
                   IdSchema("Outgoing clip. Omit, with alignment "
                            "'StartAtCut', for a 'dip_to_color' that opens "
                            "the track."),
                   false)
            .Field("right_clip_id",
                   IdSchema("Incoming clip. Omit, with alignment "
                            "'EndAtCut', for a 'dip_to_color' that closes "
                            "the track."),
                   false)
            .Field("type",
                   TransitionTypeSchema("cross_fade by default on an audio "
                                        "track, cross_dissolve otherwise."),
                   false)
            .Field("color",
                   StringSchema("Colour a 'dip_to_color' goes through, "
                                "#rrggbb; #000000 by default."),
                   false)
            .Field("duration", kTimeSchemaText, true)
            .Field("alignment",
                   EnumSchema({"Center", "StartAtCut", "EndAtCut"},
                              "How the transition straddles the cut."),
                   false)
            .Field(
                "insertion_index",
                IntSchema("Position in the transition list; omit to append."),
                false)
            .Build("add_transition arguments"),
        DispatchAddTransition);

    add("remove_transition", "Remove a transition.",
        SchemaBuilder()
            .Field("transition_id", IdSchema("Transition to remove."), true)
            .Build("remove_transition arguments"),
        DispatchRemoveTransition);

    add("update_transition",
        "Replace a transition's type, duration, alignment and colour.",
        SchemaBuilder()
            .Field("transition_id", IdSchema("Transition to update."), true)
            .Field("type", TransitionTypeSchema("The new type."), false)
            .Field("color",
                   StringSchema("Colour a 'dip_to_color' goes through, "
                                "#rrggbb."),
                   false)
            .Field("duration", kTimeSchemaText, true)
            .Field("alignment",
                   EnumSchema({"Center", "StartAtCut", "EndAtCut"}, ""), false)
            .Build("update_transition arguments"),
        DispatchUpdateTransition);

    add("conform_sequence",
        "Set the sequence's resolution and frame rate to the format the "
        "project's own rushes are in, and report what it chose. Prefer it to "
        "update_sequence to match the footage: it reads each rush's rotation "
        "(a vertical shoot is stored landscape). When the rushes disagree, the "
        "most common format wins and the others are reported in \"candidates\" "
        "for you to raise with the user.",
        SchemaBuilder()
            .Field("preview",
                   BoolSchema("Report the format without applying it; "
                              "default false."),
                   false)
            .Build("conform_sequence arguments"),
        DispatchConformSequence);
}
