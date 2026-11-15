#pragma once

#include "Json.h"
#include "Keyframes.h"
#include "RationalTime.h"
#include "Ulid.h"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct MediaRate {
    int32_t num = 0;
    int32_t den = 1;
};

// Project-wide display pipeline. Values are deliberately named (rather than
// opaque numeric IDs) so project files remain readable and extensible.
struct ColorManagementSettings {
    bool enabled = false;
    std::string input_gamut = "rec709";
    std::string input_transfer = "rec709";
    std::string input_ycbcr_matrix = "auto";
    std::string input_range = "auto";
    std::string working_gamut = "acescct";
    std::string output_gamut = "rec709";
    std::string output_transfer = "rec709";
};

// QC-2026-09 A3 -- audio levels carried as integers: an RMS amplitude as a
// fraction of full scale, times this scale. Amplitude rather than decibels,
// and integer rather than float, for the reason SpeechOnset.h's identical
// convention gives -- a logarithm cannot be taken exactly in integers, and
// no float literal may reach a canonical document. -60 dBFS is 1000 here,
// -74 dBFS (a silent cutaway, measured) is 200, ordinary speech is tens of
// thousands.
constexpr int64_t kAudioLevelScale = 1000000;

// Below this, a media carries no speech worth transcribing: it is a mute
// cutaway, and Whisper on it costs eleven times its own runtime to produce
// nothing. -60 dBFS, which sits far above the -74 dBFS the silent rushes of
// PRJA_ITW measured and far below any usable dialogue.
constexpr int64_t kSilentMediaAudioLevel = 1000;

struct DocumentSource {
    Ulid id = GenerateUlid();
    std::string path;
    MediaRate rate;
    RationalTime duration;
};

// G7 -- the white balances a media may record, in Kelvin: wider than any
// camera's dial, narrow enough to refuse a number that is not one.
inline constexpr int32_t kMinimumMediaWhiteBalance = 1000;
inline constexpr int32_t kMaximumMediaWhiteBalance = 50000;

// G3 -- docs/roadmap/GRADE-2026-09.md. The colour input one media is read
// as, in place of the project's: the names ColorManagementSettings uses for
// its own input, and the same four together, so that a Rec.709 phone clip in
// a project conformed to HLG does not inherit the project's BT.2020 matrix.
// An empty `transfer` is no declaration: the media follows the project.
// Declared at ingest from what the file says (the camera's rtmd track, or
// the container's colour tags), or set with set_media_color_input.
struct MediaColorInput {
    std::string gamut;
    std::string transfer;
    std::string ycbcr_matrix = "auto";
    std::string range = "auto";
    // DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. The media is a
    // graphic -- a logo, a packshot, a still -- whose code values are
    // display values, as a motion title's colours are: it is placed by the
    // rule of OpenColorIoCompositeDisplayReferred (ColorManagement.h), in
    // the graphics view, rather than read through a camera's input
    // transform. A white logo read as a Rec.709 camera came out at 0.65 of
    // an HLG signal on PRJH's film. Declared with set_media_color_input,
    // never guessed at ingest. Exclusive with `gamut` and `transfer`;
    // `ycbcr_matrix` and `range` still say how a YCbCr graphic's codes
    // read. Without colour management it changes nothing but the BT.709
    // matrix an RGB graphic is converted with.
    bool graphics = false;
};

inline bool DeclaresColorInput(const MediaColorInput& input) {
    return !input.transfer.empty();
}

inline bool operator==(const MediaColorInput& left,
                       const MediaColorInput& right) {
    return left.gamut == right.gamut && left.transfer == right.transfer &&
           left.ycbcr_matrix == right.ycbcr_matrix &&
           left.range == right.range && left.graphics == right.graphics;
}

inline bool operator!=(const MediaColorInput& left,
                       const MediaColorInput& right) {
    return !(left == right);
}

// The input names a document accepts, and the gamut/transfer pairs the
// pinned OpenColorIO config can read: one rule for the project's input and
// for a media's.
bool ValidateColorInputNames(const std::string& gamut,
                             const std::string& transfer,
                             const std::string& ycbcrMatrix,
                             const std::string& range, std::string& error);
bool IsSupportedColorInput(const std::string& gamut,
                           const std::string& transfer);

// The project's settings with the media's input in place of its own, when
// the media declares one. DEC31 -- for a graphic, settings that manage no
// colour (its code values are what a picture shows) and read its YCbCr
// with its own matrix and range: what every preview of it -- read_frame,
// the sheets, the measure -- decodes it with. The export and the monitor
// place it in the graphics view instead (IsGraphicsMedia).
ColorManagementSettings WithMediaColorInput(
    const ColorManagementSettings& project, const MediaColorInput& input);

// PL1 -- docs/roadmap/PLANS-2026-10.md. What one stretch of a rush shows,
// described once and kept: the picture's counterpart of the transcript. An
// agent used to look at contact sheets again at every edit to find a
// cutaway, pay for that perception each time, and lose it afterwards.
//
// A document fact, not a cache beside the media, although every other
// analysis of a rush (transcript, shot quality, waveform, proxy) is a cache:
// those are deterministic measurements that a new run gives back to the
// byte, whereas a description is written -- by a model at a price, or by a
// person -- and cannot be regenerated for free, nor identically. It is also
// a judgement someone signs (PHILOSOPHY.md principle 10), which is why it
// says who wrote it and when. So it lives in the document (principle 1) and
// changes only through an operation (principle 2): SetMediaShotsOperation.
//
// Closed vocabularies for what a search filters on, free text for what it
// reads. The spellings are the file's, the journal's and the tools' alike
// (ShotDescriptions.h).
enum class ShotSize {
    ExtremeWide,
    Wide,
    Full,
    Medium,
    MediumClose,
    Close,
    ExtremeClose,
    Insert,
};

enum class CameraMovement {
    Static,
    Handheld,
    Pan,
    Tilt,
    ZoomIn,
    ZoomOut,
    PushIn,
    PullOut,
    Tracking,
    Crane,
    Aerial,
};

// Good: nothing to say. Usable and Poor say why in `reserve`.
enum class ShotQuality {
    Good,
    Usable,
    Poor,
};

enum class ShotAuthorKind {
    Model,
    Person,
};

struct ShotDescription {
    // Supplied by the caller, like a keyframe's: a replayed operation has to
    // name the same description.
    Ulid id;
    // The stretch described, in the media's own time, as a clip's source_in
    // and duration are.
    RationalTime source_in;
    RationalTime duration;
    ShotSize size = ShotSize::Medium;
    std::vector<std::string> subjects;
    std::string action;
    std::string place;
    CameraMovement movement = CameraMovement::Static;
    ShotQuality quality = ShotQuality::Good;
    std::string reserve;
    std::vector<std::string> keywords;
    ShotAuthorKind author_kind = ShotAuthorKind::Model;
    // The model's name, or the person's.
    std::string author;
    // UTC, "YYYY-MM-DDTHH:MM:SSZ".
    std::string described_at;
};
// GN3 -- docs/roadmap/GENERATIF-2026-10.md. Where a media came from when a
// paid generation service made it rather than a camera: the service, the
// model, what it was asked, the seed it answered with, when, and the cost it
// was announced at. Written by generate_media through SetMediaGeneration, in
// the document and not in a cache: the mark "this picture was generated"
// must travel with the project, survive a copy to another machine, and be
// read without the service (PHILOSOPHY.md, principe 1). The file itself is
// in the package, so the document replays without the service.
//
// Every member is text, the seed included: what the service returned is
// recorded as it said it, never converted. An empty `service` is no
// provenance at all -- a filmed media -- and the struct is then default and
// written nowhere.
struct MediaGeneration {
    std::string service;
    // The service's own name for the model (its endpoint id on fal.ai).
    std::string model;
    // The engine's name for what was asked: "expand_image" for now.
    std::string kind;
    std::string prompt;
    std::string seed;
    // UTC, ISO 8601 to the second ("2026-10-21T15:04:05Z").
    std::string created_at;
    // As the proposal announced it ("0.04 USD"), empty when unknown. Never
    // a price the engine worked out after the fact.
    std::string announced_cost;
    // The service's request id, for its own invoice and history.
    std::string request_id;
    // The library media the generation started from; empty when none.
    Ulid source_media_id;
    // GN6b -- the words a generated voice says (`kind` "voice_design"): the
    // reference phrase design_voice had VoiceDesign say, which the Base
    // model needs word for word to clone that voice (VoiceOver.h). Empty for
    // every other generation. Written only when present, so a provenance
    // made before it existed keeps its bytes.
    std::string reference_text;
};

inline bool operator==(const MediaGeneration& left,
                       const MediaGeneration& right) {
    return left.service == right.service && left.model == right.model &&
           left.kind == right.kind && left.prompt == right.prompt &&
           left.seed == right.seed && left.created_at == right.created_at &&
           left.announced_cost == right.announced_cost &&
           left.request_id == right.request_id &&
           left.source_media_id == right.source_media_id &&
           left.reference_text == right.reference_text;
}

inline bool operator!=(const MediaGeneration& left,
                       const MediaGeneration& right) {
    return !(left == right);
}

inline bool IsGenerated(const MediaGeneration& generation) {
    return !generation.service.empty();
}

struct LibraryMedia {
    Ulid id = GenerateUlid();
    std::string path;
    std::string filename;
    std::string codec;
    // B11 -- ROADMAP.md. Audio-only media remains first-class library media;
    // consumers that decode pictures must check this capability explicitly.
    // True by default keeps pre-B11 documents and hand-built video fixtures
    // source-compatible when their serialized entry has no has_video field.
    bool has_video = true;
    int32_t width = 0;
    int32_t height = 0;
    std::string pixel_format;
    std::string color_range;
    std::string color_space;
    std::string color_transfer;
    std::string color_primaries;
    // G7 -- docs/roadmap/GRADE-2026-09.md. The white balance the camera
    // recorded the picture at, in Kelvin: the neutral of color.temperature
    // for every clip of this media. Read at ingest from Sony's timed
    // metadata (the rtmd track, tag 0x810E), or set with
    // set_media_white_balance; 0 when unknown, where 6500 K stands in.
    int32_t white_balance_kelvin = 0;
    // G3 -- the colour input this media is read as (MediaColorInput above).
    MediaColorInput color_input;
    // Counterclockwise display rotation reported by FFmpeg's display matrix.
    int32_t rotation_degrees = 0;
    MediaRate rate;
    RationalTime duration;
    std::string orientation;
    bool has_audio = false;
    int32_t audio_rate = 0;
    int32_t audio_channels = 0;
    // QC-2026-09 A3 -- mean level of the whole audio stream, in the scale
    // above. A document fact like the frame rate and the duration, and
    // recorded for the same reason: it is a property of the file that every
    // surface needs and nobody should have to measure twice. Measured at
    // ingest, which is why it is here and not in a cache -- a cache entry
    // would be one more thing to be missing at the moment a decision depends
    // on it.
    //
    // `audio_level_measured` distinguishes "not measured" from "digital
    // silence", which are the same zero and are not the same fact: one is an
    // unknown a caller can act on, the other is an answer.
    bool audio_level_measured = false;
    int64_t audio_level = 0;
    // Empty means the project root. Bins organize library media only and do
    // not change source or clip identity.
    Ulid bin_id;
    // Derived video-only media used for interactive decoding. Export and
    // audio always resolve the original path above.
    std::string proxy_path;

    // Version-1 sources do not contain technical metadata. They are promoted
    // to the library on load without fabricating values; a later ingest of the
    // same path replaces the incomplete entry with probed metadata.
    bool metadata_complete = true;
    // PL1 -- docs/roadmap/PLANS-2026-10.md. The described stretches of this
    // media, in increasing source_in, none overlapping another.
    std::vector<ShotDescription> shots;

    // GN3 -- docs/roadmap/GENERATIF-2026-10.md. Set when a generation
    // service made this media (MediaGeneration above); default otherwise.
    // Last (after shots), so that an entry spelled member by member keeps
    // its meaning.
    MediaGeneration generation;
};

// COLOR-T -- how the project reads a media's picture is the document's,
// not the file's: the as-shot white balance (the neutral of every
// color.temperature on its clips, ColorEffects.h) and the declared colour
// input (G3). A grade is set against both. They are read from the file once,
// when the media enters the library, by the one probe every import shares
// (ProbeMediaMetadata: the window, --ingest and ingest_media,
// --import-resolve), so that one rush comes in alike whichever way it comes
// in. Afterwards they change only on request: set_media_white_balance
// (from_camera reads the file again), set_media_color_input (from_file
// likewise). This copies them from `kept`, the entry the document holds,
// into `probed`, a fresh probe of a file about to replace it: a relink
// (either of the window's two), an ingest completing a version-1 entry
// (AddProbedMedia). Everything the file says about itself -- size, format,
// tags, duration -- stays the probe's. On 4 October 2026 the window's relink
// in bulk took both from the probe: the 71 rushes of a project ingested in
// September, before either was read, got 4000 K and S-Log3, and every
// color.temperature an agent had set against the 6500 K stand-in turned the
// picture yellow, in Record and in the next export alike, without a word.
void KeepMediaReading(const LibraryMedia& kept, LibraryMedia& probed);

struct DocumentBin {
    Ulid id = GenerateUlid();
    std::string name;
    // Empty means a top-level bin. Bins form a project-local hierarchy.
    Ulid parent_id;
};

// A project marker is addressable independently from clips and tracks. Its
// position remains exact in the document time domain; color and category stay
// textual so project files remain readable and extensible.
//
// D6 -- docs/roadmap/DIALOGUE-2026-10.md. `duration` makes a marker a span:
// "from here to there" is what an editor says of a passage, and a marker
// with only a time could not say it. Zero, the default, is a point, and is
// what every marker was before this ticket: it is not written, so their
// files and journals keep their bytes.
struct DocumentMarker {
    Ulid id = GenerateUlid();
    std::string name;
    RationalTime time;
    std::string color = "#f5c542";
    std::string category = "note";
    RationalTime duration{0, 1};
};

// An exact fraction for a continuous effect knob (exposure amount, mix,
// etc.). Values stay num/den pairs for the same reason RationalTime never
// carries a float: the canonical JSON parser rejects floating-point number
// literals outright, and a knob dragged in the UI must still read back the
// same bytes it was saved with. F1.3 is the single explicit boundary where
// this becomes a float for the Metal kernel that consumes it.
struct EffectParamValue {
    int32_t num = 0;
    int32_t den = 1;
};

// One stage in a clip's ordered color-grade stack. `type` is a dotted knob
// family ("color.exposure", "color.contrast", "color.saturation", ...);
// `params` holds its numeric knobs. Restricted to the "color.*" family for
// now (see ROADMAP.md F0.1); F1.3 interprets these into Metal filters.
struct ClipEffect {
    Ulid id = GenerateUlid();
    std::string type;
    std::map<std::string, EffectParamValue> params;
};

// R1 -- docs/roadmap/REEL-2026-09.md. How a clip's picture sits in the
// sequence frame. Everything here is an exact fraction, for the reason
// EffectParamValue exists at all: a canonical document carries no
// floating-point literal, and the single step to pixels belongs to the
// render (ClipFraming.h holds it, and holds the rule these fields feed).
//
// The order the fields are applied in is fixed and written once, in
// ClipFraming.h: the source's display rotation, the crop, the mode, the
// scale, the position.
enum class FramingMode {
    // The whole picture fits inside the frame; where the two aspects
    // disagree, bars. What every clip did before this ticket and what a
    // clip that sets no framing still does, byte for byte.
    Fit,
    // The frame is covered and what overruns it is cut: a 16:9 rush in a
    // 1080x1920 sequence comes out full height with its sides gone, which
    // is the whole point of R1.
    Fill,
};

struct ClipFraming {
    FramingMode mode = FramingMode::Fit;
    // Multiplies the scale the mode gives, so 1/1 is "exactly fitted" or
    // "exactly filled" and the numbers a framing carries stay readable
    // whatever the source's size: the film measured used 11/10 to 29/20.
    // Below 1/1 is allowed and shrinks the picture inside the frame --
    // where `fill` then stops covering it, and the engine says so rather
    // than the delivery showing it.
    EffectParamValue scale{1, 1};
    // The centre of the picture, as a fraction of the sequence frame away
    // from the frame's own centre. Positive x goes right, positive y goes
    // down -- the top-left origin every consumer of this geometry already
    // uses (FFmpeg's overlay and pad, the sheets, the monitor's layout).
    EffectParamValue position_x{0, 1};
    EffectParamValue position_y{0, 1};
    // What is thrown away of each edge of the source, as a fraction of the
    // displayed source. Each in [0, 1), and the two of an axis below 1:
    // a crop that leaves no picture is refused, not clamped.
    EffectParamValue crop_left{0, 1};
    EffectParamValue crop_right{0, 1};
    EffectParamValue crop_top{0, 1};
    EffectParamValue crop_bottom{0, 1};
};

// R2 -- docs/roadmap/REEL-2026-09.md. A clip parameter that keyframes may
// drive. Named rather than free-form, because each name has to reach a
// field of this file and a consumer that knows what to do with it; a knob
// nothing renders is a setting that lies. The two versions of the film
// measured on 20 September asked for exactly these: 20 then 13 shots of
// animated framing -- a zoom on a beat, a slow push, a three-frame shake
// -- plus the flashes, which are a flat at a falling opacity.
//
// Audio gain and the `color.*` stack are deliberately absent: R0 has not
// asked for them, and a parameter enters this list with the consumer that
// renders it, never before.
enum class ClipAnimatedParameter {
    // DocumentClip::opacity.
    Opacity,
    // ClipFraming::scale, position_x, position_y. The crops are NOT here:
    // see ClipAnimation.h for the measurement that keeps them out.
    FramingScale,
    FramingPositionX,
    FramingPositionY,
};

// R2 -- one key of one parameter. The shape is Keyframes.h's Keyframe, with
// one deliberate difference: the value and the easing control points are
// exact fractions here, not doubles.
//
// Why. Keyframes.h evaluates in double, and that is right -- an eased
// value between two keys is not a rational number of anything. But a key is
// not an evaluation: it is what the author wrote, and a canonical document
// carries no floating-point literal (principle 1; the document parser
// refuses one outright, DocumentFormat.cc). "100 % to 115 %" is 1/1 to
// 23/20, exactly, and it has to read back as the same bytes on any machine.
// The doubles appear inside the evaluation and die at the sampled value,
// which ClipAnimation.h turns back into an exact fraction before R1's
// geometry ever sees it.
struct ClipKeyframe {
    // Supplied by the caller, like Keyframes.h's: a serialized operation
    // that replays has to name the same key.
    Ulid id;
    // Local time in the clip: zero is the clip's first frame, whatever the
    // clip's position on the timeline and whatever part of its media it
    // reads. What happens to it under a trim, a move and a cut is the rule
    // written in ClipAnimation.h.
    RationalTime time;
    EffectParamValue value{0, 1};
    // How the value travels from this key to the next one.
    KeyframeInterpolation interpolation = KeyframeInterpolation::Linear;
    // CSS cubic-bezier control points, read only for CubicBezier. x1 and x2
    // stay in [0, 1] so the curve remains a function of time.
    EffectParamValue easing_x1{0, 1};
    EffectParamValue easing_y1{0, 1};
    EffectParamValue easing_x2{1, 1};
    EffectParamValue easing_y2{1, 1};
};

// R2 -- every key of one parameter of one clip, in increasing time.
struct ClipParameterAnimation {
    ClipAnimatedParameter parameter = ClipAnimatedParameter::Opacity;
    std::vector<ClipKeyframe> keyframes;
};

// PA8 -- docs/roadmap/PARITE-2026-11.md. A shape that cuts a clip's picture
// out: what is inside is kept, what is outside lets the tracks below show
// (or the reverse, `inverted`). A split screen, a picture in a circle, a
// soft-edged window on a face.
//
// The shape is placed on the clip's PICTURE, not on the sequence frame:
// centre, width and height are fractions of the displayed source (rotation
// applied, crops ignored), so the mask stays on what it masks when the
// framing moves or zooms, as a window of Resolve's Color page stays on its
// clip. An ellipse is the one inscribed in that rectangle.
//
// `feather` softens the edge inwards: it is the fraction of the way from
// the edge to the centre over which the picture fades in, 0 a hard edge, 1
// a fade from the edge all the way to the centre. Inwards, so that the
// shape's size is where the picture ends, whatever the softness.
//
// A mask is one more coverage on the path the opacity and the fades
// already take: a straight alpha the picture's colours never see, mixed
// with what is below exactly as an opacity is (in linear light under colour
// management, in the delivered signal without). Every renderer evaluates
// the one rule of ClipMask.h.
//
// PA15 -- a third shape, "person": the people the picture shows, cut out
// frame by frame (the roto). The coverage is not a rule but a matte, one
// grey picture per frame of the media, computed once by Vision's person
// segmentation and kept in the package (PersonMatte.h). It has no centre,
// size or feather of its own -- those stay at their defaults -- and
// `inverted` keeps everything but the people.
struct ClipMask {
    // "" for none, "rectangle", "ellipse" or "person".
    std::string shape;
    EffectParamValue center_x{1, 2};
    EffectParamValue center_y{1, 2};
    EffectParamValue width{1, 1};
    EffectParamValue height{1, 1};
    EffectParamValue feather{0, 1};
    bool inverted = false;
};

inline constexpr const char* kClipMaskRectangle = "rectangle";
inline constexpr const char* kClipMaskEllipse = "ellipse";
inline constexpr const char* kClipMaskPerson = "person";

// What Document::Validate holds a mask to: a known shape, a centre within
// one picture of the picture ([-1, 2]), a width and a height in ]0, 4], a
// feather in [0, 1], positive denominators. False with `error`.
bool ClipMaskIsValid(const ClipMask& mask, std::string& error);

// The mask with every fraction in lowest terms; a mask without a shape is
// the default one, whatever its other fields held.
ClipMask ReduceClipMask(const ClipMask& mask);

struct DocumentClip {
    Ulid id = GenerateUlid();
    Ulid source_id;
    RationalTime source_in;
    RationalTime duration;
    RationalTime timeline_in;
    // Deprecated migration flag. New video clips are always silent; audio is
    // represented exclusively by clips on audio tracks.
    bool include_audio = true;
    // Clips produced by one A/V separation share this stable group. It is a
    // selection relationship only; their edit times remain independent.
    Ulid link_group_id;
    // Exact phase relationship captured when the link is created. Drift is
    // (timeline_in-source_in) relative to the anchor, minus this reference.
    Ulid sync_anchor_clip_id;
    RationalTime sync_reference_delta{0, 1};
    // Ordered color-grade stack. Video-track clips only; see ClipEffect.
    std::vector<ClipEffect> effects;
    // Clips that make up one on-screen caption run share this ID, which must
    // reference a DocumentSequence::caption_styles entry. Empty means the
    // clip carries no caption.
    Ulid caption_group_id;
    // This clip's slice of the caption run's text.
    std::string caption_text;
    // ALPHA-2026-08 -- compositing changes the rendered project, so opacity
    // belongs to the canonical clip rather than to an Inspector preference.
    // Keep it exact until Timeline resolves the render-only float boundary.
    EffectParamValue opacity{1, 1};
    // QC-2026-09 A7 -- audio processing is part of the edit, not a mixer
    // preference: a quiet sentence must export and play back identically on
    // every machine. Gain is an exact number of decibels; the two envelopes
    // are exact timeline durations, never sample counts guessed by a UI.
    EffectParamValue audio_gain_db{0, 1};
    RationalTime audio_fade_in{0, 1};
    RationalTime audio_fade_out{0, 1};
    // MOTION-2026-09 M4 -- ROADMAP.md, docs/MOTION_FORMAT.md. A motion clip
    // reads its pictures from a composition of the project instead of a
    // media: `composition_id` names it and source_id stays empty. source_in
    // and duration then read in the composition's own time, exactly as a
    // media clip's read in its media: a head trim advances the composition,
    // and the clip cannot run past its last frame (MotionClip.h).
    Ulid composition_id;
    // The prop values this instance gives the composition, over its
    // defaults: null, or a non-empty object checked against the
    // composition's typed props. Kept as the motion format writes them,
    // decimals as their text, like a composition's definition.
    mcp_json::Value motion_props;
    // R3 -- docs/roadmap/REEL-2026-09.md. Constant playback speed, as an
    // exact fraction: the clip consumes `duration * speed` of its source.
    // 1/2 is the slow motion all 31 shots of the film measured used, 2 plays
    // twice as fast. Which source frame that makes each output frame show is
    // ClipSpeed.h's rule, and nothing else answers that question. Exact
    // rather than a float for the reason EffectParamValue exists at all: a
    // canonical document carries no floating-point literal.
    EffectParamValue speed{1, 1};
    // R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. A speed that changes
    // over the clip: keys of speed in the clip's own time, R2's ClipKeyframe
    // (an exact time, an exact value), each one held or linear up to the
    // next. The clip then keeps `speed` at 1/1 and reads the integral of
    // these keys; ClipSpeed.h holds the rule and nothing else computes it.
    // Empty on every clip that keeps one speed, which is how a project
    // without a ramp keeps its version and every byte it had.
    std::vector<ClipKeyframe> speed_ramp;
    // R1 -- docs/roadmap/REEL-2026-09.md. Scale, position, crop and mode,
    // all exact. Written only when it differs from the default above, so a
    // project that frames nothing keeps its version and every byte it had.
    ClipFraming framing;
    // R2 -- docs/roadmap/REEL-2026-09.md. The keys that drive the fields
    // above over the clip's own frames. Empty on every clip that does not
    // move, which is how a project that animates nothing keeps its version
    // and every byte it had. At most one entry per parameter; the value a
    // frame shows is ClipAnimation.h's answer, and nothing else computes
    // it.
    std::vector<ClipParameterAnimation> animation;
    // Out of lot -- the author, 29 October 2026: Resolve's Clip >
    // Enable/Disable Clip (D). A disabled clip keeps its place, its length, its
    // links and its grade, and is neither seen nor heard anywhere the timeline
    // plays or renders; ClipEnabled.h states the rule its consumers ask.
    // Written only when false, so a project that disables nothing keeps its
    // version and every byte it had.
    bool enabled = true;
    // EF3 -- docs/roadmap/EFFETS-2026-10.md. A Gaussian blur of the clip's
    // picture: its standard deviation (sigma) in sequence pixels, as an
    // exact fraction, 0 for none and at most kMaxClipBlur. In sequence
    // pixels rather than in the source's, so that the same shot blurs alike
    // whatever its rush's size and wherever its framing puts it: the
    // renderers convert it to their own pixels (the export's resolution,
    // the monitor's drawable). The picture is blurred within its own
    // rectangle, edges extended, before its grade and its opacity -- the
    // place each renderer gives it (Export.cc, Renderer.mm). Written only
    // when set, so a project that blurs nothing keeps its version and
    // every byte it had.
    EffectParamValue blur{0, 1};
    // PA4 -- docs/roadmap/PARITE-2026-11.md. When each word of
    // `caption_text` starts, on the carton's own clock: one time per word
    // (CaptionWordCount), or none for a carton that is not timed -- one
    // imported from an SRT, typed by hand or translated. A style that
    // animates its words (CaptionStyle::word_animation) reads them; a
    // carton without them is drawn whole, as it always was.
    //
    // The clock starts at `timeline_in - source_in`, so word k is said at
    // `timeline_in - source_in + caption_word_times[k]` (CaptionWordClock).
    // A carton reads no media, but every edit that cuts a clip in two
    // advances the right half's source_in as it does a rush's, and the
    // words of that half stay where they were said for free. A head trim,
    // a roll and a slide move a carton's start without touching its
    // source_in (BENCH-2026-09); ApplyOperation moves the times the other
    // way then, so a word is never said earlier or later for a carton
    // having been resized. A time may therefore be negative or past the
    // clip's end. Last, so that clips spelled member by member keep their
    // meaning.
    std::vector<RationalTime> caption_word_times;
    // PA8 -- docs/roadmap/PARITE-2026-11.md. The shape that cuts this
    // clip's picture out (ClipMask above); none by default. Video-track
    // clips of a media only.
    ClipMask mask;
};

// EF3 -- the largest blur a clip takes, in sequence pixels: past a fifth of
// an HD frame's height a picture is a wash of its average colour, and what
// is meant is a colour, not a blur.
inline constexpr int32_t kMaxClipBlur = 200;

// ITW7 -- ROADMAP.md. A clip copied into a second piece by a cut needs its
// own identity, and so does every effect it carries, because effect ids live
// in the document's one ID space. The sites that renewed the clip id alone
// made every cut through a graded clip -- a range delete, an overwrite, a
// word removal -- fail validation on a duplicate effect id.
void RenewClipIdentity(DocumentClip& clip);

enum class TransitionAlignment { Center, StartAtCut, EndAtCut };

// A transition belongs to the sequence, never to either clip. The referenced
// clips keep their original, non-overlapping edit rectangles while the
// transition reads the media handles around their shared cut.
//
// R5 -- docs/roadmap/REEL-2026-09.md. A "dip_to_color" reads no handle: it
// is a flat colour laid over the junction, so one of the two clip ids may
// be empty and the transition then sits at the edge of the track, over a
// single shot. That is the only way a film opens on black, because a shot
// cannot dissolve with nothing. A "cross_dissolve" still demands both.
struct DocumentTransition {
    Ulid id = GenerateUlid();
    Ulid track_id;
    // Empty on one side for an edge transition; never both (Validate).
    Ulid left_clip_id;
    Ulid right_clip_id;
    std::string type = "cross_dissolve";
    RationalTime duration;
    TransitionAlignment alignment = TransitionAlignment::Center;
    // R5 -- the colour a "dip_to_color" dips through, `#rrggbb` as
    // CaptionStyle spells its own. Exact in the document; it becomes a
    // float only in whichever renderer draws it (TransitionRender.h).
    // Written only when it is not black, so a project that never dips
    // keeps every byte it had.
    std::string color = "#000000";
};

// Shared presentation for one caption run. Clips join the run by setting
// their caption_group_id to this style's id; the text itself stays per-clip
// on DocumentClip so a run can span several underlying media clips.
struct CaptionStyle {
    Ulid id = GenerateUlid();
    std::string font_family = "system";
    int32_t font_size = 48;
    std::string color = "#ffffff";
    std::string position = "bottom";
    // ITW4 -- ROADMAP.md. What keeps a carton legible on any picture. On the
    // ITW-2026-09 interview, 11 white cartons of 58 fell to a median 2.1:1
    // contrast over a white shirt, and the delivery was burned again with
    // libass on top of the engine's export, so the file differed from the
    // project. Sizes are pixels at the sequence resolution, like font_size;
    // zero turns the decoration off.
    int32_t outline_width = 0;
    std::string outline_color = "#000000";
    int32_t shadow_offset = 0;
    std::string shadow_color = "#000000";
    // Empty means no box behind the text.
    std::string background_color;
    int32_t background_padding = 0;
    // Centre of the caption block as an exact fraction of the frame height
    // from the top, e.g. 3/4. {0, 1} keeps the `position` preset. "bottom"
    // put a vertical delivery's line under the apps' interface; the
    // workaround was five line breaks and U+2008 before the text.
    EffectParamValue vertical_position{0, 1};
    // Out of lot, 2026-11-04, after LEGENDES-2026-10 -- the weight a carton
    // is drawn at, as CSS and a motion text style name it: 400 the regular,
    // 500 the medium. A style used to name no weight and drew the family's
    // regular, while a house style set in Resolve can be a medium face; the
    // face is matched as a composition's is (MOTION-WEB-2026-10 MW7). Last,
    // so that styles spelled member by member keep their meaning.
    int32_t font_weight = 400;
    // PA4 -- docs/roadmap/PARITE-2026-11.md. How a carton's words follow
    // the voice, for a carton that carries their times
    // (DocumentClip::caption_word_times): "" draws the carton whole,
    // "highlight" draws the word being said in `highlight_color`, "reveal"
    // draws each word from the moment it is said, its place kept from the
    // start so that the line never moves. LEGENDES-2026-10 had set this
    // aside for want of a montage asking for it; the survey of 11 November
    // 2026 found it in every caption tool for vertical films.
    std::string word_animation;
    std::string highlight_color = "#ffe600";
};

// PA4 -- the animations a caption style may name, the empty one aside.
inline constexpr const char* kCaptionWordAnimationHighlight = "highlight";
inline constexpr const char* kCaptionWordAnimationReveal = "reveal";

// PA4 -- the words of a carton as its times count them: the runs of
// characters between spaces, tabs and line breaks. A line broken for
// delivery (WrapCaptionLines breaks at spaces) keeps its count.
size_t CaptionWordCount(const std::string& text);

// PA4 -- a word time as the document holds one: in lowest terms, zero as
// {0, 1}. Moving a carton's start moves its times the other way
// (ApplyOperation), and adding then subtracting a time written at another
// rate leaves a fraction equal to the first and spelled otherwise; reduced,
// an undo lands on the bytes it left. Document::Validate refuses any other
// spelling.
RationalTime ReducedCaptionWordTime(const RationalTime& time);

// PA4 -- how many of `clip`'s words have started at `position` on the
// timeline: what a renderer draws lit, or draws at all. Zero for a carton
// without times.
int32_t CaptionWordsSpoken(const DocumentClip& clip,
                           const RationalTime& position);

// F2.3 -- ROADMAP.md. The bounds Document::Validate holds a caption style's
// decoration to, in pixels at the sequence resolution, named so that a
// surface offering them -- the Inspector's controls -- takes the engine's
// and never keeps its own.
inline constexpr int32_t kCaptionStyleMaxOutlineWidth = 256;
inline constexpr int32_t kCaptionStyleMaxShadowOffset = 256;
inline constexpr int32_t kCaptionStyleMaxBackgroundPadding = 512;
// The weights a caption style may name, CSS's range, as a motion text's.
inline constexpr int32_t kCaptionStyleMinFontWeight = 1;
inline constexpr int32_t kCaptionStyleMaxFontWeight = 1000;

// What Document::Validate checks of one caption style, its id aside: the
// texts, the size, the decoration's bounds and the vertical position, an
// exact fraction strictly inside the frame or {0, 1} for the preset. False
// with `error` naming the group at fault.
bool ValidateCaptionStyle(const CaptionStyle& style, std::string& error);

// One camera's take within a multicam group. The angle's alignment to the
// other angles is not duplicated here: it lives on the referenced clip's
// existing sync_anchor_clip_id/sync_reference_delta (see DocumentClip),
// the same primitive used for ordinary A/V link groups.
struct MulticamAngle {
    Ulid id = GenerateUlid();
    std::string name;
    Ulid clip_id;
};

struct MulticamGroup {
    Ulid id = GenerateUlid();
    std::string name = "Multicam 1";
    std::vector<MulticamAngle> angles;
    // Empty means no angle is currently selected; otherwise references one
    // of angles[].id.
    Ulid active_angle_id;
};

struct DocumentTrack {
    Ulid id = GenerateUlid();
    std::string kind;
    int32_t index = 0;
    std::vector<DocumentClip> clips;
    // Locked tracks remain readable but reject edit operations.
    bool locked = false;
    // Ripple edits initiated on another track affect this track when enabled.
    // This is independent from the hard edit lock above.
    bool sync_lock = true;
    // Output state is canonical because it changes playback and export, not
    // merely local UI chrome. Video uses visible; audio uses muted/solo.
    bool visible = true;
    bool muted = false;
    bool solo = false;
};

// R6 -- docs/roadmap/REEL-2026-09.md. The look LUT of one sequence: a
// reference to an imported .cube, never its numbers.
//
// A .cube is floating-point data and a document refuses a float literal
// outright (PHILOSOPHY.md principle 4), so the boundary is drawn here. The
// file is copied into the project package under `digest`, and what the
// document carries is that digest, the name the file was imported under,
// and the space its author declared it in. Reopening the project anywhere
// resolves Luts/<digest>.cube inside the package, and the digest is what
// says those are the same bytes.
//
// `space` is empty, or one of the names LookLut.h lists. It is declared by
// the author and never guessed: under colour management a LUT without one
// is refused rather than laid on whichever value is at hand.
struct SequenceLut {
    std::string digest;
    std::string name;
    std::string space;
};

// A sequence is the addressable owner of one timeline. Project media and bins
// remain outside it; tracks and sequence markers cannot exist without it.
struct DocumentSequence {
    Ulid id = GenerateUlid();
    std::string name = "Sequence 1";
    int32_t width = 1920;
    int32_t height = 1080;
    MediaRate frame_rate{25, 1};
    std::vector<DocumentMarker> markers;
    std::vector<DocumentTransition> transitions;
    std::vector<DocumentTrack> tracks;
    std::vector<CaptionStyle> caption_styles;
    std::vector<MulticamGroup> multicam_groups;
    // R6 -- the look of the whole timeline, laid after every clip's grade
    // and before the output transform. Empty for every sequence that has
    // none, which is what leaves their bytes and their version untouched.
    SequenceLut look_lut;
};

// MOTION-2026-09 M3 -- ROADMAP.md, docs/MOTION_FORMAT.md. A motion
// composition of the project: a tree of nodes styled with CSS names and
// animated with Remotion's, which the engine lays out and renders.
//
// It belongs to the project, beside the media it is an alternative to, and
// not to one timeline: a title card is placed on several timelines (M4) and
// exported in several versions and formats of its own (M6).
//
// `definition` is the composition in the format docs/MOTION_FORMAT.md
// describes, as the author wrote it, plus the identities the document gives
// it: every node carries a `uid` and every interpolation the `uids` of its
// keyframes, all ULIDs in the document's one ID space. It is kept as that
// tree rather than as structs because the motion vocabulary is its own
// format, validated by MotionComposition.cc, and grows without the document
// changing version; its numbers are decimals kept as the text they were
// written in, never re-spelled from a double.
struct DocumentComposition {
    Ulid id = GenerateUlid();
    std::string name;
    mcp_json::Value definition;
};

// MOTION-2026-09 M3 -- the document version that carries compositions. A
// document is written at the oldest version that holds all it contains: one
// without a composition keeps version 7 and every byte it had, and an older
// binary still opens it; one with a composition is version 8, which a binary
// that does not know compositions refuses rather than dropping them.
inline constexpr int32_t kDocumentVersionWithCompositions = 8;

// MOTION-2026-09 M4 -- the version that carries motion clips. Version 8 as
// M3 writes it cannot hold one: its reader ignores keys it does not know, so
// an M3 binary would read a motion clip as a media clip that names no
// source, and refuse the document for that missing source at best. Version 9
// has that binary refuse the file as newer than itself, and leaves every
// document without a motion clip at the version it had.
inline constexpr int32_t kDocumentVersionWithMotionClips = 9;

// R3 -- docs/roadmap/REEL-2026-09.md. The version that carries a clip speed.
// Version 9 cannot: its reader drops the key it does not know, so a binary
// that predates R3 would read a shot slowed to 1/2 as a shot at 1/1 and
// export twice the source it should -- a wrong montage under no error at
// all. Version 10 has that binary refuse the file instead, and a document
// where every clip stays at 1/1 keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithClipSpeed = 10;

// R1 -- docs/roadmap/REEL-2026-09.md. The version that carries a clip
// framing, for the same reason version 10 carries a speed: a binary that
// predates R1 drops the key it does not know, and would deliver a shot
// fitted with black bars where the project says it fills the frame -- a
// wrong picture under no error at all. Version 11 has that binary refuse
// the file instead, and a document where every clip keeps the default
// framing keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithClipFraming = 11;

// R2 -- docs/roadmap/REEL-2026-09.md. The version that carries animated
// clip parameters, for the same reason version 11 carries a framing: a
// binary that predates R2 drops the key it does not know, and would play a
// zoom as a still -- the rhythm of the film gone, under no error at all.
// Version 12 has that binary refuse the file instead, and a document where
// no clip animates keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithClipAnimation = 12;

// R5 -- docs/roadmap/REEL-2026-09.md. The version that carries a colour
// transition, and a transition at the edge of a track. A binary that
// predates R5 does refuse both on its own -- its Validate knows only
// "cross_dissolve" and demands two adjacent clips -- but it refuses them
// with a message about an unsupported type, which sends the reader looking
// at the transition instead of at the build. Version 13 says which of the
// two is out of date, and a document whose transitions are all plain
// two-clip dissolves keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithColorTransition = 13;

// R6 -- docs/roadmap/REEL-2026-09.md. The version that carries a sequence
// look LUT. A binary that predates R6 drops the key it does not know and
// would deliver the film ungraded -- the look the whole grade was built
// around gone, under no error at all, which is the same class of silent
// wrong picture version 10 and version 11 exist for. Version 14 has that
// binary refuse the file instead, and a document whose sequences carry no
// LUT keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithSequenceLut = 14;

// G7 -- docs/roadmap/GRADE-2026-09.md. The version that carries a white
// balance: a clip's color.temperature, which is now a chromatic adaptation
// to its media's as-shot white balance, or that as-shot white balance
// itself. A binary that predates G7 reads color.temperature as the
// warm/cool offset it was and drops the as-shot key: the same film, another
// picture, under no error. Version 15 has that binary refuse the file; a
// document with neither keeps the version and the bytes it had. Unlike the
// versions before it, an older document holding a color.temperature is not
// refused: it is read, and renders by the new rule, which is the point.
inline constexpr int32_t kDocumentVersionWithWhiteBalance = 15;

// G3 -- docs/roadmap/GRADE-2026-09.md. The version that carries a media's
// own colour input. A binary that predates G3 drops the key and reads the
// media with the project's input: a Rec.709 clip in an S-Log3 project
// decoded as S-Log3, under no error. Version 16 has that binary refuse the
// file; a document whose media follow the project keeps the version and the
// bytes it had.
inline constexpr int32_t kDocumentVersionWithMediaColorInput = 16;

// GR6, GR8 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The version that
// carries a grading stage, or a knob of one, that a binary before this lot
// skips without a word: a color.contrast with its own `pivot` (the contrast
// rendered about middle grey instead), a color.hue or a color.hue_curves
// (an unregistered color.* type renders nothing). The same film, another
// picture, under no error: version 17 has that binary refuse the file, and
// a document with none of them keeps the version and the bytes it had. As
// with version 15, an older document that already holds one is not refused:
// it is read, and renders by this build's rule. The offset wheel of the same
// lot needs no version: color.wheels names its knobs, and an older binary
// refuses the one it does not know.
inline constexpr int32_t kDocumentVersionWithHueStages = 17;

// R3, criterion 4 -- docs/roadmap/REEL-2026-09.md. The version that carries
// a speed ramp. A binary before it drops the key it does not know and plays
// the ramped shot at 1/1 -- the slow motion on the beat gone, and the shot
// reading a different stretch of its rush, under no error at all, which is
// what version 10 exists for. Version 18 has that binary refuse the file
// instead, and a document without a ramp keeps the version and the bytes it
// had.
inline constexpr int32_t kDocumentVersionWithSpeedRamp = 18;

// PL1 -- docs/roadmap/PLANS-2026-10.md. The version that carries described
// shots. A binary before it drops the key it does not know, and the next
// save loses every description -- written by a model at a price, or by a
// person, and not to be had again for free -- under no error at all.
// Version 19 has that binary refuse the file instead, and a library without
// a description keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithShotDescriptions = 19;
// GN3 -- docs/roadmap/GENERATIF-2026-10.md. The version that carries a
// generated media's provenance. A binary before it drops the key it does
// not know and, at its next save, writes the library without it: the
// picture stays, the mark that a service made it is gone, under no error.
// Version 20 has that binary refuse the file instead; a library of filmed
// media keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithGeneratedMedia = 20;
// DEC31, BH2 -- docs/roadmap/BLANC-HLG-2026-10.md. The version that carries
// a media declared a graphic. A binary before it drops the key it does not
// know and reads the logo as a camera's picture: its white under the HLG
// peak, its colours through the camera's input, under no error. Version 21
// has that binary refuse the file instead, and a library without a graphic
// keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithGraphicsInput = 21;
// GR16 -- docs/roadmap/GRADE-RESOLVE-2026-10.md. The version that carries a
// color.resolve_primaries stage, a grade brought from DaVinci Resolve. A
// binary before it skips the unregistered type and renders the clip
// ungraded: the same film, another picture, under no error. Version 22 has
// that binary refuse the file instead; a document without the stage keeps
// the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithResolvePrimaries = 22;
// Out of lot -- the author, 29 October 2026. The version that carries a
// disabled clip. A binary before it drops the key it does not know and plays
// the shot the editor took out of the film -- seen and heard again, under no
// error at all. Version 23 has that binary refuse the file instead, and a
// document where every clip is enabled keeps the version and the bytes it
// had.
inline constexpr int32_t kDocumentVersionWithDisabledClips = 23;
// EF2, EF3 -- docs/roadmap/EFFETS-2026-10.md. The version that carries the
// effects of the « Effets » tab: a cross fade on an audio track (EF2) or a
// clip's Gaussian blur (EF3). A binary before it refuses the cross fade
// anyway, but as a transition that "must reference a video track", which
// sends the reader to the transition rather than to the build; and it reads
// a blurred clip without its `blur` key, delivering the shot sharp under no
// error. Version 24 says which is out of date; a document with neither keeps
// the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithTimelineEffects = 24;
// GN6b -- docs/roadmap/GENERATIF-2026-10.md. The version that carries the
// words a generated voice says (MediaGeneration::reference_text). A binary
// before it drops the key it does not know and, at its next save, writes
// the voice without them: the voice still plays, still marked, but can no
// longer be cloned, under no error. Version 25 has that binary refuse the
// file instead; a library without a voice keeps the version and the bytes
// it had.
inline constexpr int32_t kDocumentVersionWithVoiceReference = 25;
// PA8 -- docs/roadmap/PARITE-2026-11.md. The version that carries a clip's
// mask. A binary before it reads a masked clip without its `mask` key and
// delivers the whole picture over what the mask let show, under no error.
// Version 26 has that binary refuse the file instead; a document without a
// mask keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithClipMask = 26;
// PA15 -- docs/roadmap/PARITE-2026-11.md. The version that carries a mask
// of shape "person". A binary of version 26 knows masks but not this
// shape, and would refuse the clip with an error about its shape; version
// 27 has it say instead that the file is newer than it reads. A document
// without such a mask keeps the version and the bytes it had.
inline constexpr int32_t kDocumentVersionWithPersonMask = 27;

class Document {
public:
    // The version of a document without compositions; what a file declares
    // is DocumentFormatVersion below.
    int32_t version = 7;
    DocumentSequence sequence;
    ColorManagementSettings color_management;
    std::vector<LibraryMedia> library;
    std::vector<DocumentBin> bins;
    std::vector<DocumentSource> sources;
    // MOTION-2026-09 M3 -- shared by every timeline, like the library.
    std::vector<DocumentComposition> compositions;

    static bool Load(const std::string& path, Document& output,
                     std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error);
    static bool LoadFromString(const std::string& json, Document& output,
                               std::string& error, bool validate);
    bool Save(const std::string& path, std::string& error) const;
    std::string SaveToString() const;
    bool Validate(std::string& error) const;
    // Out of lot -- the author, 29 October 2026: "l'app est toujours lente dans
    // les opérations". AlreadyChecked: the caller has validated these very
    // compositions with this library and these sources (their parse, their
    // references, their identities: none of it reads the sequence), and they
    // are not parsed again; their identities are only met against the
    // sequence's. Project::
    // Validate does it once for all its timelines: per timeline, a project of
    // 34 timelines and 614 KB of compositions parsed them 34 times per
    // validation, 400 ms, twice per edit. Everything else is checked, the
    // motion clips against their compositions included.
    enum class CompositionCheck { Check, AlreadyChecked };
    bool Validate(std::string& error, CompositionCheck compositionCheck) const;

    const DocumentSource* FindSource(const Ulid& id) const;
    DocumentSource* FindSource(const Ulid& id);
    const LibraryMedia* FindLibraryMedia(const Ulid& id) const;
    LibraryMedia* FindLibraryMedia(const Ulid& id);
    // G3 -- the settings a picture of `mediaId` is read with: the
    // project's, with the media's own colour input when it declares one.
    // A clip's source id is its media's id.
    ColorManagementSettings ColorManagementFor(const Ulid& mediaId) const;
    // DEC31, BH2 -- whether `mediaId` is declared a graphic
    // (MediaColorInput::graphics): placed as authored, in the graphics
    // view, by the export and the monitor.
    bool IsGraphicsMedia(const Ulid& mediaId) const;
    const DocumentBin* FindBin(const Ulid& id) const;
    DocumentBin* FindBin(const Ulid& id);
    const DocumentMarker* FindMarker(const Ulid& id) const;
    DocumentMarker* FindMarker(const Ulid& id);
    const DocumentTransition* FindTransition(const Ulid& id) const;
    DocumentTransition* FindTransition(const Ulid& id);
    const CaptionStyle* FindCaptionStyle(const Ulid& id) const;
    CaptionStyle* FindCaptionStyle(const Ulid& id);
    const MulticamGroup* FindMulticamGroup(const Ulid& id) const;
    MulticamGroup* FindMulticamGroup(const Ulid& id);
    const DocumentTrack* FindTrack(const Ulid& id) const;
    DocumentTrack* FindTrack(const Ulid& id);
    const DocumentClip* FindClip(const Ulid& id) const;
    DocumentClip* FindClip(const Ulid& id);
    const DocumentTrack* FindTrackForClip(const Ulid& clipId) const;
    DocumentTrack* FindTrackForClip(const Ulid& clipId);
    const DocumentComposition* FindComposition(const Ulid& id) const;
    DocumentComposition* FindComposition(const Ulid& id);
};

// MOTION-2026-09 M4 -- a clip whose source is a composition, and a timeline
// that holds one: what only version 9 can carry. A clip that names instance
// props without a composition is refused by Validate, but the version still
// follows what the file spells.
inline bool IsMotionClip(const DocumentClip& clip) {
    return !clip.composition_id.empty();
}

inline bool HoldsMotionClip(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (IsMotionClip(clip) || !clip.motion_props.IsNull()) {
                return true;
            }
        }
    }
    return false;
}

// R3 -- a timeline that holds a clip whose speed is spelled anything other
// than 1/1: what only version 10 can carry. The test is the spelling, not
// the value, because it has to agree with what the file writes: the table
// omits the key only for the member's own default, so a clip written 2/2
// does carry a key an older reader would drop. set_clip_speed reduces the
// fraction it is given, so that spelling only arrives by hand.
inline bool HoldsClipSpeed(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (clip.speed.num != 1 || clip.speed.den != 1) {
                return true;
            }
        }
    }
    return false;
}

// R1 -- a clip whose framing is spelled anything other than the default.
// The test is the spelling and not the value, for the reason HoldsClipSpeed
// gives: the table omits the key only for the member's own default, so a
// framing written `scale: 2/2` does carry a key an older reader would drop.
// set_clip_framing reduces the fractions it is given, so that spelling only
// ever arrives by hand.
inline bool ClipHasFraming(const DocumentClip& clip) {
    const ClipFraming defaults;
    const auto same = [](const EffectParamValue& left,
                         const EffectParamValue& right) {
        return left.num == right.num && left.den == right.den;
    };
    return clip.framing.mode != defaults.mode ||
           !same(clip.framing.scale, defaults.scale) ||
           !same(clip.framing.position_x, defaults.position_x) ||
           !same(clip.framing.position_y, defaults.position_y) ||
           !same(clip.framing.crop_left, defaults.crop_left) ||
           !same(clip.framing.crop_right, defaults.crop_right) ||
           !same(clip.framing.crop_top, defaults.crop_top) ||
           !same(clip.framing.crop_bottom, defaults.crop_bottom);
}

inline bool HoldsClipFraming(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (ClipHasFraming(clip)) {
                return true;
            }
        }
    }
    return false;
}

// DEC36 -- docs/roadmap/GENERATIF-2026-10.md. Whether a clip plays a media
// that a generation service made (MediaGeneration, a non-empty service):
// what its badge on the timeline says (ClipBadges.h) and what check_timeline
// lists under `generated` (TimelineCheck.h). A motion clip plays a
// composition, never a library media, and is never generated in this sense.
inline bool ClipPlaysGeneratedMedia(const Document& document,
                                    const DocumentClip& clip) {
    if (IsMotionClip(clip)) {
        return false;
    }
    const LibraryMedia* media = document.FindLibraryMedia(clip.source_id);
    return media != nullptr && !media->generation.service.empty();
}

// R2 -- a clip that carries at least one key. No spelling subtlety here,
// unlike a framing or a speed: an animation is a list, and the list is
// either empty or it is not. A parameter entry with no key at all is
// refused by Document::Validate rather than written as an empty list.
inline bool ClipHasAnimation(const DocumentClip& clip) {
    return !clip.animation.empty();
}

inline bool HoldsClipAnimation(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (ClipHasAnimation(clip)) {
                return true;
            }
        }
    }
    return false;
}

// R3, criterion 4 -- a timeline that holds a clip whose speed is a ramp:
// what only version 18 can carry. A list, like an animation: it is empty or
// it is not.
inline bool HoldsSpeedRamp(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.speed_ramp.empty()) {
                return true;
            }
        }
    }
    return false;
}

// R5 -- a transition this ticket brought: one spelled "dip_to_color", or
// one carrying a colour key at all. The test is the spelling and not the
// shape, for two reasons. The first is HoldsClipSpeed's: the table omits
// `color` only for the member's own default, so a dip written `#ffffff`
// does carry a key an older reader would drop. The second is what the
// fixtures already hold: documents written long before this ticket spell
// transition types this build has never heard of, and transitions with an
// empty clip id, and they must keep reading exactly as they did -- so
// "has a type I do not know" and "has one side missing" cannot be the
// test. Both are refused by name by any build's Validate anyway, which is
// what makes leaving them out of the version safe.
inline bool IsColorTransition(const DocumentTransition& transition) {
    static const DocumentTransition defaults;
    return transition.type == "dip_to_color" ||
           transition.color != defaults.color;
}

inline bool HoldsColorTransition(const DocumentSequence& sequence) {
    for (const DocumentTransition& transition : sequence.transitions) {
        if (IsColorTransition(transition)) {
            return true;
        }
    }
    return false;
}

// R6 -- a sequence whose look LUT is spelled at all. The test is the
// spelling and not "has a digest", for HoldsClipSpeed's reason: the table
// omits the `look_lut` key only for the member's own default, so a sequence
// carrying a name or a space without a digest does write a key an older
// reader would drop. set_sequence_lut only ever writes the three together,
// so that spelling arrives by hand.
inline bool HoldsSequenceLut(const DocumentSequence& sequence) {
    static const SequenceLut defaults;
    return sequence.look_lut.digest != defaults.digest ||
           sequence.look_lut.name != defaults.name ||
           sequence.look_lut.space != defaults.space;
}

// G7 -- a sequence with a color.temperature stage, and a library with an
// as-shot white balance: what version 15 holds.
inline bool HoldsTemperature(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.temperature") return true;
    return false;
}

inline bool HoldsAsShotWhiteBalance(const std::vector<LibraryMedia>& library) {
    for (const LibraryMedia& media : library)
        if (media.white_balance_kelvin != 0) return true;
    return false;
}

// G3 -- a library with a media that declares its own colour input: what
// version 16 holds. The test is the spelling, for HoldsSequenceLut's
// reason.
inline bool HoldsMediaColorInput(const std::vector<LibraryMedia>& library) {
    static const MediaColorInput defaults;
    for (const LibraryMedia& media : library)
        if (media.color_input != defaults) return true;
    return false;
}

// DEC31 -- a library holding a media declared a graphic: what version 21
// holds.
inline bool HoldsGraphicsInput(const std::vector<LibraryMedia>& library) {
    for (const LibraryMedia& media : library)
        if (media.color_input.graphics) return true;
    return false;
}

// GN3 -- a library holding a generated media: what version 19 holds.
inline bool HoldsGeneratedMedia(const std::vector<LibraryMedia>& library) {
    static const MediaGeneration defaults;
    for (const LibraryMedia& media : library)
        if (media.generation != defaults) return true;
    return false;
}

// GN6b -- a library holding a voice with its reference words: what
// version 25 holds.
inline bool HoldsVoiceReference(const std::vector<LibraryMedia>& library) {
    for (const LibraryMedia& media : library)
        if (!media.generation.reference_text.empty()) return true;
    return false;
}

// GR6, GR8 -- a sequence with a contrast pivot, a hue rotation or hue
// curves: what version 17 holds. The pivot counts as soon as it is spelled.
inline bool HoldsHueStages(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.hue" ||
                    effect.type == "color.hue_curves" ||
                    (effect.type == "color.contrast" &&
                     effect.params.count("pivot") != 0))
                    return true;
    return false;
}

// GR16 -- a sequence with a color.resolve_primaries stage: what version 22
// holds.
inline bool HoldsResolvePrimaries(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            for (const ClipEffect& effect : clip.effects)
                if (effect.type == "color.resolve_primaries") return true;
    return false;
}

// Out of lot -- the author, 29 October 2026. A sequence with a disabled clip:
// what version 23 holds.
inline bool HoldsDisabledClip(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks) {
        for (const DocumentClip& clip : track.clips) {
            if (!clip.enabled) {
                return true;
            }
        }
    }
    return false;
}

// EF2 -- a sequence with a cross fade: what version 24 holds. The test is
// the spelling, for IsColorTransition's reason.
inline bool HoldsCrossFade(const DocumentSequence& sequence) {
    for (const DocumentTransition& transition : sequence.transitions)
        if (transition.type == "cross_fade") return true;
    return false;
}

// EF3 -- docs/roadmap/EFFETS-2026-10.md. A clip whose blur is spelled
// anything other than the default 0/1, for HoldsClipSpeed's reason: the
// table omits the key only for the member's own default, so a blur written
// 0/5 does carry a key an older reader would drop. set_clip_blur reduces
// the fraction it is given, so that spelling only arrives by hand.
inline bool ClipHasBlur(const DocumentClip& clip) {
    return clip.blur.num != 0 || clip.blur.den != 1;
}

// PA8 -- a clip whose picture a shape cuts out.
inline bool ClipHasMask(const DocumentClip& clip) {
    return !clip.mask.shape.empty();
}

// PA8 -- a sequence with a masked clip: what version 26 holds.
inline bool HoldsClipMask(const DocumentSequence& sequence);

// PA15 -- a clip cut out by the matte of the people it shows.
inline bool ClipHasPersonMask(const DocumentClip& clip) {
    return clip.mask.shape == kClipMaskPerson;
}

// PA15 -- a sequence with such a clip: what version 27 holds.
inline bool HoldsPersonMask(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasPersonMask(clip)) return true;
    return false;
}

// EF3 -- a sequence with a blurred clip: what version 24 holds too.
inline bool HoldsClipBlur(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasBlur(clip)) return true;
    return false;
}

inline bool HoldsClipMask(const DocumentSequence& sequence) {
    for (const DocumentTrack& track : sequence.tracks)
        for (const DocumentClip& clip : track.clips)
            if (ClipHasMask(clip)) return true;
    return false;
}

// PL1 -- a library with at least one described shot: what version 19
// holds.
inline bool HoldsShotDescriptions(const std::vector<LibraryMedia>& library) {
    for (const LibraryMedia& media : library)
        if (!media.shots.empty()) return true;
    return false;
}

// MOTION-2026-09 M3 -- the version a document, or a project file, declares:
// the oldest that holds what it contains. M4 -- a motion clip needs a
// composition, and the version after the one that brought them. R3 -- a clip
// speed needs the version after that. R1 -- a clip framing the version
// after that one. R2 -- an animated parameter the version after that. R5 --
// a colour or edge transition the version after that again. R6 -- a
// sequence look LUT the version after that. G7 -- a white balance the one
// after, G3 -- a media's own colour input the one after that, GR6 -- a
// contrast pivot or a hue stage the one after that again, R3's speed ramp
// the one after that, PL1's described shots the one after that, GN3's
// generated media the one after that, DEC31's graphics input the one after
// that, GR16's Resolve primaries the one after that again, a disabled
// clip (29 October 2026) the one after that, EF2's cross fade or EF3's
// blur -- the effects of the « Effets » tab -- the one after that again, and
// GN6b's voice with its reference words the one after that.
inline int32_t DocumentFormatVersion(
    bool hasCompositions, bool hasMotionClips = false,
    bool hasClipSpeed = false, bool hasClipFraming = false,
    bool hasClipAnimation = false, bool hasColorTransition = false,
    bool hasSequenceLut = false, bool hasWhiteBalance = false,
    bool hasMediaColorInput = false, bool hasHueStages = false,
    bool hasSpeedRamp = false, bool hasShotDescriptions = false,
    bool hasGeneratedMedia = false, bool hasGraphicsInput = false,
    bool hasResolvePrimaries = false, bool hasDisabledClips = false,
    bool hasTimelineEffects = false, bool hasVoiceReference = false,
    bool hasClipMask = false, bool hasPersonMask = false) {
    // PA15 -- a mask that follows the people is the newest.
    if (hasPersonMask) {
        return kDocumentVersionWithPersonMask;
    }
    // PA8 -- then a clip's mask.
    if (hasClipMask) {
        return kDocumentVersionWithClipMask;
    }
    if (hasVoiceReference) {
        return kDocumentVersionWithVoiceReference;
    }
    if (hasTimelineEffects) {
        return kDocumentVersionWithTimelineEffects;
    }
    if (hasDisabledClips) {
        return kDocumentVersionWithDisabledClips;
    }
    if (hasResolvePrimaries) {
        return kDocumentVersionWithResolvePrimaries;
    }
    if (hasGraphicsInput) {
        return kDocumentVersionWithGraphicsInput;
    }
    if (hasGeneratedMedia) {
        return kDocumentVersionWithGeneratedMedia;
    }
    if (hasShotDescriptions) {
        return kDocumentVersionWithShotDescriptions;
    }
    if (hasSpeedRamp) {
        return kDocumentVersionWithSpeedRamp;
    }
    if (hasHueStages) {
        return kDocumentVersionWithHueStages;
    }
    if (hasMediaColorInput) {
        return kDocumentVersionWithMediaColorInput;
    }
    if (hasWhiteBalance) {
        return kDocumentVersionWithWhiteBalance;
    }
    if (hasSequenceLut) {
        return kDocumentVersionWithSequenceLut;
    }
    if (hasColorTransition) {
        return kDocumentVersionWithColorTransition;
    }
    if (hasClipAnimation) {
        return kDocumentVersionWithClipAnimation;
    }
    if (hasClipFraming) {
        return kDocumentVersionWithClipFraming;
    }
    if (hasClipSpeed) {
        return kDocumentVersionWithClipSpeed;
    }
    if (hasMotionClips) {
        return kDocumentVersionWithMotionClips;
    }
    return hasCompositions ? kDocumentVersionWithCompositions
                           : Document().version;
}
