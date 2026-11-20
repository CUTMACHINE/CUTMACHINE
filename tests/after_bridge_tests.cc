#include "AfterBridge.h"

#include "Document.h"
#include "McpBackend.h"
#include "McpTools.h"
#include "Operations.h"
#include "Ulid.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "TestSupport.h"

namespace {

std::string Fixture(const std::string& name) {
    std::ifstream input(std::filesystem::path(AFTER_BRIDGE_FIXTURES_DIR) / name,
                        std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

const mcp_json::Value* Argument(const AfterBridgeRequest& request,
                                const std::string& name) {
    return request.arguments.Find(name);
}

class AfterBackend final : public McpBackend {
public:
    bool SnapshotDocument(Document& document, std::string&) override {
        document = Document{};
        return true;
    }

    bool ApplyOperation(Operation, std::string&, std::string& errorName,
                        std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used by After bridge tests";
        return false;
    }

    bool RunAfterRequest(const std::string& requestJson,
                         std::string& resultJson, std::string& errorName,
                         std::string& message) override {
        if (!ParseAfterBridgeRequest(requestJson, last_request, message)) {
            errorName = "ValidationFailed";
            return false;
        }
        ++calls;
        AfterBridgeResponse response;
        response.ok = true;
        response.project_name = "test";
        response.path = "/tmp/test.aep";
        response.item_count = 0;
        response.after_version = "26.0";
        response.result = mcp_json::Value::MakeObject();
        resultJson = SerializeAfterBridgeResponse(response);
        return true;
    }

    bool Undo(std::string&, std::string& errorName,
              std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used by After bridge tests";
        return false;
    }

    bool Redo(std::string&, std::string& errorName,
              std::string& message) override {
        errorName = "UnsupportedOperation";
        message = "not used by After bridge tests";
        return false;
    }

    bool Describe(std::string& json, std::string&) override {
        json = "{}";
        return true;
    }

    int calls = 0;
    AfterBridgeRequest last_request;
};

bool HasTool(const McpToolRegistry& registry, const std::string& name,
             std::string* schema = nullptr) {
    for (const McpTool& tool : registry.Tools()) {
        if (tool.name != name) continue;
        if (schema != nullptr) *schema = tool.input_schema_json;
        return true;
    }
    return false;
}

mcp_json::Value StringArray(std::initializer_list<std::string> values) {
    mcp_json::Value result = mcp_json::Value::MakeArray();
    for (const std::string& value : values)
        result.Push(mcp_json::Value::MakeString(value));
    return result;
}

}  // namespace

int main() {
    std::string error;
    AfterBridgeRequest request;
    Check(ParseAfterBridgeRequest(Fixture("create-project-request.json"),
                                  request, error),
          "la fixture after-request.v2 est acceptée : " + error);
    const mcp_json::Value* createPath = Argument(request, "project_path");
    Check(request.method == AfterBridgeMethod::CreateProject && createPath &&
              createPath->AsString() == "/tmp/test.aep",
          "la méthode et le chemin de création sont conservés");

    AfterBridgeRequest reparsed;
    const std::string canonicalRequest = SerializeAfterBridgeRequest(request);
    Check(ParseAfterBridgeRequest(canonicalRequest, reparsed, error) &&
              reparsed.method == request.method &&
              reparsed.arguments.Dump() == request.arguments.Dump(),
          "une requête fait un round-trip canonique : " + error);
    Check(!ParseAfterBridgeRequest(Fixture("invalid-request.json"), reparsed,
                                   error) &&
              error.find("schema") != std::string::npos,
          "une version de requête inconnue est refusée explicitement");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"describe_project","arguments":{"project_path":"/tmp/x.aep"}})",
            reparsed, error) &&
            error.find("project_path") != std::string::npos,
        "describe_project refuse un argument hors contrat");
    Check(
        ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_text","arguments":{"comp_id":"1","layer_id":"2","text":""}})",
            reparsed, error),
        "set_text permet d'effacer complètement le texte : " + error);
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_layer_state","arguments":{"comp_id":"1","layer_id":"2"}})",
            reparsed, error) &&
            error.find("at least one") != std::string::npos,
        "set_layer_state exige un changement explicite");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_property","arguments":{"comp_id":"1","layer_id":"2","property_path":["ADBE Transform Group"],"value":[[1,2]]}})",
            reparsed, error) &&
            error.find("flat array") != std::string::npos,
        "set_property refuse les valeurs imbriquées non prises en charge");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"render_frame","arguments":{"comp_id":"1","frame":12.5,"image_path":"/tmp/a.png"}})",
            reparsed, error) &&
            error.find("frame") != std::string::npos,
        "render_frame refuse une frame non entière");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"add_layer","arguments":{"comp_id":"1"}})",
            reparsed, error) &&
            error.find("item_id") != std::string::npos,
        "add_layer exige l'élément source");
    Check(
        ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"add_layer","arguments":{"comp_id":"1","item_id":"7","index":2,"out_frame":48}})",
            reparsed, error),
        "add_layer accepte un index et un out exacts : " + error);
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"duplicate_layer","arguments":{"comp_id":"1","layer_id":"2","index":3}})",
            reparsed, error) &&
            error.find("index") != std::string::npos,
        "duplicate_layer refuse un argument hors contrat");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"list_footage","arguments":{"comp_id":"1"}})",
            reparsed, error) &&
            error.find("comp_id") != std::string::npos,
        "list_footage ne prend aucun argument");

    // Pont After -- une écriture conditionnelle : la valeur lue en dernier.
    Check(
        ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_text","arguments":{"comp_id":"1","layer_id":"2","text":"B","expected":""}})",
            reparsed, error),
        "set_text accepte le texte attendu, même vide : " + error);
    Check(
        ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_property","arguments":{"comp_id":"1","layer_id":"2","property_path":["ADBE Transform Group","ADBE Position"],"value":[10,20],"frame":4,"expected":[960,540]}})",
            reparsed, error),
        "set_property accepte la valeur attendue à une frame : " + error);
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_property","arguments":{"comp_id":"1","layer_id":"2","property_path":["ADBE Opacity"],"value":50,"expected":[[1]]}})",
            reparsed, error) &&
            error.find("expected") != std::string::npos,
        "set_property refuse une valeur attendue imbriquée");
    Check(
        ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_layer_state","arguments":{"comp_id":"1","layer_id":"2","enabled":false,"expected":{"enabled":true,"in_frame":0}}})",
            reparsed, error),
        "set_layer_state accepte les états attendus : " + error);
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_layer_state","arguments":{"comp_id":"1","layer_id":"2","enabled":false,"expected":{"opacity":100}}})",
            reparsed, error) &&
            error.find("opacity") != std::string::npos,
        "set_layer_state refuse un état attendu inconnu");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_layer_state","arguments":{"comp_id":"1","layer_id":"2","enabled":false,"expected":{"in_frame":1.5}}})",
            reparsed, error) &&
            error.find("in_frame") != std::string::npos,
        "set_layer_state refuse une frame attendue non entière");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"set_layer_state","arguments":{"comp_id":"1","layer_id":"2","enabled":false,"expected":{}}})",
            reparsed, error) &&
            error.find("expected") != std::string::npos,
        "set_layer_state refuse un objet d'états attendus vide");
    Check(
        !ParseAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"duplicate_layer","arguments":{"comp_id":"1","layer_id":"2","expected":"x"}})",
            reparsed, error) &&
            error.find("expected") != std::string::npos,
        "une méthode sans écriture conditionnelle refuse expected");

    AfterBridgeResponse response;
    Check(ParseAfterBridgeResponse(Fixture("describe-project-response.json"),
                                   response, error),
          "la fixture after-response.v2 est acceptée : " + error);
    Check(response.ok && response.project_name == "test" &&
              response.path == "/tmp/test.aep" && response.item_count == 0 &&
              response.after_version == "26.0" &&
              response.result.Find("active_item_id") != nullptr,
          "la réponse conserve le résultat et les diagnostics After");
    AfterBridgeResponse responseRoundTrip;
    Check(ParseAfterBridgeResponse(SerializeAfterBridgeResponse(response),
                                   responseRoundTrip, error) &&
              responseRoundTrip.result.Dump() == response.result.Dump(),
          "une réponse fait un round-trip canonique : " + error);
    Check(ParseAfterBridgeResponse(Fixture("error-response.json"), response,
                                   error) &&
              !response.ok && response.error == "ProjectAlreadyOpen",
          "une erreur JSX structurée reste lisible : " + error);
    Check(
        !ParseAfterBridgeResponse(
            R"({"schema":"after-response.v2","ok":true,"project_name":"x","path":"","extra":1})",
            response, error) &&
            error.find("extra") != std::string::npos,
        "les clés de réponse inconnues sont refusées");
    Check(!ParseAfterBridgeResponse(
              R"({"schema":"after-response.v2","ok":true,"project_name":"x"})",
              response, error) &&
              error.find("path") != std::string::npos,
          "le chemin, même vide, est requis dans une réponse de succès");

    const std::filesystem::path target =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-after-bridge.aep");
    AfterBridgeInvocation captured;
    const AfterBridgeRunner runner =
        [&](const AfterBridgeInvocation& invocation, std::string& responseJson,
            std::string&, std::string&) {
            captured = invocation;
            AfterBridgeResponse success;
            success.ok = true;
            success.project_name = "bridge";
            success.path = target.string();
            success.result = mcp_json::Value::MakeObject();
            responseJson = SerializeAfterBridgeResponse(success);
            return true;
        };
    AfterBridgeRequest create;
    create.method = AfterBridgeMethod::CreateProject;
    create.arguments.Set("project_path",
                         mcp_json::Value::MakeString(target.string()));
    std::string resultJson;
    std::string errorName;
    std::string message;
    Check(RunAfterBridgeRequest(SerializeAfterBridgeRequest(create), resultJson,
                                errorName, message, runner),
          "le runner simulé exécute create_project : " + errorName + " " +
              message);
    Check(captured.script_name == "create_project.jsx" &&
              !captured.response_path.empty() &&
              captured.job_script.find("CUTMACHINE_AFTER_RESPONSE_PATH") !=
                  std::string::npos &&
              captured.job_script.find(target.string()) == std::string::npos,
          "le job choisit le handler et sépare les données de son source");
    // MA11 -- After keeps a script's globals for its session: the job
    // clears its request once run, even if the handler threw, so that
    // motion_import.jsx run by hand afterwards builds instead of taking
    // itself for the bridge.
    const size_t executed =
        captured.job_script.find("CUTMACHINE_AFTER.execute(");
    const size_t cleared = captured.job_script.find(
        "finally {\n    CUTMACHINE_AFTER_REQUEST = undefined;");
    Check(executed != std::string::npos && cleared != std::string::npos &&
              cleared > executed,
          "le job efface sa requête après l'avoir exécutée");

    const AfterBridgeRunner refusal = [](const AfterBridgeInvocation&,
                                         std::string& responseJson,
                                         std::string&, std::string&) {
        responseJson = Fixture("error-response.json");
        return true;
    };
    Check(
        !RunAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"describe_project","arguments":{}})",
            resultJson, errorName, message, refusal) &&
            errorName == "ProjectAlreadyOpen",
        "un refus After devient un échec MCP nommé");
    Check(
        !RunAfterBridgeRequest(
            R"({"schema":"after-request.v2","method":"create_project","arguments":{"project_path":"relative.aep"}})",
            resultJson, errorName, message, runner) &&
            errorName == "ValidationFailed",
        "le runner ne reçoit jamais un chemin relatif");

    // Seeing a frame must never cost a frame: the bridge refuses to write over
    // an existing still, which is what keeps a numbered series comparable.
    const std::filesystem::path still = std::filesystem::temp_directory_path() /
                                        (GenerateUlid() + "-after-frame.png");
    AfterBridgeRequest render;
    render.method = AfterBridgeMethod::RenderFrame;
    render.arguments.Set("comp_id", mcp_json::Value::MakeString("42"));
    render.arguments.Set("frame", mcp_json::Value::MakeInt(0));
    render.arguments.Set("image_path",
                         mcp_json::Value::MakeString(still.string()));
    Check(RunAfterBridgeRequest(SerializeAfterBridgeRequest(render), resultJson,
                                errorName, message, runner) &&
              captured.script_name == "render_frame.jsx",
          "render_frame choisit son handler : " + errorName + " " + message);
    {
        std::ofstream output(still, std::ios::binary);
        output << "existing still";
    }
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(render),
                                 resultJson, errorName, message, runner) &&
              errorName == "IoError",
          "render_frame refuse d'écraser une image existante");
    std::error_code stillError;
    std::filesystem::remove(still, stillError);

    AfterBridgeRequest jpeg = render;
    jpeg.arguments.Set(
        "image_path",
        mcp_json::Value::MakeString(
            (std::filesystem::temp_directory_path() / "frame.jpg").string()));
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(jpeg), resultJson,
                                 errorName, message, runner) &&
              errorName == "ValidationFailed" &&
              message.find(".png") != std::string::npos,
          "render_frame n'accepte que le PNG qu'After sait écrire");

    AfterBridgeRequest import;
    import.method = AfterBridgeMethod::ImportFootage;
    import.arguments.Set(
        "media_path",
        mcp_json::Value::MakeString(
            (std::filesystem::temp_directory_path() / (GenerateUlid() + ".png"))
                .string()));
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(import),
                                 resultJson, errorName, message, runner) &&
              errorName == "IoError",
          "import_footage refuse un média absent avant d'atteindre After");

    const std::filesystem::path source =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-after-source.aep");
    const std::filesystem::path working =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-after-working.aep");
    {
        std::ofstream output(source, std::ios::binary);
        output << "safe source fixture";
    }
    AfterBridgeRequest open;
    open.method = AfterBridgeMethod::OpenProjectCopy;
    open.arguments.Set("source_project_path",
                       mcp_json::Value::MakeString(source.string()));
    open.arguments.Set("working_project_path",
                       mcp_json::Value::MakeString(working.string()));
    Check(RunAfterBridgeRequest(SerializeAfterBridgeRequest(open), resultJson,
                                errorName, message, runner),
          "open_project_copy prépare une copie de travail : " + errorName +
              " " + message);
    std::ifstream copied(working, std::ios::binary);
    std::ostringstream copiedBytes;
    copiedBytes << copied.rdbuf();
    Check(captured.script_name == "open_project_copy.jsx" &&
              copiedBytes.str() == "safe source fixture",
          "la copie ouverte est identique et la source reste séparée");
    std::error_code cleanupError;
    std::filesystem::remove(source, cleanupError);
    std::filesystem::remove(working, cleanupError);

    // MA1 -- a motion composition built by the script that also runs by
    // hand, from a description that must exist, into a project that must
    // not.
    const std::filesystem::path description =
        std::filesystem::temp_directory_path() /
        (GenerateUlid() + "-motion.json");
    const std::filesystem::path built = std::filesystem::temp_directory_path() /
                                        (GenerateUlid() + "-motion.aep");
    AfterBridgeRequest motion;
    motion.method = AfterBridgeMethod::BuildMotionProject;
    motion.arguments.Set("description_path",
                         mcp_json::Value::MakeString(description.string()));
    motion.arguments.Set("project_path",
                         mcp_json::Value::MakeString(built.string()));
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(motion),
                                 resultJson, errorName, message, runner) &&
              errorName == "IoError",
          "build_motion_project refuse une description absente avant After");
    {
        std::ofstream output(description, std::ios::binary);
        output << R"({"schema":"after-motion.v1"})";
    }
    Check(
        RunAfterBridgeRequest(SerializeAfterBridgeRequest(motion), resultJson,
                              errorName, message, runner) &&
            captured.script_name == "motion_import.jsx" &&
            captured.job_script.find("CUTMACHINE_AFTER_HANDLER") !=
                std::string::npos &&
            captured.job_script.find(description.string()) == std::string::npos,
        "build_motion_project passe par motion_import.jsx, chemins encodés : " +
            errorName + " " + message);
    {
        std::ofstream output(built, std::ios::binary);
        output << "existing project";
    }
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(motion),
                                 resultJson, errorName, message, runner) &&
              errorName == "ProjectExists",
          "build_motion_project n'écrase jamais un projet");
    motion.arguments.Set("extra", mcp_json::Value::MakeBool(true));
    Check(!RunAfterBridgeRequest(SerializeAfterBridgeRequest(motion),
                                 resultJson, errorName, message, runner) &&
              errorName == "ValidationFailed",
          "build_motion_project refuse un argument inconnu");
    std::filesystem::remove(description, cleanupError);
    std::filesystem::remove(built, cleanupError);

    McpToolRegistry registry;
    std::string createSchema;
    Check(HasTool(registry, "after_create_project", &createSchema) &&
              HasTool(registry, "after_open_project_copy") &&
              HasTool(registry, "after_describe_project") &&
              HasTool(registry, "after_list_comps") &&
              HasTool(registry, "after_list_footage") &&
              HasTool(registry, "after_describe_comp") &&
              HasTool(registry, "after_set_text") &&
              HasTool(registry, "after_set_property") &&
              HasTool(registry, "after_set_layer_state") &&
              HasTool(registry, "after_render_frame") &&
              HasTool(registry, "after_replace_footage") &&
              HasTool(registry, "after_import_footage") &&
              HasTool(registry, "after_add_layer") &&
              HasTool(registry, "after_duplicate_layer") &&
              HasTool(registry, "after_save_project"),
          "les quinze outils After sont publiés dans le catalogue MCP");
    Check(createSchema.find("project_path") != std::string::npos &&
              createSchema.find("\"required\":[\"project_path\"]") !=
                  std::string::npos,
          "le schéma MCP exige le chemin de création");

    AfterBackend backend;
    mcp_json::Value arguments = mcp_json::Value::MakeObject();
    arguments.Set("project_path", mcp_json::Value::MakeString(target.string()));
    McpToolCallOutcome outcome =
        registry.Call(backend, "after_create_project", arguments);
    createPath = Argument(backend.last_request, "project_path");
    Check(outcome.ok && backend.calls == 1 &&
              backend.last_request.method == AfterBridgeMethod::CreateProject &&
              createPath && createPath->AsString() == target.string(),
          "after_create_project passe par le seam McpBackend");

    outcome = registry.Call(backend, "after_list_comps",
                            mcp_json::Value::MakeObject());
    Check(outcome.ok && backend.calls == 2 &&
              backend.last_request.method == AfterBridgeMethod::ListComps,
          "after_list_comps passe par le backend actif");

    arguments = mcp_json::Value::MakeObject();
    arguments.Set("comp_id", mcp_json::Value::MakeString("42"));
    arguments.Set("layer_id", mcp_json::Value::MakeString("9"));
    arguments.Set("property_path",
                  StringArray({"ADBE Transform Group", "ADBE Position"}));
    mcp_json::Value position = mcp_json::Value::MakeArray();
    position.Push(mcp_json::Value::MakeInt(960));
    position.Push(mcp_json::Value::MakeInt(540));
    arguments.Set("value", std::move(position));
    arguments.Set("frame", mcp_json::Value::MakeInt(12));
    outcome = registry.Call(backend, "after_set_property", arguments);
    Check(outcome.ok && backend.calls == 3 &&
              backend.last_request.method == AfterBridgeMethod::SetProperty &&
              Argument(backend.last_request, "frame") != nullptr,
          "after_set_property conserve le chemin, la valeur et la frame");

    arguments = mcp_json::Value::MakeObject();
    arguments.Set("comp_id", mcp_json::Value::MakeString("42"));
    arguments.Set("frame", mcp_json::Value::MakeInt(120));
    arguments.Set("image_path", mcp_json::Value::MakeString(still.string()));
    outcome = registry.Call(backend, "after_render_frame", arguments);
    Check(outcome.ok && backend.calls == 4 &&
              backend.last_request.method == AfterBridgeMethod::RenderFrame &&
              Argument(backend.last_request, "image_path") != nullptr,
          "after_render_frame passe la cible d'image au bridge");

    arguments = mcp_json::Value::MakeObject();
    arguments.Set("comp_id", mcp_json::Value::MakeString("42"));
    arguments.Set("layer_id", mcp_json::Value::MakeString("9"));
    outcome = registry.Call(backend, "after_duplicate_layer", arguments);
    Check(outcome.ok && backend.calls == 5 &&
              backend.last_request.method == AfterBridgeMethod::DuplicateLayer,
          "after_duplicate_layer passe par le seam McpBackend");

    mcp_json::Value invalidArguments = mcp_json::Value::MakeObject();
    invalidArguments.Set("unexpected", mcp_json::Value::MakeBool(true));
    outcome =
        registry.Call(backend, "after_describe_project", invalidArguments);
    Check(!outcome.ok && outcome.error_name == "ValidationFailed" &&
              backend.calls == 5,
          "un argument MCP inconnu est refusé avant le bridge");

    if (failures != 0) {
        std::cerr << failures << " assertion(s) en échec\n";
        return 1;
    }
    std::cout << "All After bridge tests passed\n";
    return 0;
}
