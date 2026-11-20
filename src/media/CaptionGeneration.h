#pragma once

#include "Document.h"
#include "InterviewShort.h"
#include "Operations.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ITW4 -- ROADMAP.md. Cartons built by the engine from the words of the
// montage, with the rules that used to live only in the interview skill.
//
// On the ITW-2026-09 interview the agent rewrote them by hand: one syntactic
// unit per carton, about 18 characters per second, a single line no wider
// than 650 px on a 1080-wide delivery, sentence ends as hard breaks, and
// carton edges moved onto a picture cut when one was less than 12 frames
// away. The first three are what SegmentSubtitleWords already optimises; the
// width and the cuts are added here.
//
// Width is measured by the renderer that delivers the file (LG2,
// docs/roadmap/LEGENDES-2026-10.md: Core Text through CaptionRenderer.h, no
// longer FFmpeg's drawtext), with the style's font, size and outline.
// Candidate cartons are estimated from measured words during segmentation;
// the cartons kept are measured again whole, and that is the width
// published.

struct CaptionBreakModel;

// DEC4 -- docs/roadmap/REGLES-2026-10.md. A burned-in carton aims at one line
// of about 20 characters, the style the deliveries measure: a median of 13 to
// 21 characters per carton in the 31 one-line French files of September, 16
// in the author's own Resolve tracks. The former default, 42 characters on 60 %
// of the width, gave 35 long cartons where 64 were delivered. A subtitle file
// (--export-srt, --export-vtt) keeps SubtitleSegmentationLimits' 42.
inline constexpr size_t kCaptionCartonMaximumCharacters = 20;

// A landscape frame holds a wider line: films delivered in 16:9 were
// subtitled at 32 characters, on one line, where vertical ones kept 20 per
// carton (decided by the author on 4 November 2026). A square or 4:5 frame has
// no measured delivery yet, and keeps the vertical 20.
inline constexpr size_t kCaptionCartonMaximumCharactersLandscape = 32;

// The default of generate_captions' max_characters for a timeline of this
// size: kCaptionCartonMaximumCharactersLandscape when it is wider than tall,
// kCaptionCartonMaximumCharacters otherwise.
inline constexpr size_t DefaultCaptionCartonMaximumCharacters(int64_t width,
                                                              int64_t height) {
    return width > height ? kCaptionCartonMaximumCharactersLandscape
                          : kCaptionCartonMaximumCharacters;
}

struct CaptionGenerationSettings {
    size_t maximum_characters = kCaptionCartonMaximumCharacters;
    // Pixels at the sequence resolution. Zero means 60 % of the sequence
    // width -- 648 px on a 1080-wide vertical frame.
    int64_t maximum_line_width = 0;
    // A carton edge closer than this to a visible picture cut moves onto it,
    // unless the move would uncover one of the carton's words (DEC18).
    int64_t snap_frames = 12;
    // RM3 -- docs/roadmap/REGLES-2026-10.md. When given, where a carton ends
    // is ranked by this learned model inside the rule's bounds; when null,
    // by the rule alone (CaptionBreakModel.h).
    const CaptionBreakModel* break_model = nullptr;
    // RM3 -- when given, the cartons as the caller grouped the montage's
    // words, in order (GroupSubtitleWords); neither the segmentation nor the
    // model chooses, and a short carton is never joined to a neighbour. The
    // engine still sets their times, measures them and snaps them to cuts.
    std::vector<std::string> cartons;
};

struct GeneratedCarton {
    std::string text;
    RationalTime timeline_in;
    RationalTime duration;
    size_t characters = 0;
    // Pixels at the sequence resolution, outline included.
    int64_t rendered_width = 0;
    // Frames from each edge to the nearest visible picture cut, after
    // snapping; -1 when the timeline has no cut. A cut within snap_frames
    // that the edge did not take would have uncovered a word (DEC18), cost
    // the carton its readable length, or overlapped a neighbour.
    int64_t start_cut_distance = -1;
    int64_t end_cut_distance = -1;
    bool start_snapped = false;
    bool end_snapped = false;
    // PA4 -- docs/roadmap/PARITE-2026-11.md. When each word of `text`
    // starts, counted from `timeline_in`, in lowest terms
    // (DocumentClip::caption_word_times). Empty when the text does not
    // count as many words as the montage gave it -- a carton the caller
    // regrouped and rewrote -- and the carton is then drawn whole.
    std::vector<RationalTime> word_times;
};

// The font a caption style names, as the monitor and the export both ask
// for it. "system" was the system font in Record and fontconfig's "Sans" in
// the file -- Verdana on the Mac of the agentic QA of 23 September 2026, and
// whatever else fontconfig picks on another machine, so the delivered font
// depended on who exported. It is Helvetica Neue on both sides: installed on
// every Mac, one regular face (the system font is a variable font whose
// instance fontconfig chooses unpredictably).
std::string CaptionFontName(const CaptionStyle& style);

// LG1 -- docs/roadmap/LEGENDES-2026-10.md. A caption colour read as the
// export has always read it, FFmpeg's colour syntax ("#rrggbb", "white", an
// "@0.6" alpha): straight RGBA. False when it is not one.
bool ParseCaptionColor(const std::string& text, std::array<uint8_t, 4>& rgba);

// Rendered width of each text, in sequence pixels: its widest line's advance
// at the style's size, the outline on both sides and the shadow included
// (caption_layout::MeasuredWidth). Measured by the caption renderer, which
// must be installed (CaptionRenderer.h).
bool MeasureCaptionTextWidths(const CaptionStyle& style,
                              const DocumentSequence& sequence,
                              const std::vector<std::string>& texts,
                              std::vector<int64_t>& widths, std::string& error);

// Visual QA of 19 September 2026 -- the export never breaks a line: a carton
// imported from SRT with a long line ran off both sides of a 9:16 frame,
// while the monitor's text field wrapped it. The monitor and every delivery
// now share one limit, nine tenths of the sequence width, and the text the
// file is drawn with is broken to it.
int64_t CaptionMaximumLineWidth(int32_t sequenceWidth);

// Each line of `text` wider than `maximumWidth` broken at spaces, greedily:
// as many words per line as fit. `widthOf` is a line's rendered width. A
// word wider than the limit on its own keeps a line to itself; lines that
// fit, and the line breaks already there, are kept.
std::string WrapCaptionLines(
    const std::string& text, int64_t maximumWidth,
    const std::function<int64_t(const std::string&)>& widthOf);

// The texts as delivered, each in its own style: every line wider than
// CaptionMaximumLineWidth broken with WrapCaptionLines, widths measured by
// MeasureCaptionTextWidths. The export, the Record monitor and
// check_caption_legibility draw these same texts.
bool WrapCaptionTextsForDelivery(const std::vector<CaptionStyle>& styles,
                                 const DocumentSequence& sequence,
                                 std::vector<std::string>& texts,
                                 std::string& error);

// Timeline frames where the picture on top changes: another source, or a
// jump inside the same one. A clip split without removal is not a cut.
std::vector<int64_t> VisibleCutFrames(const Document& document);

// `words` are the montage's words in timeline order, one span per word
// (BuildTimelineTranscriptWordSpans).
bool GenerateCaptionCartons(const Document& document, const CaptionStyle& style,
                            const std::vector<TimelineTranscriptSpan>& words,
                            const CaptionGenerationSettings& settings,
                            std::vector<GeneratedCarton>& cartons,
                            std::string& error);

// One reversible edit: a sync-locked caption track holding the cartons in
// `styleId`'s run, so later cuts carry it along.
AddTrackOperation BuildCaptionTrackEdit(
    const std::vector<GeneratedCarton>& cartons, const Ulid& styleId,
    int32_t trackIndex);

std::string SerializeGeneratedCartons(
    const std::vector<GeneratedCarton>& cartons);

// PA5 -- docs/roadmap/PARITE-2026-11.md. A caption track in another
// language. Translating is a model's work and timing is not: the caller
// gives one text per carton of an existing caption track, and the engine
// lays them on a new track at the same times and in the same styles, in one
// reversible edit, then says what the new texts cost to read -- their
// length, their rate, their width as the delivery draws them. It measures
// and does not judge: a translation that runs long is the author's to
// shorten (principles 7 and 12).
//
// The new track is hidden unless asked otherwise. An export burns every
// visible caption track into the picture, so two languages left visible
// would be delivered one over the other; a version is delivered by showing
// its track and hiding the others (set_track_output).
struct CaptionTranslationText {
    // The carton of the source track this text translates.
    Ulid clip_id;
    std::string text;
};

struct TranslatedCarton {
    Ulid source_clip_id;
    // The new track's clip.
    Ulid clip_id;
    std::string text;
    RationalTime timeline_in;
    RationalTime duration;
    size_t characters = 0;
    // Pixels at the sequence resolution, outline included; -1 when the
    // caption renderer could not measure (CaptionTranslation::width_error).
    int64_t rendered_width = -1;
    // Wider than the delivery's line (CaptionMaximumLineWidth): the file
    // will draw it on more lines than the text was written on.
    bool wraps = false;
};

struct CaptionTranslation {
    AddTrackOperation operation;
    std::vector<TranslatedCarton> cartons;
    // Empty when every width was measured; otherwise why none was (a
    // process without the caption renderer).
    std::string width_error;
    int64_t maximum_line_width = 0;
};

// The cartons of track `trackId` given their `texts`, on a new caption
// track at `trackIndex`. Refuses, by name: a track that is not a caption
// track or holds no carton, a text for a clip the track does not hold, two
// texts for one carton, an empty text, and any carton left without one
// (the first five are named, and how many more).
bool BuildCaptionTranslation(const Document& document, const Ulid& trackId,
                             const std::vector<CaptionTranslationText>& texts,
                             int32_t trackIndex, bool visible,
                             CaptionTranslation& translation,
                             std::string& error);

// The cartons as translate_captions answers them: ids, times, characters,
// characters per second to a tenth, width and whether it wraps.
std::string SerializeTranslatedCartons(
    const std::vector<TranslatedCarton>& cartons);

// ITW4 -- legibility measured on the delivered file. For each carton, on the
// frame at its middle: the letters are located by drawing the carton alone
// with the export's own geometry, and the text colour is compared with the
// ring of pixels just around them -- the outline where there is one, the
// picture where there is not -- as a WCAG contrast ratio. On the ITW-2026-09
// interview, white cartons over a white shirt measured a median 2.1 to 2.8:1.
//
// PA4 -- docs/roadmap/PARITE-2026-11.md. A carton whose words are timed is
// read where every word is on screen: under "reveal", from the frame its
// last word starts on (the middle, if later), since around a word not yet
// drawn the ring is the bare picture, never the outline the word brings.
// Under "highlight", the lit word passes along the whole line: its colour
// is read against the same ring, as a second ratio.
struct CaptionLegibility {
    Ulid clip_id;
    std::string text;
    int64_t frame = 0;
    int64_t ring_pixels = 0;
    double contrast_median = 0.0;
    // The tenth percentile: the worst stretch of the carton.
    double contrast_low = 0.0;
    // PA4 -- the highlight colour against the ring, for a timed carton
    // under "highlight" only.
    bool highlighted = false;
    double highlight_contrast_median = 0.0;
    double highlight_contrast_low = 0.0;
};

bool CheckCaptionLegibility(const Document& document,
                            const std::string& exportPath,
                            const std::string& ffmpegPath,
                            std::vector<CaptionLegibility>& results,
                            std::string& error);

// A carton under the minimum: its median, its lit word's median, or no ring
// of pixels to read. OR4 -- check_timeline judges cartons by this same rule.
bool CaptionLegibilityBelowMinimum(const CaptionLegibility& result,
                                   double minimumContrast);

std::string SerializeCaptionLegibility(
    const std::vector<CaptionLegibility>& results, double minimumContrast);
