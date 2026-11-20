#pragma once

// OR4 -- docs/roadmap/DONNEES-2026-10.md. A montage checked against a
// policy, by the engine, without editing it.
//
// WHY THIS FILE EXISTS. The checks a delivery needs -- picture and sound
// linked, in sync, a duration inside bounds, no black at the end, cartons
// that can be read -- lived in the Python of the LangGraph pilot (v3, commit
// 1f60686), outside the engine. When the pilot was removed on 6 October,
// neither MCP direct nor the Agent panel had them: each run re-derived them
// from timeline_stats by hand, and "aucun fondu au noir final" was a line of
// the brief that nothing measured. Here they are one call, the same for the
// command line and the MCP server, with a policy that has a name so a report
// says what it was judged against.
//
// WHAT IS JUDGED, AND ON WHAT.
//
//   links            the document. Picture and sound of one source that
//                    play over the same stretch of the timeline belong to
//                    one link group; a pair that does not is named, with
//                    the offset between them.
//   sync             the document, through CalculateTimelineStats: every
//                    linked pair reads its source at the same offset.
//   duration         the document, against the policy's bounds. Not run
//                    when the policy sets none.
//   final_black      the document, frame by frame over the end of the film,
//                    composed as the export composes it (from the highest
//                    visible video track down, the first that shows a
//                    layer wins): a frame with nothing on it is the
//                    composite's black, a black dip at full strength is
//                    black, a black dip on its way or a clip fading over
//                    nothing is a fade. A motion clip counts as a picture:
//                    the document does not see inside its composition, and
//                    the report counts those frames (motion_frames). With
//                    the delivered file, its pixels over the same frames
//                    too, with the defaults of FFmpeg's blackdetect, which
//                    is how the bench measured it -- the only judge of a
//                    composition that ends dark (teaser v7: its last two).
//   caption_contrast the delivered file only, by CheckCaptionLegibility.
//                    Not run without it.
//   charter          the document, against the brand's charter the policy
//                    names (DEC6 --
//                    docs/roadmap/MOTION-BIBLIOTHEQUE-2026-10.md, MB4): the
//                    font and the colours of every caption style a visible
//                    carton uses, and the fonts and the colours every motion
//                    clip on a visible track writes or binds -- its palette as
//                    the author wrote it, keyframes and gradient stops
//                    included, never the mixes an interpolation passes through.
//                    A value the charter does not allow is named with where it
//                    is. The document is what the file was exported from
//                    (verify_export proves the file equals it); the file's
//                    pixels are not read: an HLG delivery or a grade moves
//                    them. Not run when the policy names no kit.
//   jump_cuts        the document. A cut inside one rush -- the same
//                    source on both sides, not a through edit, the same
//                    still framing -- that the film shows as it is: on
//                    the frame before the cut and the frame at it, the
//                    composite is that track's two clips and nothing else
//                    (no cutaway over it, no dissolve, no dip, no clip at
//                    part opacity). A reframe on either side breaks it, an
//                    animated framing included. A motion clip is drawn over
//                    the cut, not instead of it: a title or a logo does
//                    not hide a jump. Always listed under `jump_cuts`;
//                    judged only when the policy forbids them, since a jump
//                    cut can be the author's choice (principle 12).
//
// A check that is not run says why; it is never counted as passed. The
// report passes when no check fails.
//
// Out of lot -- the author, 31 October 2026, on the bench against Descript: the
// interview run left 19 jump cuts bare in 57 s, and nothing the agent read
// counted them. review_cuts flags a jump it renders, one band at a time;
// this counts every one on the document, so a delivery can be judged on it.
//
// DEC36 -- docs/roadmap/GENERATIF-2026-10.md. Beside the checks, and judged
// by none of them, the report lists the clips that play a generated media
// (`generated`), so that the agent's report names each one from what the
// engine read rather than from what the agent remembers generating. A
// generated shot is no fault: the person asked for it. It is a line of the
// report, never a verdict.

#include "CaptionGeneration.h"
#include "Document.h"
#include "Json.h"
#include "MotionTemplates.h"
#include "RationalTime.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

enum class FinalBlackRule {
    // The brief of 6 October: no fade to black, no black at the end.
    Forbidden,
    Allowed,
};

enum class JumpCutRule {
    // Listed, judged by none: the default, since a film may want them.
    Allowed,
    // The interview's rule since 31 October 2026: every cut inside one rush
    // is covered or reframed, or the delivery says why it is not.
    Forbidden,
};

struct TimelinePolicy {
    // Said in the report. "default" is the policy below, unchanged.
    std::string name = "default";
    std::optional<RationalTime> duration_min;
    std::optional<RationalTime> duration_max;
    FinalBlackRule final_black = FinalBlackRule::Forbidden;
    JumpCutRule jump_cuts = JumpCutRule::Allowed;
    // The end of the film the final-black check reads. Absent: one second,
    // rounded up to whole frames of the sequence (25 at 25 i/s, 30 at
    // 30000/1001), the bench's "dernière seconde". Given: a whole number of
    // frames, or refused.
    std::optional<RationalTime> final_window;
    // Exact, as check_caption_legibility reads it: 3/1 by default.
    int32_t minimum_contrast_num = 3;
    int32_t minimum_contrast_den = 1;
    // DEC6 -- the brand's charter, by its name in the library (`kit`).
    // ParseTimelinePolicy reads the name only; the caller that knows where
    // the library is (the MCP server, the command line) looks it up into
    // `kit` before CheckTimeline, which refuses a name left unresolved.
    // The charter is a judge, like the rest of the policy: the document
    // keeps the values a style or a piece was given, never the charter
    // (principle 1: a project reopened elsewhere renders the same film).
    std::string kit_name;
    std::optional<MotionKitInfo> kit;
    // PA2, PA11 -- docs/roadmap/PARITE-2026-11.md. What a delivery is asked
    // for beyond its cut. The loudness of the delivered file, in whole LUFS:
    // judged on the file only, as a broadcast meter reads it, within one
    // loudness unit of the target (kLoudnessToleranceLu) and with its true
    // peak at or under `true_peak_max_dbtp`. Absent: not judged, and the
    // check is not listed.
    std::optional<int32_t> loudness_target_lufs;
    int32_t true_peak_max_dbtp = -1;
    // The frame's shape, "9:16": judged on the sequence. Empty: not judged,
    // and the check is not listed.
    std::string aspect_ratio;
};

// How far from its target a delivered loudness may sit: one loudness unit,
// EBU R 128's own tolerance for a programme measured after the fact.
inline constexpr double kLoudnessToleranceLu = 1.0;

// The policy an agent passes, as JSON: every key optional, unknown keys
// refused by name. Times are {"value","rate"}, the contrast {"num","den"},
// final_black "forbidden" or "allowed", kit the name of a charter of the
// library (DEC6). Without "name", a policy that changes anything is called
// "custom".
bool ParseTimelinePolicy(const mcp_json::Value& value, TimelinePolicy& policy,
                         std::string& error);

// What one frame at the end of the film shows.
enum class EndingFrame {
    Picture,
    // Nothing on it, or a black dip at full strength.
    Black,
    // On its way to black: a black dip not yet full, or the top clip's
    // opacity below one with nothing under it.
    Fading,
};

// Measured on the delivered file. Absent when no file was given.
struct TimelineFileMeasures {
    // The last frames of the file, oldest first, as many as the window
    // holds (fewer if the file is shorter): black or not.
    std::vector<bool> final_frames_black;
    std::vector<CaptionLegibility> legibility;
    // Why the cartons could not be measured (a process without the caption
    // renderer, for one); empty when they were.
    std::string legibility_error;
    // PA2 -- the file's loudness, read only when the policy sets a target.
    // Absent for a silent file; `loudness_error` says why it could not be
    // read at all.
    bool loudness_measured = false;
    std::optional<double> integrated_lufs;
    std::optional<double> true_peak_dbfs;
    std::string loudness_error;
};

enum class TimelineCheckStatus { Passed, Failed, NotRun };

struct TimelineCheckResult {
    // links, sync, duration, final_black, caption_contrast, charter,
    // jump_cuts; and, when the policy asks for them, loudness and format.
    std::string name;
    TimelineCheckStatus status = TimelineCheckStatus::Passed;
    // Why a check did not run, or what failed, in one sentence.
    std::string reason;
    // The findings and measures behind the verdict, as they are reported.
    mcp_json::Value evidence = mcp_json::Value::MakeObject();
};

// DEC36 -- a clip of the timeline whose media a generation service made
// (ClipPlaysGeneratedMedia), with what its provenance says.
struct TimelineGeneratedClip {
    Ulid clip_id;
    Ulid track_id;
    Ulid media_id;
    RationalTime timeline_in;
    RationalTime duration;
    MediaGeneration generation;
};

// Out of lot -- the author, 29 October 2026. A clip of the timeline taken out
// of the film by set_clip_enabled: on its track, neither seen nor heard
// (ClipEnabled.h). Listed beside the checks and judged by none of them, so
// that a delivery says which shots it leaves out rather than the agent
// remembering.
struct TimelineDisabledClip {
    Ulid clip_id;
    Ulid track_id;
    RationalTime timeline_in;
    RationalTime duration;
};

// A cut the film shows as a jump: see jump_cuts above.
struct TimelineJumpCut {
    // The frame of the timeline the incoming clip starts at, and that
    // instant exactly.
    int64_t frame = 0;
    RationalTime timeline_in;
    Ulid track_id;
    Ulid outgoing_clip_id;
    Ulid incoming_clip_id;
    Ulid source_id;
};

struct TimelineCheckReport {
    TimelinePolicy policy;
    Ulid timeline_id;
    RationalTime duration;
    bool file_measured = false;
    std::vector<TimelineCheckResult> checks;
    // DEC36 -- in the order of the tracks, then of the timeline.
    std::vector<TimelineGeneratedClip> generated;
    // In the order of the tracks, then of the timeline.
    std::vector<TimelineDisabledClip> disabled;
    // Ascending, whatever the policy says of them.
    std::vector<TimelineJumpCut> jump_cuts;

    bool Passed() const;
};

// How many frames the final-black check reads (see final_window).
bool FinalWindowFrames(const DocumentSequence& sequence,
                       const TimelinePolicy& policy, int64_t& frames,
                       std::string& error);

// The frames [first, first + count) of the film, as the export composes
// them. Exposed for its tests: the check reads it over the final window.
bool ClassifyEndingFrames(const Document& document, int64_t first,
                          int64_t count, std::vector<EndingFrame>& frames,
                          std::string& error);

// The verdict. `file` is null when no delivered file was measured.
bool CheckTimeline(const Document& document, const TimelinePolicy& policy,
                   const TimelineFileMeasures* file,
                   TimelineCheckReport& report, std::string& error);

// Reads the delivered file: its last frames (FFmpeg, decoded small and in
// grey) and the legibility of its cartons. `ffmpegPath` empty: the binary
// FfmpegBinary.h resolves.
bool MeasureDeliveredFile(const Document& document,
                          const TimelinePolicy& policy, const std::string& path,
                          const std::string& ffmpegPath,
                          TimelineFileMeasures& measures, std::string& error);

// A frame of grey pixels is black for FFmpeg's blackdetect, at its
// defaults: 98 % of its pixels at or under a tenth of full scale.
bool GreyFrameIsBlack(const uint8_t* pixels, size_t count);

// Canonical JSON, shared by the command line and the MCP server.
std::string SerializeTimelineCheck(const TimelineCheckReport& report);
