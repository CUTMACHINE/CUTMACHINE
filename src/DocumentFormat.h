#pragma once

// FORMAT-2026-09 -- ROADMAP.md, F6. The canonical format of a document, of
// the parts a project file and a project delta carry, described once.
//
// Document.cc used to spell each struct twice -- a writer and a key-by-key
// parser -- and nothing tied either to the struct. It is how the journal
// lost a clip's gain (QC-2026-09 A7), and member counts only noticed that a
// struct had grown, not that a writer forgot the new member.
//
// A DocumentTable names each serialized member once, in the order the file
// writes it, with how it is present:
//
//   Field(key, member)             always written, required on read
//     .AddedLater()                tolerated absent: files written before
//                                  the member existed
//     .OmittedWhenEmpty()          a string written only when non-empty,
//                                  and tolerated absent
//     .OmittedWhenDefault()        written only when it differs from the
//                                  member's in-class default, and tolerated
//                                  absent: a member most files never set
//                                  leaves their bytes as they were
//     .WrittenIf(predicate)        written when predicate(owner) holds;
//                                  read only when it holds for the members
//                                  read so far, and then required unless
//                                  AddedLater
//     .Since(version)              always written; read, and required, only
//                                  from that document version on
//     .PresenceRecordedIn(flag)    written only when `flag` is set; reading
//                                  sets `flag` to whether the key was there
//     .Lines()                     a list or map written one element per
//                                  line, `[]` when empty
//     .PaddedWhenEmpty()           the same, but `[  ]` when empty: the
//                                  closing indentation is written anyway,
//                                  as sequence lists always have been
//   CarriedBy(key, flag)           no key of its own: whether `key` is
//                                  present encodes it
//
// A document is read by key, not by position: it is a file people and
// scripts edit, and its parser has always ignored key order and unknown
// keys. Both directions are derived from the tables (DocumentFormat.cc),
// and CheckTable makes every table complete by construction: as many
// entries as the aggregate has members, no member or key twice, every flag
// carried by the key that records it. A member added to a tabled struct is
// a compile error until its place in the file is decided.
//
// Internal to the document codec and its tests. The public API stays
// Document::SaveToString, Project::SaveToString, DocumentJson.h and their
// readers.

#include "AggregateMembers.h"
#include "ArchiveManifest.h"
#include "Document.h"
#include "Project.h"
#include "RunStep.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace document_format {

enum class Kind {
    Value,
    Carried,
};

enum class Layout {
    Inline,
    Lines,
    LinesPaddedWhenEmpty,
};

template <typename T>
struct IsList : std::false_type {};

template <typename T>
struct IsList<std::vector<T>> : std::true_type {};

template <typename V>
struct IsList<std::map<std::string, V>> : std::true_type {};

template <typename Owner, typename Member, Kind kind = Kind::Value>
struct FieldSpec {
    static constexpr Kind kKind = kind;
    using OwnerType = Owner;
    using MemberType = Member;

    std::string_view key;
    Member Owner::*member = nullptr;
    bool added_later = false;
    bool omitted_when_empty = false;
    bool omitted_when_default = false;
    bool (*written_if)(const Owner&) = nullptr;
    int32_t since = 0;
    bool Owner::*presence = nullptr;
    Layout layout = Layout::Inline;

    constexpr FieldSpec AddedLater() const {
        FieldSpec copy = *this;
        copy.added_later = true;
        return copy;
    }

    constexpr FieldSpec OmittedWhenEmpty() const {
        static_assert(std::is_same_v<Member, std::string>,
                      "OmittedWhenEmpty describes a string");
        FieldSpec copy = *this;
        copy.omitted_when_empty = true;
        return copy;
    }

    constexpr FieldSpec OmittedWhenDefault() const {
        FieldSpec copy = *this;
        copy.omitted_when_default = true;
        return copy;
    }

    constexpr FieldSpec WrittenIf(bool (*predicate)(const Owner&)) const {
        FieldSpec copy = *this;
        copy.written_if = predicate;
        return copy;
    }

    constexpr FieldSpec Since(int32_t version) const {
        FieldSpec copy = *this;
        copy.since = version;
        return copy;
    }

    constexpr FieldSpec PresenceRecordedIn(bool Owner::*flag) const {
        FieldSpec copy = *this;
        copy.presence = flag;
        return copy;
    }

    constexpr FieldSpec Lines() const {
        static_assert(IsList<Member>::value, "Lines describes a list or map");
        FieldSpec copy = *this;
        copy.layout = Layout::Lines;
        return copy;
    }

    constexpr FieldSpec PaddedWhenEmpty() const {
        static_assert(IsList<Member>::value,
                      "PaddedWhenEmpty describes a list or map");
        FieldSpec copy = *this;
        copy.layout = Layout::LinesPaddedWhenEmpty;
        return copy;
    }
};

template <typename Owner, typename Member>
constexpr FieldSpec<Owner, Member> Field(std::string_view key,
                                         Member Owner::*member) {
    return {key, member};
}

// A flag with no key of its own: whether `key` is present encodes it.
template <typename Owner>
constexpr FieldSpec<Owner, bool, Kind::Carried> CarriedBy(std::string_view key,
                                                          bool Owner::*flag) {
    return {key, flag};
}

// Specialized once per serialized struct.
template <typename T>
struct DocumentTable {};

// Specialized once per serialized enum: its spelling in the file.
template <typename Enum>
struct EnumNames {};

template <typename T, typename = void>
struct IsTabled : std::false_type {};

template <typename T>
struct IsTabled<T, std::void_t<decltype(DocumentTable<T>::kFields)>>
    : std::true_type {};

template <typename T, typename Visit>
void ForEachField(Visit&& visit) {
    std::apply([&](const auto&... fields) { (visit(fields), ...); },
               DocumentTable<T>::kFields);
}

// ---------------------------------------------------------------------
// Completeness checks.
// ---------------------------------------------------------------------

template <typename Owner, typename Left, typename Right>
constexpr bool SameMember(Left Owner::*, Right Owner::*) {
    return false;
}

template <typename Owner, typename Member>
constexpr bool SameMember(Member Owner::*left, Member Owner::*right) {
    return left == right;
}

template <typename Left, typename Right>
constexpr bool Collide(const Left& left, const Right& right) {
    if (SameMember(left.member, right.member)) return true;
    return Left::kKind != Kind::Carried && Right::kKind != Kind::Carried &&
           left.key == right.key;
}

template <typename Carried, typename Candidate>
constexpr bool Carries(const Carried& carried, const Candidate& candidate) {
    return Candidate::kKind == Kind::Value && candidate.key == carried.key &&
           SameMember(candidate.presence, carried.member);
}

template <typename Candidate, typename Carried>
constexpr bool RecordedIn(const Candidate& candidate, const Carried& carried) {
    return Carried::kKind == Kind::Carried &&
           SameMember(candidate.presence, carried.member);
}

template <typename T>
constexpr bool FieldsAreDistinct() {
    return std::apply(
        [](const auto&... fields) {
            const auto collisions = [&](const auto& candidate) {
                return (0 + ... + (Collide(candidate, fields) ? 1 : 0));
            };
            return ((collisions(fields) == 1) && ...);
        },
        DocumentTable<T>::kFields);
}

template <typename T>
constexpr bool FlagsAreCarried() {
    return std::apply(
        [](const auto&... fields) {
            const auto carried = [&](const auto& candidate) {
                using Candidate = std::decay_t<decltype(candidate)>;
                if (Candidate::kKind == Kind::Carried)
                    return (false || ... || Carries(candidate, fields));
                return candidate.presence == nullptr ||
                       (false || ... || RecordedIn(candidate, fields));
            };
            return (carried(fields) && ...);
        },
        DocumentTable<T>::kFields);
}

template <typename T>
constexpr void CheckTable() {
    static_assert(std::tuple_size_v<decltype(DocumentTable<T>::kFields)> ==
                      AggregateMemberCount<T>::value,
                  "a document struct needs exactly one DocumentTable entry "
                  "per member");
    static_assert(FieldsAreDistinct<T>(),
                  "a DocumentTable names the same member or key twice");
    static_assert(FlagsAreCarried<T>(),
                  "a presence flag and its CarriedBy entry must name each "
                  "other");
}

template <typename Enum>
constexpr bool EnumNamesAreDistinct() {
    const auto& names = EnumNames<Enum>::kNames;
    const size_t count = sizeof(names) / sizeof(names[0]);
    for (size_t left = 0; left < count; ++left) {
        for (size_t right = left + 1; right < count; ++right) {
            if (names[left].first == names[right].first ||
                names[left].second == names[right].second)
                return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// Enums.
// ---------------------------------------------------------------------

// DB3, OR2 -- receipts are package metadata, not fields of the montage.
// One table still owns their read/write contract and catches lost members.
template <>
struct DocumentTable<RunStepRequest> {
    static constexpr auto kFields = std::make_tuple(
        Field("run_id", &RunStepRequest::run_id),
        Field("step_id", &RunStepRequest::step_id),
        Field("stage", &RunStepRequest::stage),
        Field("timeline_id", &RunStepRequest::timeline_id),
        Field("base_revision", &RunStepRequest::base_revision),
        Field("base_history_hash", &RunStepRequest::base_history_hash),
        Field("base_project_history_hash",
              &RunStepRequest::base_project_history_hash),
        Field("plan_json", &RunStepRequest::plan_json).AddedLater(),
        Field("observations_json", &RunStepRequest::observations_json)
            .AddedLater());
};

template <>
struct DocumentTable<RunStepReceipt> {
    static constexpr auto kFields = std::make_tuple(
        Field("format", &RunStepReceipt::format),
        Field("run_id", &RunStepReceipt::run_id),
        Field("step_id", &RunStepReceipt::step_id),
        Field("stage", &RunStepReceipt::stage),
        Field("project_id", &RunStepReceipt::project_id),
        Field("timeline_id", &RunStepReceipt::timeline_id),
        Field("snapshot_id", &RunStepReceipt::snapshot_id),
        Field("base_revision", &RunStepReceipt::base_revision),
        Field("result_revision", &RunStepReceipt::result_revision),
        Field("base_history_hash", &RunStepReceipt::base_history_hash),
        Field("timeline_history_json", &RunStepReceipt::timeline_history_json),
        Field("project_history_json", &RunStepReceipt::project_history_json),
        Field("request_json", &RunStepReceipt::request_json));
};

// DEC44 -- docs/roadmap/DONNEES-2026-10.md. A capture of the project archive
// is this manifest: the digests of its chunks and where each goes back
// (ArchiveManifest.h). Not a part of the montage either, but stored in the
// archive for good: its bytes are pinned like a project file's.
template <>
struct DocumentTable<ArchivePart> {
    static constexpr auto kFields = std::make_tuple(
        Field("kind", &ArchivePart::kind), Field("id", &ArchivePart::id),
        Field("at", &ArchivePart::at), Field("digest", &ArchivePart::digest));
};

template <>
struct DocumentTable<ArchiveManifest> {
    static constexpr auto kFields =
        std::make_tuple(Field("format", &ArchiveManifest::format),
                        Field("bytes", &ArchiveManifest::bytes),
                        Field("rest", &ArchiveManifest::rest),
                        Field("parts", &ArchiveManifest::parts));
};

template <>
struct EnumNames<TransitionAlignment> {
    static constexpr std::pair<TransitionAlignment, std::string_view> kNames[] =
        {
            {TransitionAlignment::Center, "center"},
            {TransitionAlignment::StartAtCut, "start_at_cut"},
            {TransitionAlignment::EndAtCut, "end_at_cut"},
        };
};

// R1 -- docs/roadmap/REEL-2026-09.md.
template <>
struct EnumNames<FramingMode> {
    static constexpr std::pair<FramingMode, std::string_view> kNames[] = {
        {FramingMode::Fit, "fit"},
        {FramingMode::Fill, "fill"},
    };
};

// R2 -- docs/roadmap/REEL-2026-09.md. The spelling the document, the
// journal and the MCP tool all use for a parameter, so that a file, a
// replayed operation and a tool call name the same thing the same way.
template <>
struct EnumNames<ClipAnimatedParameter> {
    static constexpr std::pair<ClipAnimatedParameter, std::string_view>
        kNames[] = {
            {ClipAnimatedParameter::Opacity, "opacity"},
            {ClipAnimatedParameter::FramingScale, "framing.scale"},
            {ClipAnimatedParameter::FramingPositionX, "framing.position_x"},
            {ClipAnimatedParameter::FramingPositionY, "framing.position_y"},
        };
};

// R2 -- Keyframes.h's three interpolations, spelled as CSS spells them, so
// that a model that already knows `cubic-bezier` needs no translation.
template <>
struct EnumNames<KeyframeInterpolation> {
    static constexpr std::pair<KeyframeInterpolation, std::string_view>
        kNames[] = {
            {KeyframeInterpolation::Hold, "hold"},
            {KeyframeInterpolation::Linear, "linear"},
            {KeyframeInterpolation::CubicBezier, "cubic-bezier"},
        };
};

// PL1 -- docs/roadmap/PLANS-2026-10.md. The same spellings the journal
// (OperationFormat.h) and the tools (ShotDescriptions.h) use.
template <>
struct EnumNames<ShotSize> {
    static constexpr std::pair<ShotSize, std::string_view> kNames[] = {
        {ShotSize::ExtremeWide, "extreme_wide"},
        {ShotSize::Wide, "wide"},
        {ShotSize::Full, "full"},
        {ShotSize::Medium, "medium"},
        {ShotSize::MediumClose, "medium_close"},
        {ShotSize::Close, "close"},
        {ShotSize::ExtremeClose, "extreme_close"},
        {ShotSize::Insert, "insert"},
    };
};

template <>
struct EnumNames<CameraMovement> {
    static constexpr std::pair<CameraMovement, std::string_view> kNames[] = {
        {CameraMovement::Static, "static"},
        {CameraMovement::Handheld, "handheld"},
        {CameraMovement::Pan, "pan"},
        {CameraMovement::Tilt, "tilt"},
        {CameraMovement::ZoomIn, "zoom_in"},
        {CameraMovement::ZoomOut, "zoom_out"},
        {CameraMovement::PushIn, "push_in"},
        {CameraMovement::PullOut, "pull_out"},
        {CameraMovement::Tracking, "tracking"},
        {CameraMovement::Crane, "crane"},
        {CameraMovement::Aerial, "aerial"},
    };
};

template <>
struct EnumNames<ShotQuality> {
    static constexpr std::pair<ShotQuality, std::string_view> kNames[] = {
        {ShotQuality::Good, "good"},
        {ShotQuality::Usable, "usable"},
        {ShotQuality::Poor, "poor"},
    };
};

template <>
struct EnumNames<ShotAuthorKind> {
    static constexpr std::pair<ShotAuthorKind, std::string_view> kNames[] = {
        {ShotAuthorKind::Model, "model"},
        {ShotAuthorKind::Person, "person"},
    };
};

// ---------------------------------------------------------------------
// Conditions.
// ---------------------------------------------------------------------

inline bool HasSyncAnchor(const DocumentClip& clip) {
    return !clip.sync_anchor_clip_id.empty();
}

// An entry promoted from a version-1 source was never probed: it has no
// codec, and none of the fields a probe fills in.
inline bool IsProbed(const LibraryMedia& media) {
    return media.metadata_complete;
}

inline bool IsProbedWithAudio(const LibraryMedia& media) {
    return media.metadata_complete && media.has_audio;
}

// ---------------------------------------------------------------------
// Tables.
// ---------------------------------------------------------------------

template <>
struct DocumentTable<MediaRate> {
    static constexpr auto kFields = std::make_tuple(
        Field("num", &MediaRate::num), Field("den", &MediaRate::den));
};

template <>
struct DocumentTable<EffectParamValue> {
    static constexpr auto kFields =
        std::make_tuple(Field("num", &EffectParamValue::num),
                        Field("den", &EffectParamValue::den));
};

// R1 -- docs/roadmap/REEL-2026-09.md. Written whole or not at all: the
// clip's `framing` key is what disappears for a clip that frames nothing
// (below), so once the object is there every field of it is spelled. A
// field this ticket has not thought of is AddedLater the day it exists.
// PA8 -- docs/roadmap/PARITE-2026-11.md. A clip's mask is written whole
// or not at all, as its framing is.
template <>
struct DocumentTable<ClipMask> {
    static constexpr auto kFields = std::make_tuple(
        Field("shape", &ClipMask::shape),
        Field("center_x", &ClipMask::center_x),
        Field("center_y", &ClipMask::center_y),
        Field("width", &ClipMask::width), Field("height", &ClipMask::height),
        Field("feather", &ClipMask::feather),
        Field("inverted", &ClipMask::inverted));
};

template <>
struct DocumentTable<ClipFraming> {
    static constexpr auto kFields = std::make_tuple(
        Field("mode", &ClipFraming::mode), Field("scale", &ClipFraming::scale),
        Field("position_x", &ClipFraming::position_x),
        Field("position_y", &ClipFraming::position_y),
        Field("crop_left", &ClipFraming::crop_left),
        Field("crop_right", &ClipFraming::crop_right),
        Field("crop_top", &ClipFraming::crop_top),
        Field("crop_bottom", &ClipFraming::crop_bottom));
};

template <>
struct DocumentTable<ColorManagementSettings> {
    static constexpr auto kFields = std::make_tuple(
        Field("enabled", &ColorManagementSettings::enabled),
        Field("input_gamut", &ColorManagementSettings::input_gamut),
        Field("input_transfer", &ColorManagementSettings::input_transfer),
        Field("input_ycbcr_matrix",
              &ColorManagementSettings::input_ycbcr_matrix),
        Field("input_range", &ColorManagementSettings::input_range)
            .AddedLater(),
        Field("working_gamut", &ColorManagementSettings::working_gamut),
        Field("output_gamut", &ColorManagementSettings::output_gamut),
        Field("output_transfer", &ColorManagementSettings::output_transfer));
};

template <>
struct DocumentTable<DocumentMarker> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentMarker::id), Field("name", &DocumentMarker::name),
        Field("time", &DocumentMarker::time),
        Field("color", &DocumentMarker::color),
        Field("category", &DocumentMarker::category),
        Field("duration", &DocumentMarker::duration).OmittedWhenDefault());
};

template <>
struct DocumentTable<DocumentTransition> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentTransition::id),
        Field("track_id", &DocumentTransition::track_id),
        Field("left_clip_id", &DocumentTransition::left_clip_id),
        Field("right_clip_id", &DocumentTransition::right_clip_id),
        Field("type", &DocumentTransition::type),
        Field("duration", &DocumentTransition::duration),
        Field("alignment", &DocumentTransition::alignment),
        // R5 -- written only when the dip is not black, so every project
        // whose transitions are plain dissolves keeps its bytes.
        Field("color", &DocumentTransition::color).OmittedWhenDefault());
};

template <>
struct DocumentTable<ClipEffect> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ClipEffect::id), Field("type", &ClipEffect::type),
        Field("params", &ClipEffect::params).Lines());
};

// R2 -- docs/roadmap/REEL-2026-09.md. One key, spelled whole. The value and
// the four easing control points are exact fractions and never decimals:
// the document parser refuses a floating-point number outright, and a key
// is what the author wrote, not the evaluation of a curve (ClipAnimation.h
// holds the whole reasoning). The easing points are omitted when they are
// the identity, so a linear or held key writes four keys fewer.
template <>
struct DocumentTable<ClipKeyframe> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ClipKeyframe::id), Field("time", &ClipKeyframe::time),
        Field("value", &ClipKeyframe::value),
        Field("interpolation", &ClipKeyframe::interpolation),
        Field("easing_x1", &ClipKeyframe::easing_x1).OmittedWhenDefault(),
        Field("easing_y1", &ClipKeyframe::easing_y1).OmittedWhenDefault(),
        Field("easing_x2", &ClipKeyframe::easing_x2).OmittedWhenDefault(),
        Field("easing_y2", &ClipKeyframe::easing_y2).OmittedWhenDefault());
};

// R2 -- every key of one parameter. An entry with fewer than two keys is
// refused by Document::Validate, so this list is never written empty.
template <>
struct DocumentTable<ClipParameterAnimation> {
    static constexpr auto kFields = std::make_tuple(
        Field("parameter", &ClipParameterAnimation::parameter),
        Field("keyframes", &ClipParameterAnimation::keyframes).Lines());
};

template <>
struct DocumentTable<DocumentClip> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentClip::id),
        Field("source_id", &DocumentClip::source_id),
        Field("source_in", &DocumentClip::source_in),
        Field("duration", &DocumentClip::duration),
        Field("timeline_in", &DocumentClip::timeline_in),
        Field("include_audio", &DocumentClip::include_audio).AddedLater(),
        Field("link_group_id", &DocumentClip::link_group_id).OmittedWhenEmpty(),
        Field("sync_anchor_clip_id", &DocumentClip::sync_anchor_clip_id)
            .OmittedWhenEmpty(),
        Field("sync_reference_delta", &DocumentClip::sync_reference_delta)
            .WrittenIf(&HasSyncAnchor),
        Field("effects", &DocumentClip::effects).Lines().AddedLater(),
        Field("caption_group_id", &DocumentClip::caption_group_id)
            .OmittedWhenEmpty(),
        Field("caption_text", &DocumentClip::caption_text).OmittedWhenEmpty(),
        // ALPHA-2026-08.
        Field("opacity", &DocumentClip::opacity).Since(5),
        // QC-2026-09 A7.
        Field("audio_gain_db", &DocumentClip::audio_gain_db).Since(7),
        Field("audio_fade_in", &DocumentClip::audio_fade_in).Since(7),
        Field("audio_fade_out", &DocumentClip::audio_fade_out).Since(7),
        // MOTION-2026-09 M4 -- spelled only on a motion clip, so a document
        // without one keeps every byte it had. Motion props are a motion
        // value, decimals as written (DocumentFormat.cc).
        Field("composition_id", &DocumentClip::composition_id)
            .OmittedWhenEmpty(),
        Field("motion_props", &DocumentClip::motion_props).OmittedWhenDefault(),
        // R3 -- written only on a clip that is not at 1/1, so a document
        // that uses no speed keeps its version and every byte it had.
        Field("speed", &DocumentClip::speed).OmittedWhenDefault(),
        // R3, criterion 4 -- written only on a clip whose speed is a ramp,
        // so a document without one keeps its version and every byte.
        Field("speed_ramp", &DocumentClip::speed_ramp)
            .Lines()
            .OmittedWhenDefault(),
        // R1 -- written only on a clip that is framed, so a document that
        // frames nothing keeps its version and every byte it had.
        Field("framing", &DocumentClip::framing).OmittedWhenDefault(),
        // R2 -- written only on a clip that carries a key, so a document
        // that animates nothing keeps its version and every byte it had.
        Field("animation", &DocumentClip::animation)
            .Lines()
            .OmittedWhenDefault(),
        // Out of lot -- the author, 29 October 2026. Written only on a disabled
        // clip, so a document that disables nothing keeps its version and
        // every byte it had.
        Field("enabled", &DocumentClip::enabled).OmittedWhenDefault(),
        // EF3 -- docs/roadmap/EFFETS-2026-10.md. Written only on a blurred
        // clip, so a document that blurs nothing keeps its version and
        // every byte it had.
        Field("blur", &DocumentClip::blur).OmittedWhenDefault(),
        // PA4 -- docs/roadmap/PARITE-2026-11.md. Written only on a carton
        // whose words are timed, so a document without one keeps its
        // version and every byte it had.
        Field("caption_word_times", &DocumentClip::caption_word_times)
            .OmittedWhenDefault(),
        // PA8 -- written only on a masked clip, so a document that masks
        // nothing keeps its version and every byte it had.
        Field("mask", &DocumentClip::mask).OmittedWhenDefault());
};

template <>
struct DocumentTable<DocumentTrack> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentTrack::id), Field("kind", &DocumentTrack::kind),
        Field("index", &DocumentTrack::index),
        Field("locked", &DocumentTrack::locked).AddedLater(),
        Field("sync_lock", &DocumentTrack::sync_lock).AddedLater(),
        Field("visible", &DocumentTrack::visible).AddedLater(),
        Field("muted", &DocumentTrack::muted).AddedLater(),
        Field("solo", &DocumentTrack::solo).AddedLater(),
        Field("clips", &DocumentTrack::clips).PaddedWhenEmpty());
};

template <>
struct DocumentTable<CaptionStyle> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &CaptionStyle::id),
        Field("font_family", &CaptionStyle::font_family),
        Field("font_size", &CaptionStyle::font_size),
        // Written only when it is not the regular: a style
        // written before weights existed reads back at 400.
        Field("font_weight", &CaptionStyle::font_weight).OmittedWhenDefault(),
        Field("color", &CaptionStyle::color),
        Field("position", &CaptionStyle::position),
        // ITW4 -- written only when set: an undecorated
        // style keeps its bytes, and one written before
        // decoration existed reads back undecorated.
        Field("outline_width", &CaptionStyle::outline_width)
            .OmittedWhenDefault(),
        Field("outline_color", &CaptionStyle::outline_color)
            .OmittedWhenDefault(),
        Field("shadow_offset", &CaptionStyle::shadow_offset)
            .OmittedWhenDefault(),
        Field("shadow_color", &CaptionStyle::shadow_color).OmittedWhenDefault(),
        Field("background_color", &CaptionStyle::background_color)
            .OmittedWhenDefault(),
        Field("background_padding", &CaptionStyle::background_padding)
            .OmittedWhenDefault(),
        Field("vertical_position", &CaptionStyle::vertical_position)
            .OmittedWhenDefault(),
        // PA4 -- written only on a style that animates its words: every
        // other style keeps its bytes.
        Field("word_animation", &CaptionStyle::word_animation)
            .OmittedWhenDefault(),
        Field("highlight_color", &CaptionStyle::highlight_color)
            .OmittedWhenDefault());
};

template <>
struct DocumentTable<MulticamAngle> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &MulticamAngle::id), Field("name", &MulticamAngle::name),
        Field("clip_id", &MulticamAngle::clip_id));
};

// R6 -- docs/roadmap/REEL-2026-09.md. Written whole or not at all, as R1's
// framing is: the sequence's `look_lut` key is what disappears for a
// sequence without one, so once the object is there every field of it is
// spelled. Not one of them is a number, which is the whole point of the
// struct (Document.h).
template <>
struct DocumentTable<SequenceLut> {
    static constexpr auto kFields = std::make_tuple(
        Field("digest", &SequenceLut::digest),
        Field("name", &SequenceLut::name), Field("space", &SequenceLut::space));
};

template <>
struct DocumentTable<MulticamGroup> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &MulticamGroup::id), Field("name", &MulticamGroup::name),
        Field("angles", &MulticamGroup::angles).Lines(),
        Field("active_angle_id", &MulticamGroup::active_angle_id)
            .OmittedWhenEmpty());
};

template <>
struct DocumentTable<DocumentSequence> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentSequence::id),
        Field("name", &DocumentSequence::name),
        Field("width", &DocumentSequence::width),
        Field("height", &DocumentSequence::height),
        Field("frame_rate", &DocumentSequence::frame_rate),
        Field("markers", &DocumentSequence::markers)
            .PaddedWhenEmpty()
            .AddedLater(),
        Field("transitions", &DocumentSequence::transitions)
            .PaddedWhenEmpty()
            .AddedLater(),
        Field("tracks", &DocumentSequence::tracks).PaddedWhenEmpty(),
        Field("caption_styles", &DocumentSequence::caption_styles)
            .PaddedWhenEmpty()
            .AddedLater(),
        Field("multicam_groups", &DocumentSequence::multicam_groups)
            .PaddedWhenEmpty()
            .AddedLater(),
        // R6 -- written only on a sequence that carries a look, so a
        // document without one keeps its version and every byte it had.
        Field("look_lut", &DocumentSequence::look_lut).OmittedWhenDefault());
};

// G3 -- docs/roadmap/GRADE-2026-09.md.
template <>
struct DocumentTable<MediaColorInput> {
    // DEC31 -- `graphics` written only when true: every input written
    // before it keeps its bytes.
    static constexpr auto kFields = std::make_tuple(
        Field("gamut", &MediaColorInput::gamut),
        Field("transfer", &MediaColorInput::transfer),
        Field("ycbcr_matrix", &MediaColorInput::ycbcr_matrix),
        Field("range", &MediaColorInput::range),
        Field("graphics", &MediaColorInput::graphics).OmittedWhenDefault());
};

// PL1 -- docs/roadmap/PLANS-2026-10.md. What a description leaves empty is
// not written: an insert has no place, a good shot no reserve.
template <>
struct DocumentTable<ShotDescription> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ShotDescription::id),
        Field("source_in", &ShotDescription::source_in),
        Field("duration", &ShotDescription::duration),
        Field("size", &ShotDescription::size),
        Field("subjects", &ShotDescription::subjects).OmittedWhenDefault(),
        Field("action", &ShotDescription::action).OmittedWhenEmpty(),
        Field("place", &ShotDescription::place).OmittedWhenEmpty(),
        Field("movement", &ShotDescription::movement),
        Field("quality", &ShotDescription::quality),
        Field("reserve", &ShotDescription::reserve).OmittedWhenEmpty(),
        Field("keywords", &ShotDescription::keywords).OmittedWhenDefault(),
        Field("author_kind", &ShotDescription::author_kind),
        Field("author", &ShotDescription::author),
        Field("described_at", &ShotDescription::described_at));
};

// GN3 -- docs/roadmap/GENERATIF-2026-10.md. What every provenance names
// is always written; what a generation may lack only when it has it.
template <>
struct DocumentTable<MediaGeneration> {
    static constexpr auto kFields = std::make_tuple(
        Field("service", &MediaGeneration::service),
        Field("model", &MediaGeneration::model),
        Field("kind", &MediaGeneration::kind),
        Field("prompt", &MediaGeneration::prompt).OmittedWhenEmpty(),
        Field("seed", &MediaGeneration::seed).OmittedWhenEmpty(),
        Field("created_at", &MediaGeneration::created_at),
        Field("announced_cost", &MediaGeneration::announced_cost)
            .OmittedWhenEmpty(),
        Field("request_id", &MediaGeneration::request_id).OmittedWhenEmpty(),
        Field("source_media_id", &MediaGeneration::source_media_id)
            .OmittedWhenEmpty(),
        // GN6b -- only a voice design_voice made says one.
        Field("reference_text", &MediaGeneration::reference_text)
            .OmittedWhenEmpty());
};

template <>
struct DocumentTable<LibraryMedia> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &LibraryMedia::id), Field("path", &LibraryMedia::path),
        Field("filename", &LibraryMedia::filename),
        Field("bin_id", &LibraryMedia::bin_id).OmittedWhenEmpty(),
        Field("proxy_path", &LibraryMedia::proxy_path).OmittedWhenEmpty(),
        Field("codec", &LibraryMedia::codec)
            .PresenceRecordedIn(&LibraryMedia::metadata_complete),
        CarriedBy("codec", &LibraryMedia::metadata_complete),
        // B11 -- entries probed before audio-only media say nothing.
        Field("has_video", &LibraryMedia::has_video)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("width", &LibraryMedia::width).WrittenIf(&IsProbed),
        Field("height", &LibraryMedia::height).WrittenIf(&IsProbed),
        Field("rotation_degrees", &LibraryMedia::rotation_degrees)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("pixel_format", &LibraryMedia::pixel_format)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("color_range", &LibraryMedia::color_range)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("color_space", &LibraryMedia::color_space)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("color_transfer", &LibraryMedia::color_transfer)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        Field("color_primaries", &LibraryMedia::color_primaries)
            .WrittenIf(&IsProbed)
            .AddedLater(),
        // G7 -- written only when known, so a media no camera told its white
        // balance keeps every byte it had.
        Field("white_balance_kelvin", &LibraryMedia::white_balance_kelvin)
            .OmittedWhenDefault()
            .AddedLater(),
        // G3 -- written only for a media that declares its own input.
        Field("color_input", &LibraryMedia::color_input)
            .OmittedWhenDefault()
            .AddedLater(),
        Field("rate", &LibraryMedia::rate),
        Field("duration", &LibraryMedia::duration),
        Field("orientation", &LibraryMedia::orientation).WrittenIf(&IsProbed),
        Field("has_audio", &LibraryMedia::has_audio).WrittenIf(&IsProbed),
        Field("audio_rate", &LibraryMedia::audio_rate)
            .WrittenIf(&IsProbedWithAudio),
        Field("audio_channels", &LibraryMedia::audio_channels)
            .WrittenIf(&IsProbedWithAudio),
        // QC-2026-09 A3 -- present exactly when it was measured, so an entry
        // no measuring ingest has seen keeps the bytes it had before.
        Field("audio_level", &LibraryMedia::audio_level)
            .WrittenIf(&IsProbedWithAudio)
            .PresenceRecordedIn(&LibraryMedia::audio_level_measured),
        CarriedBy("audio_level", &LibraryMedia::audio_level_measured),
        // PL1 -- written only for a described media, one shot per line, so
        // a library without a description keeps every byte it had.
        Field("shots", &LibraryMedia::shots).OmittedWhenDefault().Lines(),
        // GN3 -- written only for a media a service generated, so a library
        // of filmed media keeps every byte it had.
        Field("generation", &LibraryMedia::generation)
            .OmittedWhenDefault()
            .AddedLater());
};

template <>
struct DocumentTable<DocumentBin> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentBin::id), Field("name", &DocumentBin::name),
        Field("parent_id", &DocumentBin::parent_id).OmittedWhenEmpty());
};

template <>
struct DocumentTable<DocumentSource> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentSource::id), Field("path", &DocumentSource::path),
        Field("rate", &DocumentSource::rate),
        Field("duration", &DocumentSource::duration));
};

// MOTION-2026-09 M3 -- a composition of the project. Its definition is a
// motion tree, written as the JSON it is (Document.h); the only part of a
// document where a number may be a decimal.
template <>
struct DocumentTable<DocumentComposition> {
    static constexpr auto kFields =
        std::make_tuple(Field("id", &DocumentComposition::id),
                        Field("name", &DocumentComposition::name),
                        Field("definition", &DocumentComposition::definition));
};

// A project file keys each entry by its item under "item_id"; a delta keys
// it beside the entry.
template <>
struct DocumentTable<ProjectBinMetadata> {
    static constexpr auto kFields = std::make_tuple(
        Field("description", &ProjectBinMetadata::description),
        Field("rating", &ProjectBinMetadata::rating),
        Field("tags", &ProjectBinMetadata::tags),
        Field("insert_order", &ProjectBinMetadata::insert_order),
        // F2.1.
        Field("display_name", &ProjectBinMetadata::display_name).AddedLater());
};

}  // namespace document_format
