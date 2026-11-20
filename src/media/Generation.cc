// GENERATIF-2026-10 -- docs/roadmap/GENERATIF-2026-10.md. See Generation.h.

#include "Generation.h"

#include "FfmpegBinary.h"
#include "ProcessLauncher.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <limits>
#include <numeric>
#include <thread>

namespace generation {

namespace {

using mcp_json::Value;

std::atomic<NativeHttp>& TransportSlot() {
    static std::atomic<NativeHttp> slot{nullptr};
    return slot;
}

std::atomic<NativeKeyReader>& KeyReaderSlot() {
    static std::atomic<NativeKeyReader> slot{nullptr};
    return slot;
}

std::string Trimmed(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end &&
           std::isspace(static_cast<unsigned char>(text[begin])) != 0)
        ++begin;
    while (end > begin &&
           std::isspace(static_cast<unsigned char>(text[end - 1])) != 0)
        --end;
    return text.substr(begin, end - begin);
}

bool StartsWith(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::string StringAt(const Value& object, const char* key) {
    const Value* field = object.Find(key);
    return field != nullptr && field->IsString() ? field->AsString()
                                                 : std::string();
}

// What fal.ai said was wrong, in a few hundred characters at most: its
// `detail` (a sentence, or FastAPI's list of {loc, msg}), else the body.
std::string ErrorDetail(const std::string& body) {
    constexpr size_t kLimit = 300;
    Value parsed;
    std::string parseError;
    std::string detail;
    if (Value::Parse(body, parsed, parseError) && parsed.IsObject()) {
        if (const Value* field = parsed.Find("detail")) {
            if (field->IsString()) {
                detail = field->AsString();
            } else if (field->IsArray()) {
                for (const Value& item : field->AsArray()) {
                    const std::string message = StringAt(item, "msg");
                    if (message.empty()) continue;
                    if (!detail.empty()) detail += " ; ";
                    detail += message;
                    // PA14 -- which field, and fal.ai's own name for the
                    // error: "Failed to load video." alone says neither.
                    std::string where;
                    if (const Value* loc = item.Find("loc")) {
                        if (loc->IsArray()) {
                            for (const Value& part : loc->AsArray()) {
                                if (!part.IsString()) continue;
                                if (!where.empty()) where += ".";
                                where += part.AsString();
                            }
                        }
                    }
                    const std::string type = StringAt(item, "type");
                    if (!where.empty() || !type.empty())
                        detail += " [" + where +
                                  (where.empty() || type.empty() ? "" : ", ") +
                                  type + "]";
                }
            }
        }
        if (detail.empty()) detail = StringAt(parsed, "error");
    }
    if (detail.empty()) detail = body;
    if (detail.size() > kLimit) detail = detail.substr(0, kLimit) + "...";
    return detail;
}

bool IsSuccess(int status) { return status >= 200 && status < 300; }

}  // namespace

// ---------------------------------------------------------------------
// GN1 -- seam and key.
// ---------------------------------------------------------------------

void SetGenerationTransport(NativeHttp transport) {
    TransportSlot().store(transport);
}

NativeHttp CurrentGenerationTransport() { return TransportSlot().load(); }

void SetGenerationKeyReader(NativeKeyReader reader) {
    KeyReaderSlot().store(reader);
}

NativeKeyReader CurrentGenerationKeyReader() { return KeyReaderSlot().load(); }

bool ResolveFalKey(std::string& key, std::string& origin,
                   std::string& message) {
    key.clear();
    origin.clear();
    std::string storeMessage;
    if (const NativeKeyReader reader = CurrentGenerationKeyReader()) {
        std::string stored;
        if (reader(stored, storeMessage) && !Trimmed(stored).empty()) {
            key = Trimmed(stored);
            origin = "trousseau";
            return true;
        }
    }
    if (const char* value = std::getenv(kFalKeyVariable)) {
        const std::string fromEnvironment = Trimmed(value);
        if (!fromEnvironment.empty()) {
            key = fromEnvironment;
            origin = kFalKeyVariable;
            return true;
        }
    }
    message = std::string("aucune clé fal.ai sur cette machine : ") +
              "enregistre-la dans le trousseau (security "
              "add-generic-password -s " +
              kFalKeychainService + " -a " + kFalKeychainAccount +
              " -w) ou exporte " + kFalKeyVariable +
              " dans l'environnement du serveur";
    if (!storeMessage.empty()) message += " (" + storeMessage + ")";
    return false;
}

std::string RedactSecret(std::string text, const std::string& secret) {
    if (secret.size() < 8) return text;
    size_t position = 0;
    while ((position = text.find(secret, position)) != std::string::npos) {
        text.replace(position, secret.size(), "[clé masquée]");
        position += 1;
    }
    return text;
}

// ---------------------------------------------------------------------
// GN4 -- kinds and the extension canvas.
// ---------------------------------------------------------------------

const std::vector<GenerationKind>& GenerationKinds() {
    static const std::vector<GenerationKind> kinds = {
        {"expand_image", "fal.ai", "fal-ai/bria/expand", "image",
         "Requête et réponse relues dans les types du client officiel "
         "(@fal-ai/client 1.10.1, BriaExpandInput et BriaExpandOutput), "
         "le 21 octobre 2026. Jamais appelé pour de vrai depuis le moteur : "
         "le premier appel réel reste à faire, et le prix à lire sur "
         "fal.ai.",
         ""},
        // PA12 -- docs/roadmap/PARITE-2026-11.md. Read on 11 November 2026
        // in each model's public documentation; none was ever called.
        {"stock_video", "fal.ai", "fal-ai/kling-video/v3/pro/text-to-video",
         "video",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt et OpenAPI) le 11 novembre 2026. Jamais appelé.",
         "0,112 $ par seconde de vidéo sans son (lu le 11 novembre 2026)"},
        {"stock_image", "fal.ai", "fal-ai/flux-2-pro", "image",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt et OpenAPI) le 11 novembre 2026. Jamais appelé.",
         "0,03 $ le premier mégapixel, 0,015 $ chacun des suivants (lu le "
         "11 novembre 2026)"},
        {"avatar", "fal.ai", "fal-ai/kling-video/ai-avatar/v2/standard",
         "video",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt et OpenAPI) le 11 novembre 2026. Jamais appelé.",
         "0,0562 $ par seconde de vidéo (lu le 11 novembre 2026)"},
        {"lipsync", "fal.ai", "fal-ai/sync-lipsync/v2", "video",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt et OpenAPI) le 11 novembre 2026. Jamais appelé. Le "
         "modèle ne publie ni durée ni taille maximale.",
         "3 $ par minute de vidéo (lu le 11 novembre 2026)", true},
        {"dubbing", "fal.ai", "fal-ai/elevenlabs/dubbing", "video",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt et OpenAPI) le 11 novembre 2026. Jamais appelé. Le "
         "modèle traduit la voix et ne touche pas aux lèvres.",
         "0,6 $ par minute, arrondie à la minute supérieure (lu le 11 "
         "novembre 2026)",
         true},
        // PA14 -- read on 13 November 2026 in the model's llms.txt; never
        // called when this was written.
        {"edit_video", "fal.ai", "kling/v4/video-to-video/edit", "video",
         "Requête et réponse lues dans la documentation publique du modèle "
         "(llms.txt) le 13 novembre 2026. La documentation ne dit ni "
         "combien d'images de référence le modèle prend, ni si le son du "
         "plan est gardé, ni que ce qui n'est pas nommé reste identique.",
         "0,094 $ par seconde en 720p, 0,141 $ en 1080p, facturé sur la "
         "durée d'entrée plus celle de sortie (lu le 13 novembre 2026)",
         true},
    };
    return kinds;
}

bool IsCreativeKind(const std::string& kind) {
    return kind == "stock_video" || kind == "stock_image" || kind == "avatar" ||
           kind == "lipsync" || kind == "dubbing" || kind == "edit_video";
}

int64_t EditVideoMilliDollars(int64_t seconds, const std::string& resolution) {
    const int64_t rate = resolution == "720p"    ? 94
                         : resolution == "1080p" ? 141
                                                 : 0;
    return rate * 2 * std::max<int64_t>(seconds, 0);
}

const GenerationKind* FindGenerationKind(const std::string& kind) {
    for (const GenerationKind& entry : GenerationKinds()) {
        if (kind == entry.kind) return &entry;
    }
    return nullptr;
}

bool ParseAspectRatio(const std::string& text, int64_t& width, int64_t& height,
                      std::string& message) {
    const size_t colon = text.find(':');
    const auto digits = [](const std::string& part) {
        return !part.empty() && part.size() <= 4 &&
               std::all_of(part.begin(), part.end(), [](char c) {
                   return std::isdigit(static_cast<unsigned char>(c)) != 0;
               });
    };
    if (colon == std::string::npos || !digits(text.substr(0, colon)) ||
        !digits(text.substr(colon + 1))) {
        message =
            "un format s'écrit largeur:hauteur en entiers, comme "
            "\"9:16\"; reçu \"" +
            text + "\"";
        return false;
    }
    width = std::stoll(text.substr(0, colon));
    height = std::stoll(text.substr(colon + 1));
    if (width <= 0 || height <= 0) {
        message = "un format a deux côtés positifs; reçu \"" + text + "\"";
        return false;
    }
    return true;
}

bool ComputeExpandCanvas(int64_t imageWidth, int64_t imageHeight,
                         int64_t aspectWidth, int64_t aspectHeight,
                         ExpandCanvas& canvas, std::string& message) {
    // fal-ai/bria/expand: "should have an area of less than 5000x5000
    // pixels", and the subject "greater than 15%" of the canvas.
    constexpr int64_t kMaximumArea = int64_t{5000} * 5000;
    constexpr int64_t kMinimumPercent = 15;
    constexpr int64_t kMaximumSide = 100000;
    if (imageWidth <= 0 || imageHeight <= 0 || imageWidth > kMaximumSide ||
        imageHeight > kMaximumSide) {
        message = "la taille de l'image source est inconnue ou hors bornes";
        return false;
    }
    if (aspectWidth <= 0 || aspectHeight <= 0) {
        message = "un format a deux côtés positifs";
        return false;
    }
    const int64_t divisor = std::gcd(aspectWidth, aspectHeight);
    aspectWidth /= divisor;
    aspectHeight /= divisor;
    if (imageWidth * aspectHeight == imageHeight * aspectWidth) {
        message = "l'image a déjà ce format : il n'y a rien à étendre";
        return false;
    }
    // The smallest k with k*aspect holding the picture on both sides: the
    // canvas has the aspect exactly, not to a rounding.
    const auto ceilDiv = [](int64_t a, int64_t b) { return (a + b - 1) / b; };
    int64_t k = std::max(ceilDiv(imageWidth, aspectWidth),
                         ceilDiv(imageHeight, aspectHeight));
    // An even k gives two even sides, whatever the aspect.
    if (k % 2 != 0) ++k;
    ExpandCanvas result;
    result.canvas_width = k * aspectWidth;
    result.canvas_height = k * aspectHeight;
    result.image_width = imageWidth;
    result.image_height = imageHeight;
    result.x = (result.canvas_width - imageWidth) / 2;
    result.y = (result.canvas_height - imageHeight) / 2;
    const int64_t canvasArea = result.canvas_width * result.canvas_height;
    if (canvasArea > kMaximumArea) {
        message = "le canevas " + std::to_string(result.canvas_width) + "x" +
                  std::to_string(result.canvas_height) +
                  " dépasse les 5000 x 5000 pixels que le modèle accepte : "
                  "réduis l'image source d'abord";
        return false;
    }
    if (imageWidth * imageHeight * 100 < kMinimumPercent * canvasArea) {
        message = "l'image ne couvrirait que " +
                  std::to_string(imageWidth * imageHeight * 100 / canvasArea) +
                  " % du canevas : le modèle demande que le sujet en couvre "
                  "plus de 15 %";
        return false;
    }
    canvas = result;
    return true;
}

Value ExpandInput(const ExpandCanvas& canvas, const std::string& prompt,
                  const std::string& negativePrompt, const std::string& seed) {
    const auto pair = [](int64_t first, int64_t second) {
        Value list = Value::MakeArray();
        list.Push(Value::MakeInt(first));
        list.Push(Value::MakeInt(second));
        return list;
    };
    Value input = Value::MakeObject();
    input.Set("canvas_size", pair(canvas.canvas_width, canvas.canvas_height));
    input.Set("original_image_size",
              pair(canvas.image_width, canvas.image_height));
    input.Set("original_image_location", pair(canvas.x, canvas.y));
    if (!prompt.empty()) input.Set("prompt", Value::MakeString(prompt));
    if (!negativePrompt.empty())
        input.Set("negative_prompt", Value::MakeString(negativePrompt));
    if (!seed.empty()) input.Set("seed", Value::MakeRawNumber(seed));
    return input;
}

bool EncodeStillClip(const std::string& picturePath,
                     const std::string& clipPath, const MediaRate& rate,
                     int64_t frames, std::string& message) {
    if (rate.num <= 0 || rate.den <= 0 || frames <= 0) {
        message = "a still clip needs a positive rate and frame count";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(clipPath, error)) {
        message = "'" + clipPath + "' already exists";
        return false;
    }
    // Written under a name of its own, then renamed: an interrupted encode
    // never leaves a clip that looks whole.
    const std::string partial = clipPath + ".partial.mp4";
    process::Request request;
    request.arguments = {
        "",
        "-nostdin",
        "-v",
        "error",
        "-loop",
        "1",
        "-framerate",
        std::to_string(rate.num) + "/" + std::to_string(rate.den),
        "-i",
        picturePath,
        "-frames:v",
        std::to_string(frames),
        "-vf",
        "crop=trunc(iw/2)*2:trunc(ih/2)*2,"
        "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
        "-c:v",
        "libx264",
        "-preset",
        "medium",
        "-crf",
        "12",
        "-tune",
        "stillimage",
        "-colorspace",
        "bt709",
        "-color_primaries",
        "bt709",
        "-color_trc",
        "bt709",
        "-color_range",
        "tv",
        "-an",
        "-y",
        partial};
    request.standard_output = process::Stream::kDiscard;
    request.standard_error = process::Stream::kPipe;
    request.discard_input = true;
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        message = ffmpeg::StartError(result, "FFmpeg pipes");
        return false;
    }
    if (!result.Succeeded()) {
        std::filesystem::remove(partial, error);
        message = "FFmpeg could not make a clip of the generated picture: " +
                  Trimmed(result.error_tail);
        return false;
    }
    std::filesystem::rename(partial, clipPath, error);
    if (error) {
        message = "unable to name '" + clipPath + "': " + error.message();
        return false;
    }
    return true;
}

namespace {

// A time as FFmpeg's -ss and -t read one: seconds to the microsecond, by
// integers.
std::string FfmpegSeconds(const RationalTime& time) {
    const __int128 micros =
        static_cast<__int128>(time.value) * 1000000 / time.rate;
    char text[48];
    std::snprintf(text, sizeof(text), "%lld.%06lld",
                  static_cast<long long>(micros / 1000000),
                  static_cast<long long>(micros % 1000000));
    return text;
}

}  // namespace

bool EncodeExcerpt(const std::string& sourcePath, const RationalTime& in,
                   const RationalTime& duration, bool soundOnly,
                   const std::string& outPath, std::string& message,
                   const std::string& colorFilter) {
    if (in.rate <= 0 || duration.rate <= 0 || in.value < 0 ||
        duration.value <= 0) {
        message =
            "an excerpt needs a start that is not negative and a "
            "positive length";
        return false;
    }
    std::error_code error;
    if (std::filesystem::exists(outPath, error)) {
        message = "'" + outPath + "' already exists";
        return false;
    }
    const std::string partial =
        outPath + (soundOnly ? ".partial.wav" : ".partial.mp4");
    process::Request request;
    request.arguments = {
        "",    "-nostdin",        "-v", "error",
        "-ss", FfmpegSeconds(in), "-t", FfmpegSeconds(duration),
        "-i",  sourcePath};
    if (soundOnly) {
        request.arguments.insert(request.arguments.end(),
                                 {"-vn", "-c:a", "pcm_s16le", "-ar", "48000"});
    } else {
        request.arguments.insert(
            request.arguments.end(),
            {// PA14 -- one picture and one sound, nothing else: a rush's
             // timecode becomes a `tmcd` track in the .mp4, and its
             // metadata are the camera's and the editor's, which a model
             // needs no more than the track.
             "-map", "0:v:0", "-map", "0:a:0?", "-dn", "-map_metadata", "-1",
             "-vf",
             // The long side at 1920 at most, the other following, even.
             std::string("scale='if(gt(iw,ih),min(1920,iw),-2)':"
                         "'if(gt(iw,ih),-2,min(1920,ih))'") +
                 (colorFilter.empty()
                      ? std::string()
                      : "," + colorFilter +
                            ",setparams=color_primaries=bt709:"
                            "color_trc=bt709:colorspace=bt709") +
                 ",format=yuv420p",
             "-c:v", "libx264", "-preset", "medium", "-crf", "18", "-c:a",
             "aac", "-b:a", "192k", "-write_tmcd", "0", "-movflags",
             "+faststart"});
        if (!colorFilter.empty())
            request.arguments.insert(request.arguments.end(),
                                     {"-color_primaries", "bt709", "-color_trc",
                                      "bt709", "-colorspace", "bt709"});
    }
    request.arguments.insert(request.arguments.end(), {"-y", partial});
    request.standard_output = process::Stream::kDiscard;
    request.standard_error = process::Stream::kPipe;
    request.discard_input = true;
    const process::Result result = ffmpeg::Run(std::move(request));
    if (!result.Started()) {
        message = ffmpeg::StartError(result, "FFmpeg pipes");
        return false;
    }
    if (!result.Succeeded()) {
        std::filesystem::remove(partial, error);
        message =
            "FFmpeg could not cut the excerpt: " + Trimmed(result.error_tail);
        return false;
    }
    std::filesystem::rename(partial, outPath, error);
    if (error) {
        message = "unable to name '" + outPath + "': " + error.message();
        return false;
    }
    return true;
}

bool ParseCreativeOutput(const std::string& kind, const std::string& json,
                         std::string& url, std::string& seed,
                         std::string& message) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(json, parsed, parseError) || !parsed.IsObject()) {
        message = "réponse de fal.ai illisible : " + parseError;
        return false;
    }
    url.clear();
    if (kind == "stock_image") {
        const Value* images = parsed.Find("images");
        if (images != nullptr && images->IsArray() &&
            !images->AsArray().empty() && images->AsArray()[0].IsObject())
            url = StringAt(images->AsArray()[0], "url");
    } else {
        const Value* video = parsed.Find("video");
        if (video != nullptr && video->IsObject())
            url = StringAt(*video, "url");
    }
    if (!StartsWith(url, "https://")) {
        message = std::string(
                      "la réponse de fal.ai ne donne pas d'adresse "
                      "https pour ") +
                  (kind == "stock_image" ? "l'image" : "la vidéo");
        return false;
    }
    seed.clear();
    if (const Value* field = parsed.Find("seed")) {
        int64_t value = 0;
        if (field->IsNumber() && field->AsInt64(value))
            seed = std::to_string(value);
    }
    return true;
}

bool ParseExpandOutput(const std::string& json, std::string& imageUrl,
                       std::string& seed, int64_t& width, int64_t& height,
                       std::string& message) {
    Value parsed;
    std::string parseError;
    if (!Value::Parse(json, parsed, parseError) || !parsed.IsObject()) {
        message = "réponse de fal.ai illisible : " + parseError;
        return false;
    }
    const Value* image = parsed.Find("image");
    if (image == nullptr || !image->IsObject()) {
        message = "la réponse de fal.ai ne contient pas d'image";
        return false;
    }
    imageUrl = StringAt(*image, "url");
    if (!StartsWith(imageUrl, "https://")) {
        message =
            "la réponse de fal.ai ne donne pas d'adresse https pour "
            "l'image";
        return false;
    }
    width = 0;
    height = 0;
    if (const Value* field = image->Find("width")) field->AsInt64(width);
    if (const Value* field = image->Find("height")) field->AsInt64(height);
    seed.clear();
    if (const Value* field = parsed.Find("seed")) {
        int64_t value = 0;
        if (field->IsNumber() && field->AsInt64(value))
            seed = std::to_string(value);
    }
    return true;
}

// ---------------------------------------------------------------------
// GN1 -- fal.ai.
// ---------------------------------------------------------------------

std::string FalQueueAppPath(const std::string& endpoint) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= endpoint.size()) {
        const size_t slash = endpoint.find('/', start);
        const size_t end = slash == std::string::npos ? endpoint.size() : slash;
        parts.push_back(endpoint.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    const size_t keep =
        !parts.empty() && (parts[0] == "workflows" || parts[0] == "comfy") ? 3
                                                                           : 2;
    std::string path;
    for (size_t index = 0; index < parts.size() && index < keep; ++index) {
        if (index != 0) path += "/";
        path += parts[index];
    }
    return path;
}

FalClient::FalClient(HttpTransport transport, std::string key,
                     FalSettings settings)
    : transport_(std::move(transport)),
      key_(std::move(key)),
      settings_(std::move(settings)) {}

bool FalClient::Send(HttpRequest request, bool withKey, HttpResponse& response,
                     std::string& message) const {
    if (!transport_) {
        message = kNoTransportMessage;
        return false;
    }
    if (!StartsWith(request.url, "https://")) {
        message = "adresse refusée, pas en https : " + request.url;
        return false;
    }
    if (withKey) {
        // The key goes to fal.ai's own two hosts and nowhere else.
        if (!StartsWith(request.url, settings_.queue_base + "/") &&
            !StartsWith(request.url, settings_.rest_base + "/")) {
            message = "la clé ne part que vers fal.ai";
            return false;
        }
        request.headers.emplace_back("Authorization", "Key " + key_);
    }
    std::string error;
    if (!transport_(request, response, error)) {
        message = RedactSecret("fal.ai injoignable : " + error, key_);
        return false;
    }
    return true;
}

bool FalClient::Upload(const std::string& bytes, const std::string& contentType,
                       const std::string& fileName, std::string& fileUrl,
                       std::string& message) const {
    Value body = Value::MakeObject();
    body.Set("content_type", Value::MakeString(contentType));
    body.Set("file_name", Value::MakeString(fileName));
    HttpRequest initiate;
    initiate.method = "POST";
    initiate.url = settings_.rest_base +
                   "/storage/upload/initiate?storage_type=fal-cdn-v3";
    initiate.headers = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        // The source frame stays on fal.ai's storage an
        // hour, not indefinitely (the client's own header).
        {"X-Fal-Object-Lifecycle", "{\"expiration_duration_seconds\":3600}"}};
    initiate.body = body.Dump();
    initiate.timeout_seconds = 60.0;
    HttpResponse answer;
    if (!Send(initiate, true, answer, message)) return false;
    if (!IsSuccess(answer.status)) {
        message = RedactSecret("fal.ai refuse l'envoi de l'image (" +
                                   std::to_string(answer.status) +
                                   ") : " + ErrorDetail(answer.body),
                               key_);
        return false;
    }
    Value parsed;
    std::string parseError;
    if (!Value::Parse(answer.body, parsed, parseError)) {
        message = "réponse de fal.ai illisible à l'envoi de l'image";
        return false;
    }
    const std::string uploadUrl = StringAt(parsed, "upload_url");
    const std::string url = StringAt(parsed, "file_url");
    if (!StartsWith(uploadUrl, "https://") || !StartsWith(url, "https://")) {
        message = "fal.ai n'a pas donné d'adresse https pour l'image";
        return false;
    }
    HttpRequest put;
    put.method = "PUT";
    put.url = uploadUrl;
    put.headers = {{"Content-Type", contentType}};
    put.body = bytes;
    put.timeout_seconds = 300.0;
    HttpResponse stored;
    if (!Send(put, false, stored, message)) return false;
    if (!IsSuccess(stored.status)) {
        message = "le stockage de fal.ai refuse l'image (" +
                  std::to_string(stored.status) + ")";
        return false;
    }
    fileUrl = url;
    return true;
}

bool FalClient::Run(const std::string& endpoint, const Value& input,
                    std::string& resultJson, std::string& requestId,
                    std::string& message) const {
    const std::string app =
        settings_.queue_base + "/" + FalQueueAppPath(endpoint);
    HttpRequest submit;
    submit.method = "POST";
    submit.url = settings_.queue_base + "/" + endpoint;
    submit.headers = {{"Accept", "application/json"},
                      {"Content-Type", "application/json"}};
    submit.body = input.Dump();
    submit.timeout_seconds = 60.0;
    HttpResponse submitted;
    if (!Send(submit, true, submitted, message)) return false;
    if (!IsSuccess(submitted.status)) {
        message = RedactSecret("fal.ai refuse la demande (" +
                                   std::to_string(submitted.status) +
                                   ") : " + ErrorDetail(submitted.body),
                               key_);
        return false;
    }
    Value parsed;
    std::string parseError;
    if (!Value::Parse(submitted.body, parsed, parseError) ||
        StringAt(parsed, "request_id").empty()) {
        message = "fal.ai n'a pas donné d'identifiant de demande";
        return false;
    }
    requestId = StringAt(parsed, "request_id");
    const std::string requestPath = app + "/requests/" + requestId;

    double waited = 0.0;
    for (;;) {
        HttpRequest poll;
        poll.method = "GET";
        poll.url = requestPath + "/status";
        poll.headers = {{"Accept", "application/json"}};
        poll.timeout_seconds = 60.0;
        HttpResponse status;
        if (!Send(poll, true, status, message)) return false;
        if (!IsSuccess(status.status)) {
            message = RedactSecret("fal.ai ne dit pas où en est la demande " +
                                       requestId + " (" +
                                       std::to_string(status.status) +
                                       ") : " + ErrorDetail(status.body),
                                   key_);
            return false;
        }
        Value state;
        if (!Value::Parse(status.body, state, parseError)) {
            message = "état de la demande " + requestId + " illisible";
            return false;
        }
        const std::string name = StringAt(state, "status");
        if (name == "COMPLETED") break;
        if (name != "IN_QUEUE" && name != "IN_PROGRESS") {
            message = "fal.ai donne un état inconnu à la demande " + requestId +
                      " : \"" + name + "\"";
            return false;
        }
        if (waited >= settings_.max_wait_seconds) {
            HttpRequest cancel;
            cancel.method = "PUT";
            cancel.url = requestPath + "/cancel";
            cancel.timeout_seconds = 30.0;
            HttpResponse cancelled;
            std::string ignored;
            Send(cancel, true, cancelled, ignored);
            message = "la demande " + requestId + " n'a pas abouti en " +
                      std::to_string(static_cast<int64_t>(waited)) +
                      " s : annulée chez fal.ai (une demande déjà commencée "
                      "peut être facturée)";
            return false;
        }
        if (settings_.sleep) {
            settings_.sleep(settings_.poll_interval_seconds);
        } else {
            std::this_thread::sleep_for(
                std::chrono::duration<double>(settings_.poll_interval_seconds));
        }
        waited += settings_.poll_interval_seconds;
    }

    HttpRequest fetch;
    fetch.method = "GET";
    fetch.url = requestPath;
    fetch.headers = {{"Accept", "application/json"}};
    fetch.timeout_seconds = 120.0;
    HttpResponse result;
    if (!Send(fetch, true, result, message)) return false;
    if (!IsSuccess(result.status)) {
        message =
            RedactSecret("la demande " + requestId + " a échoué chez fal.ai (" +
                             std::to_string(result.status) +
                             ") : " + ErrorDetail(result.body),
                         key_);
        return false;
    }
    resultJson = result.body;
    return true;
}

bool FalClient::Download(const std::string& url, std::string& bytes,
                         std::string& contentType, std::string& message) const {
    HttpRequest get;
    get.method = "GET";
    get.url = url;
    get.timeout_seconds = 300.0;
    HttpResponse answer;
    if (!Send(get, false, answer, message)) return false;
    if (!IsSuccess(answer.status) || answer.body.empty()) {
        message = "le fichier généré n'a pas pu être téléchargé (" +
                  std::to_string(answer.status) + ")";
        return false;
    }
    bytes = std::move(answer.body);
    contentType = answer.content_type;
    return true;
}

// ---------------------------------------------------------------------
// GN2 -- proposals.
// ---------------------------------------------------------------------

void GenerationProposalStore::Add(const GenerationProposal& proposal) {
    const std::lock_guard<std::mutex> lock(mutex_);
    proposals_[proposal.id] = proposal;
}

bool GenerationProposalStore::Take(const Ulid& id,
                                   std::chrono::steady_clock::time_point now,
                                   GenerationProposal& proposal,
                                   std::string& message) {
    return Find(id, now, true, proposal, message);
}

bool GenerationProposalStore::Peek(const Ulid& id,
                                   std::chrono::steady_clock::time_point now,
                                   GenerationProposal& proposal,
                                   std::string& message) {
    return Find(id, now, false, proposal, message);
}

bool GenerationProposalStore::Find(const Ulid& id,
                                   std::chrono::steady_clock::time_point now,
                                   bool take, GenerationProposal& proposal,
                                   std::string& message) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (const auto closed = closed_.find(id); closed != closed_.end()) {
        message = "la proposition " + id + " " + closed->second +
                  " : propose à nouveau (propose_generation) et redemande "
                  "l'accord de la personne";
        return false;
    }
    const auto found = proposals_.find(id);
    if (found == proposals_.end()) {
        message = "aucune proposition " + id +
                  " dans cette session : une génération part d'une "
                  "proposition (propose_generation) que la personne a "
                  "acceptée";
        return false;
    }
    if (now - found->second.created >
        std::chrono::minutes(kProposalLifetimeMinutes)) {
        closed_[id] = "a expiré (" + std::to_string(kProposalLifetimeMinutes) +
                      " minutes)";
        proposals_.erase(found);
        message = "la proposition " + id + " " + closed_[id] +
                  " : propose à nouveau et redemande l'accord de la personne";
        return false;
    }
    proposal = found->second;
    if (take) {
        proposals_.erase(found);
        closed_[id] = "a déjà servi";
    }
    return true;
}

size_t GenerationProposalStore::Size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return proposals_.size();
}

namespace {

Value ExactTime(const RationalTime& time) {
    Value value = Value::MakeObject();
    value.Set("value", Value::MakeInt(time.value));
    value.Set("rate", Value::MakeInt(time.rate));
    return value;
}

// PA12 -- what a creative proposal says: the request as it will go, the
// files that go up with it, the announced price, and the consent it needs.
Value CreativeProposalValue(const GenerationProposal& proposal,
                            bool keyConfigured, bool transportAvailable) {
    const GenerationKind* kind = FindGenerationKind(proposal.kind);
    Value value = Value::MakeObject();
    value.Set("proposal_id", Value::MakeString(proposal.id));
    value.Set("spends", Value::MakeBool(false));
    value.Set("kind", Value::MakeString(proposal.kind));
    value.Set("service", Value::MakeString(proposal.service));
    value.Set("model", Value::MakeString(proposal.model));
    value.Set("announced_cost",
              proposal.announced_cost.empty()
                  ? Value::MakeNull()
                  : Value::MakeString(proposal.announced_cost));
    value.Set("cost_note",
              Value::MakeString(
                  "Prix lu sur la page du modèle (https://fal.ai/models/" +
                  proposal.model +
                  ") : à relire avant d'accepter, il change. Chaque "
                  "génération est facturée sur le compte fal.ai de la "
                  "personne."));
    Value request = proposal.input;
    Value uploads = Value::MakeArray();
    const char* placeholder =
        "(adresse donnée par fal.ai à l'envoi du fichier)";
    // PA14 -- the uploads of an array field, in order.
    Value references = Value::MakeArray();
    for (const GenerationUpload& upload : proposal.uploads) {
        if (upload.field == "image_urls") {
            references.Push(Value::MakeString(placeholder));
        } else {
            request.Set(upload.field, Value::MakeString(placeholder));
        }
        Value item = Value::MakeObject();
        item.Set("field", Value::MakeString(upload.field));
        item.Set("media_id", Value::MakeString(upload.media_id));
        item.Set("what", Value::MakeString(upload.what));
        if (upload.what != "still") item.Set("from", ExactTime(upload.time));
        if (upload.what == "excerpt" || upload.what == "sound")
            item.Set("duration", ExactTime(upload.duration));
        uploads.Push(std::move(item));
    }
    if (!references.AsArray().empty())
        request.Set("image_urls", std::move(references));
    value.Set("request", std::move(request));
    value.Set("uploads", std::move(uploads));
    if (!proposal.uploads.empty())
        value.Set("sent_to",
                  Value::MakeString("Ces fichiers partent sur le stockage "
                                    "de fal.ai (expiration demandée : 1 h) "
                                    "pour que le modèle les lise."));
    value.Set("needs_consent", Value::MakeBool(proposal.needs_consent));
    if (proposal.needs_consent)
        value.Set("consent",
                  Value::MakeString(
                      "Cette génération change ce qu'une personne filmée "
                      "dit ou montre. Elle ne part qu'avec son accord : "
                      "generate_media demande consent_of_filmed_person: "
                      "true, que seule la personne qui signe le film peut "
                      "affirmer."));
    value.Set("verified",
              Value::MakeString(kind != nullptr ? kind->verified : ""));
    Value ready = Value::MakeObject();
    ready.Set("key", Value::MakeBool(keyConfigured));
    ready.Set("transport", Value::MakeBool(transportAvailable));
    value.Set("ready", std::move(ready));
    value.Set("proposed_at", Value::MakeString(proposal.proposed_at));
    value.Set("expires_in_minutes", Value::MakeInt(kProposalLifetimeMinutes));
    value.Set(
        "next",
        Value::MakeString(
            std::string("Rien n'est dépensé. Montre cette proposition à la "
                        "personne, coût compris, et attends sa réponse. "
                        "Seulement si elle accepte : generate_media "
                        "{proposal_id, confirmed_by_person: true") +
            (proposal.needs_consent ? ", consent_of_filmed_person: true" : "") +
            "}. Ne confirme jamais à sa place."));
    return value;
}

}  // namespace

Value ProposalValue(const GenerationProposal& proposal, bool keyConfigured,
                    bool transportAvailable) {
    if (proposal.input.IsObject())
        return CreativeProposalValue(proposal, keyConfigured,
                                     transportAvailable);
    const GenerationKind* kind = FindGenerationKind(proposal.kind);
    Value value = Value::MakeObject();
    value.Set("proposal_id", Value::MakeString(proposal.id));
    value.Set("spends", Value::MakeBool(false));
    value.Set("kind", Value::MakeString(proposal.kind));
    value.Set("service", Value::MakeString(proposal.service));
    value.Set("model", Value::MakeString(proposal.model));
    value.Set("announced_cost",
              proposal.announced_cost.empty()
                  ? Value::MakeNull()
                  : Value::MakeString(proposal.announced_cost));
    if (proposal.announced_cost.empty()) {
        value.Set("cost_note",
                  Value::MakeString(
                      "Coût inconnu du moteur : à lire sur la page du modèle "
                      "(https://fal.ai/models/" +
                      proposal.model +
                      ") avant d'accepter. Chaque génération est facturée "
                      "sur le compte fal.ai de la personne."));
    }
    Value source = Value::MakeObject();
    source.Set("media_id", Value::MakeString(proposal.source_media_id));
    source.Set("width", Value::MakeInt(proposal.source_width));
    source.Set("height", Value::MakeInt(proposal.source_height));
    if (proposal.source_is_frame) {
        Value time = Value::MakeObject();
        time.Set("value", Value::MakeInt(proposal.source_time.value));
        time.Set("rate", Value::MakeInt(proposal.source_time.rate));
        source.Set("frame_at", std::move(time));
    }
    source.Set("sent_to",
               Value::MakeString("L'image source part sur le stockage de "
                                 "fal.ai (expiration demandée : 1 h) pour "
                                 "que le modèle la lise."));
    value.Set("source", std::move(source));
    value.Set("aspect_ratio", Value::MakeString(proposal.aspect_ratio));
    Value request = ExpandInput(proposal.canvas, proposal.prompt,
                                proposal.negative_prompt, proposal.seed);
    request.Set("image_url",
                Value::MakeString("(adresse de l'image source, donnée par "
                                  "fal.ai à la génération)"));
    value.Set("request", std::move(request));
    Value clip = Value::MakeObject();
    clip.Set("frames", Value::MakeInt(proposal.clip_frames));
    Value rate = Value::MakeObject();
    rate.Set("num", Value::MakeInt(proposal.clip_rate.num));
    rate.Set("den", Value::MakeInt(proposal.clip_rate.den));
    clip.Set("rate", std::move(rate));
    clip.Set("note",
             Value::MakeString("La bibliothèque ne prend pas encore d'image "
                               "fixe (REEL-2026-09 R4) : l'image rendue est "
                               "gardée dans le paquet, et un plan de ces "
                               "images en est tiré, qui entre dans la "
                               "bibliothèque."));
    value.Set("clip", std::move(clip));
    value.Set("verified",
              Value::MakeString(kind != nullptr ? kind->verified : ""));
    Value ready = Value::MakeObject();
    ready.Set("key", Value::MakeBool(keyConfigured));
    ready.Set("transport", Value::MakeBool(transportAvailable));
    value.Set("ready", std::move(ready));
    value.Set("proposed_at", Value::MakeString(proposal.proposed_at));
    value.Set("expires_in_minutes", Value::MakeInt(kProposalLifetimeMinutes));
    value.Set("next",
              Value::MakeString(
                  "Rien n'est dépensé. Montre cette proposition à la "
                  "personne, coût compris, et attends sa réponse. Seulement "
                  "si elle accepte : generate_media {proposal_id, "
                  "confirmed_by_person: true}. Ne confirme jamais à sa "
                  "place."));
    return value;
}

MediaGeneration ProvenanceOf(const GenerationProposal& proposal,
                             const std::string& seed,
                             const std::string& requestId,
                             const std::string& createdAt) {
    MediaGeneration provenance;
    provenance.service = proposal.service;
    provenance.model = proposal.model;
    provenance.kind = proposal.kind;
    provenance.prompt = proposal.prompt;
    provenance.seed = seed.empty() ? proposal.seed : seed;
    provenance.created_at = createdAt;
    provenance.announced_cost = proposal.announced_cost;
    provenance.request_id = requestId;
    provenance.source_media_id = proposal.source_media_id;
    return provenance;
}

std::string UtcNow() {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_r(&now, &utc);
    char text[32];
    std::strftime(text, sizeof(text), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

}  // namespace generation

bool ToolSpendsOnPersonsAccount(const std::string& toolName) {
    return toolName == "generate_media";
}
