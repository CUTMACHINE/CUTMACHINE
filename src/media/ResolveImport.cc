#include "ResolveImport.h"

#include "ClipSpeed.h"
#include "ColorEffects.h"

#include "CommandEnvelope.h"
#include "EditLog.h"
#include "Ingest.h"
#include "Json.h"
#include "PackageFiles.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "Ulid.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace {

const mcp_json::Value* RequiredString(const mcp_json::Value& object,
                                      const std::string& key,
                                      const std::string& context,
                                      std::string& error) {
    const mcp_json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsString()) {
        error = context + ": champ '" + key + "' absent ou non textuel";
        return nullptr;
    }
    return value;
}

std::string OptionalString(const mcp_json::Value& object,
                           const std::string& key) {
    const mcp_json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsString()) return std::string();
    return value->AsString();
}

// Depth-first descent used only to order bin creation: a child's parent must
// already own a ULID when its AddBinOperation is emitted. Doubles as the cycle
// detector, since a key still on the stack when reached again closes a loop.
bool OrderBins(const std::vector<ResolveManifestBin>& bins,
               std::vector<size_t>& order, std::string& error) {
    std::map<std::string, size_t> byKey;
    for (size_t index = 0; index < bins.size(); ++index) {
        if (bins[index].key.empty()) {
            error = "clé de chutier vide";
            return false;
        }
        if (!byKey.emplace(bins[index].key, index).second) {
            error = "clé de chutier dupliquée : " + bins[index].key;
            return false;
        }
    }
    enum class Mark { None, Open, Done };
    std::map<std::string, Mark> marks;
    order.clear();
    order.reserve(bins.size());
    for (const ResolveManifestBin& bin : bins) {
        std::vector<std::string> stack{bin.key};
        while (!stack.empty()) {
            const std::string key = stack.back();
            Mark& mark = marks[key];
            if (mark == Mark::Done) {
                stack.pop_back();
                continue;
            }
            const size_t index = byKey.at(key);
            const std::string& parent = bins[index].parent_key;
            if (mark == Mark::Open) {
                mark = Mark::Done;
                order.push_back(index);
                stack.pop_back();
                continue;
            }
            mark = Mark::Open;
            if (parent.empty()) continue;
            const auto parentEntry = byKey.find(parent);
            if (parentEntry == byKey.end()) {
                error = "chutier '" + bins[index].name +
                        "' rattaché à un parent inconnu : " + parent;
                return false;
            }
            const auto parentMark = marks.find(parent);
            if (parentMark != marks.end() && parentMark->second == Mark::Open) {
                error = "cycle de chutiers sur la clé " + parent;
                return false;
            }
            stack.push_back(parent);
        }
    }
    return true;
}

struct SkippedClip {
    std::string file;
    std::string reason;
};

// A count alone is not a report: "skipped: 3" out of 400 rushes leaves the
// user to diff the Media Pool by hand. Same errors array as IngestCommand --
// and, as there, a rush already in the library is counted, never listed: it
// is the expected outcome of re-importing, not a failure.
std::string SuccessJson(size_t addedMedia, size_t knownMedia, size_t addedBins,
                        size_t reusedBins, const std::string& detail,
                        const std::vector<SkippedClip>& skipped) {
    std::ostringstream output;
    output << "{\"ok\":true,\"added\":" << addedMedia
           << ",\"skipped\":" << (knownMedia + skipped.size())
           << ",\"bins_created\":" << addedBins
           << ",\"bins_reused\":" << reusedBins << ",\"detail\":\""
           << mcp_json::EscapeJsonString(detail) << "\",\"errors\":[";
    for (size_t index = 0; index < skipped.size(); ++index) {
        if (index) output << ',';
        output << "{\"file\":\""
               << mcp_json::EscapeJsonString(skipped[index].file)
               << "\",\"reason\":\""
               << mcp_json::EscapeJsonString(skipped[index].reason) << "\"}";
    }
    output << "]}\n";
    return output.str();
}

// CommitStoredProjectAndLogs rewrites *every* log it is handed a project
// for, substituting an empty one for any timeline absent from the map. So the
// commit below has to carry the whole set, not just the log it changed --
// same shape as MutateProjectLogCommand in Cli.cc.
bool LoadOptionalLogs(const std::string& projectPath, const Project& project,
                      std::map<std::string, EditLog>& timelineLogs,
                      ProjectEditLog& projectLog, std::string& error) {
    EditError editError = EditError::None;
    for (const DocumentSequence& timeline : project.timelines) {
        EditLog log;
        const std::string path =
            TimelineEditLogPathForProject(projectPath, timeline.id);
        std::error_code existsError;
        if (std::filesystem::exists(path, existsError) &&
            !EditLog::Load(path, log, editError, error))
            return false;
        if (existsError) {
            error =
                "historique de timeline illisible : " + existsError.message();
            return false;
        }
        timelineLogs.emplace(timeline.id, std::move(log));
    }
    const std::string projectLogPath =
        ProjectEditLogPathForProject(projectPath);
    std::error_code existsError;
    if (std::filesystem::exists(projectLogPath, existsError) &&
        !ProjectEditLog::Load(projectLogPath, projectLog, editError, error))
        return false;
    if (existsError) {
        error = "historique de projet illisible : " + existsError.message();
        return false;
    }
    return true;
}

bool ReadWholeFile(const std::filesystem::path& path, std::string& text,
                   std::string& error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "manifeste illisible : " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    text = buffer.str();
    return true;
}

}  // namespace

bool ParseResolveManifest(const std::string& json, ResolveManifest& manifest,
                          std::string& error) {
    mcp_json::Value root;
    if (!mcp_json::Value::Parse(json, root, error)) return false;
    if (!root.IsObject()) {
        error = "le manifeste doit être un objet JSON";
        return false;
    }
    const mcp_json::Value* schema =
        RequiredString(root, "schema", "manifeste", error);
    if (schema == nullptr) return false;
    if (schema->AsString() != kResolveManifestSchema) {
        error = "schéma inattendu : " + schema->AsString() + " (attendu " +
                std::string(kResolveManifestSchema) + ")";
        return false;
    }

    ResolveManifest parsed;
    parsed.project = OptionalString(root, "project");
    parsed.resolve_version = OptionalString(root, "resolve_version");

    const mcp_json::Value* bins = root.Find("bins");
    if (bins != nullptr && !bins->IsArray()) {
        error = "manifeste : 'bins' doit être un tableau";
        return false;
    }
    if (bins != nullptr) {
        for (const mcp_json::Value& entry : bins->AsArray()) {
            if (!entry.IsObject()) {
                error = "manifeste : un chutier n'est pas un objet";
                return false;
            }
            const mcp_json::Value* key =
                RequiredString(entry, "key", "chutier", error);
            if (key == nullptr) return false;
            const mcp_json::Value* name = RequiredString(
                entry, "name", "chutier " + key->AsString(), error);
            if (name == nullptr) return false;
            if (name->AsString().empty()) {
                error = "chutier " + key->AsString() + " : nom vide";
                return false;
            }
            ResolveManifestBin bin;
            bin.key = key->AsString();
            bin.name = name->AsString();
            bin.parent_key = OptionalString(entry, "parent_key");
            parsed.bins.push_back(std::move(bin));
        }
    }

    std::vector<size_t> order;
    if (!OrderBins(parsed.bins, order, error)) return false;

    std::set<std::string> knownKeys;
    for (const ResolveManifestBin& bin : parsed.bins) knownKeys.insert(bin.key);

    const mcp_json::Value* clips = root.Find("clips");
    if (clips != nullptr && !clips->IsArray()) {
        error = "manifeste : 'clips' doit être un tableau";
        return false;
    }
    if (clips != nullptr) {
        for (const mcp_json::Value& entry : clips->AsArray()) {
            if (!entry.IsObject()) {
                error = "manifeste : un rush n'est pas un objet";
                return false;
            }
            const mcp_json::Value* path =
                RequiredString(entry, "path", "rush", error);
            if (path == nullptr) return false;
            if (path->AsString().empty()) {
                error = "rush sans chemin de fichier";
                return false;
            }
            ResolveManifestClip clip;
            clip.path = path->AsString();
            clip.name = OptionalString(entry, "name");
            clip.bin_key = OptionalString(entry, "bin_key");
            clip.resolve_uid = OptionalString(entry, "resolve_uid");
            if (!clip.bin_key.empty() && knownKeys.count(clip.bin_key) == 0) {
                error = "rush '" + clip.path +
                        "' rattaché à un chutier inconnu : " + clip.bin_key;
                return false;
            }
            parsed.clips.push_back(std::move(clip));
        }
    }

    manifest = std::move(parsed);
    return true;
}

bool PlanResolveImport(const Document& document,
                       const ResolveManifest& manifest, ResolveImportPlan& plan,
                       std::string& error) {
    std::vector<size_t> order;
    if (!OrderBins(manifest.bins, order, error)) return false;

    ResolveImportPlan built;
    built.bin_ids.emplace(std::string(), Ulid());
    for (const size_t index : order) {
        const ResolveManifestBin& bin = manifest.bins[index];
        const auto parent = built.bin_ids.find(bin.parent_key);
        if (parent == built.bin_ids.end()) {
            error = "chutier '" + bin.name + "' : parent non résolu";
            return false;
        }
        const DocumentBin* existing = nullptr;
        for (const DocumentBin& candidate : document.bins) {
            if (candidate.name == bin.name &&
                candidate.parent_id == parent->second) {
                existing = &candidate;
                break;
            }
        }
        if (existing != nullptr) {
            built.bin_ids.emplace(bin.key, existing->id);
            ++built.reused_bins;
            continue;
        }
        AddBinOperation operation;
        operation.bin_id = GenerateUlid();
        operation.name = bin.name;
        operation.parent_id = parent->second;
        built.bin_ids.emplace(bin.key, operation.bin_id);
        built.new_bins.push_back(std::move(operation));
    }
    plan = std::move(built);
    return true;
}

int ImportResolveCommand(const std::string& projectPath,
                         const std::string& manifestPath, std::string& output) {
    std::string reason;
    std::string manifestText;
    if (!ReadWholeFile(std::filesystem::path(manifestPath), manifestText,
                       reason)) {
        return FailCliCommand("IoError", reason, output);
    }
    ResolveManifest manifest;
    if (!ParseResolveManifest(manifestText, manifest, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }

    Project project;
    if (!LoadStoredProject(projectPath, project, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    Document document = project.MakeActiveDocument();

    ResolveImportPlan plan;
    if (!PlanResolveImport(document, manifest, plan, reason)) {
        return FailCliCommand("ValidationFailed", reason, output);
    }

    std::map<std::string, EditLog> logs;
    ProjectEditLog projectLog;
    if (!LoadOptionalLogs(projectPath, project, logs, projectLog, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    EditLog& log = logs[project.active_timeline_id];

    EditError error = EditError::None;
    for (const AddBinOperation& operation : plan.new_bins) {
        if (!log.Apply(document, operation, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
    }

    const std::filesystem::path projectRoot =
        std::filesystem::path(projectPath).parent_path();
    std::map<std::string, size_t> knownPaths;
    for (size_t index = 0; index < document.library.size(); ++index) {
        std::filesystem::path stored(document.library[index].path);
        if (stored.is_relative()) stored = projectRoot / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) knownPaths.emplace(resolved.string(), index);
    }

    size_t added = 0;
    size_t alreadyPresent = 0;
    std::vector<SkippedClip> skipped;
    // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. The new rushes enter
    // the library through one IngestMediaOperation, each in its bin, applied
    // once the manifest is read: one undo step, where they used to be added
    // outside the journal and only their filing journaled.
    IngestMediaOperation ingest;
    std::map<std::string, size_t> pendingPaths;
    for (const ResolveManifestClip& clip : manifest.clips) {
        std::error_code pathError;
        const std::filesystem::path absolute =
            std::filesystem::weakly_canonical(std::filesystem::path(clip.path),
                                              pathError);
        if (pathError) {
            skipped.push_back({clip.path, pathError.message()});
            continue;
        }
        const Ulid& binId = plan.bin_ids[clip.bin_key];
        const auto pending = pendingPaths.find(absolute.string());
        if (pending != pendingPaths.end()) {
            // Named twice by the manifest: the later bin wins, as a second
            // filing used to move it there.
            ++alreadyPresent;
            ingest.added[pending->second].bin_id = binId;
            continue;
        }
        const auto known = knownPaths.find(absolute.string());
        if (known != knownPaths.end()) {
            ++alreadyPresent;
            SetMediaBinOperation filing;
            filing.media_id = document.library[known->second].id;
            filing.bin_id = binId;
            if (document.library[known->second].bin_id != binId &&
                !log.Apply(document, filing, error, reason)) {
                return FailCliCommand(EditErrorName(error), reason, output);
            }
            continue;
        }
        // DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md. Read as an
        // ingest reads it, headers then audio level (ProbeIngestFile): a
        // rush brought in from Resolve had no measured level, and nothing
        // told a mute cutaway from speech before Whisper ran on it.
        std::set<std::string> unread;
        const ProbedIngestFile probed = ProbeIngestFile(absolute, unread);
        if (!probed.media) {
            skipped.push_back(
                {absolute.filename().string(),
                 probed.error.empty() ? "unreadable media" : probed.error});
            continue;
        }
        LibraryMedia media = *probed.media;
        media.id = GenerateUlid();
        media.filename = absolute.filename().string();
        // LIEN1 -- the path Resolve names, links kept, as ingest stores it.
        media.path = StoredMediaPath(clip.path, projectRoot);
        media.bin_id = binId;
        media.proxy_path.clear();
        pendingPaths.emplace(absolute.string(), ingest.added.size());
        ingest.added.push_back(std::move(media));
        ++added;
    }
    if (!ingest.added.empty() && !log.Apply(document, ingest, error, reason)) {
        return FailCliCommand(EditErrorName(error), reason, output);
    }

    if (!document.Validate(reason)) {
        return FailCliCommand("ValidationFailed", reason, output);
    }
    if (!project.CommitActiveDocument(document, reason) ||
        !CommitStoredProjectAndLogs(projectPath, project, logs, projectLog,
                                    reason)) {
        return FailCliCommand("IoError", reason, output);
    }
    output = SuccessJson(added, alreadyPresent, plan.new_bins.size(),
                         plan.reused_bins, manifest.project, skipped);
    return 0;
}

// ---------------------------------------------------------------------------
// R10 -- docs/roadmap/REEL-2026-09.md. Reading a Resolve timeline back into
// the document. See ResolveImport.h for the route taken and what it cannot
// see.

namespace {

bool RequiredInt(const mcp_json::Value& object, const std::string& key,
                 const std::string& context, int64_t& out, std::string& error) {
    const mcp_json::Value* value = object.Find(key);
    if (value == nullptr || !value->IsNumber() || !value->AsInt64(out)) {
        error = context + " : champ '" + key + "' absent ou non entier";
        return false;
    }
    return true;
}

int64_t OptionalInt(const mcp_json::Value& object, const std::string& key) {
    const mcp_json::Value* value = object.Find(key);
    int64_t parsed = 0;
    if (value == nullptr || !value->IsNumber() || !value->AsInt64(parsed))
        return 0;
    return parsed;
}

// Exact comparison of Resolve's textual property value against a whole
// number, without ever building a float: "1.000" is 1, "-0" is 0, "0.5" is
// not 0. `readable` tells a malformed literal (which is refused for being
// unreadable) from a well-formed value that simply is not the default.
bool DecimalEqualsWhole(const std::string& text, int64_t whole,
                        bool& readable) {
    readable = false;
    size_t index = 0;
    bool negative = false;
    if (index < text.size() && (text[index] == '+' || text[index] == '-')) {
        negative = text[index] == '-';
        ++index;
    }
    std::string digits;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        digits.push_back(text[index]);
        ++index;
    }
    bool fractionIsZero = true;
    if (index < text.size() && text[index] == '.') {
        ++index;
        size_t fractionDigits = 0;
        while (index < text.size() && text[index] >= '0' &&
               text[index] <= '9') {
            if (text[index] != '0') fractionIsZero = false;
            ++fractionDigits;
            ++index;
        }
        if (fractionDigits == 0) return false;
    }
    if (index != text.size() || digits.empty()) return false;
    readable = true;
    if (!fractionIsZero) return false;
    const size_t first = digits.find_first_not_of('0');
    const std::string significant =
        first == std::string::npos ? std::string("0") : digits.substr(first);
    if (significant.size() > 18) return false;
    int64_t value = 0;
    for (const char digit : significant) value = value * 10 + (digit - '0');
    if (negative) value = -value;
    return value == whole;
}

// R10 -- what Resolve calls a cross dissolve. Measured: GetName() on a
// transition item answers the kind, in English, on a French-language
// project. Every other name is refused carrying that name: R5 gave the
// document a colour dip, but which Resolve effect maps onto it -- "Dip to
// Color Dissolve" and its colour parameter -- has not been read back from
// Resolve, and guessing it would relabel a montage nobody checked.
constexpr char kResolveCrossDissolve[] = "Cross Dissolve";

// "V1", "A2": Resolve's own name for a track, which is what the editor sees
// when told where a refusal sits.
std::string ResolveTrackLabel(const ResolveTimelineTrackRead& track) {
    return (track.kind == "audio" ? "A" : "V") + std::to_string(track.index);
}

std::string ClipLabel(const ResolveTimelineItem& item, size_t position) {
    if (!item.name.empty()) return item.name;
    return "plan " + std::to_string(position + 1);
}

// Which family a property belongs to, so a refusal says what the timeline
// carries rather than only which key moved.
std::string PropertyFamily(const std::string& key) {
    if (key == "Opacity") return "opacité";
    if (key == "CompositeMode") return "mode de fusion";
    return "cadrage";
}

std::string RateText(const MediaRate& rate) {
    if (rate.den == 1) return std::to_string(rate.num);
    return std::to_string(rate.num) + "/" + std::to_string(rate.den);
}

std::string LexicalAbsolute(const std::string& path,
                            const std::string& projectRoot) {
    std::filesystem::path stored(path);
    if (stored.is_relative() && !projectRoot.empty())
        stored = std::filesystem::path(projectRoot) / stored;
    return stored.lexically_normal().string();
}

}  // namespace

const std::vector<ResolveItemPropertyDefault>& ResolveItemPropertyDefaults() {
    static const std::vector<ResolveItemPropertyDefault> defaults = {
        // Measured against Resolve 20.3.1 on 22 September 2026 by dumping
        // GetProperty() on an untouched clip. The previous table was guessed
        // and named a key Resolve does not publish ("Dynamic Zoom"), which
        // the bridge then skipped in silence: a clip with a dynamic zoom
        // arrived looking untouched. ZoomGang is the one default that is not
        // zero-or-one-shaped -- it ships enabled.
        {"ZoomX", 1},
        {"ZoomY", 1},
        {"Pan", 0},
        {"Tilt", 0},
        {"RotationAngle", 0},
        {"AnchorPointX", 0},
        {"AnchorPointY", 0},
        {"CropLeft", 0},
        {"CropRight", 0},
        {"CropTop", 0},
        {"CropBottom", 0},
        {"CropSoftness", 0},
        {"CropRetain", 0},
        {"FlipX", 0},
        {"FlipY", 0},
        {"Opacity", 100},
        {"CompositeMode", 0},
        {"DynamicZoomEase", 0},
        {"Distortion", 0},
        {"Pitch", 0},
        {"Yaw", 0},
        {"ResizeFilter", 0},
        {"Scaling", 0},
        {"ZoomGang", 1},
        {"MotionEstimation", 0},
        {"RetimeProcess", 0},
    };
    return defaults;
}

bool ParseResolveTimelineManifest(const std::string& json,
                                  ResolveTimelineRead& manifest,
                                  std::string& error) {
    mcp_json::Value root;
    if (!mcp_json::Value::Parse(json, root, error)) return false;
    if (!root.IsObject()) {
        error = "le manifeste de timeline doit être un objet JSON";
        return false;
    }
    const mcp_json::Value* schema =
        RequiredString(root, "schema", "manifeste de timeline", error);
    if (schema == nullptr) return false;
    if (schema->AsString() != kResolveTimelineManifestSchema) {
        error = "schéma inattendu : " + schema->AsString() + " (attendu " +
                std::string(kResolveTimelineManifestSchema) + ")";
        return false;
    }

    ResolveTimelineRead parsed;
    parsed.project = OptionalString(root, "project");
    parsed.resolve_version = OptionalString(root, "resolve_version");
    const mcp_json::Value* name =
        RequiredString(root, "name", "manifeste de timeline", error);
    if (name == nullptr) return false;
    parsed.name = name->AsString();
    if (parsed.name.empty()) {
        error = "manifeste de timeline : nom vide";
        return false;
    }

    int64_t frameRate = 0;
    int64_t width = 0;
    int64_t height = 0;
    if (!RequiredInt(root, "frame_rate", "timeline « " + parsed.name + " »",
                     frameRate, error) ||
        !RequiredInt(root, "width", "timeline « " + parsed.name + " »", width,
                     error) ||
        !RequiredInt(root, "height", "timeline « " + parsed.name + " »", height,
                     error))
        return false;
    if (frameRate <= 0 || frameRate > 1000) {
        error = "timeline « " + parsed.name +
                " » : cadence hors bornes : " + std::to_string(frameRate);
        return false;
    }
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) {
        error = "timeline « " + parsed.name +
                " » : définition hors bornes : " + std::to_string(width) + "x" +
                std::to_string(height);
        return false;
    }
    parsed.frame_rate = static_cast<int32_t>(frameRate);
    parsed.width = static_cast<int32_t>(width);
    parsed.height = static_cast<int32_t>(height);
    parsed.origin = OptionalInt(root, "origin");

    const mcp_json::Value* unpublished = root.Find("unpublished");
    if (unpublished != nullptr) {
        if (!unpublished->IsArray()) {
            error =
                "manifeste de timeline : 'unpublished' doit être un "
                "tableau";
            return false;
        }
        for (const mcp_json::Value& entry : unpublished->AsArray()) {
            if (!entry.IsString()) {
                error =
                    "manifeste de timeline : 'unpublished' doit ne "
                    "contenir que du texte";
                return false;
            }
            parsed.unpublished.push_back(entry.AsString());
        }
    }

    const mcp_json::Value* tracks = root.Find("tracks");
    if (tracks == nullptr || !tracks->IsArray() || tracks->AsArray().empty()) {
        error = "timeline « " + parsed.name +
                " » : 'tracks' absent, non tabulaire ou vide";
        return false;
    }
    std::set<std::string> seenTracks;
    for (const mcp_json::Value& entry : tracks->AsArray()) {
        if (!entry.IsObject()) {
            error = "timeline « " + parsed.name +
                    " » : une piste n'est pas un objet";
            return false;
        }
        const mcp_json::Value* kind =
            RequiredString(entry, "kind", "piste", error);
        if (kind == nullptr) return false;
        ResolveTimelineTrackRead track;
        track.kind = kind->AsString();
        if (track.kind != "video" && track.kind != "audio") {
            error = "piste de type inconnu : " + track.kind +
                    " (attendu 'video' ou 'audio')";
            return false;
        }
        int64_t index = 0;
        if (!RequiredInt(entry, "index", "piste " + track.kind, index, error))
            return false;
        if (index <= 0 || index > 100000) {
            error = "piste " + track.kind +
                    " : index hors bornes : " + std::to_string(index);
            return false;
        }
        track.index = static_cast<int32_t>(index);
        track.name = OptionalString(entry, "name");
        const std::string key = ResolveTrackLabel(track);
        if (!seenTracks.insert(key).second) {
            error = "piste dupliquée dans le manifeste : " + key;
            return false;
        }

        const mcp_json::Value* clips = entry.Find("clips");
        if (clips != nullptr && !clips->IsArray()) {
            error = "piste " + key + " : 'clips' doit être un tableau";
            return false;
        }
        if (clips != nullptr) {
            for (const mcp_json::Value& item : clips->AsArray()) {
                if (!item.IsObject()) {
                    error = "piste " + key + " : un plan n'est pas un objet";
                    return false;
                }
                ResolveTimelineItem clip;
                clip.name = OptionalString(item, "name");
                const std::string context =
                    "piste " + key + ", plan « " + clip.name + " »";
                int64_t recordStart = 0;
                int64_t recordEnd = 0;
                int64_t sourceStart = 0;
                if (!RequiredInt(item, "record_start", context, recordStart,
                                 error) ||
                    !RequiredInt(item, "record_end", context, recordEnd,
                                 error) ||
                    !RequiredInt(item, "source_start", context, sourceStart,
                                 error))
                    return false;
                clip.record_start = recordStart;
                clip.record_end = recordEnd;
                clip.source_start = sourceStart;
                clip.duration = OptionalInt(item, "duration");
                clip.fusion_comps = OptionalInt(item, "fusion_comps");
                clip.left_offset = OptionalInt(item, "left_offset");
                clip.right_offset = OptionalInt(item, "right_offset");
                clip.media_frames = OptionalInt(item, "media_frames");
                clip.media_rate =
                    static_cast<int32_t>(OptionalInt(item, "media_rate"));
                clip.speed_unmeasurable =
                    OptionalString(item, "speed_unmeasurable");
                clip.path = OptionalString(item, "path");
                clip.no_file_reason = OptionalString(item, "no_file_reason");

                const mcp_json::Value* properties = item.Find("properties");
                if (properties != nullptr && !properties->IsObject()) {
                    error = context + " : 'properties' doit être un objet";
                    return false;
                }
                if (properties != nullptr) {
                    for (const auto& property : properties->AsObject()) {
                        if (!property.second.IsString()) {
                            error = context + " : la propriété '" +
                                    property.first +
                                    "' doit être du texte (jamais un "
                                    "flottant JSON)";
                            return false;
                        }
                        clip.properties.emplace_back(
                            property.first, property.second.AsString());
                    }
                }
                track.clips.push_back(std::move(clip));
            }
        }

        // R10 -- the transitions of this track, in a list of their own. They
        // overlap the clips they join, so reading them among the clips made
        // the planner see two clips laid over each other and refuse both.
        const mcp_json::Value* transitions = entry.Find("transitions");
        if (transitions != nullptr && !transitions->IsArray()) {
            error = "piste " + key + " : 'transitions' doit être un tableau";
            return false;
        }
        if (transitions != nullptr) {
            for (const mcp_json::Value& item : transitions->AsArray()) {
                if (!item.IsObject()) {
                    error =
                        "piste " + key + " : une transition n'est pas un objet";
                    return false;
                }
                ResolveTimelineTransition transition;
                transition.name = OptionalString(item, "name");
                const std::string context =
                    "piste " + key + ", transition « " + transition.name + " »";
                int64_t recordStart = 0;
                int64_t recordEnd = 0;
                if (!RequiredInt(item, "record_start", context, recordStart,
                                 error) ||
                    !RequiredInt(item, "record_end", context, recordEnd, error))
                    return false;
                transition.record_start = recordStart;
                transition.record_end = recordEnd;
                transition.duration = OptionalInt(item, "duration");
                track.transitions.push_back(std::move(transition));
            }
        }
        parsed.tracks.push_back(std::move(track));
    }

    manifest = std::move(parsed);
    return true;
}

std::string DescribeResolveTimelineRefusals(
    const std::vector<ResolveTimelineRefusal>& refusals) {
    // A long montage can refuse everything it holds; the structured list in
    // `result` stays whole, the sentence stops at what a reader can use.
    constexpr size_t kShown = 12;
    std::ostringstream text;
    text << refusals.size() << " refus";
    for (size_t index = 0; index < refusals.size() && index < kShown; ++index) {
        text << (index == 0 ? " : " : " ; ") << refusals[index].track << '/'
             << refusals[index].clip << " — " << refusals[index].reason;
    }
    if (refusals.size() > kShown)
        text << " ; et " << (refusals.size() - kShown) << " autre(s)";
    return text.str();
}

// R10 -- the exact speed of one item, or a named refusal.
//
// Measured on Resolve 20.3.1, 22 September 2026, and cross-checked against
// three real montages whose speeds had been relevés by hand: 31 clips at 1/2,
// then 32 at 1/2 with 81 at 1, then 27 at 1/2 with 65 at 1 -- 236 clips, no
// disagreement.
//
//   total = left_offset + (record_end - record_start) + right_offset
//   at speed 1 the same media spans media_frames * tl_rate / media_rate
//   speed = (media_frames * tl_rate) / (media_rate * total)
//
// Integers only, so the result is an exact fraction and nothing rounds. The
// routes that look more obvious do not work: GetDuration equals
// record_end - record_start on a retimed clip exactly as on a normal one,
// GetSourceEndFrame is short by one or two frames depending on the clip, and
// no published property carries a speed at all.
bool ResolveItemSpeed(const ResolveTimelineItem& item, int32_t timelineRate,
                      EffectParamValue& speed, std::string& reason) {
    if (!item.speed_unmeasurable.empty()) {
        reason = "vitesse non mesurable (" + item.speed_unmeasurable + ")";
        return false;
    }
    const int64_t span = item.record_end - item.record_start;
    const int64_t total = item.left_offset + span + item.right_offset;
    if (item.media_frames <= 0 || item.media_rate <= 0 || total <= 0) {
        reason = "vitesse non mesurable (marges ou média absents du manifeste)";
        return false;
    }
    const int64_t numerator = item.media_frames * timelineRate;
    const int64_t denominator = static_cast<int64_t>(item.media_rate) * total;
    const int64_t divisor = std::gcd(numerator, denominator);
    const int64_t num = numerator / divisor;
    const int64_t den = denominator / divisor;
    if (num > kMaxClipSpeedTerm || den > kMaxClipSpeedTerm) {
        reason = "vitesse " + std::to_string(num) + "/" + std::to_string(den) +
                 " : termes trop grands pour le document (maximum " +
                 std::to_string(kMaxClipSpeedTerm) + ")";
        return false;
    }
    speed =
        EffectParamValue{static_cast<int32_t>(num), static_cast<int32_t>(den)};
    return true;
}

bool PlanResolveTimelineImport(const Document& document,
                               const std::string& projectRoot,
                               const ResolveTimelineRead& manifest,
                               ResolveTimelineImportPlan& plan,
                               std::string& error) {
    ResolveTimelineImportPlan built;
    built.unpublished = manifest.unpublished;

    if (manifest.frame_rate <= 0 || manifest.width <= 0 ||
        manifest.height <= 0 || manifest.name.empty()) {
        error =
            "manifeste de timeline incomplet : nom, cadence et définition "
            "sont obligatoires";
        return false;
    }
    if (manifest.tracks.empty()) {
        error = "timeline « " + manifest.name + " » : aucune piste";
        return false;
    }

    // Reading a cut into a timeline that already holds one would interleave
    // two montages: the positions are absolute, so nothing would refuse it,
    // and the result would look like an edit nobody made.
    size_t existingClips = 0;
    for (const DocumentTrack& track : document.sequence.tracks)
        existingClips += track.clips.size();
    if (existingClips != 0) {
        error = "la timeline « " + document.sequence.name + " » porte déjà " +
                std::to_string(existingClips) +
                " plan(s) : relis dans une timeline vide "
                "(--apply-project-op add_timeline)";
        return false;
    }

    built.sequence = UpdateSequenceOperation{
        document.sequence.id, manifest.name, manifest.width, manifest.height,
        MediaRate{manifest.frame_rate, 1}};

    // Media identity is the resolved file path, as in the Media Pool import
    // above; the filename is the fallback the bridge also uses, and only
    // when it names one media and no other.
    std::map<std::string, const LibraryMedia*> byPath;
    std::map<std::string, const LibraryMedia*> byName;
    for (const LibraryMedia& media : document.library) {
        byPath.emplace(LexicalAbsolute(media.path, projectRoot), &media);
        std::string filename = media.filename;
        if (filename.empty())
            filename = std::filesystem::path(media.path).filename().string();
        if (filename.empty()) continue;
        const auto inserted = byName.emplace(filename, &media);
        if (!inserted.second) inserted.first->second = nullptr;
    }

    // One index space for every kind: ApplyAddTrack refuses a duplicate
    // index whatever the kind, and the export layers video by that same
    // index, so a new overlay must land above the tracks already there.
    std::map<std::string, std::vector<const DocumentTrack*>> existingByKind;
    int32_t nextIndex = 0;
    for (const DocumentTrack& track : document.sequence.tracks) {
        existingByKind[track.kind].push_back(&track);
        nextIndex = std::max(nextIndex, track.index + 1);
    }
    for (auto& entry : existingByKind) {
        std::sort(entry.second.begin(), entry.second.end(),
                  [](const DocumentTrack* left, const DocumentTrack* right) {
                      return left->index < right->index;
                  });
    }

    std::vector<const ResolveTimelineTrackRead*> ordered;
    for (const std::string& kind : {std::string("video"), std::string("audio")})
        for (const ResolveTimelineTrackRead& track : manifest.tracks)
            if (track.kind == kind) ordered.push_back(&track);
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const ResolveTimelineTrackRead* left,
                        const ResolveTimelineTrackRead* right) {
                         if (left->kind != right->kind) return false;
                         return left->index < right->index;
                     });

    std::map<std::string, size_t> usedByKind;
    std::vector<std::pair<const ResolveTimelineTrackRead*, Ulid>> targets;
    for (const ResolveTimelineTrackRead* track : ordered) {
        const std::vector<const DocumentTrack*>& existing =
            existingByKind[track->kind];
        const size_t used = usedByKind[track->kind]++;
        if (used < existing.size()) {
            targets.emplace_back(track, existing[used]->id);
            ++built.reused_tracks;
            continue;
        }
        AddTrackOperation operation;
        operation.track_id = GenerateUlid();
        operation.kind = track->kind;
        operation.index = nextIndex++;
        targets.emplace_back(track, operation.track_id);
        built.new_tracks.push_back(std::move(operation));
    }

    const auto refuseNamed = [&](const ResolveTimelineTrackRead& track,
                                 const std::string& label,
                                 const std::string& reason) {
        built.refusals.push_back({ResolveTrackLabel(track), label, reason});
    };
    const auto refuse = [&](const ResolveTimelineTrackRead& track,
                            const ResolveTimelineItem& item, size_t position,
                            const std::string& reason) {
        refuseNamed(track, ClipLabel(item, position), reason);
    };

    for (const auto& target : targets) {
        const ResolveTimelineTrackRead& track = *target.first;
        // What this track actually kept, in edit order: the transitions of
        // the same track are read against these and nothing else, so a
        // transition can never name a clip that was refused.
        struct KeptClip {
            int64_t record_start = 0;
            int64_t record_end = 0;
            size_t plan_index = 0;
        };
        std::vector<KeptClip> kept;
        std::vector<const ResolveTimelineItem*> items;
        items.reserve(track.clips.size());
        for (const ResolveTimelineItem& item : track.clips)
            items.push_back(&item);
        std::stable_sort(items.begin(), items.end(),
                         [](const ResolveTimelineItem* left,
                            const ResolveTimelineItem* right) {
                             return left->record_start < right->record_start;
                         });
        int64_t previousEnd = 0;
        for (size_t position = 0; position < items.size(); ++position) {
            const ResolveTimelineItem& item = *items[position];
            bool refused = false;
            if (item.record_start < 0 || item.record_end <= item.record_start) {
                refuse(track, item, position,
                       "plage de séquence vide ou négative (" +
                           std::to_string(item.record_start) + " → " +
                           std::to_string(item.record_end) + ")");
                continue;
            }
            // A gap is fine -- timeline_in is absolute -- but an overlap on
            // one track is not a cut the document can hold.
            if (position != 0 && item.record_start < previousEnd) {
                refuse(track, item, position,
                       "chevauchement de " +
                           std::to_string(previousEnd - item.record_start) +
                           " image(s) avec le plan précédent");
                refused = true;
            }
            previousEnd = item.record_end;
            if (item.fusion_comps > 0) {
                refuse(track, item, position,
                       std::to_string(item.fusion_comps) +
                           " composition(s) Fusion : un effet de Resolve que "
                           "le document ne porte pas");
                refused = true;
            }
            for (const auto& property : item.properties) {
                const auto& defaults = ResolveItemPropertyDefaults();
                const auto found =
                    std::find_if(defaults.begin(), defaults.end(),
                                 [&](const ResolveItemPropertyDefault& entry) {
                                     return property.first == entry.key;
                                 });
                if (found == defaults.end()) {
                    refuse(track, item, position,
                           "propriété Resolve inconnue de la relecture : " +
                               property.first + "=" + property.second);
                    refused = true;
                    continue;
                }
                bool readable = false;
                if (DecimalEqualsWhole(property.second, found->value, readable))
                    continue;
                if (!readable) {
                    refuse(track, item, position,
                           "valeur illisible pour " + property.first + " : " +
                               property.second);
                    refused = true;
                    continue;
                }
                refuse(track, item, position,
                       PropertyFamily(property.first) + " : " + property.first +
                           "=" + property.second +
                           " (le document ne le porte pas : R1 pour le "
                           "cadrage)");
                refused = true;
            }
            if (item.path.empty()) {
                refuse(track, item, position,
                       "aucun fichier sur le disque" +
                           (item.no_file_reason.empty()
                                ? std::string()
                                : " (" + item.no_file_reason + ")") +
                           " : clip composé, timeline imbriquée ou générateur");
                continue;
            }
            const LibraryMedia* media = nullptr;
            const auto byPathEntry =
                byPath.find(LexicalAbsolute(item.path, projectRoot));
            if (byPathEntry != byPath.end()) {
                media = byPathEntry->second;
            } else {
                const auto byNameEntry = byName.find(
                    std::filesystem::path(item.path).filename().string());
                if (byNameEntry != byName.end()) media = byNameEntry->second;
            }
            if (media == nullptr) {
                refuse(track, item, position,
                       "média absent de la bibliothèque : " + item.path +
                           " (importe d'abord le Media Pool avec "
                           "--import-resolve)");
                continue;
            }
            const DocumentSource* source = document.FindSource(media->id);
            if (source == nullptr) {
                refuse(track, item, position,
                       "média « " + media->filename +
                           " » sans source montée dans le document");
                continue;
            }
            if (media->rate.num <= 0 || media->rate.den <= 0) {
                refuse(track, item, position,
                       "média « " + media->filename +
                           " » sans cadence utilisable");
                continue;
            }
            if (item.source_start < 0) {
                refuse(track, item, position,
                       "image source négative : " +
                           std::to_string(item.source_start));
                continue;
            }

            EffectParamValue speed{1, 1};
            std::string speedReason;
            if (!ResolveItemSpeed(item, manifest.frame_rate, speed,
                                  speedReason)) {
                refuse(track, item, position, speedReason);
                continue;
            }

            const int64_t recordSpan = item.record_end - item.record_start;
            RationalTime timelineIn;
            RationalTime duration;
            RationalTime sourceIn;
            int64_t sourceFrames = 0;
            try {
                timelineIn =
                    RationalTime{item.record_start, manifest.frame_rate};
                duration = RationalTime{recordSpan, manifest.frame_rate};
                sourceIn = RationalTime{item.source_start * media->rate.den,
                                        media->rate.num};
                if (!ExactFrameCount(ScaleBySpeed(duration, speed), media->rate,
                                     sourceFrames)) {
                    refuse(track, item, position,
                           std::to_string(recordSpan) + " image(s) à " +
                               std::to_string(manifest.frame_rate) +
                               " i/s ne font pas un nombre entier d'images de "
                               "« " +
                               media->filename + " » à " +
                               RateText(media->rate) + " i/s");
                    continue;
                }
                if (sourceFrames <= 0) {
                    refuse(track, item, position,
                           "plan plus court qu'une image source");
                    continue;
                }
                // R3 refuses a source_in that does not fall on a source
                // frame once the speed is not 1/1, because monitor and export
                // only name the same picture when it does. Resolve addresses
                // the source in whole frames, so this holds -- but it is
                // checked rather than assumed.
                if (!IsUnitSpeed(speed) &&
                    !IsOnSourceFrameBoundary(sourceIn, media->rate)) {
                    refuse(track, item, position,
                           "entrée source hors d'une image de « " +
                               media->filename + " » à la vitesse " +
                               std::to_string(speed.num) + "/" +
                               std::to_string(speed.den));
                    continue;
                }
                if (sourceIn.add(ScaleBySpeed(duration, speed))
                        .compare(source->duration) > 0) {
                    refuse(
                        track, item, position,
                        "la plage source (" +
                            std::to_string(item.source_start) + " → " +
                            std::to_string(item.source_start + sourceFrames) +
                            ") sort de « " + media->filename + " »");
                    continue;
                }
            } catch (const std::exception& overflow) {
                refuse(track, item, position,
                       std::string("temps inexprimable : ") + overflow.what());
                continue;
            }
            if (refused) continue;

            InsertClipOperation insert;
            insert.track_id = target.second;
            insert.source_id = media->id;
            insert.source_in = sourceIn;
            insert.duration = duration;
            insert.timeline_in = timelineIn;
            // The id is generated here, not left to the application, because
            // EditLog::Apply takes its operation by value: an id it generates
            // never comes back to the caller, and the speed below has to
            // address this very clip. The plan already generates the ULIDs of
            // the tracks it creates, for the same reason.
            insert.clip_id = GenerateUlid();
            built.clips.push_back(std::move(insert));
            built.clip_speeds.push_back(speed);
            built.clip_origins.push_back(
                {static_cast<size_t>(&track - manifest.tracks.data()),
                 static_cast<size_t>(items[position] - track.clips.data())});
            kept.push_back(
                {item.record_start, item.record_end, built.clips.size() - 1});
            if (!IsUnitSpeed(speed)) ++built.retimed_clips;
        }

        // R10 -- the transitions of this track, once its clips are known.
        // Measured on Resolve 20.3.1, 22 September 2026, on a montage of
        // 31 clips carrying five of them: three cross dissolves of 12 frames
        // straddling a cut 6 frames either side, plus one opening from black
        // and one closing to it, which have no second clip. R5 (22
        // September) gave the document those two: they come back as black
        // colour dips. That mapping has NOT been put back in front of
        // Resolve -- the rushes were offline -- so it is read from the
        // bounds and from nothing else.
        for (size_t position = 0; position < track.transitions.size();
             ++position) {
            const ResolveTimelineTransition& transition =
                track.transitions[position];
            const std::string label =
                transition.name.empty()
                    ? "transition " + std::to_string(position + 1)
                    : transition.name;
            const int64_t frames =
                transition.record_end - transition.record_start;
            if (transition.record_start < 0 || frames <= 0) {
                refuseNamed(track, label,
                            "bornes de transition vides ou négatives (" +
                                std::to_string(transition.record_start) +
                                " → " + std::to_string(transition.record_end) +
                                ")");
                continue;
            }
            // Named with Resolve's own word for it, so the editor reads back
            // what the Resolve interface shows them. A cross dissolve at
            // the edge of a track is Resolve's own fade from or to black,
            // and comes back as a colour dip below; every other kind -- a
            // dip to colour with its own colour parameter, a flash, an
            // audio cross fade -- is still refused, because what Resolve
            // reports for them has not been read.
            if (transition.name != kResolveCrossDissolve) {
                refuseNamed(track, label,
                            "« " + label + " » : seul le fondu enchaîné (« " +
                                std::string(kResolveCrossDissolve) +
                                " ») se relit, y compris en bord de piste ; "
                                "les autres raccords ne sont pas relus faute "
                                "d'avoir été mesurés dans Resolve");
                continue;
            }
            if (track.kind != "video") {
                refuseNamed(track, label,
                            "transition sur une piste audio : le document ne "
                            "porte de transition que sur une piste vidéo");
                continue;
            }

            // The one cut this transition straddles, among the cuts the kept
            // clips of this track actually share. A transition at the edge
            // of the track -- the opening or closing fade to black -- shares
            // none; since R5 it becomes a colour dip on the single clip it
            // covers, below.
            size_t cutIndex = kept.size();
            size_t straddled = 0;
            int64_t cut = 0;
            for (size_t index = 0; index + 1 < kept.size(); ++index) {
                if (kept[index].record_end != kept[index + 1].record_start)
                    continue;
                const int64_t candidate = kept[index].record_end;
                if (candidate < transition.record_start ||
                    candidate > transition.record_end)
                    continue;
                ++straddled;
                cutIndex = index;
                cut = candidate;
            }
            if (straddled > 1) {
                refuseNamed(track, label,
                            "enjambe " + std::to_string(straddled) +
                                " coupes : le document n'attache une "
                                "transition qu'à une seule");
                continue;
            }
            if (straddled == 0) {
                // The cut may exist in Resolve and be missing here only
                // because a clip beside it was refused further up. Saying
                // "no cut" then would send the editor looking for the wrong
                // thing: the fix is the refusal on the clip.
                int64_t refusedCut = -1;
                for (size_t index = 0; index + 1 < items.size(); ++index) {
                    if (items[index]->record_end !=
                        items[index + 1]->record_start)
                        continue;
                    const int64_t candidate = items[index]->record_end;
                    if (candidate < transition.record_start ||
                        candidate > transition.record_end)
                        continue;
                    refusedCut = candidate;
                    break;
                }
                if (refusedCut >= 0) {
                    refuseNamed(track, label,
                                "fondu sur la coupe de l'image " +
                                    std::to_string(refusedCut) +
                                    ", dont un des deux plans est refusé "
                                    "ci-dessus");
                    continue;
                }
                // R5 -- depuis le 22 septembre, le document exprime un
                // fondu par une couleur sur un seul plan : une transition
                // collée à la tête ou à la queue d'un plan se relit comme
                // un « dip_to_color » noir, qui ne lit aucune poignée.
                // Non mesuré devant Resolve : les rushes étaient hors
                // ligne le jour où c'est écrit.
                size_t opens = kept.size();
                size_t closes = kept.size();
                for (size_t index = 0; index < kept.size(); ++index) {
                    if (kept[index].record_start == transition.record_start &&
                        kept[index].record_end >= transition.record_end)
                        opens = index;
                    if (kept[index].record_end == transition.record_end &&
                        kept[index].record_start <= transition.record_start)
                        closes = index;
                }
                if (opens != kept.size() || closes != kept.size()) {
                    const bool opening = opens != kept.size();
                    const KeptClip& covered = kept[opening ? opens : closes];
                    AddTransitionOperation edgeOperation;
                    edgeOperation.transition.id = GenerateUlid();
                    edgeOperation.transition.track_id = target.second;
                    const InsertClipOperation& clip =
                        built.clips[covered.plan_index];
                    if (opening) {
                        edgeOperation.transition.right_clip_id = clip.clip_id;
                        edgeOperation.transition.alignment =
                            TransitionAlignment::StartAtCut;
                    } else {
                        edgeOperation.transition.left_clip_id = clip.clip_id;
                        edgeOperation.transition.alignment =
                            TransitionAlignment::EndAtCut;
                    }
                    edgeOperation.transition.type = "dip_to_color";
                    edgeOperation.transition.color = "#000000";
                    edgeOperation.transition.duration =
                        RationalTime{frames, manifest.frame_rate};
                    built.transitions.push_back(std::move(edgeOperation));
                    continue;
                }
                refuseNamed(track, label,
                            "transition qui n'enjambe aucune coupe entre deux "
                            "plans adjacents et qui n'est collée ni à la tête "
                            "ni à la queue d'un plan (" +
                                std::to_string(transition.record_start) +
                                " → " + std::to_string(transition.record_end) +
                                ")");
                continue;
            }

            const int64_t pre = cut - transition.record_start;
            const int64_t post = transition.record_end - cut;
            TransitionAlignment alignment = TransitionAlignment::Center;
            if (pre == 0) {
                alignment = TransitionAlignment::StartAtCut;
            } else if (post == 0) {
                alignment = TransitionAlignment::EndAtCut;
            } else if (pre == frames / 2 && post == frames - frames / 2) {
                alignment = TransitionAlignment::Center;
            } else {
                // The document splits a centred transition floor/ceil around
                // the cut, and knows no other share. An 8/4 split is a real
                // montage fact that would land somewhere else if imported as
                // centred, so it is refused rather than moved.
                refuseNamed(track, label,
                            "réparti " + std::to_string(pre) +
                                " image(s) "
                                "avant la coupe et " +
                                std::to_string(post) +
                                " après : le document n'exprime qu'un fondu "
                                "centré, collé au début ou collé à la fin de "
                                "la coupe");
                continue;
            }

            const InsertClipOperation& leftClip =
                built.clips[kept[cutIndex].plan_index];
            const InsertClipOperation& rightClip =
                built.clips[kept[cutIndex + 1].plan_index];
            // R3 -- Document::Validate refuses a dissolve on a clip whose
            // speed is not 1/1, because naming the handle frames exactly on
            // a retimed shot is R5's problem. Refused here by name first, so
            // the relecture says which dissolve rather than failing whole.
            if (!IsUnitSpeed(built.clip_speeds[kept[cutIndex].plan_index]) ||
                !IsUnitSpeed(
                    built.clip_speeds[kept[cutIndex + 1].plan_index])) {
                refuseNamed(track, label,
                            "fondu entre des plans dont la vitesse n'est pas "
                            "1/1 : refusé (R3)");
                continue;
            }

            // The handles the dissolve reads past the cut have to exist in
            // both media. Document::Validate makes the same check; making it
            // here turns a whole-import failure into one named refusal.
            const DocumentSource* leftSource =
                document.FindSource(leftClip.source_id);
            const DocumentSource* rightSource =
                document.FindSource(rightClip.source_id);
            bool handlesFit = leftSource != nullptr && rightSource != nullptr;
            if (handlesFit) {
                try {
                    const RationalTime preTime{pre, manifest.frame_rate};
                    const RationalTime postTime{post, manifest.frame_rate};
                    handlesFit =
                        leftClip.source_in.add(leftClip.duration)
                                .add(postTime) <= leftSource->duration &&
                        rightClip.source_in >= preTime;
                } catch (const std::exception&) {
                    handlesFit = false;
                }
            }
            if (!handlesFit) {
                refuseNamed(track, label,
                            "poignées insuffisantes autour de la coupe (" +
                                std::to_string(pre) + " avant, " +
                                std::to_string(post) +
                                " après) : le fondu "
                                "lirait au-delà du rush");
                continue;
            }

            AddTransitionOperation operation;
            operation.transition.id = GenerateUlid();
            operation.transition.track_id = target.second;
            operation.transition.left_clip_id = leftClip.clip_id;
            operation.transition.right_clip_id = rightClip.clip_id;
            operation.transition.type = "cross_dissolve";
            operation.transition.duration =
                RationalTime{frames, manifest.frame_rate};
            operation.transition.alignment = alignment;
            built.transitions.push_back(std::move(operation));
        }
    }

    if (built.clips.empty() && built.refusals.empty()) {
        error = "timeline « " + manifest.name + " » : aucun plan à relire";
        return false;
    }
    if (!built.refusals.empty()) {
        error = DescribeResolveTimelineRefusals(built.refusals);
        plan = std::move(built);
        return false;
    }
    plan = std::move(built);
    return true;
}

namespace {

std::string RefusalsJson(const std::string& timeline,
                         const std::vector<ResolveTimelineRefusal>& refusals) {
    std::ostringstream output;
    output << "{\"timeline\":\"" << mcp_json::EscapeJsonString(timeline)
           << "\",\"refusals\":[";
    for (size_t index = 0; index < refusals.size(); ++index) {
        if (index) output << ',';
        output << "{\"track\":\""
               << mcp_json::EscapeJsonString(refusals[index].track)
               << "\",\"clip\":\""
               << mcp_json::EscapeJsonString(refusals[index].clip)
               << "\",\"reason\":\""
               << mcp_json::EscapeJsonString(refusals[index].reason) << "\"}";
    }
    output << "]}";
    return output.str();
}

std::string TimelineSuccessJson(const ResolveTimelineRead& manifest,
                                const ResolveTimelineImportPlan& plan,
                                size_t distinctSources,
                                const ResolveGradeReading* grades) {
    size_t videoTracks = 0;
    size_t audioTracks = 0;
    for (const ResolveTimelineTrackRead& track : manifest.tracks)
        (track.kind == "audio" ? audioTracks : videoTracks)++;
    std::ostringstream output;
    output << "{\"ok\":true,\"timeline\":\""
           << mcp_json::EscapeJsonString(manifest.name)
           << "\",\"clips\":" << plan.clips.size()
           << ",\"transitions\":" << plan.transitions.size()
           << ",\"video_tracks\":" << videoTracks
           << ",\"audio_tracks\":" << audioTracks
           << ",\"tracks_created\":" << plan.new_tracks.size()
           << ",\"tracks_reused\":" << plan.reused_tracks
           << ",\"sources\":" << distinctSources
           << ",\"retimed_clips\":" << plan.retimed_clips
           << ",\"unpublished\":[";
    for (size_t index = 0; index < plan.unpublished.size(); ++index) {
        if (index) output << ',';
        output << '"' << mcp_json::EscapeJsonString(plan.unpublished[index])
               << '"';
    }
    output << ']';
    // DEC15 (GR15) -- only when a Resolve database was given: an import
    // without one answers what it always answered.
    if (grades != nullptr)
        output << ",\"grades\":" << SerializeResolveGradeReading(*grades);
    output << "}\n";
    return output.str();
}

}  // namespace

bool MatchResolveTimelineGrades(
    const ResolveTimelineRead& manifest, const ResolveTimelineImportPlan& plan,
    const std::vector<ResolveDatabaseTimeline>& timelines,
    ResolveGradeReading& reading, std::string& error,
    const ResolveProjectSettings& settings) {
    if (timelines.empty()) {
        error = "timeline « " + manifest.name +
                " » absente de la base Resolve : la base est-elle celle du "
                "projet que lit le manifeste ?";
        return false;
    }
    // The clips the import lays on a video track, where Resolve has them:
    // the database counts frames from the timeline's own start, the
    // manifest from zero.
    struct Wanted {
        size_t plan_index = 0;
        const ResolveTimelineTrackRead* track = nullptr;
        const ResolveTimelineItem* item = nullptr;
        size_t item_index = 0;
        int64_t start = 0;
        int64_t duration = 0;
    };
    std::vector<Wanted> wanted;
    for (size_t index = 0;
         index < plan.clips.size() && index < plan.clip_origins.size();
         ++index) {
        const ResolveTimelineImportPlan::ClipOrigin origin =
            plan.clip_origins[index];
        if (origin.track >= manifest.tracks.size()) continue;
        const ResolveTimelineTrackRead& track = manifest.tracks[origin.track];
        if (track.kind != "video" || origin.item >= track.clips.size())
            continue;
        const ResolveTimelineItem& item = track.clips[origin.item];
        wanted.push_back({index, &track, &item, origin.item,
                          manifest.origin + item.record_start,
                          item.record_end - item.record_start});
    }

    struct Match {
        const ResolveDatabaseClip* clip = nullptr;
        std::string reason;
    };
    const auto matchIn = [&](const ResolveDatabaseTimeline& timeline,
                             std::vector<Match>& matches) {
        size_t matched = 0;
        matches.assign(wanted.size(), Match{});
        for (size_t index = 0; index < wanted.size(); ++index) {
            const Wanted& clip = wanted[index];
            std::vector<const ResolveDatabaseClip*> here;
            for (const ResolveDatabaseClip& candidate : timeline.clips)
                if (candidate.whole_frames &&
                    candidate.track == clip.track->index &&
                    candidate.start == clip.start &&
                    candidate.duration == clip.duration)
                    here.push_back(&candidate);
            if (here.size() == 1 && here[0]->path == clip.item->path) {
                matches[index].clip = here[0];
                ++matched;
            } else if (here.empty()) {
                matches[index].reason =
                    "aucun plan de la base à cette place (images " +
                    std::to_string(clip.start) + " à " +
                    std::to_string(clip.start + clip.duration) +
                    ") : la base est-elle plus ancienne que la timeline ?";
            } else if (here.size() > 1) {
                matches[index].reason =
                    std::to_string(here.size()) +
                    " plans de la base à cette place : lequel est étalonné "
                    "n'est pas sûr";
            } else {
                matches[index].reason =
                    "le plan de la base à cette place lit « " + here[0]->path +
                    " »";
            }
        }
        return matched;
    };

    size_t chosen = 0;
    std::vector<Match> matches;
    if (timelines.size() == 1) {
        matchIn(timelines[0], matches);
    } else {
        std::vector<size_t> whole;
        for (size_t index = 0; index < timelines.size(); ++index) {
            std::vector<Match> attempt;
            if (matchIn(timelines[index], attempt) == wanted.size())
                whole.push_back(index);
        }
        if (whole.size() != 1) {
            error = std::to_string(timelines.size()) + " timelines « " +
                    manifest.name + " » dans la base Resolve, et " +
                    (whole.empty() ? std::string("aucune")
                                   : std::to_string(whole.size())) +
                    " ne porte tout le montage relu : laquelle a été "
                    "étalonnée ne se dit pas. Renomme-la dans Resolve, "
                    "puis relis la timeline.";
            return false;
        }
        chosen = whole[0];
        matchIn(timelines[chosen], matches);
    }

    ResolveGradeReading built;
    built.database = reading.database;
    built.settings = settings;
    built.timeline_graded = timelines[chosen].graded;
    if (built.timeline_graded) built.timeline_grade = timelines[chosen].grade;
    for (size_t index = 0; index < wanted.size(); ++index) {
        const Wanted& clip = wanted[index];
        const std::string track = ResolveTrackLabel(*clip.track);
        const std::string label = ClipLabel(*clip.item, clip.item_index);
        if (matches[index].clip == nullptr) {
            built.unmatched.push_back({track, label, matches[index].reason});
            continue;
        }
        if (!matches[index].clip->graded) {
            ++built.ungraded;
            continue;
        }
        ResolveClipGrade graded{plan.clips[clip.plan_index].clip_id, track,
                                label, matches[index].clip->grade};
        ResolveGradeStages(graded.grade, graded.stages, graded.unrendered,
                           settings);
        built.clips.push_back(std::move(graded));
    }
    if (built.timeline_graded) {
        built.timeline_unrendered =
            "l'étalonnage de la timeline elle-même n'est pas écrit : une pile "
            "d'étalonnage appartient à un plan, et rien ne l'applique après "
            "tous les plans";
    }
    reading = std::move(built);
    return true;
}

namespace {

// GR16 -- a stored float as the exact fraction of its shortest decimal
// ("1.106" is 553/500). A decimal of more than nine places (an offset of
// -0.0058384049) is taken to the nearest billionth: a fraction's int32
// cannot hold it, and a stored offset of 1e-9 moves the picture by 2e-10
// of the signal. False only when even that does not fit.
bool StoredFloatFraction(float value, EffectParamValue& fraction) {
    // Below a ten-millionth a stored setting is a zero that drifted.
    if (std::abs(value) < 1e-7f) {
        fraction = {0, 1};
        return true;
    }
    char buffer[48];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value,
                                       std::chars_format::fixed);
    if (written.ec != std::errc{}) return false;
    const std::string text(buffer, written.ptr);
    const size_t point = text.find('.');
    const size_t decimals =
        point == std::string::npos ? 0 : text.size() - point - 1;
    long long numerator = 0;
    long long denominator = 1;
    if (decimals > 9) {
        numerator = std::llround(static_cast<double>(value) * 1e9);
        denominator = 1000000000LL;
    } else {
        const std::string digits =
            point == std::string::npos
                ? text
                : text.substr(0, point) + text.substr(point + 1);
        const auto parsed = std::from_chars(
            digits.data(), digits.data() + digits.size(), numerator);
        if (parsed.ec != std::errc{}) return false;
        for (size_t index = 0; index < decimals; ++index) denominator *= 10;
    }
    const long long divisor = std::gcd(std::llabs(numerator), denominator);
    if (divisor > 1) {
        numerator /= divisor;
        denominator /= divisor;
    }
    constexpr long long kMost = std::numeric_limits<int32_t>::max();
    if (std::llabs(numerator) > kMost || denominator > kMost) return false;
    fraction = {static_cast<int32_t>(numerator),
                static_cast<int32_t>(denominator)};
    return true;
}

// The stage's param for a setting the reading names, or nullptr.
const char* ResolvePrimariesParam(const std::string& setting) {
    static const std::map<std::string, std::string> names = [] {
        // GR16 -- the temperature and the tint since 6 November 2026.
        std::map<std::string, std::string> table = {
            {"saturation", "saturation"},
            {"lum_mix", "lum_mix"},
            {"temperature", "temperature"},
            {"tint", "tint"}};
        for (const char* wheel : {"lift", "gamma", "gain"})
            for (const char* channel : {"r", "g", "b", "master"})
                table[std::string(wheel) + "." + channel] =
                    std::string(wheel) + "_" + channel;
        for (const char* channel : {"r", "g", "b"})
            table[std::string("offset.") + channel] =
                std::string("offset_") + channel;
        return table;
    }();
    const auto found = names.find(setting);
    return found == names.end() ? nullptr : found->second.c_str();
}

std::string ShortestNumber(float value) {
    char buffer[32];
    const auto written = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return written.ec == std::errc{} ? std::string(buffer, written.ptr)
                                     : std::string("?");
}

std::string NodeText(int64_t node) {
    return node == 0 ? std::string("l'étalonnage")
                     : "le nœud " + std::to_string(node);
}

// GR16 -- why a saturation, contrast, temperature or tint does not render
// with these settings, or "" when it does: the colour science it was
// measured in is unknown, or not this one (ResolveScienceRendersBalance).
std::string BalanceUnrendered(const ResolveProjectSettings& settings) {
    if (!settings.read) {
        return settings.unread;
    }
    if (ResolveScienceRendersBalance(settings.color_science)) {
        return {};
    }
    return "dont le rendu n'est mesuré qu'en DaVinci YRGB, en DaVinci YRGB "
           "Color Managed et en ACEScct, pas en " +
           settings.color_science;
}

}  // namespace

void ResolveGradeStages(const ResolveGrade& grade,
                        std::vector<ClipEffect>& stages,
                        std::vector<std::string>& unrendered,
                        const ResolveProjectSettings& settings) {
    stages.clear();
    unrendered.clear();
    for (const ResolveGradeUnread& unread : grade.unread) {
        unrendered.push_back(NodeText(unread.node) + " : " + unread.what +
                             ", que la lecture nomme sans le lire");
    }
    if (!grade.serial) {
        unrendered.push_back(
            "les nœuds ne sont pas câblés en une simple chaîne");
    }
    // The settings the stage does not render yet, at their neutral. The Log
    // wheels' ranges say nothing while the wheels they shape are neutral.
    static const std::map<std::string, float> neutral = {
        {"hue", 0.0f},
        {"highlights", 0.0f},
        {"shadows", 0.0f},
        {"color_boost", 0.0f},
        {"midtone_detail", 0.0f},
        {"log.shadow.r", 0.0f},
        {"log.shadow.g", 0.0f},
        {"log.shadow.b", 0.0f},
        {"log.midtone.r", 0.0f},
        {"log.midtone.g", 0.0f},
        {"log.midtone.b", 0.0f},
        {"log.highlight.r", 0.0f},
        {"log.highlight.g", 0.0f},
        {"log.highlight.b", 0.0f}};
    for (const ResolveGradeNode& node : grade.nodes) {
        if (!node.enabled) continue;  // A node switched off renders nothing.
        ClipEffect stage{{}, "color.resolve_primaries", {}};
        bool contrasted = false;
        bool logWheels = false;
        for (const ResolveGradeSetting& setting : node.settings) {
            if (setting.name == "contrast" && setting.value != 1.0f)
                contrasted = true;
            if (setting.name.rfind("log.", 0) == 0 &&
                setting.name.find("range") == std::string::npos &&
                setting.value != 0.0f)
                logWheels = true;
        }
        for (const ResolveGradeSetting& setting : node.settings) {
            if (const char* param = ResolvePrimariesParam(setting.name)) {
                // GR16 -- the saturation, temperature and tint away from
                // neutral, only in the colour sciences they were measured in.
                const bool balance = setting.name == "saturation"
                                         ? setting.value != 1.0f
                                         : (setting.name == "temperature" ||
                                            setting.name == "tint") &&
                                               setting.value != 0.0f;
                const std::string blocked =
                    balance ? BalanceUnrendered(settings) : std::string();
                if (!blocked.empty()) {
                    unrendered.push_back(
                        NodeText(node.number) + " : " + setting.name + " " +
                        ShortestNumber(setting.value) + ", " + blocked);
                    continue;
                }
                EffectParamValue value;
                if (!StoredFloatFraction(setting.value, value)) {
                    unrendered.push_back(NodeText(node.number) + " : " +
                                         setting.name +
                                         " n'a pas de fraction exacte");
                    continue;
                }
                stage.params[param] = value;
                continue;
            }
            // GR16 -- the contrast and its pivot, with the project's settings;
            // a pivot says nothing while the contrast is neutral.
            if (setting.name == "contrast" || setting.name == "pivot") {
                if (!contrasted) continue;
                const std::string blocked = BalanceUnrendered(settings);
                if (!blocked.empty()) {
                    if (setting.name == "contrast")
                        unrendered.push_back(
                            NodeText(node.number) + " : contrast " +
                            ShortestNumber(setting.value) + ", " + blocked);
                    continue;
                }
                // In ACEScct, the contrast was measured with the aware tools
                // off, as every ACEScct project of the library has them (14
                // November 2026); on, it follows another law.
                if (settings.color_science == "ACEScct" &&
                    settings.color_space_aware) {
                    if (setting.name == "contrast")
                        unrendered.push_back(
                            NodeText(node.number) + " : contrast " +
                            ShortestNumber(setting.value) +
                            ", mesuré en ACEScct outils « color space aware » "
                            "coupés seulement");
                    continue;
                }
                // With the aware tools, the working luminance moves the
                // contrast (by hundreds of codes), and only HDR 1000 was
                // measured; without them it changes nothing.
                if (settings.color_space_aware &&
                    settings.working_luminance != "HDR 1000") {
                    if (setting.name == "contrast")
                        unrendered.push_back(
                            NodeText(node.number) + " : contrast " +
                            ShortestNumber(setting.value) +
                            ", mesuré avec les outils « color space aware » "
                            "en HDR 1000 seulement, pas en " +
                            settings.working_luminance);
                    continue;
                }
                EffectParamValue value;
                if (!StoredFloatFraction(setting.value, value)) {
                    unrendered.push_back(NodeText(node.number) + " : " +
                                         setting.name +
                                         " n'a pas de fraction exacte");
                    continue;
                }
                stage.params[setting.name] = value;
                stage.params["color_space_aware"] = {
                    settings.color_space_aware ? 1 : 0, 1};
                stage.params["contrast_s_curve"] = {
                    settings.contrast_s_curve ? 1 : 0, 1};
                continue;
            }
            if ((setting.name == "log.low_range" ||
                 setting.name == "log.high_range") &&
                !logWheels)
                continue;
            const auto rest = neutral.find(setting.name);
            if (rest != neutral.end() && rest->second == setting.value)
                continue;
            unrendered.push_back(NodeText(node.number) + " : " + setting.name +
                                 " " + ShortestNumber(setting.value) +
                                 ", que color.resolve_primaries ne rend pas "
                                 "encore");
        }
        // GR16 -- a node graded in ACEScct carries it: it grades ACEScct
        // codes there, its gamma passes a negative value, its saturation
        // turns about Rec.709's luma and its temperature and tint act on AP0
        // light (ColorEffects.h).
        if (settings.read && settings.color_science == "ACEScct") {
            stage.params["acescct"] = {1, 1};
        }
        std::string error;
        if (!ValidateColorEffect(stage, error)) {
            unrendered.push_back(NodeText(node.number) + " : " + error);
            continue;
        }
        // A neutral node draws nothing: no stage for it.
        if (ResolveColorGrade({stage}, {}).count == 0) {
            continue;
        }
        // GR16 -- a colour science where not even the lift, gamma and gain
        // were measured (Color Managed v1, ACEScc): nothing is rendered.
        if (settings.read &&
            !ResolveScienceRendersPrimaries(settings.color_science)) {
            unrendered.push_back(NodeText(node.number) + " : en " +
                                 settings.color_science +
                                 ", où color.resolve_primaries n'a jamais été "
                                 "mesurée");
            continue;
        }
        stages.push_back(stage);
    }
    if (!unrendered.empty()) stages.clear();
}

std::string SerializeResolveGradeReading(const ResolveGradeReading& reading) {
    std::ostringstream output;
    output << "{\"database\":\"" << mcp_json::EscapeJsonString(reading.database)
           << "\",\"note\":\"valeurs telles que Resolve les stocke ; un plan "
              "dont written_stages n'est pas nul porte déjà son étalonnage en "
              "color.resolve_primaries, rendu comme Resolve le rend\""
           << ",\"timeline_grade\":"
           << (reading.timeline_graded
                   ? SerializeResolveGrade(reading.timeline_grade)
                   : std::string("null"))
           << ",\"clips\":[";
    for (size_t index = 0; index < reading.clips.size(); ++index) {
        const ResolveClipGrade& clip = reading.clips[index];
        if (index) output << ',';
        output << "{\"clip_id\":\"" << mcp_json::EscapeJsonString(clip.clip_id)
               << "\",\"track\":\"" << mcp_json::EscapeJsonString(clip.track)
               << "\",\"clip\":\"" << mcp_json::EscapeJsonString(clip.clip)
               << "\",\"grade\":"
               << SerializeResolveGrade(clip.grade)
               // GR16 -- what the import wrote on the clip, or why nothing.
               << ",\"written_stages\":" << clip.stages.size()
               << ",\"unrendered\":[";
        for (size_t reason = 0; reason < clip.unrendered.size(); ++reason) {
            if (reason != 0) output << ',';
            output << '"' << mcp_json::EscapeJsonString(clip.unrendered[reason])
                   << '"';
        }
        output << "]}";
    }
    size_t written = 0;
    for (const ResolveClipGrade& clip : reading.clips)
        if (!clip.stages.empty()) ++written;
    output
        << "],\"written\":" << written << ",\"timeline_unrendered\":"
        << (reading.timeline_unrendered.empty()
                ? std::string("null")
                : "\"" +
                      mcp_json::EscapeJsonString(reading.timeline_unrendered) +
                      "\"")
        // GR16 -- the settings a contrast was rendered with, or why none.
        << ",\"project_settings\":{\"read\":"
        << (reading.settings.read ? "true" : "false")
        << ",\"color_space_aware\":"
        << (reading.settings.color_space_aware ? "true" : "false")
        << ",\"contrast_s_curve\":"
        << (reading.settings.contrast_s_curve ? "true" : "false")
        << ",\"working_luminance\":\""
        << mcp_json::EscapeJsonString(reading.settings.working_luminance) << '"'
        << ",\"color_science\":\""
        << mcp_json::EscapeJsonString(reading.settings.color_science) << '"'
        << ",\"unread\":"
        << (reading.settings.unread.empty()
                ? std::string("null")
                : "\"" + mcp_json::EscapeJsonString(reading.settings.unread) +
                      "\"")
        << "}"
        << ",\"ungraded\":" << reading.ungraded << ",\"unmatched\":[";
    for (size_t index = 0; index < reading.unmatched.size(); ++index) {
        if (index) output << ',';
        output << "{\"track\":\""
               << mcp_json::EscapeJsonString(reading.unmatched[index].track)
               << "\",\"clip\":\""
               << mcp_json::EscapeJsonString(reading.unmatched[index].clip)
               << "\",\"reason\":\""
               << mcp_json::EscapeJsonString(reading.unmatched[index].reason)
               << "\"}";
    }
    output << "]}";
    return output.str();
}

int ReadResolveTimelineImportInput(const std::string& manifestPath,
                                   const std::string& resolveDatabase,
                                   ResolveTimelineImportInput& input,
                                   std::string& output) {
    input = ResolveTimelineImportInput{};
    std::string reason;
    std::string manifestText;
    if (!ReadWholeFile(std::filesystem::path(manifestPath), manifestText,
                       reason)) {
        return FailCliCommand("IoError", reason, output);
    }
    if (!ParseResolveTimelineManifest(manifestText, input.manifest, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    // DEC15 (GR15) -- read before anything is planned or written: a
    // database that cannot be read refuses the call as a whole.
    if (!resolveDatabase.empty()) {
        std::error_code exists;
        if (!std::filesystem::is_regular_file(resolveDatabase, exists)) {
            return FailCliCommand(
                "IoError", "base Resolve introuvable : " + resolveDatabase,
                output);
        }
        if (!ReadResolveDatabaseTimelines(resolveDatabase, input.manifest.name,
                                          input.database_timelines, reason,
                                          &input.settings)) {
            return FailCliCommand("ParseError", reason, output);
        }
        input.database = resolveDatabase;
    }
    return 0;
}

int PlanResolveTimelineImportStep(const Document& document,
                                  const std::string& projectRoot,
                                  const ResolveTimelineImportInput& input,
                                  ResolveTimelineImportStaged& staged,
                                  std::string& output) {
    staged = ResolveTimelineImportStaged{};
    std::string reason;
    if (!PlanResolveTimelineImport(document, projectRoot, input.manifest,
                                   staged.plan, reason)) {
        // Nothing has been applied and nothing is written: a partial document
        // is exactly the state the ticket refuses.
        return FailCliCommandWithResult(
            "ValidationFailed", reason,
            staged.plan.refusals.empty()
                ? std::string()
                : RefusalsJson(input.manifest.name, staged.plan.refusals),
            output);
    }
    if (!input.database.empty()) {
        staged.grades.database = input.database;
        if (!MatchResolveTimelineGrades(input.manifest, staged.plan,
                                        input.database_timelines, staged.grades,
                                        reason, input.settings)) {
            return FailCliCommand("ValidationFailed", reason, output);
        }
    }
    return 0;
}

int ApplyResolveTimelineImportStep(Document& document, EditLog& log,
                                   const ResolveTimelineImportInput& input,
                                   const ResolveTimelineImportStaged& staged,
                                   std::string& output) {
    const ResolveTimelineImportPlan& plan = staged.plan;
    std::string reason;
    EditError error = EditError::None;
    UpdateSequenceOperation sequence = plan.sequence;
    if (!log.Apply(document, sequence, error, reason)) {
        return FailCliCommand(EditErrorName(error), reason, output);
    }
    for (AddTrackOperation operation : plan.new_tracks) {
        if (!log.Apply(document, operation, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
    }
    std::set<Ulid> distinctSources;
    for (size_t index = 0; index < plan.clips.size(); ++index) {
        InsertClipOperation operation = plan.clips[index];
        distinctSources.insert(operation.source_id);
        if (!log.Apply(document, operation, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
        // The speed rides a second operation because the clip's id exists
        // only once the insert has been applied. Unit speeds write nothing,
        // so a montage without slow motion produces the same journal it did
        // before R3 touched this path.
        const EffectParamValue speed = index < plan.clip_speeds.size()
                                           ? plan.clip_speeds[index]
                                           : EffectParamValue{1, 1};
        if (IsUnitSpeed(speed)) continue;
        SetClipSpeedOperation retime;
        retime.clip_id = plan.clips[index].clip_id;
        retime.speed = speed;
        if (!log.Apply(document, retime, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
    }
    // R10 -- after every clip, because a transition names two of them by id
    // and ApplyAddTransition validates that they exist, are adjacent and
    // share a cut.
    for (AddTransitionOperation operation : plan.transitions) {
        if (!log.Apply(document, operation, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
    }
    // GR16 -- the grades that render whole, one SetClipEffects per clip:
    // the imported nodes first, as the base of the grade, then whatever the
    // clip carried. A grade that does not render writes nothing.
    for (const ResolveClipGrade& clip : staged.grades.clips) {
        if (clip.stages.empty()) continue;
        const DocumentClip* target = document.FindClip(clip.clip_id);
        if (target == nullptr) continue;
        SetClipEffectsOperation operation;
        operation.clip_id = clip.clip_id;
        operation.effects = clip.stages;
        operation.effects.insert(operation.effects.end(),
                                 target->effects.begin(),
                                 target->effects.end());
        if (!log.Apply(document, operation, error, reason)) {
            return FailCliCommand(EditErrorName(error), reason, output);
        }
    }

    if (!document.Validate(reason)) {
        return FailCliCommand("ValidationFailed", reason, output);
    }
    output =
        TimelineSuccessJson(input.manifest, plan, distinctSources.size(),
                            input.database.empty() ? nullptr : &staged.grades);
    return 0;
}

int ImportResolveTimelineCommand(const std::string& projectPath,
                                 const std::string& manifestPath,
                                 std::string& output,
                                 const std::string& timelineId,
                                 const std::string& resolveDatabase) {
    ResolveTimelineImportInput input;
    if (const int status = ReadResolveTimelineImportInput(
            manifestPath, resolveDatabase, input, output)) {
        return status;
    }

    std::string reason;
    Project project;
    if (!LoadStoredProject(projectPath, project, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    const Ulid target =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (project.FindTimeline(target) == nullptr) {
        return FailCliCommand("UnknownSequence",
                              "unknown timeline_id '" + target + "'", output);
    }
    Document document = project.MakeDocument(target);

    ResolveTimelineImportStaged staged;
    const std::string projectRoot =
        std::filesystem::path(projectPath).parent_path().string();
    if (const int status = PlanResolveTimelineImportStep(
            document, projectRoot, input, staged, output)) {
        return status;
    }

    std::map<std::string, EditLog> logs;
    ProjectEditLog projectLog;
    if (!LoadOptionalLogs(projectPath, project, logs, projectLog, reason)) {
        return FailCliCommand("ParseError", reason, output);
    }
    std::string answer;
    if (const int status = ApplyResolveTimelineImportStep(
            document, logs[target], input, staged, answer)) {
        output = answer;
        return status;
    }
    if (!project.CommitDocument(target, document, reason) ||
        !CommitStoredProjectAndLogs(projectPath, project, logs, projectLog,
                                    reason)) {
        return FailCliCommand("IoError", reason, output);
    }
    output = answer;
    return 0;
}
