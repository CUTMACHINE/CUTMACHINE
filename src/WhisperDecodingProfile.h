#pragma once

#include <string>

class MediaTaskContext;
struct whisper_context;
struct whisper_full_params;

// AL4 -- docs/roadmap/ALIGNEMENT-2026-11.md. A decoding policy changes the
// words without changing the weights, and therefore belongs to cache identity.
enum class WhisperDecodingProfile { Default, FrenchHesitationsV1 };

const char* WhisperDecodingProfileName(WhisperDecodingProfile profile);
bool ParseWhisperDecodingProfile(const std::string& name,
                                 WhisperDecodingProfile& profile,
                                 std::string& error);
bool ValidateWhisperDecodingRequest(WhisperDecodingProfile profile,
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
