#include "MotionCli.h"

#include "CaptionRender.h"
#include "Cli.h"
#include "CliCommands.h"
#include "FontCatalog.h"
#include "Json.h"
#include "MotionAfterExport.h"
#include "MotionClip.h"
#include "MotionComposition.h"
#include "MotionDocument.h"
#include "MotionEvaluate.h"
#include "MotionFrameRenderer.h"
#include "MotionHumanize.h"
#include "MotionRender.h"
#include "MotionSafeArea.h"
#include "MotionVideoRender.h"
#include "Project.h"
#include "ProjectStorage.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>

namespace {

struct Request {
    std::string composition_path;
    int64_t frame = 0;
    std::string image_path;
    mcp_json::Value props;
    std::vector<std::string> font_directories;
    // M3 -- set: composition_path is a project, and this names one of its
    // compositions.
    std::string composition_id;
    // M6 -- a version and a format the composition declares.
    std::string variant;
    std::string format;
    // M4 -- set: composition_path is a project, this names a motion clip of
    // one of its timelines, and `frame` is a frame of that timeline.
    std::string clip_id;
};

int Fail(const MotionError& error, std::string& output) {
    const std::string where = error.path.empty() ? "" : error.path + ": ";
    return FailCliCommand(error.code, where + error.message, output, 1);
}

// PRD39 -- ROADMAP.md. A command line refused, by the repository's rule:
// status 2, where the other refusals are 1. These commands answered 1 or 2
// depending on which of them refused. main() appends the command's usage
// from the table (WithCliUsage).
int FailUsage(const MotionError& error, std::string& output) {
    const std::string where = error.path.empty() ? "" : error.path + ": ";
    return FailCliCommand(error.code, where + error.message, output, 2);
}

int FailUsage(const std::string& detail, std::string& output) {
    return FailCliCommand("ValidationFailed", detail, output, 2);
}

// PRD39 -- a command's entry in the table of commands (CliCommands.h): its
// refusals are worded from its usage there, where this file kept usage
// lines of its own, out of step with the table and with --help.
const CliCommand& MotionCliCommand(const char* name) {
    static const CliCommand kUnlisted{"", "", "", CliSection::kMotion};
    const CliCommand* command = FindCliCommand(name);
    return command != nullptr ? *command : kUnlisted;
}

bool ParseRequest(const CliCommand& command,
                  const std::vector<std::string>& arguments, bool render,
                  Request& request, MotionError& error) {
    const size_t positional = render ? 3 : 2;
    if (arguments.size() < positional) {
        error = {"ValidationFailed", "",
                 CliArgumentsMismatch(command, arguments)};
        return false;
    }
    request.composition_path = arguments[0];
    const std::string& frame = arguments[1];
    const auto parsed = std::from_chars(
        frame.data(), frame.data() + frame.size(), request.frame);
    if (parsed.ec != std::errc{} || parsed.ptr != frame.data() + frame.size()) {
        error = {"ValidationFailed", "frame",
                 "frame must be an integer, got '" + frame + "'"};
        return false;
    }
    if (render) {
        request.image_path = arguments[2];
    }
    for (size_t index = positional; index < arguments.size(); ++index) {
        const std::string& flag = arguments[index];
        if (index + 1 >= arguments.size()) {
            error = {"ValidationFailed", "",
                     CliOptionRefusal(command, arguments, index)};
            return false;
        }
        const std::string& value = arguments[++index];
        if (flag == "--props") {
            std::string message;
            if (!mcp_json::Value::Parse(value, request.props, message) ||
                !request.props.IsObject()) {
                error = {"ValidationFailed", "props",
                         "--props must be a JSON object: " + message};
                return false;
            }
        } else if (flag == "--font-dir") {
            request.font_directories.push_back(value);
        } else if (flag == "--composition" && request.composition_id.empty()) {
            request.composition_id = value;
        } else if (flag == "--clip" && request.clip_id.empty()) {
            request.clip_id = value;
        } else if (flag == "--variant" && request.variant.empty()) {
            request.variant = value;
        } else if (flag == "--format" && request.format.empty()) {
            request.format = value;
        } else {
            // The value is read: the option sits just before it.
            error = {"ValidationFailed", "",
                     CliOptionRefusal(command, arguments, index - 1)};
            return false;
        }
    }
    // M4 -- a clip names its composition and its props. PRD39 -- refused
    // here, with the command line, rather than once the fonts were
    // registered and the project read.
    if (!request.clip_id.empty() &&
        (!request.composition_id.empty() || !request.props.IsNull() ||
         !request.variant.empty() || !request.format.empty())) {
        error = {"ValidationFailed", "--clip",
                 "--clip names its composition and its props: leave out "
                 "--composition, --props, --variant and --format"};
        return false;
    }
    return true;
}

bool RegisterFonts(const std::vector<std::string>& directories,
                   MotionError& error) {
    for (const std::string& directory : directories) {
        if (!RegisterMotionFontDirectory(directory, error)) {
            return false;
        }
    }
    return true;
}

// M3 -- a composition of a stored project, and the package its relative
// image sources resolve against. REM2 -- and what it reaches there: the
// project's media and the compositions its instances show.
bool LoadStoredComposition(const std::string& projectPath,
                           const std::string& compositionId,
                           DocumentComposition& composition,
                           std::string& mediaDirectory, MotionReach& reach,
                           MotionError& error) {
    Project project;
    std::string message;
    if (!LoadStoredProject(projectPath, project, message)) {
        error = {"ParseError", "project", message};
        return false;
    }
    for (const DocumentComposition& candidate : project.compositions) {
        if (candidate.id == compositionId) {
            composition = candidate;
            mediaDirectory =
                std::filesystem::absolute(projectPath).parent_path().string();
            return MotionReachFor(project.compositions, project.sources,
                                  project.rushes, compositionId, reach, error);
        }
    }
    error = {"UnknownComposition", "composition",
             "unknown composition_id '" + compositionId + "'"};
    return false;
}

// M6 -- the version and the format resolved first: the format's size, the
// props both set, then the caller's overrides.
bool EvaluateFrame(const MotionComposition& composition,
                   const std::string& variant, const std::string& format,
                   const mcp_json::Value& props, int64_t frame,
                   const std::string& mediaDirectory, const MotionReach& reach,
                   std::unique_ptr<MotionCoreTextMeasurer>& measurer,
                   MotionFrame& output, MotionError& error) {
    MotionComposition resolved;
    mcp_json::Value merged;
    if (!ResolveMotionVariant(composition, variant, format, props, resolved,
                              merged, error)) {
        return false;
    }
    measurer = std::make_unique<MotionCoreTextMeasurer>(mediaDirectory);
    measurer->SetMediaPaths(reach.media_paths);
    return EvaluateMotionFrame(resolved, merged, frame, *measurer,
                               reach.Context(), output, error);
}

// A composition from a file, or (M3) from a project, and the directory its
// relative image sources resolve against.
bool LoadComposition(const std::string& path, const std::string& compositionId,
                     MotionComposition& composition,
                     std::string& mediaDirectory, MotionReach& reach,
                     MotionError& error) {
    if (!compositionId.empty()) {
        DocumentComposition stored;
        return LoadStoredComposition(path, compositionId, stored,
                                     mediaDirectory, reach, error) &&
               ParseDocumentComposition(stored, composition, error);
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = {"IoError", "composition", "cannot read '" + path + "'"};
        return false;
    }
    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad()) {
        error = {"IoError", "composition",
                 "cannot finish reading '" + path + "'"};
        return false;
    }
    if (!ParseMotionComposition(contents.str(), composition, error)) {
        return false;
    }
    std::error_code status;
    const std::filesystem::path directory =
        std::filesystem::absolute(path, status).parent_path();
    if (status) {
        error = {"IoError", "composition", status.message()};
        return false;
    }
    mediaDirectory = directory.string();
    return true;
}

// M4 -- what a motion clip shows at a frame of its timeline, at the
// sequence's rate: the frame Timeline resolves and the export renders.
bool EvaluateClip(const Request& request,
                  std::unique_ptr<MotionCoreTextMeasurer>& measurer,
                  MotionFrame& frame, MotionError& error) {
    // ParseRequest has refused a clip given with a composition or props.
    Project project;
    std::string message;
    if (!LoadStoredProject(request.composition_path, project, message)) {
        error = {"ParseError", "project", message};
        return false;
    }
    for (const DocumentSequence& timeline : project.timelines) {
        const Document document = project.MakeDocument(timeline.id);
        if (document.FindClip(request.clip_id) == nullptr) {
            continue;
        }
        const RationalTime time{request.frame * timeline.frame_rate.den,
                                timeline.frame_rate.num};
        MotionClipInstant instant;
        std::string errorName;
        if (!MotionClipAt(document, request.clip_id, time, instant, errorName,
                          message)) {
            error = {errorName, "clip", message};
            return false;
        }
        MotionReach reach;
        return MotionReachFor(document, instant.composition->id, reach,
                              error) &&
               EvaluateFrame(instant.parsed, std::string(), std::string(),
                             instant.clip->motion_props, instant.frame,
                             std::filesystem::absolute(request.composition_path)
                                 .parent_path()
                                 .string(),
                             reach, measurer, frame, error);
    }
    error = {
        "UnknownClip", "clip",
        "no timeline of the project holds clip_id '" + request.clip_id + "'"};
    return false;
}

bool Evaluate(const Request& request,
              std::unique_ptr<MotionCoreTextMeasurer>& measurer,
              MotionFrame& frame, MotionError& error) {
    if (!request.clip_id.empty()) {
        return RegisterFonts(request.font_directories, error) &&
               EvaluateClip(request, measurer, frame, error);
    }
    MotionComposition composition;
    std::string mediaDirectory;
    MotionReach reach;
    return RegisterFonts(request.font_directories, error) &&
           LoadComposition(request.composition_path, request.composition_id,
                           composition, mediaDirectory, reach, error) &&
           EvaluateFrame(composition, request.variant, request.format,
                         request.props, request.frame, mediaDirectory, reach,
                         measurer, frame, error);
}

// M6 -- "a,b,c", as names never hold a comma.
std::vector<std::string> SplitNames(const std::string& list) {
    std::vector<std::string> names;
    size_t start = 0;
    while (true) {
        const size_t comma = list.find(',', start);
        names.push_back(list.substr(start, comma == std::string::npos
                                               ? std::string::npos
                                               : comma - start));
        if (comma == std::string::npos) {
            return names;
        }
        start = comma + 1;
    }
}

// M3 -- what render_motion_frame returns: the pixels and the geometry of one
// evaluation.
bool RenderFrameImage(const MotionFrameRequest& request,
                      MotionFrameImage& image, MotionError& error) {
    if (request.composition == nullptr) {
        error = {"InvalidValue", "composition", "no composition to render"};
        return false;
    }
    std::unique_ptr<MotionCoreTextMeasurer> measurer;
    MotionFrame frame;
    MotionRaster raster;
    if (!RegisterFonts(request.font_directories, error) ||
        !EvaluateFrame(*request.composition, request.variant, request.format,
                       request.props, request.frame, request.media_directory,
                       request.reach, measurer, frame, error)) {
        return false;
    }
    ScaleMotionFrame(frame, request.scale);
    measurer->SetMediaTurns(request.media_turns, request.media_turn);
    if (request.raster &&
        (!RenderMotionFrame(frame, *measurer, raster, error) ||
         !EncodeMotionPng(raster, image.png, error))) {
        return false;
    }
    image.geometry = DumpMotionFrame(frame);
    image.width = request.raster ? raster.width : frame.width;
    image.height = request.raster ? raster.height : frame.height;
    image.evaluated = std::move(frame);
    return true;
}

}  // namespace

int DescribeMotionFrameCommand(const std::vector<std::string>& arguments,
                               std::string& output) {
    Request request;
    MotionError error;
    if (!ParseRequest(MotionCliCommand("--describe-motion-frame"), arguments,
                      false, request, error)) {
        return FailUsage(error, output);
    }
    std::unique_ptr<MotionCoreTextMeasurer> measurer;
    MotionFrame frame;
    if (!Evaluate(request, measurer, frame, error)) {
        return Fail(error, output);
    }
    output = DumpMotionFrame(frame) + "\n";
    return 0;
}

int RenderMotionFrameCommand(const std::vector<std::string>& arguments,
                             std::string& output) {
    Request request;
    MotionError error;
    if (!ParseRequest(MotionCliCommand("--render-motion-frame"), arguments,
                      true, request, error)) {
        return FailUsage(error, output);
    }
    std::unique_ptr<MotionCoreTextMeasurer> measurer;
    MotionFrame frame;
    MotionRaster raster;
    if (!Evaluate(request, measurer, frame, error) ||
        !RenderMotionFrame(frame, *measurer, raster, error) ||
        !WriteMotionPng(raster, request.image_path, error)) {
        return Fail(error, output);
    }
    output = "{\"ok\":true,\"image_path\":\"" +
             mcp_json::EscapeJsonString(request.image_path) +
             "\",\"width\":" + std::to_string(raster.width) +
             ",\"height\":" + std::to_string(raster.height) +
             ",\"frame\":" + std::to_string(frame.frame) + "}\n";
    return 0;
}

int RenderMotionVideoCommand(const std::vector<std::string>& arguments,
                             std::string& output) {
    const CliCommand& command = MotionCliCommand("--render-motion-video");
    if (arguments.size() < 2) {
        return FailUsage(CliArgumentsMismatch(command, arguments), output);
    }
    MotionVideoRequest request;
    request.options.output = arguments[1];
    std::string compositionId;
    bool variantsGiven = false;
    bool formatsGiven = false;
    for (size_t index = 2; index < arguments.size(); ++index) {
        const std::string& flag = arguments[index];
        if (flag == "--alpha") {
            request.options.alpha = true;
            continue;
        }
        if (flag == "--overwrite") {
            request.options.overwrite = true;
            continue;
        }
        if (flag == "--preview") {
            request.options.preview = true;
            continue;
        }
        if (index + 1 >= arguments.size()) {
            return FailUsage(CliOptionRefusal(command, arguments, index),
                             output);
        }
        const std::string& value = arguments[++index];
        if (flag == "--composition" && compositionId.empty()) {
            compositionId = value;
        } else if (flag == "--variants" && !variantsGiven) {
            request.options.variants = SplitNames(value);
            variantsGiven = true;
        } else if (flag == "--formats" && !formatsGiven) {
            request.options.formats = SplitNames(value);
            formatsGiven = true;
        } else if ((flag == "--loudness" &&
                    !request.options.loudness.has_value()) ||
                   flag == "--true-peak") {
            // MM1 -- a delivery loudness, LUFS, and its true-peak ceiling.
            double number = 0.0;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), number);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size()) {
                std::string detail = flag;
                detail += " must be a number, got '";
                detail += value;
                detail += "'";
                return FailUsage(detail, output);
            }
            if (flag == "--loudness") {
                request.options.loudness = number;
            } else {
                request.options.true_peak = number;
            }
        } else if (flag == "--font-dir") {
            request.font_directories.push_back(value);
        } else {
            // The value is read: the option sits just before it.
            return FailUsage(CliOptionRefusal(command, arguments, index - 1),
                             output);
        }
    }
    MotionComposition composition;
    MotionError error;
    if (!LoadComposition(arguments[0], compositionId, composition,
                         request.media_directory, request.reach, error)) {
        return Fail(error, output);
    }
    request.composition = &composition;
    std::string report;
    if (!RenderMotionVideos(request, report, error)) {
        const std::string where = error.path.empty() ? "" : error.path + ": ";
        return FailCliCommandWithResult(error.code, where + error.message,
                                        report, output, 1);
    }
    output = report + "\n";
    return 0;
}

int DescribeCompositionsCommand(const std::vector<std::string>& arguments,
                                std::string& output) {
    const CliCommand& command = MotionCliCommand("--describe-compositions");
    if (arguments.empty()) {
        return FailUsage(CliArgumentsMismatch(command, arguments), output);
    }
    if (arguments.size() > 2) {
        return FailUsage(CliOptionRefusal(command, arguments, 2), output);
    }
    Project project;
    std::string message;
    if (!LoadStoredProject(arguments[0], project, message)) {
        return FailCliCommand("ParseError", message, output, 1);
    }
    if (arguments.size() == 1) {
        output = MotionCompositionListJson(project.compositions) + "\n";
        return 0;
    }
    for (const DocumentComposition& composition : project.compositions) {
        if (composition.id == arguments[1]) {
            output = MotionCompositionJson(composition) + "\n";
            return 0;
        }
    }
    return FailCliCommand("UnknownComposition",
                          "unknown composition_id '" + arguments[1] + "'",
                          output, 1);
}

namespace {

// MA1 -- the describer the executable installs (MotionAfterExport.h): the
// fonts registered, text measured by Core Text, images and instances
// reached through the project.
bool DescribeMotionForAfter(const MotionAfterRequest& request,
                            mcp_json::Value& description,
                            MotionAfterExportReport& report,
                            MotionError& error) {
    if (request.composition == nullptr) {
        error = {"InvalidComposition", "", "no composition to describe"};
        return false;
    }
    if (!RegisterFonts(request.font_directories, error)) {
        return false;
    }
    MotionCoreTextMeasurer measurer(request.options.media_directory);
    measurer.SetMediaPaths(request.reach.media_paths);
    // MA11 -- a video or an audio node naming a project media by ULID.
    MotionAfterExportOptions options = request.options;
    options.media_paths = request.reach.media_paths;
    return ExportMotionToAfter(*request.composition, request.props, measurer,
                               request.reach.Context(), options, description,
                               report, error);
}

// FI15 -- docs/roadmap/FIABILITE-2026-10.md. An output naming the file the
// command reads: written over it, a composition -- or a whole project --
// became what was derived from it, and the command answered ok.
bool RefuseOutputOverSource(const std::string& source,
                            const std::string& destination,
                            std::string& output) {
    std::error_code error;
    if (!std::filesystem::equivalent(source, destination, error) || error) {
        return false;
    }
    FailCliCommand("OutputExists",
                   "refusing to write over the source '" + destination +
                       "': name another output",
                   output, 1);
    return true;
}

}  // namespace

// MOTION-AFTER-2026-09 -- the composition described for After Effects
// (MotionAfterExport.h): the file tools/after-bridge/scripts/
// motion_import.jsx builds a project from.
int ExportMotionAfterCommand(const std::vector<std::string>& arguments,
                             std::string& output) {
    const CliCommand& command = MotionCliCommand("--export-motion-after");
    if (arguments.size() < 2) {
        return FailUsage(CliArgumentsMismatch(command, arguments), output);
    }
    std::vector<std::string> rest{arguments[0], "0"};
    MotionAfterExportOptions options;
    std::string timelineId;
    for (size_t index = 2; index < arguments.size(); ++index) {
        if ((arguments[index] == "--aep" || arguments[index] == "--name" ||
             arguments[index] == "--timeline") &&
            index + 1 < arguments.size()) {
            (arguments[index] == "--aep"    ? options.aep_path
             : arguments[index] == "--name" ? options.name
                                            : timelineId) =
                arguments[index + 1];
            ++index;
            continue;
        }
        if (arguments[index] == "--boil" && index + 1 < arguments.size()) {
            options.boil.push_back(arguments[index + 1]);
            ++index;
            continue;
        }
        rest.push_back(arguments[index]);
    }
    Request request;
    MotionError error;
    // `rest` is the command line with "0" in place of the description path
    // and without the options read above; what it refuses is refused in
    // this command's name.
    if (!ParseRequest(command, rest, false, request, error)) {
        return FailUsage(error, output);
    }
    if (RefuseOutputOverSource(arguments[0], arguments[1], output)) {
        return 1;
    }
    if (!RegisterFonts(request.font_directories, error)) {
        return Fail(error, output);
    }
    MotionComposition composition;
    MotionReach reach;
    if (!LoadComposition(request.composition_path, request.composition_id,
                         composition, options.media_directory, reach, error)) {
        return Fail(error, output);
    }
    if (options.aep_path.empty()) {
        options.aep_path = std::filesystem::path(arguments[1])
                               .replace_extension(".aep")
                               .string();
    }
    options.aep_path = std::filesystem::absolute(options.aep_path).string();
    if (!timelineId.empty()) {
        // The sounds of that timeline, as its audio tracks play them.
        Project project;
        std::string message;
        if (!LoadStoredProject(request.composition_path, project, message)) {
            return FailCliCommand("ParseError", message, output, 1);
        }
        bool found = false;
        for (const DocumentSequence& timeline : project.timelines) {
            if (timeline.id == timelineId) {
                found = true;
                if (!MotionAfterTimelineAudio(
                        project.MakeDocument(timeline.id),
                        std::filesystem::absolute(request.composition_path)
                            .parent_path()
                            .string(),
                        options.audio, error)) {
                    return Fail(error, output);
                }
            }
        }
        if (!found) {
            return FailCliCommand("UnknownTimeline",
                                  "unknown timeline_id '" + timelineId + "'",
                                  output, 1);
        }
    }
    MotionAfterRequest describe;
    describe.composition = &composition;
    describe.props = request.props;
    describe.reach = reach;
    describe.options = options;
    mcp_json::Value description;
    MotionAfterExportReport report;
    if (!DescribeMotionForAfter(describe, description, report, error)) {
        return Fail(error, output);
    }
    std::ofstream file(arguments[1], std::ios::binary | std::ios::trunc);
    file << description.Dump();
    file.close();
    if (!file) {
        return FailCliCommand("IoError", "cannot write '" + arguments[1] + "'",
                              output, 1);
    }
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(true));
    result.Set("path", mcp_json::Value::MakeString(
                           std::filesystem::absolute(arguments[1]).string()));
    result.Set("aep_path", mcp_json::Value::MakeString(options.aep_path));
    MotionAfterReportFields(report, result);
    output = result.Dump() + "\n";
    return 0;
}

// MA6 -- the pass with Core Text, installed for the surfaces of the model.
bool HumanizeWithCoreText(const MotionHumanizeRequest& request,
                          mcp_json::Value& output, MotionHumanizeReport& report,
                          MotionError& error) {
    if (request.definition == nullptr ||
        !RegisterFonts(request.font_directories, error)) {
        if (error.code.empty()) {
            error = {"InvalidValue", "", "no definition to humanize"};
        }
        return false;
    }
    MotionCoreTextMeasurer measurer(request.media_directory);
    measurer.SetMediaPaths(request.reach.media_paths);
    return HumanizeMotionDefinition(*request.definition, measurer,
                                    request.reach.Context(), output, report,
                                    error);
}

// MA6 -- docs/roadmap/MOTION-AFTER-2026-09.md. A composition rewritten at
// a human's level (MotionHumanize.h) and proved before it is written: every
// frame of both rendered, refused when one differs. From a project the
// definition keeps its identities; it enters the project as an
// update_motion_composition, which humanize_motion_composition applies.
int HumanizeMotionCommand(const std::vector<std::string>& arguments,
                          std::string& output) {
    const CliCommand& command = MotionCliCommand("--humanize-motion");
    if (arguments.size() < 2) {
        return FailUsage(CliArgumentsMismatch(command, arguments), output);
    }
    std::string compositionId;
    std::vector<std::string> fonts;
    for (size_t index = 2; index < arguments.size(); ++index) {
        if (index + 1 >= arguments.size() ||
            (arguments[index] != "--composition" &&
             arguments[index] != "--font-dir")) {
            return FailUsage(CliOptionRefusal(command, arguments, index),
                             output);
        }
        (arguments[index] == "--composition" ? compositionId
                                             : fonts.emplace_back()) =
            arguments[index + 1];
        ++index;
    }
    if (RefuseOutputOverSource(arguments[0], arguments[1], output)) {
        return 1;
    }
    MotionError error;
    mcp_json::Value definition;
    std::string mediaDirectory;
    MotionReach reach;
    if (!compositionId.empty()) {
        DocumentComposition stored;
        if (!LoadStoredComposition(arguments[0], compositionId, stored,
                                   mediaDirectory, reach, error)) {
            return Fail(error, output);
        }
        definition = stored.definition;
    } else {
        std::ifstream input(arguments[0], std::ios::binary);
        std::ostringstream contents;
        contents << input.rdbuf();
        std::string message;
        if (!input ||
            !mcp_json::Value::Parse(contents.str(), definition, message)) {
            return FailCliCommand(
                "ParseError", "cannot read '" + arguments[0] + "' " + message,
                output, 1);
        }
        mediaDirectory =
            std::filesystem::absolute(arguments[0]).parent_path().string();
    }
    MotionHumanizeRequest request;
    request.definition = &definition;
    request.reach = reach;
    request.media_directory = mediaDirectory;
    request.font_directories = fonts;
    mcp_json::Value humanized;
    MotionHumanizeReport report;
    MotionComposition before;
    MotionComposition after;
    if (!HumanizeWithCoreText(request, humanized, report, error) ||
        !ParseMotionComposition(definition, before, error) ||
        !ParseMotionComposition(humanized, after, error)) {
        return Fail(error, output);
    }
    MotionFrameRequest base;
    base.media_directory = mediaDirectory;
    base.font_directories = fonts;
    base.reach = reach;
    std::vector<int64_t> differing;
    if (!CompareMotionRenders(base, before, after, differing, error)) {
        return Fail(error, output);
    }
    if (!differing.empty()) {
        std::string frames;
        for (size_t index = 0; index < differing.size() && index < 12;
             ++index) {
            frames +=
                (index > 0 ? ", " : "") + std::to_string(differing[index]);
        }
        return FailCliCommand("NotEquivalent",
                              std::to_string(differing.size()) +
                                  " frame(s) would change, first " + frames +
                                  ": nothing written",
                              output, 1);
    }
    std::ofstream file(arguments[1], std::ios::binary | std::ios::trunc);
    file << humanized.Dump();
    file.close();
    if (!file) {
        return FailCliCommand("IoError", "cannot write '" + arguments[1] + "'",
                              output, 1);
    }
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(true));
    result.Set("path", mcp_json::Value::MakeString(
                           std::filesystem::absolute(arguments[1]).string()));
    MotionHumanizeReportFields(report, result);
    result.Set("frames_compared", mcp_json::Value::MakeInt(before.duration));
    output = result.Dump() + "\n";
    return 0;
}

// AX-2026-09 AX6 -- the font lister of the model's surfaces (FontCatalog.h):
// the directories registered first, as a render registers them.
bool ListFontsWithCoreText(const std::vector<std::string>& directories,
                           std::vector<std::string>& families,
                           MotionError& error) {
    return RegisterFonts(directories, error) &&
           ListMotionFontFamilies(families, error);
}

// AX6 -- the families motion can name, and those a caption would export in
// another font: list_fonts's answer, with "ok" first.
int ListFontsCommand(const std::vector<std::string>& arguments,
                     std::string& output) {
    const CliCommand& command = MotionCliCommand("--list-fonts");
    std::string query;
    bool queried = false;
    std::vector<std::string> fonts;
    for (size_t index = 0; index < arguments.size(); ++index) {
        const bool known = arguments[index] == "--font-dir" ||
                           (arguments[index] == "--query" && !queried);
        if (!known || index + 1 >= arguments.size()) {
            return FailUsage(CliOptionRefusal(command, arguments, index),
                             output);
        }
        if (arguments[index] == "--query") {
            query = arguments[index + 1];
            queried = true;
        } else {
            fonts.push_back(arguments[index + 1]);
        }
        ++index;
    }
    MotionError error;
    std::vector<std::string> families;
    if (!ListFontsWithCoreText(fonts, families, error)) {
        return Fail(error, output);
    }
    const mcp_json::Value listing = FontCatalogJson(
        BuildFontCatalog(families, ReadCaptionFontFinder(), query));
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(true));
    for (const auto& member : listing.AsObject()) {
        result.Set(member.first, member.second);
    }
    output = result.Dump() + "\n";
    return 0;
}

// MM3 -- docs/roadmap/MOTION-MARQUE-2026-10.md. Every frame of a composition
// rendered alone, in a version and a format, against a safe zone: what
// check_motion_safe_area does for a composition_id. As --check-timeline,
// "ok" is the verdict, and so is the exit status.
int CheckMotionSafeAreaCommand(const std::vector<std::string>& arguments,
                               std::string& output) {
    const CliCommand& command = MotionCliCommand("--check-motion-safe-area");
    if (arguments.empty()) {
        return FailUsage(CliArgumentsMismatch(command, arguments), output);
    }
    std::string compositionId;
    std::string variant;
    std::string format;
    mcp_json::Value props;
    std::vector<std::string> fonts;
    MotionSafeZone zone = DefaultMotionSafeZone();
    bool named = false;
    bool margined = false;
    bool atRest = false;
    for (size_t index = 1; index < arguments.size(); ++index) {
        const std::string& flag = arguments[index];
        if (flag == "--at-rest") {
            atRest = true;
            continue;
        }
        if (index + 1 >= arguments.size()) {
            return FailUsage(CliOptionRefusal(command, arguments, index),
                             output);
        }
        const std::string& value = arguments[++index];
        if (flag == "--composition" && compositionId.empty()) {
            compositionId = value;
        } else if (flag == "--variant" && variant.empty()) {
            variant = value;
        } else if (flag == "--format" && format.empty()) {
            format = value;
        } else if (flag == "--font-dir") {
            fonts.push_back(value);
        } else if (flag == "--props" && props.IsNull()) {
            std::string message;
            if (!mcp_json::Value::Parse(value, props, message) ||
                !props.IsObject()) {
                return FailUsage("--props must be a JSON object: " + message,
                                 output);
            }
        } else if (flag == "--zone" && !named) {
            if (!NamedMotionSafeZone(value, zone)) {
                return FailUsage(
                    "--zone must be \"default\" or \"social_vertical\"",
                    output);
            }
            named = true;
        } else if (flag == "--margin-percent" && !margined) {
            int64_t margin = -1;
            const auto parsed = std::from_chars(
                value.data(), value.data() + value.size(), margin);
            if (parsed.ec != std::errc{} ||
                parsed.ptr != value.data() + value.size() || margin < 0 ||
                margin > 25) {
                return FailUsage(
                    "--margin-percent must be an integer from 0 to 25", output);
            }
            zone = DefaultMotionSafeZone(margin);
            margined = true;
        } else {
            return FailUsage(CliOptionRefusal(command, arguments, index - 1),
                             output);
        }
    }
    if (named && margined) {
        return FailUsage(
            "--margin-percent sets the default zone's margin: give it "
            "without --zone",
            output);
    }
    MotionComposition composition;
    std::string mediaDirectory;
    MotionReach reach;
    MotionComposition resolved;
    mcp_json::Value merged;
    MotionError error;
    if (!RegisterFonts(fonts, error) ||
        !LoadComposition(arguments[0], compositionId, composition,
                         mediaDirectory, reach, error) ||
        !ResolveMotionVariant(composition, variant, format, props, resolved,
                              merged, error)) {
        return Fail(error, output);
    }
    // The check reads each frame itself, never its shutter's instants.
    resolved.motion_blur_samples = 0;
    MotionCoreTextMeasurer measurer(mediaDirectory);
    measurer.SetMediaPaths(reach.media_paths);
    MotionSafeAreaReport report;
    report.zone = zone;
    report.at_rest_only = atRest;
    for (int64_t index = 0; index < resolved.duration; ++index) {
        MotionFrame frame;
        if (!EvaluateMotionFrame(resolved, merged, index, measurer,
                                 reach.Context(), frame, error)) {
            return Fail(error, output);
        }
        CheckMotionFrameSafeArea(frame, report);
    }
    mcp_json::Value checked;
    std::string message;
    mcp_json::Value::Parse(SerializeMotionSafeAreaReport(report), checked,
                           message);
    mcp_json::Value result = mcp_json::Value::MakeObject();
    result.Set("ok", mcp_json::Value::MakeBool(report.issues.empty()));
    if (!compositionId.empty()) {
        result.Set("composition_id",
                   mcp_json::Value::MakeString(compositionId));
    }
    result.Set("variant", variant.empty()
                              ? mcp_json::Value::MakeNull()
                              : mcp_json::Value::MakeString(variant));
    result.Set("format", format.empty() ? mcp_json::Value::MakeNull()
                                        : mcp_json::Value::MakeString(format));
    result.Set("width", mcp_json::Value::MakeInt(resolved.width));
    result.Set("height", mcp_json::Value::MakeInt(resolved.height));
    for (const auto& member : checked.AsObject()) {
        result.Set(member.first, member.second);
    }
    output = result.Dump() + "\n";
    return report.issues.empty() ? 0 : 1;
}

bool IsMotionCommand(const std::string& name) {
    return name == "--list-fonts" || name == "--describe-motion-frame" ||
           name == "--render-motion-frame" ||
           name == "--describe-compositions" ||
           name == "--render-motion-video" || name == "--export-motion-after" ||
           name == "--humanize-motion" || name == "--check-motion-safe-area";
}

int MotionCommand(const std::string& name,
                  const std::vector<std::string>& arguments,
                  std::string& output) {
    if (name == "--describe-motion-frame") {
        return DescribeMotionFrameCommand(arguments, output);
    }
    if (name == "--render-motion-frame") {
        return RenderMotionFrameCommand(arguments, output);
    }
    if (name == "--render-motion-video") {
        return RenderMotionVideoCommand(arguments, output);
    }
    if (name == "--export-motion-after") {
        return ExportMotionAfterCommand(arguments, output);
    }
    if (name == "--humanize-motion") {
        return HumanizeMotionCommand(arguments, output);
    }
    if (name == "--check-motion-safe-area") {
        return CheckMotionSafeAreaCommand(arguments, output);
    }
    if (name == "--list-fonts") {
        return ListFontsCommand(arguments, output);
    }
    return DescribeCompositionsCommand(arguments, output);
}

void InstallMotionFrameRenderer() {
    SetMotionFrameRenderer(&RenderFrameImage);
    // M6 -- and the renderer of whole videos, for render_motion_video.
    SetMotionVideoRenderer(&RenderMotionVideos);
    // MA1 -- and the describer of export_motion_after.
    SetMotionAfterDescriber(&DescribeMotionForAfter);
    // MA6 -- and the humanize pass, which measures as they do.
    SetMotionHumanizer(&HumanizeWithCoreText);
    // AX6 -- and the families list_fonts names.
    SetMotionFontLister(&ListFontsWithCoreText);
    // LG1 -- and the drawing of a carton, which resolves a family as a
    // composition does.
    InstallCaptionRenderer();
}
