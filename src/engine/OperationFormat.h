#pragma once

// FORMAT-2026-09 -- ROADMAP.md. The journal format of every operation,
// described once.
//
// Edit logs and project journals store operations as positional JSON. Each
// struct used to be described three times -- its declaration, a writer and a
// reader -- and nothing tied the three together. QC-2026-09 A7 and A3 were
// what that cost: DocumentClip and LibraryMedia gained members that their
// journal writers never learned, so undoing from a reloaded journal reset a
// clip's gain and fades, and a relink read back without its measured level.
//
// A FieldTable names each serialized member once, in the order the format
// writes it, with how it is present:
//
//   Field(key, member)                      always written, required on read
//     .AddedLater()                         absent from journals written
//                                           before the member existed
//     .OmittedWhenDefault()                 written only when it differs from
//                                           the member's in-class default
//     .WrittenIf(predicate)                 written when predicate(owner)
//                                           holds; required on read when it
//                                           holds for the members read so far
//     .AsInteger()                          a bool spelled 1/0
//     .Custom(write, read)                  a shape no presence above
//                                           describes; the codec writes its
//                                           own key
//   CarriedBy(key, member)                  no key of its own: the Custom
//                                           codec of `key` encodes it
//
// Both directions of the codec (OperationFormat.cc) are derived from the
// table, and CheckTable makes every table complete by construction: as many
// entries as the aggregate has members, no member twice, no key twice. A
// member added to a tabled struct is a compile error until its presence in
// the journal is decided.
//
// Internal to the journal codec and its tests. Nothing else should include
// this header: the public API stays SerializeOperation and friends in
// Operations.h.

#include "AggregateMembers.h"
#include "EditMotif.h"
#include "Operations.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace operation_format {

class Reader;

enum class Presence {
    Required,
    AddedLater,
    OmittedWhenDefault,
    WrittenIf,
};

// How a field is spelled. Part of the field's type, so that a member whose
// shape only a custom codec knows never needs a generic one.
enum class Kind {
    Value,
    Custom,
    Carried,
};

enum class Encoding {
    Natural,
    BoolAsInteger,
};

template <typename Owner, typename Member, Kind kind = Kind::Value>
struct FieldSpec {
    static constexpr Kind kKind = kind;
    using OwnerType = Owner;
    using MemberType = Member;

    std::string_view key;
    Member Owner::*member = nullptr;
    Presence presence = Presence::Required;
    Encoding encoding = Encoding::Natural;
    bool (*condition)(const Owner&) = nullptr;
    // A custom codec writes and reads whole `,"key":value` segments, leading
    // comma included when `comma` is true, and sets `comma` once it has
    // written or read one.
    void (*write)(std::ostringstream&, const Owner&, bool& comma) = nullptr;
    void (*read)(Reader&, Owner&, bool& comma) = nullptr;

    constexpr FieldSpec AddedLater() const {
        FieldSpec copy = *this;
        copy.presence = Presence::AddedLater;
        return copy;
    }

    constexpr FieldSpec OmittedWhenDefault() const {
        FieldSpec copy = *this;
        copy.presence = Presence::OmittedWhenDefault;
        return copy;
    }

    constexpr FieldSpec WrittenIf(bool (*predicate)(const Owner&)) const {
        FieldSpec copy = *this;
        copy.presence = Presence::WrittenIf;
        copy.condition = predicate;
        return copy;
    }

    constexpr FieldSpec AsInteger() const {
        static_assert(std::is_same_v<Member, bool>,
                      "AsInteger spells a bool as 1/0");
        FieldSpec copy = *this;
        copy.encoding = Encoding::BoolAsInteger;
        return copy;
    }

    constexpr FieldSpec<Owner, Member, Kind::Custom> Custom(
        void (*writer)(std::ostringstream&, const Owner&, bool&),
        void (*reader)(Reader&, Owner&, bool&)) const {
        FieldSpec<Owner, Member, Kind::Custom> copy{key, member};
        copy.write = writer;
        copy.read = reader;
        return copy;
    }
};

template <typename Owner, typename Member>
constexpr FieldSpec<Owner, Member> Field(std::string_view key,
                                         Member Owner::*member) {
    return {key, member};
}

// A member with no key of its own: the custom codec of `key` encodes it.
template <typename Owner, typename Member>
constexpr FieldSpec<Owner, Member, Kind::Carried> CarriedBy(
    std::string_view key, Member Owner::*member) {
    return {key, member};
}

// Specialized once per serialized struct. `kFields` lists its members;
// `kType` is present only on an operation, whose object starts with a
// "type" tag that nested structs do not carry.
template <typename T>
struct FieldTable {};

// Specialized once per serialized enum: its spelling in the journal, and
// what to call it in a parse error.
template <typename Enum>
struct EnumNames {};

template <typename T, typename = void>
struct IsTabled : std::false_type {};

template <typename T>
struct IsTabled<T, std::void_t<decltype(FieldTable<T>::kFields)>>
    : std::true_type {};

template <typename T, typename = void>
struct IsTagged : std::false_type {};

template <typename T>
struct IsTagged<T, std::void_t<decltype(FieldTable<T>::kType)>>
    : std::true_type {};

template <typename T, typename Visit>
void ForEachField(Visit&& visit) {
    std::apply([&](const auto&... fields) { (visit(fields), ...); },
               FieldTable<T>::kFields);
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
    return Candidate::kKind == Kind::Custom && candidate.key == carried.key;
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
        FieldTable<T>::kFields);
}

template <typename T>
constexpr bool CarriedFieldsHaveCarriers() {
    return std::apply(
        [](const auto&... fields) {
            const auto carried = [&](const auto& candidate) {
                using Candidate = std::decay_t<decltype(candidate)>;
                return Candidate::kKind != Kind::Carried ||
                       (false || ... || Carries(candidate, fields));
            };
            return (carried(fields) && ...);
        },
        FieldTable<T>::kFields);
}

template <typename T>
constexpr void CheckTable() {
    static_assert(std::tuple_size_v<decltype(FieldTable<T>::kFields)> ==
                      AggregateMemberCount<T>::value,
                  "a journal struct needs exactly one FieldTable entry per "
                  "member");
    static_assert(FieldsAreDistinct<T>(),
                  "a FieldTable names the same member or key twice");
    static_assert(CarriedFieldsHaveCarriers<T>(),
                  "a CarriedBy entry names no Custom field of its table");
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

template <typename T>
constexpr std::string_view TypeTagOf() {
    if constexpr (IsTagged<T>::value) {
        return FieldTable<T>::kType;
    } else {
        return {};
    }
}

template <typename... Alternatives>
constexpr bool TypeTagsAreDistinct(const std::variant<Alternatives...>*) {
    const std::string_view tags[] = {TypeTagOf<Alternatives>()...};
    for (size_t left = 0; left < sizeof...(Alternatives); ++left) {
        for (size_t right = left + 1; right < sizeof...(Alternatives);
             ++right) {
            if (tags[left] == tags[right]) return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------
// Structural equality. Deliberately not RationalTime::operator==, which
// compares rationals: a {0, 25} fade is not a {0, 1} fade on disk, and
// writing one as the other would lose its rate. Used by the codec to decide
// whether a member still holds the default its struct declares, and by the
// tests to compare an operation with what the journal gives back.
// ---------------------------------------------------------------------

bool SameValue(const std::string& left, const std::string& right);
bool SameValue(bool left, bool right);
bool SameValue(int32_t left, int32_t right);
bool SameValue(int64_t left, int64_t right);
bool SameValue(uint32_t left, uint32_t right);
bool SameValue(uint64_t left, uint64_t right);
bool SameValue(const RationalTime& left, const RationalTime& right);
bool SameValue(const mcp_json::Value& left, const mcp_json::Value& right);
bool SameValue(const ProjectDelta& left, const ProjectDelta& right);
bool SameValue(const ExactProjectState& left, const ExactProjectState& right);
bool SameValue(const Operation& left, const Operation& right);
template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int> = 0>
bool SameValue(Enum left, Enum right);
template <typename T>
bool SameValue(const std::vector<T>& left, const std::vector<T>& right);
template <typename T>
bool SameValue(const std::optional<T>& left, const std::optional<T>& right);
template <typename V>
bool SameValue(const std::map<std::string, V>& left,
               const std::map<std::string, V>& right);
template <typename T, std::enable_if_t<IsTabled<T>::value, int> = 0>
bool SameValue(const T& left, const T& right);

inline bool SameValue(const std::string& left, const std::string& right) {
    return left == right;
}
inline bool SameValue(bool left, bool right) { return left == right; }
inline bool SameValue(int32_t left, int32_t right) { return left == right; }
inline bool SameValue(int64_t left, int64_t right) { return left == right; }
inline bool SameValue(uint32_t left, uint32_t right) { return left == right; }
inline bool SameValue(uint64_t left, uint64_t right) { return left == right; }

inline bool SameValue(const RationalTime& left, const RationalTime& right) {
    return left.value == right.value && left.rate == right.rate;
}

// MOTION-2026-09 M3 -- a motion tree is compared as the journal spells it:
// key order and number spelling included.
inline bool SameValue(const mcp_json::Value& left,
                      const mcp_json::Value& right) {
    return left.Dump() == right.Dump();
}

template <typename Enum, std::enable_if_t<std::is_enum_v<Enum>, int>>
bool SameValue(Enum left, Enum right) {
    return left == right;
}

// A delta has its own canonical text; comparing it is comparing that.
inline bool SameValue(const ProjectDelta& left, const ProjectDelta& right) {
    return SerializeProjectDelta(left) == SerializeProjectDelta(right);
}

inline bool SameValue(const ExactProjectState& left,
                      const ExactProjectState& right) {
    return left.canonical_json == right.canonical_json &&
           SameValue(left.delta, right.delta);
}

// PL1 -- a batch member is compared as the journal spells it, type tag
// included, which is what a batch has to preserve.
inline bool SameValue(const Operation& left, const Operation& right) {
    return SerializeOperation(left) == SerializeOperation(right);
}

template <typename T>
inline bool SameValue(const std::vector<T>& left, const std::vector<T>& right) {
    if (left.size() != right.size()) return false;
    for (size_t index = 0; index < left.size(); ++index) {
        if (!SameValue(left[index], right[index])) return false;
    }
    return true;
}

template <typename T>
inline bool SameValue(const std::optional<T>& left,
                      const std::optional<T>& right) {
    if (left.has_value() != right.has_value()) return false;
    return !left || SameValue(*left, *right);
}

template <typename V>
inline bool SameValue(const std::map<std::string, V>& left,
                      const std::map<std::string, V>& right) {
    if (left.size() != right.size()) return false;
    auto other = right.begin();
    for (auto entry = left.begin(); entry != left.end(); ++entry, ++other) {
        if (entry->first != other->first) return false;
        if (!SameValue(entry->second, other->second)) return false;
    }
    return true;
}

template <typename T, std::enable_if_t<IsTabled<T>::value, int>>
inline bool SameValue(const T& left, const T& right) {
    bool same = true;
    ForEachField<T>([&](const auto& field) {
        if (!same) return;
        same = SameValue(left.*field.member, right.*field.member);
    });
    return same;
}

// ---------------------------------------------------------------------
// Custom codecs, defined in OperationFormat.cc.
// ---------------------------------------------------------------------

// AddTrack's clips are spelled as a one-track exact state keyed by the
// operation's own track, and only when there are any.
void WriteAddTrackClips(std::ostringstream& output,
                        const AddTrackOperation& value, bool& comma);
void ReadAddTrackClips(Reader& reader, AddTrackOperation& value, bool& comma);

// PL1 -- ROADMAP.md. A batch's members are whole operations, each with its
// type tag, read by the codec of a journal entry. A member that is itself a
// batch is refused.
void WriteBatchMembers(std::ostringstream& output, const BatchOperation& value,
                       bool& comma);
void ReadBatchMembers(Reader& reader, BatchOperation& value, bool& comma);

// QC-2026-09 A3 -- the level is present exactly when it was measured, the
// convention Document.cc uses for the same member.
void WriteMeasuredAudioLevel(std::ostringstream& output,
                             const LibraryMedia& value, bool& comma);
void ReadMeasuredAudioLevel(Reader& reader, LibraryMedia& value, bool& comma);

// S6 -- SAVING_ROADMAP.md. A delta is written under its own key, so that an
// operation without exact state keeps the `"exact_project_hex":null` every
// caller and document already spells.
void WriteExactProject(std::ostringstream& output,
                       const std::optional<ExactProjectState>& exact,
                       bool& comma);
void ReadExactProject(Reader& reader, std::optional<ExactProjectState>& exact,
                      bool& comma);

template <typename Owner>
void WriteExactProjectOf(std::ostringstream& output, const Owner& value,
                         bool& comma) {
    WriteExactProject(output, value.exact_project_result, comma);
}

template <typename Owner>
void ReadExactProjectOf(Reader& reader, Owner& value, bool& comma) {
    ReadExactProject(reader, value.exact_project_result, comma);
}

template <typename Owner>
constexpr auto ExactProjectField() {
    return Field("exact_project_hex", &Owner::exact_project_result)
        .Custom(&WriteExactProjectOf<Owner>, &ReadExactProjectOf<Owner>);
}

inline bool HasSyncAnchor(const DocumentClip& clip) {
    return !clip.sync_anchor_clip_id.empty();
}

// ---------------------------------------------------------------------
// Enums.
// ---------------------------------------------------------------------

template <>
struct EnumNames<TrimEdge> {
    static constexpr std::string_view kWhat = "trim edge";
    static constexpr std::pair<TrimEdge, std::string_view> kNames[] = {
        {TrimEdge::Head, "Head"},
        {TrimEdge::Tail, "Tail"},
    };
};

// R1 -- docs/roadmap/REEL-2026-09.md.
template <>
struct EnumNames<FramingMode> {
    static constexpr std::string_view kWhat = "framing mode";
    static constexpr std::pair<FramingMode, std::string_view> kNames[] = {
        {FramingMode::Fit, "fit"},
        {FramingMode::Fill, "fill"},
    };
};

// R2 -- docs/roadmap/REEL-2026-09.md. The same spellings the document and
// the MCP tool use (DocumentFormat.h, ClipAnimation.h).
template <>
struct EnumNames<ClipAnimatedParameter> {
    static constexpr std::string_view kWhat = "animated parameter";
    static constexpr std::pair<ClipAnimatedParameter, std::string_view>
        kNames[] = {
            {ClipAnimatedParameter::Opacity, "opacity"},
            {ClipAnimatedParameter::FramingScale, "framing.scale"},
            {ClipAnimatedParameter::FramingPositionX, "framing.position_x"},
            {ClipAnimatedParameter::FramingPositionY, "framing.position_y"},
        };
};

template <>
struct EnumNames<KeyframeInterpolation> {
    static constexpr std::string_view kWhat = "keyframe interpolation";
    static constexpr std::pair<KeyframeInterpolation, std::string_view>
        kNames[] = {
            {KeyframeInterpolation::Hold, "hold"},
            {KeyframeInterpolation::Linear, "linear"},
            {KeyframeInterpolation::CubicBezier, "cubic-bezier"},
        };
};

// PL1 -- docs/roadmap/PLANS-2026-10.md. The same spellings the document
// (DocumentFormat.h) and the tools (ShotDescriptions.h) use.
template <>
struct EnumNames<ShotSize> {
    static constexpr std::string_view kWhat = "shot size";
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
    static constexpr std::string_view kWhat = "camera movement";
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
    static constexpr std::string_view kWhat = "shot quality";
    static constexpr std::pair<ShotQuality, std::string_view> kNames[] = {
        {ShotQuality::Good, "good"},
        {ShotQuality::Usable, "usable"},
        {ShotQuality::Poor, "poor"},
    };
};

template <>
struct EnumNames<ShotAuthorKind> {
    static constexpr std::string_view kWhat = "shot author kind";
    static constexpr std::pair<ShotAuthorKind, std::string_view> kNames[] = {
        {ShotAuthorKind::Model, "model"},
        {ShotAuthorKind::Person, "person"},
    };
};

template <>
struct EnumNames<TransitionAlignment> {
    static constexpr std::string_view kWhat = "transition alignment";
    static constexpr std::pair<TransitionAlignment, std::string_view> kNames[] =
        {
            {TransitionAlignment::Center, "center"},
            {TransitionAlignment::StartAtCut, "start_at_cut"},
            {TransitionAlignment::EndAtCut, "end_at_cut"},
        };
};

// ---------------------------------------------------------------------
// Values nested in operations.
// ---------------------------------------------------------------------

template <>
struct FieldTable<MediaRate> {
    static constexpr auto kFields = std::make_tuple(
        Field("num", &MediaRate::num), Field("den", &MediaRate::den));
};

template <>
struct FieldTable<EffectParamValue> {
    static constexpr auto kFields =
        std::make_tuple(Field("num", &EffectParamValue::num),
                        Field("den", &EffectParamValue::den));
};

// R1 -- docs/roadmap/REEL-2026-09.md. Every field required: a framing the
// journal spells at all it spells whole, so nothing about it can be lost
// by omission the way a clip's gain once was (QC-2026-09 A7). The clip's
// own `framing` key is what disappears for an unframed clip.
template <>
struct FieldTable<ClipMask> {
    static constexpr auto kFields = std::make_tuple(
        Field("shape", &ClipMask::shape),
        Field("center_x", &ClipMask::center_x),
        Field("center_y", &ClipMask::center_y),
        Field("width", &ClipMask::width), Field("height", &ClipMask::height),
        Field("feather", &ClipMask::feather),
        Field("inverted", &ClipMask::inverted));
};

template <>
struct FieldTable<ClipFraming> {
    static constexpr auto kFields = std::make_tuple(
        Field("mode", &ClipFraming::mode), Field("scale", &ClipFraming::scale),
        Field("position_x", &ClipFraming::position_x),
        Field("position_y", &ClipFraming::position_y),
        Field("crop_left", &ClipFraming::crop_left),
        Field("crop_right", &ClipFraming::crop_right),
        Field("crop_top", &ClipFraming::crop_top),
        Field("crop_bottom", &ClipFraming::crop_bottom));
};

// R6 -- docs/roadmap/REEL-2026-09.md. Every field required, for the reason
// ClipFraming's table gives: a look the journal spells at all it spells
// whole. Three strings and not one number -- the numbers are the file the
// digest names (Document.h).
template <>
struct FieldTable<SequenceLut> {
    static constexpr auto kFields = std::make_tuple(
        Field("digest", &SequenceLut::digest),
        Field("name", &SequenceLut::name), Field("space", &SequenceLut::space));
};

template <>
struct FieldTable<ClipFramingAssignment> {
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &ClipFramingAssignment::clip_id),
                        Field("framing", &ClipFramingAssignment::framing));
};

// R2 -- docs/roadmap/REEL-2026-09.md. A key spelled whole, value and
// easing as exact fractions: a journal line is replayed on another machine
// and a decimal would not come back the same. The easing points are
// omitted when they are the identity, which is every key that is not a
// cubic-bezier.
template <>
struct FieldTable<ClipKeyframe> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ClipKeyframe::id), Field("time", &ClipKeyframe::time),
        Field("value", &ClipKeyframe::value),
        Field("interpolation", &ClipKeyframe::interpolation),
        Field("easing_x1", &ClipKeyframe::easing_x1).OmittedWhenDefault(),
        Field("easing_y1", &ClipKeyframe::easing_y1).OmittedWhenDefault(),
        Field("easing_x2", &ClipKeyframe::easing_x2).OmittedWhenDefault(),
        Field("easing_y2", &ClipKeyframe::easing_y2).OmittedWhenDefault());
};

template <>
struct FieldTable<ClipParameterAnimation> {
    static constexpr auto kFields =
        std::make_tuple(Field("parameter", &ClipParameterAnimation::parameter),
                        Field("keyframes", &ClipParameterAnimation::keyframes));
};

// Out of lot -- the author, 29 October 2026.
template <>
struct FieldTable<ClipEnabledAssignment> {
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &ClipEnabledAssignment::clip_id),
                        Field("enabled", &ClipEnabledAssignment::enabled));
};

template <>
struct FieldTable<ClipAnimationAssignment> {
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &ClipAnimationAssignment::clip_id),
        Field("animation", &ClipAnimationAssignment::animation));
};

template <>
struct FieldTable<ExactTimelinePosition> {
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &ExactTimelinePosition::clip_id),
        Field("timeline_in", &ExactTimelinePosition::timeline_in));
};

template <>
struct FieldTable<ExactClipTimes> {
    static constexpr auto kFields =
        std::make_tuple(Field("source_in", &ExactClipTimes::source_in),
                        Field("duration", &ExactClipTimes::duration),
                        Field("timeline_in", &ExactClipTimes::timeline_in));
};

template <>
struct FieldTable<ClipEffect> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ClipEffect::id), Field("type", &ClipEffect::type),
        Field("params", &ClipEffect::params));
};

template <>
struct FieldTable<DocumentClip> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentClip::id),
        Field("source_id", &DocumentClip::source_id),
        Field("source_in", &DocumentClip::source_in),
        Field("duration", &DocumentClip::duration),
        Field("timeline_in", &DocumentClip::timeline_in),
        Field("include_audio", &DocumentClip::include_audio).AddedLater(),
        Field("link_group_id", &DocumentClip::link_group_id)
            .OmittedWhenDefault(),
        Field("sync_anchor_clip_id", &DocumentClip::sync_anchor_clip_id)
            .OmittedWhenDefault(),
        Field("sync_reference_delta", &DocumentClip::sync_reference_delta)
            .WrittenIf(&HasSyncAnchor),
        Field("effects", &DocumentClip::effects),
        Field("caption_group_id", &DocumentClip::caption_group_id)
            .OmittedWhenDefault(),
        Field("caption_text", &DocumentClip::caption_text).OmittedWhenDefault(),
        // ALPHA-2026-08 -- snapshots written before opacity mean opaque.
        Field("opacity", &DocumentClip::opacity).AddedLater(),
        // QC-2026-09 A7.
        Field("audio_gain_db", &DocumentClip::audio_gain_db)
            .OmittedWhenDefault(),
        Field("audio_fade_in", &DocumentClip::audio_fade_in)
            .OmittedWhenDefault(),
        Field("audio_fade_out", &DocumentClip::audio_fade_out)
            .OmittedWhenDefault(),
        // MOTION-2026-09 M4 -- spelled only on a motion clip: every snapshot
        // of a media clip keeps its bytes.
        Field("composition_id", &DocumentClip::composition_id)
            .OmittedWhenDefault(),
        Field("motion_props", &DocumentClip::motion_props).OmittedWhenDefault(),
        // R3 -- spelled only on a clip that is not at 1/1: every snapshot
        // of an ordinary clip keeps its bytes.
        Field("speed", &DocumentClip::speed).OmittedWhenDefault(),
        // R3, criterion 4 -- spelled only on a ramped clip: every snapshot
        // of a clip at one speed keeps its bytes.
        Field("speed_ramp", &DocumentClip::speed_ramp).OmittedWhenDefault(),
        // R1 -- spelled only on a framed clip: every snapshot of an
        // unframed one keeps its bytes.
        Field("framing", &DocumentClip::framing).OmittedWhenDefault(),
        // R2 -- spelled only on a clip that carries a key: every snapshot
        // of a still one keeps its bytes.
        Field("animation", &DocumentClip::animation).OmittedWhenDefault(),
        // Out of lot -- the author, 29 October 2026. Spelled only on a disabled
        // clip: every snapshot of an enabled one keeps its bytes.
        Field("enabled", &DocumentClip::enabled).OmittedWhenDefault(),
        // EF3 -- spelled only on a blurred clip: every snapshot of a sharp
        // one keeps its bytes.
        Field("blur", &DocumentClip::blur).OmittedWhenDefault(),
        // PA4 -- spelled only on a carton whose words are timed: every
        // snapshot of another clip keeps its bytes.
        Field("caption_word_times", &DocumentClip::caption_word_times)
            .OmittedWhenDefault(),
        // PA8 -- spelled only on a masked clip: every snapshot of another
        // clip keeps its bytes.
        Field("mask", &DocumentClip::mask).OmittedWhenDefault());
};

template <>
struct FieldTable<ExactTrackState> {
    static constexpr auto kFields =
        std::make_tuple(Field("track_id", &ExactTrackState::track_id),
                        Field("clips", &ExactTrackState::clips));
};

template <>
struct FieldTable<PastedClip> {
    static constexpr auto kFields = std::make_tuple(
        Field("copied_clip_id", &PastedClip::copied_clip_id),
        Field("track_id", &PastedClip::track_id),
        Field("source_id", &PastedClip::source_id),
        Field("source_in", &PastedClip::source_in),
        Field("duration", &PastedClip::duration),
        Field("timeline_in", &PastedClip::timeline_in),
        Field("clip_id", &PastedClip::clip_id),
        Field("copied_link_group_id", &PastedClip::copied_link_group_id),
        Field("link_group_id", &PastedClip::link_group_id),
        Field("copied_sync_anchor_clip_id",
              &PastedClip::copied_sync_anchor_clip_id),
        Field("sync_anchor_clip_id", &PastedClip::sync_anchor_clip_id),
        Field("sync_reference_delta", &PastedClip::sync_reference_delta));
};

template <>
struct FieldTable<LinkedClipMove> {
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &LinkedClipMove::clip_id),
                        Field("track_id", &LinkedClipMove::track_id),
                        Field("timeline_in", &LinkedClipMove::timeline_in));
};

template <>
struct FieldTable<LinkedClipTrim> {
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &LinkedClipTrim::clip_id),
                        Field("edge", &LinkedClipTrim::edge),
                        Field("delta", &LinkedClipTrim::delta));
};

template <>
struct FieldTable<BoundaryAirTrim> {
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &BoundaryAirTrim::clip_id),
        Field("edge", &BoundaryAirTrim::edge),
        Field("delta", &BoundaryAirTrim::delta),
        Field("linked_clip_ids", &BoundaryAirTrim::linked_clip_ids));
};

template <>
struct FieldTable<RollEditPair> {
    static constexpr auto kFields =
        std::make_tuple(Field("left_clip_id", &RollEditPair::left_clip_id),
                        Field("right_clip_id", &RollEditPair::right_clip_id));
};

template <>
struct FieldTable<ColorManagementSettings> {
    static constexpr auto kFields = std::make_tuple(
        Field("enabled", &ColorManagementSettings::enabled),
        Field("input_gamut", &ColorManagementSettings::input_gamut),
        Field("input_transfer", &ColorManagementSettings::input_transfer),
        Field("input_ycbcr_matrix",
              &ColorManagementSettings::input_ycbcr_matrix),
        Field("input_range", &ColorManagementSettings::input_range),
        Field("working_gamut", &ColorManagementSettings::working_gamut),
        Field("output_gamut", &ColorManagementSettings::output_gamut),
        Field("output_transfer", &ColorManagementSettings::output_transfer));
};

template <>
struct FieldTable<DocumentMarker> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentMarker::id), Field("name", &DocumentMarker::name),
        Field("time", &DocumentMarker::time),
        Field("color", &DocumentMarker::color),
        Field("category", &DocumentMarker::category),
        Field("duration", &DocumentMarker::duration).OmittedWhenDefault());
};

template <>
struct FieldTable<DocumentTransition> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &DocumentTransition::id),
        Field("track_id", &DocumentTransition::track_id),
        Field("left_clip_id", &DocumentTransition::left_clip_id),
        Field("right_clip_id", &DocumentTransition::right_clip_id),
        Field("transition_type", &DocumentTransition::type),
        Field("duration", &DocumentTransition::duration),
        Field("alignment", &DocumentTransition::alignment),
        // R5 -- omitted for black, the colour a transition that carries
        // none also spells: an operation recorded before this ticket
        // replays byte for byte.
        Field("color", &DocumentTransition::color).OmittedWhenDefault());
};

template <>
struct FieldTable<SetClipLinkOperation::ExactState> {
    static constexpr auto kFields = std::make_tuple(
        Field("group_id", &SetClipLinkOperation::ExactState::group_id),
        Field("anchor_clip_id",
              &SetClipLinkOperation::ExactState::anchor_clip_id),
        Field("reference_delta",
              &SetClipLinkOperation::ExactState::reference_delta));
};

template <>
struct FieldTable<CaptionStyle> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &CaptionStyle::id),
        Field("font_family", &CaptionStyle::font_family),
        Field("font_size", &CaptionStyle::font_size),
        // Written only when it is not the regular, so journals
        // written before weights existed keep their bytes.
        Field("font_weight", &CaptionStyle::font_weight).OmittedWhenDefault(),
        Field("color", &CaptionStyle::color),
        Field("position", &CaptionStyle::position),
        // ITW4 -- written only when set, so journals of
        // undecorated styles keep their bytes.
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
        // PA4 -- spelled only on a style that animates its words, so
        // journals of the other styles keep their bytes.
        Field("word_animation", &CaptionStyle::word_animation)
            .OmittedWhenDefault(),
        Field("highlight_color", &CaptionStyle::highlight_color)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<MulticamAngle> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &MulticamAngle::id), Field("name", &MulticamAngle::name),
        Field("clip_id", &MulticamAngle::clip_id));
};

template <>
struct FieldTable<MulticamGroup> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &MulticamGroup::id), Field("name", &MulticamGroup::name),
        Field("angles", &MulticamGroup::angles),
        Field("active_angle_id", &MulticamGroup::active_angle_id));
};

template <>
struct FieldTable<WordRemovalRange> {
    static constexpr auto kFields =
        std::make_tuple(Field("source_start", &WordRemovalRange::source_start),
                        Field("source_end", &WordRemovalRange::source_end));
};

// G3 -- docs/roadmap/GRADE-2026-09.md.
template <>
struct FieldTable<MediaColorInput> {
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
// not written, as in the document.
template <>
struct FieldTable<ShotDescription> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &ShotDescription::id),
        Field("source_in", &ShotDescription::source_in),
        Field("duration", &ShotDescription::duration),
        Field("size", &ShotDescription::size),
        Field("subjects", &ShotDescription::subjects).OmittedWhenDefault(),
        Field("action", &ShotDescription::action).OmittedWhenDefault(),
        Field("place", &ShotDescription::place).OmittedWhenDefault(),
        Field("movement", &ShotDescription::movement),
        Field("quality", &ShotDescription::quality),
        Field("reserve", &ShotDescription::reserve).OmittedWhenDefault(),
        Field("keywords", &ShotDescription::keywords).OmittedWhenDefault(),
        Field("author_kind", &ShotDescription::author_kind),
        Field("author", &ShotDescription::author),
        Field("described_at", &ShotDescription::described_at));
};

// GN3 -- docs/roadmap/GENERATIF-2026-10.md. Every member spelled, as the
// journal spells every struct: a provenance is replayed whole.
template <>
struct FieldTable<MediaGeneration> {
    static constexpr auto kFields = std::make_tuple(
        Field("service", &MediaGeneration::service),
        Field("model", &MediaGeneration::model),
        Field("kind", &MediaGeneration::kind),
        Field("prompt", &MediaGeneration::prompt),
        Field("seed", &MediaGeneration::seed),
        Field("created_at", &MediaGeneration::created_at),
        Field("announced_cost", &MediaGeneration::announced_cost),
        Field("request_id", &MediaGeneration::request_id),
        Field("source_media_id", &MediaGeneration::source_media_id),
        // GN6b -- written only when present, so journals written before a
        // voice could be made keep their bytes.
        Field("reference_text", &MediaGeneration::reference_text)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<LibraryMedia> {
    static constexpr auto kFields = std::make_tuple(
        Field("id", &LibraryMedia::id), Field("path", &LibraryMedia::path),
        Field("filename", &LibraryMedia::filename),
        Field("codec", &LibraryMedia::codec),
        Field("width", &LibraryMedia::width),
        Field("height", &LibraryMedia::height),
        Field("pixel_format", &LibraryMedia::pixel_format),
        Field("color_range", &LibraryMedia::color_range),
        Field("color_space", &LibraryMedia::color_space),
        Field("color_transfer", &LibraryMedia::color_transfer),
        Field("color_primaries", &LibraryMedia::color_primaries),
        // G7 -- only when known, so journals keep their bytes; an older
        // journal without it reads as unknown.
        Field("white_balance_kelvin", &LibraryMedia::white_balance_kelvin)
            .OmittedWhenDefault(),
        // G3 -- the same: only a media that declares its own input.
        Field("color_input", &LibraryMedia::color_input).OmittedWhenDefault(),
        // GN3 -- the same: only a media a service generated.
        Field("generation", &LibraryMedia::generation).OmittedWhenDefault(),
        Field("rotation_degrees", &LibraryMedia::rotation_degrees),
        Field("rate", &LibraryMedia::rate),
        Field("duration", &LibraryMedia::duration),
        Field("orientation", &LibraryMedia::orientation),
        Field("has_video", &LibraryMedia::has_video).AsInteger().AddedLater(),
        Field("has_audio", &LibraryMedia::has_audio).AsInteger(),
        Field("audio_rate", &LibraryMedia::audio_rate),
        Field("audio_channels", &LibraryMedia::audio_channels),
        Field("audio_level", &LibraryMedia::audio_level)
            .Custom(&WriteMeasuredAudioLevel, &ReadMeasuredAudioLevel),
        CarriedBy("audio_level", &LibraryMedia::audio_level_measured),
        Field("bin_id", &LibraryMedia::bin_id),
        Field("proxy_path", &LibraryMedia::proxy_path),
        Field("metadata_complete", &LibraryMedia::metadata_complete)
            .AsInteger(),
        // PL1 -- only a described media, so journals keep their bytes.
        Field("shots", &LibraryMedia::shots).OmittedWhenDefault());
};

template <>
struct FieldTable<ProjectBinMetadata> {
    static constexpr auto kFields = std::make_tuple(
        Field("description", &ProjectBinMetadata::description),
        Field("rating", &ProjectBinMetadata::rating),
        Field("tags", &ProjectBinMetadata::tags),
        Field("insert_order", &ProjectBinMetadata::insert_order),
        Field("display_name", &ProjectBinMetadata::display_name).AddedLater());
};

template <>
struct FieldTable<ProjectTimelineSourceSegment> {
    static constexpr auto kFields = std::make_tuple(
        Field("source_id", &ProjectTimelineSourceSegment::source_id),
        Field("source_in", &ProjectTimelineSourceSegment::source_in),
        Field("duration", &ProjectTimelineSourceSegment::duration),
        Field("video_clip_id", &ProjectTimelineSourceSegment::video_clip_id),
        Field("audio_clip_id", &ProjectTimelineSourceSegment::audio_clip_id),
        Field("link_group_id", &ProjectTimelineSourceSegment::link_group_id),
        // SYNCHRO-2026-10 SY3 -- written only for a segment that carries a
        // recorder's sound: every journal before it keeps its bytes.
        Field("separate_source_id",
              &ProjectTimelineSourceSegment::separate_source_id)
            .OmittedWhenDefault(),
        Field("separate_source_in",
              &ProjectTimelineSourceSegment::separate_source_in)
            .OmittedWhenDefault(),
        Field("separate_clip_id",
              &ProjectTimelineSourceSegment::separate_clip_id)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<CaptionTextReplacement> {
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &CaptionTextReplacement::clip_id),
        Field("from", &CaptionTextReplacement::from),
        Field("to", &CaptionTextReplacement::to),
        Field("expected_count", &CaptionTextReplacement::expected_count));
};

template <>
struct FieldTable<ProjectRelinkItem> {
    static constexpr auto kFields =
        std::make_tuple(Field("media_id", &ProjectRelinkItem::media_id),
                        Field("replacement", &ProjectRelinkItem::replacement),
                        Field("stored_path", &ProjectRelinkItem::stored_path));
};

// ---------------------------------------------------------------------
// Timeline operations.
// ---------------------------------------------------------------------

template <>
struct FieldTable<InsertClipOperation> {
    static constexpr std::string_view kType = "InsertClip";
    static constexpr auto kFields = std::make_tuple(
        Field("track_id", &InsertClipOperation::track_id),
        Field("source_id", &InsertClipOperation::source_id),
        Field("source_in", &InsertClipOperation::source_in),
        Field("duration", &InsertClipOperation::duration),
        Field("timeline_in", &InsertClipOperation::timeline_in),
        Field("clip_id", &InsertClipOperation::clip_id),
        Field("exact_timeline", &InsertClipOperation::exact_timeline_result),
        // QC-2026-09 C1.
        Field("sync_track_ids", &InsertClipOperation::sync_track_ids)
            .AddedLater(),
        // R4 -- written only for a clip laid at another speed than 1/1:
        // every journal before it, and every plain insert since, keeps its
        // bytes.
        Field("speed", &InsertClipOperation::speed).OmittedWhenDefault());
};

template <>
struct FieldTable<RemoveClipOperation> {
    static constexpr std::string_view kType = "RemoveClip";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &RemoveClipOperation::clip_id),
        // QC-2026-09 A5 -- added mid-object, not at its end.
        Field("sync_track_ids", &RemoveClipOperation::sync_track_ids)
            .AddedLater(),
        Field("exact_timeline", &RemoveClipOperation::exact_timeline_result));
};

template <>
struct FieldTable<ClearClipOperation> {
    static constexpr std::string_view kType = "ClearClip";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &ClearClipOperation::clip_id),
        Field("exact_tracks", &ClearClipOperation::exact_track_result));
};

template <>
struct FieldTable<ClearClipsOperation> {
    static constexpr std::string_view kType = "ClearClips";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_ids", &ClearClipsOperation::clip_ids),
        Field("exact_tracks", &ClearClipsOperation::exact_track_result));
};

template <>
struct FieldTable<PasteClipsOperation> {
    static constexpr std::string_view kType = "PasteClips";
    static constexpr auto kFields = std::make_tuple(
        Field("clips", &PasteClipsOperation::clips),
        Field("exact_tracks", &PasteClipsOperation::exact_track_result),
        // Version-1 edit logs predate overwrite-at-playhead.
        Field("overwrite", &PasteClipsOperation::overwrite).AddedLater());
};

template <>
struct FieldTable<TrimClipOperation> {
    static constexpr std::string_view kType = "TrimClip";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &TrimClipOperation::clip_id),
        Field("edge", &TrimClipOperation::edge),
        Field("delta", &TrimClipOperation::delta),
        Field("exact_clip", &TrimClipOperation::exact_clip_result));
};

template <>
struct FieldTable<MoveClipOperation> {
    static constexpr std::string_view kType = "MoveClip";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &MoveClipOperation::clip_id),
        Field("track_id", &MoveClipOperation::track_id),
        Field("timeline_in", &MoveClipOperation::timeline_in),
        Field("exact_tracks", &MoveClipOperation::exact_track_result)
            .AddedLater());
};

template <>
struct FieldTable<MoveClipsOperation> {
    static constexpr std::string_view kType = "MoveClips";
    static constexpr auto kFields = std::make_tuple(
        Field("moves", &MoveClipsOperation::moves),
        Field("exact_tracks", &MoveClipsOperation::exact_track_result));
};

template <>
struct FieldTable<MoveLinkedClipsOperation> {
    static constexpr std::string_view kType = "MoveLinkedClips";
    static constexpr auto kFields = std::make_tuple(
        Field("link_group_id", &MoveLinkedClipsOperation::link_group_id),
        Field("moves", &MoveLinkedClipsOperation::moves),
        Field("exact_tracks", &MoveLinkedClipsOperation::exact_track_result));
};

template <>
struct FieldTable<TrimLinkedClipsOperation> {
    static constexpr std::string_view kType = "TrimLinkedClips";
    static constexpr auto kFields = std::make_tuple(
        Field("link_group_id", &TrimLinkedClipsOperation::link_group_id),
        Field("trims", &TrimLinkedClipsOperation::trims),
        Field("exact_tracks", &TrimLinkedClipsOperation::exact_track_result));
};

template <>
struct FieldTable<RippleTrimOperation> {
    static constexpr std::string_view kType = "RippleTrim";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &RippleTrimOperation::clip_id),
        Field("edge", &RippleTrimOperation::edge),
        Field("delta", &RippleTrimOperation::delta),
        Field("linked_clip_ids", &RippleTrimOperation::linked_clip_ids),
        Field("sync_track_ids", &RippleTrimOperation::sync_track_ids),
        Field("exact_tracks", &RippleTrimOperation::exact_track_result));
};

template <>
struct FieldTable<TrimBoundaryAirOperation> {
    static constexpr std::string_view kType = "TrimBoundaryAir";
    static constexpr auto kFields = std::make_tuple(
        Field("trims", &TrimBoundaryAirOperation::trims),
        Field("sync_track_ids", &TrimBoundaryAirOperation::sync_track_ids),
        Field("exact_tracks", &TrimBoundaryAirOperation::exact_track_result));
};

template <>
struct FieldTable<RollEditOperation> {
    static constexpr std::string_view kType = "RollEdit";
    static constexpr auto kFields = std::make_tuple(
        Field("pairs", &RollEditOperation::pairs),
        Field("delta", &RollEditOperation::delta),
        Field("exact_tracks", &RollEditOperation::exact_track_result));
};

template <>
struct FieldTable<SlipEditOperation> {
    static constexpr std::string_view kType = "SlipEdit";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_ids", &SlipEditOperation::clip_ids),
        Field("delta", &SlipEditOperation::delta),
        Field("exact_tracks", &SlipEditOperation::exact_track_result));
};

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md.
template <>
struct FieldTable<SlideEditOperation> {
    static constexpr std::string_view kType = "SlideEdit";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_ids", &SlideEditOperation::clip_ids),
        Field("delta", &SlideEditOperation::delta),
        Field("exact_tracks", &SlideEditOperation::exact_track_result));
};

// FC3 -- docs/roadmap/FILMCRAFT-2026-10.md.
template <>
struct FieldTable<RateStretchOperation> {
    static constexpr std::string_view kType = "RateStretch";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &RateStretchOperation::clip_id),
        Field("edge", &RateStretchOperation::edge),
        Field("duration", &RateStretchOperation::duration),
        // DEC40 -- written only when the stretch ripples, so that every
        // journal written before it keeps its bytes.
        Field("ripple", &RateStretchOperation::ripple).OmittedWhenDefault(),
        Field("sync_track_ids", &RateStretchOperation::sync_track_ids)
            .OmittedWhenDefault(),
        Field("exact_tracks", &RateStretchOperation::exact_track_result));
};

template <>
struct FieldTable<RemoveLinkedClipsOperation> {
    static constexpr std::string_view kType = "RemoveLinkedClips";
    static constexpr auto kFields = std::make_tuple(
        Field("link_group_id", &RemoveLinkedClipsOperation::link_group_id),
        Field("clip_ids", &RemoveLinkedClipsOperation::clip_ids),
        Field("sync_track_ids", &RemoveLinkedClipsOperation::sync_track_ids)
            .AddedLater(),
        Field("exact_tracks", &RemoveLinkedClipsOperation::exact_track_result));
};

template <>
struct FieldTable<ClearLinkedClipsOperation> {
    static constexpr std::string_view kType = "ClearLinkedClips";
    static constexpr auto kFields = std::make_tuple(
        Field("link_group_id", &ClearLinkedClipsOperation::link_group_id),
        Field("clip_ids", &ClearLinkedClipsOperation::clip_ids),
        Field("exact_tracks", &ClearLinkedClipsOperation::exact_track_result));
};

template <>
struct FieldTable<SplitClipOperation> {
    static constexpr std::string_view kType = "SplitClip";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &SplitClipOperation::clip_id),
        Field("timeline_position", &SplitClipOperation::timeline_position),
        Field("right_clip_id", &SplitClipOperation::right_clip_id),
        // Logs written before a split carried the clip's grade.
        Field("right_effect_ids", &SplitClipOperation::right_effect_ids)
            .AddedLater());
};

template <>
struct FieldTable<SplitLinkedClipsOperation> {
    static constexpr std::string_view kType = "SplitLinkedClips";
    static constexpr auto kFields = std::make_tuple(
        Field("link_group_id", &SplitLinkedClipsOperation::link_group_id),
        Field("clip_ids", &SplitLinkedClipsOperation::clip_ids),
        Field("timeline_position",
              &SplitLinkedClipsOperation::timeline_position),
        Field("left_group_id", &SplitLinkedClipsOperation::left_group_id),
        Field("right_group_id", &SplitLinkedClipsOperation::right_group_id),
        Field("right_clip_ids", &SplitLinkedClipsOperation::right_clip_ids),
        Field("sync_track_ids", &SplitLinkedClipsOperation::sync_track_ids)
            .AddedLater(),
        Field("exact_tracks", &SplitLinkedClipsOperation::exact_track_result));
};

template <>
struct FieldTable<DeleteGapOperation> {
    static constexpr std::string_view kType = "DeleteGap";
    static constexpr auto kFields = std::make_tuple(
        Field("track_id", &DeleteGapOperation::track_id),
        Field("gap_start", &DeleteGapOperation::gap_start),
        Field("gap_duration", &DeleteGapOperation::gap_duration),
        Field("linked_track_ids", &DeleteGapOperation::linked_track_ids)
            .OmittedWhenDefault(),
        Field("exact_tracks", &DeleteGapOperation::exact_track_result));
};

template <>
struct FieldTable<DetachAudioOperation> {
    static constexpr std::string_view kType = "DetachAudio";
    static constexpr auto kFields = std::make_tuple(
        Field("video_clip_id", &DetachAudioOperation::video_clip_id),
        Field("audio_track_id", &DetachAudioOperation::audio_track_id),
        Field("audio_clip_id", &DetachAudioOperation::audio_clip_id),
        Field("exact_tracks", &DetachAudioOperation::exact_track_result));
};

template <>
struct FieldTable<AddTrackOperation> {
    static constexpr std::string_view kType = "AddTrack";
    static constexpr auto kFields = std::make_tuple(
        Field("track_id", &AddTrackOperation::track_id),
        Field("kind", &AddTrackOperation::kind),
        Field("index", &AddTrackOperation::index),
        Field("locked", &AddTrackOperation::locked).AddedLater(),
        Field("sync_lock", &AddTrackOperation::sync_lock).AddedLater(),
        Field("visible", &AddTrackOperation::visible).AddedLater(),
        Field("muted", &AddTrackOperation::muted).AddedLater(),
        Field("solo", &AddTrackOperation::solo).AddedLater(),
        Field("exact_tracks", &AddTrackOperation::clips)
            .Custom(&WriteAddTrackClips, &ReadAddTrackClips),
        Field("insert_before_track_id",
              &AddTrackOperation::insert_before_track_id)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<RemoveTrackOperation> {
    static constexpr std::string_view kType = "RemoveTrack";
    static constexpr auto kFields =
        std::make_tuple(Field("track_id", &RemoveTrackOperation::track_id));
};

template <>
struct FieldTable<SetTrackLockOperation> {
    static constexpr std::string_view kType = "SetTrackLock";
    static constexpr auto kFields =
        std::make_tuple(Field("track_id", &SetTrackLockOperation::track_id),
                        Field("locked", &SetTrackLockOperation::locked));
};

template <>
struct FieldTable<SetTrackSyncLockOperation> {
    static constexpr std::string_view kType = "SetTrackSyncLock";
    static constexpr auto kFields = std::make_tuple(
        Field("track_id", &SetTrackSyncLockOperation::track_id),
        Field("sync_lock", &SetTrackSyncLockOperation::sync_lock));
};

template <>
struct FieldTable<SetTrackOutputOperation> {
    static constexpr std::string_view kType = "SetTrackOutput";
    static constexpr auto kFields =
        std::make_tuple(Field("track_id", &SetTrackOutputOperation::track_id),
                        Field("visible", &SetTrackOutputOperation::visible),
                        Field("muted", &SetTrackOutputOperation::muted),
                        Field("solo", &SetTrackOutputOperation::solo));
};

template <>
struct FieldTable<UpdateSequenceOperation> {
    static constexpr std::string_view kType = "UpdateSequence";
    static constexpr auto kFields = std::make_tuple(
        Field("sequence_id", &UpdateSequenceOperation::sequence_id),
        Field("name", &UpdateSequenceOperation::name),
        Field("width", &UpdateSequenceOperation::width),
        Field("height", &UpdateSequenceOperation::height),
        Field("frame_rate", &UpdateSequenceOperation::frame_rate));
};

template <>
struct FieldTable<SetColorManagementOperation> {
    static constexpr std::string_view kType = "SetColorManagement";
    static constexpr auto kFields = std::make_tuple(
        Field("settings", &SetColorManagementOperation::settings));
};

template <>
struct FieldTable<AddBinOperation> {
    static constexpr std::string_view kType = "AddBin";
    static constexpr auto kFields = std::make_tuple(
        Field("bin_id", &AddBinOperation::bin_id),
        Field("name", &AddBinOperation::name),
        Field("parent_id", &AddBinOperation::parent_id).AddedLater());
};

template <>
struct FieldTable<RemoveBinOperation> {
    static constexpr std::string_view kType = "RemoveBin";
    static constexpr auto kFields = std::make_tuple(
        Field("bin_id", &RemoveBinOperation::bin_id),
        Field("name", &RemoveBinOperation::name),
        Field("parent_id", &RemoveBinOperation::parent_id).AddedLater());
};

template <>
struct FieldTable<RenameBinOperation> {
    static constexpr std::string_view kType = "RenameBin";
    static constexpr auto kFields =
        std::make_tuple(Field("bin_id", &RenameBinOperation::bin_id),
                        Field("name", &RenameBinOperation::name));
};

template <>
struct FieldTable<MoveBinOperation> {
    static constexpr std::string_view kType = "MoveBin";
    static constexpr auto kFields =
        std::make_tuple(Field("bin_id", &MoveBinOperation::bin_id),
                        Field("parent_id", &MoveBinOperation::parent_id));
};

template <>
struct FieldTable<SetMediaBinOperation> {
    static constexpr std::string_view kType = "SetMediaBin";
    static constexpr auto kFields =
        std::make_tuple(Field("media_id", &SetMediaBinOperation::media_id),
                        Field("bin_id", &SetMediaBinOperation::bin_id));
};

template <>
struct FieldTable<SetMediaProxyOperation> {
    static constexpr std::string_view kType = "SetMediaProxy";
    static constexpr auto kFields = std::make_tuple(
        Field("media_id", &SetMediaProxyOperation::media_id),
        Field("proxy_path", &SetMediaProxyOperation::proxy_path));
};

// G7 -- docs/roadmap/GRADE-2026-09.md.
template <>
struct FieldTable<SetMediaWhiteBalanceOperation> {
    static constexpr std::string_view kType = "SetMediaWhiteBalance";
    static constexpr auto kFields = std::make_tuple(
        Field("media_id", &SetMediaWhiteBalanceOperation::media_id),
        Field("kelvin", &SetMediaWhiteBalanceOperation::kelvin));
};

// G3 -- docs/roadmap/GRADE-2026-09.md.
// PL1 -- docs/roadmap/PLANS-2026-10.md.
template <>
struct FieldTable<SetMediaShotsOperation> {
    static constexpr std::string_view kType = "SetMediaShots";
    static constexpr auto kFields =
        std::make_tuple(Field("media_id", &SetMediaShotsOperation::media_id),
                        Field("shots", &SetMediaShotsOperation::shots));
};

template <>
struct FieldTable<SetMediaColorInputOperation> {
    static constexpr std::string_view kType = "SetMediaColorInput";
    static constexpr auto kFields = std::make_tuple(
        Field("media_id", &SetMediaColorInputOperation::media_id),
        Field("input", &SetMediaColorInputOperation::input));
};

// GN3 -- docs/roadmap/GENERATIF-2026-10.md.
template <>
struct FieldTable<SetMediaGenerationOperation> {
    static constexpr std::string_view kType = "SetMediaGeneration";
    static constexpr auto kFields = std::make_tuple(
        Field("media_id", &SetMediaGenerationOperation::media_id),
        Field("generation", &SetMediaGenerationOperation::generation));
};

// DEC32 (PRD7) -- docs/roadmap/PRODUIT-2026-10.md.
template <>
struct FieldTable<MediaBinAssignment> {
    static constexpr auto kFields =
        std::make_tuple(Field("media_id", &MediaBinAssignment::media_id),
                        Field("bin_id", &MediaBinAssignment::bin_id));
};

template <>
struct FieldTable<IngestMediaOperation> {
    static constexpr std::string_view kType = "IngestMedia";
    static constexpr auto kFields = std::make_tuple(
        Field("added", &IngestMediaOperation::added),
        Field("bin_id", &IngestMediaOperation::bin_id),
        Field("filed", &IngestMediaOperation::filed),
        Field("completed", &IngestMediaOperation::completed)
            .OmittedWhenDefault(),
        Field("mounted", &IngestMediaOperation::mounted).OmittedWhenDefault());
};

template <>
struct FieldTable<RemoveIngestedMediaOperation> {
    static constexpr std::string_view kType = "RemoveIngestedMedia";
    static constexpr auto kFields = std::make_tuple(
        Field("media_ids", &RemoveIngestedMediaOperation::media_ids),
        Field("bin_id", &RemoveIngestedMediaOperation::bin_id),
        Field("refiled", &RemoveIngestedMediaOperation::refiled),
        Field("unmounted", &RemoveIngestedMediaOperation::unmounted)
            .OmittedWhenDefault(),
        Field("restored", &RemoveIngestedMediaOperation::restored)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<AddMarkerOperation> {
    static constexpr std::string_view kType = "AddMarker";
    static constexpr auto kFields = std::make_tuple(
        Field("marker", &AddMarkerOperation::marker),
        Field("insertion_index", &AddMarkerOperation::insertion_index));
};

template <>
struct FieldTable<RemoveMarkerOperation> {
    static constexpr std::string_view kType = "RemoveMarker";
    static constexpr auto kFields =
        std::make_tuple(Field("marker_id", &RemoveMarkerOperation::marker_id));
};

template <>
struct FieldTable<UpdateMarkerOperation> {
    static constexpr std::string_view kType = "UpdateMarker";
    static constexpr auto kFields =
        std::make_tuple(Field("marker_id", &UpdateMarkerOperation::marker_id),
                        Field("name", &UpdateMarkerOperation::name),
                        Field("time", &UpdateMarkerOperation::time),
                        Field("color", &UpdateMarkerOperation::color),
                        Field("category", &UpdateMarkerOperation::category),
                        Field("duration", &UpdateMarkerOperation::duration)
                            .OmittedWhenDefault());
};

template <>
struct FieldTable<AddTransitionOperation> {
    static constexpr std::string_view kType = "AddTransition";
    static constexpr auto kFields = std::make_tuple(
        Field("transition", &AddTransitionOperation::transition),
        Field("insertion_index", &AddTransitionOperation::insertion_index));
};

template <>
struct FieldTable<RemoveTransitionOperation> {
    static constexpr std::string_view kType = "RemoveTransition";
    static constexpr auto kFields = std::make_tuple(
        Field("transition_id", &RemoveTransitionOperation::transition_id));
};

template <>
struct FieldTable<UpdateTransitionOperation> {
    static constexpr std::string_view kType = "UpdateTransition";
    static constexpr auto kFields = std::make_tuple(
        Field("transition_id", &UpdateTransitionOperation::transition_id),
        Field("transition_type", &UpdateTransitionOperation::type),
        Field("duration", &UpdateTransitionOperation::duration),
        Field("alignment", &UpdateTransitionOperation::alignment),
        // R5 -- omitted for black, so a journal written before this ticket
        // replays byte for byte.
        Field("color", &UpdateTransitionOperation::color).OmittedWhenDefault());
};

template <>
struct FieldTable<SetClipLinkOperation> {
    static constexpr std::string_view kType = "SetClipLink";
    static constexpr auto kFields = std::make_tuple(
        Field("first_clip_id", &SetClipLinkOperation::first_clip_id),
        Field("second_clip_id", &SetClipLinkOperation::second_clip_id),
        Field("first_group_id", &SetClipLinkOperation::first_group_id),
        Field("second_group_id", &SetClipLinkOperation::second_group_id),
        Field("exact_first", &SetClipLinkOperation::exact_first_result)
            .AddedLater(),
        Field("exact_second", &SetClipLinkOperation::exact_second_result)
            .AddedLater());
};

template <>
struct FieldTable<SetClipEffectsOperation> {
    static constexpr std::string_view kType = "SetClipEffects";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipEffectsOperation::clip_id),
                        Field("effects", &SetClipEffectsOperation::effects));
};

template <>
struct FieldTable<SetClipOpacityOperation> {
    static constexpr std::string_view kType = "SetClipOpacity";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipOpacityOperation::clip_id),
                        Field("opacity", &SetClipOpacityOperation::opacity));
};

// EF3 -- docs/roadmap/EFFETS-2026-10.md.
template <>
struct FieldTable<SetClipBlurOperation> {
    static constexpr std::string_view kType = "SetClipBlur";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipBlurOperation::clip_id),
                        Field("blur", &SetClipBlurOperation::blur));
};

// PA8 -- docs/roadmap/PARITE-2026-11.md.
template <>
struct FieldTable<SetClipMaskOperation> {
    static constexpr std::string_view kType = "SetClipMask";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipMaskOperation::clip_id),
                        Field("mask", &SetClipMaskOperation::mask));
};

// R3 -- docs/roadmap/REEL-2026-09.md.
template <>
struct FieldTable<SetClipSpeedOperation> {
    static constexpr std::string_view kType = "SetClipSpeed";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &SetClipSpeedOperation::clip_id),
        Field("speed", &SetClipSpeedOperation::speed),
        // Criterion 4 -- spelled only for a ramp, so every
        // constant speed keeps the bytes it had.
        Field("ramp", &SetClipSpeedOperation::ramp).OmittedWhenDefault());
};

// R1 -- docs/roadmap/REEL-2026-09.md.
template <>
struct FieldTable<SetClipFramingOperation> {
    static constexpr std::string_view kType = "SetClipFraming";
    static constexpr auto kFields =
        std::make_tuple(Field("clips", &SetClipFramingOperation::clips));
};

// R6 -- docs/roadmap/REEL-2026-09.md.
template <>
struct FieldTable<SetSequenceLutOperation> {
    static constexpr std::string_view kType = "SetSequenceLut";
    static constexpr auto kFields =
        std::make_tuple(Field("lut", &SetSequenceLutOperation::lut));
};

// R2 -- docs/roadmap/REEL-2026-09.md.
template <>
struct FieldTable<SetClipAnimationOperation> {
    static constexpr std::string_view kType = "SetClipAnimation";
    static constexpr auto kFields =
        std::make_tuple(Field("clips", &SetClipAnimationOperation::clips));
};

// Out of lot -- the author, 29 October 2026: Resolve's Enable/Disable Clip.
template <>
struct FieldTable<SetClipEnabledOperation> {
    static constexpr std::string_view kType = "SetClipEnabled";
    static constexpr auto kFields =
        std::make_tuple(Field("clips", &SetClipEnabledOperation::clips));
};

template <>
struct FieldTable<SetClipAudioOperation> {
    static constexpr std::string_view kType = "SetClipAudio";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipAudioOperation::clip_id),
                        Field("gain_db", &SetClipAudioOperation::gain_db),
                        Field("fade_in", &SetClipAudioOperation::fade_in),
                        Field("fade_out", &SetClipAudioOperation::fade_out));
};

template <>
struct FieldTable<AddCaptionStyleOperation> {
    static constexpr std::string_view kType = "AddCaptionStyle";
    static constexpr auto kFields = std::make_tuple(
        Field("style", &AddCaptionStyleOperation::style),
        Field("insertion_index", &AddCaptionStyleOperation::insertion_index));
};

template <>
struct FieldTable<UpdateCaptionStyleOperation> {
    static constexpr std::string_view kType = "UpdateCaptionStyle";
    static constexpr auto kFields =
        std::make_tuple(Field("style", &UpdateCaptionStyleOperation::style));
};

template <>
struct FieldTable<RemoveCaptionStyleOperation> {
    static constexpr std::string_view kType = "RemoveCaptionStyle";
    static constexpr auto kFields = std::make_tuple(
        Field("style_id", &RemoveCaptionStyleOperation::style_id));
};

template <>
struct FieldTable<SetClipCaptionOperation> {
    static constexpr std::string_view kType = "SetClipCaption";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &SetClipCaptionOperation::clip_id),
        Field("caption_group_id", &SetClipCaptionOperation::caption_group_id),
        Field("caption_text", &SetClipCaptionOperation::caption_text),
        // PA4 -- spelled only when the carton's words are timed, so the
        // journals written before keep their bytes.
        Field("caption_word_times",
              &SetClipCaptionOperation::caption_word_times)
            .OmittedWhenDefault(),
        Field("word_times_resolved",
              &SetClipCaptionOperation::word_times_resolved)
            .OmittedWhenDefault());
};

template <>
struct FieldTable<AddMulticamGroupOperation> {
    static constexpr std::string_view kType = "AddMulticamGroup";
    static constexpr auto kFields = std::make_tuple(
        Field("group", &AddMulticamGroupOperation::group),
        Field("insertion_index", &AddMulticamGroupOperation::insertion_index));
};

template <>
struct FieldTable<RemoveMulticamGroupOperation> {
    static constexpr std::string_view kType = "RemoveMulticamGroup";
    static constexpr auto kFields = std::make_tuple(
        Field("group_id", &RemoveMulticamGroupOperation::group_id));
};

template <>
struct FieldTable<SetMulticamActiveAngleOperation> {
    static constexpr std::string_view kType = "SetMulticamActiveAngle";
    static constexpr auto kFields = std::make_tuple(
        Field("group_id", &SetMulticamActiveAngleOperation::group_id),
        Field("active_angle_id",
              &SetMulticamActiveAngleOperation::active_angle_id));
};

template <>
struct FieldTable<SplitClipAtPositionsOperation> {
    static constexpr std::string_view kType = "SplitClipAtPositions";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &SplitClipAtPositionsOperation::clip_id),
        Field("positions", &SplitClipAtPositionsOperation::positions),
        Field("exact_tracks",
              &SplitClipAtPositionsOperation::exact_track_result));
};

// SYNCHRO-2026-10 SY3.
template <>
struct FieldTable<LinkedWordRemoval> {
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &LinkedWordRemoval::clip_id),
                        Field("ranges", &LinkedWordRemoval::ranges));
};

template <>
struct FieldTable<RemoveWordsOperation> {
    static constexpr std::string_view kType = "RemoveWords";
    static constexpr auto kFields = std::make_tuple(
        Field("clip_id", &RemoveWordsOperation::clip_id),
        Field("ranges", &RemoveWordsOperation::ranges),
        Field("gap_padding", &RemoveWordsOperation::gap_padding),
        // Edit logs recorded before linked cuts existed.
        Field("linked_clip_ids", &RemoveWordsOperation::linked_clip_ids)
            .AddedLater(),
        Field("sync_track_ids", &RemoveWordsOperation::sync_track_ids),
        Field("exact_tracks", &RemoveWordsOperation::exact_track_result),
        // SYNCHRO-2026-10 SY3 -- written only when a cut carries a linked
        // clip of another source: every journal before it, and every cut
        // without one since, keeps its bytes.
        Field("translated_links", &RemoveWordsOperation::translated_links)
            .OmittedWhenDefault());
};

// PL1 -- ROADMAP.md.
template <>
struct FieldTable<BatchOperation> {
    static constexpr std::string_view kType = "Batch";
    static constexpr auto kFields =
        std::make_tuple(Field("label", &BatchOperation::label),
                        Field("operations", &BatchOperation::operations)
                            .Custom(&WriteBatchMembers, &ReadBatchMembers));
};

template <>
struct FieldTable<JoinClipOperation> {
    static constexpr std::string_view kType = "JoinClip";
    static constexpr auto kFields = std::make_tuple(
        Field("left_clip_id", &JoinClipOperation::left_clip_id),
        Field("right_clip_id", &JoinClipOperation::right_clip_id),
        Field("joined_times", &JoinClipOperation::joined_times));
};

// ---------------------------------------------------------------------
// MOTION-2026-09 M3 -- compositions. A motion tree (a definition, a node, a
// style value, an animation, prop values) is written as the JSON it is,
// decimals included: the motion format is its own, validated by
// MotionComposition.cc when the operation is applied, not here.
// ---------------------------------------------------------------------

template <>
struct FieldTable<DocumentComposition> {
    static constexpr auto kFields =
        std::make_tuple(Field("id", &DocumentComposition::id),
                        Field("name", &DocumentComposition::name),
                        Field("definition", &DocumentComposition::definition));
};

template <>
struct FieldTable<AddMotionCompositionOperation> {
    static constexpr std::string_view kType = "AddMotionComposition";
    static constexpr auto kFields = std::make_tuple(
        Field("composition", &AddMotionCompositionOperation::composition),
        Field("insertion_index",
              &AddMotionCompositionOperation::insertion_index));
};

template <>
struct FieldTable<RemoveMotionCompositionOperation> {
    static constexpr std::string_view kType = "RemoveMotionComposition";
    static constexpr auto kFields = std::make_tuple(Field(
        "composition_id", &RemoveMotionCompositionOperation::composition_id));
};

template <>
struct FieldTable<UpdateMotionCompositionOperation> {
    static constexpr std::string_view kType = "UpdateMotionComposition";
    static constexpr auto kFields = std::make_tuple(
        Field("composition", &UpdateMotionCompositionOperation::composition));
};

template <>
struct FieldTable<SetMotionNodeOperation> {
    static constexpr std::string_view kType = "SetMotionNode";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &SetMotionNodeOperation::composition_id),
        Field("parent_uid", &SetMotionNodeOperation::parent_uid),
        Field("before_uid", &SetMotionNodeOperation::before_uid),
        Field("node", &SetMotionNodeOperation::node));
};

template <>
struct FieldTable<RemoveMotionNodeOperation> {
    static constexpr std::string_view kType = "RemoveMotionNode";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &RemoveMotionNodeOperation::composition_id),
        Field("node_uid", &RemoveMotionNodeOperation::node_uid));
};

template <>
struct FieldTable<SetMotionStyleOperation> {
    static constexpr std::string_view kType = "SetMotionStyle";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &SetMotionStyleOperation::composition_id),
        Field("node_uid", &SetMotionStyleOperation::node_uid),
        Field("property", &SetMotionStyleOperation::property),
        Field("value", &SetMotionStyleOperation::value));
};

template <>
struct FieldTable<SetMotionAnimationOperation> {
    static constexpr std::string_view kType = "SetMotionAnimation";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &SetMotionAnimationOperation::composition_id),
        Field("node_uid", &SetMotionAnimationOperation::node_uid),
        Field("property", &SetMotionAnimationOperation::property),
        Field("animation", &SetMotionAnimationOperation::animation));
};

template <>
struct FieldTable<SetMotionKeyframeOperation> {
    static constexpr std::string_view kType = "SetMotionKeyframe";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &SetMotionKeyframeOperation::composition_id),
        Field("keyframe_uid", &SetMotionKeyframeOperation::keyframe_uid),
        Field("frame", &SetMotionKeyframeOperation::frame),
        Field("value", &SetMotionKeyframeOperation::value));
};

template <>
struct FieldTable<SetMotionPropsOperation> {
    static constexpr std::string_view kType = "SetMotionProps";
    static constexpr auto kFields = std::make_tuple(
        Field("composition_id", &SetMotionPropsOperation::composition_id),
        Field("props", &SetMotionPropsOperation::props));
};

// MOTION-2026-09 M4 -- motion clips on the timeline. Instance props are a
// motion value like a composition's, decimals included; null when the
// instance sets none.
template <>
struct FieldTable<InsertMotionClipOperation> {
    static constexpr std::string_view kType = "InsertMotionClip";
    static constexpr auto kFields = std::make_tuple(
        Field("track_id", &InsertMotionClipOperation::track_id),
        Field("composition_id", &InsertMotionClipOperation::composition_id),
        Field("source_in", &InsertMotionClipOperation::source_in),
        Field("duration", &InsertMotionClipOperation::duration),
        Field("timeline_in", &InsertMotionClipOperation::timeline_in),
        Field("motion_props", &InsertMotionClipOperation::motion_props),
        Field("clip_id", &InsertMotionClipOperation::clip_id),
        Field("exact_timeline",
              &InsertMotionClipOperation::exact_timeline_result),
        Field("sync_track_ids", &InsertMotionClipOperation::sync_track_ids));
};

template <>
struct FieldTable<SetClipMotionPropsOperation> {
    static constexpr std::string_view kType = "SetClipMotionProps";
    static constexpr auto kFields =
        std::make_tuple(Field("clip_id", &SetClipMotionPropsOperation::clip_id),
                        Field("props", &SetClipMotionPropsOperation::props));
};

// ---------------------------------------------------------------------
// MOTIF1 -- docs/roadmap/MOTIF-2026-10.md. Why an entry was made: not an
// operation but what rides beside one in a journal entry (EditMotif.h).
// Every member a motif leaves at its default is omitted, so a motif spells
// only what was said; `author` is always written, for a reader of the raw
// journal to tell an agent's choice from a person's at a glance.
// ---------------------------------------------------------------------

template <>
struct EnumNames<MotifReason> {
    static constexpr std::string_view kWhat = "motif reason";
    static constexpr std::pair<MotifReason, std::string_view> kNames[] = {
        {MotifReason::Unstated, "unstated"},
        {MotifReason::Hesitation, "hesitation"},
        {MotifReason::Pause, "pause"},
        {MotifReason::BoundaryAir, "boundary_air"},
        {MotifReason::Duplicate, "duplicate"},
        {MotifReason::Selection, "selection"},
        {MotifReason::Cutaway, "cutaway"},
        {MotifReason::Rhythm, "rhythm"},
        {MotifReason::Brief, "brief"},
        {MotifReason::Marker, "marker"},
        {MotifReason::Format, "format"},
        {MotifReason::Captions, "captions"},
        {MotifReason::Technical, "technical"},
        {MotifReason::Other, "other"},
    };
};

template <>
struct EnumNames<MotifAuthor> {
    static constexpr std::string_view kWhat = "motif author";
    static constexpr std::pair<MotifAuthor, std::string_view> kNames[] = {
        {MotifAuthor::Agent, "agent"},
        {MotifAuthor::Person, "person"},
        {MotifAuthor::Procedure, "procedure"},
    };
};

template <>
struct FieldTable<EditMotif> {
    static constexpr auto kFields = std::make_tuple(
        Field("reason", &EditMotif::reason).OmittedWhenDefault(),
        Field("author", &EditMotif::author),
        Field("rule", &EditMotif::rule).OmittedWhenDefault(),
        Field("params", &EditMotif::params).OmittedWhenDefault(),
        Field("note", &EditMotif::note).OmittedWhenDefault(),
        Field("marker_id", &EditMotif::marker_id).OmittedWhenDefault());
};

template <>
struct FieldTable<EntryMotif> {
    static constexpr auto kFields = std::make_tuple(
        Field("first", &EntryMotif::first), Field("count", &EntryMotif::count),
        Field("motif", &EntryMotif::motif));
};

// ---------------------------------------------------------------------
// Project operations.
// ---------------------------------------------------------------------

template <>
struct FieldTable<AddProjectTimelineOperation> {
    static constexpr std::string_view kType = "AddProjectTimeline";
    static constexpr auto kFields = std::make_tuple(
        Field("name", &AddProjectTimelineOperation::name),
        Field("width", &AddProjectTimelineOperation::width),
        Field("height", &AddProjectTimelineOperation::height),
        Field("frame_rate", &AddProjectTimelineOperation::frame_rate),
        Field("timeline_id", &AddProjectTimelineOperation::timeline_id),
        Field("video_track_id", &AddProjectTimelineOperation::video_track_id),
        Field("audio_track_id", &AddProjectTimelineOperation::audio_track_id),
        ExactProjectField<AddProjectTimelineOperation>());
};

template <>
struct FieldTable<CloneProjectTimelineOperation> {
    static constexpr std::string_view kType = "CloneProjectTimeline";
    static constexpr auto kFields = std::make_tuple(
        Field("source_timeline_id",
              &CloneProjectTimelineOperation::source_timeline_id),
        Field("name", &CloneProjectTimelineOperation::name),
        Field("timeline_id", &CloneProjectTimelineOperation::timeline_id),
        Field("id_remap", &CloneProjectTimelineOperation::id_remap),
        Field("make_active", &CloneProjectTimelineOperation::make_active),
        ExactProjectField<CloneProjectTimelineOperation>());
};

template <>
struct FieldTable<ProjectTimelineVariant> {
    static constexpr auto kFields = std::make_tuple(
        Field("clone", &ProjectTimelineVariant::clone),
        Field("replacements", &ProjectTimelineVariant::replacements));
};

template <>
struct FieldTable<CreateProjectTimelineVariantsOperation> {
    static constexpr std::string_view kType = "CreateProjectTimelineVariants";
    static constexpr auto kFields = std::make_tuple(
        Field("variants", &CreateProjectTimelineVariantsOperation::variants),
        ExactProjectField<CreateProjectTimelineVariantsOperation>());
};

template <>
struct FieldTable<CreateProjectTimelineFromSegmentsOperation> {
    static constexpr std::string_view kType =
        "CreateProjectTimelineFromSegments";
    static constexpr auto kFields = std::make_tuple(
        Field("name", &CreateProjectTimelineFromSegmentsOperation::name),
        Field("width", &CreateProjectTimelineFromSegmentsOperation::width),
        Field("height", &CreateProjectTimelineFromSegmentsOperation::height),
        Field("frame_rate",
              &CreateProjectTimelineFromSegmentsOperation::frame_rate),
        Field("segments",
              &CreateProjectTimelineFromSegmentsOperation::segments),
        Field("timeline_id",
              &CreateProjectTimelineFromSegmentsOperation::timeline_id),
        Field("video_track_id",
              &CreateProjectTimelineFromSegmentsOperation::video_track_id),
        Field("audio_track_id",
              &CreateProjectTimelineFromSegmentsOperation::audio_track_id),
        // SYNCHRO-2026-10 SY3 -- written only when a segment carries a
        // recorder's sound.
        Field("separate_track_id",
              &CreateProjectTimelineFromSegmentsOperation::separate_track_id)
            .OmittedWhenDefault(),
        Field("make_active",
              &CreateProjectTimelineFromSegmentsOperation::make_active),
        ExactProjectField<CreateProjectTimelineFromSegmentsOperation>());
};

template <>
struct FieldTable<RemoveProjectTimelineOperation> {
    static constexpr std::string_view kType = "RemoveProjectTimeline";
    static constexpr auto kFields = std::make_tuple(
        Field("timeline_id", &RemoveProjectTimelineOperation::timeline_id),
        ExactProjectField<RemoveProjectTimelineOperation>());
};

template <>
struct FieldTable<SetProjectBinMetadataOperation> {
    static constexpr std::string_view kType = "SetProjectBinMetadata";
    static constexpr auto kFields = std::make_tuple(
        Field("item_id", &SetProjectBinMetadataOperation::item_id),
        Field("metadata", &SetProjectBinMetadataOperation::metadata),
        ExactProjectField<SetProjectBinMetadataOperation>());
};

template <>
struct FieldTable<SetProjectTimelineBinOperation> {
    static constexpr std::string_view kType = "SetProjectTimelineBin";
    static constexpr auto kFields = std::make_tuple(
        Field("timeline_id", &SetProjectTimelineBinOperation::timeline_id),
        Field("bin_id", &SetProjectTimelineBinOperation::bin_id),
        ExactProjectField<SetProjectTimelineBinOperation>());
};

template <>
struct FieldTable<RenameProjectItemOperation> {
    static constexpr std::string_view kType = "RenameProjectItem";
    static constexpr auto kFields =
        std::make_tuple(Field("item_id", &RenameProjectItemOperation::item_id),
                        Field("name", &RenameProjectItemOperation::name),
                        ExactProjectField<RenameProjectItemOperation>());
};

template <>
struct FieldTable<SetActiveProjectTimelineOperation> {
    static constexpr std::string_view kType = "SetActiveProjectTimeline";
    static constexpr auto kFields = std::make_tuple(
        Field("timeline_id", &SetActiveProjectTimelineOperation::timeline_id),
        ExactProjectField<SetActiveProjectTimelineOperation>());
};

template <>
struct FieldTable<RelinkProjectMediaOperation> {
    static constexpr std::string_view kType = "RelinkProjectMedia";
    static constexpr auto kFields = std::make_tuple(
        Field("replacements", &RelinkProjectMediaOperation::replacements),
        ExactProjectField<RelinkProjectMediaOperation>());
};

// MOTIF6 -- docs/roadmap/MOTIF-2026-10.md, DEC41. A choice between
// timelines; its reason rides on the journal entry, not here.
template <>
struct FieldTable<ChooseProjectTimelineOperation> {
    static constexpr std::string_view kType = "ChooseProjectTimeline";
    static constexpr auto kFields = std::make_tuple(
        Field("timeline_id", &ChooseProjectTimelineOperation::timeline_id),
        Field("alternative_timeline_ids",
              &ChooseProjectTimelineOperation::alternative_timeline_ids),
        Field("make_active", &ChooseProjectTimelineOperation::make_active),
        ExactProjectField<ChooseProjectTimelineOperation>());
};

}  // namespace operation_format
