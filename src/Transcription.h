#pragma once

#include "Document.h"
#include "MediaTaskManager.h"
#include "Operations.h"
#include "RationalTime.h"
#include "SpeechOnset.h"
#include "Ulid.h"
#include "WhisperDecodingProfile.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// F1.4 -- transcription and word-level cutting. See PHILOSOPHY.md's "pas de
// service central": transcription runs a local whisper.cpp model against
// media already on disk. There is no network call, no account, no cloud API
// in this file.

// One transcribed word. `start`/`end` are exact positions in the source
// media's own time domain -- the same domain as DocumentSource::duration and
// DocumentClip::source_in/duration, at `Transcript::source_rate`. See
// RoundToSourceFrame below for the single explicit rounding rule that gets
// them there from whisper.cpp's native centisecond timestamps.
struct TranscriptWord {
    std::string text;
    RationalTime start;
    RationalTime end;
};

// A transcript is a cache artifact, not document state (PHILOSOPHY.md
// principle 1: the document is only ever a JSON file describing the edit).
// It is keyed on `media_id`, the same LibraryMedia/DocumentSource ULID the
// rest of the per-media cache is keyed on -- see main.mm's
// loadOrEnqueueWaveformForMediaIdentifier and its neighbors for the existing
// `.cutmachine/<kind>/<media_id>.<ext>` convention this mirrors instead of
// inventing a new one.
struct Transcript {
    std::string media_id;
    // Recorded so a transcript produced by a different local model is never
    // silently reused as if it were this one -- e.g. a project migrated to a
    // larger local model should re-transcribe, not read stale words.
    std::string whisper_model;
    // Whisper's language constraint changes the decoded words just as much
    // as the model does. It is therefore part of the cache identity; "auto"
    // is explicit so legacy caches have a deterministic meaning.
    std::string language = "auto";
    // ITW9 -- docs/roadmap/ITW-2026-09.md. The language whisper.cpp
    // detected when `language` is "auto": the one most of the words were
    // decoded in. Empty when the language was forced, and in caches made
    // before it was recorded. Not part of the cache identity: it is what a
    // transcription found, not what it was asked. mfa_align.py aligns a
    // rush in it, with that language's model.
    std::string detected_language;
    // Verbatim decoding deliberately biases Whisper toward audible fillers,
    // repetitions and false starts. Keep it in the cache identity so a
    // standard transcript is never silently substituted for one.
    bool verbatim = false;
    // AL4 -- distinct decoding policies must never reuse each other's cache.
    WhisperDecodingProfile decoding_profile = WhisperDecodingProfile::Default;
    // ITW9 -- docs/roadmap/ITW-2026-09.md. How many of a verbatim
    // transcript's words come from a pass made without the verbatim prompt
    // (CompleteVerbatimWithPlainPass): the verbatim pass came out
    // likely_incomplete, and its words now stand only as the hesitations it
    // heard where the plain pass heard nothing. Zero, and never written, for
    // every other transcript, which keeps its bytes. Not part of the cache
    // identity: it is what a transcription found, not what it was asked.
    // A reader that says `verbatim` says this beside it: those words carry
    // no hesitation the verbatim pass did not keep.
    size_t plain_pass_words = 0;
    // ITW9 -- how many words FillEllipsesFromPass put where whisper had
    // answered "..." over measured speech: they come from a pass made with
    // the ellipsis forbidden. Zero, and never written, for every other
    // transcript, which keeps its bytes; not part of the cache identity.
    size_t ellipsis_pass_words = 0;
    // QC-2026-09 (A1) -- true once AlignTranscriptToSpeech has been applied to
    // this cache file, so a reader can tell a boundary Whisper claimed from
    // one the signal confirmed. It is not a guarantee that every word moved
    // (most are already right, and the ones with no speech edge nearby are
    // deliberately refused); it says only that the pass has run, which is
    // what distinguishes a transcript worth cutting on from one that still
    // needs probing.
    bool speech_aligned = false;
    // QC-2026-09b B12 -- result of comparing this text with the measured
    // speech groups. This belongs to the transcript cache, not the project:
    // it describes whether one inference artifact is supported by another
    // measured artifact and can change when either is regenerated.
    bool speech_assessed = false;
    RationalTime measured_speech_duration{0, 1};
    bool likely_hallucinated = false;
    // A9 -- measured speech long enough to assess, but implausibly few words.
    // This is the opposite failure from likely_hallucinated: audio evidence
    // exists and the inference appears to have dropped most of it.
    bool likely_incomplete = false;
    // A known caption/credit/subscription phrase is supporting evidence only.
    // It never marks a transcript by itself: a real speaker may use the same
    // words, and measured speech must win over a blacklist match.
    bool known_hallucination_phrase = false;
    // The exact timebase TranscriptWord::start/end are expressed in. This is
    // normally the source media's own MediaRate (DocumentSource::rate), so a
    // word's RationalTime is already frame-exact in the domain
    // DocumentClip::source_in/duration use -- no further rescale needed by
    // callers.
    MediaRate source_rate;
    std::vector<TranscriptWord> words;
    // ITW3 -- ROADMAP.md. Who placed the word boundaries: "whisper" as
    // cached, or the aligner method (WordAlignment.h) once LoadSourceWords
    // has replaced them. Not written to the cache, and a transcript that is
    // not "whisper" refuses to be saved, so aligned words never overwrite
    // the Whisper cache they were derived from.
    std::string word_timing = "whisper";
};

struct WhisperSettings {
    // Path to a local ggml model file (e.g. "ggml-base.en.bin"). Never
    // downloaded or resolved from a remote service by this code; the caller
    // supplies a path already on disk.
    std::string whisper_model_path;
    // Empty: the binary FfmpegBinary.h resolves (PRD1).
    std::string ffmpeg_path;
    // ISO 639-1 code, or "auto" to let whisper.cpp detect the language.
    std::string language = "auto";
    bool verbatim = false;
    // ITW9 -- docs/roadmap/ITW-2026-09.md. Forbids the tokens made only of
    // dots or ellipsis characters. Set by the transcription command alone,
    // on the pass it makes again over speech that came out as an ellipsis
    // (EllipsisOverSpeech); never part of what a caller asks.
    bool suppress_ellipsis = false;
    // AL4 -- distinct decoding policies must never reuse each other's cache.
    WhisperDecodingProfile decoding_profile = WhisperDecodingProfile::Default;
};

// ITW9 -- docs/roadmap/ITW-2026-09.md. Adds `words` decoded in `language`
// to the counts, kept in the order languages were first heard.
void CountWordsInLanguage(std::vector<std::pair<std::string, size_t>>& counts,
                          const std::string& language, size_t words);

// The language most words were decoded in; the first heard on a tie; empty
// when nothing was counted. What Transcript::detected_language records.
std::string MostSpokenLanguage(
    const std::vector<std::pair<std::string, size_t>>& counts);

// A cache can be reused only for the exact decoding request that produced it.
// The model is compared by filename because caches deliberately never persist
// a machine-local absolute path.
bool TranscriptCacheMatches(const Transcript& transcript,
                            const std::string& mediaId,
                            const WhisperSettings& settings);

// ALPHA-2026-08 -- the model path is the one thing standing between the
// agent and a transcript it can produce rather than only read. It cannot be a
// tool argument (an agent has no way to know where a human keeps a ggml file,
// and a wrong guess is a silent failure) and it cannot live in the project
// (it is machine-local; see LocalEnv.h). So it is a local setting, resolved
// here once for every surface: the CLI, the MCP tool and the app all fail the
// same way, with a message naming the variable and the file to edit.
//
// Returns false with a user-facing reason when nothing is configured or the
// configured path is not a file. Never downloads anything.
bool ResolveConfiguredWhisperModel(std::string& path, std::string& reason);

// The machine-local setting that names the model.
inline constexpr const char* kWhisperModelVariable = "CUTMACHINE_WHISPER_MODEL";

// PRD21 -- docs/roadmap/PRODUIT-2026-10b.md. The same resolution on values
// the caller supplies: `configured` is the setting's value, `settingsFile`
// the file a reason names, `isRegularFile` what is at a path. The setup
// report (SetupCheck.h) resolves the model through it on the machine it is
// given, so it says what the tools will find rather than a guess of its own.
bool ResolveWhisperModel(
    const std::string& configured, const std::string& settingsFile,
    const std::function<bool(const std::string&)>& isRegularFile,
    std::string& path, std::string& reason);

// One media to transcribe. The caller resolves the paths and the source's
// own frame rate, because both are project questions, not model ones.
struct TranscriptionJob {
    std::string media_id;
    std::string input_path;
    std::string output_path;
    MediaRate source_rate;
};

struct TranscriptionOutcome {
    std::string media_id;
    bool ok = false;
    // Empty on success; on failure, why this one media failed. A batch does
    // not stop at the first one.
    std::string error;
    size_t words = 0;
    bool speech_assessed = false;
    RationalTime measured_speech_duration{0, 1};
    bool likely_hallucinated = false;
    bool likely_incomplete = false;
    bool known_hallucination_phrase = false;
    // AGENT-2026-09 A27 -- the words, joined, so the caller reads what it
    // just made instead of opening the cache file the reply names: both
    // runs of the 25 September bench did, first thing after transcribing.
    std::string text;
    bool text_truncated = false;
    // ITW9 -- Transcript::detected_language of what was made; empty when
    // the language was forced.
    std::string detected_language;
    // PRD40 -- the transcription was made but the cached one stays
    // (CachedTranscriptPreferredReason): `words` and `text` are what was
    // made, `cached_words` what the cache still holds.
    bool cache_kept = false;
    std::string cache_kept_reason;
    size_t cached_words = 0;
    // ITW9 -- a verbatim pass that came out likely_incomplete was done again
    // without the verbatim prompt: what each pass decoded, how many of the
    // verbatim pass's hesitations were kept, and whether the result took
    // the verbatim pass's place (`plain_pass_detail` says why when not).
    bool plain_pass_tried = false;
    bool plain_pass_used = false;
    size_t verbatim_pass_words = 0;
    size_t plain_pass_words = 0;
    size_t kept_verbatim_words = 0;
    std::string plain_pass_detail;
    // ITW9 -- the transcript answered "..." over measured speech
    // (EllipsisOverSpeech at least kEllipsisRepairMinSpeechCentis) and the
    // pass was made again with the ellipsis forbidden: how much speech the
    // ellipses covered, how many of them were filled, how many stay because
    // what was decoded under them was refused (FillEllipsesFromPass), the
    // words put in their place, and how many of those came from the plain
    // pass asked after a verbatim one.
    bool ellipsis_pass_tried = false;
    RationalTime ellipsis_speech{0, 1};
    size_t ellipsis_spans = 0;
    size_t ellipsis_spans_filled = 0;
    size_t ellipsis_spans_refused = 0;
    size_t ellipsis_pass_words = 0;
    size_t ellipsis_plain_words = 0;
    std::string ellipsis_pass_detail;
};

struct TranscriptSpeechAssessment {
    size_t word_count = 0;
    RationalTime measured_speech_duration{0, 1};
    bool known_hallucination_phrase = false;
    bool likely_hallucinated = false;
    bool likely_incomplete = false;
};

constexpr int64_t kSparseTranscriptMinSpeechSeconds = 10;
constexpr int64_t kSparseTranscriptMinWordsPerMinute = 30;

// A9/B12 -- ROADMAP.md. Compares decoded words with B1's measured speech
// groups. The exact sum of group durations is retained rather than converted
// to floating point. Known caption/credit/subscription phrases strengthen the
// hallucination evidence but cannot make a voiced rush unsafe; sustained
// speech below the conservative word-density floor is flagged incomplete.
bool AssessTranscriptAgainstSpeech(const Transcript& transcript,
                                   const SpeechOnsetReport& envelope,
                                   TranscriptSpeechAssessment& assessment,
                                   std::string& error);

// Copies an assessment into the cache fields consumers enforce. Kept
// separate from the pure comparison so tests can inspect the decision before
// any artifact is rewritten.
void ApplyTranscriptSpeechAssessment(
    Transcript& transcript, const TranscriptSpeechAssessment& assessment);

// ITW9 -- docs/roadmap/ITW-2026-09.md. Four takes of 51, in English and
// Spanish, came out of the verbatim pass with 2 to 6 words for 13 to 22 s of
// voice -- the crew's lines at the head and tail, the speaker's sentences
// skipped under words stretched over seconds -- and the pass without the
// verbatim prompt gave them back whole. Only after the speech envelope
// flags such a pass likely_incomplete is a plain pass made, and then its
// words are the transcript: what the verbatim pass decoded is not to be
// trusted there, except for the one thing it is for. Its hesitations
// (IsHesitationToken: "euh", "uh", "um"...), held no longer than
// kMaxKeptHesitationCentis, are kept where they overlap no word of the
// plain pass. `verbatim` stays true, as asked; `plain_pass_words` says how
// many words came from the plain pass. Speech assessment is left to the
// caller, which reassesses the result. `keptVerbatimWords` counts the
// hesitations kept.
inline constexpr int64_t kMaxKeptHesitationCentis = 200;
Transcript CompleteVerbatimWithPlainPass(const Transcript& verbatim,
                                         const Transcript& plain,
                                         size_t& keptVerbatimWords);

// ITW9 -- what a plain pass made after a verbatim one the speech envelope
// flagged likely_incomplete does to it. The plain pass takes its place --
// CompleteVerbatimWithPlainPass, assessed against `envelope`, into
// `transcript` -- when it decoded more words and is not itself flagged
// likely_hallucinated; otherwise `transcript` is left as it was. Either way
// `outcome` says what was tried (plain_pass_tried, the two passes' word
// counts, plain_pass_detail when not taken) and, when taken, carries the
// result's assessment and the hesitations kept. True when taken.
bool TakePlainPassOverIncompleteVerbatim(Transcript& transcript,
                                         const Transcript& plain,
                                         const SpeechOnsetReport& envelope,
                                         TranscriptionOutcome& outcome);

// Whether one decoded word is a hesitation, in the languages the verbatim
// pass has met: the French fillers FindDisfluencies removes, and "uh",
// "um", "er", "eh", "ah", "mm" and their held forms.
bool IsHesitationToken(const std::string& text);

// ITW9 -- docs/roadmap/ITW-2026-09.md, measured on 15 November 2026. What
// emptied a take is an ellipsis: whisper answers "..." alone, with
// confidence, over speech it hears badly (a far microphone, a room), and
// the segment's end timestamp carries the decoding past that speech. The
// plain pass does it; the verbatim pass more, because its prompt ends in
// "...": on 45 rushes of two series, it left 399 s of measured speech under
// ellipses where the plain pass left 158 s (none on the 12 drawn at
// random). Forbidding the token gives the words back -- 40 to 55 words on
// three conference takes both passes had emptied -- and, where nothing
// intelligible is said, makes whisper fall back on its caption credits or
// loop on a phrase.
//
// Whether `text` is made only of dots, ellipsis characters and spaces.
bool IsEllipsisOnlyWord(const std::string& text);

// The measured speech (in the envelope's rate) that the transcript's
// ellipsis-only words cover, each word counted for its overlap with the
// speech groups. Zero for a transcript and envelope of different media.
RationalTime EllipsisOverSpeech(const Transcript& transcript,
                                const SpeechOnsetReport& envelope);

// Below this much covered speech in a transcript, no pass is made again:
// a sentence ending on "..." covers a few tenths of a second.
inline constexpr int64_t kEllipsisRepairMinSpeechCentis = 200;
// An ellipsis covering less speech than this is left as it is.
inline constexpr int64_t kEllipsisSpanMinSpeechCentis = 50;

struct EllipsisRepair {
    size_t spans = 0;
    size_t filled = 0;
    size_t refused = 0;
    size_t words = 0;
};

// Puts in place of each ellipsis-only word of `transcript` that covers at
// least kEllipsisSpanMinSpeechCentis of measured speech the words of
// `redo` -- the same media, the same pass, made with
// WhisperSettings::suppress_ellipsis -- whose middle falls inside it. The
// words of one ellipsis are refused together, and the ellipsis stays, when
// they hold a known caption credit (the phrases of
// known_hallucination_phrase), loop (four words or more, fewer than half
// of them distinct), or are too few for the speech the ellipsis covers
// (below kSparseTranscriptMinWordsPerMinute, the floor of
// likely_incomplete: one "Merci." over 25 s of voice): that is what
// whisper says when it may not say "..." and hears nothing it can decode. A
// word of `redo` that would overlap a word kept from `transcript` is dropped.
// `ellipsis_pass_words` counts the words put in place; speech assessment is
// left to the caller. True when at least one ellipsis was filled.
bool FillEllipsesFromPass(Transcript& transcript, const Transcript& redo,
                          const SpeechOnsetReport& envelope,
                          EllipsisRepair& repair);

// PRD40 -- ROADMAP.md. Whether a transcription just made may take the place
// of the one cached for the same media. Every agent of a series reads the
// project's cache, and one degraded run -- five words, flagged
// likely_incomplete, for a 29 s rush already transcribed whole -- replaced
// it for all of them. Empty when it may; otherwise the reason it may not:
// the new one is flagged (likely_incomplete, likely_hallucinated) and the
// cached one is not, or, made the same way (verbatim or not), it holds fewer
// than half the cached one's words. A cached transcript that is itself
// flagged, or empty, never stands in the way. Whether an explicit overwrite
// passes over the reason is the caller's decision.
std::string CachedTranscriptPreferredReason(const Transcript& cached,
                                            const Transcript& incoming);

// PRD40 -- puts `incoming` in the cache file at `cachePath`, unless that
// file holds a transcript CachedTranscriptPreferredReason prefers and
// `overwrite` is false: the cache is then left as it is, and `outcome` says
// so (cache_kept, the reason, the cached word count). False, with `error`,
// only when the install was due and failed.
bool InstallTranscriptUnlessWorse(const std::string& cachePath,
                                  const Transcript& incoming, bool overwrite,
                                  TranscriptionOutcome& outcome,
                                  std::string& error);

// QC-2026-09 A3 -- transcribes a batch of media against one loaded model.
// Loading it measured about 8 s, paid once per `--transcribe` call before
// this existed: on the 43 spoken rushes of one project that was nearly six
// minutes of loading the same file over and over, against 11 min of actual
// inference. Decodes each media's audio locally via FFmpeg (mirrors
// Waveform.cc's decode pipeline), runs local whisper.cpp inference against
// the PCM samples, and writes each transcript as JSON to its `output_path`
// (creating parent directories as needed). No network access, no account:
// `settings.whisper_model_path` must already be a local file.
//
// Returns false only when the batch itself could not start -- an empty job
// list, or a model that will not load. A media that fails on its own is
// reported in its own outcome, because one unreadable rush is not a reason
// to throw away the model load the others still need.
bool GenerateAudioTranscripts(const std::vector<TranscriptionJob>& jobs,
                              const WhisperSettings& settings,
                              MediaTaskContext& context,
                              std::vector<TranscriptionOutcome>& outcomes,
                              std::string& error);

// The single-media form, in terms of the batch. Kept because most callers
// have exactly one media and should not have to build a vector for it.
bool GenerateAudioTranscript(const std::string& inputPath,
                             const std::string& outputPath,
                             const std::string& mediaId,
                             const MediaRate& sourceRate,
                             const WhisperSettings& settings,
                             MediaTaskContext& context, std::string& error);

// B7 -- ROADMAP.md. The timeline decoder already has an FFmpeg filter graph
// for the audible edit; it must be able to hand its 16 kHz float PCM directly
// to Whisper instead of writing a throw-away media file only to decode it
// again. `ffmpegArguments` excludes the executable and must write f32le PCM
// to stdout. This is deliberately an engine seam rather than a CLI shortcut:
// a timeline's audio is an arrangement, not a LibraryMedia file.
bool DecodeFfmpegAudioToPcm16k(const std::string& ffmpegPath,
                               const std::vector<std::string>& ffmpegArguments,
                               MediaTaskContext& context,
                               std::vector<float>& samples, std::string& error);

// Runs one local Whisper model against already decoded mono 16 kHz PCM and
// atomically writes the normal Transcript cache. See
// DecodeFfmpegAudioToPcm16k for why B7 needs this form.
bool GenerateAudioTranscriptFromPcm(const std::vector<float>& samples,
                                    const std::string& outputPath,
                                    const std::string& mediaId,
                                    const MediaRate& sourceRate,
                                    const WhisperSettings& settings,
                                    MediaTaskContext& context,
                                    std::string& error);

// QC-2026-09 (A1) -- writing a transcript back is a caller's decision, so
// the one implementation of the write path is exposed rather than duplicated.
// Same atomic temp-file-then-rename install GenerateAudioTranscript uses: a
// cache half-rewritten under a reader would be worse than a stale one.
bool SaveAudioTranscript(const std::string& path, const Transcript& transcript,
                         std::string& error);

// Loads a transcript previously written by GenerateAudioTranscript. Rejects
// genuinely out-of-order input, while deterministically folding legacy
// zero-frame and outward-rounding collisions into neighboring words. Every
// returned word is therefore exact, positive and non-overlapping.
bool LoadAudioTranscript(const std::string& path, Transcript& transcript,
                         std::string& error);

// One inclusive span of transcript word indices to remove. Deliberately not
// a frame range: this is the granularity an agent or a UI names ("remove
// words 12 through 15"), never a frame number it computed itself.
struct WordRange {
    size_t start_word_index;
    size_t end_word_index;
};

// What kind of thing a detected stretch of words is. The two are kept apart
// because they do not carry the same risk: a filler syllable is never a word
// a speaker meant to say, while an immediately repeated word can be
// deliberate emphasis ("tres tres bien"). A caller that cleans automatically
// should act on Filler and put Repetition in front of a human.
enum class DisfluencyKind {
    Filler,
    Repetition,
    // AGENT-2026-09 A35 -- a discourse marker ("donc", "en fait", "en
    // gros"...): listed so an agent finds the indices to remove without
    // reading the whole rush, never removed by clean_disfluencies unless
    // asked, since the same word often carries the sentence.
    Marker,
};

struct Disfluency {
    WordRange range;
    DisfluencyKind kind;
    // The transcript text the range covers, so a proposed cut can be
    // reviewed without reading the transcript back.
    std::string text;
};

// F1.4 -- deterministic detection of French disfluencies over an already
// frame-exact transcript. No model is involved, on purpose: what can be
// proven from the words is decided here, and what needs judgement ("is this
// 'donc' a tic or the argument?") is deliberately left out rather than
// guessed. The result is sorted and non-overlapping, which is exactly
// ResolveWordRemoval's precondition, so cleaning up a clip is a straight
// hand-off from this function to that one -- and stays one reversible
// RemoveWordsOperation.
//
// Only a transcript generated with WhisperSettings::verbatim carries these
// words at all: Whisper's default decoding silently drops them (measured on
// a 7 min 35 interview -- 3 tics kept out of 20 actually spoken).
std::vector<Disfluency> FindDisfluencies(
    const std::vector<TranscriptWord>& words);

// The same detection, restricted to the words `clip` actually plays. A clip
// in a montage exposes a few seconds of a media that may run for minutes, and
// ApplyOperation refuses any range outside the clip's own source span -- so
// proposing one would only ever produce an error a caller has to guess its
// way out of. Word indices stay those of the full transcript, because that is
// what ResolveWordRemoval resolves against.
std::vector<Disfluency> FindDisfluenciesInClip(const DocumentClip& clip,
                                               const Transcript& transcript);

// The single explicit rounding rule this file uses to place a transcribed
// word boundary onto the exact frame grid of `frameRate` (PHILOSOPHY.md
// principle 4: conversions that cannot be exact are refused elsewhere, but a
// word boundary is inherently continuous input -- whisper.cpp's centisecond
// timestamp -- being intentionally snapped to a discrete cut grid, so the
// rule must be an explicit rounding, not a refusal). `roundUp` false rounds
// down to the frame containing or preceding `time`; true rounds up to the
// frame containing or following it. GenerateAudioTranscript applies this
// once, at generation time, rounding every word's start down and its end up
// before the transcript is ever cached -- so a cut removes the *whole* frame
// a word's audio touches, and every TranscriptWord read back by
// LoadAudioTranscript is already frame-exact with no further rounding left
// for a caller to get wrong or skip.
RationalTime RoundToSourceFrame(const RationalTime& time,
                                const MediaRate& frameRate, bool roundUp);

// Resolves word index ranges against `transcript` into the exact
// RemoveWordsOperation ApplyOperation can execute -- a straight lookup, not a
// conversion, since the transcript's words are already frame-exact. This is
// the deterministic computation PHILOSOPHY.md principle 7 asks for: the
// caller names *which words* (semantic, agent- or UI-sized); this function
// resolves *which frames* (exact, code-guaranteed) -- it is the only
// intended producer of WordRemovalRange. `wordRanges` must already be sorted
// by `start_word_index` and non-overlapping; `clip` only supplies clip_id,
// its own source bounds are re-validated by ApplyOperation, not trusted
// here. `transcript.media_id` must match `clip.source_id` -- the same
// mounted-source identity DocumentSource::id shares with LibraryMedia::id --
// or this is refused rather than resolving a mismatched clip's cut against
// another source's words.
bool ResolveWordRemoval(const DocumentClip& clip, const Transcript& transcript,
                        const std::vector<WordRange>& wordRanges,
                        const RationalTime& gapPadding,
                        const std::vector<Ulid>& syncTrackIds,
                        RemoveWordsOperation& operation, std::string& error);
