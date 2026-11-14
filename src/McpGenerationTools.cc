#include "McpGenerationTools.h"

#include "ColorManagement.h"
#include "Document.h"
#include "FrameCapture.h"
#include "Operations.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <system_error>
#include <utility>

namespace {

using mcp_json::Value;
namespace fs = std::filesystem;

bool Refuse(std::string& errorName, std::string& message,
            const std::string& name, std::string text) {
    errorName = name;
    message = std::move(text);
    return false;
}

bool Invalid(std::string& errorName, std::string& message, std::string text) {
    return Refuse(errorName, message, "ValidationFailed", std::move(text));
}

std::string Quoted(const std::string& text) {
    return "\"" + mcp_json::EscapeJsonString(text) + "\"";
}

std::string Described(const std::string& type, const std::string& text) {
    return "{\"type\":\"" + type + "\",\"description\":" + Quoted(text) + "}";
}

// PRD15 -- McpTools.cc's kTimeSchemaText, with what the time names.
const char kSourceTime[] =
    "{\"type\":\"object\",\"description\":\"expand_image, avatar: the frame "
    "of a video to start from. lipsync, dubbing, edit_video: where the "
    "stretch starts. "
    "In the media's own time. Exact time: value ticks of 1/rate s.\","
    "\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";

// PA12 -- where a stretch of a media starts and how long it lasts.
const char kStretchStart[] =
    "{\"type\":\"object\",\"description\":\"Where the stretch of the "
    "sound starts, in its media's own time; its start when left out. Exact "
    "time: value ticks of 1/rate s.\","
    "\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";
const char kStretchLength[] =
    "{\"type\":\"object\",\"description\":\"How long the stretch lasts; "
    "to the media's end when left out, 300 s at most. Exact time: value "
    "ticks of 1/rate s.\","
    "\"properties\":{\"value\":{\"type\":\"integer\"},"
    "\"rate\":{\"type\":\"integer\",\"minimum\":1}},\"required\":[\"value\","
    "\"rate\"],\"additionalProperties\":false}";

std::string Schema(
    const std::vector<std::pair<std::string, std::string>>& properties,
    const std::vector<std::string>& required, const std::string& description) {
    std::string json =
        "{\"type\":\"object\",\"description\":" + Quoted(description) +
        ",\"properties\":{";
    for (size_t index = 0; index < properties.size(); ++index) {
        json += (index != 0 ? "," : "") + Quoted(properties[index].first) +
                ":" + properties[index].second;
    }
    json += "}";
    for (size_t index = 0; index < required.size(); ++index) {
        json +=
            (index != 0 ? "," : ",\"required\":[") + Quoted(required[index]);
    }
    if (!required.empty()) json += "]";
    return json + ",\"additionalProperties\":false}";
}

bool Known(const Value& args, const std::vector<std::string>& allowed,
           const std::string& path, std::string& message) {
    if (!args.IsObject()) {
        message = "'" + path + "' arguments must be a JSON object";
        return false;
    }
    for (const auto& entry : args.AsObject()) {
        if (std::find(allowed.begin(), allowed.end(), entry.first) ==
            allowed.end()) {
            message =
                "'" + path + "' has unknown argument '" + entry.first + "'";
            return false;
        }
    }
    return true;
}

bool ReadText(const Value& args, const std::string& key,
              const std::string& path, bool required, std::string& out,
              std::string& message) {
    const Value* field = args.Find(key);
    if (field == nullptr) {
        if (required) message = "'" + path + "." + key + "' is required";
        return !required;
    }
    if (!field->IsString()) {
        message = "'" + path + "." + key + "' must be a string";
        return false;
    }
    out = field->AsString();
    return true;
}

bool ReadTime(const Value& args, const std::string& key,
              const std::string& path, RationalTime& out, bool& present,
              std::string& message) {
    const Value* field = args.Find(key);
    present = field != nullptr;
    if (field == nullptr) return true;
    const Value* value = field->Find("value");
    const Value* rate = field->Find("rate");
    int64_t ticks = 0;
    int64_t perSecond = 0;
    if (!field->IsObject() || field->AsObject().size() != 2 ||
        value == nullptr || rate == nullptr || !value->AsInt64(ticks) ||
        !rate->AsInt64(perSecond) || perSecond <= 0 ||
        perSecond > std::numeric_limits<int32_t>::max() || ticks < 0) {
        message = "'" + path + "." + key +
                  "' must be {value, rate}: exact integers, rate positive, "
                  "value not negative";
        return false;
    }
    out = RationalTime{ticks, static_cast<int32_t>(perSecond)};
    return true;
}

// A seed as the person or the agent gave it: a non-negative integer, kept
// as its decimal spelling (MediaGeneration records text).
bool ReadSeed(const Value& args, const std::string& path, std::string& seed,
              std::string& message) {
    const Value* field = args.Find("seed");
    if (field == nullptr) return true;
    int64_t value = 0;
    if (!field->IsNumber() || !field->AsInt64(value) || value < 0) {
        message = "'" + path + ".seed' must be a non-negative integer";
        return false;
    }
    seed = std::to_string(value);
    return true;
}

std::string Lowercase(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](char c) {
        return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    });
    return text;
}

// A still the model can read as it is; anything else is a video whose frame
// is taken.
std::string StillContentType(const std::string& path) {
    const std::string extension =
        Lowercase(fs::path(path).extension().string());
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".webp") return "image/webp";
    return {};
}

std::string ExtensionFor(const std::string& contentType,
                         const std::string& url) {
    const std::string type = Lowercase(contentType);
    if (type.rfind("image/png", 0) == 0) return ".png";
    if (type.rfind("image/jpeg", 0) == 0) return ".jpg";
    if (type.rfind("image/webp", 0) == 0) return ".webp";
    std::string tail = url.substr(0, url.find('?'));
    const std::string extension =
        Lowercase(fs::path(tail).extension().string());
    if (extension == ".png" || extension == ".jpg" || extension == ".jpeg" ||
        extension == ".webp")
        return extension == ".jpeg" ? ".jpg" : extension;
    return ".png";
}

bool ReadFileBytes(const std::string& path, std::string& bytes,
                   std::string& message) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        message = "impossible de lire '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << input.rdbuf();
    bytes = buffer.str();
    return true;
}

// Written beside its final name, then renamed: the library never sees half
// a picture.
bool WriteFileBytes(const fs::path& path, const std::string& bytes,
                    std::string& message) {
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    if (error) {
        message = "impossible de créer '" + path.parent_path().string() +
                  "' : " + error.message();
        return false;
    }
    const fs::path partial = path.string() + ".partial";
    {
        std::ofstream output(partial, std::ios::binary | std::ios::trunc);
        output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!output) {
            message = "impossible d'écrire '" + partial.string() + "'";
            return false;
        }
    }
    fs::rename(partial, path, error);
    if (error) {
        message =
            "impossible de nommer '" + path.string() + "' : " + error.message();
        return false;
    }
    return true;
}

std::string AbsoluteMediaPath(const std::string& packageDirectory,
                              const LibraryMedia& media) {
    fs::path path(media.path);
    if (path.is_relative()) path = fs::path(packageDirectory) / path;
    return path.lexically_normal().string();
}

generation::HttpTransport InstalledTransport() {
    const generation::NativeHttp native =
        generation::CurrentGenerationTransport();
    if (native == nullptr) return {};
    return [native](const generation::HttpRequest& request,
                    generation::HttpResponse& response, std::string& error) {
        return native(request, response, error);
    };
}

// ---------------------------------------------------------------------
// PA12 -- the creative kinds (Generation.h): stock_video, stock_image,
// avatar, lipsync, dubbing.
// ---------------------------------------------------------------------

std::string VideoExtensionFor(const std::string& contentType,
                              const std::string& url) {
    const std::string type = Lowercase(contentType);
    if (type.rfind("video/quicktime", 0) == 0) return ".mov";
    if (type.rfind("video/webm", 0) == 0) return ".webm";
    const std::string tail = url.substr(0, url.find('?'));
    const std::string extension =
        Lowercase(fs::path(tail).extension().string());
    if (extension == ".mov" || extension == ".webm") return extension;
    return ".mp4";
}

// A stretch of a media: `from` (the media's start when left out) and
// `length` (to its end when left out), inside the media and no longer than
// kMaxExcerptSeconds.
bool ReadStretch(const Value& args, const std::string& tool,
                 const std::string& fromKey, const std::string& lengthKey,
                 const LibraryMedia& media, RationalTime& from,
                 RationalTime& length, std::string& message) {
    bool hasFrom = false;
    bool hasLength = false;
    from = RationalTime{0, 1};
    if (!ReadTime(args, fromKey, tool, from, hasFrom, message) ||
        !ReadTime(args, lengthKey, tool, length, hasLength, message))
        return false;
    if (media.duration.rate <= 0 || media.duration.value <= 0) {
        message = "media '" + media.id + "' has no known duration";
        return false;
    }
    if (from >= media.duration) {
        message = "'" + tool + "." + fromKey + "' is past the end of media '" +
                  media.id + "'";
        return false;
    }
    if (!hasLength) length = media.duration.sub(from);
    if (length.value <= 0 || from.add(length) > media.duration) {
        message = "'" + tool + "." + lengthKey +
                  "' must be longer than zero and end inside media '" +
                  media.id + "'";
        return false;
    }
    if (length > RationalTime{generation::kMaxExcerptSeconds, 1}) {
        message = "the stretch of media '" + media.id + "' is longer than " +
                  std::to_string(generation::kMaxExcerptSeconds) +
                  " s, the most one request sends: name a shorter '" +
                  lengthKey + "'";
        return false;
    }
    return true;
}

bool ProposeCreative(McpBackend& backend, const IdResolver& resolver,
                     const Value& args, const std::string& kindName,
                     generation::GenerationProposalStore& proposals,
                     std::string& resultJson, std::string& errorName,
                     std::string& message) {
    const std::string tool = "propose_generation";
    const generation::GenerationKind* kind =
        generation::FindGenerationKind(kindName);
    std::string prompt;
    std::string negativePrompt;
    std::string seed;
    if (!ReadText(args, "prompt", tool, false, prompt, message) ||
        !ReadText(args, "negative_prompt", tool, false, negativePrompt,
                  message) ||
        !ReadSeed(args, tool, seed, message))
        return Invalid(errorName, message, message);
    // An argument of another kind is a mistake said now, not a setting
    // silently ignored once the money is spent.
    const auto only = [&](const std::vector<std::string>& allowed) {
        for (const auto& entry : args.AsObject()) {
            if (entry.first == "kind" || entry.first == "prompt") continue;
            if (std::find(allowed.begin(), allowed.end(), entry.first) ==
                allowed.end()) {
                message = "'" + tool + "." + entry.first +
                          "' is not read by '" + kindName + "'";
                return false;
            }
        }
        return true;
    };

    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Refuse(errorName, message, "IoError", message);
    const std::string packageDirectory = backend.PackageDirectory();
    if (packageDirectory.empty())
        return Refuse(errorName, message, "UnsupportedOperation",
                      "this backend has no project package to write a "
                      "generated file into: open a project first");
    // A media named by `key`, with its file on disk.
    const auto mediaAt = [&](const std::string& key, const LibraryMedia*& media,
                             std::string& path) {
        std::string text;
        if (!ReadText(args, key, tool, false, text, message))
            return Invalid(errorName, message, message);
        if (text.empty())
            return Invalid(errorName, message,
                           "kind '" + kindName + "' needs '" + key + "'");
        Ulid id;
        if (!resolver.Resolve(tool + "." + key, text, id, message))
            return Invalid(errorName, message, message);
        media = document.FindLibraryMedia(id);
        if (media == nullptr)
            return Refuse(errorName, message, "UnknownMedia",
                          "unknown " + key + " '" + id + "'");
        path = AbsoluteMediaPath(packageDirectory, *media);
        std::error_code existsError;
        if (!fs::exists(path, existsError))
            return Refuse(
                errorName, message, "IoError",
                "the file of media '" + id + "' is missing: '" + path + "'");
        return true;
    };

    generation::GenerationProposal proposal;
    proposal.input = Value::MakeObject();
    proposal.needs_consent = kind->acts_on_filmed_person;
    // PA14 -- what the request will cost, when the published rule lets the
    // engine work it out.
    std::string estimatedCost;
    if (kindName == "stock_video") {
        if (!only({"negative_prompt", "duration_seconds", "aspect_ratio"}))
            return Invalid(errorName, message, message);
        if (prompt.empty())
            return Invalid(errorName, message,
                           "'" + tool + ".prompt' says what the shot shows");
        int64_t seconds = 5;
        if (const Value* field = args.Find("duration_seconds")) {
            if (!field->IsNumber() || !field->AsInt64(seconds) || seconds < 3 ||
                seconds > 15)
                return Invalid(errorName, message,
                               "'" + tool +
                                   ".duration_seconds' is a whole number "
                                   "from 3 to 15");
        }
        std::string aspect;
        if (!ReadText(args, "aspect_ratio", tool, false, aspect, message))
            return Invalid(errorName, message, message);
        if (aspect.empty()) {
            // The timeline's own shape, when the model makes it.
            const int64_t width = document.sequence.width;
            const int64_t height = document.sequence.height;
            aspect = width * 9 == height * 16   ? "16:9"
                     : width * 16 == height * 9 ? "9:16"
                     : width == height          ? "1:1"
                                                : "";
        }
        if (aspect != "16:9" && aspect != "9:16" && aspect != "1:1")
            return Invalid(errorName, message,
                           "'" + tool +
                               ".aspect_ratio' must be \"16:9\", \"9:16\" or "
                               "\"1:1\", the shapes the model makes (the "
                               "timeline's is none of them: name one)");
        proposal.input.Set("prompt", Value::MakeString(prompt));
        proposal.input.Set("duration",
                           Value::MakeString(std::to_string(seconds)));
        proposal.input.Set("aspect_ratio", Value::MakeString(aspect));
        // A stock shot is a picture: the film's sound is the film's.
        proposal.input.Set("generate_audio", Value::MakeBool(false));
        if (!negativePrompt.empty())
            proposal.input.Set("negative_prompt",
                               Value::MakeString(negativePrompt));
    } else if (kindName == "stock_image") {
        if (!only({"seed"})) return Invalid(errorName, message, message);
        if (prompt.empty())
            return Invalid(errorName, message,
                           "'" + tool + ".prompt' says what the picture shows");
        Value size = Value::MakeObject();
        size.Set("width", Value::MakeInt(document.sequence.width));
        size.Set("height", Value::MakeInt(document.sequence.height));
        proposal.input.Set("prompt", Value::MakeString(prompt));
        // At the timeline's own size: the picture is made for this film.
        proposal.input.Set("image_size", std::move(size));
        proposal.input.Set("output_format", Value::MakeString("png"));
        if (!seed.empty())
            proposal.input.Set("seed", Value::MakeRawNumber(seed));
    } else if (kindName == "dubbing") {
        if (!only({"media_id", "source_time", "duration", "target_language"}))
            return Invalid(errorName, message, message);
        const LibraryMedia* media = nullptr;
        std::string path;
        if (!mediaAt("media_id", media, path)) return false;
        if (!media->has_audio)
            return Invalid(errorName, message,
                           "media '" + media->id +
                               "' has no sound to put in another language");
        std::string language;
        if (!ReadText(args, "target_language", tool, false, language, message))
            return Invalid(errorName, message, message);
        if (language.size() != 2 ||
            !std::islower(static_cast<unsigned char>(language[0])) ||
            !std::islower(static_cast<unsigned char>(language[1])))
            return Invalid(errorName, message,
                           "'" + tool +
                               ".target_language' is a two-letter language "
                               "code, as \"en\" or \"es\"");
        generation::GenerationUpload upload;
        upload.field = media->has_video ? "video_url" : "audio_url";
        upload.media_id = media->id;
        upload.path = path;
        upload.what = media->has_video ? "excerpt" : "sound";
        if (!ReadStretch(args, tool, "source_time", "duration", *media,
                         upload.time, upload.duration, message))
            return Invalid(errorName, message, message);
        proposal.uploads.push_back(std::move(upload));
        proposal.input.Set("target_lang", Value::MakeString(language));
        proposal.source_media_id = media->id;
    } else if (kindName == "edit_video") {
        // PA14 -- Kling 4.0 edit: a stretch of a shot, the words that say
        // what changes, and the pictures of what it becomes.
        if (!only({"media_id", "source_time", "duration", "reference_media_ids",
                   "resolution"}))
            return Invalid(errorName, message, message);
        if (prompt.empty())
            return Invalid(errorName, message,
                           "'" + tool +
                               ".prompt' says what changes and what stays; "
                               "the shot is @Video1, the reference pictures "
                               "@Image1, @Image2... in order");
        const LibraryMedia* media = nullptr;
        std::string path;
        if (!mediaAt("media_id", media, path)) return false;
        if ((media->metadata_complete && !media->has_video) ||
            !StillContentType(path).empty())
            return Invalid(errorName, message,
                           "media '" + media->id +
                               "' is not a video: 'media_id' is the shot to "
                               "edit, 'reference_media_ids' the pictures");
        generation::GenerationUpload shot;
        shot.field = "video_url";
        shot.media_id = media->id;
        shot.path = path;
        shot.what = "excerpt";
        if (!ReadStretch(args, tool, "source_time", "duration", *media,
                         shot.time, shot.duration, message))
            return Invalid(errorName, message, message);
        // The model's own bounds, said before anything is sent.
        if (shot.duration < RationalTime{generation::kEditVideoMinSeconds, 1} ||
            shot.duration > RationalTime{generation::kEditVideoMaxSeconds, 1})
            return Invalid(
                errorName, message,
                "'edit_video' reads a stretch of " +
                    std::to_string(generation::kEditVideoMinSeconds) + " to " +
                    std::to_string(generation::kEditVideoMaxSeconds) +
                    " seconds: name 'source_time' and 'duration' inside "
                    "media '" +
                    media->id + "'");
        if (media->width > 0 && media->height > 0) {
            // What the excerpt holds: the shot, its long side at 1920 at
            // most (EncodeExcerpt).
            double width = static_cast<double>(media->width);
            double height = static_cast<double>(media->height);
            const double longSide = std::max(width, height);
            if (longSide > 1920.0) {
                width *= 1920.0 / longSide;
                height *= 1920.0 / longSide;
            }
            if (std::min(width, height) <
                static_cast<double>(generation::kEditVideoMinSide))
                return Invalid(
                    errorName, message,
                    "media '" + media->id + "' is " +
                        std::to_string(media->width) + "x" +
                        std::to_string(media->height) +
                        ": 'edit_video' needs " +
                        std::to_string(generation::kEditVideoMinSide) +
                        " pixels on its short side");
        }
        // DEC51 -- a shot and pictures the engine generated show nobody who
        // was filmed; any other may, and then needs their consent.
        bool allGenerated = IsGenerated(media->generation);
        std::vector<generation::GenerationUpload> pictures;
        if (const Value* list = args.Find("reference_media_ids")) {
            if (!list->IsArray() ||
                list->AsArray().size() > generation::kEditVideoMaxReferences)
                return Invalid(
                    errorName, message,
                    "'" + tool + ".reference_media_ids' lists at most " +
                        std::to_string(generation::kEditVideoMaxReferences) +
                        " stills of the library");
            for (const Value& item : list->AsArray()) {
                if (!item.IsString())
                    return Invalid(errorName, message,
                                   "'" + tool +
                                       ".reference_media_ids' holds media "
                                       "ids");
                Ulid id;
                if (!resolver.Resolve(tool + ".reference_media_ids",
                                      item.AsString(), id, message))
                    return Invalid(errorName, message, message);
                const LibraryMedia* picture = document.FindLibraryMedia(id);
                if (picture == nullptr)
                    return Refuse(errorName, message, "UnknownMedia",
                                  "unknown reference media '" + id + "'");
                const std::string picturePath =
                    AbsoluteMediaPath(packageDirectory, *picture);
                if (StillContentType(picturePath).empty())
                    return Invalid(errorName, message,
                                   "reference media '" + id +
                                       "' is not a still (PNG, JPEG or "
                                       "WebP): ingest a picture, or generate "
                                       "one (stock_image)");
                std::error_code existsError;
                if (!fs::exists(picturePath, existsError))
                    return Refuse(errorName, message, "IoError",
                                  "the file of media '" + id +
                                      "' is missing: '" + picturePath + "'");
                for (const generation::GenerationUpload& known : pictures) {
                    if (known.media_id == id)
                        return Invalid(
                            errorName, message,
                            "reference media '" + id + "' is listed twice");
                }
                allGenerated = allGenerated && IsGenerated(picture->generation);
                generation::GenerationUpload upload;
                upload.field = "image_urls";
                upload.media_id = id;
                upload.path = picturePath;
                upload.what = "still";
                pictures.push_back(std::move(upload));
            }
        }
        std::string resolution;
        if (!ReadText(args, "resolution", tool, false, resolution, message))
            return Invalid(errorName, message, message);
        if (resolution.empty()) resolution = "1080p";
        if (resolution != "720p" && resolution != "1080p")
            return Invalid(errorName, message,
                           "'" + tool +
                               ".resolution' is \"720p\" or \"1080p\"; "
                               "default \"1080p\", the model's own");
        // What it will be billed, by the published rule: the rate times
        // the input plus the output, in whole seconds.
        const int64_t seconds =
            (shot.duration.value + shot.duration.rate - 1) / shot.duration.rate;
        const int64_t cents =
            (generation::EditVideoMilliDollars(seconds, resolution) + 5) / 10;
        char estimate[160];
        std::snprintf(estimate, sizeof(estimate),
                      "environ %lld,%02lld $ : %lld s d'entrée plus %lld s de "
                      "sortie en %s, au prix annoncé",
                      static_cast<long long>(cents / 100),
                      static_cast<long long>(cents % 100),
                      static_cast<long long>(seconds),
                      static_cast<long long>(seconds), resolution.c_str());
        estimatedCost = estimate;
        proposal.uploads.push_back(std::move(shot));
        for (generation::GenerationUpload& upload : pictures)
            proposal.uploads.push_back(std::move(upload));
        proposal.input.Set("prompt", Value::MakeString(prompt));
        proposal.input.Set("resolution", Value::MakeString(resolution));
        proposal.needs_consent = !allGenerated;
        proposal.source_media_id = media->id;
    } else {
        // avatar and lipsync: a picture of someone, and the sound they say.
        if (!only({"media_id", "source_time", "duration", "audio_media_id",
                   "audio_source_time", "audio_duration"}))
            return Invalid(errorName, message, message);
        const LibraryMedia* media = nullptr;
        std::string path;
        if (!mediaAt("media_id", media, path)) return false;
        if (media->metadata_complete && !media->has_video)
            return Invalid(errorName, message,
                           "media '" + media->id +
                               "' is audio-only: 'media_id' is the picture, "
                               "'audio_media_id' the sound");
        generation::GenerationUpload picture;
        picture.media_id = media->id;
        picture.path = path;
        if (kindName == "avatar") {
            const bool still = !StillContentType(path).empty();
            bool hasTime = false;
            if (!ReadTime(args, "source_time", tool, picture.time, hasTime,
                          message))
                return Invalid(errorName, message, message);
            if (args.Find("duration") != nullptr)
                return Invalid(errorName, message,
                               "'" + tool +
                                   ".duration' is not read by 'avatar': the "
                                   "picture is one frame, and the film lasts "
                                   "as long as the sound");
            if (still && hasTime)
                return Invalid(errorName, message,
                               "media '" + media->id +
                                   "' is a still: leave 'source_time' out");
            if (!still && !hasTime)
                return Invalid(errorName, message,
                               "media '" + media->id +
                                   "' is a video: 'source_time' names the "
                                   "frame the avatar is made from");
            if (hasTime && media->duration.rate > 0 &&
                picture.time >= media->duration)
                return Invalid(errorName, message,
                               "'" + tool +
                                   ".source_time' is past the end of media '" +
                                   media->id + "'");
            picture.field = "image_url";
            picture.what = still ? "still" : "frame";
            // DEC51 -- a picture the engine generated shows nobody who was
            // filmed; any other may, and then needs their consent.
            proposal.needs_consent = !IsGenerated(media->generation);
        } else {
            picture.field = "video_url";
            picture.what = "excerpt";
            if (!ReadStretch(args, tool, "source_time", "duration", *media,
                             picture.time, picture.duration, message))
                return Invalid(errorName, message, message);
        }
        const LibraryMedia* sound = nullptr;
        std::string soundPath;
        if (!mediaAt("audio_media_id", sound, soundPath)) return false;
        if (!sound->has_audio)
            return Invalid(errorName, message,
                           "media '" + sound->id + "' has no sound");
        generation::GenerationUpload voice;
        voice.field = "audio_url";
        voice.media_id = sound->id;
        voice.path = soundPath;
        voice.what = "sound";
        if (!ReadStretch(args, tool, "audio_source_time", "audio_duration",
                         *sound, voice.time, voice.duration, message))
            return Invalid(errorName, message, message);
        proposal.uploads.push_back(std::move(picture));
        proposal.uploads.push_back(std::move(voice));
        if (kindName == "avatar") {
            if (!prompt.empty())
                proposal.input.Set("prompt", Value::MakeString(prompt));
        } else {
            // The sound is the measure: a picture that runs short is cut
            // off with it, never looped.
            proposal.input.Set("model", Value::MakeString("lipsync-2"));
            proposal.input.Set("sync_mode", Value::MakeString("cut_off"));
        }
        proposal.source_media_id = media->id;
    }

    proposal.id = GenerateUlid();
    proposal.kind = kind->kind;
    proposal.service = kind->service;
    proposal.model = kind->model;
    proposal.announced_cost = kind->announced_cost;
    proposal.prompt = prompt;
    proposal.negative_prompt = negativePrompt;
    proposal.seed = seed;
    proposal.proposed_at = generation::UtcNow();
    proposal.created = std::chrono::steady_clock::now();
    proposals.Add(proposal);

    std::string key;
    std::string origin;
    std::string keyMessage;
    const bool keyConfigured =
        generation::ResolveFalKey(key, origin, keyMessage);
    Value reply = generation::ProposalValue(
        proposal, keyConfigured,
        generation::CurrentGenerationTransport() != nullptr);
    if (!estimatedCost.empty())
        reply.Set("estimated_cost", Value::MakeString(estimatedCost));
    if (!keyConfigured) reply.Set("key_missing", Value::MakeString(keyMessage));
    if (generation::CurrentGenerationTransport() == nullptr)
        reply.Set("transport_missing",
                  Value::MakeString(generation::kNoTransportMessage));
    resultJson = reply.Dump();
    return true;
}

// What one upload sends: the still as it is, one frame, or an excerpt cut
// by FFmpeg into the package's generated folder and removed once read.
bool ReadUploadBytes(const Document& document, const fs::path& workDirectory,
                     const generation::GenerationProposal& proposal,
                     size_t index, std::string& bytes, std::string& type,
                     std::string& name, std::string& message) {
    const generation::GenerationUpload& upload = proposal.uploads[index];
    const std::string stem = proposal.id + "-" + std::to_string(index);
    if (upload.what == "still") {
        type = StillContentType(upload.path);
        name = stem + fs::path(upload.path).extension().string();
        return ReadFileBytes(upload.path, bytes, message);
    }
    if (upload.what == "frame") {
        const LibraryMedia* media = document.FindLibraryMedia(upload.media_id);
        FrameCaptureSettings settings;
        settings.max_dimension =
            media != nullptr ? std::max(media->width, media->height) : 1920;
        settings.jpeg_quality = 2;
        settings.color_management =
            document.ColorManagementFor(upload.media_id);
        type = "image/jpeg";
        name = stem + ".jpg";
        return ::CaptureSourceFrame(upload.path, upload.time, settings, bytes,
                                    message);
    }
    const bool soundOnly = upload.what == "sound";
    type = soundOnly ? "audio/wav" : "video/mp4";
    name = stem + (soundOnly ? ".wav" : ".mp4");
    std::error_code error;
    fs::create_directories(workDirectory, error);
    const fs::path excerpt = workDirectory / ("." + name);
    fs::remove(excerpt, error);
    // PA14 -- Kling 4.0 edit answers "Failed to load video" to a shot
    // tagged HLG (measured on 13 November 2026), and paints in Rec.709
    // whatever it is given: a shot whose media declares another input goes
    // up as its Rec.709 picture, by the transform read_frame shows under
    // colour management, whether or not the project manages colour.
    std::string colorFilter;
    const fs::path lut = workDirectory / ("." + stem + ".cube");
    const LibraryMedia* media = document.FindLibraryMedia(upload.media_id);
    if (!soundOnly && proposal.kind == "edit_video" && media != nullptr &&
        DeclaresColorInput(media->color_input) &&
        !media->color_input.graphics &&
        (media->color_input.transfer != "rec709" ||
         media->color_input.gamut != "rec709")) {
        ColorManagementSettings settings =
            document.ColorManagementFor(upload.media_id);
        settings.enabled = true;
        std::string cube;
        if (!BuildOpenColorIoCube(ColorManagementForSdrPreview(settings), cube,
                                  message)) {
            message =
                "the shot's colour transform to Rec.709 could not be "
                "built: " +
                message;
            return false;
        }
        std::ofstream out(lut, std::ios::binary | std::ios::trunc);
        out << cube;
        out.close();
        if (!out) {
            message = "unable to write '" + lut.string() + "'";
            return false;
        }
        colorFilter = ColorLutFilter(settings, lut.string(), "bt709");
    }
    const bool cut = generation::EncodeExcerpt(
        upload.path, upload.time, upload.duration, soundOnly, excerpt.string(),
        message, colorFilter);
    fs::remove(lut, error);
    if (!cut) return false;
    const bool read = ReadFileBytes(excerpt.string(), bytes, message);
    fs::remove(excerpt, error);
    return read;
}

bool GenerateCreative(McpBackend& backend,
                      generation::GenerationProposalStore& proposals,
                      const std::string& proposalId,
                      const generation::HttpTransport& transport,
                      const std::string& key, const std::string& origin,
                      const std::string& packageDirectory,
                      std::string& resultJson, std::string& errorName,
                      std::string& message) {
    generation::GenerationProposal proposal;
    if (!proposals.Peek(proposalId, std::chrono::steady_clock::now(), proposal,
                        message))
        return Refuse(errorName, message, "ProposalUnavailable",
                      "nothing was spent: " + message);
    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Refuse(errorName, message, "IoError",
                      "nothing was spent: " + message);
    const fs::path generated = fs::path(packageDirectory) / "generated";
    // Every file is read, and cut, before the proposal is taken: an excerpt
    // FFmpeg cannot make leaves the proposal for another try.
    struct Ready {
        std::string bytes;
        std::string type;
        std::string name;
    };
    std::vector<Ready> ready(proposal.uploads.size());
    for (size_t index = 0; index < proposal.uploads.size(); ++index) {
        if (!ReadUploadBytes(document, generated, proposal, index,
                             ready[index].bytes, ready[index].type,
                             ready[index].name, message))
            return Refuse(errorName, message, "IoError",
                          "nothing was spent: '" +
                              proposal.uploads[index].field +
                              "' could not be read: " + message);
        if (ready[index].bytes.size() > generation::kMaxUploadBytes)
            return Refuse(
                errorName, message, "ValidationFailed",
                "nothing was spent: '" + proposal.uploads[index].field +
                    "' weighs " +
                    std::to_string(ready[index].bytes.size() / (1024 * 1024)) +
                    " MB, over the " +
                    std::to_string(generation::kMaxUploadBytes /
                                   (1024 * 1024)) +
                    " MB one upload takes: propose a shorter stretch");
    }
    if (!proposals.Take(proposalId, std::chrono::steady_clock::now(), proposal,
                        message))
        return Refuse(errorName, message, "ProposalUnavailable",
                      "nothing was spent: " + message);

    const generation::FalClient client(transport, key);
    Value input = proposal.input;
    // PA14 -- the uploads of an array field ("image_urls"), in order.
    Value references = Value::MakeArray();
    for (size_t index = 0; index < proposal.uploads.size(); ++index) {
        std::string url;
        if (!client.Upload(ready[index].bytes, ready[index].type,
                           ready[index].name, url, message))
            return Refuse(errorName, message, "GenerationFailed",
                          "nothing was generated ('" +
                              proposal.uploads[index].field +
                              "' did not reach fal.ai's storage): " + message);
        if (proposal.uploads[index].field == "image_urls") {
            references.Push(Value::MakeString(url));
        } else {
            input.Set(proposal.uploads[index].field, Value::MakeString(url));
        }
    }
    if (!references.AsArray().empty())
        input.Set("image_urls", std::move(references));
    std::string resultBody;
    std::string requestId;
    if (!client.Run(proposal.model, input, resultBody, requestId, message))
        return Refuse(errorName, message, "GenerationFailed",
                      "the generation did not complete" +
                          (requestId.empty() ? std::string()
                                             : " (fal.ai request " + requestId +
                                                   ", which may be billed)") +
                          ": " + message);
    std::string outputUrl;
    std::string seed;
    if (!generation::ParseCreativeOutput(proposal.kind, resultBody, outputUrl,
                                         seed, message))
        return Refuse(
            errorName, message, "GenerationFailed",
            "fal.ai request " + requestId +
                " ran (and is billed) but its answer is unusable: " + message);
    std::string outputBytes;
    std::string outputType;
    if (!client.Download(outputUrl, outputBytes, outputType, message))
        return Refuse(errorName, message, "GenerationFailed",
                      "fal.ai request " + requestId +
                          " ran (and is billed) but what it made could not "
                          "be fetched from " +
                          outputUrl + ": " + message);
    const bool image = proposal.kind == "stock_image";
    const fs::path target =
        generated /
        (proposal.id + (image ? ExtensionFor(outputType, outputUrl)
                              : VideoExtensionFor(outputType, outputUrl)));
    std::error_code existsError;
    if (fs::exists(target, existsError))
        return Refuse(errorName, message, "IoError",
                      "'" + generated.string() +
                          "' already holds a generation " + proposal.id);
    if (!WriteFileBytes(target, outputBytes, message))
        return Refuse(errorName, message, "IoError",
                      "fal.ai request " + requestId +
                          " ran (and is billed) but what it made could not "
                          "be written: " +
                          message);
    std::string ingestJson;
    if (!backend.IngestMedia(target.string(), false, std::nullopt, ingestJson,
                             errorName, message)) {
        message = "the file is at '" + target.string() + "' (fal.ai request " +
                  requestId + ", billed) but could not be ingested: " + message;
        return false;
    }
    Document ingested;
    if (!backend.SnapshotDocument(ingested, message))
        return Refuse(errorName, message, "IoError", message);
    const std::string fileName = target.filename().string();
    const LibraryMedia* added = nullptr;
    for (const LibraryMedia& item : ingested.library) {
        if (item.filename == fileName) added = &item;
    }
    if (added == nullptr)
        return Refuse(
            errorName, message, "IoError",
            "'" + target.string() + "' was ingested but is not in the library");
    const Ulid addedId = added->id;
    const MediaGeneration provenance = generation::ProvenanceOf(
        proposal, seed, requestId, generation::UtcNow());
    std::string applied;
    if (!backend.ApplyOperation(
            SetMediaGenerationOperation{addedId, provenance}, applied,
            errorName, message)) {
        message =
            "media '" + addedId +
            "' is in the library but its provenance was refused: " + message;
        return false;
    }

    Value reply = Value::MakeObject();
    reply.Set("spent", Value::MakeBool(true));
    reply.Set("media_id", Value::MakeString(addedId));
    reply.Set("path", Value::MakeString(target.string()));
    Value made = Value::MakeObject();
    made.Set("service", Value::MakeString(provenance.service));
    made.Set("model", Value::MakeString(provenance.model));
    made.Set("kind", Value::MakeString(provenance.kind));
    made.Set("prompt", Value::MakeString(provenance.prompt));
    made.Set("seed", Value::MakeString(provenance.seed));
    made.Set("created_at", Value::MakeString(provenance.created_at));
    made.Set("announced_cost",
             provenance.announced_cost.empty()
                 ? Value::MakeNull()
                 : Value::MakeString(provenance.announced_cost));
    made.Set("request_id", Value::MakeString(provenance.request_id));
    made.Set("source_media_id", Value::MakeString(provenance.source_media_id));
    reply.Set("generation", std::move(made));
    if (proposal.needs_consent)
        reply.Set("consent_of_filmed_person", Value::MakeBool(true));
    reply.Set("key_from", Value::MakeString(origin));
    Value parsed;
    std::string parseError;
    if (Value::Parse(applied, parsed, parseError))
        reply.Set("result", std::move(parsed));
    resultJson = reply.Dump();
    return true;
}

// ---------------------------------------------------------------------
// propose_generation
// ---------------------------------------------------------------------

bool DispatchProposeGeneration(McpBackend& backend, const IdResolver& resolver,
                               const Value& args,
                               generation::GenerationProposalStore& proposals,
                               std::string& resultJson, std::string& errorName,
                               std::string& message) {
    const std::string tool = "propose_generation";
    if (!Known(args,
               {"kind", "media_id", "source_time", "aspect_ratio", "prompt",
                "negative_prompt", "seed", "frames", "duration",
                "duration_seconds", "audio_media_id", "audio_source_time",
                "audio_duration", "target_language", "reference_media_ids",
                "resolution"},
               tool, message))
        return Invalid(errorName, message, message);
    // PA12 -- the creative kinds read their own arguments.
    {
        std::string creative;
        if (!ReadText(args, "kind", tool, true, creative, message))
            return Invalid(errorName, message, message);
        if (generation::IsCreativeKind(creative))
            return ProposeCreative(backend, resolver, args, creative, proposals,
                                   resultJson, errorName, message);
    }
    std::string kindName;
    std::string mediaText;
    std::string aspect;
    std::string prompt;
    std::string negativePrompt;
    std::string seed;
    RationalTime sourceTime;
    bool hasTime = false;
    if (!ReadText(args, "kind", tool, true, kindName, message) ||
        !ReadText(args, "media_id", tool, false, mediaText, message) ||
        !ReadText(args, "aspect_ratio", tool, false, aspect, message) ||
        !ReadText(args, "prompt", tool, false, prompt, message) ||
        !ReadText(args, "negative_prompt", tool, false, negativePrompt,
                  message) ||
        !ReadSeed(args, tool, seed, message) ||
        !ReadTime(args, "source_time", tool, sourceTime, hasTime, message))
        return Invalid(errorName, message, message);
    // GN3 -- the clip the picture becomes: 125 frames unless asked.
    int64_t frames = 125;
    if (const Value* field = args.Find("frames")) {
        if (!field->IsNumber() || !field->AsInt64(frames) || frames < 1 ||
            frames > 100000)
            return Invalid(
                errorName, message,
                "'" + tool + ".frames' must be an integer from 1 to 100000");
    }
    const generation::GenerationKind* kind =
        generation::FindGenerationKind(kindName);
    if (kind == nullptr) {
        std::string names;
        for (const auto& entry : generation::GenerationKinds())
            names += (names.empty() ? "" : ", ") + std::string(entry.kind);
        return Invalid(errorName, message,
                       "'" + tool + ".kind' must be one of: " + names +
                           " (the other uses are not built yet)");
    }
    // PA12 -- what a kind needs is said with the kind: the schema requires
    // `kind` alone, each kind reading its own arguments.
    for (const auto& [key, value] :
         {std::pair<const char*, const std::string*>{"media_id", &mediaText},
          {"aspect_ratio", &aspect}}) {
        if (value->empty())
            return Invalid(errorName, message,
                           "kind '" + kindName + "' needs '" + key + "'");
    }
    Ulid mediaId;
    if (!resolver.Resolve(tool + ".media_id", mediaText, mediaId, message))
        return Invalid(errorName, message, message);

    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Refuse(errorName, message, "IoError", message);
    const LibraryMedia* media = document.FindLibraryMedia(mediaId);
    if (media == nullptr)
        return Refuse(errorName, message, "UnknownMedia",
                      "unknown media_id '" + mediaId + "'");
    if (media->metadata_complete && !media->has_video)
        return Invalid(errorName, message,
                       "media '" + mediaId +
                           "' is audio-only: there is no picture to extend");
    const std::string packageDirectory = backend.PackageDirectory();
    if (packageDirectory.empty())
        return Refuse(errorName, message, "UnsupportedOperation",
                      "this backend has no project package to write a "
                      "generated file into: open a project first");
    const std::string sourcePath = AbsoluteMediaPath(packageDirectory, *media);
    std::error_code existsError;
    if (!fs::exists(sourcePath, existsError))
        return Refuse(errorName, message, "IoError",
                      "the file of media '" + mediaId + "' is missing: '" +
                          sourcePath + "'");
    const bool still = !StillContentType(sourcePath).empty();
    if (still && hasTime)
        return Invalid(
            errorName, message,
            "media '" + mediaId + "' is a still: leave 'source_time' out");
    if (!still && !hasTime)
        return Invalid(errorName, message,
                       "media '" + mediaId +
                           "' is a video: 'source_time' names the frame to "
                           "extend, in the media's own time");
    // Checked now, while nothing is spent: a frame past the end would only
    // fail once the proposal is taken.
    if (hasTime && media->duration.rate > 0 && sourceTime >= media->duration)
        return Invalid(errorName, message,
                       "'" + tool + ".source_time' is past the end of media '" +
                           mediaId + "'");

    int64_t width = media->width;
    int64_t height = media->height;
    // A frame comes back as the rush is displayed (FrameCapture.h).
    if (!still &&
        (media->rotation_degrees == 90 || media->rotation_degrees == -90 ||
         media->rotation_degrees == 270 || media->rotation_degrees == -270))
        std::swap(width, height);
    int64_t aspectWidth = 0;
    int64_t aspectHeight = 0;
    generation::ExpandCanvas canvas;
    if (!generation::ParseAspectRatio(aspect, aspectWidth, aspectHeight,
                                      message) ||
        !generation::ComputeExpandCanvas(width, height, aspectWidth,
                                         aspectHeight, canvas, message))
        return Invalid(errorName, message, message);

    generation::GenerationProposal proposal;
    proposal.id = GenerateUlid();
    proposal.kind = kind->kind;
    proposal.service = kind->service;
    proposal.model = kind->model;
    proposal.announced_cost = kind->announced_cost;
    proposal.source_media_id = mediaId;
    proposal.source_path = sourcePath;
    proposal.source_is_frame = !still;
    proposal.source_time = sourceTime;
    proposal.source_width = width;
    proposal.source_height = height;
    proposal.aspect_ratio = aspect;
    proposal.canvas = canvas;
    proposal.prompt = prompt;
    proposal.negative_prompt = negativePrompt;
    proposal.seed = seed;
    proposal.clip_rate = still ? MediaRate{25, 1} : media->rate;
    proposal.clip_frames = frames;
    proposal.proposed_at = generation::UtcNow();
    proposal.created = std::chrono::steady_clock::now();
    proposals.Add(proposal);

    std::string key;
    std::string origin;
    std::string keyMessage;
    const bool keyConfigured =
        generation::ResolveFalKey(key, origin, keyMessage);
    Value reply = generation::ProposalValue(
        proposal, keyConfigured,
        generation::CurrentGenerationTransport() != nullptr);
    if (!keyConfigured) reply.Set("key_missing", Value::MakeString(keyMessage));
    if (generation::CurrentGenerationTransport() == nullptr)
        reply.Set("transport_missing",
                  Value::MakeString(generation::kNoTransportMessage));
    resultJson = reply.Dump();
    return true;
}

// ---------------------------------------------------------------------
// generate_media
// ---------------------------------------------------------------------

bool DispatchGenerateMedia(McpBackend& backend, const Value& args,
                           generation::GenerationProposalStore& proposals,
                           std::string& resultJson, std::string& errorName,
                           std::string& message) {
    const std::string tool = "generate_media";
    // `timeline_id` is read by the catalog: the provenance joins that
    // timeline's history, as a white balance does.
    if (!Known(args,
               {"proposal_id", "confirmed_by_person", "timeline_id",
                "consent_of_filmed_person"},
               tool, message))
        return Invalid(errorName, message, message);
    std::string proposalId;
    if (!ReadText(args, "proposal_id", tool, true, proposalId, message))
        return Invalid(errorName, message, message);
    const Value* confirmed = args.Find("confirmed_by_person");
    if (confirmed == nullptr || !confirmed->IsBool() || !confirmed->AsBool())
        return Refuse(errorName, message, "ConfirmationRequired",
                      "nothing was spent: generate_media runs only on a "
                      "proposal the person accepted, with "
                      "'confirmed_by_person': true. Show them the proposal "
                      "and its cost first; never confirm in their place");

    // Everything that can be checked without spending is checked before the
    // proposal is taken: a missing key must not use up a proposal.
    const generation::HttpTransport transport = InstalledTransport();
    if (!transport)
        return Refuse(errorName, message, "UnsupportedOperation",
                      std::string("nothing was spent: ") +
                          generation::kNoTransportMessage);
    std::string key;
    std::string origin;
    if (!generation::ResolveFalKey(key, origin, message))
        return Refuse(errorName, message, "GenerationKeyMissing",
                      "nothing was spent: " + message);
    const std::string packageDirectory = backend.PackageDirectory();
    if (packageDirectory.empty())
        return Refuse(errorName, message, "UnsupportedOperation",
                      "nothing was spent: no project package is open");

    // The proposal is read first and taken only once its source is in hand:
    // a frame that cannot be read leaves it for another try.
    generation::GenerationProposal proposal;
    if (!proposals.Peek(proposalId, std::chrono::steady_clock::now(), proposal,
                        message))
        return Refuse(errorName, message, "ProposalUnavailable",
                      "nothing was spent: " + message);

    // DEC51 -- a kind that acts on a filmed person runs only with that
    // person's consent, said in the call; refused before the proposal is
    // taken, so the same proposal serves once the consent is there.
    if (proposal.needs_consent) {
        const Value* consent = args.Find("consent_of_filmed_person");
        if (consent == nullptr || !consent->IsBool() || !consent->AsBool())
            return Refuse(errorName, message, "ConsentRequired",
                          "nothing was spent: '" + proposal.kind +
                              "' changes what a filmed person says or "
                              "shows, and runs only with their consent: "
                              "'consent_of_filmed_person': true, which only "
                              "the person who signs the film can state. Ask "
                              "them; never state it in their place");
    }
    if (proposal.input.IsObject())
        return GenerateCreative(backend, proposals, proposalId, transport, key,
                                origin, packageDirectory, resultJson, errorName,
                                message);

    Document document;
    if (!backend.SnapshotDocument(document, message))
        return Refuse(errorName, message, "IoError",
                      "nothing was spent: " + message);
    std::string sourceBytes;
    std::string sourceType = StillContentType(proposal.source_path);
    if (proposal.source_is_frame) {
        FrameCaptureSettings settings;
        settings.max_dimension = static_cast<int32_t>(
            std::max(proposal.source_width, proposal.source_height));
        settings.jpeg_quality = 2;
        settings.color_management =
            document.ColorManagementFor(proposal.source_media_id);
        if (!::CaptureSourceFrame(proposal.source_path, proposal.source_time,
                                  settings, sourceBytes, message))
            return Refuse(
                errorName, message, "IoError",
                "nothing was spent: the frame could not be read: " + message);
        sourceType = "image/jpeg";
    } else if (!ReadFileBytes(proposal.source_path, sourceBytes, message)) {
        return Refuse(errorName, message, "IoError",
                      "nothing was spent: " + message);
    }
    if (!proposals.Take(proposalId, std::chrono::steady_clock::now(), proposal,
                        message))
        return Refuse(errorName, message, "ProposalUnavailable",
                      "nothing was spent: " + message);

    const generation::FalClient client(transport, key);
    std::string imageUrl;
    const std::string uploadName =
        proposal.id + (sourceType == "image/jpeg"   ? ".jpg"
                       : sourceType == "image/webp" ? ".webp"
                                                    : ".png");
    if (!client.Upload(sourceBytes, sourceType, uploadName, imageUrl, message))
        return Refuse(errorName, message, "GenerationFailed",
                      "nothing was generated (the source did not reach "
                      "fal.ai's storage): " +
                          message);
    Value input =
        generation::ExpandInput(proposal.canvas, proposal.prompt,
                                proposal.negative_prompt, proposal.seed);
    input.Set("image_url", Value::MakeString(imageUrl));
    std::string resultBody;
    std::string requestId;
    if (!client.Run(proposal.model, input, resultBody, requestId, message))
        return Refuse(errorName, message, "GenerationFailed",
                      "the generation did not complete" +
                          (requestId.empty() ? std::string()
                                             : " (fal.ai request " + requestId +
                                                   ", which may be billed)") +
                          ": " + message);
    std::string outputUrl;
    std::string seed;
    int64_t outputWidth = 0;
    int64_t outputHeight = 0;
    if (!generation::ParseExpandOutput(resultBody, outputUrl, seed, outputWidth,
                                       outputHeight, message))
        return Refuse(
            errorName, message, "GenerationFailed",
            "fal.ai request " + requestId +
                " ran (and is billed) but its answer is unusable: " + message);
    std::string outputBytes;
    std::string outputType;
    if (!client.Download(outputUrl, outputBytes, outputType, message))
        return Refuse(errorName, message, "GenerationFailed",
                      "fal.ai request " + requestId +
                          " ran (and is billed) but its picture could not be "
                          "fetched from " +
                          outputUrl + ": " + message);

    // The picture as the service returned it, and the clip made from it,
    // side by side in the package (EncodeStillClip says why a clip).
    const fs::path picture =
        fs::path(packageDirectory) / "generated" /
        (proposal.id + ExtensionFor(outputType, outputUrl));
    const fs::path target =
        fs::path(packageDirectory) / "generated" / (proposal.id + ".mp4");
    std::error_code existsError;
    if (fs::exists(picture, existsError) || fs::exists(target, existsError))
        return Refuse(errorName, message, "IoError",
                      "'" + target.parent_path().string() +
                          "' already holds a generation " + proposal.id);
    if (!WriteFileBytes(picture, outputBytes, message))
        return Refuse(errorName, message, "IoError",
                      "fal.ai request " + requestId +
                          " ran (and is billed) but its picture could not be "
                          "written: " +
                          message);
    if (!generation::EncodeStillClip(picture.string(), target.string(),
                                     proposal.clip_rate, proposal.clip_frames,
                                     message))
        return Refuse(errorName, message, "IoError",
                      "the picture is at '" + picture.string() +
                          "' (fal.ai request " + requestId +
                          ", billed) but could not become a clip: " + message);
    std::string ingestJson;
    if (!backend.IngestMedia(target.string(), false, std::nullopt, ingestJson,
                             errorName, message)) {
        message = "the picture is at '" + target.string() +
                  "' (fal.ai request " + requestId +
                  ", billed) but could not be ingested: " + message;
        return false;
    }
    Document ingested;
    if (!backend.SnapshotDocument(ingested, message))
        return Refuse(errorName, message, "IoError", message);
    const std::string fileName = target.filename().string();
    const LibraryMedia* added = nullptr;
    for (const LibraryMedia& item : ingested.library) {
        if (item.filename == fileName) added = &item;
    }
    if (added == nullptr)
        return Refuse(
            errorName, message, "IoError",
            "'" + target.string() + "' was ingested but is not in the library");
    const Ulid addedId = added->id;
    const MediaGeneration provenance = generation::ProvenanceOf(
        proposal, seed, requestId, generation::UtcNow());
    std::string applied;
    if (!backend.ApplyOperation(
            SetMediaGenerationOperation{addedId, provenance}, applied,
            errorName, message)) {
        message =
            "media '" + addedId +
            "' is in the library but its provenance was refused: " + message;
        return false;
    }

    Value reply = Value::MakeObject();
    reply.Set("spent", Value::MakeBool(true));
    reply.Set("media_id", Value::MakeString(addedId));
    reply.Set("path", Value::MakeString(target.string()));
    reply.Set("picture", Value::MakeString(picture.string()));
    if (outputWidth > 0) reply.Set("width", Value::MakeInt(outputWidth));
    if (outputHeight > 0) reply.Set("height", Value::MakeInt(outputHeight));
    Value generated = Value::MakeObject();
    generated.Set("service", Value::MakeString(provenance.service));
    generated.Set("model", Value::MakeString(provenance.model));
    generated.Set("kind", Value::MakeString(provenance.kind));
    generated.Set("prompt", Value::MakeString(provenance.prompt));
    generated.Set("seed", Value::MakeString(provenance.seed));
    generated.Set("created_at", Value::MakeString(provenance.created_at));
    generated.Set("announced_cost",
                  provenance.announced_cost.empty()
                      ? Value::MakeNull()
                      : Value::MakeString(provenance.announced_cost));
    generated.Set("request_id", Value::MakeString(provenance.request_id));
    generated.Set("source_media_id",
                  Value::MakeString(provenance.source_media_id));
    reply.Set("generation", std::move(generated));
    reply.Set("key_from", Value::MakeString(origin));
    Value parsed;
    std::string parseError;
    if (Value::Parse(applied, parsed, parseError))
        reply.Set("result", std::move(parsed));
    resultJson = reply.Dump();
    return true;
}

}  // namespace

void RegisterGenerationTools(const McpToolAdder& add,
                             generation::GenerationProposalStore& proposals) {
    generation::GenerationProposalStore* store = &proposals;
    add("propose_generation",
        "Propose a paid generation WITHOUT spending anything: what would be "
        "sent, to which fal.ai model, at what announced cost, and a "
        "proposal_id. It is billed on the person's own fal.ai account. "
        "Call it when the person asked for one, or to suggest one; "
        "then show them the proposal, cost included, and wait for their "
        "answer. Kinds: 'expand_image' (media_id, aspect_ratio; source_time "
        "for a video): a still or a frame extended to aspect_ratio around the "
        "picture kept at its own size, the canvas computed by the engine -- "
        "no expansion of a shot with a person in it; a clip of `frames` frames "
        "enters the library. 'stock_video' (prompt; duration_seconds, "
        "aspect_ratio): a silent shot made from words. 'stock_image' "
        "(prompt): a picture at the timeline's size. 'avatar' (media_id: a "
        "still, or a frame at source_time; audio_media_id): a synthetic "
        "presenter speaking that sound. 'lipsync' (media_id and its stretch; "
        "audio_media_id): a filmed person's lips moved to another sound. "
        "'dubbing' (media_id and its stretch; target_language): the voices "
        "put in another language, the lips untouched. 'edit_video' (media_id "
        "and a stretch of 3 to 30 s; prompt; reference_media_ids; "
        "resolution): a shot edited by words, its motion kept -- a person or "
        "a thing replaced, a place or a look changed. A stretch is "
        "source_time and duration (audio_source_time and audio_duration for "
        "the sound), the whole media when left out, 300 s at most; the files "
        "go to fal.ai's storage. lipsync, dubbing, an avatar from a "
        "picture the engine did not generate, and edit_video on a shot or a "
        "picture the engine did not generate change what a FILMED person says "
        "or shows: the proposal says `needs_consent`, and generate_media then "
        "asks for that person's consent; without it, propose nothing on a "
        "filmed person. No kind corrects a gaze. "
        "The proposal lives for this session, 30 minutes, and is spent once.",
        Schema(
            {{"kind",
              "{\"type\":\"string\",\"enum\":[\"expand_image\","
              "\"stock_video\",\"stock_image\",\"avatar\",\"lipsync\","
              "\"dubbing\",\"edit_video\"],"
              "\"description\":\"What to generate.\"}"},
             {"media_id",
              Described("string",
                        "The picture to start from (expand_image, avatar), "
                        "or the film to act on (lipsync, dubbing, "
                        "edit_video).")},
             {"reference_media_ids",
              "{\"type\":\"array\",\"items\":{\"type\":\"string\"},"
              "\"maxItems\":4,\"description\":"
              "\"edit_video: optional stills (PNG, JPEG, WebP) of what the "
              "shot becomes; the prompt names them @Image1, @Image2... in "
              "order.\"}"},
             {"resolution",
              "{\"type\":\"string\",\"enum\":[\"720p\",\"1080p\"],"
              "\"description\":\"edit_video: the definition of what comes "
              "back; default 1080p. The price rises with it.\"}"},
             {"source_time", kSourceTime},
             {"duration", kStretchLength},
             {"audio_media_id",
              Described("string",
                        "avatar, lipsync: the media whose sound is spoken.")},
             {"audio_source_time", kStretchStart},
             {"audio_duration", kStretchLength},
             {"target_language",
              Described("string",
                        "dubbing: the language to dub into, two letters, as "
                        "\"en\" or \"es\".")},
             {"duration_seconds",
              Described("integer",
                        "stock_video: length of the shot, 3 to 15 seconds; "
                        "default 5.")},
             {"aspect_ratio",
              Described("string",
                        "expand_image: the aspect to reach, width:height in "
                        "integers, such as \"9:16\". stock_video: "
                        "\"16:9\", \"9:16\" or \"1:1\"; default: the "
                        "timeline's when it is one of them.")},
             {"prompt",
              Described("string",
                        "In English. stock_video, stock_image: what to "
                        "show (required). expand_image: optional, what the "
                        "margins show. avatar: optional direction. "
                        "edit_video (required): what changes and what "
                        "stays; the shot is @Video1.")},
             {"negative_prompt",
              Described("string",
                        "expand_image, stock_video: optional, what not to "
                        "show.")},
             {"seed", Described("integer",
                                "expand_image, stock_image: optional seed, "
                                "to reproduce a result.")},
             {"frames",
              Described("integer",
                        "expand_image: length of the clip the picture "
                        "becomes, in frames of the source's rate (25/1 for "
                        "a still); default 125.")}},
            {"kind"}, "propose_generation"),
        [store](McpBackend& backend, const IdResolver& resolver,
                const Value& args, std::string& resultJson,
                std::string& errorName, std::string& message) {
            return DispatchProposeGeneration(backend, resolver, args, *store,
                                             resultJson, errorName, message);
        });

    // DEC2 -- docs/roadmap/GENERATIF-2026-10.md. What generation never
    // does, decided on 23 October 2026, is said where an agent reads it
    // before spending: in this description, and in the skills that call
    // it. No check enforces it -- whether a shot shows a person is not
    // something the engine reads reliably yet -- so it is a rule the agent
    // is told, and the permission prompt is where the person sees it.
    add("generate_media",
        "SPENDS MONEY on the person's fal.ai account. Runs a proposal of this "
        "session (propose_generation) that the PERSON explicitly accepted, and "
        "adds what it made to the library as an ordinary media, marked with "
        "its provenance (service, model, prompt, seed, date, announced cost; "
        "describe shows it as `generation`). Never call it because a "
        "generation seems useful: propose, and wait for the person's yes. A "
        "failure after fal.ai ran the request says so: it may be billed. WHAT "
        "GENERATION NEVER DOES, even when asked (decided by the person who "
        "signs the films): anything that makes a filmed person say or do what "
        "they did not say or do, without their consent -- no voice, face or "
        "body made, completed or prolonged. lipsync, dubbing, an avatar "
        "from a picture the engine did not generate, and edit_video on a "
        "shot or a picture the engine did not generate act on a filmed "
        "person: "
        "they run only with 'consent_of_filmed_person': true, which states "
        "that this person agreed to this use; only the person who signs the "
        "film can state it, never you in their place. expand_image only on a "
        "shot with no person in it: a 9:16 interview goes through framing "
        "(set_clip_framing, track_subject_framing), never an expansion. A "
        "generated music only from a model whose licence covers paid "
        "advertising, which is verified neither for Stable Audio nor for "
        "Lyria. When a request crosses one of these, say so and do not "
        "propose it. The media made keeps its provenance: no edit clears or "
        "replaces it, only undo does.",
        Schema({{"proposal_id",
                 Described("string", "The id propose_generation returned.")},
                {"confirmed_by_person",
                 Described("boolean",
                           "true only when the person accepted this "
                           "proposal and its cost.")},
                {"consent_of_filmed_person",
                 Described("boolean",
                           "For a proposal that says needs_consent: true "
                           "only when the person who signs the film states "
                           "that the filmed person agreed to this use.")}},
               {"proposal_id", "confirmed_by_person"}, "generate_media"),
        [store](McpBackend& backend, const IdResolver&, const Value& args,
                std::string& resultJson, std::string& errorName,
                std::string& message) {
            return DispatchGenerateMedia(backend, args, *store, resultJson,
                                         errorName, message);
        });
}
