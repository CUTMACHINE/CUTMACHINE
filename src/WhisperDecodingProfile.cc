#include "WhisperDecodingProfile.h"

#include "MediaTaskManager.h"
#include "whisper.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <array>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <tuple>
#include <utility>
#include <vector>

namespace {
constexpr const char* kFrenchModelSha256 = kFrenchHesitationsModelSha256;
// The same weights' size: a file of another size cannot have their digest,
// and is told apart without reading 3 GB.
constexpr std::uintmax_t kFrenchModelBytes = 3095033483;
constexpr std::array<int, 2> kEuhTokens{32678, 47320};

// AL4 -- a digest stands for the file as long as its path, size and
// modification time do. Replacing the weights by others of the same size
// with the same modification time would go unseen until the process ends.
struct ModelKey {
    std::string path;
    std::uintmax_t bytes = 0;
    std::filesystem::file_time_type modified;
    bool operator<(const ModelKey& other) const {
        return std::tie(path, bytes, modified) <
               std::tie(other.path, other.bytes, other.modified);
    }
};

struct ModelRecognition {
    std::mutex mutex;
    std::uintmax_t bytes = kFrenchModelBytes;
    std::string sha256 = kFrenchModelSha256;
    std::map<ModelKey, std::string> digests;
    std::size_t computed = 0;
};

ModelRecognition& Recognition() {
    static ModelRecognition recognition;
    return recognition;
}

bool StatModel(const std::string& path, ModelKey& key) {
    std::error_code error;
    key.path = path;
    key.bytes = std::filesystem::file_size(path, error);
    if (error) {
        return false;
    }
    key.modified = std::filesystem::last_write_time(path, error);
    return !error;
}

// Evaluated or not, when that is known without reading the file.
std::optional<bool> KnownEvaluated(const ModelKey& key) {
    ModelRecognition& recognition = Recognition();
    const std::lock_guard<std::mutex> lock(recognition.mutex);
    if (key.bytes != recognition.bytes) {
        return false;
    }
    const auto found = recognition.digests.find(key);
    if (found == recognition.digests.end()) {
        return std::nullopt;
    }
    return found->second == recognition.sha256;
}

bool Sha256OfFile(const std::string& path,
                  const std::function<bool()>& cancelled, std::string& hex,
                  std::string& error) {
    std::ifstream input(path, std::ios::binary);
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), &av_free);
    if (!input || !sha || av_sha_init(sha.get(), 256) != 0) {
        error = "unable to verify the fr-hesitations-v1 model '" + path + "'";
        return false;
    }
    std::vector<char> buffer(std::size_t{1024} * 1024);
    while (input) {
        if (cancelled && cancelled()) {
            error = "transcription cancelled";
            return false;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        av_sha_update(sha.get(),
                      reinterpret_cast<const uint8_t*>(buffer.data()),
                      static_cast<size_t>(input.gcount()));
    }
    if (!input.eof()) {
        error = "unable to read the fr-hesitations-v1 model '" + path + "'";
        return false;
    }
    std::array<uint8_t, 32> digest{};
    av_sha_final(sha.get(), digest.data());
    hex.clear();
    for (uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    return true;
}

// A hesitations model the resolution can use: set, and a file there. A
// setting pointing at nothing is no setting, as the setup report says.
bool HesitationsModelSet(const std::string& path) {
    std::error_code error;
    return !path.empty() && std::filesystem::is_regular_file(path, error);
}
}  // namespace

const char* WhisperDecodingProfileName(WhisperDecodingProfile profile) {
    switch (profile) {
        case WhisperDecodingProfile::Default:
            return "default";
        case WhisperDecodingProfile::FrenchHesitationsV1:
            return "fr-hesitations-v1";
    }
    return "unknown";
}

bool ParseWhisperDecodingProfile(const std::string& name,
                                 WhisperDecodingProfile& profile,
                                 std::string& error) {
    error.clear();
    if (name == "default") {
        profile = WhisperDecodingProfile::Default;
        return true;
    }
    if (name == "fr-hesitations-v1") {
        profile = WhisperDecodingProfile::FrenchHesitationsV1;
        return true;
    }
    error = "unknown Whisper decoding_profile '" + name + "'";
    return false;
}

bool ValidateWhisperDecodingRequest(WhisperDecodingProfile profile,
                                    const std::string& language, bool verbatim,
                                    std::string& error) {
    error.clear();
    if (profile == WhisperDecodingProfile::Default) {
        return true;
    }
    if (profile == WhisperDecodingProfile::FrenchHesitationsV1 &&
        language == "fr" && verbatim) {
        return true;
    }
    error = "fr-hesitations-v1 requires language=fr and verbatim=true";
    return false;
}

bool ValidateWhisperDecodingRequest(const WhisperDecodingRequest& request,
                                    const std::string& language, bool verbatim,
                                    std::string& error) {
    error.clear();
    return !request ||
           ValidateWhisperDecodingRequest(*request, language, verbatim, error);
}

bool ValidateWhisperDecodingModel(WhisperDecodingProfile profile,
                                  const std::string& path,
                                  MediaTaskContext& context,
                                  std::string& error) {
    error.clear();
    if (profile == WhisperDecodingProfile::Default) {
        return true;
    }
    bool evaluated = false;
    if (!RecognizeWhisperDecodingModel(path, context, evaluated, error)) {
        return false;
    }
    if (!evaluated) {
        error =
            "fr-hesitations-v1 requires the evaluated s075 model "
            "(SHA-256 " +
            std::string(kFrenchModelSha256) +
            "), as CUTMACHINE_WHISPER_HESITATIONS_MODEL or as the model";
        return false;
    }
    return true;
}

bool RecognizeWhisperDecodingModel(const std::string& path,
                                   MediaTaskContext& context, bool& evaluated,
                                   std::string& error) {
    return RecognizeWhisperDecodingModel(
        path, [&context] { return context.Cancelled(); }, evaluated, error);
}

bool RecognizeWhisperDecodingModel(const std::string& path,
                                   const std::function<bool()>& cancelled,
                                   bool& evaluated, std::string& error) {
    error.clear();
    evaluated = false;
    // An unreadable file is not known: the read below says why.
    ModelKey key;
    const bool stated = StatModel(path, key);
    if (stated) {
        if (const std::optional<bool> known = KnownEvaluated(key)) {
            evaluated = *known;
            return true;
        }
    }
    std::string hex;
    if (!Sha256OfFile(path, cancelled, hex, error)) {
        return false;
    }
    ModelRecognition& recognition = Recognition();
    const std::lock_guard<std::mutex> lock(recognition.mutex);
    ++recognition.computed;
    if (stated) {
        recognition.digests[key] = hex;
    }
    evaluated = hex == recognition.sha256;
    return true;
}

std::optional<bool> KnownWhisperDecodingModel(const std::string& path) {
    ModelKey key;
    if (!StatModel(path, key)) {
        return std::nullopt;
    }
    return KnownEvaluated(key);
}

std::optional<WhisperDecodingChoice> KnownWhisperDecodingChoice(
    const WhisperDecodingRequest& request, const std::string& language,
    bool verbatim, const std::string& model,
    const std::string& hesitationsModel) {
    const bool implicit = !request.has_value();
    const WhisperDecodingProfile asked =
        request.value_or(WhisperDecodingProfile::FrenchHesitationsV1);
    if (asked == WhisperDecodingProfile::Default ||
        (implicit && (language != "fr" || !verbatim)) ||
        !HesitationsModelSet(hesitationsModel)) {
        // fr-hesitations-v1 named without its own weights stays on `model`.
        return WhisperDecodingChoice{
            implicit ? WhisperDecodingProfile::Default : asked, implicit,
            model};
    }
    const std::optional<bool> evaluated =
        KnownWhisperDecodingModel(hesitationsModel);
    if (!evaluated) {
        return std::nullopt;
    }
    if (*evaluated) {
        return WhisperDecodingChoice{
            WhisperDecodingProfile::FrenchHesitationsV1, implicit,
            hesitationsModel};
    }
    return WhisperDecodingChoice{
        implicit ? WhisperDecodingProfile::Default : asked, implicit, model};
}

bool ResolveWhisperDecodingProfile(const WhisperDecodingRequest& request,
                                   const std::string& language, bool verbatim,
                                   const std::string& model,
                                   const std::string& hesitationsModel,
                                   MediaTaskContext& context,
                                   WhisperDecodingChoice& choice,
                                   std::string& error) {
    error.clear();
    if (!ValidateWhisperDecodingRequest(request, language, verbatim, error)) {
        return false;
    }
    if (const std::optional<WhisperDecodingChoice> known =
            KnownWhisperDecodingChoice(request, language, verbatim, model,
                                       hesitationsModel)) {
        choice = *known;
        return true;
    }
    bool evaluated = false;
    if (!RecognizeWhisperDecodingModel(hesitationsModel, context, evaluated,
                                       error)) {
        return false;
    }
    const bool implicit = !request.has_value();
    if (evaluated) {
        choice = {WhisperDecodingProfile::FrenchHesitationsV1, implicit,
                  hesitationsModel};
    } else {
        choice = {request.value_or(WhisperDecodingProfile::Default), implicit,
                  model};
    }
    return true;
}

ScopedEvaluatedWhisperModelForTesting::ScopedEvaluatedWhisperModelForTesting(
    std::uintmax_t bytes, std::string sha256)
    : previous_bytes_(bytes), previous_sha256_(std::move(sha256)) {
    ModelRecognition& recognition = Recognition();
    const std::lock_guard<std::mutex> lock(recognition.mutex);
    std::swap(recognition.bytes, previous_bytes_);
    recognition.sha256.swap(previous_sha256_);
    recognition.digests.clear();
}

// Swaps back what the constructor swapped: nothing here allocates.
ScopedEvaluatedWhisperModelForTesting::
    ~ScopedEvaluatedWhisperModelForTesting() {
    ModelRecognition& recognition = Recognition();
    const std::lock_guard<std::mutex> lock(recognition.mutex);
    std::swap(recognition.bytes, previous_bytes_);
    recognition.sha256.swap(previous_sha256_);
    recognition.digests.clear();
}

std::size_t WhisperModelDigestsComputed() {
    ModelRecognition& recognition = Recognition();
    const std::lock_guard<std::mutex> lock(recognition.mutex);
    return recognition.computed;
}

bool ConfigureWhisperDecodingProfile(WhisperDecodingProfile profile,
                                     whisper_context* context,
                                     whisper_full_params& params,
                                     std::string& error) {
    error.clear();
    if (profile == WhisperDecodingProfile::Default) {
        return true;
    }
    for (int token : kEuhTokens) {
        const char* raw = whisper_token_to_str(context, token);
        const std::string text = raw == nullptr ? "" : raw;
        if (text != "euh" && text != " euh" && text != "Euh" &&
            text != " Euh") {
            error = "unexpected vocabulary for fr-hesitations-v1";
            return false;
        }
    }
    // Timestamps do not end an immediate repetition, but punctuation does.
    // This is a soft prior: an audible repeated filler remains possible.
    params.logits_filter_callback = [](whisper_context* ctx, whisper_state*,
                                       const whisper_token_data* history,
                                       int count, float* logits, void*) {
        bool repeated = false;
        for (int index = count - 1; index >= 0; --index) {
            const int token = history[index].id;
            if (token >= whisper_token_eot(ctx)) {
                continue;
            }
            repeated = token == kEuhTokens[0] || token == kEuhTokens[1];
            break;
        }
        const float delta = repeated ? 0.0f : 0.50f;
        for (int token : kEuhTokens) {
            logits[token] += delta;
        }
    };
    return true;
}
