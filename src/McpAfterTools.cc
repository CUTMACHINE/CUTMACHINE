// DECOUPAGE-2026-10 DC2 -- docs/roadmap/DECOUPAGE-2026-10.md. The MCP tools of
// the After Effects bridge. Moved out of McpTools.cc as they were; what they
// share with the other files of the catalog is in McpToolsInternal.h.

#include "McpToolsInternal.h"

using namespace mcp_tools;

namespace {

std::string AfterPropertyValueSchema(
    const std::string& description =
        "After Effects property value: a scalar or flat scalar array.") {
    const std::string scalar =
        "{\"oneOf\":[{\"type\":\"number\"},{\"type\":\"string\"},"
        "{\"type\":\"boolean\"}]}";
    return "{\"oneOf\":[" + scalar +
           ",{\"type\":\"array\",\"items\":" + scalar +
           "}],\"description\":\"" + Esc(description) + "\"}";
}

bool DispatchAfterRequest(McpBackend& backend, const Value& args,
                          AfterBridgeMethod method, const std::string& toolName,
                          const std::vector<std::string>& allowed,
                          std::string& resultJson, std::string& errorName,
                          std::string& message) {
    if (!CheckKnownKeys(args, allowed, toolName, message))
        return Fail(errorName, message, message);

    AfterBridgeRequest request;
    request.method = method;
    request.arguments = args;
    return backend.RunAfterRequest(SerializeAfterBridgeRequest(request),
                                   resultJson, errorName, message);
}

bool DispatchAfterCreateProject(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::CreateProject,
                                "after_create_project", {"project_path"},
                                resultJson, errorName, message);
}

bool DispatchAfterOpenProjectCopy(McpBackend& backend, const IdResolver&,
                                  const Value& args, std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    return DispatchAfterRequest(backend, args,
                                AfterBridgeMethod::OpenProjectCopy,
                                "after_open_project_copy",
                                {"source_project_path", "working_project_path"},
                                resultJson, errorName, message);
}

bool DispatchAfterDescribeProject(McpBackend& backend, const IdResolver&,
                                  const Value& args, std::string& resultJson,
                                  std::string& errorName,
                                  std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::DescribeProject,
        "after_describe_project", {}, resultJson, errorName, message);
}

bool DispatchAfterListComps(McpBackend& backend, const IdResolver&,
                            const Value& args, std::string& resultJson,
                            std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::ListComps,
                                "after_list_comps", {}, resultJson, errorName,
                                message);
}

bool DispatchAfterListFootage(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::ListFootage,
                                "after_list_footage", {}, resultJson, errorName,
                                message);
}

bool DispatchAfterDescribeComp(McpBackend& backend, const IdResolver&,
                               const Value& args, std::string& resultJson,
                               std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::DescribeComp,
                                "after_describe_comp", {"comp_id", "layer_id"},
                                resultJson, errorName, message);
}

bool DispatchAfterRenderFrame(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::RenderFrame, "after_render_frame",
        {"comp_id", "frame", "image_path"}, resultJson, errorName, message);
}

bool DispatchAfterSetText(McpBackend& backend, const IdResolver&,
                          const Value& args, std::string& resultJson,
                          std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::SetText, "after_set_text",
        {"comp_id", "layer_id", "text", "expected", "save"}, resultJson,
        errorName, message);
}

bool DispatchAfterSetProperty(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::SetProperty,
                                "after_set_property",
                                {"comp_id", "layer_id", "property_path",
                                 "value", "frame", "expected", "save"},
                                resultJson, errorName, message);
}

bool DispatchAfterSetLayerState(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::SetLayerState,
        "after_set_layer_state",
        {"comp_id", "layer_id", "enabled", "locked", "start_frame", "in_frame",
         "out_frame", "expected", "save"},
        resultJson, errorName, message);
}

bool DispatchAfterReplaceFootage(McpBackend& backend, const IdResolver&,
                                 const Value& args, std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::ReplaceFootage,
        "after_replace_footage", {"footage_id", "media_path", "save"},
        resultJson, errorName, message);
}

bool DispatchAfterImportFootage(McpBackend& backend, const IdResolver&,
                                const Value& args, std::string& resultJson,
                                std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::ImportFootage, "after_import_footage",
        {"media_path", "folder_name", "save"}, resultJson, errorName, message);
}

bool DispatchAfterAddLayer(McpBackend& backend, const IdResolver&,
                           const Value& args, std::string& resultJson,
                           std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::AddLayer,
                                "after_add_layer",
                                {"comp_id", "item_id", "index", "start_frame",
                                 "in_frame", "out_frame", "save"},
                                resultJson, errorName, message);
}

bool DispatchAfterDuplicateLayer(McpBackend& backend, const IdResolver&,
                                 const Value& args, std::string& resultJson,
                                 std::string& errorName, std::string& message) {
    return DispatchAfterRequest(
        backend, args, AfterBridgeMethod::DuplicateLayer,
        "after_duplicate_layer", {"comp_id", "layer_id", "save"}, resultJson,
        errorName, message);
}

bool DispatchAfterSaveProject(McpBackend& backend, const IdResolver&,
                              const Value& args, std::string& resultJson,
                              std::string& errorName, std::string& message) {
    return DispatchAfterRequest(backend, args, AfterBridgeMethod::SaveProject,
                                "after_save_project", {"project_path"},
                                resultJson, errorName, message);
}

}  // namespace

void RegisterAfterTools(const McpToolAdder& add) {
    add("after_create_project",
        "Create and save an empty derived After Effects project.",
        SchemaBuilder()
            .Field("project_path",
                   StringSchema("Absolute path of the new .aep project."), true)
            .Build("after_create_project arguments"),
        DispatchAfterCreateProject);

    add("after_open_project_copy",
        "Copy an existing .aep to a new path and open only that working copy.",
        SchemaBuilder()
            .Field("source_project_path",
                   StringSchema("Absolute path of the source .aep."), true)
            .Field("working_project_path",
                   StringSchema("Absolute path of the new working-copy .aep."),
                   true)
            .Build("after_open_project_copy arguments"),
        DispatchAfterOpenProjectCopy);

    add("after_describe_project",
        "Describe the active After Effects project without importing or "
        "changing any state in CUTMACHINE.",
        SchemaBuilder().Build("after_describe_project takes no arguments"),
        DispatchAfterDescribeProject);

    add("after_list_comps",
        "List every composition in the active After Effects project with its "
        "host ID, dimensions, frame rate, duration and layer count.",
        SchemaBuilder().Build("after_list_comps takes no arguments"),
        DispatchAfterListComps);

    add("after_list_footage",
        "List every file-backed footage item with its host ID, absolute path "
        "and whether the file is offline. After Effects renders missing media "
        "as colour bars instead of failing, so check this before trusting a "
        "rendered frame; repair an offline item with after_replace_footage.",
        SchemaBuilder().Build("after_list_footage takes no arguments"),
        DispatchAfterListFootage);

    add("after_describe_comp",
        "Describe one After Effects composition. Without layer_id it returns "
        "compact layer summaries; with layer_id it also returns that layer's "
        "transforms, effects, values and keyframes.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("layer_id",
                   StringSchema("Optional exact host ID for property details."),
                   false)
            .Build("after_describe_comp arguments"),
        DispatchAfterDescribeComp);

    add("after_render_frame",
        "Render one composition frame to an absolute .png path and return its "
        "dimensions: the way to see an edit instead of inferring it from "
        "values. The render queue is untouched.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("frame", IntSchema("Exact composition frame to render."),
                   true)
            .Field("image_path",
                   StringSchema("Absolute .png path that must not exist yet."),
                   true)
            .Build("after_render_frame arguments"),
        DispatchAfterRenderFrame);

    add("after_set_text",
        "Replace a text layer's source text while preserving its existing "
        "TextDocument styling. Returns before/after text and creates one "
        "After Effects undo group.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("layer_id", StringSchema("Exact host ID of the text layer."),
                   true)
            .Field("text", StringSchema("Replacement text; may be empty."),
                   true)
            .Field("expected",
                   StringSchema("The text last read (after_describe_comp): "
                                "if After now holds another, nothing is "
                                "written and StaleValue gives the text "
                                "found, so a hand edit is never "
                                "overwritten."),
                   false)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_set_text arguments"),
        DispatchAfterSetText);

    add("after_set_property",
        "Set one transform or effect property by its inspected matchName/name "
        "path. Returns before/after values and creates one undo group.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("layer_id", StringSchema("Exact host ID of the layer."),
                   true)
            .Field("property_path",
                   ArraySchema(StringSchema("One matchName or display-name "
                                            "segment."),
                               "Path returned by after_describe_comp."),
                   true)
            .Field("value", AfterPropertyValueSchema(), true)
            .Field("frame",
                   IntSchema("Exact composition frame for a keyframe."), false)
            .Field("expected",
                   AfterPropertyValueSchema(
                       "The value last read, at that frame if given: if "
                       "After now holds another, nothing is written and "
                       "StaleValue gives the value found. Numbers compare to "
                       "a millionth of their size."),
                   false)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_set_property arguments"),
        DispatchAfterSetProperty);

    add("after_set_layer_state",
        "Change a layer's enabled/locked state or start, in and out frames. "
        "The result contains the full before/after state and forms one undo "
        "group.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("layer_id", StringSchema("Exact host ID of the layer."),
                   true)
            .Field("enabled", BoolSchema("Layer video visibility."), false)
            .Field("locked", BoolSchema("Layer lock state."), false)
            .Field("start_frame", IntSchema("Exact layer start frame."), false)
            .Field("in_frame", IntSchema("Exact layer in frame."), false)
            .Field("out_frame", IntSchema("Exact layer out frame."), false)
            .Field(
                "expected",
                SchemaBuilder()
                    .Field("enabled", BoolSchema("Visibility read."), false)
                    .Field("locked", BoolSchema("Lock state read."), false)
                    .Field("start_frame", IntSchema("Start frame read."), false)
                    .Field("in_frame", IntSchema("In frame read."), false)
                    .Field("out_frame", IntSchema("Out frame read."), false)
                    .Build("The states last read: if one differs in After "
                           "now, nothing is written and StaleValue names "
                           "it."),
                false)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_set_layer_state arguments"),
        DispatchAfterSetLayerState);

    add("after_replace_footage",
        "Replace one file-backed footage item by host ID. Returns the old and "
        "new absolute paths and creates one After Effects undo group.",
        SchemaBuilder()
            .Field("footage_id", StringSchema("Exact host ID of the footage."),
                   true)
            .Field("media_path",
                   StringSchema("Absolute path of the replacement media."),
                   true)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_replace_footage arguments"),
        DispatchAfterReplaceFootage);

    add("after_import_footage",
        "Import one media file into the active project as a single footage "
        "item, optionally into a top-level folder created on demand. Returns "
        "the new host ID; use after_add_layer to place it in a composition.",
        SchemaBuilder()
            .Field("media_path",
                   StringSchema("Absolute path of an existing media file."),
                   true)
            .Field("folder_name",
                   StringSchema("Optional top-level project folder; created "
                                "when missing."),
                   false)
            .Field("save",
                   BoolSchema("Save the .aep after importing; true by "
                              "default."),
                   false)
            .Build("after_import_footage arguments"),
        DispatchAfterImportFootage);

    add("after_add_layer",
        "Place an existing footage item or composition into a composition as "
        "a new layer, optionally at an exact stacking index and with exact "
        "start, in and out frames. Returns the new layer's host ID and "
        "resolved timing.",
        SchemaBuilder()
            .Field("comp_id",
                   StringSchema("Exact host ID of the destination "
                                "composition."),
                   true)
            .Field("item_id",
                   StringSchema("Exact host ID of the footage or composition "
                                "to place."),
                   true)
            .Field("index",
                   IntSchema("Optional 1-based stacking index; 1 is the top "
                             "layer."),
                   false)
            .Field("start_frame", IntSchema("Exact layer start frame."), false)
            .Field("in_frame", IntSchema("Exact layer in frame."), false)
            .Field("out_frame", IntSchema("Exact layer out frame."), false)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_add_layer arguments"),
        DispatchAfterAddLayer);

    add("after_duplicate_layer",
        "Duplicate one layer with its keyframes, effects and styling intact, "
        "which is how a new line or card inherits an existing animation. "
        "Returns the copy's host ID and stacking index.",
        SchemaBuilder()
            .Field("comp_id", StringSchema("Exact host ID of the composition."),
                   true)
            .Field("layer_id",
                   StringSchema("Exact host ID of the layer to copy."), true)
            .Field("save",
                   BoolSchema("Save the .aep after editing; true by "
                              "default."),
                   false)
            .Build("after_duplicate_layer arguments"),
        DispatchAfterDuplicateLayer);

    add("after_save_project",
        "Save the active derived After Effects project. An optional absolute "
        ".aep path is accepted only for a project that has not yet been saved.",
        SchemaBuilder()
            .Field("project_path",
                   StringSchema("Optional absolute .aep path for the first "
                                "save."),
                   false)
            .Build("after_save_project arguments"),
        DispatchAfterSaveProject);
}
