#include "CliCommands.h"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr char kProgram[] = "cutmachine";
constexpr char kHelpName[] = "--help";
// The file that documents the commands. `--help` used to name a README
// section that did not exist ("Ligne de commande"); PRD30 then moved the
// reference out of the README, one file per section.
constexpr char kReadmeSection[] = "docs/reference/commandes-headless.md";
// A usage line this narrow leaves room for its summary beside it in
// `--help`, at this column.
constexpr size_t kSummaryColumn = 42;

const char* SectionTitle(CliSection section) {
    switch (section) {
        case CliSection::kAgents:
            return "Agents (the whole engine as tools)";
        case CliSection::kProject:
            return "Project";
        case CliSection::kAnalyses:
            return "Analyses (cached beside the project)";
        case CliSection::kColor:
            return "Colour";
        case CliSection::kEdits:
            return "Edits by intention";
        case CliSection::kMulticam:
            return "Multicam";
        case CliSection::kMotion:
            return "Motion";
        case CliSection::kDelivery:
            return "Delivery and exchange";
        case CliSection::kConnectors:
            return "After Effects";
        case CliSection::kMachine:
            return "This machine";
    }
    return "";
}

std::vector<std::string> UsageTokens(const CliCommand& command) {
    std::vector<std::string> tokens(1);
    for (const char* cursor = command.arguments; *cursor != '\0'; ++cursor) {
        if (*cursor == ' ') {
            tokens.emplace_back();
        } else {
            tokens.back().push_back(*cursor);
        }
    }
    tokens.erase(std::remove(tokens.begin(), tokens.end(), std::string()),
                 tokens.end());
    return tokens;
}

// A token of a usage line without the grouping around it: "[--timeline"
// and "--stdio)" are "--timeline" and "--stdio", "<media-id>]..." is
// "<media-id>".
std::string Bare(const std::string& token) {
    const size_t first = token.find_first_not_of("[(");
    if (first == std::string::npos) {
        return std::string();
    }
    const size_t last = token.find_last_not_of("]).");
    return last == std::string::npos || last < first
               ? std::string()
               : token.substr(first, last - first + 1);
}

// "--" and a letter: what an option looks like, in a usage line and on a
// command line alike.
bool LooksLikeOption(const std::string& text) {
    return text.size() > 2 && text[0] == '-' && text[1] == '-' &&
           text[2] >= 'a' && text[2] <= 'z';
}

struct UsageOption {
    std::string name;
    bool takes_value = false;
    // PRD39 -- its group ends with "...": "[--media <media-id>]..." may be
    // given as often as wanted, "[--timeline <id>]" once.
    bool repeats = false;
};

bool EndsWithEllipsis(const std::string& token) {
    return token.size() >= 3 && token.compare(token.size() - 3, 3, "...") == 0;
}

// The options a usage line names. One takes a value when something that is
// neither another option nor an alternative follows it inside its group:
// "[--timeline <id>]" does, "[--verbatim]" and "--stdio)" do not.
std::vector<UsageOption> UsageOptions(const CliCommand& command) {
    const std::vector<std::string> tokens = UsageTokens(command);
    std::vector<UsageOption> options;
    for (size_t index = 0; index < tokens.size(); ++index) {
        const std::string name = Bare(tokens[index]);
        if (!LooksLikeOption(name)) {
            continue;
        }
        const char closing = tokens[index].back();
        const bool closesGroup = closing == ']' || closing == ')';
        const std::string next =
            index + 1 < tokens.size() ? Bare(tokens[index + 1]) : std::string();
        const bool takesValue = !closesGroup && !next.empty() && next != "|" &&
                                !LooksLikeOption(next);
        options.push_back(
            {name, takesValue,
             EndsWithEllipsis(takesValue ? tokens[index + 1] : tokens[index])});
    }
    return options;
}

std::vector<UsageOption>::const_iterator FindUsageOption(
    const std::vector<UsageOption>& options, const std::string& name) {
    return std::find_if(
        options.begin(), options.end(),
        [&name](const UsageOption& option) { return option.name == name; });
}

// Whether the option at `arguments[index]` already came among the options
// before it, past the required arguments, read as the commands read them:
// an option the usage gives a value is followed by that value, which is
// never taken for an option.
bool GivenBefore(const std::vector<UsageOption>& options,
                 const std::vector<std::string>& arguments, size_t from,
                 size_t index) {
    for (size_t at = from; at < index && at < arguments.size(); ++at) {
        if (!LooksLikeOption(arguments[at])) {
            continue;
        }
        if (arguments[at] == arguments[index]) {
            return true;
        }
        const auto known = FindUsageOption(options, arguments[at]);
        if (known != options.end() && known->takes_value) {
            ++at;
        }
    }
    return false;
}

// Whether the usage admits nothing beyond its required arguments: no
// optional group, no alternative, nothing that repeats.
bool HasFixedArguments(const CliCommand& command) {
    const std::string usage(command.arguments);
    return usage.find_first_of("[(") == std::string::npos &&
           usage.find("...") == std::string::npos;
}

// PROFIL-2026-11 PF1 -- whether the usage admits, past its required
// arguments, options alone, each with its value: "[--profile <name>]" does,
// "[<project>] (...)" and "<corpus>..." do not. A word there that no option
// takes as its value is then an argument too many, as for a usage with
// nothing past its required arguments.
bool AdmitsOnlyOptions(const CliCommand& command) {
    const std::vector<std::string> tokens = UsageTokens(command);
    const std::vector<std::string> required = CliRequiredArguments(command);
    for (const std::string& token : required) {
        if (EndsWithEllipsis(token)) return false;
    }
    for (size_t index = required.size(); index < tokens.size(); ++index) {
        const std::string name = Bare(tokens[index]);
        if (name.empty() || name == "|" || LooksLikeOption(name)) continue;
        // A placeholder that follows an option inside its group is its value.
        const std::string& previous = tokens[index - 1];
        const bool valueOfOption =
            index > required.size() && LooksLikeOption(Bare(previous)) &&
            previous.back() != ']' && previous.back() != ')';
        if (!valueOfOption) return false;
    }
    return true;
}

// The first word past `from` that is neither an option nor the value an
// option of the usage takes; empty when there is none.
std::string FirstStrayWord(const std::vector<UsageOption>& options,
                           const std::vector<std::string>& arguments,
                           size_t from) {
    for (size_t index = from; index < arguments.size(); ++index) {
        if (!LooksLikeOption(arguments[index])) return arguments[index];
        const auto known = FindUsageOption(options, arguments[index]);
        if (known != options.end() && known->takes_value) ++index;
    }
    return std::string();
}

std::string WithoutDashes(const std::string& name) {
    const size_t first = name.find_first_not_of('-');
    return first == std::string::npos ? std::string() : name.substr(first);
}

// A name as the table would spell it: two dashes, lower case, dashes where
// an underscore was typed.
std::string Normalized(const std::string& name) {
    std::string normalized = "--" + WithoutDashes(name);
    for (char& character : normalized) {
        if (character == '_') {
            character = '-';
        } else if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return normalized;
}

// Edits between two names, a swap of neighbours counting for one (optimal
// string alignment): "--exprot" is one edit from "--export".
size_t EditDistance(const std::string& left, const std::string& right) {
    std::vector<std::vector<size_t>> cost(
        left.size() + 1, std::vector<size_t>(right.size() + 1, 0));
    for (size_t row = 0; row <= left.size(); ++row) {
        cost[row][0] = row;
    }
    for (size_t column = 0; column <= right.size(); ++column) {
        cost[0][column] = column;
    }
    for (size_t row = 1; row <= left.size(); ++row) {
        for (size_t column = 1; column <= right.size(); ++column) {
            const size_t substitution =
                left[row - 1] == right[column - 1] ? 0 : 1;
            cost[row][column] =
                std::min({cost[row - 1][column] + 1, cost[row][column - 1] + 1,
                          cost[row - 1][column - 1] + substitution});
            if (row > 1 && column > 1 && left[row - 1] == right[column - 2] &&
                left[row - 2] == right[column - 1]) {
                cost[row][column] =
                    std::min(cost[row][column], cost[row - 2][column - 2] + 1);
            }
        }
    }
    return cost[left.size()][right.size()];
}

std::string Join(const std::vector<std::string>& items, size_t from) {
    std::string joined;
    for (size_t index = from; index < items.size(); ++index) {
        if (!joined.empty()) {
            joined += ' ';
        }
        joined += items[index];
    }
    return joined;
}

bool AsksForHelp(const std::string& argument) {
    return argument == kHelpName || argument == "-h";
}

}  // namespace

const std::vector<CliCommand>& CliCommands() {
    static const std::vector<CliCommand> commands = {
        {"--tools", "[--profile <name>]",
         "the MCP tool catalog, as JSON, or what a profile serves of it",
         CliSection::kAgents},
        // PRD18 -- ROADMAP.md (ACP4). The skills the binary embeds
        // (Procedures.h), as get_procedure answers them.
        {"--procedure", "[<name> [<file>]]",
         "the editing procedures it carries", CliSection::kAgents},
        // RM3 -- docs/roadmap/REGLES-2026-10.md. The forge of the learned
        // carton boundary (CaptionBreakModel.h), as train_caption_breaks.
        {"--train-caption-breaks",
         "<corpus>... [--evaluate <corpus>]... [--install] [--folds <n>] "
         "[--epochs <n>] [--step <n>]",
         "learn where cartons end from delivered captions",
         CliSection::kAgents},
        {"--archive-project", "<project> <database> [<context-json>]",
         "archive complete canonical project JSON in local SQLite",
         CliSection::kAgents},
        {"--query-project-archive", "<database> '<sql>' [<options-json>]",
         "read-only SQL/JSON query over archived projects",
         CliSection::kAgents},
        {"--project-archive-status", "<project>",
         "inspect pending automatic SQL captures", CliSection::kAgents},
        {"--sync-project-archive", "<project>",
         "retry pending SQL captures without editing", CliSection::kAgents},
        {"--import-project-archive", "<database> <project>...",
         "import up to 100 existing projects into SQLite", CliSection::kAgents},
        // DEC44 -- docs/roadmap/DONNEES-2026-10.md.
        {"--backup-project-archive", "",
         "copy the local SQL archive into CUTMACHINE_ARCHIVE_BACKUP_DIR now",
         CliSection::kAgents},
        {"--commit-run-step", "<project> '<request-json>'",
         "commit a montage step and its durable receipt", CliSection::kAgents},
        {"--get-run-step", "<project> <step-id>",
         "compare a saved run step with project and journals",
         CliSection::kAgents},
        {"--call-tool", "<project> <tool> [<json>]",
         "call one tool without a server", CliSection::kAgents},
        {"--mcp-serve",
         "[<project>] (--port <n> [--token <t>] | --stdio) "
         "[--require-timeline] [--profile <name>]",
         "MCP server over loopback HTTP, or over stdin/stdout for the client "
         "that starts it (.mcp.json); without a project it answers "
         "create_project and open_project; --profile motion, itw or reel "
         "serves the tools of that trade only",
         CliSection::kAgents},
        {"--prepare-edit-plan", "<project> '<json>' <plan-file>",
         "run an edit plan's steps on a copy and keep the plan in a file",
         CliSection::kAgents},
        {"--apply-edit-plan", "<project> <plan-file>",
         "apply a prepared plan as one undo step", CliSection::kAgents},
        {"--discard-edit-plan", "<plan-file>", "delete a plan file",
         CliSection::kAgents},

        {"--create-project", "<folder> <name> [--format WxH@fps]",
         "create a project package with one timeline", CliSection::kProject},
        {"--ingest",
         "<project> <file-or-folder> [--recursive] [--bin <bin-id>]",
         "add media to the library, into a bin, as one undo step",
         CliSection::kProject},
        {"--describe", "<project> [--timeline <id>]",
         "the project as JSON: timeline, library, bins, markers",
         CliSection::kProject},
        {"--propose-sequence", "<project>",
         "the sequence format the rushes imply", CliSection::kProject},
        {"--apply-op", "<project> '<operation-json>' ['<motif-json>']",
         "apply one timeline operation, or a Batch of them, with why",
         CliSection::kProject},
        {"--apply-project-op", "<project> '<operation-json>' ['<motif-json>']",
         "apply one project operation, or an array of them as one step, with "
         "why",
         CliSection::kProject},
        {"--undo-project-op", "<project>", "undo the last project operation",
         CliSection::kProject},
        {"--redo-project-op", "<project>", "redo the one just undone",
         CliSection::kProject},

        {"--transcribe",
         "<project> <media-id>[,<media-id>...] [<model>] [<language>] "
         "[--verbatim] [--include-silent] [--overwrite]",
         "transcribe rushes with the local Whisper model, into the cache",
         CliSection::kAnalyses},
        {"--transcribe-timeline",
         "<project> [--timeline <id>] [--language <code>] [--verbatim]",
         "transcribe the sound the timeline plays", CliSection::kAnalyses},
        {"--align-words", "<project> [<media-id>...] [--texts '<json>']",
         "MFA's word boundaries for the rushes named, or for every "
         "transcribed rush",
         CliSection::kAnalyses},
        {"--align-transcripts", "<project> [--write]",
         "report the word boundaries the speech envelope contradicts; "
         "--write installs the corrections",
         CliSection::kAnalyses},
        {"--speech-onset", "<project> <media-id>",
         "measure where speech starts and stops in a rush",
         CliSection::kAnalyses},
        {"--speech-onset-report", "<project>",
         "the measured head trims, in frames", CliSection::kAnalyses},
        {"--sync-audio",
         "<project> <picture-clip-id> <recorder-media-id> <track-id> "
         "[--offset <value>/<rate>] [--picture-channel mix|left|right] "
         "[--recorder-channel mix|left|right] [--keep-camera-sound]",
         "lay a recorder's sound under a picture clip, measured and linked, "
         "in one undo",
         CliSection::kEdits},
        {"--sync-audio-clip",
         "<project> <picture-clip-id> <sound-clip-id> "
         "[--offset <value>/<rate>] [--picture-channel mix|left|right] "
         "[--recorder-channel mix|left|right] [--keep-camera-sound]",
         "slide a recorder's sound already on the timeline into sync with a "
         "picture clip and link it, in one undo",
         CliSection::kEdits},
        {"--measure-audio-offset",
         "<project> <reference-media-id> <other-media-id> "
         "[--reference-channel mix|left|right] "
         "[--other-channel mix|left|right]",
         "how far one rush's sound sits from another's, to the sample; "
         "read only",
         CliSection::kAnalyses},
        {"--propose-ducking",
         "<project> <media-id> [--channel mix|left|right] "
         "[--threshold-db <n>] [--hysteresis-db <n>] "
         "[--floor-percentile <n>] [--minimum-speech-ms <n>] "
         "[--bridge-ms <n>] [--depth-db <n>] [--attack-ms <n>] "
         "[--release-ms <n>]",
         "where a rush's voice speaks, and the gain a music under it would "
         "follow; read only",
         CliSection::kAnalyses},
        {"--shot-quality", "<project> <media-id>",
         "measure the picture quality of a rush", CliSection::kAnalyses},
        {"--shot-quality-report", "<project>",
         "the grades of the timeline's clips", CliSection::kAnalyses},
        {"--isolated-frames",
         "<project> [--analyze] [--timeline <id>] [--media <media-id>]...",
         "the frames a shot holds that its neighbours do not; --analyze "
         "measures what no cache covers",
         CliSection::kAnalyses},
        {"--disfluencies", "<project> <clip-id>",
         "the hesitations of a clip, by word index", CliSection::kAnalyses},
        {"--locate-source-frame", "<project> <media-id> <frame>",
         "which clips of the timeline play that frame of the rush, and "
         "where",
         CliSection::kAnalyses},
        {"--contact-sheet",
         "<project> <image.jpg> [--timeline <id>] [--max-images <n>] "
         "[--graded]",
         "one JPEG of frames sampled along the timeline; --graded shows "
         "each clip's grade and the sequence look",
         CliSection::kAnalyses},
        {"--cut-sheet",
         "<project> <image.jpg> [--timeline <id>] [--max-images <n>] "
         "[--graded]",
         "one JPEG of the frames on either side of each cut; --graded as "
         "--contact-sheet",
         CliSection::kAnalyses},
        {"--review-cuts",
         "<project> <image.jpg> [--timeline <id>] [--frame <n>]... "
         "[--max-bands <n>]",
         "measure what jumps at each junction of the timeline, and draw "
         "its bands",
         CliSection::kAnalyses},
        {"--media-sheet",
         "<project> <image.jpg> [--media <media-id>]... [--max-images <n>]",
         "one JPEG of frames of the rushes named", CliSection::kAnalyses},
        {"--timeline-stats", "<project> [--timeline <id>]",
         "durations, counts and ratios of the timeline, exact",
         CliSection::kAnalyses},
        {"--check-timeline",
         "<project> [--timeline <id>] [--policy '<json>'] [--file <export>]",
         "check the timeline against a delivery policy, without editing it",
         CliSection::kAnalyses},

        {"--measure-color",
         "<project> <clip-id>[,<clip-id>...] [--timeline <id>] "
         "[--position Start|Middle|End] [--region x,y,width,height]",
         "measure the exposure, balance and saturation of clips",
         CliSection::kColor},
        {"--match-color",
         "<project> <clip-id> <reference-clip-id> [--timeline <id>] "
         "[--position Start|Middle|End] "
         "[--reference-position Start|Middle|End] "
         "[--region x,y,width,height] [--reference-region x,y,width,height] "
         "[--match exposure,balance,saturation]",
         "propose the grade that brings a clip to a reference; changes "
         "nothing",
         CliSection::kColor},
        {"--export-cdl",
         "<project> <clip-id>[,<clip-id>...] [--file <file.cc|.ccc|.cdl>] "
         "[--timeline <id>] [--resolve-node <n>]",
         "the grade of clips as an ASC CDL", CliSection::kColor},
        {"--apply-cdl",
         "<project> <clip-id>[,<clip-id>...] --space <space> "
         "(--file <file> [--id <id>] | --slope \"r g b\" --offset \"r g b\" "
         "--power \"r g b\" [--saturation <s>]) [--timeline <id>] [--preview]",
         "apply an ASC CDL to clips as one undo step; --preview prints the "
         "operation instead",
         CliSection::kColor},

        {"--remove-words", "<project> <clip-id> '<ranges-json>'",
         "cut the words at these indices out of a clip", CliSection::kEdits},
        {"--tighten-pauses", "<project> <clip-id> [<min-gap-ms> <keep-frames>]",
         "close the silences inside a clip", CliSection::kEdits},
        {"--trim-boundary-air",
         "<project> <clip-id> [<keep-frames> <min-air-ms>]",
         "trim the air at the head and tail of a clip", CliSection::kEdits},
        {"--close-junction-air",
         "<project> <left-clip-id> <right-clip-id> "
         "[<keep-frames> <min-air-ms>]",
         "close the air across the cut between two clips", CliSection::kEdits},

        {"--add-multicam-group",
         "<project> <clip-id>,<clip-id>[,...] [--name <name>] "
         "[--angle-name <name>]... [--active <clip-id>] [--timeline <id>]",
         "group clips as the angles of one multicam", CliSection::kMulticam},
        {"--set-multicam-angle",
         "<project> <angle|clip|none> [--group <group>] [--timeline <id>]",
         "choose the angle a group shows; none shows them all and needs "
         "--group",
         CliSection::kMulticam},
        {"--remove-multicam-group", "<project> <group> [--timeline <id>]",
         "dissolve a group; its clips stay", CliSection::kMulticam},
        {"--split-multicam-group",
         "<project> <group> <frames|value/rate> [--active <angle>] "
         "[--timeline <id>]",
         "cut every angle of a group at a position; what follows becomes a "
         "second group, which can show another angle",
         CliSection::kMulticam},
        {"--list-multicam-groups", "<project> [--timeline <id>]",
         "the groups of the timeline and their angles", CliSection::kMulticam},

        // AX-2026-09 AX6 -- docs/roadmap/AX-2026-09.md (FontCatalog.h).
        {"--list-fonts", "[--query <text>] [--font-dir <directory>]...",
         "the font families motion can name, and those a caption would "
         "export in another font",
         CliSection::kMotion},
        {"--describe-compositions", "<project> [<composition-id>]",
         "the motion compositions of a project, or one as it is stored",
         CliSection::kMotion},
        {"--describe-motion-frame",
         "<composition.json|project> <frame> "
         "[--composition <id> | --clip <clip-id>] [--variant <name>] "
         "[--format <name>] [--props '<json>'] [--font-dir <directory>]...",
         "one evaluated frame as JSON: boxes, text lines, baselines",
         CliSection::kMotion},
        {"--render-motion-frame",
         "<composition.json|project> <frame> <image.png> "
         "[--composition <id> | --clip <clip-id>] [--variant <name>] "
         "[--format <name>] [--props '<json>'] [--font-dir <directory>]...",
         "one frame as a PNG; never overwrites", CliSection::kMotion},
        {"--render-motion-video",
         "<composition.json|project> <output> [--composition <id>] "
         "[--variants <a,b>] [--formats <x,y>] [--alpha] [--overwrite] "
         "[--preview] [--loudness <LUFS> [--true-peak <dBTP>]] "
         "[--font-dir <directory>]...",
         "a composition's versions and formats as video files; {variant} "
         "and {format} in <output> name each; --loudness brings the sound "
         "to it, under -1 dBTP or --true-peak",
         CliSection::kMotion},
        {"--export-motion-after",
         "<composition.json|project> <description.json> "
         "[--composition <id>] [--timeline <id>] [--aep <project.aep>] "
         "[--name <name>] [--boil <node-id>]... [--props '<json>'] "
         "[--font-dir <directory>]...",
         "a composition described for After Effects", CliSection::kMotion},
        // MB7 -- docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md.
        {"--import-after-comp",
         "<reading.json> <composition.json> "
         "[--font <After PostScript name>=<PostScript name>]... "
         "[--font-dir <directory>]...",
         "an After Effects composition read by the bridge (read_comp) as a "
         "composition of the format, with what is not translated",
         CliSection::kMotion},
        // MM3 -- docs/roadmap/MOTION-MARQUE-2026-10.md.
        {"--check-motion-safe-area",
         "<composition.json|project> [--composition <id>] "
         "[--variant <name>] [--format <name>] [--props '<json>'] "
         "[--zone default|social_vertical | --margin-percent <n>] "
         "[--at-rest] [--font-dir <directory>]...",
         "every frame of a composition rendered alone against a safe zone; "
         "exit 1 if anything leaves it (--at-rest: while standing still)",
         CliSection::kMotion},
        {"--humanize-motion",
         "<composition.json|project> <output.json> [--composition <id>] "
         "[--font-dir <directory>]...",
         "a composition rewritten as a person would write it, proved "
         "frame for frame",
         CliSection::kMotion},

        {"--export",
         "<project> <file> [--timeline <id>] [--codec h264|hevc] "
         "[--preset <id>] [--software] [--rec709a] [--overwrite] [--entire] "
         "[--loudness <LUFS>]",
         "render the timeline to a video file", CliSection::kDelivery},
        {"--export-srt",
         "<project> <file.srt> [--timeline <id>] [--word-by-word]",
         "the timeline's subtitles as SRT", CliSection::kDelivery},
        {"--export-vtt",
         "<project> <file.vtt> [--timeline <id>] [--word-by-word]",
         "the timeline's subtitles as WebVTT", CliSection::kDelivery},
        {"--srt-from-media",
         "<media> <file.srt> [--language <code>] [--verbatim]",
         "transcribe a media file straight into SRT, without a project",
         CliSection::kDelivery},
        {"--export-resolve-timeline",
         "<project> [--timeline <id>] [--grade none|cdl] [--cdl-node <n>]",
         "the timeline as the JSON the Resolve bridge builds it from",
         CliSection::kDelivery},
        {"--send-resolve",
         "<project> [--timeline <id>] [--name <name>] [--grade none|cdl] "
         "[--cdl-node <n>] [--dry-run]",
         "send the timeline to the open DaVinci Resolve project, whole",
         CliSection::kDelivery},
        {"--export-resolve-xml",
         "<project> <file.xml> [--timeline <id>] "
         "[--resolve-scaling fit|fill]",
         "the timeline as an XML file Resolve imports", CliSection::kDelivery},
        {"--export-edl",
         "<project> <file.edl> [--timeline <id>] [--start-timecode <tc>]",
         "the timeline as CMX 3600 lists, one per video track",
         CliSection::kDelivery},
        {"--export-edl-markers",
         "<project> <file.edl> [--timeline <id>] [--style loc|resolve] "
         "[--start-timecode <tc>]",
         "the timeline's markers as a CMX 3600 list", CliSection::kDelivery},
        {"--export-otio", "<project> <file.otio> [--timeline <id>]",
         "the timeline as an OpenTimelineIO file, every track in one",
         CliSection::kDelivery},
        {"--export-fcpxml", "<project> <file.fcpxml> [--timeline <id>]",
         "the timeline as a Final Cut Pro XML file", CliSection::kDelivery},
        {"--import-edl-markers",
         "<project> <file.edl> [--timeline <id>] [--start-timecode <tc>]",
         "the markers of a CMX 3600 list, in one undo step",
         CliSection::kDelivery},
        {"--import-resolve", "<project> <manifest.json>",
         "the bins and rushes of a Resolve Media Pool manifest",
         CliSection::kDelivery},
        {"--import-resolve-timeline",
         "<project> <manifest.json> [--resolve-database <Project.db>]",
         "a Resolve timeline manifest into the active timeline, with the "
         "grades read from a copy of the project's database",
         CliSection::kDelivery},

        {"--after-request", "'<request-json>'",
         "send one request to the local After Effects bridge",
         CliSection::kConnectors},

        // PRD21 -- docs/roadmap/PRODUIT-2026-10b.md (SetupCheck.h).
        {"--check-setup", "", "what this machine lacks, item by item, as JSON",
         CliSection::kMachine},
        {"--check-ffmpeg", "", "which FFmpeg runs, and what it lacks",
         CliSection::kMachine},
        {"--version", "", "this build's version and commit, as JSON",
         CliSection::kMachine},
        {kHelpName, "[<command>]", "this list, or how one command is called",
         CliSection::kMachine},
    };
    return commands;
}

const CliCommand* FindCliCommand(const std::string& name) {
    const std::string bare =
        name == "-h" ? WithoutDashes(kHelpName) : WithoutDashes(name);
    if (bare.empty()) {
        return nullptr;
    }
    for (const CliCommand& command : CliCommands()) {
        if (WithoutDashes(command.name) == bare) {
            return &command;
        }
    }
    return nullptr;
}

std::string CliUsageLine(const CliCommand& command) {
    std::string line = std::string("usage: ") + kProgram + ' ' + command.name;
    if (command.arguments[0] != '\0') {
        line += ' ';
        line += command.arguments;
    }
    return line;
}

std::vector<std::string> CliRequiredArguments(const CliCommand& command) {
    std::vector<std::string> required;
    for (const std::string& token : UsageTokens(command)) {
        if (token.rfind('<', 0) != 0 && token.rfind("'<", 0) != 0) {
            break;
        }
        required.push_back(token);
    }
    return required;
}

std::string SuggestCliCommand(const std::string& name) {
    const std::string typed = Normalized(name);
    if (const CliCommand* same = FindCliCommand(typed)) {
        return same->name;
    }
    // One edit is a slip of the finger in a name of any length. Two are
    // only taken for one in a long name: "--export-xml" is two letters from
    // "--export-cdl" and means something else.
    constexpr size_t kLongName = 14;
    const size_t limit = typed.size() >= kLongName ? 2 : 1;
    size_t best = limit + 1;
    size_t ties = 0;
    const CliCommand* nearest = nullptr;
    for (const CliCommand& command : CliCommands()) {
        const size_t distance = EditDistance(typed, command.name);
        if (distance < best) {
            best = distance;
            ties = 1;
            nearest = &command;
        } else if (distance == best) {
            ++ties;
        }
    }
    // Two commands as near as each other: neither is the obvious one.
    return nearest != nullptr && ties == 1 ? std::string(nearest->name)
                                           : std::string();
}

std::string UnknownCliCommandDetail(const std::string& name) {
    const std::string suggestion = SuggestCliCommand(name);
    return "unknown command '" + name + "'" +
           (suggestion.empty() ? std::string()
                               : ", did you mean '" + suggestion + "'?") +
           " (" + kProgram + " --help lists the commands)";
}

std::string CliArgumentsMismatch(const CliCommand& command,
                                 const std::vector<std::string>& arguments) {
    const std::string name = command.name;
    const std::vector<std::string> required = CliRequiredArguments(command);
    if (arguments.size() < required.size()) {
        return name + " is missing " + Join(required, arguments.size());
    }
    // The first option at fault, past the required arguments: one the usage
    // does not have, one left without the value the usage gives it, or
    // (PRD39) one given again that the usage does not let repeat. A value is
    // never read as an option.
    const std::vector<UsageOption> options = UsageOptions(command);
    for (size_t index = required.size(); index < arguments.size(); ++index) {
        const std::string& argument = arguments[index];
        if (!LooksLikeOption(argument)) {
            continue;
        }
        const auto known = FindUsageOption(options, argument);
        if (known == options.end() ||
            (known->takes_value && index + 1 >= arguments.size()) ||
            (!known->repeats &&
             GivenBefore(options, arguments, required.size(), index))) {
            return CliOptionRefusal(command, arguments, index);
        }
        if (known->takes_value) {
            ++index;
        }
    }
    if (HasFixedArguments(command) || AdmitsOnlyOptions(command)) {
        const std::string stray =
            FirstStrayWord(options, arguments, required.size());
        if (!stray.empty()) {
            return "unexpected " + name + " argument '" + stray + "'";
        }
    }
    return "these arguments do not fit " + name;
}

std::string CliOptionRefusal(const CliCommand& command,
                             const std::vector<std::string>& arguments,
                             size_t index) {
    const std::string name = command.name;
    const std::string option =
        index < arguments.size() ? arguments[index] : std::string();
    // A word where an option was expected is an argument too many.
    if (!LooksLikeOption(option)) {
        return "unexpected " + name + " argument '" + option + "'";
    }
    const std::vector<UsageOption> options = UsageOptions(command);
    const auto known = FindUsageOption(options, option);
    if (known == options.end()) {
        return "unknown " + name + " option '" + option + "'";
    }
    if (known->takes_value && index + 1 >= arguments.size()) {
        return name + " option '" + option + "' needs a value";
    }
    if (!known->repeats &&
        GivenBefore(options, arguments, CliRequiredArguments(command).size(),
                    index)) {
        return name + " option '" + option + "' is given twice";
    }
    if (known->takes_value) {
        return name + " option '" + option + "' does not take '" +
               arguments[index + 1] + "'";
    }
    return "these arguments do not fit " + name;
}

std::string CliHelpText() {
    const std::string program(kProgram);
    std::string text;
    text += "usage: " + program + " [<project>]            open the editor\n";
    text += "       " + program +
            " --help [<command>]     this list, or how one command is "
            "called\n";
    text += "       " + program + " <command> --help       the same\n";
    text += "\n";
    text +=
        "<project> is a .cutmachine-project folder or its "
        "project.cutmachine.json.\n";
    text +=
        "Every command answers one JSON object on stdout "
        "({\"ok\":true,...} or {\"ok\":false,\"error\":...}).\n";
    text += std::string("Arguments and examples: ") + kReadmeSection + ".\n";
    const CliSection* section = nullptr;
    for (const CliCommand& command : CliCommands()) {
        if (section == nullptr || *section != command.section) {
            section = &command.section;
            text += std::string("\n") + SectionTitle(command.section) + ":\n";
        }
        std::string line = std::string("  ") + command.name;
        if (command.arguments[0] != '\0') {
            line += ' ';
            line += command.arguments;
        }
        // The summary only where it fits beside the usage: a long usage
        // line says enough, and `--help <command>` gives the rest.
        if (line.size() + 2 <= kSummaryColumn && command.summary[0] != '\0') {
            line.append(kSummaryColumn - line.size(), ' ');
            line += command.summary;
        }
        text += line + '\n';
    }
    return text;
}

std::string CliCommandHelpText(const CliCommand& command) {
    std::string text = CliUsageLine(command) + '\n';
    if (command.summary[0] != '\0') {
        text += std::string(command.summary) + '\n';
    }
    return text + "Examples: " + kReadmeSection + ".\n";
}

CliPreflight PreflightCommandLine(
    const std::vector<std::string>& arguments,
    const std::function<bool(const std::string&)>& exists) {
    CliPreflight preflight;
    const auto refuse = [&preflight](std::string detail) {
        preflight.action = CliPreflight::Action::kRefuse;
        preflight.text = std::move(detail);
        return preflight;
    };
    const auto print = [&preflight](std::string text) {
        preflight.action = CliPreflight::Action::kPrint;
        preflight.text = std::move(text);
        return preflight;
    };
    if (arguments.empty()) {
        return preflight;
    }
    const std::string& first = arguments.front();
    if (first.empty() || first.front() != '-') {
        // A command's name without its dashes, followed by arguments or
        // naming nothing that exists: the command, mistyped, rather than a
        // project called "describe".
        const bool commandName = FindCliCommand(first) != nullptr;
        if (commandName &&
            (arguments.size() > 1 || (exists && !exists(first)))) {
            return refuse(UnknownCliCommandDetail(first));
        }
        // A project path opens the editor, and nothing follows it.
        if (arguments.size() == 1) {
            return preflight;
        }
        return refuse("unexpected argument '" + arguments[1] +
                      "' after the project path '" + first +
                      "'; usage: " + kProgram + " [<project>] (" + kProgram +
                      " --help lists the commands)");
    }
    // A command is typed as the table spells it: "-describe" is a mistake
    // to point out, not a spelling to accept.
    const CliCommand* command = FindCliCommand(first);
    if (command != nullptr && first != command->name && first != "-h") {
        command = nullptr;
    }
    if (command == nullptr) {
        return refuse(UnknownCliCommandDetail(first));
    }
    preflight.command = command;
    const std::vector<std::string> rest(arguments.begin() + 1, arguments.end());
    if (std::string(command->name) == kHelpName) {
        if (rest.empty()) {
            return print(CliHelpText());
        }
        if (rest.size() > 1) {
            return refuse("unexpected " + std::string(kHelpName) +
                          " argument '" + rest[1] + "'; " +
                          CliUsageLine(*command));
        }
        // With or without its dashes: "--help export", "--help --export".
        const CliCommand* asked = FindCliCommand(rest.front());
        if (asked == nullptr) {
            return refuse(UnknownCliCommandDetail(rest.front()));
        }
        return print(CliCommandHelpText(*asked));
    }
    if (std::any_of(rest.begin(), rest.end(), AsksForHelp)) {
        return print(CliCommandHelpText(*command));
    }
    const size_t required = CliRequiredArguments(*command).size();
    if (rest.size() < required ||
        (HasFixedArguments(*command) && rest.size() > required)) {
        return refuse(CliArgumentsMismatch(*command, rest) + "; " +
                      CliUsageLine(*command));
    }
    return preflight;
}
