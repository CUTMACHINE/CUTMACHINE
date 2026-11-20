#pragma once

// GN6b -- docs/roadmap/GENERATIF-2026-10.md. What a voice the engine made
// is, and the one rule about cloning it.
//
// design_voice has VoiceDesign say a reference phrase the engine fixes, in
// a voice made from a description; the WAV enters the library as a media
// marked `service` "local", `kind` "voice_design", with that phrase as its
// `reference_text`. generate_voice_over has the Base model say a text in
// that voice, by cloning the media with its words (VoiceOver.h); the result
// is marked `kind` "voice_over", its `source_media_id` the voice. Nothing
// new in the document but the reference words: the provenance, the GÉNÉRÉ
// badge, check_timeline's line and the deletion guard (DEC36) exist.
//
// DEC49 -- the engine clones only the voices it made, so DEC2 (no voice of
// a filmed person) holds by the engine and not by an agent's goodwill
// (PHILOSOPHY.md, principe 7): a recording, a rush, an interview or a
// voice-over is never a reference. A provenance is written only by a
// generation and cleared only by undo (DEC36), so it cannot be forged
// through the tools.

#include "Document.h"
#include "VoiceOver.h"

#include <string>

inline constexpr const char* kLocalVoiceService = "local";
inline constexpr const char* kVoiceDesignKind = "voice_design";
inline constexpr const char* kVoiceOverKind = "voice_over";

// The stable name surfaces give the refusal of a reference that is not a
// voice the engine made.
inline constexpr const char* kNotAnEngineVoiceErrorName = "NotAnEngineVoice";

// The phrase design_voice has VoiceDesign say in `language`, an ISO 639-1
// code VoiceOverLanguageName knows; empty for any other. Two sentences and
// a pause, about 7 s: long enough for Base to take the voice, short enough
// to make quickly. Only the French one was heard (7 November 2026 bench).
std::string VoiceReferenceText(const std::string& language);

// The model a run used, as provenance names it: the Hugging Face repository
// and the commit its folder was downloaded at ("Qwen/Qwen3-TTS-12Hz-1.7B-
// Base@fd4b2543..."), without "@" when the folder does not say.
std::string VoiceModelName(const VoiceOverResult& result);

// The provenance of what a run made. `prompt` is what was asked: the
// description of a voice, the text of a voice-over. `referenceText` only
// for a voice; `sourceMediaId` only for a voice-over (the voice cloned).
MediaGeneration VoiceProvenance(const std::string& kind,
                                const VoiceOverResult& result,
                                const std::string& prompt,
                                const std::string& referenceText,
                                const Ulid& sourceMediaId,
                                const std::string& createdAt);

// Why `media` cannot be cloned, in one sentence that says what can; empty
// when it is a voice design_voice made, with its reference words.
std::string CloneRefusal(const LibraryMedia& media);
