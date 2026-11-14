// PA1 -- docs/roadmap/PARITE-2026-11.md. See Interchange.h for what each
// format carries, what is counted and what was never verified.

#include "Interchange.h"

#include "Cli.h"
#include "ClipAnimation.h"
#include "ClipSpeed.h"
#include "Json.h"
#include "Multicam.h"
#include "ProjectStorage.h"
#include "ResolveExport.h"
#include "ResolveXmlExport.h"
#include "TransitionRender.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>

namespace {

using mcp_json::Value;

constexpr const char* kOtioExtension = ".otio";
constexpr const char* kFcpxmlExtension = ".fcpxml";

__int128 Gcd(__int128 a, __int128 b) {
    if (a < 0) a = -a;
    if (b < 0) b = -b;
    while (b != 0) {
        const __int128 t = a % b;
        a = b;
        b = t;
    }
    return a == 0 ? 1 : a;
}

std::string Int128Text(__int128 value) {
    if (value == 0) return "0";
    const bool negative = value < 0;
    if (negative) value = -value;
    std::string digits;
    while (value > 0) {
        digits.insert(digits.begin(),
                      static_cast<char>('0' + static_cast<int>(value % 10)));
        value /= 10;
    }
    return negative ? "-" + digits : digits;
}

bool UsableRate(const MediaRate& rate) { return rate.num > 0 && rate.den > 0; }

std::string EscapeXml(const std::string& text) {
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text) {
        switch (character) {
            case '&':
                escaped += "&amp;";
                break;
            case '<':
                escaped += "&lt;";
                break;
            case '>':
                escaped += "&gt;";
                break;
            case '"':
                escaped += "&quot;";
                break;
            case '\'':
                escaped += "&apos;";
                break;
            default:
                // A control character other than tab, line feed and
                // carriage return is not XML 1.0 at all: a name holding one
                // is written without it rather than as a file no parser
                // opens.
                if (static_cast<unsigned char>(character) < 0x20 &&
                    character != '\t' && character != '\n' && character != '\r')
                    break;
                escaped += character;
        }
    }
    return escaped;
}

// A file URL: everything outside the unreserved set percent-encoded, the
// separators kept, as ResolveXmlExport.cc writes a <pathurl>.
std::string FileUrl(const std::string& path) {
    static const char* kHex = "0123456789ABCDEF";
    std::string url = "file://";
    for (const unsigned char byte : path) {
        const bool unreserved =
            (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') ||
            (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' ||
            byte == '.' || byte == '~' || byte == '/';
        if (unreserved) {
            url += static_cast<char>(byte);
        } else {
            url += '%';
            url += kHex[byte >> 4];
            url += kHex[byte & 0x0f];
        }
    }
    return url;
}

RationalTime Frames(int64_t frames, const MediaRate& rate) {
    return {frames * rate.den, rate.num};
}

bool IsZero(const RationalTime& time) { return time.value == 0; }

bool HasExtension(const std::string& path, const char* extension) {
    std::string actual = std::filesystem::path(path).extension().string();
    std::transform(actual.begin(), actual.end(), actual.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return actual == extension;
}

void ResolveMediaPaths(Document& document, const std::string& directory) {
    // Stored relative to the package; another editor knows only absolute
    // paths (BuildAndWriteResolveXml does the same for the same reason).
    const std::filesystem::path base(directory);
    for (LibraryMedia& media : document.library) {
        std::filesystem::path stored(media.path);
        if (stored.is_relative()) stored = base / stored;
        std::error_code pathError;
        const std::filesystem::path resolved =
            std::filesystem::weakly_canonical(stored, pathError);
        if (!pathError) media.path = resolved.string();
    }
}

int64_t LossCount(const std::vector<InterchangeLoss>& losses,
                  const std::string& kind) {
    return std::count_if(
        losses.begin(), losses.end(),
        [&](const InterchangeLoss& loss) { return loss.kind == kind; });
}

Value IdArray(const std::vector<Ulid>& ids) {
    Value array = Value::MakeArray();
    for (const Ulid& id : ids) array.Push(Value::MakeString(id));
    return array;
}

// What the two reports share: the montage as counted, and every loss.
Value ReportBase(const InterchangeTimeline& timeline, const std::string& path,
                 const char* schema) {
    Value report = Value::MakeObject();
    report.Set("ok", Value::MakeBool(true));
    report.Set("schema", Value::MakeString(schema));
    report.Set("path", Value::MakeString(path));
    report.Set("name", Value::MakeString(timeline.name));
    report.Set("timeline_id", Value::MakeString(timeline.id));
    Value frameRate = Value::MakeObject();
    frameRate.Set("num", Value::MakeInt(timeline.frame_rate.num));
    frameRate.Set("den", Value::MakeInt(timeline.frame_rate.den));
    report.Set("frame_rate", std::move(frameRate));
    report.Set("width", Value::MakeInt(timeline.width));
    report.Set("height", Value::MakeInt(timeline.height));
    Value duration = Value::MakeObject();
    duration.Set("value", Value::MakeInt(timeline.duration.value));
    duration.Set("rate", Value::MakeInt(timeline.duration.rate));
    report.Set("duration", std::move(duration));
    int64_t videoTracks = 0;
    int64_t audioTracks = 0;
    int64_t clips = 0;
    int64_t retimed = 0;
    int64_t disabled = 0;
    int64_t transitions = 0;
    for (const InterchangeTrack& track : timeline.tracks) {
        (track.video ? videoTracks : audioTracks) += 1;
        clips += static_cast<int64_t>(track.clips.size());
        transitions += static_cast<int64_t>(track.transitions.size());
        for (const InterchangeClip& clip : track.clips) {
            if (clip.speed.num != clip.speed.den) ++retimed;
            if (!clip.enabled) ++disabled;
        }
    }
    report.Set("video_tracks", Value::MakeInt(videoTracks));
    report.Set("audio_tracks", Value::MakeInt(audioTracks));
    report.Set("clips", Value::MakeInt(clips));
    report.Set("retimed_clips", Value::MakeInt(retimed));
    report.Set("disabled_clips", Value::MakeInt(disabled));
    report.Set("transitions", Value::MakeInt(transitions));
    report.Set("markers",
               Value::MakeInt(static_cast<int64_t>(timeline.markers.size())));
    report.Set("media",
               Value::MakeInt(static_cast<int64_t>(timeline.media.size())));
    const std::vector<std::pair<const char*, const char*>> counts = {
        {"grades_not_sent", "grade"},
        {"framing_not_sent", "framing"},
        {"opacity_not_sent", "opacity"},
        {"blurs_not_sent", "blur"},
        {"masks_not_sent", "mask"},
        {"audio_gains_not_sent", "audio_gain"},
        {"audio_fades_not_sent", "audio_fade"},
        {"speed_ramps_flattened", "speed_ramp"},
        {"motion_clips_not_sent", "motion_clip"},
        {"caption_tracks_not_sent", "caption_track"},
        {"multicam_angles_disabled", "multicam_angle"},
        {"hidden_tracks_disabled", "hidden_track"},
    };
    for (const auto& [key, kind] : counts)
        report.Set(key, Value::MakeInt(LossCount(timeline.losses, kind)));
    report.Set("look_lut_not_sent",
               Value::MakeBool(LossCount(timeline.losses, "look_lut") != 0));
    return report;
}

void FinishReport(Value& report, std::vector<InterchangeLoss> losses,
                  const std::vector<Ulid>& transitionsNotSent,
                  std::string& reportJson) {
    for (const Ulid& id : transitionsNotSent)
        losses.push_back({"transition", id});
    report.Set("transitions_not_sent",
               Value::MakeInt(static_cast<int64_t>(transitionsNotSent.size())));
    Value list = Value::MakeArray();
    for (const InterchangeLoss& loss : losses) {
        Value item = Value::MakeObject();
        item.Set("kind", Value::MakeString(loss.kind));
        item.Set("id", Value::MakeString(loss.id));
        list.Push(std::move(item));
    }
    report.Set("losses", std::move(list));
    report.Set("verified_in_editor", Value::MakeBool(false));
    reportJson = report.Dump() + "\n";
}

bool LoadTimelineDocument(const std::string& projectPath,
                          const std::string& timelineId, Document& document,
                          std::string& output, int& status) {
    Project project;
    std::string error;
    if (!LoadStoredProject(projectPath, project, error)) {
        status = FailCliCommand("InvalidDocument", error, output);
        return false;
    }
    const Ulid selected =
        timelineId.empty() ? project.active_timeline_id : timelineId;
    if (!project.FindTimeline(selected)) {
        status =
            FailCliCommand("UnknownSequence",
                           "unknown timeline_id '" + selected + "'", output);
        return false;
    }
    document = project.MakeDocument(selected);
    return true;
}

// --- OTIO ------------------------------------------------------------------

// OTIO's marker colours, and the `#rrggbb` each is read as here.
struct OtioColor {
    const char* word;
    int red;
    int green;
    int blue;
};

const std::vector<OtioColor>& OtioColors() {
    static const std::vector<OtioColor> colors = {
        {"RED", 255, 0, 0},       {"PINK", 255, 192, 203},
        {"ORANGE", 255, 165, 0},  {"YELLOW", 255, 255, 0},
        {"GREEN", 0, 255, 0},     {"CYAN", 0, 255, 255},
        {"BLUE", 0, 0, 255},      {"PURPLE", 128, 0, 128},
        {"MAGENTA", 255, 0, 255}, {"BLACK", 0, 0, 0},
        {"WHITE", 255, 255, 255},
    };
    return colors;
}

int HexPair(const std::string& text, size_t offset) {
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    const int high = digit(text[offset]);
    const int low = digit(text[offset + 1]);
    return high < 0 || low < 0 ? -1 : high * 16 + low;
}

// The nearest OTIO colour word, by squared distance in RGB, ties to the
// table's order. A colour that is not `#rrggbb` reads as RED, OTIO's own
// default, and is not exact.
std::string OtioColorWord(const std::string& hex, bool& exact) {
    exact = false;
    if (hex.size() != 7 || hex[0] != '#') return "RED";
    const int red = HexPair(hex, 1);
    const int green = HexPair(hex, 3);
    const int blue = HexPair(hex, 5);
    if (red < 0 || green < 0 || blue < 0) return "RED";
    int64_t best = -1;
    const char* word = "RED";
    for (const OtioColor& entry : OtioColors()) {
        const int64_t dr = red - entry.red;
        const int64_t dg = green - entry.green;
        const int64_t db = blue - entry.blue;
        const int64_t distance = dr * dr + dg * dg + db * db;
        if (best < 0 || distance < best) {
            best = distance;
            word = entry.word;
        }
    }
    exact = best == 0;
    return word;
}

struct OtioWriter {
    const InterchangeTimeline& timeline;
    OtioExport& exported;

    // `time` in frames of `rate` when it is a whole number of them, in its
    // own rate otherwise (counted).
    Value Time(const RationalTime& time, const MediaRate& rate) {
        Value value = Value::MakeObject();
        value.Set("OTIO_SCHEMA", Value::MakeString("RationalTime.1"));
        int64_t frames = 0;
        if (UsableRate(rate) && ExactFrameCount(time, rate, frames)) {
            value.Set("rate", Value::MakeRawNumber(FormatInterchangeDecimal(
                                  rate.num, rate.den)));
            value.Set("value", Value::MakeInt(frames));
        } else {
            ++exported.off_frame_times;
            value.Set("rate", Value::MakeInt(time.rate));
            value.Set("value", Value::MakeInt(time.value));
        }
        return value;
    }

    Value Range(const RationalTime& start, const MediaRate& startRate,
                const RationalTime& duration, const MediaRate& durationRate) {
        Value range = Value::MakeObject();
        range.Set("OTIO_SCHEMA", Value::MakeString("TimeRange.1"));
        range.Set("duration", Time(duration, durationRate));
        range.Set("start_time", Time(start, startRate));
        return range;
    }

    Value Metadata(const char* key, const std::string& id) {
        Value own = Value::MakeObject();
        own.Set(key, Value::MakeString(id));
        Value metadata = Value::MakeObject();
        metadata.Set("cutmachine", std::move(own));
        return metadata;
    }

    // The fields every item of a track shares, in OTIO's own order.
    void Item(Value& item, const std::string& name, Value sourceRange,
              Value effects, bool enabled) {
        item.Set("name", Value::MakeString(name));
        item.Set("source_range", std::move(sourceRange));
        item.Set("effects", std::move(effects));
        item.Set("markers", Value::MakeArray());
        item.Set("enabled", Value::MakeBool(enabled));
    }

    Value Gap(const RationalTime& duration) {
        Value gap = Value::MakeObject();
        gap.Set("OTIO_SCHEMA", Value::MakeString("Gap.1"));
        gap.Set("metadata", Value::MakeObject());
        Item(gap, "",
             Range({0, 1}, timeline.frame_rate, duration, timeline.frame_rate),
             Value::MakeArray(), true);
        return gap;
    }

    Value Clip(const InterchangeClip& clip) {
        const InterchangeMedia& media = timeline.media[clip.media];
        // A media with no picture counts its source in the sequence's
        // frames, the only cadence it is ever cut on.
        const MediaRate sourceRate =
            UsableRate(media.rate) ? media.rate : timeline.frame_rate;
        Value effects = Value::MakeArray();
        if (clip.speed.num != clip.speed.den) {
            Value effect = Value::MakeObject();
            const bool freeze = clip.speed.num == 0;
            effect.Set("OTIO_SCHEMA",
                       Value::MakeString(freeze ? "FreezeFrame.1"
                                                : "LinearTimeWarp.1"));
            effect.Set("metadata", Value::MakeObject());
            effect.Set("name", Value::MakeString(""));
            effect.Set(
                "effect_name",
                Value::MakeString(freeze ? "FreezeFrame" : "LinearTimeWarp"));
            effect.Set("time_scalar",
                       Value::MakeRawNumber(FormatInterchangeDecimal(
                           clip.speed.num, clip.speed.den)));
            effects.Push(std::move(effect));
        }
        Value reference = Value::MakeObject();
        reference.Set("OTIO_SCHEMA", Value::MakeString("ExternalReference.1"));
        reference.Set("metadata", Metadata("media_id", media.id));
        reference.Set("name", Value::MakeString(media.filename));
        reference.Set("available_range",
                      Range({0, 1}, sourceRate, media.duration, sourceRate));
        reference.Set("available_image_bounds", Value::MakeNull());
        reference.Set("target_url", Value::MakeString(FileUrl(media.path)));
        Value references = Value::MakeObject();
        references.Set("DEFAULT_MEDIA", std::move(reference));

        Value item = Value::MakeObject();
        item.Set("OTIO_SCHEMA", Value::MakeString("Clip.2"));
        item.Set("metadata", Metadata("clip_id", clip.clip_id));
        Item(item, media.filename,
             Range(clip.source_in, sourceRate, clip.duration,
                   timeline.frame_rate),
             std::move(effects), clip.enabled);
        item.Set("media_references", std::move(references));
        item.Set("active_media_reference_key",
                 Value::MakeString("DEFAULT_MEDIA"));
        return item;
    }

    Value Transition(const InterchangeTransition& transition) {
        Value item = Value::MakeObject();
        item.Set("OTIO_SCHEMA", Value::MakeString("Transition.1"));
        item.Set("metadata", Metadata("transition_id", transition.id));
        item.Set("name", Value::MakeString(transition.type));
        item.Set("in_offset", Time(transition.before_cut, timeline.frame_rate));
        item.Set("out_offset", Time(transition.after_cut, timeline.frame_rate));
        item.Set("transition_type", Value::MakeString("SMPTE_Dissolve"));
        return item;
    }

    Value Track(const InterchangeTrack& track) {
        Value children = Value::MakeArray();
        RationalTime cursor{0, 1};
        const InterchangeClip* previous = nullptr;
        std::set<Ulid> placed;
        for (const InterchangeClip& clip : track.clips) {
            const bool touches =
                previous != nullptr && clip.timeline_in == cursor;
            if (clip.timeline_in > cursor)
                children.Push(Gap(clip.timeline_in.sub(cursor)));
            if (touches) {
                for (const InterchangeTransition& transition :
                     track.transitions) {
                    if (transition.left_clip_id != previous->clip_id ||
                        transition.right_clip_id != clip.clip_id)
                        continue;
                    children.Push(Transition(transition));
                    placed.insert(transition.id);
                    break;
                }
            }
            children.Push(Clip(clip));
            cursor = clip.timeline_in.add(clip.duration);
            previous = &clip;
        }
        for (const InterchangeTransition& transition : track.transitions)
            if (placed.count(transition.id) == 0)
                exported.transitions_not_sent.push_back(transition.id);

        Value item = Value::MakeObject();
        item.Set("OTIO_SCHEMA", Value::MakeString("Track.1"));
        item.Set("metadata", Metadata("track_id", track.id));
        Item(item, (track.video ? "V" : "A") + std::to_string(track.number),
             Value::MakeNull(), Value::MakeArray(), track.enabled);
        item.Set("children", std::move(children));
        item.Set("kind", Value::MakeString(track.video ? "Video" : "Audio"));
        return item;
    }

    Value Marker(const InterchangeMarker& marker) {
        bool exact = false;
        const std::string word = OtioColorWord(marker.color, exact);
        if (!exact) exported.colors_approximated.push_back(marker.id);
        Value own = Value::MakeObject();
        own.Set("marker_id", Value::MakeString(marker.id));
        own.Set("color", Value::MakeString(marker.color));
        own.Set("category", Value::MakeString(marker.category));
        Value metadata = Value::MakeObject();
        metadata.Set("cutmachine", std::move(own));
        Value item = Value::MakeObject();
        item.Set("OTIO_SCHEMA", Value::MakeString("Marker.2"));
        item.Set("metadata", std::move(metadata));
        item.Set("name", Value::MakeString(marker.name));
        item.Set("color", Value::MakeString(word));
        item.Set("marked_range", Range(marker.time, timeline.frame_rate,
                                       marker.duration, timeline.frame_rate));
        item.Set("comment", Value::MakeString(""));
        return item;
    }
};

// --- FCPXML ----------------------------------------------------------------

// The length of one frame, as a duration.
RationalTime FrameDuration(const MediaRate& rate) {
    return {rate.den, rate.num};
}

// `time` divided by an exact speed: where a source time falls in a
// retimed clip's local time. Reduced, and refused by the caller's check
// (the terms stay far below int32 for any clip the document validates).
bool DivideBySpeed(const RationalTime& time, const EffectParamValue& speed,
                   RationalTime& result) {
    __int128 num = static_cast<__int128>(time.value) * speed.den;
    __int128 den = static_cast<__int128>(time.rate) * speed.num;
    if (den <= 0) return false;
    const __int128 g = Gcd(num, den);
    num /= g;
    den /= g;
    if (den > INT32_MAX || num > INT64_MAX || num < INT64_MIN) return false;
    result = {static_cast<int64_t>(num), static_cast<int32_t>(den)};
    return true;
}

RationalTime MultiplyBySpeed(const RationalTime& time,
                             const EffectParamValue& speed) {
    __int128 num = static_cast<__int128>(time.value) * speed.num;
    __int128 den = static_cast<__int128>(time.rate) * speed.den;
    const __int128 g = Gcd(num, den);
    num /= g;
    den /= g;
    return {static_cast<int64_t>(num), static_cast<int32_t>(den)};
}

struct FcpxmlFormat {
    MediaRate rate;
    int32_t width = 0;
    int32_t height = 0;
};

bool SameFormat(const FcpxmlFormat& left, const FcpxmlFormat& right) {
    return left.width == right.width && left.height == right.height &&
           static_cast<int64_t>(left.rate.num) * right.rate.den ==
               static_cast<int64_t>(right.rate.num) * left.rate.den;
}

}  // namespace

std::string FormatInterchangeDecimal(int64_t num, int64_t den, int digits) {
    if (den == 0) return "0";
    __int128 n = num;
    __int128 d = den;
    if (d < 0) {
        n = -n;
        d = -d;
    }
    const bool negative = n < 0;
    if (negative) n = -n;
    std::string text = Int128Text(n / d);
    __int128 remainder = n % d;
    if (remainder != 0 && digits > 0) {
        std::string fraction;
        for (int index = 0; index < digits && remainder != 0; ++index) {
            remainder *= 10;
            fraction +=
                static_cast<char>('0' + static_cast<int>(remainder / d));
            remainder %= d;
        }
        while (!fraction.empty() && fraction.back() == '0') fraction.pop_back();
        if (!fraction.empty()) text += "." + fraction;
    }
    // "-0" is no number a reader expects for a value that truncates to
    // zero.
    if (negative && text != "0") text.insert(text.begin(), '-');
    return text;
}

std::string FormatFcpxmlTime(const RationalTime& time) {
    if (time.value == 0 || time.rate <= 0) return "0s";
    __int128 num = time.value;
    __int128 den = time.rate;
    const __int128 g = Gcd(num, den);
    num /= g;
    den /= g;
    if (den == 1) return Int128Text(num) + "s";
    return Int128Text(num) + "/" + Int128Text(den) + "s";
}

bool BuildInterchangeTimeline(const Document& document,
                              InterchangeTimeline& timeline,
                              std::string& error) {
    InterchangeTimeline built;
    const DocumentSequence& sequence = document.sequence;
    built.id = sequence.id;
    built.name = sequence.name;
    built.frame_rate = sequence.frame_rate;
    built.width = sequence.width;
    built.height = sequence.height;
    if (!UsableRate(sequence.frame_rate)) {
        error = "the sequence has no usable frame rate";
        return false;
    }
    if (!sequence.look_lut.digest.empty())
        built.losses.push_back({"look_lut", sequence.id});

    std::vector<const DocumentTrack*> videoTracks;
    std::vector<const DocumentTrack*> audioTracks;
    for (const DocumentTrack& track : sequence.tracks) {
        if (track.kind == "video") {
            videoTracks.push_back(&track);
        } else if (track.kind == "audio") {
            audioTracks.push_back(&track);
        } else if (track.kind == "caption") {
            built.losses.push_back({"caption_track", track.id});
        }
    }
    const auto byIndex = [](const DocumentTrack* left,
                            const DocumentTrack* right) {
        return left->index < right->index;
    };
    std::stable_sort(videoTracks.begin(), videoTracks.end(), byIndex);
    std::stable_sort(audioTracks.begin(), audioTracks.end(), byIndex);
    // A soloed audio track silences the others, as the mix does.
    const bool anySolo =
        std::any_of(audioTracks.begin(), audioTracks.end(),
                    [](const DocumentTrack* track) { return track->solo; });

    const std::set<Ulid> hiddenAngles = MulticamHiddenClipIds(sequence);
    std::map<Ulid, size_t> mediaIndex;
    size_t clipCount = 0;

    const auto addTrack = [&](const DocumentTrack& track, bool video,
                              int32_t number) -> bool {
        InterchangeTrack entry;
        entry.id = track.id;
        entry.video = video;
        entry.number = number;
        entry.enabled =
            video ? track.visible : !track.muted && (!anySolo || track.solo);
        if (!entry.enabled) built.losses.push_back({"hidden_track", track.id});
        std::set<Ulid> carried;
        for (const DocumentClip& clip : track.clips) {
            if (IsMotionClip(clip)) {
                built.losses.push_back({"motion_clip", clip.id});
                continue;
            }
            const LibraryMedia* media =
                document.FindLibraryMedia(clip.source_id);
            if (media == nullptr) {
                error = "clip '" + clip.id + "' references media '" +
                        clip.source_id + "' that is not in the library";
                return false;
            }
            InterchangeClip placed;
            placed.clip_id = clip.id;
            placed.timeline_in = clip.timeline_in;
            placed.duration = clip.duration;
            placed.source_in = clip.source_in;
            placed.enabled = entry.enabled && clip.enabled;
            if (hiddenAngles.count(clip.id) != 0) {
                placed.enabled = false;
                built.losses.push_back({"multicam_angle", clip.id});
            }
            if (ClipHasSpeedRamp(clip)) {
                RationalTime span;
                try {
                    span = ClipSourceSpan(clip);
                } catch (const std::exception& exception) {
                    error = "clip '" + clip.id +
                            "' ramps over a source span with no exact "
                            "spelling: " +
                            exception.what();
                    return false;
                }
                __int128 num =
                    static_cast<__int128>(span.value) * clip.duration.rate;
                __int128 den =
                    static_cast<__int128>(span.rate) * clip.duration.value;
                const __int128 g = Gcd(num, den);
                num /= g;
                den /= g;
                if (den <= 0 || num < 0 || num > INT32_MAX || den > INT32_MAX) {
                    error = "clip '" + clip.id +
                            "' ramps at a mean speed with no exact spelling";
                    return false;
                }
                placed.speed = {static_cast<int32_t>(num),
                                static_cast<int32_t>(den)};
                placed.ramp = true;
                built.losses.push_back({"speed_ramp", clip.id});
            } else {
                placed.speed = clip.speed;
            }
            if (placed.speed.num < 0 || placed.speed.den <= 0) {
                error = "clip '" + clip.id + "' plays no source forward";
                return false;
            }
            const auto found = mediaIndex.find(media->id);
            if (found != mediaIndex.end()) {
                placed.media = found->second;
            } else {
                InterchangeMedia item;
                item.id = media->id;
                item.path = media->path;
                item.filename = media->filename;
                item.has_video = media->has_video;
                item.has_audio = media->has_audio;
                item.width = media->width;
                item.height = media->height;
                item.rate = media->has_video ? media->rate : MediaRate{0, 1};
                item.duration = media->duration;
                item.audio_rate = media->audio_rate;
                item.audio_channels = media->audio_channels;
                placed.media = built.media.size();
                mediaIndex[media->id] = placed.media;
                built.media.push_back(std::move(item));
            }
            if (video) {
                if (!clip.effects.empty())
                    built.losses.push_back({"grade", clip.id});
                if (ClipHasFraming(clip) ||
                    ClipAnimates(clip, ClipAnimatedParameter::FramingScale) ||
                    ClipAnimates(clip,
                                 ClipAnimatedParameter::FramingPositionX) ||
                    ClipAnimates(clip, ClipAnimatedParameter::FramingPositionY))
                    built.losses.push_back({"framing", clip.id});
                if (clip.opacity.num != clip.opacity.den ||
                    ClipAnimates(clip, ClipAnimatedParameter::Opacity))
                    built.losses.push_back({"opacity", clip.id});
                if (ClipHasBlur(clip))
                    built.losses.push_back({"blur", clip.id});
                // PA8 -- neither format is written a mask: the clip travels
                // whole, and the report says which.
                if (ClipHasMask(clip))
                    built.losses.push_back({"mask", clip.id});
            } else {
                if (clip.audio_gain_db.num != 0)
                    built.losses.push_back({"audio_gain", clip.id});
                if (!IsZero(clip.audio_fade_in) || !IsZero(clip.audio_fade_out))
                    built.losses.push_back({"audio_fade", clip.id});
            }
            const RationalTime end = clip.timeline_in.add(clip.duration);
            if (end > built.duration) built.duration = end;
            carried.insert(clip.id);
            entry.clips.push_back(std::move(placed));
        }
        std::stable_sort(
            entry.clips.begin(), entry.clips.end(),
            [](const InterchangeClip& left, const InterchangeClip& right) {
                return left.timeline_in < right.timeline_in;
            });
        for (const DocumentTransition& transition : sequence.transitions) {
            if (transition.track_id != track.id) continue;
            InterchangeTransition item;
            item.id = transition.id;
            item.type = transition.type;
            item.left_clip_id = transition.left_clip_id;
            item.right_clip_id = transition.right_clip_id;
            TransitionSpan span;
            std::string spanError;
            const bool crosses = (transition.type == "cross_dissolve" ||
                                  transition.type == "cross_fade") &&
                                 carried.count(transition.left_clip_id) != 0 &&
                                 carried.count(transition.right_clip_id) != 0;
            if (crosses &&
                ResolveTransitionSpan(sequence, transition, span, spanError)) {
                item.before_cut = Frames(span.pre_frames, sequence.frame_rate);
                item.after_cut = Frames(span.post_frames, sequence.frame_rate);
            } else {
                // Kept with empty ends: both writers count it as not sent.
                item.left_clip_id.clear();
                item.right_clip_id.clear();
            }
            entry.transitions.push_back(std::move(item));
        }
        clipCount += entry.clips.size();
        built.tracks.push_back(std::move(entry));
        return true;
    };

    int32_t number = 0;
    for (const DocumentTrack* track : videoTracks)
        if (!addTrack(*track, true, ++number)) return false;
    number = 0;
    for (const DocumentTrack* track : audioTracks)
        if (!addTrack(*track, false, ++number)) return false;
    if (clipCount == 0) {
        error = "the timeline holds no clip this file could carry";
        return false;
    }

    for (const DocumentMarker& marker : sequence.markers) {
        InterchangeMarker item;
        item.id = marker.id;
        item.name = marker.name;
        item.color = marker.color;
        item.category = marker.category;
        item.time = marker.time;
        item.duration = marker.duration;
        built.markers.push_back(std::move(item));
    }
    std::stable_sort(
        built.markers.begin(), built.markers.end(),
        [](const InterchangeMarker& left, const InterchangeMarker& right) {
            return left.time < right.time;
        });
    timeline = std::move(built);
    error.clear();
    return true;
}

OtioExport WriteOtio(const InterchangeTimeline& timeline) {
    OtioExport exported;
    OtioWriter writer{timeline, exported};

    Value children = Value::MakeArray();
    for (const InterchangeTrack& track : timeline.tracks)
        children.Push(writer.Track(track));
    Value markers = Value::MakeArray();
    for (const InterchangeMarker& marker : timeline.markers)
        markers.Push(writer.Marker(marker));

    Value stack = Value::MakeObject();
    stack.Set("OTIO_SCHEMA", Value::MakeString("Stack.1"));
    stack.Set("metadata", Value::MakeObject());
    stack.Set("name", Value::MakeString("tracks"));
    stack.Set("source_range", Value::MakeNull());
    stack.Set("effects", Value::MakeArray());
    stack.Set("markers", std::move(markers));
    stack.Set("enabled", Value::MakeBool(true));
    stack.Set("children", std::move(children));

    Value own = Value::MakeObject();
    own.Set("timeline_id", Value::MakeString(timeline.id));
    own.Set("width", Value::MakeInt(timeline.width));
    own.Set("height", Value::MakeInt(timeline.height));
    Value frameRate = Value::MakeObject();
    frameRate.Set("num", Value::MakeInt(timeline.frame_rate.num));
    frameRate.Set("den", Value::MakeInt(timeline.frame_rate.den));
    own.Set("frame_rate", std::move(frameRate));
    Value metadata = Value::MakeObject();
    metadata.Set("cutmachine", std::move(own));

    Value root = Value::MakeObject();
    root.Set("OTIO_SCHEMA", Value::MakeString("Timeline.1"));
    root.Set("metadata", std::move(metadata));
    root.Set("name", Value::MakeString(timeline.name));
    root.Set("global_start_time", writer.Time({0, 1}, timeline.frame_rate));
    root.Set("tracks", std::move(stack));
    exported.text = root.Dump() + "\n";
    return exported;
}

FcpxmlExport WriteFcpxml(const InterchangeTimeline& timeline) {
    FcpxmlExport exported;
    for (const InterchangeTrack& track : timeline.tracks)
        for (const InterchangeTransition& transition : track.transitions)
            exported.transitions_not_sent.push_back(transition.id);

    // r1 is the sequence's format; each distinct picture format a media
    // has takes the next id, then each media its asset id.
    std::vector<FcpxmlFormat> formats;
    formats.push_back({timeline.frame_rate, timeline.width, timeline.height});
    std::vector<size_t> mediaFormat(timeline.media.size(), 0);
    for (size_t index = 0; index < timeline.media.size(); ++index) {
        const InterchangeMedia& media = timeline.media[index];
        if (!media.has_video || !UsableRate(media.rate)) continue;
        const FcpxmlFormat wanted{media.rate, media.width, media.height};
        size_t found = formats.size();
        for (size_t candidate = 0; candidate < formats.size(); ++candidate)
            if (SameFormat(formats[candidate], wanted)) found = candidate;
        if (found == formats.size()) formats.push_back(wanted);
        mediaFormat[index] = found + 1;
    }
    const auto formatId = [](size_t oneBased) {
        return "r" + std::to_string(oneBased);
    };
    const auto assetId = [&](size_t media) {
        return "r" + std::to_string(formats.size() + media + 1);
    };

    std::ostringstream out;
    out << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        << "<!DOCTYPE fcpxml>\n"
        << "<fcpxml version=\"1.10\">\n"
        << "    <resources>\n";
    for (size_t index = 0; index < formats.size(); ++index) {
        const FcpxmlFormat& format = formats[index];
        out << "        <format id=\"" << formatId(index + 1)
            << "\" frameDuration=\""
            << FormatFcpxmlTime(FrameDuration(format.rate)) << "\" width=\""
            << format.width << "\" height=\"" << format.height << "\"/>\n";
    }
    for (size_t index = 0; index < timeline.media.size(); ++index) {
        const InterchangeMedia& media = timeline.media[index];
        out << "        <asset id=\"" << assetId(index) << "\" name=\""
            << EscapeXml(media.filename) << "\" start=\"0s\" duration=\""
            << FormatFcpxmlTime(media.duration) << "\" hasVideo=\""
            << (media.has_video ? 1 : 0) << "\" hasAudio=\""
            << (media.has_audio ? 1 : 0) << "\"";
        if (mediaFormat[index] != 0)
            out << " format=\"" << formatId(mediaFormat[index]) << "\"";
        if (media.has_audio) {
            out << " audioSources=\"1\"";
            if (media.audio_channels > 0)
                out << " audioChannels=\"" << media.audio_channels << "\"";
            if (media.audio_rate > 0)
                out << " audioRate=\"" << media.audio_rate << "\"";
        }
        out << ">\n            <media-rep kind=\"original-media\" src=\""
            << EscapeXml(FileUrl(media.path)) << "\"/>\n"
            << "        </asset>\n";
    }
    out << "    </resources>\n"
        << "    <library>\n"
        << "        <event name=\"CUTMACHINE\">\n"
        << "            <project name=\"" << EscapeXml(timeline.name) << "\">\n"
        << "                <sequence format=\"r1\" duration=\""
        << FormatFcpxmlTime(timeline.duration)
        << "\" tcStart=\"0s\" tcFormat=\"NDF\" audioLayout=\"stereo\"";
    // The engine mixes at 48 kHz whatever the rushes were recorded at
    // (audio_mix::kSampleRate): that is the sequence's rate.
    out << " audioRate=\"48k\"";
    out << ">\n"
        << "                    <spine>\n"
        << "                        <gap name=\"Gap\" offset=\"0s\" "
           "start=\"0s\" duration=\""
        << FormatFcpxmlTime(timeline.duration) << "\">\n";
    const std::string indent(28, ' ');
    int32_t audioLane = 0;
    for (const InterchangeTrack& track : timeline.tracks) {
        const int32_t lane = track.video ? track.number : -(++audioLane);
        for (const InterchangeClip& clip : track.clips) {
            const InterchangeMedia& media = timeline.media[clip.media];
            const bool retimed = clip.speed.num != clip.speed.den;
            const bool freeze = clip.speed.num == 0;
            // In a retimed clip `start` counts the clip's local time, which
            // is the source time divided by the speed; a held frame's local
            // time starts at zero and maps to the one source time below.
            RationalTime start = clip.source_in;
            if (freeze) {
                start = {0, 1};
            } else if (retimed &&
                       !DivideBySpeed(clip.source_in, clip.speed, start)) {
                start = clip.source_in;
            }
            out << indent << "<asset-clip ref=\"" << assetId(clip.media)
                << "\" lane=\"" << lane << "\" offset=\""
                << FormatFcpxmlTime(clip.timeline_in) << "\" name=\""
                << EscapeXml(media.filename) << "\" start=\""
                << FormatFcpxmlTime(start) << "\" duration=\""
                << FormatFcpxmlTime(clip.duration) << "\"";
            if (mediaFormat[clip.media] != 0)
                out << " format=\"" << formatId(mediaFormat[clip.media])
                    << "\"";
            out << " srcEnable=\"" << (track.video ? "video" : "audio") << "\"";
            if (!clip.enabled) out << " enabled=\"0\"";
            if (!retimed) {
                out << "/>\n";
                continue;
            }
            const RationalTime sourceEnd =
                freeze ? clip.source_in
                       : clip.source_in.add(
                             MultiplyBySpeed(clip.duration, clip.speed));
            out << ">\n"
                << indent << "    <timeMap>\n"
                << indent << "        <timept time=\""
                << FormatFcpxmlTime(start) << "\" value=\""
                << FormatFcpxmlTime(clip.source_in)
                << "\" interp=\"linear\"/>\n"
                << indent << "        <timept time=\""
                << FormatFcpxmlTime(start.add(clip.duration)) << "\" value=\""
                << FormatFcpxmlTime(sourceEnd) << "\" interp=\"linear\"/>\n"
                << indent << "    </timeMap>\n"
                << indent << "</asset-clip>\n";
        }
    }
    const RationalTime oneFrame = FrameDuration(timeline.frame_rate);
    for (const InterchangeMarker& marker : timeline.markers) {
        // A point is one frame long: the format has no zero-length marker.
        out << indent << "<marker start=\"" << FormatFcpxmlTime(marker.time)
            << "\" duration=\""
            << FormatFcpxmlTime(IsZero(marker.duration) ? oneFrame
                                                        : marker.duration)
            << "\" value=\"" << EscapeXml(marker.name) << "\"/>\n";
    }
    out << "                        </gap>\n"
        << "                    </spine>\n"
        << "                </sequence>\n"
        << "            </project>\n"
        << "        </event>\n"
        << "    </library>\n"
        << "</fcpxml>\n";
    exported.text = out.str();
    return exported;
}

bool ExportOtioFile(Document document, const std::string& packageDirectory,
                    const std::string& path, std::string& reportJson,
                    std::string& errorName, std::string& error) {
    if (!std::filesystem::path(path).is_absolute() ||
        !HasExtension(path, kOtioExtension)) {
        errorName = "ValidationFailed";
        error = "'" + path + "' must be an absolute path ending in .otio";
        return false;
    }
    ResolveMediaPaths(document, packageDirectory);
    InterchangeTimeline timeline;
    if (!BuildInterchangeTimeline(document, timeline, error)) {
        errorName = "InvalidExport";
        return false;
    }
    const OtioExport exported = WriteOtio(timeline);
    if (!PublishNewInterchangeFile(path, exported.text, errorName, error))
        return false;
    Value report = ReportBase(timeline, path, "cutmachine.otio.v1");
    report.Set("off_frame_times", Value::MakeInt(exported.off_frame_times));
    report.Set("colors_approximated", IdArray(exported.colors_approximated));
    FinishReport(report, timeline.losses, exported.transitions_not_sent,
                 reportJson);
    errorName.clear();
    error.clear();
    return true;
}

bool ExportFcpxmlFile(Document document, const std::string& packageDirectory,
                      const std::string& path, std::string& reportJson,
                      std::string& errorName, std::string& error) {
    if (!std::filesystem::path(path).is_absolute() ||
        !HasExtension(path, kFcpxmlExtension)) {
        errorName = "ValidationFailed";
        error = "'" + path + "' must be an absolute path ending in .fcpxml";
        return false;
    }
    ResolveMediaPaths(document, packageDirectory);
    InterchangeTimeline timeline;
    if (!BuildInterchangeTimeline(document, timeline, error)) {
        errorName = "InvalidExport";
        return false;
    }
    const FcpxmlExport exported = WriteFcpxml(timeline);
    if (!PublishNewInterchangeFile(path, exported.text, errorName, error))
        return false;
    Value report = ReportBase(timeline, path, "cutmachine.fcpxml.v1");
    report.Set("version", Value::MakeString("1.10"));
    FinishReport(report, timeline.losses, exported.transitions_not_sent,
                 reportJson);
    errorName.clear();
    error.clear();
    return true;
}

int ExportOtioCommand(const std::string& projectPath, const std::string& path,
                      std::string& output, const std::string& timelineId) {
    Document document;
    int status = 0;
    if (!LoadTimelineDocument(projectPath, timelineId, document, output,
                              status))
        return status;
    std::string errorName;
    std::string error;
    if (!ExportOtioFile(
            std::move(document),
            std::filesystem::absolute(projectPath).parent_path().string(),
            std::filesystem::absolute(path).string(), output, errorName, error))
        return FailCliCommand(errorName, error, output);
    return 0;
}

int ExportFcpxmlCommand(const std::string& projectPath, const std::string& path,
                        std::string& output, const std::string& timelineId) {
    Document document;
    int status = 0;
    if (!LoadTimelineDocument(projectPath, timelineId, document, output,
                              status))
        return status;
    std::string errorName;
    std::string error;
    if (!ExportFcpxmlFile(
            std::move(document),
            std::filesystem::absolute(projectPath).parent_path().string(),
            std::filesystem::absolute(path).string(), output, errorName, error))
        return FailCliCommand(errorName, error, output);
    return 0;
}
