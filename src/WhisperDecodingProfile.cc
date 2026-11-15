#include "WhisperDecodingProfile.h"

#include "MediaTaskManager.h"
#include "whisper.h"

extern "C" {
#include <libavutil/mem.h>
#include <libavutil/sha.h>
}

#include <array>
#include <fstream>
#include <memory>
#include <vector>

namespace {
constexpr const char* kFrenchModelSha256 =
    "5b05a0870a122a833e4c74fbc223e1acbb74875bab3e3682d9cef692e51e385e";
constexpr std::array<int, 2> kEuhTokens{32678, 47320};
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

bool ValidateWhisperDecodingModel(WhisperDecodingProfile profile,
                                  const std::string& path,
                                  MediaTaskContext& context,
                                  std::string& error) {
    error.clear();
    if (profile == WhisperDecodingProfile::Default) {
        return true;
    }
    std::ifstream input(path, std::ios::binary);
    std::unique_ptr<AVSHA, decltype(&av_free)> sha(av_sha_alloc(), &av_free);
    if (!input || !sha || av_sha_init(sha.get(), 256) != 0) {
        error = "unable to verify the fr-hesitations-v1 model";
        return false;
    }
    std::vector<char> buffer(std::size_t{1024} * 1024);
    while (input) {
        if (context.Cancelled()) {
            error = "transcription cancelled";
            return false;
        }
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        av_sha_update(sha.get(),
                      reinterpret_cast<const uint8_t*>(buffer.data()),
                      static_cast<size_t>(input.gcount()));
    }
    if (!input.eof()) {
        error = "unable to read the fr-hesitations-v1 model";
        return false;
    }
    std::array<uint8_t, 32> digest{};
    av_sha_final(sha.get(), digest.data());
    std::string hex;
    for (uint8_t byte : digest) {
        hex += "0123456789abcdef"[byte >> 4];
        hex += "0123456789abcdef"[byte & 15];
    }
    if (hex != kFrenchModelSha256) {
        error =
            "fr-hesitations-v1 requires the evaluated s075 model "
            "(SHA-256 " +
            std::string(kFrenchModelSha256) + ")";
        return false;
    }
    return true;
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
