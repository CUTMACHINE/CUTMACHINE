#pragma once

#include "Document.h"
#include "Operations.h"
#include "Transcription.h"

#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// SRT-2026-08 -- SubRip is a surface format. The engine keeps its timestamps
// exact and turns one import into the existing reversible AddTrack operation.
struct SubtitleCue {
    RationalTime timeline_in{0, 1000};
    RationalTime duration{0, 1000};
    std::string text;
};

// The silence that ends one cue and starts the next. SubtitleCuesForClip
// breaks a cue when words are further apart than this, so two consecutive
// cues are separated either by nothing, or by a pause shorter than this that
// simply falls outside both of them. Exposed because InterviewShort.cc has
// to tell those two apart from a real editorial gap.
inline constexpr RationalTime kSubtitleCueMaximumGap{7, 10};

// SUBTITLE-GROUPING-2026-09 -- this is the centre of the readability cost,
// not a per-cue ceiling. Fast phrases may exceed it when keeping a syntactic
// unit together is the less disruptive choice; the global cost keeps the cue
// distribution centred around the target.
inline constexpr int32_t kSubtitleTargetCharactersPerSecond = 18;

bool ParseSrt(const std::string& contents, std::vector<SubtitleCue>& cues,
              std::string& error);
bool LoadSrt(const std::string& path, std::vector<SubtitleCue>& cues,
             std::string& error);
bool SaveSrt(const Document& document, const std::string& path,
             std::string& error);

// SRT-2026-08 -- the serializer SaveSrt is built on, taking cues directly.
// A caller that already holds them from somewhere other than a caption track
// -- the timeline's cached transcripts, say -- can write a file without first
// mutating the document to park them on a track it does not otherwise want.
// `roundedTimestamps`, when given, receives how many cue boundaries had to be
// rounded to SRT's millisecond (see SrtMilliseconds). Nothing is written to
// `path` unless every cue is.
bool WriteSrt(const std::vector<SubtitleCue>& cues, const std::string& path,
              std::string& error, size_t* roundedTimestamps = nullptr);

// FC7 -- docs/roadmap/FILMCRAFT-2026-10.md. Adapted from FilmCraft
// crates/captions/src/vtt.rs (MIT OR Apache-2.0, commit 09aaf9b). WebVTT
// beside SubRip: the same cues, the same millisecond and the same one
// rounding (SrtMilliseconds), counted in `roundedTimestamps` exactly as
// WriteSrt counts it. Written as `WEBVTT`, a blank line, then for each cue
// its number, `HH:MM:SS.mmm --> HH:MM:SS.mmm` and its text. The text is
// plain, as the document holds it: `&`, `<` and `>` are escaped, since WebVTT
// reads them as markup (and `-->` may not appear in a cue). A text holding
// an empty line is refused, since an empty line ends a WebVTT cue. Nothing
// is written to `path` unless every cue is.
bool FormatVtt(const std::vector<SubtitleCue>& cues, std::string& text,
               std::string& error, size_t* roundedTimestamps = nullptr);
bool WriteVtt(const std::vector<SubtitleCue>& cues, const std::string& path,
              std::string& error, size_t* roundedTimestamps = nullptr);

// SRT-MS-2026-09 -- SubRip counts in milliseconds, and a frame at 24 or
// 30000/1001 fps almost never lands on one: WriteSrt used to refuse every
// such cue, so no 24 fps timeline or delivery could be subtitled. This is the
// one place a subtitle time is rounded -- to the nearest millisecond, half
// up. The shift is at most 0.5 ms, under half a frame at any rate below
// 1000 fps, so the file read back at the source rate lands on the same
// frame. PHILOSOPHY.md principle 4 refuses a silent rounding that loses a
// frame; this one loses none and is not silent (`rounded`, WriteSrt's count).
// Throws std::invalid_argument on a negative time, which SRT cannot hold.
int64_t SrtMilliseconds(RationalTime time, bool* rounded = nullptr);

AddTrackOperation BuildSubtitleTrackEdit(const std::vector<SubtitleCue>& cues,
                                         int32_t trackIndex,
                                         const Ulid& trackId = {});

std::vector<const DocumentClip*> ActiveSubtitles(const Document& document,
                                                 RationalTime position);

// ITW4 -- ROADMAP.md. One word already placed on the timeline, in order.
struct SubtitleWord {
    std::string text;
    RationalTime start;
    RationalTime end;
};

struct SubtitleSegmentationLimits {
    // Safety rails, not the reading-speed target (see below).
    size_t maximum_characters = 42;
    RationalTime maximum_duration{5, 1};
    // Rendered width of words [begin, end) set on one line, in sequence
    // pixels. When given with a positive maximum_width, a cue of several
    // words never exceeds it: a carton is one line.
    std::function<int64_t(size_t begin, size_t end)> width;
    int64_t maximum_width = 0;
    // RM3 -- docs/roadmap/REGLES-2026-10.md. When given, one cost per
    // junction (words.size() - 1 of them): ending a cue after word i costs
    // (*boundary_costs)[i] in place of SubtitleRuleBoundaryCost. A learned
    // score changes the preferences this way and nothing else: the limits
    // above, sentence ends and long pauses still bound every cue.
    const std::vector<int64_t>* boundary_costs = nullptr;
    // RM3 -- when given, added to the cost of every candidate cue holding
    // words [begin, end) in `characters` characters, beside reading pace.
    std::function<int64_t(size_t begin, size_t end, size_t characters)>
        cue_cost;
};

// RM3 -- docs/roadmap/REGLES-2026-10.md. The hand-written cost of ending a
// cue between `left` and `right`, `gap` apart: what SegmentSubtitleWords
// minimises when no boundary_costs are given, and the base a learned score
// corrects (CaptionBreakModel.h).
int64_t SubtitleRuleBoundaryCost(const std::string& left,
                                 const std::string& right,
                                 const RationalTime& gap);

// RM3 -- the families of French words the rule reads, so that a learned
// score weighs the same evidence. A word may belong to several ("en" is a
// preposition and a clitic, "ce" a determiner and a pronoun).
enum SubtitleWordFamily : uint32_t {
    kSubtitleDeterminer = 1U << 0,
    kSubtitlePreposition = 1U << 1,
    kSubtitleSubjectPronoun = 1U << 2,
    kSubtitleAuxiliary = 1U << 3,
    kSubtitleNegation = 1U << 4,
    kSubtitleConjunction = 1U << 5,
    kSubtitleModifier = 1U << 6,
    kSubtitleClitic = 1U << 7,
    kSubtitleSubordinator = 1U << 8,
};
uint32_t SubtitleWordFamilies(const std::string& word);

// The word as the rule's families read it: trimmed, outer punctuation off
// (an apostrophe kept), ASCII lower case.
std::string SubtitleSyntaxWord(const std::string& text);

// RM3 -- a cue as SegmentSubtitleWords measures it: the characters of words
// [begin, end) set on one line, and the moment it leaves the screen (the
// next word when it follows within kSubtitleCueMaximumGap, else the cue's
// last word's end).
size_t SubtitleCueCharacters(const std::vector<SubtitleWord>& words,
                             size_t begin, size_t end);
RationalTime SubtitleCueDisplayEnd(const std::vector<SubtitleWord>& words,
                                   size_t end);

// The global segmentation SubtitleCuesForClip uses, over any ordered words:
// every candidate cue scored on reading pace and on the syntax and air at
// its outgoing edge, sentence ends and long pauses as hard boundaries.
// `ranges`, when given, receives each cue's words as [begin, end).
// Refused when `limits.boundary_costs` does not hold one cost per junction.
bool SegmentSubtitleWords(const std::vector<SubtitleWord>& words,
                          const SubtitleSegmentationLimits& limits,
                          std::vector<SubtitleCue>& cues,
                          std::vector<std::pair<size_t, size_t>>* ranges,
                          std::string& error);

// RM3 -- docs/roadmap/REGLES-2026-10.md. Cartons whose grouping the caller
// chose -- an agent reading the words, as the author preferred blind on 29 of 35
// passages of the PRJA series -- set on the same words with the
// segmentation's own times. `cartons` are texts whose words, in order, must
// be `words` exactly, case and typographic apostrophes aside; a word that
// differs, is missing or is added is refused by its carton and its index.
// The hard bounds still hold: no sentence end or long pause inside a carton,
// and a carton of several words within `limits`' characters and duration.
// The rendered width is not checked here (the caller measures it).
bool GroupSubtitleWords(const std::vector<SubtitleWord>& words,
                        const std::vector<std::string>& cartons,
                        const SubtitleSegmentationLimits& limits,
                        std::vector<SubtitleCue>& cues,
                        std::vector<std::pair<size_t, size_t>>* ranges,
                        std::string& error);

// French subtitle characters of `text`: UTF-8 code points, as the reading
// pace counts them.
size_t SubtitleCharacterCount(const std::string& text);

// RM2 -- docs/roadmap/REGLES-2026-10.md. A word that closes a sentence as
// the segmentation reads it -- a full stop, a question or an exclamation
// mark -- and so a hard boundary no cue crosses.
bool SubtitleEndsSentence(const std::string& text);

// RM2 -- whether a carton opening on the word after `previous` starts a
// sentence: at the film's start (`previous` empty) and after a sentence end,
// but not after an ellipsis, which breaks a cue without ending a thought.
bool SubtitleOpensSentenceAfter(const std::string& previous);

// RM2 -- `word` with its first letter in capitals, past an opening quote or
// bracket. A montage joins sentences said in different places: on the
// PRJA series, ten delivered cartons were fixed by hand only for the
// capital a word did not need where it was said. ASCII, Latin-1 letters
// (é, à, ç...) and œ; any other first letter is left as it is.
std::string SubtitleWithCapital(const std::string& word);

// F1.4 / SUBTITLE-GROUPING-2026-09 -- Word timestamps are source-relative;
// this deterministic bridge maps them through a clip, then globally groups
// them by temporal proximity and French syntax around the soft CPS target.
bool SubtitleCuesForClip(const Transcript& transcript, const DocumentClip& clip,
                         std::vector<SubtitleCue>& cues, std::string& error);

// WORD-ALIGNMENT-2026-09 -- a review track must expose every stored word
// boundary independently; grouping those words for reading would hide the
// exact boundary the editor needs to correct.
bool WordSubtitleCuesForClip(const Transcript& transcript,
                             const DocumentClip& clip,
                             std::vector<SubtitleCue>& cues,
                             std::string& error);
