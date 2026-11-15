#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

class MediaTaskContext;
struct whisper_context;
struct whisper_full_params;

// AL4 -- docs/roadmap/ALIGNEMENT-2026-11.md. A decoding policy changes the
// words without changing the weights, and therefore belongs to cache identity.
enum class WhisperDecodingProfile { Default, FrenchHesitationsV1 };

// The SHA-256 of the s075 weights fr-hesitations-v1 was evaluated with.
inline constexpr const char* kFrenchHesitationsModelSha256 =
    "5b05a0870a122a833e4c74fbc223e1acbb74875bab3e3682d9cef692e51e385e";

// AL4 -- what a caller asked for. No value: the caller named no profile and
// ResolveWhisperDecodingProfile chooses one. `default` named is a request
// like any other, and is kept.
using WhisperDecodingRequest = std::optional<WhisperDecodingProfile>;

// AL4 -- the profile a transcription runs with, the weights it runs on, and
// whether the engine chose them because the caller named no profile: an
// answer says all three.
struct WhisperDecodingChoice {
    WhisperDecodingProfile profile = WhisperDecodingProfile::Default;
    bool implicit = false;
    std::string model_path;
};

const char* WhisperDecodingProfileName(WhisperDecodingProfile profile);
bool ParseWhisperDecodingProfile(const std::string& name,
                                 WhisperDecodingProfile& profile,
                                 std::string& error);
bool ValidateWhisperDecodingRequest(WhisperDecodingProfile profile,
                                    const std::string& language, bool verbatim,
                                    std::string& error);
// A request that names no profile is always valid: its resolution stays in
// the profile's scope.
bool ValidateWhisperDecodingRequest(const WhisperDecodingRequest& request,
                                    const std::string& language, bool verbatim,
                                    std::string& error);
// The experimental profile is valid only for the evaluated weights. Check
// their content rather than trusting a filename that can be renamed.
bool ValidateWhisperDecodingModel(WhisperDecodingProfile profile,
                                  const std::string& path,
                                  MediaTaskContext& context,
                                  std::string& error);
bool ConfigureWhisperDecodingProfile(WhisperDecodingProfile profile,
                                     whisper_context* context,
                                     whisper_full_params& params,
                                     std::string& error);

// AL4 -- whether `path` holds the weights fr-hesitations-v1 was evaluated
// with. A file of another size is told apart without being read. Otherwise
// its SHA-256 is computed once per path, size and modification time in this
// process and remembered: 10 s for the 3 GB weights on the Mac of
// 15 November 2026, paid by every transcription with the profile until then.
bool RecognizeWhisperDecodingModel(const std::string& path,
                                   MediaTaskContext& context, bool& evaluated,
                                   std::string& error);
// The same outside a task (the setup report): `cancelled` stops the read.
bool RecognizeWhisperDecodingModel(const std::string& path,
                                   const std::function<bool()>& cancelled,
                                   bool& evaluated, std::string& error);
// What is known of `path` without reading it: the evaluated weights or not,
// or nothing yet (a file of the same size not read in this process, or no
// readable file at all).
std::optional<bool> KnownWhisperDecodingModel(const std::string& path);

// AL4 -- decided by the author on 15 November 2026, restricted the same day:
// the s075 weights serve French verbatim only (on an English take they stopped
// short where large-v3 went on), from a setting of their own
// (kWhisperHesitationsModelVariable, Transcription.h).
//
// A request that names no profile gets fr-hesitations-v1 on
// `hesitationsModel` when it transcribes French (language "fr", never
// "auto") verbatim and that file is the evaluated weights; otherwise
// `default` on `model`, as before. `default` named keeps `model`.
// fr-hesitations-v1 named runs on `hesitationsModel` when it is the
// evaluated weights, and on `model` otherwise, where the check at loading
// refuses other weights (ValidateWhisperDecodingModel). An empty
// `hesitationsModel` is no setting.
bool ResolveWhisperDecodingProfile(const WhisperDecodingRequest& request,
                                   const std::string& language, bool verbatim,
                                   const std::string& model,
                                   const std::string& hesitationsModel,
                                   MediaTaskContext& context,
                                   WhisperDecodingChoice& choice,
                                   std::string& error);

// The choice ResolveWhisperDecodingProfile makes when it is known without
// reading any weights: `default` named, a request outside French verbatim,
// no hesitations model, or one already told apart. No value: only reading
// the weights tells. For a caller that cannot wait on them (the app's main
// thread).
std::optional<WhisperDecodingChoice> KnownWhisperDecodingChoice(
    const WhisperDecodingRequest& request, const std::string& language,
    bool verbatim, const std::string& model,
    const std::string& hesitationsModel);

// Tests cannot ship 3 GB of weights. While one of these lives, a file of
// `bytes` bytes and SHA-256 `sha256` stands for the evaluated weights, and
// the digests remembered are forgotten when it starts and when it ends.
class ScopedEvaluatedWhisperModelForTesting {
public:
    ScopedEvaluatedWhisperModelForTesting(std::uintmax_t bytes,
                                          std::string sha256);
    ~ScopedEvaluatedWhisperModelForTesting();
    ScopedEvaluatedWhisperModelForTesting(
        const ScopedEvaluatedWhisperModelForTesting&) = delete;
    ScopedEvaluatedWhisperModelForTesting& operator=(
        const ScopedEvaluatedWhisperModelForTesting&) = delete;

private:
    std::uintmax_t previous_bytes_ = 0;
    std::string previous_sha256_;
};

// How many model files this process has hashed: what the memory saves.
std::size_t WhisperModelDigestsComputed();
