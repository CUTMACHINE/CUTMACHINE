#include "Cli.h"
#include "Document.h"
#include "EditLog.h"
#include "Json.h"
#include "LookLut.h"
#include "LookLutStorage.h"
#include "MotionDocument.h"
#include "MotionTemplates.h"
#include "Operations.h"
#include "Project.h"
#include "ProjectStorage.h"
#include "Ulid.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <map>
#include <sstream>

namespace {

int failures = 0;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

void Write(const std::filesystem::path& path, const std::string& contents) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << contents;
}

std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::ostringstream output;
    output << input.rdbuf();
    return output.str();
}

mcp_json::Value Json(const std::string& text) {
    mcp_json::Value value;
    std::string error;
    if (!mcp_json::Value::Parse(text, value, error)) {
        throw std::runtime_error(error);
    }
    return value;
}

// Every file of a directory and its bytes: what "unchanged" means for a
// package.
std::map<std::string, std::string> Snapshot(
    const std::filesystem::path& directory) {
    std::map<std::string, std::string> files;
    std::error_code ignored;
    for (const auto& entry :
         std::filesystem::recursive_directory_iterator(directory, ignored)) {
        if (entry.is_regular_file()) {
            files[entry.path().lexically_relative(directory).generic_string()] =
                Read(entry.path());
        }
    }
    return files;
}

// FI12 -- collect the stored alternatives, not just the default picture.
void CheckMotionCollection(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using mcp_json::Value;
    const fs::path package = root / "MotionSource.cutmachine-project";
    Document document;
    DocumentComposition child;
    child.name = "Child";
    child.definition = WithDefinitionIdentities(Json(R"({
      "width":32,"height":32,"fps":25,"duration":25,
      "props":{"logo":{"type":"text","default":"images/child.png"}},
      "root":{"type":"image","src":{"prop":"logo"}}})"));
    DocumentComposition composition;
    composition.name = "Assets";
    composition.definition = WithDefinitionIdentities(Json(
        R"({"width":32,"height":32,"fps":25,"duration":25,
        "props":{
          "logo":{"type":"text","default":"images/default.png"},
          "formatLogo":{"type":"text","default":"images/format-default.png"},
          "childLogo":{"type":"text","default":"images/instance.png"},
          "logos":{"type":"list","of":"text",
                   "default":["images/repeat.png"]},
          "choice":{"type":"enum","values":["a","b"],"default":"a"}},
        "tokens":{"pictures":{"a":"images/a.png","b":"images/b.png"}},
        "variants":{"alternate":{"props":{"logo":"images/variant.png",
                                     "logos":["images/repeat-variant.png"]}}},
        "formats":{"square":{"width":32,"height":32,
                                "props":{"formatLogo":"images/format.png"}}},
        "root":{"type":"fill","children":[
          {"type":"image","id":"literal","src":"images/literal.png"},
          {"type":"image","src":{"prop":"logo"}},
          {"type":"image","src":{"prop":"formatLogo"}},
          {"type":"image","src":{"prop":"choice","map":"pictures"}},
          {"type":"image","id":"external","src":)" +
        Value::MakeString((root / "external.png").string()).Dump() +
        R"(},{"type":"image","id":"inline-map",
               "src":{"prop":"choice","map":{"a":)" +
        Value::MakeString((root / "external.png").string()).Dump() +
        R"(,"b":"../outside.png"}}},
          {"type":"image","src":"../outside.png"},
          {"type":"repeat","each":{"prop":"logos"},
           "template":{"type":"image","src":{"item":true}}},
          {"type":"composition","composition":)" +
        Value::MakeString(child.id).Dump() +
        R"(,"props":{"logo":{"prop":"childLogo"}}},
          {"type":"text","content":"not-an-asset.png"}]}})"));
    document.compositions = {composition, child};
    DocumentTrack track;
    track.kind = "video";
    DocumentClip clip;
    clip.composition_id = composition.id;
    clip.timeline_in = {0, 25};
    clip.source_in = {0, 25};
    clip.duration = {25, 25};
    clip.motion_props = Json(R"({"logo":"images/clip.png",
                                 "logos":["images/repeat-clip.png"]})");
    track.clips = {clip};
    document.sequence.tracks = {track};
    Project project = Project::FromDocument(document, "Motion assets");
    std::string source;
    std::string error;
    for (const auto& stored : project.compositions) {
        MotionComposition parsed;
        MotionError parseError;
        Check(ParseDocumentComposition(stored, parsed, parseError),
              "parse asset fixture: " + parseError.path + ": " +
                  parseError.message);
    }
    Check(CreatePortableProject(package.string(), project, source, error),
          "create motion fixture: " + error);
    const std::vector<std::string> files = {
        "default",  "literal",        "child",      "variant", "format",
        "clip",     "repeat",         "a",          "b",       "format-default",
        "instance", "repeat-variant", "repeat-clip"};
    for (const auto& name : files) {
        Write(package / "images" / (name + ".png"), name);
    }
    Write(root / "external.png", "external");
    Write(root / "outside.png", "outside");
    const std::string before = Read(source);
    const fs::path destination = root / "MotionCollected.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(source, destination.string(), result, error),
          "collect all motion resources: " + error);
    for (const auto& name : files) {
        Check(Read(destination / "images" / (name + ".png")) == name,
              "collect motion choice " + name);
    }
    Check(!fs::exists(destination / "not-an-asset.png"),
          "text content is not mistaken for an asset");
    Check(Read(source) == before,
          "collection leaves the source definitions untouched");
    Project reopened;
    Check(LoadStoredProject(result.project_path, reopened, error),
          "reopen collected motion project: " + error);
    if (!reopened.compositions.empty()) {
        MotionNodeLocation node;
        Check(FindMotionNodeByName(reopened.compositions.front().definition,
                                   "external", node),
              "external image keeps its identity");
        const Value* image =
            MotionTreeAt(reopened.compositions.front().definition, node.path);
        const Value* src = (image != nullptr) ? image->Find("src") : nullptr;
        Check((src != nullptr) && src->IsString() &&
                  fs::path(src->AsString()).is_relative() &&
                  Read(destination / src->AsString()) == "external",
              "an external literal is rebased to the collected bytes");
    }
    // Missing choices and unsafe bindings must not publish a partial folder.
    fs::remove(package / "images/b.png");
    PortableProjectResult refused;
    const fs::path missing = root / "MotionMissing";
    Check(!CollectPortableProject(source, missing.string(), refused, error) &&
              !fs::exists(missing) && error.find("motion") != std::string::npos,
          "a missing non-default token choice refuses collection");
    Write(package / "images/b.png", "b");
    // FI12 -- `logo` feeds nothing but a src: the clip's external value is
    // rewritten in the copy, not refused.
    project.timelines.front().tracks.front().clips.front().motion_props =
        Json("{\"logo\":" +
             Value::MakeString((root / "external.png").string()).Dump() + "}");
    Check(CommitStoredProject(source, project, error),
          "store an external path in a src-only prop: " + error);
    const auto external = Snapshot(package);
    const fs::path rebased = root / "MotionRebased.cutmachine-project";
    Check(CollectPortableProject(source, rebased.string(), result, error),
          "an external value of a src-only prop collects: " + error);
    Check(Snapshot(package) == external,
          "rewriting the clip's value leaves every source file untouched");
    Project rebasedProject;
    Check(LoadStoredProject(result.project_path, rebasedProject, error),
          "reopen the rebased motion project: " + error);
    const Value* rebasedLogo =
        rebasedProject.timelines.empty() ||
                rebasedProject.timelines.front().tracks.empty() ||
                rebasedProject.timelines.front().tracks.front().clips.empty()
            ? nullptr
            : rebasedProject.timelines.front()
                  .tracks.front()
                  .clips.front()
                  .motion_props.Find("logo");
    Check(rebasedLogo != nullptr && rebasedLogo->IsString() &&
              rebasedLogo->AsString().rfind("Media/Motion/", 0) == 0 &&
              Read(rebased / rebasedLogo->AsString()) == "external",
          "the clip's value names the collected copy inside the package");
    // `childLogo` reaches the child's src through an instance: rewriting it
    // would change what the instance binding means, so it stays refused.
    project.timelines.front().tracks.front().clips.front().motion_props =
        Json("{\"childLogo\":" +
             Value::MakeString((root / "external.png").string()).Dump() + "}");
    Check(CommitStoredProject(source, project, error),
          "store an external path in a prop passed to an instance: " + error);
    const fs::path unsafe = root / "MotionUnsafe";
    Check(!CollectPortableProject(source, unsafe.string(), refused, error) &&
              !fs::exists(unsafe) &&
              error.find("src binding") != std::string::npos,
          "external prop binding is explicitly refused before publication");
}

// The value at `keys` below `root`, or null.
const mcp_json::Value* At(const mcp_json::Value& root,
                          std::initializer_list<const char*> keys) {
    const mcp_json::Value* value = &root;
    for (const char* key : keys) {
        value = value->Find(key);
        if (value == nullptr) {
            return nullptr;
        }
    }
    return value;
}

// FI12 -- the values a src-only prop stores in the definition itself move
// too: its default, a variant's and a format's.
void CheckMotionDefinitionValuesMove(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using mcp_json::Value;
    const fs::path day = root / "Brand" / "jour.png";
    const fs::path night = root / "Brand" / "nuit.png";
    Write(day, "day mark");
    Write(night, "night mark");
    const std::string dayPath = Value::MakeString(day.string()).Dump();
    const std::string nightPath = Value::MakeString(night.string()).Dump();
    Document document;
    DocumentComposition stored;
    stored.name = "Versions";
    stored.definition = WithDefinitionIdentities(Json(
        R"({"width":32,"height":32,"fps":25,"duration":25,
        "props":{"logo":{"type":"text","default":)" +
        dayPath + R"(},
                 "marque":{"type":"text","default":)" +
        dayPath + R"(}},
        "variants":{"nuit":{"props":{"logo":)" +
        nightPath + R"(}}},
        "formats":{"carre":{"width":32,"height":32,
                            "props":{"marque":)" +
        nightPath + R"(}}},
        "root":{"type":"fill","children":[
          {"type":"image","src":{"prop":"logo"}},
          {"type":"image","src":{"prop":"marque"}}]}})"));
    document.compositions = {stored};
    MotionComposition parsed;
    MotionError parseError;
    Check(ParseDocumentComposition(stored, parsed, parseError),
          "parse the versions fixture: " + parseError.path + ": " +
              parseError.message);
    DocumentTrack track;
    track.kind = "video";
    DocumentClip clip;
    clip.composition_id = stored.id;
    clip.timeline_in = {0, 25};
    clip.source_in = {0, 25};
    clip.duration = {25, 25};
    track.clips = {clip};
    document.sequence.tracks = {track};
    Project project = Project::FromDocument(document, "Versions");
    const fs::path package = root / "VersionsSource.cutmachine-project";
    std::string source;
    std::string error;
    Check(CreatePortableProject(package.string(), project, source, error),
          "create the versions fixture: " + error);
    const auto before = Snapshot(package);
    const fs::path destination = root / "VersionsCollected.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(source, destination.string(), result, error),
          "a definition storing external logos collects: " + error);
    Check(Snapshot(package) == before,
          "the source definition is left as it was");
    Project reopened;
    Check(LoadStoredProject(result.project_path, reopened, error) &&
              !reopened.compositions.empty(),
          "reopen the collected versions: " + error);
    if (reopened.compositions.empty()) {
        return;
    }
    const Value& definition = reopened.compositions.front().definition;
    const auto collected = [&](const Value* value, const std::string& bytes) {
        return value != nullptr && value->IsString() &&
               value->AsString().rfind("Media/Motion/", 0) == 0 &&
               Read(destination / value->AsString()) == bytes;
    };
    Check(collected(At(definition, {"props", "logo", "default"}), "day mark"),
          "the default the clip shows is rewritten to its copy");
    Check(collected(At(definition, {"variants", "nuit", "props", "logo"}),
                    "night mark"),
          "a variant's logo is rewritten to its copy");
    Check(collected(At(definition, {"formats", "carre", "props", "marque"}),
                    "night mark"),
          "a format's logo is rewritten to its copy");
    Check(collected(At(definition, {"props", "marque", "default"}), "day mark"),
          "two props naming the same file share its copy");
}

// FI12, undo in a collected copy -- docs/roadmap/FIABILITE-2026-10.md. The
// journals were copied as they were: undoing then redoing a motion edit in
// the copy brought back the source's path, and so did redoing a rush relink.
// History here: a clip's logo changed from A to B, an image node's literal
// src from C to D, a rush relinked from an old path to a new one. A and C
// are named by history alone.
void CheckCollectedHistory(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using mcp_json::Value;
    const fs::path brand = root / "Histoire";
    const auto file = [&](const std::string& name, const std::string& bytes) {
        Write(brand / name, bytes);
        return Value::MakeString((brand / name).string());
    };
    const Value logoA = file("logo-a.png", "logo A");
    const Value logoB = file("logo-b.png", "logo B");
    const Value imageC = file("image-c.png", "image C");
    const Value imageD = file("image-d.png", "image D");
    Write(brand / "nouveau.mov", "relinked media");

    Document document;
    DocumentComposition stored;
    stored.name = "Histoire";
    stored.definition = WithDefinitionIdentities(Json(
        R"({"width":32,"height":32,"fps":25,"duration":25,
        "props":{"logo":{"type":"text","default":"logo.png"}},
        "root":{"type":"fill","children":[
          {"type":"image","src":{"prop":"logo"}},
          {"type":"image","src":)" +
        imageC.Dump() + "}]}}"));
    document.compositions = {stored};
    const Value* literalNode = At(stored.definition, {"root", "children"});
    const std::string literalUid =
        literalNode != nullptr && literalNode->AsArray().size() == 2
            ? literalNode->AsArray()[1].Find("uid")->AsString()
            : std::string();
    DocumentClip clip;
    clip.composition_id = stored.id;
    clip.timeline_in = {0, 25};
    clip.source_in = {0, 25};
    clip.duration = {25, 25};
    clip.motion_props = Json("{\"logo\":" + logoA.Dump() + "}");
    DocumentTrack track;
    track.kind = "video";
    track.clips = {clip};
    document.sequence.tracks = {track};
    LibraryMedia rush;
    rush.path = (brand / "ancien.mov").string();
    rush.filename = "ancien.mov";
    rush.rate = {25, 1};
    rush.duration = {100, 25};
    // A sound: complete metadata is what a relink asks of its replacement.
    rush.codec = "pcm_s16le";
    rush.has_video = false;
    rush.has_audio = true;
    rush.orientation = "audio";
    rush.audio_rate = 48000;
    rush.audio_channels = 2;
    rush.metadata_complete = true;
    document.library = {rush};
    document.sources = {{rush.id, rush.path, rush.rate, rush.duration}};
    Project project = Project::FromDocument(document, "Histoire");
    const fs::path package = root / "HistorySource.cutmachine-project";
    std::string source;
    std::string error;
    Check(CreatePortableProject(package.string(), project, source, error),
          "create the history fixture: " + error);

    const Ulid timelineId = project.timelines.front().id;
    Document timeline = project.MakeDocument(timelineId);
    EditLog log;
    EditError editError = EditError::None;
    Check(log.Apply(timeline,
                    SetClipMotionPropsOperation{
                        clip.id, Json("{\"logo\":" + logoB.Dump() + "}")},
                    editError, error),
          "the clip's logo changes from A to B: " + error);
    Check(log.Apply(timeline,
                    SetMotionNodeOperation{stored.id,
                                           {},
                                           {},
                                           Json(R"({"uid":")" + literalUid +
                                                R"(","type":"image","src":)" +
                                                imageD.Dump() + "}")},
                    editError, error),
          "the image node's src changes from C to D: " + error);
    Check(project.CommitDocument(timelineId, timeline, error),
          "the timeline keeps both edits: " + error);
    LibraryMedia replacement = rush;
    replacement.path = (brand / "nouveau.mov").string();
    replacement.filename = "nouveau.mov";
    ProjectEditLog projectLog;
    Check(projectLog.Apply(project,
                           RelinkProjectMediaOperation{
                               {{rush.id, replacement, replacement.path}}},
                           editError, error),
          "the rush is relinked: " + error);
    Check(CommitStoredProjectAndLogs(source, project, {{timelineId, log}},
                                     projectLog, error),
          "store the history fixture: " + error);
    const auto before = Snapshot(package);

    const fs::path destination = root / "HistoryCollected.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(source, destination.string(), result, error),
          "a project with a history collects: " + error);
    Check(Snapshot(package) == before, "the source package is left as it was");
    Check(result.verbatim_histories.empty(),
          "every journal is rewritten, none kept as it was");

    Project copy;
    Check(LoadStoredProject(result.project_path, copy, error),
          "reopen the collected copy: " + error);
    EditLog copyLog;
    Check(EditLog::Load(
              TimelineEditLogPathForProject(result.project_path, timelineId),
              copyLog, editError, error) &&
              copyLog.AppliedCount() == 2,
          "the copy keeps the timeline's two entries: " + error);
    ProjectEditLog copyProjectLog;
    Check(
        ProjectEditLog::Load(ProjectEditLogPathForProject(result.project_path),
                             copyProjectLog, editError, error) &&
            copyProjectLog.AppliedCount() == 1,
        "the copy keeps the project's entry: " + error);
    if (copy.timelines.empty() || copy.compositions.empty() ||
        copy.rushes.empty()) {
        Check(false, "the copy holds the timeline, composition and rush");
        return;
    }
    // A path inside the copy whose bytes are `bytes`.
    const auto inside = [&](const Value* value, const std::string& bytes) {
        return value != nullptr && value->IsString() &&
               !fs::path(value->AsString()).is_absolute() &&
               value->AsString().rfind("Media/Motion/", 0) == 0 &&
               Read(destination / value->AsString()) == bytes;
    };
    Document state = copy.MakeDocument(timelineId);
    const auto logo = [&state]() -> const Value* {
        for (const DocumentTrack& candidate : state.sequence.tracks)
            for (const DocumentClip& item : candidate.clips)
                if (!item.composition_id.empty())
                    return item.motion_props.Find("logo");
        return nullptr;
    };
    const auto image = [&state]() -> const Value* {
        if (state.compositions.empty()) return nullptr;
        const Value* children =
            At(state.compositions.front().definition, {"root", "children"});
        return children != nullptr && children->AsArray().size() == 2
                   ? children->AsArray()[1].Find("src")
                   : nullptr;
    };
    Check(inside(logo(), "logo B") && inside(image(), "image D"),
          "the copy's current state names its own copies of B and D");
    Check(copyLog.Undo(state, editError, error) && inside(image(), "image C") &&
              inside(logo(), "logo B"),
          "undoing the node edit in the copy names its copy of C, which "
          "history alone named: " +
              error);
    Check(copyLog.Undo(state, editError, error) && inside(logo(), "logo A"),
          "undoing the clip edit names the copy of A: " + error);
    Check(copyLog.Redo(state, editError, error) &&
              copyLog.Redo(state, editError, error) &&
              inside(logo(), "logo B") && inside(image(), "image D"),
          "redoing both comes back to B and D inside the copy: " + error);

    const std::string relinked = copy.rushes.front().path;
    Check(relinked.rfind("Media/", 0) == 0 &&
              Read(destination / relinked) == "relinked media",
          "the rush is copied into the package: " + relinked);
    Check(
        copyProjectLog.Undo(copy, editError, error) &&
            copy.rushes.front().path == (brand / "ancien.mov").string(),
        "undoing the relink names the old path, which is not copied: " + error);
    Check(copyProjectLog.Redo(copy, editError, error) &&
              copy.rushes.front().path == relinked &&
              copy.sources.front().path == relinked,
          "redoing it comes back to the copy, not the source's path: " +
              copy.rushes.front().path);
}

// FI12 -- the shipped `logo` template, posed the way it is used: the logo
// is a file outside the package, named by its absolute path in each clip,
// and the template's default (logo.png) exists nowhere.
void CheckMotionLogoCollection(const std::filesystem::path& root) {
    namespace fs = std::filesystem;
    using mcp_json::Value;
    std::vector<MotionTemplateInfo> catalog;
    std::string error;
    Check(MotionTemplateCatalog(catalog, error),
          "read the shipped templates: " + error);
    const auto shipped = std::find_if(
        catalog.begin(), catalog.end(),
        [](const MotionTemplateInfo& piece) { return piece.name == "logo"; });
    if (shipped == catalog.end()) {
        Check(false, "the logo template ships");
        return;
    }
    Value instance;
    std::string format;
    Check(InstantiateMotionTemplate(shipped->definition, 1080, 1920, instance,
                                    format, error),
          "instantiate the logo template: " + error);
    Document document;
    DocumentComposition logo;
    logo.name = "Logo";
    logo.definition = WithDefinitionIdentities(instance);
    document.compositions = {logo};
    const fs::path mark = root / "Brand" / "marque.png";
    Write(mark, "brand mark");
    const auto logoClip = [&](int64_t start, const std::string& props) {
        DocumentClip clip;
        clip.composition_id = logo.id;
        clip.timeline_in = {start, 25};
        clip.source_in = {0, 25};
        clip.duration = {250, 25};
        clip.motion_props = Json(props);
        return clip;
    };
    const std::string posed =
        "{\"logo\":" + Value::MakeString(mark.string()).Dump() +
        ",\"coin\":\"bas-gauche\"}";
    DocumentTrack track;
    track.kind = "video";
    track.clips = {logoClip(0, posed), logoClip(250, posed)};
    document.sequence.tracks = {track};
    Project project = Project::FromDocument(document, "Logo");
    const fs::path package = root / "LogoSource.cutmachine-project";
    std::string source;
    Check(CreatePortableProject(package.string(), project, source, error),
          "create the logo fixture: " + error);
    Check(!fs::exists(package / "logo.png"),
          "the template's default logo is nowhere on disk");
    const auto before = Snapshot(package);

    const fs::path destination = root / "LogoCollected.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(source, destination.string(), result, error),
          "a logo posed by its absolute path collects: " + error);
    Check(Snapshot(package) == before,
          "collection leaves every file of the source package as it was");
    std::vector<fs::path> copies;
    std::error_code ignored;
    for (const auto& entry :
         fs::directory_iterator(destination / "Media" / "Motion", ignored)) {
        copies.push_back(entry.path());
    }
    Check(copies.size() == 1 && Read(copies.front()) == "brand mark" &&
              copies.front().extension() == ".png" &&
              copies.front().stem().string().size() == 64,
          "two clips posing the same logo share one copy, named by its "
          "SHA-256");
    Check(!fs::exists(destination / "logo.png"),
          "a default no clip uses is neither required nor invented");
    const std::string collectedPath =
        copies.empty()
            ? std::string()
            : copies.front().lexically_relative(destination).generic_string();
    Project reopened;
    Check(LoadStoredProject(result.project_path, reopened, error),
          "reopen the collected logo project: " + error);
    size_t logoClips = 0;
    for (const DocumentSequence& timeline : reopened.timelines) {
        for (const DocumentTrack& candidate : timeline.tracks) {
            for (const DocumentClip& clip : candidate.clips) {
                if (clip.composition_id != logo.id) {
                    continue;
                }
                ++logoClips;
                const Value* value = clip.motion_props.Find("logo");
                const Value* corner = clip.motion_props.Find("coin");
                Check(value != nullptr && value->IsString() &&
                          value->AsString() == collectedPath,
                      "each clip names the collected logo in the package");
                Check(corner != nullptr && corner->IsString() &&
                          corner->AsString() == "bas-gauche",
                      "the clip's other props are kept as they were");
            }
        }
    }
    Check(logoClips == 2, "both logo clips are collected");
    const Value* props =
        reopened.compositions.empty()
            ? nullptr
            : reopened.compositions.front().definition.Find("props");
    const Value* logoProp = props == nullptr ? nullptr : props->Find("logo");
    const Value* fallback =
        logoProp == nullptr ? nullptr : logoProp->Find("default");
    Check(fallback != nullptr && fallback->IsString() &&
              fallback->AsString() == "logo.png",
          "the unused default keeps its spelling in the copy");

    // A clip that keeps the default relies on it: absent, it is refused
    // before anything is published; present, it travels as it is spelled.
    project.timelines.front().tracks.front().clips.push_back(
        logoClip(500, R"({"largeur":20})"));
    Check(CommitStoredProject(source, project, error),
          "store a clip that keeps the default logo: " + error);
    PortableProjectResult refused;
    const fs::path missing = root / "LogoDefaultMissing";
    Check(!CollectPortableProject(source, missing.string(), refused, error) &&
              !fs::exists(missing) &&
              error.find("logo.png") != std::string::npos,
          "a default a clip relies on is still required: " + error);
    Write(package / "logo.png", "default logo");
    const fs::path withDefault = root / "LogoWithDefault.cutmachine-project";
    Check(CollectPortableProject(source, withDefault.string(), result, error),
          "a clip keeping the default collects once it exists: " + error);
    Check(Read(withDefault / "logo.png") == "default logo",
          "the default a clip keeps travels at its relative path");
    Project kept;
    Check(LoadStoredProject(result.project_path, kept, error) &&
              !kept.timelines.empty() &&
              !kept.timelines.front().tracks.empty() &&
              kept.timelines.front().tracks.front().clips.size() == 3 &&
              kept.timelines.front()
                      .tracks.front()
                      .clips.back()
                      .motion_props.Dump() == R"({"largeur":20})",
          "the clip that keeps the default is copied as it was: " + error);

    // A prop that is a src and also shows as text cannot be rewritten
    // without changing the text: its external value stays refused.
    DocumentComposition signature;
    signature.name = "Signature";
    signature.definition = WithDefinitionIdentities(Json(R"({
      "width":32,"height":32,"fps":25,"duration":25,
      "props":{"logo":{"type":"text","default":"logo.png"}},
      "root":{"type":"fill","children":[
        {"type":"image","src":{"prop":"logo"}},
        {"type":"text","content":{"prop":"logo"}}]}})"));
    project.compositions.push_back(signature);
    DocumentClip shown = logoClip(750, posed);
    shown.composition_id = signature.id;
    shown.duration = {25, 25};
    shown.motion_props =
        Json("{\"logo\":" + Value::MakeString(mark.string()).Dump() + "}");
    project.timelines.front().tracks.front().clips.push_back(shown);
    Check(CommitStoredProject(source, project, error),
          "store a prop used as a src and as text: " + error);
    const fs::path shared = root / "LogoShared";
    Check(!CollectPortableProject(source, shared.string(), refused, error) &&
              !fs::exists(shared) &&
              error.find("src binding") != std::string::npos,
          "a prop that is not only a src keeps its refusal: " + error);
}

std::string Hex(const std::string& input) {
    constexpr char kDigits[] = "0123456789abcdef";
    std::string output;
    for (const unsigned char byte : input) {
        output.push_back(kDigits[byte >> 4]);
        output.push_back(kDigits[byte & 0xf]);
    }
    return output;
}

std::string MediaManifestSection(const std::string& manifest) {
    const size_t begin = manifest.find("\"media\":");
    const size_t end = manifest.rfind(']');
    if (begin == std::string::npos || end == std::string::npos || end < begin)
        return {};
    return manifest.substr(begin, end - begin + 1);
}

}  // namespace

int main() {
    namespace fs = std::filesystem;
    const fs::path root =
        fs::temp_directory_path() / (GenerateUlid() + "-project-storage-tests");
    const fs::path sourceDirectory = root / "Film.cutmachine-project";
    CheckMotionCollection(root);
    CheckMotionLogoCollection(root);
    CheckMotionDefinitionValuesMove(root);
    CheckCollectedHistory(root);
    // S4 -- SAVING_ROADMAP.md. A real filename may hold JSON punctuation: it
    // must not hide a collected original from integrity verification. The
    // manifest was searched as text, up to the first '}' after the media's
    // id, and a path holding one ended that search before its fingerprint.
    const std::string filename = "prise\"\\}.mov";
    Write(root / "rushes/a" / filename, "first-media");
    Write(root / "rushes/b" / filename, "second-media");

    Document document;
    LibraryMedia first;
    first.id = "01K50000000000000000000001";
    first.path = (root / "rushes/a" / filename).string();
    first.filename = filename;
    first.rate = {25, 1};
    first.duration = {100, 25};
    first.metadata_complete = false;
    LibraryMedia second = first;
    second.id = "01K50000000000000000000002";
    second.path = (root / "rushes/b" / filename).string();
    document.library = {first, second};
    document.sources = {
        {first.id, first.path, first.rate, first.duration},
        {second.id, second.path, second.rate, second.duration},
    };
    Project project = Project::FromDocument(document, "Portable Film");
    DocumentSequence secondTimeline;
    secondTimeline.id = "01K50000000000000000000003";
    secondTimeline.name = "Vertical";
    secondTimeline.width = 1080;
    secondTimeline.height = 1920;
    project.timelines.push_back(secondTimeline);

    std::map<std::string, EditLog> logs;
    for (const DocumentSequence& timeline : project.timelines) {
        Document timelineDocument = project.MakeDocument(timeline.id);
        EditLog log;
        EditError editError = EditError::None;
        std::string detail;
        DocumentMarker marker;
        marker.name = timeline.name;
        Check(log.Apply(timelineDocument, AddMarkerOperation{marker}, editError,
                        detail),
              "timeline history fixture applies: " + detail);
        logs.emplace(timeline.id, std::move(log));
    }
    ProjectEditLog projectLog;
    std::string error;
    std::string sourceProject;
    Check(CreatePortableProject(sourceDirectory.string(), project,
                                sourceProject, error),
          "source project package is created: " + error);
    // AGENT-2026-09 A28 -- a destination whose parent does not exist yet
    // gets it created, as a shell's mkdir -p would.
    {
        const fs::path nested =
            root / "a28-missing" / "deeper" / "Nested.cutmachine-project";
        std::string nestedProject;
        std::string nestedError;
        Check(CreatePortableProject(nested.string(), project, nestedProject,
                                    nestedError) &&
                  fs::is_directory(nested),
              "a project is created inside a parent that did not exist: " +
                  nestedError);
    }
    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "source project and histories save: " + error);
    CollectionIntegrityReport sourceIntegrity;
    Check(VerifyPortableProject(sourceProject, sourceIntegrity, error) &&
              sourceIntegrity.verified_media == 0 &&
              sourceIntegrity.missing_media.empty() &&
              sourceIntegrity.modified_media.empty(),
          "external media absent from the collection manifest is not "
          "reported as modified: " +
              error);

    // PERF-2026-09. Every commit is handed the whole package, but only the
    // artifacts whose bytes actually change may be rewritten: each rewrite
    // costs an F_FULLFSYNC plus two directory syncs, and on an external
    // exFAT volume that is what makes a single edit visibly stall the
    // editor. A rewrite renames the destination aside and moves a fresh file
    // into place, so the inode is the exact observable: it survives a skip
    // and never survives a rewrite.
    const auto inodeOf = [](const fs::path& path) -> ino_t {
        struct stat status = {};
        return ::stat(path.c_str(), &status) == 0 ? status.st_ino : 0;
    };
    const fs::path package = fs::path(sourceProject).parent_path();
    const std::string editedTimeline = project.timelines.front().id;
    const std::string untouchedTimeline = project.timelines.back().id;
    const fs::path manifestPath = package / "manifest.json";
    const fs::path editedPath =
        package / "Timelines" / (editedTimeline + ".json");
    const fs::path untouchedPath =
        package / "Timelines" / (untouchedTimeline + ".json");
    const fs::path editedLogPath =
        TimelineEditLogPathForProject(sourceProject, editedTimeline);
    const fs::path untouchedLogPath =
        TimelineEditLogPathForProject(sourceProject, untouchedTimeline);
    const fs::path projectLogPath = ProjectEditLogPathForProject(sourceProject);

    const ino_t manifestInode = inodeOf(manifestPath);
    const ino_t untouchedInode = inodeOf(untouchedPath);
    const ino_t untouchedLogInode = inodeOf(untouchedLogPath);
    const ino_t projectLogInode = inodeOf(projectLogPath);
    const ino_t editedInode = inodeOf(editedPath);
    const ino_t editedLogInode = inodeOf(editedLogPath);
    Check(manifestInode != 0 && untouchedInode != 0 && editedInode != 0 &&
              untouchedLogInode != 0 && projectLogInode != 0 &&
              editedLogInode != 0,
          "every package artifact exists before the incremental commit");

    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "re-committing an unchanged project succeeds: " + error);
    Check(inodeOf(fs::path(sourceProject)) != 0 &&
              inodeOf(manifestPath) == manifestInode &&
              inodeOf(editedPath) == editedInode &&
              inodeOf(untouchedPath) == untouchedInode &&
              inodeOf(editedLogPath) == editedLogInode &&
              inodeOf(untouchedLogPath) == untouchedLogInode &&
              inodeOf(projectLogPath) == projectLogInode,
          "a commit that changes nothing rewrites nothing");

    // Edited on copies, and reverted below: everything after this block
    // reads the package back from disk and expects the pristine fixture.
    Project incrementalProject = project;
    std::map<std::string, EditLog> incrementalLogs = logs;
    Document editedDocument = incrementalProject.MakeDocument(editedTimeline);
    EditError incrementalError = EditError::None;
    std::string incrementalDetail;
    DocumentMarker incrementalMarker;
    incrementalMarker.name = "Incremental";
    incrementalMarker.time = {50, 25};
    Check(incrementalLogs.at(editedTimeline)
              .Apply(editedDocument, AddMarkerOperation{incrementalMarker},
                     incrementalError, incrementalDetail),
          "incremental fixture edit applies: " + incrementalDetail);
    Check(incrementalProject.CommitDocument(editedTimeline, editedDocument,
                                            incrementalDetail),
          "incremental fixture edit stages: " + incrementalDetail);
    Check(CommitStoredProjectAndLogs(sourceProject, incrementalProject,
                                     incrementalLogs, projectLog, error),
          "committing one edited timeline succeeds: " + error);
    Check(inodeOf(editedPath) != editedInode &&
              inodeOf(editedLogPath) != editedLogInode,
          "the edited timeline and its journal are rewritten");
    Check(inodeOf(manifestPath) == manifestInode &&
              inodeOf(untouchedPath) == untouchedInode &&
              inodeOf(untouchedLogPath) == untouchedLogInode &&
              inodeOf(projectLogPath) == projectLogInode,
          "the manifest, the other timeline, its journal and the project "
          "journal are left untouched");

    Project reloadedIncremental;
    Check(LoadStoredProject(sourceProject, reloadedIncremental, error) &&
              reloadedIncremental.MakeDocument(editedTimeline)
                      .sequence.markers.size() ==
                  editedDocument.sequence.markers.size(),
          "the incrementally committed generation reloads with its edit: " +
              error);
    Check(CommitStoredProjectAndLogs(sourceProject, project, logs, projectLog,
                                     error),
          "the pristine fixture is restored for the checks below: " + error);

    // R6 -- docs/roadmap/REEL-2026-09.md. A timeline's look LUT is part of
    // the package, like its media: a collection that left it behind would
    // hand over a project that refuses to render rather than one that
    // renders the film ungraded -- still not the film that was collected.
    {
        Project looked;
        Check(LoadStoredProject(sourceProject, looked, error),
              "the fixture loads before the look is laid: " + error);
        const fs::path authorCube = root / "author-look.cube";
        {
            std::ofstream cube(authorCube, std::ios::binary | std::ios::trunc);
            cube << "TITLE \"lift\"\nLUT_3D_SIZE 2\n";
            for (int corner = 0; corner < 8; ++corner)
                cube << (corner & 1) << ' ' << ((corner >> 1) & 1) << ' '
                     << ((corner >> 2) & 1) << '\n';
        }
        SequenceLut laid;
        Check(ImportLookLutIntoPackage(sourceProject, authorCube.string(), "",
                                       {}, laid, error),
              "the look is imported into the source package: " + error);
        looked.timelines.front().look_lut = laid;
        Check(CommitStoredProject(sourceProject, looked, error),
              "the look commits to the source package: " + error);
        const fs::path lookedDestination = root / "Looked.cutmachine-project";
        PortableProjectResult lookedResult;
        Check(CollectPortableProject(sourceProject, lookedDestination.string(),
                                     lookedResult, error),
              "a project carrying a look collects: " + error);
        Check(fs::is_regular_file(lookedDestination /
                                  LookLutPackagePath(laid.digest)),
              "the collected package carries the look under its digest");
        Project reopened;
        Check(LoadStoredProject(lookedResult.project_path, reopened, error) &&
                  reopened.timelines.front().look_lut.digest == laid.digest,
              "the collected project still names the same look: " + error);
        LoadedLookLut readBack;
        Check(
            LoadSequenceLookLut(reopened.timelines.front(),
                                reopened.settings.color_management,
                                lookedDestination.string(), readBack, error) &&
                readBack.samples.size == 2,
            "the look reads back from the copy at its new path: " + error);
        // FI13 -- docs/roadmap/FIABILITE-2026-10.md. Its filename is a
        // promise about its bytes, not proof that they are still there.
        const fs::path storedCube = fs::path(sourceProject).parent_path() /
                                    LookLutPackagePath(laid.digest);
        const auto originalTime = fs::last_write_time(storedCube);
        struct stat originalStatus {};
        Check(stat(storedCube.c_str(), &originalStatus) == 0,
              "inspect the original LUT inode");
        SequenceLut unchanged;
        Check(ImportLookLutIntoPackage(sourceProject, authorCube.string(), "",
                                       {}, unchanged, error),
              "reimport an intact LUT: " + error);
        struct stat unchangedStatus {};
        Check(stat(storedCube.c_str(), &unchangedStatus) == 0 &&
                  unchangedStatus.st_ino == originalStatus.st_ino &&
                  fs::last_write_time(storedCube) == originalTime,
              "an intact LUT keeps its inode and modification time");
        Write(storedCube, "corrupted LUT");
        PortableProjectResult corruptResult;
        const fs::path corruptDestination = root / "CorruptLook";
        Check(
            !CollectPortableProject(sourceProject, corruptDestination.string(),
                                    corruptResult, error) &&
                !fs::exists(corruptDestination),
            "collection refuses a corrupt look before publishing");
        CollectionIntegrityReport corruptIntegrity;
        Check(VerifyPortableProject(sourceProject, corruptIntegrity, error) &&
                  corruptIntegrity.damaged_looks ==
                      std::vector<std::string>{reopened.timelines.front().name},
              "integrity verification reports a corrupt look, and the "
              "project still opens: " +
                  error);
        SequenceLut repaired;
        Check(ImportLookLutIntoPackage(sourceProject, authorCube.string(), "",
                                       {}, repaired, error) &&
                  repaired.digest == laid.digest &&
                  Read(storedCube) == Read(authorCube) &&
                  LoadSequenceLookLut(
                      looked.timelines.front(), {},
                      storedCube.parent_path().parent_path().string(), readBack,
                      error),
              "reimporting the original repairs the damaged stored bytes: " +
                  error);
        fs::remove(storedCube);
        const fs::path unrelated = root / "unrelated.cube";
        Write(unrelated, "do not overwrite");
        fs::create_symlink(unrelated, storedCube);
        Check(ImportLookLutIntoPackage(sourceProject, authorCube.string(), "",
                                       {}, repaired, error) &&
                  !fs::is_symlink(storedCube) &&
                  Read(storedCube) == Read(authorCube) &&
                  Read(unrelated) == "do not overwrite",
              "repair replaces a symlink without writing through it");
        fs::remove(storedCube);
        fs::create_directory(storedCube);
        Check(!ImportLookLutIntoPackage(sourceProject, authorCube.string(), "",
                                        {}, repaired, error) &&
                  fs::is_directory(storedCube),
              "a directory at the LUT path is refused");
        // And a look whose file is gone is a refusal, not a silent
        // collection of a project that no longer renders its film.
        fs::remove(fs::path(sourceProject).parent_path() /
                   LookLutPackagePath(laid.digest));
        PortableProjectResult refused;
        Check(!CollectPortableProject(
                  sourceProject, (root / "Absent.cutmachine-project").string(),
                  refused, error) &&
                  error.find("look LUT") != std::string::npos,
              "collecting without the look LUT is refused by name: " + error);
        looked.timelines.front().look_lut = SequenceLut{};
        Check(CommitStoredProject(sourceProject, looked, error),
              "the fixture is restored without a look: " + error);
    }

    const fs::path destination = root / "Portable Film.cutmachine-project";
    PortableProjectResult result;
    Check(CollectPortableProject(sourceProject, destination.string(), result,
                                 error),
          "portable collection succeeds: " + error);
    Check(result.media_count == 2 && result.media_bytes == 23,
          "collection reports copied media count and bytes");
    // FI12 -- these journals add markers the stored timelines never got:
    // they do not replay against their own state, so no rewrite can be
    // proven. They travel as they were, and the result names them.
    Check(result.verbatim_histories.size() == project.timelines.size(),
          "a journal that does not replay is copied as it was, and named (" +
              std::to_string(result.verbatim_histories.size()) + ")");
    Check(result.project_path ==
              (destination / "project.cutmachine.json").string(),
          "collection reports canonical inner project");

    Project collected;
    Check(LoadStoredProject(result.project_path, collected, error),
          "collected project loads: " + error);
    Check(IsPortableProjectV2(result.project_path),
          "collection publishes the separated-timeline v2 format");
    for (const DocumentSequence& timeline : collected.timelines)
        Check(fs::is_regular_file(destination / "Timelines" /
                                  (timeline.id + ".json")),
              "each v2 timeline has its own physical file");
    Check(collected.rushes.size() == 2,
          "collected project retains every media identity");
    for (const LibraryMedia& media : collected.rushes) {
        Check(!fs::path(media.path).is_absolute() &&
                  media.path.rfind("Media/", 0) == 0,
              "collected media path is internal and relative");
        Check(fs::is_regular_file(destination / media.path),
              "collected media exists at rewritten path");
        Check(media.proxy_path.empty(), "collection clears derived proxy path");
    }
    Check(collected.rushes[0].path != collected.rushes[1].path,
          "same-name originals cannot collide in the collection");
    Check(Read(destination / "manifest.json").find("\"sha256\"") !=
              std::string::npos,
          "collection manifest fingerprints originals");
    const std::string collectedMediaManifest =
        MediaManifestSection(Read(destination / "manifest.json"));
    Project withTransientTimeline = collected;
    DocumentSequence transientTimeline;
    transientTimeline.id = "01K50000000000000000000004";
    transientTimeline.name = "Temporaire";
    withTransientTimeline.timelines.push_back(transientTimeline);
    Check(
        CommitStoredProject(result.project_path, withTransientTimeline, error),
        "adding a timeline commits package metadata: " + error);
    Check(Read(destination / "manifest.json").find(transientTimeline.id) !=
                  std::string::npos &&
              fs::is_regular_file(destination / "Timelines" /
                                  (transientTimeline.id + ".json")) &&
              fs::is_regular_file(TimelineEditLogPathForProject(
                  result.project_path, transientTimeline.id)),
          "manifest, physical timeline, and history advance together");
    Check(CommitStoredProject(result.project_path, collected, error),
          "removing a timeline commits package metadata: " + error);
    Check(Read(destination / "manifest.json").find(transientTimeline.id) ==
                  std::string::npos &&
              !fs::exists(destination / "Timelines" /
                          (transientTimeline.id + ".json")) &&
              !fs::exists(TimelineEditLogPathForProject(result.project_path,
                                                        transientTimeline.id)),
          "obsolete timeline state and history are removed atomically");

    // macOS writes an AppleDouble "._<name>" sidecar next to every file on an
    // exFAT or FAT volume -- the format of most shared media drives. Those
    // sidecars cannot be renamed on their own, so treating one as an obsolete
    // timeline made every save to a project stored on such a drive fail.
    const fs::path sidecar = destination / "Timelines" /
                             ("._" + collected.timelines.front().id + ".json");
    const fs::path stranger = destination / "Timelines" / "notes.json";
    Write(sidecar, "apple-double");
    Write(stranger, "{}");
    Check(CommitStoredProject(result.project_path, collected, error),
          "a package holding AppleDouble sidecars still commits: " + error);
    Check(fs::exists(sidecar) && Read(sidecar) == "apple-double" &&
              fs::exists(stranger),
          "the transaction never touches files it did not write");
    fs::remove(sidecar);
    fs::remove(stranger);
    Check(MediaManifestSection(Read(destination / "manifest.json")) ==
              collectedMediaManifest,
          "timeline commits preserve collected-media paths and fingerprints");
    const fs::path rejectedPackage = root / "Rejected.cutmachine-project";
    fs::copy(destination, rejectedPackage, fs::copy_options::recursive);
    std::string rejectedManifest = Read(rejectedPackage / "manifest.json");
    const size_t v2 = rejectedManifest.find("\"version\":2");
    Check(v2 != std::string::npos, "v2 manifest fixture is recognizable");
    rejectedManifest.replace(v2, std::string("\"version\":2").size(),
                             "\"version\":1");
    Write(rejectedPackage / "manifest.json", rejectedManifest);
    Project rejected;
    Check(!LoadStoredProject(
              (rejectedPackage / "project.cutmachine.json").string(), rejected,
              error),
          "v1 packages are rejected instead of migrated");
    CollectionIntegrityReport integrity;
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.verified_media == 2 &&
              integrity.missing_media.empty() &&
              integrity.modified_media.empty(),
          "fresh collection passes full integrity verification: " + error);

    // A path with no manifest.json beside it has two very different causes,
    // and the message has to say which: a single-file project from before
    // the package format (the file is there, the package is not) versus a
    // path that simply is not there. Reporting "collection manifest is
    // missing" for both describes our bookkeeping instead of the mistake.
    const fs::path legacyProject = root / "example-timeline.json";
    Write(legacyProject, "{\"version\":3,\"sequence\":{}}\n");
    CollectionIntegrityReport legacyReport;
    std::string legacyError;
    Check(!VerifyPortableProject(legacyProject.string(), legacyReport,
                                 legacyError) &&
              legacyError.find("cutmachine-project") != std::string::npos,
          "a single-file project is refused by naming the package "
          "requirement, not the missing manifest: " +
              legacyError);
    CollectionIntegrityReport absentReport;
    std::string absentError;
    // PRD39 -- ROADMAP.md. In the sentence the command line gives
    // (MissingProjectMessage): the window said "no project file at".
    Check(!VerifyPortableProject((root / "nope.cutmachine.json").string(),
                                 absentReport, absentError) &&
              absentError == MissingProjectMessage(
                                 (root / "nope.cutmachine.json").string()),
          "a path that does not exist says so rather than blaming the "
          "manifest, as the command line does: " +
              absentError);
    for (const DocumentSequence& timeline : collected.timelines) {
        EditLog log;
        EditError editError = EditError::None;
        std::string detail;
        Check(EditLog::Load(TimelineEditLogPathForProject(result.project_path,
                                                          timeline.id),
                            log, editError, detail) &&
                  log.AppliedCount() == 1,
              "each timeline history survives collection: " + detail);
    }

    Document authoritative;
    authoritative.sequence = collected.timelines.front();
    authoritative.sequence.name = "Nom physique autoritaire";
    authoritative.color_management = collected.settings.color_management;
    Write(destination / "Timelines" / (authoritative.sequence.id + ".json"),
          authoritative.SaveToString());
    Project reloadedAuthority;
    Check(
        LoadStoredProject(result.project_path, reloadedAuthority, error) &&
            reloadedAuthority.timelines.front().name ==
                "Nom physique autoritaire",
        "v2 physical timeline overrides the embedded compatibility snapshot: " +
            error);

    PortableProjectResult duplicate;
    Check(!CollectPortableProject(sourceProject, destination.string(),
                                  duplicate, error) &&
              error == "destination already exists",
          "collection never overwrites an existing destination");

    // S4 -- a fingerprint the manifest holds is read or the package is
    // refused: one that cannot be read used to pass for a rush left outside
    // the package, which nothing verifies.
    const std::string validManifest = Read(destination / "manifest.json");
    mcp_json::Value manifest;
    Check(mcp_json::Value::Parse(validManifest, manifest, error),
          "collected manifest is valid JSON: " + error);
    const mcp_json::Value validEntry =
        manifest.Find("media")->AsArray().front();
    const auto rejectManifest = [&](const mcp_json::Value& invalid,
                                    const std::string& reason) {
        Write(destination / "manifest.json", invalid.Dump());
        Check(!VerifyPortableProject(result.project_path, integrity, error) &&
                  error.find("collection manifest") != std::string::npos,
              reason + ": " + error);
        Write(destination / "manifest.json", validManifest);
    };
    for (const std::string& invalidBytes :
         {"-1", "11.5", "11e0", "18446744073709551616"}) {
        auto entry = validEntry;
        entry.Set("bytes", mcp_json::Value::MakeRawNumber(invalidBytes));
        auto media = mcp_json::Value::MakeArray();
        media.Push(entry);
        auto invalid = manifest;
        invalid.Set("media", media);
        rejectManifest(invalid, "invalid byte count is rejected");
    }
    for (const auto& invalidSha :
         {mcp_json::Value::MakeNull(),
          mcp_json::Value::MakeString(std::string(64, 'g'))}) {
        auto entry = validEntry;
        entry.Set("sha256", invalidSha);
        auto media = mcp_json::Value::MakeArray();
        media.Push(entry);
        auto invalid = manifest;
        invalid.Set("media", media);
        rejectManifest(invalid, "invalid SHA-256 is rejected");
    }
    auto duplicateMedia = mcp_json::Value::MakeArray();
    duplicateMedia.Push(validEntry);
    duplicateMedia.Push(validEntry);
    auto invalid = manifest;
    invalid.Set("media", duplicateMedia);
    rejectManifest(invalid, "duplicate fingerprints are rejected");
    invalid.Set("media", mcp_json::Value::MakeNull());
    rejectManifest(invalid, "missing media array is rejected");
    // PRD20 -- ROADMAP.md. Refused as what it is, a package newer than this
    // build, by the manifest's reader and by the plain load alike: the load
    // used to take any version starting with a 2 for version 2.
    invalid = manifest;
    invalid.Set("version", mcp_json::Value::MakeInt(20));
    const std::string newerPackage =
        "project package version 20 is newer than this build reads (up to "
        "2): the project was written by a more recent CUTMACHINE";
    Write(destination / "manifest.json", invalid.Dump());
    Check(!VerifyPortableProject(result.project_path, integrity, error) &&
              error == newerPackage,
          "v20 is not mistaken for v2: " + error);
    Project newer;
    Check(!IsPortableProjectV2(result.project_path) &&
              !LoadStoredProject(result.project_path, newer, error) &&
              error == newerPackage,
          "nor by the load that does not verify: " + error);
    Write(destination / "manifest.json", validManifest);
    // PRD39 -- ROADMAP.md. The manifest is read as JSON: written back with
    // other spacing, by a person or a tool, it is the same manifest. A
    // search for the text "version":2 refused it.
    {
        // A space after every colon and comma outside the strings.
        std::string spaced;
        bool quoted = false;
        bool escaped = false;
        for (const char character : validManifest) {
            spaced += character;
            if (quoted) {
                if (escaped) {
                    escaped = false;
                } else if (character == '\\') {
                    escaped = true;
                } else if (character == '"') {
                    quoted = false;
                }
            } else if (character == '"') {
                quoted = true;
            } else if (character == ':' || character == ',') {
                spaced += ' ';
            }
        }
        Check(spaced.find("\"version\": 2") != std::string::npos &&
                  spaced.find("\"format\": \"cutmachine-collection\"") !=
                      std::string::npos,
              "the fixture spaces the manifest out");
        Write(destination / "manifest.json", spaced);
        Project spacedProject;
        Check(
            IsPortableProjectV2(result.project_path) &&
                LoadStoredProject(result.project_path, spacedProject, error) &&
                VerifyPortableProject(result.project_path, integrity, error) &&
                integrity.verified_media == 2,
            "a manifest with other spacing is read and verified: " + error);
        invalid = manifest;
        invalid.Set("version", mcp_json::Value::MakeRawNumber("2.0"));
        Write(destination / "manifest.json", invalid.Dump());
        Check(!IsPortableProjectV2(result.project_path),
              "a version that is not the integer 2 is not version 2");
        invalid.Set("version", mcp_json::Value::MakeString("2"));
        Write(destination / "manifest.json", invalid.Dump());
        Check(!IsPortableProjectV2(result.project_path),
              "nor is the string \"2\"");
        Write(destination / "manifest.json", validManifest);
    }
    Write(destination / "manifest.json", validManifest + "}");
    Check(!VerifyPortableProject(result.project_path, integrity, error),
          "malformed manifest JSON is rejected");
    Write(destination / "manifest.json", validManifest);
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.verified_media == 2,
          "the manifest put back verifies again: " + error);

    // Same size, other bytes: only the SHA-256 tells.
    Write(destination / collected.rushes.front().path, "FIRST-media");
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.modified_media.size() == 1 &&
              integrity.missing_media.empty() && integrity.verified_media == 1,
          "same-sized media with JSON punctuation in its name is detected "
          "by SHA-256");
    fs::remove(destination / collected.rushes.back().path);
    Check(VerifyPortableProject(result.project_path, integrity, error) &&
              integrity.modified_media.size() == 1 &&
              integrity.missing_media.size() == 1 &&
              integrity.verified_media == 0,
          "missing media with JSON punctuation in its name is detected");

    ProjectSessionLock firstLock;
    ProjectSessionLock secondLock;
    std::string owner;
    Check(firstLock.Acquire(sourceProject, owner, error) && firstLock.Held(),
          "first writer acquires the project lock: " + error);
    Check(!secondLock.Acquire(sourceProject, owner, error) && !owner.empty(),
          "second writer is rejected with lock owner information");
    firstLock.Release();
    Check(secondLock.Acquire(sourceProject, owner, error),
          "lock can be reacquired after clean release: " + error);
    secondLock.Release();

    // SAVING_ROADMAP.md, forced-stop validation. A writer killed between
    // creating the lock and writing its owner left an empty lock that
    // refused every later opening. The lock is now published whole; an
    // empty one heals, one with unreadable metadata is abandoned once old.
    {
        const fs::path lockDirectory = sourceProject + ".lock";
        const auto staging = [&] {
            std::vector<fs::path> found;
            for (const auto& entry : fs::directory_iterator(
                     fs::path(sourceProject).parent_path())) {
                if (entry.path().filename().string().rfind(
                        fs::path(sourceProject).filename().string() + ".lock-",
                        0) == 0)
                    found.push_back(entry.path());
            }
            return found;
        };
        const auto age = [](const fs::path& path) {
            fs::last_write_time(path, fs::file_time_type::clock::now() -
                                          std::chrono::minutes(5));
        };
        ProjectSessionLock lock;
        fs::create_directory(lockDirectory);
        Check(lock.Acquire(sourceProject, owner, error) &&
                  fs::exists(lockDirectory / "owner") && staging().empty(),
              "an empty lock left by a killed writer heals: " + error);
        lock.Release();
        Check(!fs::exists(lockDirectory), "the healed lock is released");

        fs::create_directory(lockDirectory);
        Write(lockDirectory / "owner", "01K5");
        Check(!lock.Acquire(sourceProject, owner, error) &&
                  error.find("owner metadata is invalid") != std::string::npos,
              "a fresh lock with unreadable metadata is not taken: " + error);
        age(lockDirectory);
        Check(lock.Acquire(sourceProject, owner, error),
              "an old lock with unreadable metadata is abandoned: " + error);
        lock.Release();

        // A writer killed between writing its staging lock and publishing
        // it: removed once its owner is gone; a fresh one without metadata
        // may be a writer still at work and stays until it is old.
        std::array<char, 256> host{};
        gethostname(host.data(), host.size() - 1);
        const pid_t child = fork();
        if (child == 0) _exit(0);
        waitpid(child, nullptr, 0);
        const fs::path deadStaging =
            sourceProject + ".lock-01K50000000000000000000001";
        fs::create_directory(deadStaging);
        Write(deadStaging / "owner", "01K50000000000000000000001\n" +
                                         std::string(host.data()) + "\n" +
                                         std::to_string(child) + "\nqa\n");
        const fs::path freshStaging =
            sourceProject + ".lock-01K50000000000000000000002";
        fs::create_directory(freshStaging);
        Check(lock.Acquire(sourceProject, owner, error) &&
                  !fs::exists(deadStaging) && fs::exists(freshStaging),
              "a dead writer's staging lock is removed, a fresh one kept: " +
                  error);
        lock.Release();
        age(freshStaging);
        Check(lock.Acquire(sourceProject, owner, error) && staging().empty(),
              "an old staging lock without metadata is removed: " + error);
        lock.Release();

        // Mutual exclusion is unchanged, and a dead owner's lock is taken.
        ProjectSessionLock holder;
        Check(holder.Acquire(sourceProject, owner, error), "holder: " + error);
        Check(!lock.Acquire(sourceProject, owner, error) &&
                  error.find("already open") != std::string::npos &&
                  staging().empty(),
              "a held lock refuses, and leaves no staging behind: " + error);
        holder.Release();
        fs::create_directory(lockDirectory);
        Write(lockDirectory / "owner", "01K50000000000000000000003\n" +
                                           std::string(host.data()) + "\n" +
                                           std::to_string(child) + "\nqa\n");
        Check(lock.Acquire(sourceProject, owner, error),
              "a dead owner's lock is taken over: " + error);
        lock.Release();
    }

    const fs::path interrupted = root / "Interrupted.cutmachine-project";
    fs::create_directories(interrupted / "Timelines");
    const std::string nonce = ".cutmachine-01K50000000000000000000005";
    const fs::path replaced = interrupted / "project.cutmachine.json";
    const fs::path created = interrupted / "new.editlog.json";
    const fs::path removed = interrupted / "Timelines/removed.json";
    Write(replaced, "partially-published-new");
    Write(replaced.string() + nonce + ".bak", "previous-project");
    Write(created, "partially-published-new-log");
    Write(removed.string() + nonce + ".bak", "previous-timeline");
    Write(created.string() + nonce + ".tmp", "unused-temporary");
    std::ostringstream transaction;
    transaction << "CUTMACHINE_TRANSACTION_V1\n"
                << nonce << "\n1 " << Hex("project.cutmachine.json") << "\n0 "
                << Hex("new.editlog.json") << "\n1 "
                << Hex("Timelines/removed.json") << '\n';
    Write(interrupted / ".cutmachine-transaction", transaction.str());
    Check(RecoverTextArtifactTransaction(interrupted.string(), error),
          "an interrupted multi-file commit is recoverable: " + error);
    Check(Read(replaced) == "previous-project" && !fs::exists(created) &&
              Read(removed) == "previous-timeline" &&
              !fs::exists(interrupted / ".cutmachine-transaction") &&
              !fs::exists(created.string() + nonce + ".tmp"),
          "recovery restores the complete previous generation and cleans "
          "transaction files");
    Write(replaced.string() + nonce + ".bak", "stale-backup");
    Check(RecoverTextArtifactTransaction(interrupted.string(), error) &&
              !fs::exists(replaced.string() + nonce + ".bak"),
          "leftovers from an already committed generation are cleaned");

    fs::remove_all(root);
    if (failures) {
        std::cerr << failures << " assertion(s) failed\n";
        return 1;
    }
    std::cout << "project storage tests passed\n";
    return 0;
}
